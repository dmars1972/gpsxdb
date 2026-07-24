// Exports one binary-COPY dump per region-scoped table, per region, plus a
// manifest, into --out-dir/<region>/ — the DB-side counterpart to
// regional_export (which handles nodes.dat). A later bundling step
// (regional_install's packaging, or a driver script) tars+gzips each
// region's directory (table dumps + manifest + <region>.nodes.dat) into
// <region>.gpsxdb.tar.gz.
//
// Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>
//                            [--regions name1,name2,...] [--parallel-tables N] [-v]
// Requires ~/.pgpass for authentication (no -p/password flag) — same
// convention as terrain_load/wmm_load/airspace_load.
//
// The 5 big parent+child table pairs (ways/way_tags, areas/area_tags,
// roads/road_tags, relations/relation_tags, nodes/node_tags) are handled
// separately, by regional_table_export (see its top-of-file comment): a
// per-region ST_Intersects query, repeated once per region per table, was
// measured this driving the query planner to do a per-region indexed scan
// of the underlying big table no matter how the SQL was restructured (see
// kTables' comment below for the mapping-table attempt that didn't help)
// -- for those 5 pairs specifically, a single client-side pass beats N
// server-side passes. This tool still owns every other region-scoped
// table (smaller, where N per-region passes are cheap enough not to
// bother) and expects regional_table_export to run first: it appends to
// (rather than overwrites) manifest.txt, and regional_install.cpp reads
// both tools' output the same way regardless of which one produced a
// given <table>.bin.
//
// Each table is dumped via `psql \copy ... WITH (FORMAT binary)`: pqxx has
// no clean primitive for raw binary COPY TO a client-side file (stream_to/
// stream_from only handle typed row-by-row transfer), and \copy is the
// standard, well-tested tool for exactly this job — same shell-out
// philosophy already used for raster2pgsql in TerrainLoader.
//
// A region's remaining ~10 tables are exported --parallel-tables at a time
// (default 4, each its own psql subprocess/connection) rather than
// strictly sequentially: they're independent queries writing independent
// files, and serializing them back-to-back left most of the DB server idle
// waiting on one query at a time. Each Postgres worker process itself may
// still use intra-query parallelism (see EXPLAIN's "Workers Planned") --
// --parallel-tables is deliberately modest (not e.g. 20) so concurrent
// exports don't starve each other of the server's max_parallel_workers pool.
//
// Each region's real natural-boundary polygon (data/regions/<name>.wkt, see
// include/Regions.h) is upserted into a small public.region_boundaries
// table (name text primary key, geom geometry(MultiPolygon,4326)) at
// startup — self-healing against the checked-in WKT on every run, and
// reusable as-is by the not-yet-built region-aware poll/replication filter.
// Each table query wraps a subquery against that table in
// ST_Transform((SELECT geom FROM region_boundaries WHERE name=...), <col_srid>)
// so the export works regardless of whether the target database was loaded
// with Mercator or WGS84 geometry (g_srid) — the SRID is looked up once per
// table, not assumed.
#include "Regions.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <mutex>

