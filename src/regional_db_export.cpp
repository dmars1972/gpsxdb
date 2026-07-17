// Exports one binary-COPY dump per region-scoped table, per region, plus a
// manifest, into --out-dir/<region>/ — the DB-side counterpart to
// regional_export (which handles nodes.dat). A later bundling step
// (regional_install's packaging, or a driver script) tars+gzips each
// region's directory (table dumps + manifest + <region>.nodes.dat) into
// <region>.gpsxdb.tar.gz.
//
// Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>
//                            [--regions name1,name2,...] [-v]
// Requires ~/.pgpass for authentication (no -p/password flag) — same
// convention as terrain_load/wmm_load/airspace_load.
//
// Each table is dumped via `psql \copy ... WITH (FORMAT binary)`: pqxx has
// no clean primitive for raw binary COPY TO a client-side file (stream_to/
// stream_from only handle typed row-by-row transfer), and \copy is the
// standard, well-tested tool for exactly this job — same shell-out
// philosophy already used for raster2pgsql in TerrainLoader.
//
// Region bboxes are WGS84 degrees (see include/Regions.h); each query wraps
// them in ST_Transform(ST_MakeEnvelope(...,4326), <col_srid>) so the export
// works regardless of whether the target database was loaded with Mercator
// or WGS84 geometry (g_srid) — the SRID is looked up once per table, not
// assumed.
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

namespace {

struct TableExport {
    std::string name;            // used for the output filename: <name>.bin
    std::string geom_table;      // table whose geometry column drives the SRID lookup + envelope test
    std::string geom_column;     // "geog" for normal tables, "ST_ConvexHull(rast)" for raster tables
    std::string select_sql;      // full SELECT, with {ENVELOPE} substituted in
};

// {ENVELOPE} is replaced with ST_Transform(ST_MakeEnvelope(min_lon,min_lat,max_lon,max_lat,4326), <srid>)
const std::vector<TableExport> kTables = {
    {"ways",       "ways",   "geog", "SELECT * FROM public.ways WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"way_tags",   "ways",   "geog", "SELECT wt.* FROM public.way_tags wt JOIN public.ways w ON w.id = wt.id WHERE ST_Intersects(w.geog, {ENVELOPE})"},

    {"areas",      "areas",  "geog", "SELECT * FROM public.areas WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"area_tags",  "areas",  "geog", "SELECT at.* FROM public.area_tags at JOIN public.areas a ON a.id = at.id WHERE ST_Intersects(a.geog, {ENVELOPE})"},

    {"roads",      "roads",  "geog", "SELECT * FROM public.roads WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"road_tags",  "roads",  "geog", "SELECT rt.* FROM public.road_tags rt JOIN public.roads r ON r.id = rt.id WHERE ST_Intersects(r.geog, {ENVELOPE})"},

    {"relations",      "relations", "geog", "SELECT * FROM public.relations WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"relation_tags",  "relations", "geog", "SELECT rt.* FROM public.relation_tags rt JOIN public.relations r ON r.id = rt.id WHERE ST_Intersects(r.geog, {ENVELOPE})"},

    {"nodes",      "nodes",  "geog", "SELECT * FROM public.nodes WHERE ST_Intersects(geog, {ENVELOPE})"},
    {"node_tags",  "nodes",  "geog", "SELECT nt.* FROM public.node_tags nt JOIN public.nodes n ON n.id = nt.id WHERE ST_Intersects(n.geog, {ENVELOPE})"},

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
    ss << "ST_Transform(ST_MakeEnvelope(" << r.min_lon << "," << r.min_lat << ","
       << r.max_lon << "," << r.max_lat << ",4326), " << srid << ")";
    return ss.str();
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
    std::vector<std::string> region_filter;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) host = argv[++i];
        else if ((arg == "-d") && i+1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user = argv[++i];
        else if ((arg == "--out-dir") && i+1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions") && i+1 < argc) {
            std::stringstream ss(std::string(argv[++i]));
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_db_export -s <host> -d <db> -u <user> --out-dir <dir>\n"
                         "                           [--regions name1,name2,...] [-v]\n"
                         "\n"
                         "Writes <out-dir>/<region>/<table>.bin (binary COPY format) for every\n"
                         "region-scoped table, plus <out-dir>/<region>/manifest.txt. Does not\n"
                         "bundle/compress -- pair with regional_export for nodes.dat, then tar.\n"
                         "Requires ~/.pgpass for authentication.\n";
            _exit(0);  // avoid pqxx static-destructor double-free on normal return
        }
    }

    if (host.empty() || db.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n";
        _exit(1);
    }
    if (!dirExists(out_dir) && !mkdirP(out_dir)) {
        std::cerr << "Error: cannot create --out-dir " << out_dir << "\n";
        _exit(1);
    }

    pqxx::connection conn("host=" + host + " dbname=" + db + " user=" + user);

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

    for (const auto& r : kGlobalRegions) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), std::string(r.name)) == region_filter.end())
            continue;

        std::string region_dir = out_dir + "/" + r.name;
        if (!mkdirP(region_dir)) {
            std::cerr << "Error: cannot create " << region_dir << "\n";
            continue;
        }

        if (verbose) std::cout << "[regional_db_export] === " << r.name << " ===\n";

        std::ofstream manifest(region_dir + "/manifest.txt");
        manifest << "region=" << r.name << "\n";
        manifest << "bbox=" << r.min_lon << "," << r.min_lat << "," << r.max_lon << "," << r.max_lat << "\n";
        manifest << "exported_at=" << static_cast<int64_t>(time(nullptr)) << "\n";

        for (size_t i = 0; i < kTables.size(); ++i) {
            const auto& t = kTables[i];
            std::string envelope = envelopeSql(r, srids[i]);
            std::string sql = t.select_sql;
            auto pos = sql.find("{ENVELOPE}");
            sql.replace(pos, std::string("{ENVELOPE}").size(), envelope);

            // Raster columns (terrain, wmm) have no binary send/recv function.
            std::string format = (t.name == "terrain" || t.name == "wmm") ? "text" : "binary";
            std::string out_path = region_dir + "/" + t.name + ".bin";
            int64_t n = copyOut(host, user, db, sql, out_path, format);
            if (n < 0) {
                std::cerr << "[regional_db_export] " << r.name << "/" << t.name << " FAILED\n";
                manifest << "table." << t.name << ".rows=FAILED\n";
                continue;
            }
            manifest << "table." << t.name << ".rows=" << n << "\n";
            manifest << "table." << t.name << ".format=" << format << "\n";
            if (verbose) std::cout << "  " << t.name << ": " << n << " row(s)\n";
        }

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
