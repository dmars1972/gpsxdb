// Installs a region bundle (produced by regional_export + regional_db_export,
// packaged as <region>.gpsxdb.tar.gz) into a target Postgres database and
// merges the region's node coordinates into the target's local nodes.dat.
// Safe to run more than once against the same target with different (or
// even overlapping) regions — every row insert is deduped, so re-running
// with a region that shares boundary features with an already-installed one
// is a no-op for those rows, not an error or a duplicate.
//
// Usage: regional_install <bundle.gpsxdb.tar.gz> -s <host> -d <db> -u <user>
//                          --nodes-file <target nodes.dat path> [-v]
//
// Dedup strategy:
//   - Every table with a real primary key (ways, areas, roads, relations,
//     nodes, airports, navaids, frequencies, runways, faa_obstacles,
//     class_airspace, special_use_airspace, international_airspace,
//     wmm_bands, terrain, wmm, countries, regions) is deduped with
//     INSERT ... ON CONFLICT (pk) DO NOTHING. The pk value comes straight
//     from the source database's COPY dump (COPY preserves explicit column
//     values, including serial-defaulted ones -- it never re-triggers
//     nextval()), so it's stable and meaningful across every region export
//     that happens to include the same boundary-spanning row.
//   - The five OSM *_tags tables (way_tags/area_tags/road_tags/
//     relation_tags/node_tags) and the airport `tags` table have no primary
//     key or unique constraint at all, so ON CONFLICT has nothing to target.
//     Instead, each is inserted only for parent rows that were newly added
//     in *this* install (computed via a temp "new ids" set before the
//     parent's own ON CONFLICT insert) -- a boundary parent row that's
//     already present means its tags are already present too, so they're
//     correctly skipped.
//
// All staging/dedup SQL runs as ONE psql -f script over a single session
// (needed for TEMP TABLEs and \copy ... FORMAT binary to share scope),
// wrapped in one transaction so a region's install is all-or-nothing.
#include "NavDB.h"
#include "RegionalNodeMap.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <unistd.h>  // _exit -- MinGW-w64 provides this natively too

namespace fs = std::filesystem;

namespace {

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// Every table's export is a plain file -- terrain used to optionally ship
// pre-compressed as terrain.bin.gz (regional_db_export.cpp's old inline
// pigz pass, removed 2026-08-08 since it just double-compressed against
// the bundle's own outer tar.gz), but all 22 region bundles are being
// rebuilt with the fixed exporter, so no target file is ever gzipped now.
std::string copyFromClause(const std::string& bin_path) {
    return "'" + bin_path + "'";
}

bool dirExists(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

// mkdtemp() isn't available on MinGW-w64 -- create a directory with a
// random suffix under `parent` instead, retrying on the (vanishingly
// unlikely) chance of a collision. Used on both platforms rather than a
// Windows-only shim, so there's exactly one temp-dir-creation path to
// reason about instead of two.
std::string makeTempDir(const std::string& parent) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string suffix;
        for (int i = 0; i < 12; ++i) suffix += charset[gen() % (sizeof(charset) - 1)];
        fs::path candidate = fs::path(parent) / ("regional_install." + suffix);
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate.string();
    }
    throw std::runtime_error("makeTempDir: failed to create a unique temp dir under " + parent);
}

// Equivalent to `find <root> -mindepth 1 -maxdepth 2 -name <filename>`,
// returning the first match's parent directory (or "" if none). A plain
// nested loop rather than std::filesystem::recursive_directory_iterator's
// depth-limiting API, which is easy to get subtly wrong -- this is easy to
// verify correct by inspection instead.
std::string findFileWithinTwoLevels(const std::string& root, const std::string& filename) {
    std::error_code ec;
    for (const auto& e1 : fs::directory_iterator(root, ec)) {
        if (e1.is_regular_file() && e1.path().filename() == filename)
            return e1.path().parent_path().string();
        if (e1.is_directory()) {
            std::error_code ec2;
            for (const auto& e2 : fs::directory_iterator(e1.path(), ec2)) {
                if (e2.is_regular_file() && e2.path().filename() == filename)
                    return e2.path().parent_path().string();
            }
        }
    }
    return "";
}

