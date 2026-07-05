// Standalone terrain elevation loader (USGS 3DEP or Copernicus DEM GLO-30).
// Usage: terrain_load -s <host> -d <db> -u <user> [-p <password>]
//                      --bbox minlon,minlat,maxlon,maxlat [-4]
//                      [--source 3dep|copernicus]
//                      [--band-ft <feet>] [--no-bands] [--simplify-m <meters>]
//                      [--threads <n>]
#include "TerrainLoader.h"
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <unistd.h>

int main(int argc, char** argv) {
    std::string server, database, user, password;
    double min_lon = 0, min_lat = 0, max_lon = 0, max_lat = 0;
    bool have_bbox = false;
    TerrainSource source = TerrainSource::USGS3DEP;
    int dest_srid = 3857;
    int band_ft = 500;
    double simplify_m = 50.0;
    int threads = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if ((arg == "-p") && i+1 < argc) password = argv[++i];
        else if  (arg == "-4")                dest_srid = 4326;
        else if  (arg == "--no-bands")        band_ft = 0;
        else if ((arg == "--band-ft") && i+1 < argc) band_ft = std::stoi(argv[++i]);
        else if ((arg == "--simplify-m") && i+1 < argc) simplify_m = std::stod(argv[++i]);
        else if ((arg == "--threads") && i+1 < argc) threads = std::stoi(argv[++i]);
        else if ((arg == "--source") && i+1 < argc) {
            std::string v = argv[++i];
            if (v == "3dep") source = TerrainSource::USGS3DEP;
            else if (v == "copernicus") source = TerrainSource::CopernicusGLO30;
            else { std::cerr << "Error: --source must be '3dep' or 'copernicus'\n"; return 1; }
        }
        else if ((arg == "--bbox") && i+1 < argc) {
            std::string v = argv[++i];
            std::replace(v.begin(), v.end(), ',', ' ');
            std::istringstream iss(v);
            if (!(iss >> min_lon >> min_lat >> max_lon >> max_lat)) {
                std::cerr << "Error: --bbox must be minlon,minlat,maxlon,maxlat\n";
                return 1;
            }
            have_bbox = true;
        }
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: terrain_load -s <host> -d <db> -u <user> [-p <pass>]\n"
                         "                     --bbox minlon,minlat,maxlon,maxlat [-4 (WGS84 instead of Mercator)]\n"
                         "                     [--source 3dep|copernicus, default 3dep (US-only, higher accuracy);\n"
                         "                       copernicus is near-global but overlaps 3dep in the US if used there]\n"
                         "                     [--band-ft <feet>, default 500] [--no-bands]\n"
                         "                     [--simplify-m <meters>, default 50, 0 to disable]\n"
                         "                     [--threads <n>, default 4]\n";
            std::cout.flush();
            _exit(0);
        }
    }

    if (server.empty() || database.empty() || user.empty() || !have_bbox) {
        std::cerr << "Error: -s, -d, -u, --bbox are required\n";
        return 1;
    }

    bool ok = loadTerrain(server, user, database, password,
                          min_lon, min_lat, max_lon, max_lat,
                          source, dest_srid, band_ft, simplify_m, threads, true);
    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
