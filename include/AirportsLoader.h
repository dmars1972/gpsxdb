#pragma once
#include <string>

// Downloads and loads all OurAirports data into the database.
// Creates ap_countries, ap_regions, ap_airports, ap_airport_tags,
// ap_frequencies, ap_runways, ap_navaids, ap_navaid_tags.
// Schema must already exist (run create_airports.sql first).
//
// verbose: when true, prints download/load progress to stdout (used by the
// standalone airports_load tool). When false, all progress output is
// suppressed — used by osm_import, where this output would otherwise
// interleave with and corrupt the live status line.
void loadAirportsData(const std::string& server,
                      const std::string& user,
                      const std::string& database,
                      const std::string& password = "",
                      bool verbose = true);
