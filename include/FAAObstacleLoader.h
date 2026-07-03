#pragma once
#include <string>

// Downloads and loads the FAA Daily Digital Obstacle File (DDOF) into the
// faa_obstacles table. The DDOF is updated daily and covers all known man-made
// obstacles in the US that affect aeronautical charting, including towers,
// antennas, wind turbines, buildings, and other structures.
//
// Data source: https://aeronav.faa.gov/Obst_Data/DDOF.zip
// Format documentation: FAA DOF README (fixed-width ASCII)
//
// verbose: when false, suppresses progress output (used by osm_import;
//          the standalone faa_obstacles_load tool passes true).
void loadFAAObstacles(const std::string& server,
                      const std::string& user,
                      const std::string& database,
                      const std::string& password = "",
                      bool verbose = true);
