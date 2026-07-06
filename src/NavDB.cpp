#include "NavDB.h"

#include <iostream>
#include <stdexcept>
#include <cctype>
#include <string>

#include <pqxx/pqxx>
#include "Log.h"

// ---- helpers ----

// Drops invalid UTF-8 byte sequences and embedded NUL bytes (both of which
// PostgreSQL's COPY protocol rejects with "invalid byte sequence for
// encoding \"UTF8\"", aborting the whole stream). Keeps well-formed
// multi-byte characters intact rather than rejecting the entire tag.
static std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int len;
        if      ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { ++i; continue; } // invalid leading byte

        if (c == 0x00) { ++i; continue; } // NUL not allowed in postgres text

        if (i + len > s.size()) { ++i; continue; } // truncated sequence

        bool valid = true;
        for (int j = 1; j < len; ++j) {
            unsigned char cc = s[i + j];
            if ((cc & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) { ++i; continue; }

        out.append(s, i, len);
        i += len;
    }
    return out;
}

// Sanitizes and truncates to at most 256 codepoints (matching the
// varchar(256) tag columns), cutting on a UTF-8 boundary so the result
// stays valid.
static std::string sanitizeTag(const std::string& s) {
    std::string clean = sanitizeUtf8(s);
    size_t count = 0, i = 0;
    while (i < clean.size() && count < 256) {
        unsigned char c = clean[i];
        int len = (c & 0x80) == 0x00 ? 1 :
                  (c & 0xE0) == 0xC0 ? 2 :
                  (c & 0xF0) == 0xE0 ? 3 : 4;
        i += len;
        ++count;
    }
    return clean.substr(0, i);
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
        std::string ck = sanitizeTag(k), cv = sanitizeTag(v);
        if (ck.empty() || cv.empty()) continue;
        tag_buf_.push_back({id, std::move(ck), std::move(cv)});
    }
}

void NavDB::addWayTags(int64_t id, const Tags& tags) {
    for (const auto& [k, v] : tags) {
        std::string ck = sanitizeTag(k), cv = sanitizeTag(v);
        if (ck.empty() || cv.empty()) continue;
        way_tag_buf_.push_back({id, std::move(ck), std::move(cv)});
    }
}

