// Standalone airspace loader (FAA Class/Special Use Airspace + OpenAIP
// international airspace).
// Usage: airspace_load -s <host> -d <db> -u <user> [-p <password>] [-4]
//                       [--class-only] [--sua-only] [--intl-only]
//                       [--no-intl] [--api-key <key>]
#include "AirspaceLoader.h"
#include "GeoUtils.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>

// Defined here for the standalone binary (osm_import defines it in main.cpp)
int g_srid = 3857;

namespace {
// Falls back to ~/.openaip_api_key (mirroring how DB credentials can fall
// back to ~/.pgpass) so the key doesn't need to be typed/scripted on every
// invocation, and never needs to live in the repo.
std::string defaultApiKeyPath() {
    const char* home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    return std::string(home) + "/.openaip_api_key";
}

std::string readApiKeyFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string key;
    std::getline(f, key);
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' '))
        key.pop_back();
    return key;
}
} // namespace

int main(int argc, char** argv) {
    std::string server, database, user, password, api_key;
    bool do_class = true, do_sua = true, do_intl = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i+1 < argc) server   = argv[++i];
        else if ((arg == "-d") && i+1 < argc) database = argv[++i];
        else if ((arg == "-u") && i+1 < argc) user     = argv[++i];
        else if ((arg == "-p") && i+1 < argc) password = argv[++i];
        else if  (arg == "-4")                g_srid   = 4326;
        else if  (arg == "--class-only")      { do_sua = false; do_intl = false; }
        else if  (arg == "--sua-only")        { do_class = false; do_intl = false; }
        else if  (arg == "--intl-only")       { do_class = false; do_sua = false; }
        else if  (arg == "--no-intl")         do_intl  = false;
        else if ((arg == "--api-key") && i+1 < argc) api_key = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: airspace_load -s <host> -d <db> -u <user> [-p <pass>]\n"
                         "                      [-4 (WGS84 instead of Mercator)]\n"
                         "                      [--class-only | --sua-only | --intl-only]\n"
                         "                      [--no-intl (skip OpenAIP international airspace)]\n"
                         "                      [--api-key <key> (default: ~/.openaip_api_key)]\n";
            std::cout.flush();
            _exit(0);
        }
    }
    if (server.empty() || database.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; return 1;
    }
    if (do_intl && api_key.empty()) api_key = readApiKeyFile(defaultApiKeyPath());
    if (do_intl && api_key.empty()) {
        std::cerr << "Warning: no OpenAIP API key (--api-key or ~/.openaip_api_key) — "
                     "skipping international airspace\n";
        do_intl = false;
    }

    bool ok = true;
    if (do_class) ok = loadClassAirspace(server, user, database, password, true) && ok;
    if (do_sua)   ok = loadSpecialUseAirspace(server, user, database, password, true) && ok;
    if (do_intl)  ok = loadInternationalAirspace(server, user, database, password, api_key, true) && ok;

    std::cout.flush();
    // _exit skips static destructor teardown that causes double-free with PROJ/pqxx
    _exit(ok ? 0 : 1);
}
