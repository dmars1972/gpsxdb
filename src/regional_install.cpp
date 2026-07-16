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
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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
    sql << "CREATE TEMP TABLE " << staging << " (LIKE public." << g.table << " INCLUDING ALL) ON COMMIT DROP;\n";
    sql << "\\copy " << staging << " FROM '" << bin_path << "' WITH (FORMAT binary)\n";
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
    sql << "CREATE TEMP TABLE " << staging << " (LIKE public." << g.table << " INCLUDING ALL) ON COMMIT DROP;\n";
    sql << "\\copy " << staging << " FROM '" << bin_path << "' WITH (FORMAT binary)\n";
    sql << "INSERT INTO public." << g.table << " "
        << "SELECT c.* FROM " << staging << " c "
        << "JOIN " << g.new_ids_temp << " n ON n.id = c." << g.join_col << ";\n\n";
}

// public.tags (airport metadata) -- keyed by airport_ident, filtered
// against new_airport_ids' ident column rather than an integer id.
void appendAirportTags(std::ostringstream& sql, const std::string& region_dir) {
    std::string bin_path = region_dir + "/airport_tags.bin";
    if (!fileExists(bin_path)) return;
    sql << "CREATE TEMP TABLE staging_airport_tags (LIKE public.tags INCLUDING ALL) ON COMMIT DROP;\n";
    sql << "\\copy staging_airport_tags FROM '" << bin_path << "' WITH (FORMAT binary)\n";
    sql << "INSERT INTO public.tags "
           "SELECT c.* FROM staging_airport_tags c "
           "JOIN new_airport_ids n ON n.ident = c.airport_ident;\n\n";
}

// countries/regions are shipped whole in every bundle -- same ON CONFLICT
// DO NOTHING dedup as any other PK-bearing table (handled by appendParentGroup
// already, included in kParents).

bool runScript(const std::string& host, const std::string& user, const std::string& db,
              const std::string& script_path, bool verbose) {
    std::ostringstream cmd;
    cmd << "psql -h " << host << " -U " << user << " -d " << db
        << " -v ON_ERROR_STOP=1 -f '" << script_path << "'";
    if (!verbose) cmd << " -q";
    cmd << " 2>&1";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return false;
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) { output += buf; if (verbose) std::cout << buf; }
    int rc = pclose(p);
    if (rc != 0) {
        std::cerr << "[regional_install] install script failed:\n" << output << "\n";
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

std::string readManifestField(const std::string& manifest_path, const std::string& key) {
    std::ifstream f(manifest_path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return "";
}

} // namespace

int main(int argc, char** argv) {
    std::string bundle_path, host, db, user, nodes_file;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) host = argv[++i];
        else if ((arg == "-d") && i+1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user = argv[++i];
        else if ((arg == "--nodes-file") && i+1 < argc) nodes_file = argv[++i];
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_install <bundle.gpsxdb.tar.gz> -s <host> -d <db> -u <user>\n"
                         "                         --nodes-file <target nodes.dat path> [-v]\n"
                         "\n"
                         "Idempotent: safe to install multiple (even overlapping) regions into\n"
                         "the same target -- see regional_install.cpp's top-of-file comment for\n"
                         "the dedup strategy. Requires ~/.pgpass for authentication.\n";
            return 0;
        }
        else if (arg[0] != '-') bundle_path = arg;
    }

    if (bundle_path.empty() || host.empty() || db.empty() || user.empty() || nodes_file.empty()) {
        std::cerr << "Error: bundle path, -s, -d, -u, --nodes-file are all required\n";
        return 1;
    }
    if (!fileExists(bundle_path)) {
        std::cerr << "Error: bundle not found: " << bundle_path << "\n";
        return 1;
    }

    // Extract to a private temp dir.
    char tmpl[] = "/tmp/regional_install.XXXXXX";
    char* tmp_dir_c = mkdtemp(tmpl);
    if (!tmp_dir_c) {
        std::cerr << "Error: mkdtemp failed\n";
        return 1;
    }
    std::string tmp_dir = tmp_dir_c;

    if (verbose) std::cout << "[regional_install] extracting " << bundle_path << " to " << tmp_dir << "\n";
    std::string extract_cmd = "tar xzf '" + bundle_path + "' -C '" + tmp_dir + "'";
    if (system(extract_cmd.c_str()) != 0) {
        std::cerr << "Error: failed to extract " << bundle_path << "\n";
        return 1;
    }

    // regional_db_export writes one subdirectory per region under its
    // --out-dir; a single-region bundle's tar root should contain exactly
    // one such directory. Find it.
    std::string region_dir;
    {
        std::string find_cmd = "find '" + tmp_dir + "' -mindepth 1 -maxdepth 2 -name manifest.txt";
        FILE* p = popen(find_cmd.c_str(), "r");
        char buf[4096];
        if (p && fgets(buf, sizeof(buf), p)) {
            std::string manifest_path = buf;
            if (!manifest_path.empty() && manifest_path.back() == '\n') manifest_path.pop_back();
            region_dir = manifest_path.substr(0, manifest_path.find_last_of('/'));
        }
        if (p) pclose(p);
    }
    if (region_dir.empty() || !dirExists(region_dir)) {
        std::cerr << "Error: could not find a region directory (manifest.txt) inside " << bundle_path << "\n";
        return 1;
    }

    std::string region_name = readManifestField(region_dir + "/manifest.txt", "region");
    std::cout << "[regional_install] installing region '" << region_name << "' from " << bundle_path << "\n";

    // ---- 1. Idempotent schema (everything with fixed DDL) ----
    {
        std::mutex dummy_mu;
        NavDB db_client(0, host, user, db, dummy_mu);
        db_client.ensureSchema();
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
                std::string cmd = "psql -h " + host + " -U " + user + " -d " + db + " -v ON_ERROR_STOP=1 -f '" + schema_path + "'";
                if (system(cmd.c_str()) != 0) {
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
        return 1;
    }
    std::cout << "[regional_install] DB tables installed\n";

    // ---- 4. Merge/install the region's node coordinates ----
    std::string region_nodes_path = region_dir + "/" + region_name + ".nodes.dat";
    if (!fileExists(region_nodes_path)) {
        std::cerr << "[regional_install] WARNING: no " << region_name
                  << ".nodes.dat in bundle -- node coordinate store not updated\n";
    } else if (!fileExists(nodes_file)) {
        std::string cp_cmd = "cp '" + region_nodes_path + "' '" + nodes_file + "'";
        if (system(cp_cmd.c_str()) != 0) {
            std::cerr << "[regional_install] ERROR: failed to install initial " << nodes_file << "\n";
            return 1;
        }
        std::cout << "[regional_install] installed initial " << nodes_file << "\n";
    } else {
        std::string merged_path = nodes_file + ".merging";
        if (!RegionalNodeMap::merge(nodes_file, region_nodes_path, merged_path)) {
            std::cerr << "[regional_install] ERROR: nodes.dat merge failed\n";
            return 1;
        }
        if (rename(merged_path.c_str(), nodes_file.c_str()) != 0) {
            std::cerr << "[regional_install] ERROR: failed to replace " << nodes_file << " with merged result "
                      << "(merged file left at " << merged_path << ")\n";
            return 1;
        }
        std::cout << "[regional_install] merged region nodes into " << nodes_file << "\n";
    }

    std::string cleanup_cmd = "rm -rf '" + tmp_dir + "'";
    system(cleanup_cmd.c_str());

    std::cout << "[regional_install] done -- region '" << region_name << "' installed\n";
    return 0;
}