// Runs a command built from an argument vector (argv[0] = program name),
// capturing combined stdout+stderr -- callers never need to think about
// shell quoting, which differs completely between POSIX shells and
// cmd.exe. Uses popen/_popen rather than exec directly, since capturing
// output through a pipe is the simplest cross-platform way to get both
// the exit code and any error text for reporting. stream_output echoes
// each line to stdout as it arrives (matches the previous verbose-mode
// behavior), independent of the buffered `output` this always returns.
struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

#ifdef _WIN32
// Windows command-line quoting: wrap in double quotes if the argument
// contains whitespace or a double quote, doubling any backslashes that
// immediately precede a quote and escaping the quote itself -- the
// convention the MSVC runtime's argument parser (and so cmd.exe) expects.
std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { out.append(backslashes * 2 + 1, '\\'); backslashes = 0; out += '"'; continue; }
        if (backslashes) { out.append(backslashes, '\\'); backslashes = 0; }
        out += c;
    }
    if (backslashes) out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}
#else
// POSIX single-quote escaping: wrap in '...', breaking out any embedded
// single quote as '\'' (close quote, escaped literal quote, reopen quote).
std::string quoteArg(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}
#endif

ProcessResult runProcess(const std::vector<std::string>& argv, bool stream_output = false) {
    std::ostringstream cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd << ' ';
        cmd << quoteArg(argv[i]);
    }
    cmd << " 2>&1";

    ProcessResult result;
#ifdef _WIN32
    FILE* p = _popen(cmd.str().c_str(), "r");
#else
    FILE* p = popen(cmd.str().c_str(), "r");
#endif
    if (!p) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) {
        result.output += buf;
        if (stream_output) std::cout << buf;
    }
#ifdef _WIN32
    result.exit_code = _pclose(p);
#else
    result.exit_code = pclose(p);
#endif
    return result;
}

