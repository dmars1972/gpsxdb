// Exports every region-scoped Postgres table into --out-dir/<region>/, plus
// a manifest -- the DB-side counterpart to regional_export.cpp (which
// handles nodes.dat, a raw file, not a Postgres table at all -- that one
// stays a separate tool since it isn't even talking to the same kind of
// data source). A later bundling step (regional_install's packaging, or a
// driver script) tars+gzips each region's directory (table dumps + manifest
// + <region>.nodes.dat) into <region>.gpsxdb.tar.gz.
//
// Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>
//                            [--regions-dir <dir>] [--regions name1,name2,...]
//                            [--threads N] [--parallel-table-pairs N]
//                            [--parallel-tables N] [--extra-vertices-only] [-v]
// Requires ~/.pgpass for authentication (no -p/password flag) — same
// convention as terrain_load/wmm_load/airspace_load.
//
// Originally two separate tools (regional_table_export + regional_db_export),
// merged 2026-07-30: every real invocation needed both run together in a
// fixed order to produce a usable bundle anyway (regional_table_export's
// extra_vertices.bin has to exist before regional_export's nodes.dat pass,
// and regional_install reads both tools' <table>.bin output identically
// regardless of which part produced it) -- the split was purely a
// performance fix that grew into two binaries instead of two code paths in
// one, not a real independent-use boundary. The one genuinely necessary
// split remains regional_export, which talks to a different kind of data
// source entirely (nodes.dat) and has nothing in common with either half
// of what's in this file.
//
// This tool covers two categories of table, handled two different ways:
//
// 1. The 5 big parent+child table pairs (ways/way_tags, areas/area_tags,
//    roads/road_tags, relations/relation_tags, nodes/node_tags): a
//    per-region ST_Intersects query, repeated once per region per table,
//    was measured to drive the query planner into a per-region indexed
//    scan of the underlying big table no matter how the SQL was
//    restructured (confirmed via EXPLAIN -- see the abandoned
//    mapping-table attempt noted below kTables). For these 5 pairs
//    specifically (read up to 22 times each under a naive per-region
//    approach), this instead does ONE unfiltered COPY BINARY pass per
//    table (a plain, cheap PK range scan server-side), decodes each row's
//    geometry client-side (WkbDecode.h), and classifies it against every
//    region's real polygon in-process (RegionIndex.h, the same
//    rtree-per-region infrastructure regional_export.cpp uses for
//    nodes.dat). Alongside each region's <table>.bin files, the
//    ways/areas/roads/relations parent passes also write
//    <region>/extra_vertices.bin: every vertex of a matched geometry that
//    falls OUTSIDE that same region's own polygon (a border-crossing
//    way/area/road can legitimately reference such vertices) -- consumed
//    by regional_export.cpp to widen its node classification. Per table
//    pair: the parent table's id range (MIN/MAX id) is split into
//    --threads disjoint windows; each worker gets its own raw libpq
//    connection (PgCopyBinary::Reader). The child pass reuses the
//    IDENTICAL id-range partition (same worker index means the same
//    [lo,hi] window, so a child row's id -- always some existing parent id
//    -- falls in that worker's own local map, no cross-thread lookups
//    needed). Matched rows go to per-thread-per-region temp files, merged
//    into the final <region>/<table>.bin at the end. --parallel-table-pairs
//    (default 1) controls how many of the 5 pairs run concurrently.
//    --extra-vertices-only skips writing the normal table/tag output
//    entirely (and this tool's whole second category below) -- used for a
//    one-time re-run that widens an already-built bundle's node file
//    without re-exporting already-correct table data.
//
// 2. Every other region-scoped table (small enough that N per-region
//    passes are cheap enough not to bother with the above): airports,
//    obstacles, airspace (FAA + OpenAIP + MTR/TFR), WMM, terrain,
//    countries/regions. Each table is dumped via `psql \copy ... WITH
//    (FORMAT binary)`: pqxx has no clean primitive for raw binary COPY TO
//    a client-side file, and \copy is the standard, well-tested tool for
//    exactly this job — same shell-out philosophy already used for
//    raster2pgsql in TerrainLoader. Exported --parallel-tables at a time
//    (default 4, each its own psql subprocess/connection) rather than
//    strictly sequentially, since they're independent queries that would
//    otherwise leave the DB mostly idle waiting on one query at a time.
//    Each region's real natural-boundary polygon (data/regions/<name>.wkt)
//    is upserted into public.region_boundaries at startup, and each
//    query here references that table via ST_Intersects rather than doing
//    its own client-side polygon test (unlike category 1 above) -- fine
//    for tables this size, and reusable as-is by the region-aware poll's
//    replication filter.
#include "GeoUtils.h"
#include "PgCopyBinary.h"
#include "RegionIndex.h"
#include "RegionPolygons.h"
#include "Regions.h"
#include "WkbDecode.h"

#include <boost/geometry.hpp>

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
#include <memory>
#include <type_traits>
#include <unordered_map>

