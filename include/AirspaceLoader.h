#pragma once
#include <string>

// Downloads and loads FAA Class Airspace boundaries (Class A/B/C/D/E/G
// surface areas, Mode-C veils, etc.) into the class_airspace table.
//
// Data source: FAA Aeronautical Information Services' public ArcGIS open
// data portal (adds-faa.opendata.arcgis.com), GeoJSON download, no API key
// needed. US + territories only (Puerto Rico, Virgin Islands) — matches
// FAA's own coverage, same as faa_obstacles. Published on an ~8-week cycle
// alongside the NASR subscription.
//
// verbose: when false, suppresses progress output (used by osm_import;
//          the standalone airspace_load tool passes true).
//
// Returns false if the download failed (existing table contents are left
// untouched in that case).
bool loadClassAirspace(const std::string& server,
                       const std::string& user,
                       const std::string& database,
                       const std::string& password = "",
                       bool verbose = true);

// Downloads and loads FAA Special Use Airspace (Military Operations Areas,
// Restricted, Warning, Alert, and Prohibited areas) into the
// special_use_airspace table. Same source/coverage/cadence as
// loadClassAirspace above, separate FAA dataset with a different attribute
// schema (e.g. times-of-use instead of a CLASS A-G column), hence the
// separate table.
bool loadSpecialUseAirspace(const std::string& server,
                            const std::string& user,
                            const std::string& database,
                            const std::string& password = "",
                            bool verbose = true);

// Downloads and loads global (non-US) airspace from OpenAIP
// (openaip.net/api.core.openaip.net) into the international_airspace table
// — crowd-sourced, CC BY-NC 4.0 licensed (noncommercial use only), requires
// a free API key (see https://www.openaip.net/). US is deliberately
// excluded: FAA's own class_airspace/special_use_airspace data is the more
// authoritative source there, same "authoritative-source-stays, other
// source fills the gap" split as terrain's 3DEP (US) / Copernicus
// (elsewhere).
//
// OpenAIP's numeric `type`/`icao_class`/altitude-unit/altitude-reference
// codes are stored as-is (raw integers) rather than decoded to names —
// their meaning is only documented in OpenAIP's JS-rendered API docs
// (docs.openaip.net), which couldn't be fetched programmatically to build
// a verified mapping; guessing at these for safety-relevant airspace
// classification data would be worse than leaving them raw. Consult
// OpenAIP's docs directly if you need the decode table.
//
// api_key: your OpenAIP API key (see openaip.net account settings).
bool loadInternationalAirspace(const std::string& server,
                               const std::string& user,
                               const std::string& database,
                               const std::string& password,
                               const std::string& api_key,
                               bool verbose = true);