std::string readManifestField(const std::string& manifest_path, const std::string& key) {
    std::ifstream f(manifest_path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return "";
}

// regional_db_export records "table.<name>.format=binary|text" per table
// (raster columns -- terrain, wmm -- have no binary send/recv function, so
// those two are dumped as text). Defaults to binary if the manifest predates
// this field or doesn't mention the table.
std::string tableFormat(const std::string& region_dir, const std::string& table_name) {
    std::string v = readManifestField(region_dir + "/manifest.txt", "table." + table_name + ".format");
    return v.empty() ? "binary" : v;
}

// A parent table with a real PK, optionally feeding a "new ids" temp table
// that child groups filter against.
struct ParentGroup {
    std::string table;
    std::string pk_col;
    std::string new_ids_temp;  // name of the temp table of newly-added pk values this group creates
};

// A child (tag-like, no PK) table inserted only for parent rows that were
// new this run.
struct ChildGroup {
    std::string table;
    std::string new_ids_temp;   // which parent group's "new ids" temp table to join against
    std::string join_col;       // this table's column that references the parent's key
};

const std::vector<ParentGroup> kParents = {
    {"ways",       "id", "new_way_ids"},
    {"areas",      "id", "new_area_ids"},
    {"roads",      "id", "new_road_ids"},
    {"relations",  "id", "new_relation_ids"},
    {"nodes",      "id", "new_node_ids"},
    {"airports",   "id", "new_airport_ids"},
    {"navaids",    "id", ""},
    {"frequencies","id", ""},
    {"runways",    "id", ""},
    {"faa_obstacles",          "id", ""},
    {"class_airspace",         "id", ""},
    {"special_use_airspace",   "id", ""},
    {"international_airspace", "id", ""},
    {"wmm_bands",  "id",  ""},
    {"terrain",    "rid", ""},
    {"wmm",        "rid", ""},
    {"countries",  "id", ""},
    {"regions",    "id", ""},
};

const std::vector<ChildGroup> kChildren = {
    {"way_tags",      "new_way_ids",      "id"},
    {"area_tags",     "new_area_ids",     "id"},
    {"road_tags",     "new_road_ids",     "id"},
    {"relation_tags", "new_relation_ids", "id"},
    {"node_tags",     "new_node_ids",     "id"},
};

// airport_tags.bin holds public.tags rows (keyed by airport_ident, not an
// integer id) -- handled separately from kChildren since its join column
// isn't "id" and its dest table name ("tags") differs from its bundle
// filename ("airport_tags", chosen to avoid confusion with the OSM tag
// tables at export time).

// Appends staging + dedup SQL for one PK-bearing table. If bin file is
// missing (table had zero matching rows, or export failed and the manifest
// already warned about it), the group is skipped entirely.
void appendParentGroup(std::ostringstream& sql, const std::string& region_dir,
                       const ParentGroup& g) {
    std::string bin_path = region_dir + "/" + g.table + ".bin";
    if (!fileExists(bin_path)) return;

    std::string staging = "staging_" + g.table;
    // Bare columns only -- no indexes/constraints (in particular, no GIST
    // spatial index). Staging never needs its own index: the "new ids"
    // anti-join uses the real table's existing PK index regardless, and
    // building the real table's own GIST index once (on the final INSERT)
    // is unavoidable, but duplicating that same expensive incremental
    // index-maintenance work onto a throwaway staging table during \copy
    // was pure waste -- and, at multi-million-row scale, expensive enough
    // to have visibly hung the whole box.
    sql << "CREATE TEMP TABLE " << staging << " (LIKE public." << g.table << ") ON COMMIT DROP;\n";
    sql << "\\copy " << staging << " FROM " << copyFromClause(bin_path) << " WITH (FORMAT " << tableFormat(region_dir, g.table) << ")\n";
    if (!g.new_ids_temp.empty()) {
        sql << "CREATE TEMP TABLE " << g.new_ids_temp << " AS "
            << "SELECT s." << g.pk_col << " AS id"
            << (g.table == "airports" ? ", s.ident AS ident" : "")
            << " FROM " << staging << " s "
            << "LEFT JOIN public." << g.table << " t ON t." << g.pk_col << " = s." << g.pk_col << " "
            << "WHERE t." << g.pk_col << " IS NULL;\n";
    }
    sql << "INSERT INTO public." << g.table << " SELECT * FROM " << staging
        << " ON CONFLICT (" << g.pk_col << ") DO NOTHING;\n\n";
}

void appendChildGroup(std::ostringstream& sql, const std::string& region_dir,
                      const ChildGroup& g) {
    std::string bin_path = region_dir + "/" + g.table + ".bin";
    if (!fileExists(bin_path)) return;

    std::string staging = "staging_" + g.table;
    // Bare columns only -- no indexes/constraints (in particular, no GIST
    // spatial index). Staging never needs its own index: the "new ids"
    // anti-join uses the real table's existing PK index regardless, and
    // building the real table's own GIST index once (on the final INSERT)
    // is unavoidable, but duplicating that same expensive incremental
    // index-maintenance work onto a throwaway staging table during \copy
    // was pure waste -- and, at multi-million-row scale, expensive enough
    // to have visibly hung the whole box.
    sql << "CREATE TEMP TABLE " << staging << " (LIKE public." << g.table << ") ON COMMIT DROP;\n";
    sql << "\\copy " << staging << " FROM " << copyFromClause(bin_path) << " WITH (FORMAT " << tableFormat(region_dir, g.table) << ")\n";
    sql << "INSERT INTO public." << g.table << " "
        << "SELECT c.* FROM " << staging << " c "
        << "JOIN " << g.new_ids_temp << " n ON n.id = c." << g.join_col << ";\n\n";
}

// public.tags (airport metadata) -- keyed by airport_ident, filtered
// against new_airport_ids' ident column rather than an integer id.
void appendAirportTags(std::ostringstream& sql, const std::string& region_dir) {
    std::string bin_path = region_dir + "/airport_tags.bin";
    if (!fileExists(bin_path)) return;
    sql << "CREATE TEMP TABLE staging_airport_tags (LIKE public.tags) ON COMMIT DROP;\n";
    sql << "\\copy staging_airport_tags FROM " << copyFromClause(bin_path) << " WITH (FORMAT " << tableFormat(region_dir, "airport_tags") << ")\n";
    sql << "INSERT INTO public.tags "
           "SELECT c.* FROM staging_airport_tags c "
           "JOIN new_airport_ids n ON n.ident = c.airport_ident;\n\n";
}

// countries/regions are shipped whole in every bundle -- same ON CONFLICT
// DO NOTHING dedup as any other PK-bearing table (handled by appendParentGroup
// already, included in kParents).

bool runScript(const std::string& host, const std::string& user, const std::string& db,
              const std::string& script_path, bool verbose) {
    std::vector<std::string> argv = {"psql", "-h", host, "-U", user, "-d", db,
                                      "-v", "ON_ERROR_STOP=1", "-f", script_path};
    if (!verbose) argv.push_back("-q");
    ProcessResult r = runProcess(argv, verbose);
    if (r.exit_code != 0) {
        std::cerr << "[regional_install] install script failed:\n" << r.output << "\n";
        return false;
    }
    return true;
}

bool tableExists(pqxx::connection& conn, const std::string& table) {
    pqxx::work txn(conn);
    auto r = txn.exec(
        "SELECT EXISTS (SELECT 1 FROM information_schema.tables "
        "WHERE table_schema='public' AND table_name=$1)",
        pqxx::params{table});
    return r[0][0].as<bool>();
}


} // namespace

