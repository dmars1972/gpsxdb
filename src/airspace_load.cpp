// Standalone airspace loader (FAA Class/Special Use Airspace + OpenAIP
// international airspace).
// Usage: airspace_load -s <host> -d <db> -u <user> [-4]
//                       [--class-only] [--sua-only] [--intl-only]
//                       [--no-intl] [--api-key <key>]
// Requires ~/.pgpass for authentication (no -p/password flag).
#include "AirspaceLoader.h"
#include "GeoUtils.h"
#include <iostream>
#include <string>
#include <unistd.h>

// Defined here for the standalone binary (osm_import defines it in main.cpp)
int g_srid = 3857;

int main(int argc, char** argv) {
    std::string server, database, user, api_key;
    bool do_class = true, do_sua = true, do_intl = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if  (arg == "-4")                g_srid   = 4326;
        else if  (arg == "--class-only")      { do_sua = false; do_intl = false; }
        else if  (arg == "--sua-only")        { do_class = false; do_intl = false; }
        else if  (arg == "--intl-only")       { do_class = false; do_sua = false; }
        else if  (arg == "--no-intl")         do_intl  = false;
        else if ((arg == "--api-key") && i+1 < argc) api_key = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: airspace_load -s <host> -d <db> -u <user>\n"
                         "                      [-4 (WGS84 instead of Mercator)]\n"
                         "                      [--class-only | --sua-only | --intl-only]\n"
                         "                      [--no-intl (skip OpenAIP international airspace)]\n"
                         "                      [--api-key <key> (default: ~/.openaip_api_key)]\n"
                         "Requires ~/.pgpass for authentication.\n";
            std::cout.flush();
            _exit(0);
        }
    }
    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; return 1;
    }
    if (do_intl && api_key.empty()) api_key = defaultOpenAipApiKey();
    if (do_intl && api_key.empty()) {
        std::cerr << "Warning: no OpenAIP API key (--api-key or ~/.openaip_api_key) — "
                     "skipping international airspace\n";
        do_intl = false;
    }

    AirspaceLoader loader(server, user, database);
    bool ok = true;
    if (do_class) ok = loader.loadClassAirspace(true) && ok;
    if (do_sua)   ok = loader.loadSpecialUseAirspace(true) && ok;
    if (do_intl)  ok = loader.loadInternationalAirspace(api_key, true) && ok;

    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
