// Standalone World Magnetic Model (declination) loader.
// Usage: wmm_load -s <host> -d <db> -u <user> [-p <password>]
//                  [--bbox minlon,minlat,maxlon,maxlat] [-4]
//                  [--year <decimal year>]
//                  [--grid-deg <deg>] [--band-deg <deg>] [--no-bands]
//                  [--simplify-m <meters>] [--threads <n>]
#include "WMMLoader.h"
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <unistd.h>

namespace {
double currentDecimalYear() {
    std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    int year = tm_utc.tm_year + 1900;
    return year + tm_utc.tm_yday / 365.25;
}
} // namespace

int main(int argc, char** argv) {
    std::string server, database, user, password;
    double min_lon = -180, min_lat = -90, max_lon = 180, max_lat = 90;
    int dest_srid = 3857;
    double year = currentDecimalYear();
    double grid_deg = 0.25;
    double band_deg = 1.0;
    double simplify_m = 50.0;
    int threads = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if ((arg == "-p") && i+1 < argc) password = argv[++i];
        else if  (arg == "-4")                dest_srid = 4326;
        else if  (arg == "--no-bands")        band_deg = 0;
        else if ((arg == "--year") && i+1 < argc) year = std::stod(argv[++i]);
        else if ((arg == "--grid-deg") && i+1 < argc) grid_deg = std::stod(argv[++i]);
        else if ((arg == "--band-deg") && i+1 < argc) band_deg = std::stod(argv[++i]);
        else if ((arg == "--simplify-m") && i+1 < argc) simplify_m = std::stod(argv[++i]);
        else if ((arg == "--threads") && i+1 < argc) threads = std::stoi(argv[++i]);
        else if ((arg == "--bbox") && i+1 < argc) {
            std::string v = argv[++i];
            std::replace(v.begin(), v.end(), ',', ' ');
            std::istringstream iss(v);
            if (!(iss >> min_lon >> min_lat >> max_lon >> max_lat)) {
                std::cerr << "Error: --bbox must be minlon,minlat,maxlon,maxlat\n";
                return 1;
            }
        }
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: wmm_load -s <host> -d <db> -u <user> [-p <pass>]\n"
                         "                 [--bbox minlon,minlat,maxlon,maxlat, default whole globe]\n"
                         "                 [-4 (WGS84 instead of Mercator for wmm_bands)]\n"
                         "                 [--year <decimal year>, default: today]\n"
                         "                 [--grid-deg <deg>, default 0.25]\n"
                         "                 [--band-deg <deg>, default 1.0] [--no-bands]\n"
                         "                 [--simplify-m <meters>, default 50, 0 to disable]\n"
                         "                 [--threads <n>, default 4]\n";
            std::cout.flush();
            _exit(0);
        }
    }

    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n";
        return 1;
    }

    bool ok = loadWMM(server, user, database, password, year,
                      min_lon, min_lat, max_lon, max_lat,
                      grid_deg, dest_srid, band_deg, simplify_m, threads, true);
    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
