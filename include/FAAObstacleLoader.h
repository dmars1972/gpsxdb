#pragma once
#include <string>
#include "DbClient.h"

// Downloads and loads the FAA Daily Digital Obstacle File (DDOF) into the
// faa_obstacles table. The DDOF is updated daily and covers all known man-made
// obstacles in the US that affect aeronautical charting, including towers,
// antennas, wind turbines, buildings, and other structures.
//
// Data source: https://aeronav.faa.gov/Obst_Data/DDOF.zip
// Format documentation: FAA DOF README (fixed-width ASCII)
class FAAObstacleLoader : public DbClient {
public:
    FAAObstacleLoader(std::string host, std::string user, std::string database)
        : DbClient(std::move(host), std::move(user), std::move(database)) {}

    // verbose: when false, suppresses progress output (used by osm_import;
    //          the standalone faa_obstacles_load tool passes true).
    //
    // Returns false if the download/extract failed (nothing is loaded in
    // that case — existing table contents are left untouched). Callers
    // that need to distinguish "genuinely reloaded" from "skipped" (e.g.
    // before recording an upstream-update checkpoint) must check the
    // return value.
    bool load(bool verbose = true);
};