void NavDB::addAreaTags(int64_t id, const Tags& tags) {
    for (const auto& [k, v] : tags) {
        std::string ck = sanitizeTag(k), cv = sanitizeTag(v);
        if (ck.empty() || cv.empty()) continue;
        area_tag_buf_.push_back({id, std::move(ck), std::move(cv)});
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
    addWayTags(id, tags);
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
    auto& buf = (table == "way")  ? way_tag_buf_  :
                (table == "area") ? area_tag_buf_ : tag_buf_;
    if (buf.empty()) return;
    LOGI(thread_id_, "finalize_tags table=", table, " count=", buf.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn,
            {"" + table + "_tags"},
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
            {"nodes"},
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
// Attempt to COPY a subset of records into real_table.
// Returns the number of rows successfully written.
// On failure, parses the offending line number from the error message,
// removes that row, and retries. Repeats until the batch succeeds or
// no more line numbers can be parsed.
template<typename Record, typename WriteFn>
static int tryCopyBatch(pqxx::connection& conn,
                        const std::string& real_table,
                        std::vector<Record>& buf,
                        WriteFn write_row) {
    int skipped = 0;
    while (!buf.empty()) {
        try {
            pqxx::work txn(conn);
            auto stream = pqxx::stream_to::table(
                txn, {real_table}, {"id", "name", "geog"});
            for (const auto& r : buf)
                write_row(stream, r);
            stream.complete();
            txn.commit();
            return static_cast<int>(buf.size()); // all remaining rows written
        } catch (const std::exception& e) {
            // The line number is only available via PostgreSQL's error text
            // (there's no structured field in pqxx for COPY row errors).
            // PostgreSQL always formats this as "COPY tablename, line N[,...]"
            // so we anchor on ", line " to be more specific than just "line ".
            std::string msg = e.what();
            size_t pos = msg.find(", line ");
            if (pos == std::string::npos) {
                // Can't identify the bad row — give up on this batch
                std::cerr << "[NavDB] tryCopyBatch(" << real_table
                          << ") unrecoverable error (no line number): "
                          << msg << "\n";
                break;
            }
            // COPY line numbers are 1-based; skip ", line " (7 chars)
            int line = std::stoi(msg.substr(pos + 7));
            int idx  = line - 1;
            if (idx < 0 || idx >= static_cast<int>(buf.size())) {
                std::cerr << "[NavDB] tryCopyBatch(" << real_table
                          << ") bad line number " << line
                          << " (buf size=" << buf.size() << "): " << msg << "\n";
                break;
            }
            std::cerr << "[NavDB] skipping bad row in " << real_table
                      << " at line " << line
                      << " id=" << buf[idx].id << ": " << msg << "\n";
            buf.erase(buf.begin() + idx);
            ++skipped;
        }
    }
    return static_cast<int>(buf.size()); // rows remaining after giving up
}

void NavDB::flushViaTemp(const std::string& /*tmp_table*/,
                         const std::string& real_table,
                         const std::string& tag_table,
                         std::vector<GeomRecord>& buf) {
    if (buf.empty()) return;
    LOGI(thread_id_, "flushViaTemp real=", real_table, " count=", buf.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn, {real_table}, {"id", "name", "geog"});
        for (const auto& r : buf)
            stream.write_values(r.id, r.name, r.geog);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushViaTemp(" << real_table << ") initial error: "
                  << e.what() << "\n"
                  << "[NavDB] retrying with bad rows removed...\n";
        auto write_geom = [](auto& stream, const NavDB::GeomRecord& r) {
            stream.write_values(r.id, r.name, r.geog);
        };
        int written = tryCopyBatch(*conn_, real_table, buf, write_geom);
        std::cerr << "[NavDB] recovered: wrote " << written << " of "
                  << buf.size() + written << " rows to " << real_table << "\n";
    }
    finalize_tags(tag_table);
    buf.clear();
}

void NavDB::flushWays()  { flushViaTemp("tmp_ways",  "ways",  "way",  way_buf_);  }
void NavDB::flushAreas() { flushViaTemp("tmp_areas", "areas", "area", area_buf_); }
void NavDB::flushRoads() { flushViaTemp("tmp_roads", "roads", "road", road_buf_); }

void NavDB::flushRelations() {
    if (relation_buf_.empty()) return;
    LOGI(thread_id_, "flushRelations count=", relation_buf_.size());
    try {
        pqxx::work txn(*conn_);
        auto stream = pqxx::stream_to::table(
            txn, {"relations"}, {"id", "name", "geog"});
        for (const auto& r : relation_buf_)
            stream.write_values(r.id, r.name, r.geog);
        stream.complete();
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] flushRelations initial error: " << e.what() << "\n"
                  << "[NavDB] retrying with bad rows removed...\n";
        auto write_rel = [](auto& stream, const NavDB::RelationRecord& r) {
            stream.write_values(r.id, r.name, r.geog);
        };
        int written = tryCopyBatch(*conn_, "relations", relation_buf_, write_rel);
        std::cerr << "[NavDB] recovered: wrote " << written << " of "
                  << relation_buf_.size() + written << " rows to relations\n";
    }
    finalize_tags("relation");
    relation_buf_.clear();
}

// ---- index management ----

// These should only be called by one thread (thread 0) at the phase boundary.
static const char* DISABLE_INDEXES = R"SQL(
    ALTER TABLE ways      DROP CONSTRAINT IF EXISTS ways_pkey;
    ALTER TABLE areas     DROP CONSTRAINT IF EXISTS areas_pkey;
    ALTER TABLE roads     DROP CONSTRAINT IF EXISTS roads_pkey;
    ALTER TABLE relations DROP CONSTRAINT IF EXISTS relations_pkey;
)SQL";

static const char* ENABLE_INDEXES = R"SQL(
    ALTER TABLE ways      ADD PRIMARY KEY (id);
    ALTER TABLE areas     ADD PRIMARY KEY (id);
    ALTER TABLE roads     ADD PRIMARY KEY (id);
    ALTER TABLE relations ADD PRIMARY KEY (id);
)SQL";

static const char* CREATE_GIST_INDEXES = R"SQL(
    CREATE INDEX IF NOT EXISTS nodes_geog_idx     ON public.nodes     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS ways_geog_idx      ON public.ways      USING GIST (geog);
    CREATE INDEX IF NOT EXISTS areas_geog_idx     ON public.areas     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS roads_geog_idx     ON public.roads     USING GIST (geog);
    CREATE INDEX IF NOT EXISTS relations_geog_idx ON public.relations USING GIST (geog);
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
        auto res = txn.exec(
            "SELECT geog FROM ways WHERE id = $1 UNION ALL SELECT geog FROM areas WHERE id = $1",
            pqxx::params{id});
        if (res.empty() || res[0][0].is_null()) return "";
        return res[0][0].as<std::string>();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] getWay error: " << e.what() << "\n";
        return "";
    }
}

