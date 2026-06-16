#include "NavDB.h"

#include <iostream>
#include <stdexcept>
#include <cctype>
#include <string>

#include <pqxx/pqxx>
#include "Log.h"

// ---- helpers ----

static bool allAlphaNum(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isalnum(c)) return false;
    return true;
}

// ---- NavDB ----

NavDB::NavDB(int thread_id, const std::string& host,
             const std::string& user, const std::string& database,
             int commit_interval)
    : thread_id_(thread_id), commit_interval_(commit_interval) {

    std::string connstr = "host=" + host +
                          " dbname=" + database +
                          " user=" + user;
    conn_ = std::make_unique<pqxx::connection>(connstr);

    // Create per-thread temp tables for ways, areas, roads.
    // Nodes don't need this because they're partitioned by the barrier —
    // all nodes are done before any thread starts ways, so no cross-thread
    // duplicate nodes can occur within a single flush.
    // Ways/areas/roads CAN duplicate across threads because multiple PBF
    // primitive blocks can contain the same way ID.
    pqxx::work txn(*conn_);
    std::string tid = std::to_string(thread_id_);
    txn.exec("CREATE TEMP TABLE tmp_ways_"  + tid +
             " (id bigint, name varchar(256), geog public.geography) ON COMMIT DELETE ROWS");
    txn.exec("CREATE TEMP TABLE tmp_areas_" + tid +
             " (id bigint, name varchar(256), geog public.geography) ON COMMIT DELETE ROWS");
    txn.exec("CREATE TEMP TABLE tmp_roads_" + tid +
             " (id bigint, name varchar(256), geog public.geography) ON COMMIT DELETE ROWS");
    txn.commit();
}

NavDB::~NavDB() {
    try {
        flushNodes();
        flushWays();
        flushAreas();
        flushRoads();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] destructor flush error: " << e.what() << "\n";
    }
}

// ---- tag helpers ----

void NavDB::addTags(int64_t id, const Tags& tags) {
    for (const auto& [k, v] : tags) {
        if (!allAlphaNum(k) || !allAlphaNum(v)) continue;
        tag_buf_.push_back({id, k, v});
    }
}

// ---- insert ----

void NavDB::insertNode(int64_t id, const std::string& name,
                       double lon_m, double lat_m,
                       const Tags& tags, const std::string& geog) {
    if (tags.empty()) return;
    addTags(id, tags);
    node_buf_.push_back({id, name, geog, lon_m, lat_m});
    if (static_cast<int>(node_buf_.size()) >= NODE_BUFFER_SIZE)
        flushNodes();
}

void NavDB::insertWay(int64_t id, const std::string& name,
                      const Tags& tags, const std::string& geog) {
    addTags(id, tags);
    way_buf_.push_back({id, name, geog});
    if (static_cast<int>(way_buf_.size()) >= WAY_BUFFER_SIZE)
        flushWays();
}

void NavDB::insertArea(int64_t id, const std::string& name,
                       const Tags& tags, const std::string& geog) {
    addTags(id, tags);
    area_buf_.push_back({id, name, geog});
    if (static_cast<int>(area_buf_.size()) > WAY_BUFFER_SIZE)
        flushAreas();
}

void NavDB::insertRoad(int64_t id, const std::string& name,
                       const Tags& tags, const std::string& geog) {
    addTags(id, tags);
    road_buf_.push_back({id, name, geog});
    if (static_cast<int>(road_buf_.size()) > WAY_BUFFER_SIZE)
        flushRoads();
}

// ---- finalizers ----

void NavDB::finalize_nodes() { flushNodes(); }
void NavDB::finalize_ways()  { flushWays(); flushAreas(); }
void NavDB::finalize_roads() { flushRoads(); }

void NavDB::finalize_tags(const std::string& table) {
    if (tag_buf_.empty()) return;
    LOGI(thread_id_, "finalize_tags table=", table, " count=", tag_buf_.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn,
            {"my_" + table + "_tags"},
            {"id", "key_name", "key_value"}
        );
        for (const auto& r : tag_buf_)
            stream.write_values(r.id, r.key, r.value);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] finalize_tags(" << table << ") error: " << e.what() << "\n";
        throw;
    }
    tag_buf_.clear();
}

// ---- flush internals ----

void NavDB::flushNodes() {
    if (node_buf_.empty()) return;
    try {
        LOGI(thread_id_, "flushNodes start count=", node_buf_.size());
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn,
            {"my_nodes"},
            {"id", "name", "longitude", "latitude", "geog"}
        );
        for (const auto& r : node_buf_)
            stream.write_values(r.id, r.name, r.lon_m, r.lat_m, r.geog);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushNodes error: " << e.what() << "\n";
        throw;
    }
    finalize_tags("node");
    node_buf_.clear();
}

// Helper: COPY buffer into temp table, then INSERT ... ON CONFLICT DO NOTHING
// into the real table, then clear temp table — all in one transaction.
void NavDB::flushViaTemp(const std::string& tmp_table,
                         const std::string& real_table,
                         const std::string& tag_table,
                         std::vector<GeomRecord>& buf) {
    if (buf.empty()) return;
    LOGI(thread_id_, "flushViaTemp real=", real_table, " count=", buf.size());
    const std::string tid = std::to_string(thread_id_);
    try {
        pqxx::work txn(*conn_);

        // 1. COPY into temp table
        auto stream = pqxx::stream_to::table(
            txn, {tmp_table + "_" + tid}, {"id", "name", "geog"});
        for (const auto& r : buf)
            stream.write_values(r.id, r.name, r.geog);
        stream.complete();

        // 2. Insert from temp → real, skipping duplicates
        txn.exec("INSERT INTO " + real_table +
                 " (id, name, geog)"
                 " SELECT id, name, geog FROM " + tmp_table + "_" + tid +
                 " ON CONFLICT (id) DO NOTHING");

        // 3. Clear temp table (ON COMMIT DELETE ROWS handles this too,
        //    but explicit delete keeps the transaction clean)
        txn.exec("DELETE FROM " + tmp_table + "_" + tid);

        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushViaTemp(" << real_table << ") error: "
                  << e.what() << "\n";
        throw;
    }
    finalize_tags(tag_table);
    buf.clear();
}

void NavDB::flushWays()  { flushViaTemp("tmp_ways",  "my_ways",  "way",  way_buf_);  }
void NavDB::flushAreas() { flushViaTemp("tmp_areas", "my_areas", "way",  area_buf_); }
void NavDB::flushRoads() { flushViaTemp("tmp_roads", "my_roads", "road", road_buf_); }

// ---- query ----

std::string NavDB::getWay(int64_t id) {
    try {
        pqxx::work txn(*conn_);
        auto res = txn.exec_params(
            "SELECT geog FROM my_ways WHERE id = $1", id);
        if (res.empty() || res[0][0].is_null()) return "";
        return res[0][0].as<std::string>();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] getWay error: " << e.what() << "\n";
        return "";
    }
}
