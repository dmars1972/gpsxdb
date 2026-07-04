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
// band_ft: elevation band width in feet for the derived terrain_bands
// polygons (see buildTerrainBands below). Pass 0 to skip band generation
// entirely and only load the raw raster.
// simplify_m, band_threads: see buildTerrainBands.
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
                 int band_ft = 500,
                 double simplify_m = 50.0,
                 int band_threads = 4,
                 bool verbose = true);

// Derives elevation-band area polygons from the entire `terrain` raster
// table (not just newly-loaded tiles) and (re)populates `terrain_bands`.
//
// Each row is one contiguous polygon within a single [band_min_ft,
// band_max_ft) elevation range, e.g. a color-tint layer suited to a
// sectional-chart-style terrain display — much smaller than either the raw
// raster or USGS's own vector contour-line product (10ft line intervals),
// since it stores areas, not individual elevation isolines.
//
// Bands are unioned across ALL loaded tiles so a mountain range spanning
// several 1-degree tiles comes out as one seamless polygon rather than one
// per tile. This means the whole table is rebuilt from scratch on every
// call (TRUNCATE + regenerate) — cheap at the tile counts this tool is
// meant for (a region/state), but it does mean cost grows with total
// terrain coverage, not just what's newly added.
//
// simplify_m: ST_SimplifyPreserveTopology tolerance in meters, applied to
// each band's unioned shape before the final ST_Dump. The raw band edges
// trace individual 30m raster pixel boundaries (a visible staircase),
// which bloats vertex count/storage without adding real information;
// simplifying smooths that out. Pass 0 to disable. Each band is simplified
// independently, so adjacent bands can develop small gaps/slivers at their
// shared boundary at higher tolerances — fine for a visual terrain tint,
// not for exact-boundary analysis.
//
// band_threads: number of worker threads processing bands concurrently,
// same pattern as the main import's node/way worker pools (main.cpp) — each
// thread owns its own pqxx::connection and pulls the next unprocessed band
// off a shared atomic counter. Unlike the node/way workers, no shared mutex
// is needed here: each band writes disjoint (band_min_ft, band_max_ft) rows
// with no ON CONFLICT/merge step, so there's nothing to serialize.
//
// Returns false if `terrain` is empty (nothing to band) or on a DB error.
bool buildTerrainBands(const std::string& server,
                       const std::string& user,
                       const std::string& database,
                       const std::string& password,
                       int band_ft = 500,
                       double simplify_m = 50.0,
                       int band_threads = 4,
                       bool verbose = true);
