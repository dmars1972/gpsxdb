#pragma once
#include <string>
#include <utility>
#include <cstdint>
#include <vector>

// Process-level SRID — set at startup from -L flag.
// 3857 = Web Mercator (default), 4326 = WGS84 lon/lat.
// Defined in main.cpp (or faa_obstacles_load.cpp for standalone),
// declared here so all TUs can reference it without scattering
// 'extern int g_srid' everywhere.
extern int g_srid;

// Project WGS84 lon/lat to Web Mercator (EPSG:3857).
// Returns (x_m, y_m) in metres.
std::pair<double,double> toMercator(double lon, double lat);

// Build a PostGIS-compatible EWKB hex string for a point,
// embedding g_srid as the SRID. Coordinates must already be
// in the target projection (Mercator or WGS84 depending on g_srid).
std::string pointWKB(double x, double y);

// Build a PostGIS-compatible EWKB hex string for a MultiPolygon, embedding
// g_srid as the SRID. Coordinates must already be in the target projection.
// polygons[i] is one polygon's rings (rings[0] = exterior, rest = holes);
// each ring is a closed sequence of (x,y) points (first == last).
std::string multiPolygonWKB(
    const std::vector<std::vector<std::vector<std::pair<double,double>>>>& polygons);