int main(int argc, char** argv) {
    std::string bundle_path, host, db, user, nodes_file;
    // Deliberately not a hardcoded "/tmp": on Windows that resolves to \tmp
    // on the current drive, which normally doesn't exist, so every run
    // without an explicit --work-dir would fail at extraction.
    // temp_directory_path() gives %TEMP% on Windows and $TMPDIR-or-/tmp on
    // POSIX. Note this makes the Linux default honour $TMPDIR where it
    // previously always used /tmp -- standard behaviour, but a change.
    std::string work_dir;
    {
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path(ec);
        work_dir = ec ? std::string(".") : tmp.string();
    }
    std::string regions_dir = "data/regions";
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) host = argv[++i];
        else if ((arg == "-d") && i+1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user = argv[++i];
        else if ((arg == "--nodes-file") && i+1 < argc) nodes_file = argv[++i];
        else if ((arg == "--work-dir") && i+1 < argc) work_dir = argv[++i];
        else if ((arg == "--regions-dir") && i+1 < argc) regions_dir = argv[++i];
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_install <bundle.gpsxdb.tar.gz> -s <host> -d <db> -u <user>\n"
                         "                         --nodes-file <target nodes.dat path>\n"
                         "                         [--work-dir <dir>, default " << work_dir << "]\n"
                         "                         [--regions-dir <dir>, default data/regions] [-v]\n"
                         "\n"
                         "--work-dir is where the bundle is extracted -- a multi-GB region bundle\n"
                         "can exceed the system temp dir's size or quota (notably a tmpfs /tmp on\n"
                         "Linux), so point this at plain disk (e.g. alongside nodes.dat) if that\n"
                         "happens.\n"
                         "\n"
                         "--regions-dir is a fallback only -- current bundles embed their own\n"
                         "<region>.wkt (used to register this region in public.installed_regions,\n"
                         "so a region-aware poll process can auto-detect which polygon(s) to\n"
                         "filter against without a --region flag); this is only consulted for\n"
                         "older bundles built before that was added.\n"
                         "\n"
                         "Idempotent: safe to install multiple (even overlapping) regions into\n"
                         "the same target -- see regional_install.cpp's top-of-file comment for\n"
                         "the dedup strategy.\n"
                         "\n"
    // libpq's password file lives in a different place on each platform, and
    // naming the wrong one sends people down a long debugging detour: libpq
    // silently ignores ~/.pgpass on Windows, so the failure surfaces as a
    // bare authentication error with nothing pointing at the file.
#ifdef _WIN32
                         "Authentication uses libpq's password file, which on Windows is\n"
                         "%APPDATA%\\postgresql\\pgpass.conf -- NOT ~/.pgpass, which libpq does\n"
                         "not read on this platform.\n";
#else
                         "Authentication uses libpq's password file: ~/.pgpass.\n";
#endif
            std::cout.flush();  // _exit() skips normal stdio cleanup -- must flush explicitly,
                                 // otherwise this is silently lost whenever stdout isn't line-
                                 // buffered (any pipe/redirect, not just a real terminal)
            _exit(0);  // avoid pqxx static-destructor double-free on normal return
        }
        else if (arg[0] != '-') bundle_path = arg;
    }

    if (bundle_path.empty() || host.empty() || db.empty() || user.empty() || nodes_file.empty()) {
        std::cerr << "Error: bundle path, -s, -d, -u, --nodes-file are all required\n";
        _exit(1);
    }
    if (!fileExists(bundle_path)) {
        std::cerr << "Error: bundle not found: " << bundle_path << "\n";
        _exit(1);
    }

    // Extract to a private temp dir under work_dir (the system temp dir by
    // default, but a multi-GB bundle can exceed its size or quota -- notably
    // a tmpfs /tmp on Linux -- see --work-dir above).
    {
        std::error_code ec;
        fs::create_directories(work_dir, ec);  // best-effort, same as before
    }
    std::string tmp_dir;
    try {
        tmp_dir = makeTempDir(work_dir);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        _exit(1);
    }

    if (verbose) std::cout << "[regional_install] extracting " << bundle_path << " to " << tmp_dir << "\n";
    if (runProcess({"tar", "xzf", bundle_path, "-C", tmp_dir}).exit_code != 0) {
        std::cerr << "Error: failed to extract " << bundle_path << "\n";
        _exit(1);
    }

    // regional_db_export writes one subdirectory per region under its
    // --out-dir; a single-region bundle's tar root should contain exactly
    // one such directory. Find it.
    std::string region_dir = findFileWithinTwoLevels(tmp_dir, "manifest.txt");
    if (region_dir.empty() || !dirExists(region_dir)) {
        std::cerr << "Error: could not find a region directory (manifest.txt) inside " << bundle_path << "\n";
        _exit(1);
    }

    std::string region_name = readManifestField(region_dir + "/manifest.txt", "region");
    std::cout << "[regional_install] installing region '" << region_name << "' from " << bundle_path << "\n";

    // ---- 1. Idempotent schema (everything with fixed DDL) ----
    {
        std::mutex dummy_mu;
        NavDB db_client(0, host, user, db, dummy_mu);
        db_client.ensureSchema();

        // GiST spatial indexes aren't needed for correctness during the
        // load (only the primary keys are, for INSERT ... ON CONFLICT (id)
        // dedup below -- those are never dropped). Dropping them first
        // (safe no-op on a fresh table, or one from a prior regional_install
        // run that already dropped them) and recreating in step 6 gives
        // every region's load the same efficient one-pass bulk index build
        // the bulk importer already gets, instead of incremental per-row
        // GiST maintenance on a second/third region installed into an
        // already-indexed table.
        db_client.dropGistIndexes();
    }

    // ---- 2. public.terrain's DDL (raster2pgsql-generated, not fixed) ----
    {
        pqxx::connection conn("host=" + host + " dbname=" + db + " user=" + user);
        bool has_terrain_rows = fileExists(region_dir + "/terrain.bin");
        bool terrain_table_exists = tableExists(conn, "terrain");
        std::string schema_path = region_dir + "/terrain.schema.sql";
        if (has_terrain_rows && !terrain_table_exists) {
            if (!fileExists(schema_path)) {
                std::cerr << "[regional_install] WARNING: terrain.bin present but no captured "
                             "terrain.schema.sql and public.terrain doesn't exist on target -- "
                             "skipping terrain import for this region\n";
            } else {
                std::vector<std::string> argv = {"psql", "-h", host, "-U", user, "-d", db,
                                                  "-v", "ON_ERROR_STOP=1", "-f", schema_path};
                if (runProcess(argv).exit_code != 0) {
                    std::cerr << "[regional_install] WARNING: applying terrain.schema.sql failed -- "
                                 "skipping terrain import for this region\n";
                }
            }
        }
    }

    // ---- 3. Build and run the staged-load script ----
    std::string script_path = tmp_dir + "/install.sql";
    {
        std::ostringstream sql;
        sql << "BEGIN;\n\n";
        for (auto& g : kParents) appendParentGroup(sql, region_dir, g);
        for (auto& g : kChildren) appendChildGroup(sql, region_dir, g);
        appendAirportTags(sql, region_dir);
        sql << "COMMIT;\n";

        std::ofstream out(script_path);
        out << sql.str();
    }
    if (!runScript(host, user, db, script_path, verbose)) {
        std::cerr << "[regional_install] DB load failed -- nodes.dat NOT modified, "
                     "temp files left at " << tmp_dir << " for inspection\n";
        _exit(1);
    }
    std::cout << "[regional_install] DB tables installed\n";

    // ---- 4. Recreate GiST spatial indexes (see step 1's drop) ----
    {
        std::mutex dummy_mu;
        NavDB db_client(0, host, user, db, dummy_mu);
        db_client.createGistIndexes();
    }
    std::cout << "[regional_install] GiST indexes rebuilt\n";

    // ---- 5. Merge/install the region's node coordinates ----
    std::string region_nodes_path = region_dir + "/" + region_name + ".nodes.dat";
    if (!fileExists(region_nodes_path)) {
        std::cerr << "[regional_install] WARNING: no " << region_name
                  << ".nodes.dat in bundle -- node coordinate store not updated\n";
    } else if (!fileExists(nodes_file)) {
        std::error_code ec;
        fs::copy_file(region_nodes_path, nodes_file, ec);
        if (ec) {
            std::cerr << "[regional_install] ERROR: failed to install initial " << nodes_file << "\n";
            _exit(1);
        }
        std::cout << "[regional_install] installed initial " << nodes_file << "\n";
    } else {
        std::string merged_path = nodes_file + ".merging";
        if (!RegionalNodeMap::merge(nodes_file, region_nodes_path, merged_path)) {
            std::cerr << "[regional_install] ERROR: nodes.dat merge failed\n";
            _exit(1);
        }
        // std::filesystem::rename (unlike raw POSIX rename()/Windows
        // rename()) is specified with atomic replace-if-exists semantics on
        // every platform -- Windows' own rename() fails outright if the
        // destination already exists, which this call always does here.
        std::error_code ec;
        fs::rename(merged_path, nodes_file, ec);
        if (ec) {
            std::cerr << "[regional_install] ERROR: failed to replace " << nodes_file << " with merged result "
                      << "(merged file left at " << merged_path << ")\n";
            _exit(1);
        }
        std::cout << "[regional_install] merged region nodes into " << nodes_file << "\n";
    }

    // ---- 6. Register in public.installed_regions ----
    // Prefer the bundle's own embedded <region>.wkt (self-contained, no
    // dependency on this machine's data/regions/ matching the bundle's
    // source); fall back to --regions-dir for bundles built before this
    // was added (see --help).
    {
        std::string wkt_path = region_dir + "/" + region_name + ".wkt";
        if (!fileExists(wkt_path)) wkt_path = regions_dir + "/" + region_name + ".wkt";

        std::string bbox = readManifestField(region_dir + "/manifest.txt", "bbox");
        std::ifstream wkt_in(wkt_path);
        if (!fileExists(wkt_path) || bbox.empty()) {
            std::cerr << "[regional_install] WARNING: could not find " << region_name
                      << ".wkt (checked bundle and --regions-dir " << regions_dir
                      << ") or manifest bbox -- skipping installed_regions registration; "
                         "a region-aware poll process won't auto-detect this region\n";
        } else {
            std::stringstream wkt_ss;
            wkt_ss << wkt_in.rdbuf();
            double min_lon, min_lat, max_lon, max_lat;
            std::stringstream bbox_ss(bbox);
            char comma;
            bbox_ss >> min_lon >> comma >> min_lat >> comma >> max_lon >> comma >> max_lat;

            std::mutex dummy_mu;
            NavDB db_client(0, host, user, db, dummy_mu);
            db_client.registerInstalledRegion(region_name, wkt_ss.str(), min_lon, min_lat, max_lon, max_lat);
            std::cout << "[regional_install] registered '" << region_name << "' in installed_regions\n";
        }
    }

    {
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);  // best-effort cleanup, matches original's un-checked call
    }

    std::cout << "[regional_install] done -- region '" << region_name << "' installed\n";
    std::cout.flush();
    _exit(0);  // avoid pqxx static-destructor double-free on normal return
}