// GeoUtils.h declares this extern — defined once per executable.
int g_srid = 3857;

namespace {

// ---- shared helpers ----

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdirP(const std::string& path) {
    return system(("mkdir -p '" + path + "'").c_str()) == 0;
}

// Looks up the SRID of a table's geometry/raster column once (via
// PostGIS's own registered-column bookkeeping for plain geometry columns;
// raster columns aren't in geometry_columns, so fall back to inspecting
// one row directly). column defaults to "geog"; the 5 big table pairs
// (category 1) always pass that default (no raster tables among them).
int lookupSrid(pqxx::connection& conn, const std::string& table, const std::string& column = "geog") {
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

// Creates public.region_boundaries if needed and upserts every region's
// polygon from <regions_dir>/<name>.wkt — makes every export run
// self-healing against the checked-in WKT (see include/Regions.h), no
// separate load step to remember, and leaves the table ready for the
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

void appendManifestLines(std::mutex& mtx, const std::string& region_dir, const std::string& text) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream mf(region_dir + "/manifest.txt", std::ios::app);
    mf << text;
}

// ---- category 2 helpers (small, per-region-queried tables) ----

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
// fix was moving that work client-side instead, for the 5 biggest tables
// (see the top-of-file comment's category 1). --parallel-tables (see
// above) is the improvement that survived here, for these smaller tables.
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
    {"military_training_routes","military_training_routes","geog", "SELECT * FROM public.military_training_routes WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"national_defense_tfr", "national_defense_tfr", "geog", "SELECT * FROM public.national_defense_tfr WHERE ST_Intersects(geog, {ENVELOPE})"},

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

std::string envelopeSql(const GlobalRegion& r, int srid) {
    std::ostringstream ss;
    ss << "ST_Transform((SELECT geom FROM public.region_boundaries WHERE name='"
       << r.name << "'), " << srid << ")";
    return ss.str();
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

// ---- category 1 helpers (5 big parent+child table pairs, single pass) ----

// One parent+child pair. `parent_geom_field` is the 0-based field index
// of the `geog` column in `SELECT * FROM public.<parent>` — id is always
// field 0 for every table here (NavDB.cpp's DDL puts the bigint PK/FK
// first in every case), but geog's position varies: nodes has two extra
// coordinate columns before it (id, name, longitude_m, latitude_m, geog),
// the other 4 parents don't (id, name, geog). The child table's join
// column is always its own field 0 too (its "id" column, a foreign
// reference to the parent's id, not a unique key of its own).
struct TablePair {
    const char* parent;
    int parent_geom_field;
    const char* child;
};

const std::vector<TablePair> kTablePairs = {
    {"ways",      2, "way_tags"},
    {"areas",     2, "area_tags"},
    {"roads",     2, "road_tags"},
    {"relations", 2, "relation_tags"},
    {"nodes",     4, "node_tags"},
};

// Region polygons are stored WGS84-degree in data/regions/*.wkt. Only two
// SRIDs are ever produced by this codebase's own loader (see
// GeoUtils.h's g_srid: 3857 Mercator by default, 4326 if -L/--wgs84 was
// used) -- project for Mercator, pass through as-is for 4326, and warn
// loudly (rather than silently mis-projecting) on anything else.
RegionPolygons::MultiPolygon projectForSrid(RegionPolygons::MultiPolygon mp, int srid) {
    if (srid == 4326) return mp;
    if (srid != 3857) {
        std::cerr << "[regional_db_export] WARNING: unexpected SRID " << srid
                  << " (expected 3857 or 4326) -- treating region polygons as WGS84 degrees, "
                     "matching may be wrong\n";
        return mp;
    }
    boost::geometry::for_each_point(mp, [](RegionPolygons::Point& p) {
        auto [x, y] = toMercator(p.x(), p.y());
        p.x(x);
        p.y(y);
    });
    return mp;
}

// One worker's local classification result from the parent pass: only
// ids that matched at least one region are stored (a zero-bitmask id is
// simply absent, same convention as a NULL-geometry row). Sorted by id
// after the parent pass completes, so the child pass can binary-search it.
struct LocalMap {
    std::vector<std::pair<int64_t, uint32_t>> entries;
};

uint32_t lookupBitmask(const LocalMap& m, int64_t id) {
    auto it = std::lower_bound(m.entries.begin(), m.entries.end(), id,
                                [](const std::pair<int64_t, uint32_t>& e, int64_t v) { return e.first < v; });
    if (it != m.entries.end() && it->first == id) return it->second;
    return 0;
}

// Headerless per-thread-per-region scratch file: each row is a uint32_t
// length prefix (host byte order -- private framing, never touches the
// network or another process) followed by that many raw bytes (a row's
// exact, unmodified COPY-binary field-count+fields payload as returned by
// PgCopyBinary::Reader). No COPY-binary file header/trailer here -- those
// get written exactly once, at merge time (mergeTempFiles), analogous to
// regional_export.cpp's fixed-24-byte-record temp files, generalized to
// variable-length rows.
class TempRowWriter {
public:
    explicit TempRowWriter(const std::string& path) : f_(fopen(path.c_str(), "wb")) {}
    ~TempRowWriter() { if (f_) fclose(f_); }
    TempRowWriter(const TempRowWriter&) = delete;
    TempRowWriter& operator=(const TempRowWriter&) = delete;

    void write(const std::vector<uint8_t>& raw) {
        if (!f_) return;
        uint32_t len = static_cast<uint32_t>(raw.size());
        fwrite(&len, sizeof(len), 1, f_);
        fwrite(raw.data(), 1, raw.size(), f_);
    }

private:
    FILE* f_;
};

// Streams every temp file's rows into one real PgCopyBinary::Writer
// (header once, rows verbatim, trailer once), deleting each temp file as
// it's consumed. Returns the number of rows written (for the manifest).
int64_t mergeTempFiles(const std::string& out_path, const std::vector<std::string>& temp_paths) {
    PgCopyBinary::Writer writer(out_path);
    int64_t count = 0;
    std::vector<uint8_t> buf;
    for (const auto& p : temp_paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            uint32_t len;
            while (fread(&len, sizeof(len), 1, f) == 1) {
                buf.resize(len);
                if (fread(buf.data(), 1, len, f) != len) break;
                writer.writeRow(buf);
                ++count;
            }
            fclose(f);
        }
        unlink(p.c_str());
    }
    writer.close();
    return count;
}

// Headerless per-thread-per-region scratch file for extra-vertex candidates:
// fixed 16-byte {double lon_m; double lat_m;} records, no length prefix
// needed (unlike TempRowWriter's variable-length COPY rows). Deliberately
// not deduplicated here -- a vertex can legitimately appear once per way
// that references it, and regional_export.cpp's consuming hash-map build
// dedups naturally on insert.
class ExtraVertexWriter {
public:
    explicit ExtraVertexWriter(const std::string& path) : f_(fopen(path.c_str(), "wb")) {}
    ~ExtraVertexWriter() { if (f_) fclose(f_); }
    ExtraVertexWriter(const ExtraVertexWriter&) = delete;
    ExtraVertexWriter& operator=(const ExtraVertexWriter&) = delete;

    void write(double lon_m, double lat_m) {
        if (!f_) return;
        double buf[2] = {lon_m, lat_m};
        fwrite(buf, sizeof(buf), 1, f_);
    }

private:
    FILE* f_;
};

// Enumerates every vertex of a decoded geometry (std::monostate contributes
// none). boost::geometry::for_each_point handles a bare Point too, so this
// covers all 6 GeomVariant alternatives uniformly with no per-type code.
void collectVertices(const WkbDecode::GeomVariant& geom, std::vector<RegionIndex::Point>& out) {
    std::visit([&](const auto& g) {
        using T = std::decay_t<decltype(g)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
            boost::geometry::for_each_point(g, [&](const RegionIndex::Point& p) { out.push_back(p); });
        }
    }, geom);
}

// Appends every temp file's raw 16-byte records onto <region_dir>/
// extra_vertices.bin (creating it on first append) and deletes the temp
// files. Locked: with --parallel-table-pairs > 1, multiple pairs' merge
// steps can target the same region's file concurrently (unlike
// mergeTempFiles's per-pair-unique <table>.bin, this file is shared across
// all 4 geometry-bearing pairs).
void appendExtraVertices(std::mutex& mtx, const std::string& region_dir, const std::vector<std::string>& temp_paths) {
    std::lock_guard<std::mutex> lock(mtx);
    FILE* out = fopen((region_dir + "/extra_vertices.bin").c_str(), "ab");
    std::vector<char> buf;
    for (const auto& p : temp_paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            if (out) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    buf.resize(static_cast<size_t>(sz));
                    if (fread(buf.data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz))
                        fwrite(buf.data(), 1, static_cast<size_t>(sz), out);
                }
            }
            fclose(f);
        }
        unlink(p.c_str());
    }
    if (out) fclose(out);
}

