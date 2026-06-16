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
             std::mutex& db_flush_mu,
             int commit_interval)
    : thread_id_(thread_id), commit_interval_(commit_interval), db_flush_mu_(db_flush_mu) {
    // Stagger flush thresholds so threads don't all flush simultaneously.
    // Thread N flushes at WAY_BUFFER_SIZE + N*137 (prime offset avoids harmonics).
    way_buffer_size_ = WAY_BUFFER_SIZE + thread_id * 137;

    std::string connstr = "host=" + host +
                          " dbname=" + database +
                          " user=" + user +
                          " sslmode=disable";
    conn_ = std::make_unique<pqxx::connection>(connstr);
    // Optimize for bulk import — disable WAL sync and fsync per connection
    pqxx::work txn(*conn_);
    txn.exec("SET synchronous_commit = off");
    txn.commit();

}

NavDB::~NavDB() {
    try {
        flushNodes();
        flushWays();
        flushAreas();
        flushRoads();
        flushRelations();
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
    if (static_cast<int>(way_buf_.size()) >= way_buffer_size_)
        flushWays();
}

void NavDB::insertArea(int64_t id, const std::string& name,
                       const Tags& tags, const std::string& geog) {
    addAreaTags(id, tags);
    area_buf_.push_back({id, name, geog});
    if (static_cast<int>(area_buf_.size()) > way_buffer_size_)
        flushAreas();
}

void NavDB::insertRoad(int64_t id, const std::string& name,
                       const Tags& tags, const std::string& geog) {
    addTags(id, tags);
    road_buf_.push_back({id, name, geog});
    if (static_cast<int>(road_buf_.size()) > way_buffer_size_)
        flushRoads();
}

// ---- finalizers ----

void NavDB::insertRelation(int64_t id, const std::string& name,
                            const Tags& tags, const std::string& geog) {
    addTags(id, tags);
    std::optional<std::string> g = geog.empty() ? std::nullopt : std::make_optional(geog);
    relation_buf_.push_back({id, name, std::move(g)});
    if (static_cast<int>(relation_buf_.size()) > way_buffer_size_)
        flushRelations();
}

void NavDB::finalize_nodes() { flushNodes(); }
void NavDB::finalize_ways()  { flushWays(); flushAreas(); }
void NavDB::finalize_roads() { flushRoads(); }
void NavDB::finalize_relations() { flushRelations(); }

void NavDB::finalize_tags(const std::string& table) {
    // Tag tables are written via COPY — no cross-thread conflicts,
    // so no lock needed here.
    finalize_tags_locked(table);
}

void NavDB::finalize_tags_locked(const std::string& table) {
    auto& buf = (table == "area") ? area_tag_buf_ : tag_buf_;
    if (buf.empty()) return;
    LOGI(thread_id_, "finalize_tags table=", table, " count=", buf.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn,
            {"my_" + table + "_tags"},
            {"id", "key_name", "key_value"}
        );
        for (const auto& r : buf)
            stream.write_values(r.id, r.key, r.value);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] finalize_tags(" << table << ") error: " << e.what() << "\n";
        throw;
    }
    buf.clear();
}

// ---- flush internals ----

void NavDB::flushNodes() {
    if (node_buf_.empty()) return;
    LOGI(thread_id_, "flushNodes start count=", node_buf_.size());
    std::lock_guard flush_lk(db_flush_mu_);
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn,
            {"my_nodes"},
            {"id", "name", "longitude_m", "latitude_m", "geog"}
        );
        for (const auto& r : node_buf_)
            stream.write_values(r.id, r.name, r.lon_m, r.lat_m, r.geog);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushNodes error: " << e.what() << "\n";
        throw;
    }
    finalize_tags_locked("node");
    node_buf_.clear();
}

