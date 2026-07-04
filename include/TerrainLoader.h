#pragma once
#include <string>

// Downloads USGS 3DEP 1 arc-second (~30m) elevation tiles covering the given
// WGS84 bounding box from the public prd-tnm S3 bucket (no API key needed),
// reprojects them from NAD83 (EPSG:4269) to dest_srid via raster2pgsql, and
// loads them into the `terrain` PostGIS raster table.
//
// US-only coverage — 3DEP does not cover outside the US and territories;
// tiles outside its coverage simply fail to download and are skipped rather
// than treated as fatal, since a requested bbox may partially overlap ocean
// or non-US territory.
//
// Already-loaded tiles (tracked in `terrain_tiles`) are skipped, so
// re-running with an overlapping bbox only fetches what's new.
//
// dest_srid: 3857 (Web Mercator, default) or 4326 (WGS84).
// verbose: when true, prints download/load progress to stdout.
//
// Returns false if no tile could be downloaded/loaded at all. Partial
// coverage (some tiles missing, e.g. bbox extends past 3DEP's edge) is not
// treated as failure.
bool loadTerrain(const std::string& server,
                 const std::string& user,
                 const std::string& database,
                 const std::string& password,
                 double min_lon, double min_lat,
                 double max_lon, double max_lat,
                 int dest_srid = 3857,
                 bool verbose = true);
