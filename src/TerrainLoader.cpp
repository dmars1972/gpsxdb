#include "TerrainLoader.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <curl/curl.h>

namespace {

size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* f = static_cast<std::ofstream*>(stream);
    f->write(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

bool downloadFile(const std::string& url, const std::string& dest) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::ofstream f(dest, std::ios::binary);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // treat HTTP 4xx/5xx as failure
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    f.close();
    return res == CURLE_OK;
}

struct Tile { int lat; int lon; std::string name; };

// 3DEP tiles are named by their NW corner: "n40w105" covers lat [39,40] x
// lon [-105,-104] — the "n" value is the tile's NORTH edge, but the "w"
// value is the tile's WEST edge (confirmed empirically: n40w105 contains
// Denver at 39.7N/-105.0W). So the row loop variable (a tile's south edge,
// i.e. floor(lat)) needs +1 to become the naming convention's north edge;
// the column loop variable (a tile's west edge, i.e. floor(lon)) is used
// as-is. US-only coverage, so always north/west.
std::vector<Tile> tilesForBBox(double min_lon, double min_lat,
                               double max_lon, double max_lat) {
    std::vector<Tile> tiles;
    int lat0 = static_cast<int>(std::floor(min_lat));
    int lat1 = static_cast<int>(std::ceil(max_lat));
    int lon0 = static_cast<int>(std::floor(min_lon));
    int lon1 = static_cast<int>(std::ceil(max_lon));
    for (int lat = lat0; lat < lat1; ++lat) {
        for (int lon = lon0; lon < lon1; ++lon) {
            char buf[32];
            snprintf(buf, sizeof(buf), "n%02dw%03d", lat + 1, -lon);
            tiles.push_back({lat, lon, buf});
        }
    }
    return tiles;
}

} // namespace

bool loadTerrain(const std::string& server, const std::string& user,
                 const std::string& database, const std::string& password,
                 double min_lon, double min_lat, double max_lon, double max_lat,
                 int dest_srid, bool verbose) {
    auto tiles = tilesForBBox(min_lon, min_lat, max_lon, max_lat);
    if (tiles.empty()) {
        std::cerr << "[Terrain] bounding box produced no tiles\n";
        return false;
    }

    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;

    pqxx::connection conn(connstr);
    {
        pqxx::work txn(conn);
        txn.exec("CREATE EXTENSION IF NOT EXISTS postgis_raster");
        txn.exec(
            "CREATE TABLE IF NOT EXISTS terrain_tiles ("
            "  tile_name text PRIMARY KEY,"
            "  loaded_at timestamptz NOT NULL DEFAULT now())");
        txn.commit();
    }

    bool table_exists;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT EXISTS (SELECT 1 FROM information_schema.tables "
            "WHERE table_schema='public' AND table_name='terrain')");
        txn.commit();
        table_exists = r[0][0].as<bool>();
    }

    std::vector<Tile> to_load;
    for (auto& t : tiles) {
        pqxx::work txn(conn);
        auto r = txn.exec("SELECT 1 FROM terrain_tiles WHERE tile_name=$1",
                          pqxx::params{t.name});
        txn.commit();
        if (r.empty()) to_load.push_back(t);
        else if (verbose) std::cout << "  " << t.name << " already loaded, skipping\n";
    }

    if (to_load.empty()) {
        if (verbose) std::cout << "All requested tiles already loaded.\n";
        return true;
    }

    const std::string tmp_dir = "/tmp/terrain_tiles";
    system(("mkdir -p " + tmp_dir).c_str());

    std::vector<std::string> downloaded_paths;
    std::vector<Tile> downloaded_tiles;
    for (auto& t : to_load) {
        std::string url = "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1/TIFF/current/"
                          + t.name + "/USGS_1_" + t.name + ".tif";
        std::string dest = tmp_dir + "/" + t.name + ".tif";
        if (verbose) { std::cout << "  downloading " << t.name << "... "; std::cout.flush(); }
        if (!downloadFile(url, dest)) {
            // Not fatal — a requested bbox may extend past 3DEP's coverage
            // (ocean, non-US territory), so a missing tile is expected, not
            // an error condition worth aborting the whole load over.
            if (verbose) std::cout << "not available, skipping\n";
            std::remove(dest.c_str());
            continue;
        }
        if (verbose) std::cout << "OK\n";
        downloaded_paths.push_back(dest);
        downloaded_tiles.push_back(t);
    }

    if (downloaded_paths.empty()) {
        std::cerr << "[Terrain] no tiles in this bounding box are available from 3DEP\n";
        return false;
    }

    // Deliberately no -C (standard constraints): reprojecting tiles spanning
    // a range of latitudes into Mercator gives each tile a slightly
    // different pixel scale, and 256x256 tiling gives edge blocks
    // non-uniform width/height. Fixed-value CHECK constraints derived from
    // one load batch then reject any later append whose tiles happen to
    // have different dimensions/scale — not needed for ST_Value point
    // queries, which only rely on the GIST index from -I.
    std::ostringstream cmd;
    cmd << "raster2pgsql -s 4269:" << dest_srid << " -t 256x256 -F";
    cmd << (table_exists ? " -a" : " -I -M");
    for (auto& p : downloaded_paths) cmd << " " << p;
    cmd << " terrain | psql -h " << server << " -U " << user << " -d " << database
        << " -q -v ON_ERROR_STOP=1";

    if (verbose)
        std::cout << "Loading " << downloaded_paths.size()
                  << " tile(s) into terrain table...\n";

    if (!password.empty()) setenv("PGPASSWORD", password.c_str(), 1);
    int rc = system(cmd.str().c_str());
    if (!password.empty()) unsetenv("PGPASSWORD");

    for (auto& p : downloaded_paths) std::remove(p.c_str());

    if (rc != 0) {
        std::cerr << "[Terrain] raster2pgsql/psql load failed\n";
        return false;
    }

    {
        pqxx::work txn(conn);
        for (auto& t : downloaded_tiles)
            txn.exec("INSERT INTO terrain_tiles(tile_name) VALUES ($1) "
                     "ON CONFLICT DO NOTHING", pqxx::params{t.name});
        txn.commit();
    }

    if (verbose)
        std::cout << "Terrain data loaded (" << downloaded_tiles.size()
                  << " new tile(s)).\n";
    return true;
}