// Helper: COPY buffer into temp table, then INSERT ... ON CONFLICT DO NOTHING
// into the real table, then clear temp table — all in one transaction.
void NavDB::flushViaTemp(const std::string& /*tmp_table*/,
                         const std::string& real_table,
                         const std::string& tag_table,
                         std::vector<GeomRecord>& buf) {
    if (buf.empty()) return;
    LOGI(thread_id_, "flushViaTemp real=", real_table, " count=", buf.size());
    try {
        pqxx::work txn(*conn_);
        // COPY directly into the real table — no temp table, no ON CONFLICT.
        // Primary key constraints must be dropped before import and recreated
        // after (see create.sql). This is ~10x faster than the temp+INSERT approach.
        auto stream = pqxx::stream_to::table(
            txn, {real_table}, {"id", "name", "geog"});
        for (const auto& r : buf)
            stream.write_values(r.id, r.name, r.geog);
        stream.complete();
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

void NavDB::flushRelations() {
    if (relation_buf_.empty()) return;
    LOGI(thread_id_, "flushRelations count=", relation_buf_.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn, {"my_relations"}, {"id", "name", "geog"});
        for (const auto& r : relation_buf_)
            stream.write_values(r.id, r.name, r.geog);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushRelations error: " << e.what() << "\n";
        throw;
    }
    finalize_tags("relation");
    relation_buf_.clear();
}

// ---- index management ----

// These should only be called by one thread (thread 0) at the phase boundary.
static const char* DISABLE_INDEXES = R"SQL(
    ALTER TABLE my_ways      DROP CONSTRAINT IF EXISTS my_ways_pkey;
    ALTER TABLE my_areas     DROP CONSTRAINT IF EXISTS my_areas_pkey;
    ALTER TABLE my_roads     DROP CONSTRAINT IF EXISTS my_roads_pkey;
    ALTER TABLE my_relations DROP CONSTRAINT IF EXISTS my_relations_pkey;
)SQL";

static const char* ENABLE_INDEXES = R"SQL(
    ALTER TABLE my_ways      ADD PRIMARY KEY (id);
    ALTER TABLE my_areas     ADD PRIMARY KEY (id);
    ALTER TABLE my_roads     ADD PRIMARY KEY (id);
    ALTER TABLE my_relations ADD PRIMARY KEY (id);
)SQL";

static const char* CREATE_GIST_INDEXES = R"SQL(
    CREATE INDEX IF NOT EXISTS my_nodes_geog_idx     ON public.my_nodes     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS my_ways_geog_idx      ON public.my_ways      USING GIST (geog);
    CREATE INDEX IF NOT EXISTS my_areas_geog_idx     ON public.my_areas     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS my_roads_geog_idx     ON public.my_roads     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS my_relations_geog_idx ON public.my_relations USING GIST (geog);
)SQL";

void NavDB::disableIndexes() {
    LOGI(thread_id_, "disabling indexes");
    try {
        pqxx::work txn(*conn_);
        txn.exec(DISABLE_INDEXES);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] disableIndexes error: " << e.what() << "\n";
        throw;
    }
    LOGI(thread_id_, "indexes disabled");
}

void NavDB::enableIndexes() {
    LOGI(thread_id_, "enabling indexes (REINDEX may take a while)");
    try {
        pqxx::work txn(*conn_);
        txn.exec(ENABLE_INDEXES);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] enableIndexes error: " << e.what() << "\n";
        throw;
    }
    LOGI(thread_id_, "indexes enabled");
}

void NavDB::createGistIndexes() {
    LOGI(thread_id_, "creating GiST spatial indexes (may take a while)");
    try {
        pqxx::work txn(*conn_);
        txn.exec(CREATE_GIST_INDEXES);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] createGistIndexes error: " << e.what() << "\n";
        throw;
    }
    LOGI(thread_id_, "GiST indexes created");
}

// ---- query ----

std::string NavDB::getWay(int64_t id) {
    try {
        pqxx::work txn(*conn_);
        auto res = txn.exec_params(
            "SELECT geog FROM my_ways WHERE id = $1 UNION ALL SELECT geog FROM my_areas WHERE id = $1", id);
        if (res.empty() || res[0][0].is_null()) return "";
        return res[0][0].as<std::string>();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] getWay error: " << e.what() << "\n";
        return "";
    }
}