// Processes one table pair end-to-end: id-range partition, parent pass
// (classify + write matched rows + build local id->bitmask maps), child
// pass (reuse those maps against the same id-range partition), merge,
// manifest. `regions` must already be projected into this pair's table's
// coordinate system (see the srid cache in main()).
void processPair(const TablePair& pair, const std::string& conninfo, const std::string& out_dir,
                  const std::vector<RegionIndex::Entry>& regions, int threads, bool verbose,
                  bool extra_vertices_only, std::mutex& manifest_mutex, std::mutex& extra_vertices_mutex) {
    // `nodes` rows are bare Points -- a vertex "outside its own matched
    // region" is a contradiction for a Point candidate (matches() already
    // required it inside), so this pair never contributes to
    // extra_vertices.bin. In --extra-vertices-only mode there's therefore
    // nothing at all for this pair to do -- skip the whole (potentially
    // huge) table scan.
    bool emit_extra_vertices = (std::string(pair.parent) != "nodes");
    if (extra_vertices_only && !emit_extra_vertices) {
        if (verbose) std::cout << "[regional_db_export] " << pair.parent << ": skipped (--extra-vertices-only)\n";
        return;
    }

    pqxx::connection ctl(conninfo);
    int64_t min_id = 0, max_id = -1;
    {
        pqxx::work txn(ctl);
        auto r = txn.exec("SELECT MIN(id), MAX(id) FROM public." + std::string(pair.parent));
        if (!r[0][0].is_null()) {
            min_id = r[0][0].as<int64_t>();
            max_id = r[0][1].as<int64_t>();
        }
    }

    if (max_id < min_id) {
        // Empty table -- nothing to scan, but every region still needs a
        // manifest line so regional_install.cpp's binOrGzExists() check
        // (and a future reader eyeballing the manifest) sees an explicit
        // zero rather than a silently-missing entry.
        if (!extra_vertices_only) {
            for (const auto& reg : regions) {
                std::string region_dir = out_dir + "/" + reg.name;
                std::ostringstream lines;
                lines << "table." << pair.parent << ".rows=0\ntable." << pair.parent << ".format=binary\n"
                      << "table." << pair.child << ".rows=0\ntable." << pair.child << ".format=binary\n";
                appendManifestLines(manifest_mutex, region_dir, lines.str());
            }
        }
        if (verbose) std::cout << "[regional_db_export] " << pair.parent << "/" << pair.child << ": empty, skipped\n";
        return;
    }

    int nthreads = std::max(1, threads);
    int64_t span = max_id - min_id + 1;
    int64_t chunk = (span + nthreads - 1) / nthreads;

    std::vector<LocalMap> local_maps(static_cast<size_t>(nthreads));
    std::vector<std::vector<std::string>> parent_temp(static_cast<size_t>(nthreads),
                                                        std::vector<std::string>(regions.size()));
    std::vector<std::vector<std::string>> child_temp(static_cast<size_t>(nthreads),
                                                       std::vector<std::string>(regions.size()));
    std::vector<std::vector<std::string>> extra_vertex_temp(static_cast<size_t>(nthreads),
                                                              std::vector<std::string>(regions.size()));
    for (int ti = 0; ti < nthreads; ++ti) {
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            std::string base = out_dir + "/tmp_" + regions[ri].name + "_" + std::to_string(ti) + "_";
            parent_temp[static_cast<size_t>(ti)][ri] = base + pair.parent + ".raw";
            child_temp[static_cast<size_t>(ti)][ri] = base + pair.child + ".raw";
            if (emit_extra_vertices)
                extra_vertex_temp[static_cast<size_t>(ti)][ri] = base + "extra_vertices.raw";
        }
    }

    auto windowFor = [&](int ti) -> std::pair<int64_t, int64_t> {
        int64_t lo = min_id + static_cast<int64_t>(ti) * chunk;
        int64_t hi = std::min(lo + chunk - 1, max_id);
        return {lo, hi};
    };

    // ---- Parent pass: classify every row against every region, write
    // matches, record (id, bitmask) for the child pass. ----
    auto parentWorker = [&](int ti) {
        auto [lo, hi] = windowFor(ti);
        if (lo > hi) return;

        std::vector<std::unique_ptr<TempRowWriter>> writers;
        if (!extra_vertices_only) {
            writers.reserve(regions.size());
            for (size_t ri = 0; ri < regions.size(); ++ri)
                writers.push_back(std::make_unique<TempRowWriter>(parent_temp[static_cast<size_t>(ti)][ri]));
        }
        std::vector<std::unique_ptr<ExtraVertexWriter>> vwriters;
        if (emit_extra_vertices) {
            vwriters.reserve(regions.size());
            for (size_t ri = 0; ri < regions.size(); ++ri)
                vwriters.push_back(std::make_unique<ExtraVertexWriter>(extra_vertex_temp[static_cast<size_t>(ti)][ri]));
        }

        std::string sql = "SELECT * FROM public." + std::string(pair.parent) +
                           " WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi);
        PgCopyBinary::Reader reader(conninfo, sql);
        std::vector<std::pair<RegionIndex::Box, size_t>> scratch;
        std::vector<RegionIndex::Point> verts;
        auto& map = local_maps[static_cast<size_t>(ti)].entries;

        while (auto row = reader.next()) {
            int64_t id = PgCopyBinary::readBigintBE(row->fields[0]);
            const auto& gf = row->fields[static_cast<size_t>(pair.parent_geom_field)];

            uint32_t bitmask = 0;
            if (gf.data != nullptr) {
                auto decoded = WkbDecode::decode(gf.data, static_cast<size_t>(gf.len));
                if (!decoded) {
                    std::cerr << "[regional_db_export] WARNING: " << pair.parent << " id=" << id
                              << " geog failed to decode -- excluded from every region\n";
                } else {
                    verts.clear();
                    if (emit_extra_vertices) collectVertices(decoded->geom, verts);

                    for (size_t ri = 0; ri < regions.size(); ++ri) {
                        if (RegionIndex::matches(regions[ri], decoded->geom, scratch)) {
                            bitmask |= (1u << ri);
                            if (!extra_vertices_only) writers[ri]->write(row->raw);

                            // Only vertices OUTSIDE this region's own polygon are
                            // "extra" -- vertices inside it are already covered by
                            // regional_export.cpp's own per-node polygon test, and
                            // recording them here would just bloat the hash it
                            // builds from extra_vertices.bin for no benefit.
                            if (emit_extra_vertices) {
                                for (const auto& v : verts) {
                                    WkbDecode::GeomVariant vg = v;
                                    if (!RegionIndex::matches(regions[ri], vg, scratch))
                                        vwriters[ri]->write(v.x(), v.y());
                                }
                            }
                        }
                    }
                }
            }
            if (bitmask != 0) map.emplace_back(id, bitmask);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(nthreads));
        for (int ti = 0; ti < nthreads; ++ti) pool.emplace_back(parentWorker, ti);
        for (auto& t : pool) t.join();
    }
    for (auto& m : local_maps) std::sort(m.entries.begin(), m.entries.end());

    // ---- Child pass: reuses the identical id-range partition, each
    // worker classifying against only its own local map from the parent
    // pass (see top-of-file comment for why that's safe). ----
    auto childWorker = [&](int ti) {
        auto [lo, hi] = windowFor(ti);
        if (lo > hi) return;

        std::vector<std::unique_ptr<TempRowWriter>> writers;
        writers.reserve(regions.size());
        for (size_t ri = 0; ri < regions.size(); ++ri)
            writers.push_back(std::make_unique<TempRowWriter>(child_temp[static_cast<size_t>(ti)][ri]));

        std::string sql = "SELECT * FROM public." + std::string(pair.child) +
                           " WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi);
        PgCopyBinary::Reader reader(conninfo, sql);
        const auto& map = local_maps[static_cast<size_t>(ti)];

        while (auto row = reader.next()) {
            int64_t id = PgCopyBinary::readBigintBE(row->fields[0]);
            uint32_t bitmask = lookupBitmask(map, id);
            if (bitmask == 0) continue;
            for (size_t ri = 0; ri < regions.size(); ++ri)
                if (bitmask & (1u << ri)) writers[ri]->write(row->raw);
        }
    };

    // --extra-vertices-only has no use for the child table at all (tags
    // never affect geometry/vertices) -- skip that whole scan.
    if (!extra_vertices_only) {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(nthreads));
        for (int ti = 0; ti < nthreads; ++ti) pool.emplace_back(childWorker, ti);
        for (auto& t : pool) t.join();
    }

    // ---- Merge + manifest ----
    for (size_t ri = 0; ri < regions.size(); ++ri) {
        std::string region_dir = out_dir + "/" + regions[ri].name;

        if (!extra_vertices_only) {
            std::vector<std::string> p_paths, c_paths;
            p_paths.reserve(static_cast<size_t>(nthreads));
            c_paths.reserve(static_cast<size_t>(nthreads));
            for (int ti = 0; ti < nthreads; ++ti) {
                p_paths.push_back(parent_temp[static_cast<size_t>(ti)][ri]);
                c_paths.push_back(child_temp[static_cast<size_t>(ti)][ri]);
            }
            int64_t prows = mergeTempFiles(region_dir + "/" + pair.parent + ".bin", p_paths);
            int64_t crows = mergeTempFiles(region_dir + "/" + pair.child + ".bin", c_paths);

            std::ostringstream lines;
            lines << "table." << pair.parent << ".rows=" << prows << "\ntable." << pair.parent << ".format=binary\n"
                  << "table." << pair.child << ".rows=" << crows << "\ntable." << pair.child << ".format=binary\n";
            appendManifestLines(manifest_mutex, region_dir, lines.str());

            if (verbose && (prows > 0 || crows > 0)) {
                std::cout << "[regional_db_export] " << regions[ri].name << "/" << pair.parent << ": "
                          << prows << " row(s), " << pair.child << ": " << crows << " row(s)\n";
            }
        }

        if (emit_extra_vertices) {
            std::vector<std::string> v_paths;
            v_paths.reserve(static_cast<size_t>(nthreads));
            for (int ti = 0; ti < nthreads; ++ti)
                v_paths.push_back(extra_vertex_temp[static_cast<size_t>(ti)][ri]);
            appendExtraVertices(extra_vertices_mutex, region_dir, v_paths);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string host, db, user, out_dir = ".";
    std::string regions_dir = "data/regions";
    std::vector<std::string> region_filter;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    int parallel_table_pairs = 1;
    int parallel_tables = 4;
    bool verbose = false;
    bool extra_vertices_only = false;
    bool big_tables_only = false;
    bool small_tables_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i + 1 < argc) host = argv[++i];
        else if ((arg == "-d") && i + 1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i + 1 < argc) user = argv[++i];
        else if ((arg == "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions-dir") && i + 1 < argc) regions_dir = argv[++i];
        else if ((arg == "--threads") && i + 1 < argc) threads = std::stoi(argv[++i]);
        else if ((arg == "--parallel-table-pairs") && i + 1 < argc) parallel_table_pairs = std::stoi(argv[++i]);
        else if ((arg == "--parallel-tables") && i + 1 < argc) parallel_tables = std::stoi(argv[++i]);
        else if (arg == "--extra-vertices-only") extra_vertices_only = true;
        else if (arg == "--big-tables-only") big_tables_only = true;
        else if (arg == "--small-tables-only") small_tables_only = true;
        else if ((arg == "--regions") && i + 1 < argc) {
            std::stringstream ss(std::string(argv[++i]));
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>\n"
                         "                           [--regions-dir <dir>] [--regions name1,name2,...]\n"
                         "                           [--threads N] [--parallel-table-pairs N]\n"
                         "                           [--parallel-tables N] [--extra-vertices-only] [-v]\n"
                         "\n"
                         "Exports every region-scoped Postgres table for every region (see\n"
                         "include/Regions.h) into <out-dir>/<region>/<table>.bin, plus a\n"
                         "manifest.txt. Pair with regional_export for nodes.dat, then tar.\n"
                         "\n"
                         "The 5 big parent+child table pairs (ways/way_tags, areas/area_tags,\n"
                         "roads/road_tags, relations/relation_tags, nodes/node_tags) use a single\n"
                         "client-side pass (--threads worker threads, default = hardware\n"
                         "concurrency; --parallel-table-pairs, default 1, controls how many of\n"
                         "the 5 pairs run concurrently) and also write <region>/extra_vertices.bin\n"
                         "(border-crossing way/area/road vertices) for regional_export to fold in.\n"
                         "--extra-vertices-only writes only that file (no table/tag output, no\n"
                         "child-table pass, `nodes` pair skipped, and this tool's remaining\n"
                         "smaller-table export is skipped entirely) -- for widening an\n"
                         "already-built region's node file without re-exporting its\n"
                         "already-correct table data.\n"
                         "\n"
                         "Every other region-scoped table (airports, obstacles, airspace, WMM,\n"
                         "terrain, countries/regions) uses a plain per-region ST_Intersects query\n"
                         "instead -- cheap enough at this table size not to bother with the above.\n"
                         "--parallel-tables (default 4) runs that many of a region's remaining\n"
                         "table exports concurrently, each its own psql subprocess.\n"
                         "\n"
                         "--big-tables-only / --small-tables-only run just one of the two halves\n"
                         "above (mutually exclusive with each other and with\n"
                         "--extra-vertices-only). build_regional_bundles.sh uses this split\n"
                         "deliberately: the big-table pass has to cover every requested region in\n"
                         "one call (that's the whole point of the single pass), but the small-table\n"
                         "pass is run once per region so the driver script can bundle and delete\n"
                         "each region's staging data immediately after, bounding peak disk usage\n"
                         "to roughly one region's worth instead of the whole planet's at once.\n"
                         "\n"
                         "Requires ~/.pgpass for authentication. --regions-dir (default data/regions)\n"
                         "points at the WKT polygons upserted into public.region_boundaries.\n";
            std::cout.flush();
            _exit(0);  // avoid pqxx static-destructor double-free on normal return
        }
    }
    if (threads < 1) threads = 1;
    if (parallel_table_pairs < 1) parallel_table_pairs = 1;
    if (parallel_tables < 1) parallel_tables = 1;
    if (static_cast<int>(extra_vertices_only) + static_cast<int>(big_tables_only) + static_cast<int>(small_tables_only) > 1) {
        std::cerr << "Error: --extra-vertices-only, --big-tables-only, and --small-tables-only are mutually exclusive\n";
        _exit(1);
    }

    if (host.empty() || db.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n";
        _exit(1);
    }
    if (!dirExists(out_dir) && !mkdirP(out_dir)) {
        std::cerr << "Error: cannot create --out-dir " << out_dir << "\n";
        _exit(1);
    }

    std::string conninfo = "host=" + host + " dbname=" + db + " user=" + user;
    pqxx::connection conn(conninfo);

    loadRegionBoundaries(conn, regions_dir, verbose);

    auto polygons = RegionPolygons::load(regions_dir, region_filter);  // WGS84 degrees, throws on missing/bad WKT

    std::vector<GlobalRegion> selected;
    for (const auto& r : allExportRegions()) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), std::string(r.name)) == region_filter.end())
            continue;
        selected.push_back(r);
    }
    if (selected.empty()) {
        std::cerr << "Error: no matching regions (check --regions names against include/Regions.h)\n";
        _exit(1);
    }

    // Manifest header lines -- written exactly once per region, up front,
    // regardless of which of the two categories below ends up contributing
    // rows for a given table. Skipped in --extra-vertices-only mode: that
    // mode targets a scratch --out-dir used only to produce
    // extra_vertices.bin for an already-built region (no table/tag output
    // goes there, so no real manifest to own).
    //
    // Only written if manifest.txt doesn't already exist -- NOT just for
    // idempotence's sake. --big-tables-only (one call, all regions) and
    // --small-tables-only (one call per region) are two separate process
    // invocations of this same main(), run back to back by
    // build_regional_bundles.sh for every real bundle build. Unconditionally
    // truncating here on every invocation silently discarded the previous
    // invocation's already-appended table.*.rows= entries -- confirmed live:
    // after --big-tables-only wrote ways/areas/roads/relations/nodes rows
    // for a region, the following --small-tables-only call for that same
    // region overwrote manifest.txt back down to just these 3 lines before
    // appending its own tables, permanently losing the big-table entries
    // (the .bin files themselves were untouched -- regional_install.cpp
    // detects tables by file existence, not manifest entries, so this
    // wasn't data loss, but it made every shipped bundle's manifest an
    // incomplete, misleading record).
    for (const auto& r : selected) {
        std::string region_dir = out_dir + "/" + r.name;
        if (!mkdirP(region_dir)) {
            std::cerr << "Error: cannot create " << region_dir << "\n";
            _exit(1);
        }
        if (extra_vertices_only) continue;
        std::ifstream existing(region_dir + "/manifest.txt");
        if (existing.good()) continue;
        std::ofstream manifest(region_dir + "/manifest.txt");
        manifest << "region=" << r.name << "\n";
        manifest << "bbox=" << r.min_lon << "," << r.min_lat << "," << r.max_lon << "," << r.max_lat << "\n";
        manifest << "exported_at=" << static_cast<int64_t>(time(nullptr)) << "\n";
    }

    // ---- Category 1: the 5 big parent+child table pairs, single pass ----
    // Skipped entirely under --small-tables-only -- see that flag's help text.
    //
    // SRID lookup + region-index cache, built once up front (not lazily
    // inside worker threads) so it needs no locking even when
    // --parallel-table-pairs runs multiple pairs concurrently.
    if (!small_tables_only) {
        std::unordered_map<int, std::vector<RegionIndex::Entry>> regions_by_srid;
        std::vector<int> pair_srid(kTablePairs.size());
        for (size_t pi = 0; pi < kTablePairs.size(); ++pi) {
            int srid = lookupSrid(conn, kTablePairs[pi].parent);
            pair_srid[pi] = srid;
            if (regions_by_srid.count(srid)) continue;

            std::vector<RegionIndex::Entry> entries;
            entries.reserve(selected.size());
            for (const auto& r : selected) {
                RegionPolygons::MultiPolygon mp = projectForSrid(polygons.at(r.name), srid);
                auto [min_x, min_y] = (srid == 4326) ? std::pair{r.min_lon, r.min_lat} : toMercator(r.min_lon, r.min_lat);
                auto [max_x, max_y] = (srid == 4326) ? std::pair{r.max_lon, r.max_lat} : toMercator(r.max_lon, r.max_lat);
                entries.push_back(RegionIndex::build(r.name, min_x, min_y, max_x, max_y, std::move(mp)));
            }
            regions_by_srid.emplace(srid, std::move(entries));
            if (verbose) std::cout << "[regional_db_export] built region index for SRID " << srid << "\n";
        }

        if (verbose) {
            std::cout << "[regional_db_export] === big table pairs: " << selected.size() << " region(s), "
                      << kTablePairs.size() << " pair(s), --threads " << threads
                      << ", --parallel-table-pairs " << parallel_table_pairs << " ===\n";
        }

        std::mutex manifest_mutex;
        std::mutex extra_vertices_mutex;
        std::atomic<size_t> next_pair{0};
        auto pairDriver = [&]() {
            for (;;) {
                size_t pi = next_pair.fetch_add(1, std::memory_order_relaxed);
                if (pi >= kTablePairs.size()) return;
                const auto& pair = kTablePairs[pi];
                const auto& regions = regions_by_srid.at(pair_srid[pi]);
                if (verbose) std::cout << "[regional_db_export] === " << pair.parent << "/" << pair.child << " ===\n";
                processPair(pair, conninfo, out_dir, regions, threads, verbose, extra_vertices_only,
                            manifest_mutex, extra_vertices_mutex);
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(parallel_table_pairs));
        for (int i = 0; i < parallel_table_pairs; ++i) pool.emplace_back(pairDriver);
        for (auto& t : pool) t.join();
    }

    // --extra-vertices-only is a narrow one-time maintenance mode (widen an
    // already-built bundle's node file) -- it has no business touching any
    // of the remaining, already-correct table data below. --big-tables-only
    // means the caller will invoke this tool again per-region with
    // --small-tables-only (see that flag's help text) -- stop here too.
    if (extra_vertices_only || big_tables_only) {
        std::cout << "[regional_db_export] done"
                  << (extra_vertices_only ? " (--extra-vertices-only)" : " (--big-tables-only)") << "\n";
        std::cout.flush();
        _exit(0);
    }

    // ---- Category 2: every other region-scoped table, per-region query ----
    // (also runs here for the plain no-flags case, doing both categories
    // together in one process -- --small-tables-only is only needed when
    // the caller wants to invoke this half separately, per region.)
    //
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

    // Cache SRID lookups per (table, column) — most tables share g_srid,
    // but look each up individually rather than assume that.
    std::vector<int> srids;
    srids.reserve(kTables.size());
    for (auto& t : kTables) srids.push_back(lookupSrid(conn, t.geom_table, t.geom_column));

    if (verbose) std::cout << "[regional_db_export] === remaining tables, --parallel-tables " << parallel_tables << " ===\n";

    for (const auto& r : selected) {
        std::string region_dir = out_dir + "/" + r.name;
        std::mutex manifest_mu;

        if (verbose) std::cout << "[regional_db_export] === " << r.name << " ===\n";

        // kTables entries are independent queries writing independent
        // files -- run parallel_tables of them concurrently (each its own
        // psql subprocess/connection) instead of strictly sequentially,
        // see top-of-file comment for why. manifest writes from different
        // worker threads are serialized inside appendManifestLines() (it
        // takes manifest_mu itself) -- do NOT also lock manifest_mu around
        // a call to it (see the comment at that call site below): an
        // earlier version did exactly that and self-deadlocked every
        // worker thread on its first table, 100% reproducible, confirmed
        // live via unbuffered debug prints showing every thread finish
        // exactly one copyOut() and then never proceed.
        //
        // Connections are constructed here, one at a time on the main
        // thread, before any worker thread is spawned, rather than one
        // per thread inside table_worker -- harmless either way for this
        // bug, but avoids ever having a thread mid-connection-setup (this
        // Postgres requires SSL) at the same moment another thread forks
        // via popen(), which is a separate real hazard class worth not
        // inviting even though it wasn't what caused the hang above.
        std::atomic<size_t> next_idx{0};
        std::vector<std::unique_ptr<pqxx::connection>> count_conns;
        count_conns.reserve(static_cast<size_t>(parallel_tables));
        for (int p = 0; p < parallel_tables; ++p) count_conns.push_back(std::make_unique<pqxx::connection>(conninfo));

        auto table_worker = [&](pqxx::connection& count_conn) {
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
                // writing the uncompressed dump to disk first. `wmm` stays
                // on the plain path: it's tiny, so neither the disk-I/O
                // savings nor the compression itself are worth the extra
                // query. The rest of kTables (binary format) also stay
                // uncompressed -- this machine's Postgres runs
                // CPU-saturated from the spatial queries themselves, so
                // adding pigz there would compete for the same CPU the
                // queries need, for tables that are only single-digit GB
                // at most. regional_install.cpp detects the resulting .gz
                // suffix and decompresses via `\copy ... FROM PROGRAM
                // 'gunzip -c ...'` -- the manifest's format= field stays a
                // plain valid COPY format ("text"), not "text.gz".
                bool is_terrain = (t.name == "terrain");
                std::string format = (t.name == "terrain" || t.name == "wmm") ? "text" : "binary";
                std::string out_path = region_dir + "/" + t.name + ".bin";
                int64_t n;
                if (is_terrain) {
                    n = copyOutCompressedGz(host, user, db, sql, out_path) ? countRows(count_conn, sql) : -1;
                } else {
                    n = copyOut(host, user, db, sql, out_path, format);
                }

                // appendManifestLines locks manifest_mu itself -- do NOT
                // also hold it here across that call (a prior version did,
                // via an outer lock_guard on the same non-recursive mutex,
                // which self-deadlocked every worker thread on its very
                // first table: confirmed live via unbuffered debug prints
                // showing all parallel_tables threads finish exactly one
                // copyOut() each and then never proceed).
                if (n < 0) {
                    appendManifestLines(manifest_mu, region_dir, "table." + t.name + ".rows=FAILED\n");
                    std::lock_guard<std::mutex> lock(manifest_mu);
                    std::cerr << "[regional_db_export] " << r.name << "/" << t.name << " FAILED\n";
                    continue;
                }
                std::ostringstream lines;
                lines << "table." << t.name << ".rows=" << n << "\ntable." << t.name << ".format=" << format << "\n";
                appendManifestLines(manifest_mu, region_dir, lines.str());
                if (verbose) {
                    std::lock_guard<std::mutex> lock(manifest_mu);
                    std::cout << "  " << t.name << ": " << n << " row(s)\n";
                }
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(parallel_tables));
        for (int p = 0; p < parallel_tables; ++p) pool.emplace_back(table_worker, std::ref(*count_conns[p]));
        for (auto& th : pool) th.join();

        for (const auto& name : kGlobalTables) {
            std::string out_path = region_dir + "/" + name + ".bin";
            int64_t n = copyOut(host, user, db, "SELECT * FROM public." + name, out_path);
            appendManifestLines(manifest_mu, region_dir,
                "table." + name + ".rows=" + (n < 0 ? "FAILED" : std::to_string(n)) + "\n"
                "table." + name + ".format=binary\n");
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
