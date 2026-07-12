/**
 * airports_load.cpp — standalone OurAirports loader for testing.
 * Usage: ./airports_load -s <host> -d <db> -u <user>
 * Requires ~/.pgpass for authentication (no -p/password flag).
 */
#include "AirportsLoader.h"
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    std::string server, database, user;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: airports_load -s <host> -d <db> -u <user>\n"
                         "Requires ~/.pgpass for authentication.\n";
            std::cout.flush();
            _exit(0);
        }
    }

    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; return 1;
    }

    bool ok = AirportsLoader(server, user, database).load();
    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
