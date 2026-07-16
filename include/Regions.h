#pragma once

// Continent-style bounding boxes used by both TerrainLoader (global
// Copernicus DEM coverage) and the regional export tooling — kept in one
// place so the two never drift apart.

struct GlobalRegion {
    const char* name;
    double min_lon, min_lat, max_lon, max_lat;
};

constexpr GlobalRegion kGlobalRegions[] = {
    // Continental US — terrain uses 3DEP for this (separate, higher-accuracy
    // load call), so TerrainLoader::loadGlobal() skips this entry by name.
    // Included here so regional_export sees full coverage from one list.
    {"united_states",     -125, 24, -66, 50},
    // GA-relevant (formerly load_copernicus_regions.sh)
    {"canada",            -141, 41, -52, 60},
    {"mexico",            -118, 14, -86, 33},
    {"central_america",   -93, 7, -77, 18},
    {"caribbean",          -85, 10, -59, 27},
    // Rest of world (formerly load_copernicus_global_rest.sh)
    {"south_america",      -82, -56, -34, 13},
    {"europe",             -25, 34, 40, 71},
    {"africa",             -18, -35, 52, 38},
    {"middle_east",         25, 12, 63, 42},
    {"south_asia",          60, 5, 100, 38},
    {"east_asia",           95, 15, 150, 55},
    {"southeast_asia",      90, -11, 141, 25},
    {"oceania_australia",  110, -47, 180, -10},
    {"russia_north_asia",   40, 45, 180, 78},
    // Final gaps (formerly load_copernicus_final.sh)
    {"northern_canada",   -141, 60, -52, 84},
    {"alaska",            -170, 51, -129, 72},
    {"greenland",          -75, 58, -10, 84},
    {"svalbard_high_arctic", -10, 70, 65, 84},
    {"pacific_islands_west", 170, -25, 180, 25},
    {"pacific_islands_east", -180, -25, -150, 25},
};
