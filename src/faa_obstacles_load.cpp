// Standalone FAA Digital Obstacle File loader.
// Usage: faa_obstacles_load -s <host> -d <db> -u <user> [-4]
// Requires ~/.pgpass for authentication (no -p/password flag).
#include "FAAObstacleLoader.h"
#include "GeoUtils.h"
#include <iostream>
#include <string>
#include <unistd.h>

// Defined here for the standalone binary (osm_import defines it in main.cpp)
int g_srid = 3857;

int main(int argc, char** argv) {
    std::string server, database, user;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if  (arg == "-4")                g_srid   = 4326;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: faa_obstacles_load -s <host> -d <db> -u <user> "
                         "[-4 (WGS84 instead of Mercator)]\n"
                         "Requires ~/.pgpass for authentication.\n";
            std::cout.flush();
            _exit(0);
        }
    }
    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; return 1;
    }
    bool ok = FAAObstacleLoader(server, user, database).load(true);
    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