namespace {

struct TableExport {
    std::string name;            // used for the output filename: <name>.bin
    std::string geom_table;      // table whose geometry column drives the SRID lookup + envelope test
    std::string geom_column;     // "geog" for normal tables, "ST_ConvexHull(rast)" for raster tables
    std::string select_sql;      // full SELECT, with {ENVELOPE} substituted in
};

// {ENVELOPE} is replaced with ST_Transform(ST_MakeEnvelope(min_lon,min_lat,max_lon,max_lat,4326), <srid>)
//
// Tried (and reverted) a precomputed id->region_name mapping table per base
// table here, to turn the repeated-per-region ST_Intersects into one
// spatial pass + cheap indexed joins. Measured no win: Postgres's planner
// drives the join from region_boundaries (small) via a nested loop that
// does one index-driven scan of the big table PER region anyway (confirmed
// via EXPLAIN) -- algorithmically identical to the original per-region
// queries, just consolidated into one SQL statement. Forcing a true single
// sequential pass would mean bypassing the spatial index entirely, likely
// worse server-side for a query planner to choose on its own -- the actual
// fix was moving that work client-side instead (regional_table_export,
// which now handles the 5 biggest tables this way; see this file's
// top-of-file comment). --parallel-tables (see above) is the improvement
// that survived here, for the tables staying on this per-region-query path.
const std::vector<TableExport> kTables = {
    {"airports",     "airports", "geog", "SELECT * FROM public.airports WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"airport_tags", "airports", "geog", "SELECT t.* FROM public.tags t JOIN public.airports a ON a.ident = t.airport_ident WHERE ST_Intersects(a.geog, {ENVELOPE})"},
    {"frequencies",  "airports", "geog", "SELECT f.* FROM public.frequencies f JOIN public.airports a ON a.id = f.airport_ref WHERE ST_Intersects(a.geog, {ENVELOPE})"},
    {"runways",      "airports", "geog", "SELECT r.* FROM public.runways r JOIN public.airports a ON a.id = r.airport_ref WHERE ST_Intersects(a.geog, {ENVELOPE})"},
    {"navaids",      "navaids",  "geog", "SELECT * FROM public.navaids WHERE ST_Intersects(geog, {ENVELOPE})"},

    {"faa_obstacles",         "faa_obstacles",         "geog", "SELECT * FROM public.faa_obstacles WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"class_airspace",        "class_airspace",        "geog", "SELECT * FROM public.class_airspace WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"special_use_airspace",  "special_use_airspace",  "geog", "SELECT * FROM public.special_use_airspace WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"international_airspace","international_airspace","geog", "SELECT * FROM public.international_airspace WHERE ST_Intersects(geog, {ENVELOPE})"},

    // Raster tables: index is on ST_ConvexHull(rast) (raster2pgsql -I default), so
    // the predicate must match that expression, not a plain geog column.
    {"terrain", "terrain", "ST_ConvexHull(rast)", "SELECT * FROM public.terrain WHERE ST_Intersects(ST_ConvexHull(rast), {ENVELOPE})"},
    {"wmm",     "wmm",     "ST_ConvexHull(rast)", "SELECT * FROM public.wmm WHERE ST_Intersects(ST_ConvexHull(rast), {ENVELOPE})"},

    {"wmm_bands", "wmm_bands", "geog", "SELECT * FROM public.wmm_bands WHERE ST_Intersects(geog, {ENVELOPE})"},

    // Not region-filtered — import-time bookkeeping only (terrain_tiles,
    // wmm_cells have no geometry column, aren't meaningful to a customer's
    // install) or intentionally global/tiny (countries, regions).
};

const std::vector<std::string> kGlobalTables = {"countries", "regions"};

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool mkdirP(const std::string& path) {
    return system(("mkdir -p '" + path + "'").c_str()) == 0;
}

// Looks up the SRID of a table's geometry/raster column once (via
// PostGIS's own registered-column bookkeeping for plain geometry columns;
// raster columns aren't in geometry_columns, so fall back to inspecting
// one row directly).
int lookupSrid(pqxx::connection& conn, const std::string& table, const std::string& column) {
    pqxx::work txn(conn);
    if (column == "ST_ConvexHull(rast)") {
        auto r = txn.exec("SELECT ST_SRID(rast) FROM public." + table + " LIMIT 1");
        return r.empty() ? 3857 : r[0][0].as<int>();
    }
    auto r = txn.exec(
        "SELECT srid FROM geometry_columns WHERE f_table_schema='public' "
        "AND f_table_name=$1 AND f_geometry_column=$2",
        pqxx::params{table, column});
    // NavDB's DDL declares columns as untyped `public.geometry` (no SRID
    // typmod), so geometry_columns reports srid=0 even though every stored
    // value carries a real embedded SRID -- 0 isn't usable as an
    // ST_Transform target, so treat it as "not found" too, not just an
    // empty result set.
    if (!r.empty() && r[0][0].as<int>() != 0) return r[0][0].as<int>();
    // Fallback: inspect an actual row's embedded SRID directly.
    auto r2 = txn.exec("SELECT ST_SRID(" + column + ") FROM public." + table + " WHERE " + column + " IS NOT NULL LIMIT 1");
    return r2.empty() ? 3857 : r2[0][0].as<int>();
}

std::string envelopeSql(const GlobalRegion& r, int srid) {
    std::ostringstream ss;
    ss << "ST_Transform((SELECT geom FROM public.region_boundaries WHERE name='"
       << r.name << "'), " << srid << ")";
    return ss.str();
}

// Creates public.region_boundaries if needed and upserts every region's
// polygon from <regions_dir>/<name>.wkt — makes every export run
// self-healing against the checked-in WKT (see include/Regions.h), no
// separate load step to remember, and leaves the table ready for a future
// region-aware poll/replication filter to query directly.
void loadRegionBoundaries(pqxx::connection& conn, const std::string& regions_dir, bool verbose) {
    {
        pqxx::work txn(conn);
        txn.exec("CREATE TABLE IF NOT EXISTS public.region_boundaries ("
                 "name text PRIMARY KEY, geom geometry(MultiPolygon, 4326))");
        txn.exec("CREATE INDEX IF NOT EXISTS region_boundaries_geom_idx "
                 "ON public.region_boundaries USING GIST (geom)");
        txn.commit();
    }
    for (const auto& r : allExportRegions()) {
        std::string path = regions_dir + "/" + std::string(r.name) + ".wkt";
        std::ifstream in(path);
        if (!in) {
            std::cerr << "[regional_db_export] cannot open " << path
                      << " -- region_boundaries not updated for " << r.name << "\n";
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();

        pqxx::work txn(conn);
        txn.exec("INSERT INTO public.region_boundaries (name, geom) "
                 "VALUES ($1, ST_GeomFromText($2, 4326)) "
                 "ON CONFLICT (name) DO UPDATE SET geom = EXCLUDED.geom",
                 pqxx::params{std::string(r.name), ss.str()});
        txn.commit();
        if (verbose) std::cout << "[regional_db_export] region_boundaries: " << r.name << "\n";
    }
    // Without this, the planner has no row-count/size stats for a table
    // that just went from empty to 10 rows via INSERT (not a bulk load it
    // auto-analyzes) -- confirmed via EXPLAIN to make it estimate ~880 rows
    // instead of the real 10, badly skewing the cost of any query that
    // joins region_boundaries against a big table (~88x in one measured
    // case) and picking a correspondingly worse plan.
    {
        pqxx::work txn(conn);
        txn.exec("ANALYZE public.region_boundaries");
        txn.commit();
    }
}

// Safely single-quotes an arbitrary string for use as one shell argument.
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// COUNT(*) via pqxx of an arbitrary SELECT -- used to get the row count for
// text-format tables, since (unlike copyOut()'s file-based \copy)
// copyOutCompressedGz streams straight through a compressor and never
// prints the usual "COPY N" notice (confirmed empirically: `\copy ... TO
// STDOUT` suppresses it entirely rather than mixing it into the piped data).
int64_t countRows(pqxx::connection& conn, const std::string& select_sql) {
    pqxx::work txn(conn);
    auto r = txn.exec("SELECT COUNT(*) FROM (" + select_sql + ") q");
    return r[0][0].as<int64_t>();
}

// Streams `psql \copy (<select_sql>) TO STDOUT WITH (FORMAT text)` straight
// through pigz to <out_path>.gz -- the uncompressed data never touches disk
// (unlike write-then-compress), which matters given terrain/wmm's
// hex-encoded raster dumps can run to hundreds of GB uncompressed for a
// large region (measured). select_sql and out_path are passed as bash
// positional parameters ($1/$2, via safely shell-quoted arguments) rather
// than interpolated into the script text, since select_sql routinely
// contains single quotes (e.g. name='urals') that would otherwise break
// naive quoting; `set -o pipefail` (bash-specific, hence explicit `bash -c`
// rather than relying on system()'s default /bin/sh, which may be dash)
// makes a psql failure fail the whole pipeline even though pigz's own exit
// code would otherwise mask it.
bool copyOutCompressedGz(const std::string& host, const std::string& user, const std::string& db,
                         const std::string& select_sql, const std::string& out_path) {
    std::ostringstream script;
    script << "set -o pipefail; psql -h " << host << " -U " << user << " -d " << db
           << " -v ON_ERROR_STOP=1 -A -t -c \"\\copy ($1) TO STDOUT WITH (FORMAT text)\""
           << " | pigz > \"$2\"";

    std::ostringstream cmd;
    cmd << "bash -c " << shellQuote(script.str()) << " _ "
        << shellQuote(select_sql) << " " << shellQuote(out_path + ".gz") << " 2>&1";

    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return false;
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) output += buf;
    int rc = pclose(p);
    if (rc != 0) {
        std::cerr << "[regional_db_export] compressed copy failed: " << output << "\n";
        return false;
    }
    return true;
}

// Runs `psql \copy (<select_sql>) TO '<out_path>' WITH (FORMAT <format>)`,
// returns the row count psql reports ("COPY N"), or -1 on failure. `format`
// defaults to binary; raster columns (terrain/wmm) have no binary
// send/recv function registered, so those two tables use "text" instead
// (regional_install must load them the same way).
int64_t copyOut(const std::string& host, const std::string& user, const std::string& db,
                const std::string& select_sql, const std::string& out_path,
                const std::string& format = "binary") {
    std::ostringstream cmd;
    cmd << "psql -h " << host << " -U " << user << " -d " << db
        << " -v ON_ERROR_STOP=1 -A -t -c \"\\copy (" << select_sql << ") TO '"
        << out_path << "' WITH (FORMAT " << format << ")\" 2>&1";

    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return -1;
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) output += buf;
    int rc = pclose(p);
    if (rc != 0) {
        std::cerr << "[regional_db_export] psql failed: " << output << "\n";
        return -1;
    }
    // psql -A -t on a \copy prints "COPY <n>" to stdout.
    auto pos = output.find("COPY ");
    if (pos == std::string::npos) return 0;
    try {
        return std::stoll(output.substr(pos + 5));
    } catch (...) {
        return 0;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string host, db, user, out_dir = ".";
    std::string regions_dir = "data/regions";
    std::vector<std::string> region_filter;
    int parallel_tables = 4;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) host = argv[++i];
        else if ((arg == "-d") && i+1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user = argv[++i];
        else if ((arg == "--out-dir") && i+1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions-dir") && i+1 < argc) regions_dir = argv[++i];
        else if ((arg == "--parallel-tables") && i+1 < argc) parallel_tables = std::stoi(argv[++i]);
        else if ((arg == "--regions") && i+1 < argc) {
            std::stringstream ss(std::string(argv[++i]));
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>\n"
                         "                           [--regions-dir <dir>] [--regions name1,name2,...]\n"
                         "                           [--parallel-tables N] [-v]\n"
                         "\n"
                         "Writes <out-dir>/<region>/<table>.bin (binary COPY format) for every\n"
                         "region-scoped table not already handled by regional_table_export (the\n"
                         "5 big parent+child pairs), plus appends to <out-dir>/<region>/manifest.txt.\n"
                         "Run regional_table_export first. Does not bundle/compress -- pair with\n"
                         "regional_export for nodes.dat, then tar.\n"
                         "Requires ~/.pgpass for authentication. --regions-dir (default data/regions)\n"
                         "points at the WKT polygons upserted into public.region_boundaries.\n"
                         "--parallel-tables (default 4) runs that many of a region's remaining\n"
                         "table exports concurrently, each its own psql subprocess.\n";
            std::cout.flush();
            _exit(0);  // avoid pqxx static-destructor double-free on normal return
        }
    }
    if (parallel_tables < 1) parallel_tables = 1;

    if (host.empty() || db.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n";
        _exit(1);
    }
    if (!dirExists(out_dir) && !mkdirP(out_dir)) {
        std::cerr << "Error: cannot create --out-dir " << out_dir << "\n";
        _exit(1);
    }

    pqxx::connection conn("host=" + host + " dbname=" + db + " user=" + user);

    loadRegionBoundaries(conn, regions_dir, verbose);

    // Cache SRID lookups per (table, column) — most tables share g_srid,
    // but look each up individually rather than assume that.
    std::vector<int> srids;
    srids.reserve(kTables.size());
    for (auto& t : kTables) srids.push_back(lookupSrid(conn, t.geom_table, t.geom_column));

    // public.terrain's DDL is generated by raster2pgsql from actual tile
    // metadata (pixel type, scale, SRID-specific CHECK constraints), not
    // fixed like every other table here — NavDB::ensureSchema() deliberately
    // doesn't create it (see its doc comment). Capture the real DDL once so
    // regional_install can apply it verbatim on a target that doesn't have
    // the table yet, instead of trying to hand-synthesize it.
    std::string terrain_schema_path = out_dir + "/terrain.schema.sql";
    {
        std::ostringstream cmd;
        cmd << "pg_dump -h " << host << " -U " << user << " -d " << db
            << " --schema-only --table=public.terrain -f '" << terrain_schema_path << "' 2>&1";
        int rc = system(cmd.str().c_str());
        if (rc != 0) {
            std::cerr << "[regional_db_export] pg_dump of public.terrain schema failed "
                         "(table may not exist yet) -- terrain rows, if any, will not be "
                         "installable until this is captured\n";
            terrain_schema_path.clear();
        } else if (verbose) {
            std::cout << "[regional_db_export] captured public.terrain schema\n";
        }
    }

    for (const auto& r : allExportRegions()) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), std::string(r.name)) == region_filter.end())
            continue;

