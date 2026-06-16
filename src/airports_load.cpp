/**
 * airports_load.cpp — standalone OurAirports loader for testing.
 * Usage: ./airports_load -s <host> -d <db> -u <user> [-p <password>]
 */
#include "AirportsLoader.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string server, database, user, password;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if ((arg == "-p") && i+1 < argc) password = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: airports_load -s <host> -d <db> -u <user> [-p <pass>]\n";
            return 0;
        }
    }

    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; return 1;
    }

    loadAirportsData(server, user, database, password);
    return 0;
}