// ---- delta / update methods ----

void NavDB::updateNode(int64_t id, const std::string& name,
                       double lon_m, double lat_m,
                       const Tags& tags, const std::string& geog) {
    try {
        pqxx::work txn(*conn_);
        txn.exec(
            "UPDATE nodes SET name=$2, longitude_m=$3, latitude_m=$4, geog=$5 WHERE id=$1",
            pqxx::params{id, name, lon_m, lat_m, geog});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] updateNode error: " << e.what() << "\n"; throw;
    }
    // Diff tags: delete removed, insert new
    try {
        pqxx::work txn(*conn_);
        // Get existing tags
        auto rows = txn.exec(
            "SELECT key_name, key_value FROM node_tags WHERE id=$1",
            pqxx::params{id});
        std::unordered_map<std::string,std::string> existing;
        for (const auto& r : rows) existing[r[0].c_str()] = r[1].c_str();

        for (auto& [k, v] : tags) {
            auto it = existing.find(k);
            if (it == existing.end())
                txn.exec("INSERT INTO node_tags(id,key_name,key_value) VALUES($1,$2,$3)",
                         pqxx::params{id, k, v});
            else if (it->second != v)
                txn.exec("UPDATE node_tags SET key_value=$3 WHERE id=$1 AND key_name=$2",
                         pqxx::params{id, k, v});
            existing.erase(k);
        }
        for (auto& [k, _] : existing)
            txn.exec("DELETE FROM node_tags WHERE id=$1 AND key_name=$2", pqxx::params{id, k});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] updateNode tags error: " << e.what() << "\n"; throw;
    }
}

static void diffTags(pqxx::work& txn, int64_t id,
                     const std::string& tag_table, const Tags& tags) {
    std::string sel_q = "SELECT key_name, key_value FROM " + tag_table + " WHERE id=$1";
    auto rows = txn.exec(sel_q, pqxx::params{id});
    std::unordered_map<std::string,std::string> existing;
    for (const auto& r : rows) existing[r[0].c_str()] = r[1].c_str();

    for (auto& [k, v] : tags) {
        auto it = existing.find(k);
        if (it == existing.end())
        {
            std::string ins_q = "INSERT INTO " + tag_table + "(id,key_name,key_value) VALUES($1,$2,$3)";
            txn.exec(ins_q, pqxx::params{id, k, v});
        } else if (it->second != v) {
            std::string upd_q = "UPDATE " + tag_table + " SET key_value=$3 WHERE id=$1 AND key_name=$2";
            txn.exec(upd_q, pqxx::params{id, k, v});
        }
        existing.erase(k);
    }
    std::string del_q = "DELETE FROM " + tag_table + " WHERE id=$1 AND key_name=$2";
    for (auto& [k, _] : existing)
        txn.exec(del_q, pqxx::params{id, k});
}

void NavDB::updateWay(int64_t id, const std::string& name,
                      const Tags& tags, const std::string& geog) {
    try {
        pqxx::work txn(*conn_);
        txn.exec("UPDATE ways SET name=$2, geog=$3 WHERE id=$1", pqxx::params{id, name, geog});
        diffTags(txn, id, "way_tags", tags);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] updateWay error: " << e.what() << "\n"; throw;
    }
}

void NavDB::updateArea(int64_t id, const std::string& name,
                       const Tags& tags, const std::string& geog) {
    try {
        pqxx::work txn(*conn_);
        txn.exec("UPDATE areas SET name=$2, geog=$3 WHERE id=$1", pqxx::params{id, name, geog});
        diffTags(txn, id, "area_tags", tags);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] updateArea error: " << e.what() << "\n"; throw;
    }
}

void NavDB::updateRelation(int64_t id, const std::string& name,
                           const Tags& tags, const std::string& geog) {
    try {
        pqxx::work txn(*conn_);
        std::optional<std::string> g = geog.empty() ? std::nullopt : std::make_optional(geog);
        txn.exec("UPDATE relations SET name=$2, geog=$3 WHERE id=$1", pqxx::params{id, name, g});
        diffTags(txn, id, "relation_tags", tags);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] updateRelation error: " << e.what() << "\n"; throw;
    }
}

