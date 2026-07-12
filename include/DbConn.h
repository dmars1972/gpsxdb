#pragma once
#include <string>

// Builds a libpq connection string from the pieces every loader (and NavDB)
// takes on its CLI (-s/-d/-u). Centralizes what was previously
// hand-duplicated across 10 call sites with divergent variants (some
// included sslmode=disable, some didn't). Deliberately never includes a
// password= clause -- auth relies on ~/.pgpass (or peer/trust), not a
// password passed on the command line or embedded in the connection
// string.
inline std::string makeConnString(const std::string& host, const std::string& database,
                                   const std::string& user) {
    return "host=" + host + " dbname=" + database + " user=" + user + " sslmode=disable";
}
