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

// ---- Way/relation WKB construction ----
//
// Ring/Segment are the same underlying type (a coordinate sequence) named
// differently for context: a Segment is a raw way's coordinates before
// stitching, a Ring is a closed loop after stitching.
using Ring    = std::vector<std::pair<double,double>>;
using Segment = std::vector<std::pair<double,double>>;

// Encode a byte buffer as an uppercase hex string.
std::string toHex(const std::vector<uint8_t>& buf);

// Build WKB (hex) for a way's coordinate sequence: LineString, or Polygon
// if the sequence is closed (>=4 points, first == last). Embeds g_srid.
std::string buildWayGeom(const std::vector<std::pair<double,double>>& coords, bool& is_closed);

// Merge multiple way WKB hex strings into one MultiLineString WKB hex
// string. Only LineString/MultiLineString inputs are included — Polygon
// and other non-linear geometries are silently skipped.
std::string mergeWayGeoms(const std::vector<std::string>& wkb_hexes);

// Stitch open segments into closed rings (relation/multipolygon
// assembly). A segment set that never closes into a ring (>=4 points,
// first == last) is dropped rather than emitted malformed.
std::vector<Ring> stitchRings(std::vector<Segment> segs);

// Write a WKB ring (LinearRing) into buf.
void writeWkbRing(std::vector<uint8_t>& buf, const Ring& ring);

// Write a WKB POLYGON (one outer + zero or more inner/hole rings) into
// buf. Embeds g_srid.
void writeWkbPolygon(std::vector<uint8_t>& buf, const Ring& outer, const std::vector<Ring>& inners);