void NavDB::deleteEntity(int64_t id, const std::string& type) {
    static const std::unordered_map<std::string,std::string> tag_tables = {
        {"node",     "node_tags"},
        {"way",      "way_tags"},
        {"area",     "area_tags"},
        {"road",     "road_tags"},
        {"relation", "relation_tags"},
    };
    static const std::unordered_map<std::string,std::string> main_tables = {
        {"node",     "nodes"},
        {"way",      "ways"},
        {"area",     "areas"},
        {"road",     "roads"},
        {"relation", "relations"},
    };
    try {
        pqxx::work txn(*conn_);
        if (tag_tables.count(type))
            { std::string q = "DELETE FROM " + tag_tables.at(type) + " WHERE id=$1";
              txn.exec(q, pqxx::params{id}); }
        if (main_tables.count(type))
            { std::string q = "DELETE FROM " + main_tables.at(type) + " WHERE id=$1";
              txn.exec(q, pqxx::params{id}); }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] deleteEntity(" << type << ") error: " << e.what() << "\n"; throw;
    }
}

int64_t NavDB::getReplicationSequence() {
    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec("SELECT sequence FROM osm_replication_state LIMIT 1");
        txn.commit();
        if (r.empty()) return -1;
        return r[0][0].as<int64_t>();
    } catch (...) {
        return -1;
    }
}

void NavDB::setReplicationSequence(int64_t seq) {
    try {
        pqxx::work txn(*conn_);
        txn.exec(
            "INSERT INTO osm_replication_state(sequence) VALUES($1) "
            "ON CONFLICT (id) DO UPDATE SET sequence=$1",
            pqxx::params{seq});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] setReplicationSequence error: " << e.what() << "\n"; throw;
    }
}

int64_t NavDB::getExternalDataTimestamp(const std::string& name) {
    try {
        pqxx::work txn(*conn_);
        auto r = txn.exec(
            "SELECT last_modified FROM external_data_state WHERE name=$1",
            pqxx::params{name});
        txn.commit();
        if (r.empty()) return -1;
        return r[0][0].as<int64_t>();
    } catch (...) {
        return -1;
    }
}

void NavDB::setExternalDataTimestamp(const std::string& name, int64_t epoch_seconds) {
    try {
        pqxx::work txn(*conn_);
        txn.exec(
            "INSERT INTO external_data_state(name, last_modified) VALUES($1, $2) "
            "ON CONFLICT (name) DO UPDATE SET last_modified=$2, checked_at=now()",
            pqxx::params{name, epoch_seconds});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] setExternalDataTimestamp error: " << e.what() << "\n"; throw;
    }
}

void NavDB::setAutovacuum(bool enabled) {
    try {
        pqxx::nontransaction txn(*conn_);
        txn.exec(std::string("ALTER SYSTEM SET autovacuum = ")
                 + (enabled ? "on" : "off"));
        txn.exec("SELECT pg_reload_conf()");
        LOGI(thread_id_, "autovacuum set to ", enabled ? "on" : "off");
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] setAutovacuum error: " << e.what() << "\n";
    }
}

void NavDB::vacuumAnalyze() {
    static const char* tables[] = {
        "nodes", "ways", "areas", "roads", "relations",
        "node_tags", "way_tags", "area_tags", "road_tags",
        "relation_tags",
        "airports", "runways", "navaids", "frequencies",
        "tags", "countries", "regions",
        "faa_obstacles",
    };
    // VACUUM cannot run inside a transaction block — pqxx::nontransaction
    // issues commands without wrapping them in BEGIN/COMMIT.
    for (const char* t : tables) {
        try {
            pqxx::nontransaction txn(*conn_);
            LOGI(thread_id_, "VACUUM ANALYZE ", t);
            txn.exec("VACUUM ANALYZE " + std::string(t));
        } catch (const std::exception& e) {
            std::cerr << "[NavDB] vacuumAnalyze(" << t << ") error: " << e.what() << "\n";
            // Continue with remaining tables rather than aborting entirely
        }
    }
}