        std::string region_dir = out_dir + "/" + r.name;
        if (!mkdirP(region_dir)) {
            std::cerr << "Error: cannot create " << region_dir << "\n";
            continue;
        }

        if (verbose) std::cout << "[regional_db_export] === " << r.name << " ===\n";

        // regional_table_export (the 5 big parent+child table pairs' single-
        // pass exporter -- see its top-of-file comment) is meant to run
        // before this tool and already wrote the header lines plus its own
        // table.*.rows/format entries into this same manifest.txt. Append
        // rather than truncate, and skip re-writing the header, so a run of
        // this tool alone (manifest doesn't exist yet) still produces a
        // complete manifest, while running after regional_table_export
        // composes onto its output instead of clobbering it.
        bool manifest_exists = fileExists(region_dir + "/manifest.txt");
        std::ofstream manifest(region_dir + "/manifest.txt", std::ios::app);
        if (!manifest_exists) {
            manifest << "region=" << r.name << "\n";
            manifest << "bbox=" << r.min_lon << "," << r.min_lat << "," << r.max_lon << "," << r.max_lat << "\n";
            manifest << "exported_at=" << static_cast<int64_t>(time(nullptr)) << "\n";
        }

        // kTables' remaining entries are independent queries writing
        // independent files -- run parallel_tables of them concurrently
        // (each its own psql subprocess/connection) instead of strictly
        // sequentially, see top-of-file comment for why. manifest/cerr/cout
        // writes from different worker threads are serialized via
        // manifest_mu.
        std::atomic<size_t> next_idx{0};
        std::mutex manifest_mu;
        auto table_worker = [&]() {
            // Own connection per worker thread -- only used for the
            // COUNT(*) queries text-format tables need (see
            // copyOutCompressedGz's comment); pqxx::connection isn't safe
            // to share across threads. Opened once per worker regardless
            // of whether this thread ever hits a text-format table, since
            // that's unknown up front and connection setup is cheap next
            // to everything else this loop does.
            pqxx::connection count_conn("host=" + host + " dbname=" + db + " user=" + user);
            for (;;) {
                size_t i = next_idx.fetch_add(1);
                if (i >= kTables.size()) return;
                const auto& t = kTables[i];
                std::string envelope = envelopeSql(r, srids[i]);
                std::string sql = t.select_sql;
                auto pos = sql.find("{ENVELOPE}");
                sql.replace(pos, std::string("{ENVELOPE}").size(), envelope);

                // Raster columns (terrain, wmm) have no binary send/recv
                // function, so those two use "text" instead -- hex-encoded
                // raw bytes, ~2x bloat from the encoding alone. For
                // `terrain` specifically this has measured to hundreds of
                // GB uncompressed for a single large region, so its
                // export streams straight through pigz rather than
                // writing the uncompressed dump to disk first (avoiding
                // that disk I/O, at the cost of a separate COUNT(*) query
                // since `\copy ... TO STDOUT` doesn't report a row count --
                // see copyOutCompressedGz/countRows). `wmm` stays on the
                // plain path: it's tiny (hundreds of KB), so neither the
                // disk-I/O savings nor the compression itself are worth
                // the extra query. The rest of kTables (binary format)
                // also stay uncompressed -- this machine's Postgres runs
                // CPU-saturated from the spatial queries themselves
                // (confirmed via uptime/iostat: load ~12 on 12 cores,
                // disk near-idle), so adding pigz there would compete for
                // the same CPU the queries need, for tables that are only
                // single-digit GB at most -- a much smaller win than
                // terrain's. regional_install.cpp detects the resulting
                // .gz suffix and decompresses via `\copy ... FROM PROGRAM
                // 'gunzip -c ...'` -- the manifest's format= field stays a
                // plain valid COPY format ("text"), not "text.gz", since
                // regional_install passes it straight through as the
                // FORMAT clause value.
                bool is_terrain = (t.name == "terrain");
                std::string format = (t.name == "terrain" || t.name == "wmm") ? "text" : "binary";
                std::string out_path = region_dir + "/" + t.name + ".bin";
                int64_t n;
                if (is_terrain) {
                    n = copyOutCompressedGz(host, user, db, sql, out_path) ? countRows(count_conn, sql) : -1;
                } else {
                    n = copyOut(host, user, db, sql, out_path, format);
                }

                std::lock_guard<std::mutex> lock(manifest_mu);
                if (n < 0) {
                    std::cerr << "[regional_db_export] " << r.name << "/" << t.name << " FAILED\n";
                    manifest << "table." << t.name << ".rows=FAILED\n";
                    continue;
                }
                manifest << "table." << t.name << ".rows=" << n << "\n";
                manifest << "table." << t.name << ".format=" << format << "\n";
                if (verbose) std::cout << "  " << t.name << ": " << n << " row(s)\n";
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(parallel_tables);
        for (int p = 0; p < parallel_tables; ++p) pool.emplace_back(table_worker);
        for (auto& th : pool) th.join();

        for (const auto& name : kGlobalTables) {
            std::string out_path = region_dir + "/" + name + ".bin";
            int64_t n = copyOut(host, user, db, "SELECT * FROM public." + name, out_path);
            manifest << "table." << name << ".rows=" << (n < 0 ? "FAILED" : std::to_string(n)) << "\n";
            manifest << "table." << name << ".format=binary\n";
            if (verbose) std::cout << "  " << name << " (global): " << n << " row(s)\n";
        }

        if (!terrain_schema_path.empty()) {
            system(("cp '" + terrain_schema_path + "' '" + region_dir + "/terrain.schema.sql'").c_str());
        }
    }

    std::cout << "[regional_db_export] done\n";
    std::cout.flush();
    _exit(0);  // avoid pqxx static-destructor double-free on normal return
}