void NavDB::truncateForResume(const std::string& phase) {
    std::vector<std::string> tables;
    if (phase == "ways") {
        tables = {"ways", "areas", "way_tags", "area_tags"};
    } else if (phase == "relations") {
        tables = {"relations", "roads", "relation_tags", "road_tags"};
    } else if (phase == "airports") {
        tables = {"countries", "regions", "airports", "tags",
                  "frequencies", "runways", "navaids"};
    } else {
        std::cerr << "[NavDB] truncateForResume: unknown phase '" << phase << "'\n";
        return;
    }

    try {
        pqxx::work txn(*conn_);
        std::string sql = "TRUNCATE " ;
        for (size_t i = 0; i < tables.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += tables[i];
        }
        LOGI(thread_id_, "truncating tables for resume (phase=", phase, "): ", sql);
        txn.exec(sql);
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] truncateForResume(" << phase << ") error: " << e.what() << "\n";
        throw; // resuming into a non-empty, partially-populated table set is
               // unsafe — better to abort than silently hit duplicate-key
               // errors mid-run
    }
}

void NavDB::initializeSchema() {
    // DDL mirrors create.sql + create_airports.sql exactly.
    // Using nontransaction since DDL (CREATE/DROP) cannot always run inside
    // a transaction block in PostgreSQL (e.g. some index types).
    static const char* statements[] = {
        // ---- OSM tables ----
        "DROP TABLE IF EXISTS public.node_tags",
        "DROP TABLE IF EXISTS public.way_tags",
        "DROP TABLE IF EXISTS public.area_tags",
        "DROP TABLE IF EXISTS public.road_tags",
        "DROP TABLE IF EXISTS public.relation_tags",
        "DROP TABLE IF EXISTS public.nodes",
        "DROP TABLE IF EXISTS public.ways",
        "DROP TABLE IF EXISTS public.areas",
        "DROP TABLE IF EXISTS public.roads",
        "DROP TABLE IF EXISTS public.relations",
        "DROP TABLE IF EXISTS public.osm_replication_state",
        "DROP TABLE IF EXISTS public.external_data_state",

        "CREATE TABLE public.nodes ("
        "  id          bigint NOT NULL,"
        "  name        varchar(256),"
        "  longitude_m double precision,"
        "  latitude_m  double precision,"
        "  geog        public.geometry,"
        "  CONSTRAINT nodes_pkey PRIMARY KEY (id))",

        "CREATE TABLE public.ways ("
        "  id   bigint NOT NULL,"
        "  name varchar(256),"
        "  geog public.geometry,"
        "  CONSTRAINT ways_pkey PRIMARY KEY (id))",

        "CREATE TABLE public.areas ("
        "  id   bigint NOT NULL,"  // positive=way, negative=relation (-id)
        "  name varchar(256),"
        "  geog public.geometry,"
        "  CONSTRAINT areas_pkey PRIMARY KEY (id))",

        "CREATE TABLE public.roads ("
        "  id   bigint NOT NULL,"
        "  name varchar(256),"
        "  geog public.geometry,"
        "  CONSTRAINT roads_pkey PRIMARY KEY (id))",

        "CREATE TABLE public.relations ("
        "  id   bigint NOT NULL,"
        "  name varchar(256),"
        "  geog public.geometry,"
        "  CONSTRAINT relations_pkey PRIMARY KEY (id))",

        "CREATE TABLE public.node_tags ("
        "  id bigint NOT NULL, key_name varchar(256), key_value varchar(256))",

        "CREATE TABLE public.way_tags ("
        "  id bigint NOT NULL, key_name varchar(256), key_value varchar(256))",

        "CREATE TABLE public.area_tags ("
        "  id bigint NOT NULL, key_name varchar(256), key_value varchar(256))",

        "CREATE TABLE public.road_tags ("
        "  id bigint NOT NULL, key_name varchar(256), key_value varchar(256))",

        "CREATE TABLE public.relation_tags ("
        "  id bigint NOT NULL, key_name varchar(256), key_value varchar(256))",

        "CREATE TABLE public.osm_replication_state ("
        "  id integer PRIMARY KEY DEFAULT 1 CHECK (id = 1),"
        "  sequence bigint NOT NULL)",

        "CREATE TABLE public.external_data_state ("
        "  name          varchar(32) PRIMARY KEY,"
        "  last_modified bigint NOT NULL,"
        "  checked_at    timestamptz NOT NULL DEFAULT now())",

        "CREATE INDEX node_tags_idx     ON public.node_tags     (id)",
        "CREATE INDEX way_tags_idx      ON public.way_tags      (id)",
        "CREATE INDEX area_tags_idx     ON public.area_tags     (id)",
        "CREATE INDEX road_tags_idx     ON public.road_tags     (id)",
        "CREATE INDEX relation_tags_idx ON public.relation_tags (id)",

        // ---- OurAirports tables ----
        "DROP TABLE IF EXISTS public.tags",
        "DROP TABLE IF EXISTS public.runways",
        "DROP TABLE IF EXISTS public.frequencies",
        "DROP TABLE IF EXISTS public.navaids",
        "DROP TABLE IF EXISTS public.airports",
        "DROP TABLE IF EXISTS public.regions",
        "DROP TABLE IF EXISTS public.countries",

        "CREATE TABLE public.countries ("
        "  id integer PRIMARY KEY, code varchar(2) NOT NULL,"
        "  name varchar(256), continent varchar(2))",

        "CREATE TABLE public.regions ("
        "  id integer PRIMARY KEY, code varchar(16) NOT NULL,"
        "  local_code varchar(16), name varchar(256),"
        "  continent varchar(2), iso_country varchar(2))",

        "CREATE TABLE public.airports ("
        "  id integer PRIMARY KEY, ident varchar(16) NOT NULL,"
        "  type varchar(32), name varchar(256),"
        "  latitude_m double precision, longitude_m double precision,"
        "  elevation_ft integer, continent varchar(2),"
        "  iso_country varchar(2), iso_region varchar(16),"
        "  municipality varchar(256), scheduled_service boolean,"
        "  icao_code varchar(8), iata_code varchar(8),"
        "  gps_code varchar(8), local_code varchar(16),"
        "  geog public.geometry)",

        "CREATE TABLE public.tags ("
        "  airport_ident varchar(16) NOT NULL, entity_type varchar(16) NOT NULL,"
        "  key_name varchar(256) NOT NULL, key_value varchar(256))",

        "CREATE TABLE public.frequencies ("
        "  id integer PRIMARY KEY, airport_ref integer NOT NULL,"
        "  airport_ident varchar(16), type varchar(16),"
        "  description varchar(256), frequency_mhz double precision)",

        "CREATE TABLE public.runways ("
        "  id integer PRIMARY KEY, airport_ref integer NOT NULL,"
        "  airport_ident varchar(16), length_ft integer, width_ft integer,"
        "  surface varchar(64), lighted boolean, closed boolean,"
        "  le_ident varchar(8), le_latitude_m double precision,"
        "  le_longitude_m double precision, le_elevation_ft integer,"
        "  le_heading_degT double precision, le_displaced_threshold_ft integer,"
        "  le_geog public.geometry,"
        "  he_ident varchar(8), he_latitude_m double precision,"
        "  he_longitude_m double precision, he_elevation_ft integer,"
        "  he_heading_degT double precision, he_displaced_threshold_ft integer,"
        "  he_geog public.geometry)",

        "CREATE TABLE public.navaids ("
        "  id integer PRIMARY KEY, ident varchar(16), name varchar(256),"
        "  type varchar(16), frequency_khz double precision,"
        "  latitude_m double precision, longitude_m double precision,"
        "  elevation_ft integer, iso_country varchar(2),"
        "  dme_frequency_khz double precision, dme_channel varchar(16),"
        "  dme_latitude_m double precision, dme_longitude_m double precision,"
        "  dme_elevation_ft integer, slaved_variation_deg double precision,"
        "  magnetic_variation_deg double precision, usage_type varchar(16),"
        "  power varchar(16), associated_airport varchar(16),"
        "  geog public.geometry)",

        "CREATE INDEX airports_geog_idx    ON public.airports  USING GIST (geog)",
        "CREATE INDEX airports_ident_idx   ON public.airports  (ident)",
        "CREATE INDEX airports_icao_idx    ON public.airports  (icao_code)",
        "CREATE INDEX airports_iata_idx    ON public.airports  (iata_code)",
        "CREATE INDEX airports_country_idx ON public.airports  (iso_country)",
        "CREATE INDEX airports_region_idx  ON public.airports  (iso_region)",
        "CREATE INDEX airports_type_idx    ON public.airports  (type)",
        "CREATE INDEX freq_airport_idx     ON public.frequencies (airport_ref)",
        "CREATE INDEX freq_ident_idx       ON public.frequencies (airport_ident)",
        "CREATE INDEX runways_airport_idx  ON public.runways (airport_ref)",
        "CREATE INDEX runways_le_geog_idx  ON public.runways USING GIST (le_geog)",
        "CREATE INDEX runways_he_geog_idx  ON public.runways USING GIST (he_geog)",
        "CREATE INDEX navaids_geog_idx     ON public.navaids  USING GIST (geog)",
        "CREATE INDEX navaids_ident_idx    ON public.navaids  (ident)",
        "CREATE INDEX navaids_type_idx     ON public.navaids  (type)",
        "CREATE INDEX navaids_country_idx  ON public.navaids  (iso_country)",
        "CREATE INDEX navaids_airport_idx  ON public.navaids  (associated_airport)",
        "CREATE INDEX tags_ident_idx       ON public.tags (airport_ident)",
        "CREATE INDEX tags_type_idx        ON public.tags (airport_ident, entity_type)",
        "CREATE INDEX regions_country_idx  ON public.regions  (iso_country)",
        "CREATE INDEX regions_code_idx     ON public.regions  (code)",

        // ---- FAA Digital Obstacle File ----
        "DROP TABLE IF EXISTS public.faa_obstacles",
        "CREATE TABLE public.faa_obstacles ("
        "  id              serial PRIMARY KEY,"
        "  oas_number      varchar(9)  NOT NULL,"
        "  verified        boolean     NOT NULL,"
        "  country         varchar(2),"
        "  state           varchar(2),"
        "  city            varchar(16),"
        "  latitude        double precision NOT NULL,"
        "  longitude       double precision NOT NULL,"
        "  obstacle_type   varchar(18),"
        "  quantity        integer,"
        "  agl_ht          integer,"
        "  amsl_ht         integer,"
        "  lighting        varchar(1),"
        "  horiz_accuracy  varchar(1),"
        "  vert_accuracy   varchar(1),"
        "  marking         varchar(1),"
        "  faa_study_no    varchar(14),"
        "  action          varchar(1),"
        "  julian_date     varchar(7),"
        "  geog            public.geometry)",
        "CREATE INDEX faa_obstacles_geog_idx  ON public.faa_obstacles USING GIST (geog)",
        "CREATE INDEX faa_obstacles_type_idx  ON public.faa_obstacles (obstacle_type)",
        "CREATE INDEX faa_obstacles_state_idx ON public.faa_obstacles (state)",
        "CREATE INDEX faa_obstacles_amsl_idx  ON public.faa_obstacles (amsl_ht)",
        "CREATE INDEX faa_obstacles_agl_idx   ON public.faa_obstacles (agl_ht)",

        // ---- FAA Class Airspace / Special Use Airspace ----
        "DROP TABLE IF EXISTS public.class_airspace",
        "CREATE TABLE public.class_airspace ("
        "  id          serial PRIMARY KEY,"
        "  ident       varchar(8),"
        "  icao_id     varchar(8),"
        "  name        text,"
        "  class       varchar(8),"
        "  type_code   varchar(16),"
        "  local_type  varchar(32),"
        "  lower_val   double precision,"
        "  lower_uom   varchar(4),"
        "  lower_code  varchar(8),"
        "  upper_val   double precision,"
        "  upper_uom   varchar(4),"
        "  upper_code  varchar(8),"
        "  city        text,"
        "  state       text,"
        "  country     text,"
        "  geog        public.geometry)",
        "CREATE INDEX class_airspace_geog_idx  ON public.class_airspace USING GIST (geog)",
        "CREATE INDEX class_airspace_class_idx ON public.class_airspace (class)",
        "CREATE INDEX class_airspace_state_idx ON public.class_airspace (state)",

        "DROP TABLE IF EXISTS public.special_use_airspace",
        "CREATE TABLE public.special_use_airspace ("
        "  id            serial PRIMARY KEY,"
        "  name          text,"
        "  type_code     varchar(8),"
        "  class         varchar(8),"
        "  lower_val     double precision,"
        "  lower_uom     varchar(4),"
        "  lower_code    varchar(8),"
        "  upper_val     double precision,"
        "  upper_uom     varchar(4),"
        "  upper_code    varchar(8),"
        "  city          text,"
        "  state         text,"
        "  country       text,"
        "  times_of_use  text,"
        "  remarks       text,"
        "  geog          public.geometry)",
        "CREATE INDEX special_use_airspace_geog_idx  ON public.special_use_airspace USING GIST (geog)",
        "CREATE INDEX special_use_airspace_type_idx  ON public.special_use_airspace (type_code)",
        "CREATE INDEX special_use_airspace_state_idx ON public.special_use_airspace (state)",

        // ---- International (non-US) airspace, from OpenAIP ----
        // type/icao_class/*_unit/*_ref are raw OpenAIP numeric enum codes,
        // not decoded — see AirspaceLoader.h for why.
        "DROP TABLE IF EXISTS public.international_airspace",
        "CREATE TABLE public.international_airspace ("
        "  id          serial PRIMARY KEY,"
        "  openaip_id  varchar(32),"
        "  name        text,"
        "  type        integer,"
        "  icao_class  integer,"
        "  country     varchar(4),"
        "  lower_val   double precision,"
        "  lower_unit  integer,"
        "  lower_ref   integer,"
        "  upper_val   double precision,"
        "  upper_unit  integer,"
        "  upper_ref   integer,"
        "  activity    integer,"
        "  on_demand   boolean,"
        "  on_request  boolean,"
        "  by_notam    boolean,"
        "  updated_at  timestamptz,"
        "  geog        public.geometry)",
        "CREATE INDEX international_airspace_geog_idx    ON public.international_airspace USING GIST (geog)",
        "CREATE INDEX international_airspace_country_idx ON public.international_airspace (country)",
        "CREATE INDEX international_airspace_type_idx    ON public.international_airspace (type)",

        // ---- Terrain (USGS 3DEP elevation raster tiles) ----
        // `terrain`'s columns/constraints are generated by raster2pgsql from
        // actual tile metadata (pixel type, dimensions, SRID, etc.) rather
        // than fixed DDL, so it isn't created here — just dropped for a
        // clean -I reset. The terrain_load tool (re)creates it as needed.
        "CREATE EXTENSION IF NOT EXISTS postgis_raster",
        "DROP TABLE IF EXISTS public.terrain",
        "DROP TABLE IF EXISTS public.terrain_tiles",
        "DROP TABLE IF EXISTS public.terrain_bands",
        "DROP TABLE IF EXISTS public.terrain_frags_staging",
    };

    for (const char* sql : statements) {
        try {
            pqxx::nontransaction txn(*conn_);
            txn.exec(sql);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("initializeSchema failed on: ") + sql + "\n  " + e.what());
        }
    }
    std::cout << "[NavDB] schema initialized\n";
}

std::vector<std::pair<double,double>> NavDB::getWayCoords(int64_t id) {
    std::vector<std::pair<double,double>> coords;
    try {
        pqxx::work txn(*conn_);
        auto res = txn.exec(
            "SELECT ST_X(p.geom), ST_Y(p.geom) "
            "FROM ("
            "  SELECT (ST_DumpPoints(geog)).geom FROM ways  WHERE id=$1 "
            "  UNION ALL "
            "  SELECT (ST_DumpPoints(geog)).geom FROM areas WHERE id=$1"
            ") p",
            pqxx::params{id});
        coords.reserve(res.size());
        for (const auto& row : res) {
            if (row[0].is_null() || row[1].is_null()) continue;
            coords.emplace_back(row[0].as<double>(), row[1].as<double>());
        }
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] getWayCoords(" << id << ") error: " << e.what() << "\n";
    }
    return coords;
}

std::unordered_map<int64_t, std::vector<std::pair<double,double>>>
NavDB::getWayCoordsMap(const std::vector<int64_t>& ids) {
    std::unordered_map<int64_t, std::vector<std::pair<double,double>>> out;
    if (ids.empty()) return out;
    try {
        pqxx::work txn(*conn_);
        // Single round trip: dump all points for all requested IDs from
        // both ways and areas, ordered so points for each ID arrive together.
        // ST_DumpPoints preserves vertex order within each geometry.
        auto res = txn.exec(
            "SELECT id, ST_X(p.geom), ST_Y(p.geom) "
            "FROM ("
            "  SELECT id, (ST_DumpPoints(geog)).geom FROM ways  WHERE id = ANY($1) "
            "  UNION ALL "
            "  SELECT id, (ST_DumpPoints(geog)).geom FROM areas WHERE id = ANY($1)"
            ") p "
            "ORDER BY id",
            pqxx::params{ids});
        for (const auto& row : res) {
            if (row[1].is_null() || row[2].is_null()) continue;
            out[row[0].as<int64_t>()].emplace_back(
                row[1].as<double>(), row[2].as<double>());
        }
    } catch (const std::exception& e) {
        std::cerr << "[NavDB] getWayCoordsMap error: " << e.what() << "\n";
    }
    return out;
}
