#pragma once
#include <string>
#include "DbClient.h"

// USGS3DEP: US + territories only, 1 arc-second (~30m), NAD83 (EPSG:4269),
// tiles named by NW corner (see the tile-naming comment in TerrainLoader.cpp).
//
// CopernicusGLO30: near-global coverage, ~30m, WGS84 (EPSG:4326), tiles
// named by SW corner (the more common convention — opposite of 3DEP's).
// Public AWS Open Data bucket, no API key needed, same no-auth workflow as
// 3DEP. Use this for coverage outside the US; for the US, 3DEP is preferred
// (US-government-authoritative, and likely already loaded) — running
// Copernicus over a bbox that overlaps existing 3DEP coverage will layer
// redundant/overlapping raster data in `terrain` (both sources' tiles are
// tracked independently in terrain_tiles, keyed by tile_name, which never
// collides between sources since the naming formats are structurally
// distinct) — pick bboxes deliberately to avoid overlap.
enum class TerrainSource { USGS3DEP, CopernicusGLO30 };

class TerrainLoader : public DbClient {
public:
    TerrainLoader(std::string host, std::string user, std::string database)
        : DbClient(std::move(host), std::move(user), std::move(database)) {}

    // Downloads elevation tiles from `source` covering the given WGS84
    // bounding box (no API key needed for either source), reprojects to
    // dest_srid via raster2pgsql, and loads them into the `terrain` PostGIS
    // raster table.
    //
    // Tiles outside the source's coverage simply fail to download and are
    // skipped rather than treated as fatal, since a requested bbox may
    // partially overlap ocean or (for 3DEP) non-US territory.
    //
    // Already-loaded tiles (tracked in `terrain_tiles`, alongside which
    // source loaded them) are skipped, so re-running with an overlapping
    // bbox only fetches what's new.
    //
    // dest_srid: 3857 (Web Mercator, default) or 4326 (WGS84).
    // threads: worker-thread count for parallel tile download/load batches.
    // verbose: when true, prints download/load progress to stdout.
    //
    // Returns false if no tile could be downloaded/loaded at all. Partial
    // coverage (some tiles missing, e.g. bbox extends past the source's edge)
    // is not treated as failure.
    bool load(double min_lon, double min_lat,
              double max_lon, double max_lat,
              TerrainSource source = TerrainSource::USGS3DEP,
              int dest_srid = 3857,
              int threads = 4,
              bool verbose = true);

    // Loads Copernicus DEM GLO-30 for every populated non-US region on the
    // planet, in one call — the same 19 named region bboxes previously split
    // across load_copernicus_regions.sh (Canada/Mexico/Central America/
    // Caribbean), load_copernicus_global_rest.sh (South America, Europe,
    // Africa, Middle East, Asia, Russia, Oceania/Australia), and
    // load_copernicus_final.sh (northern Canada, Alaska, Greenland, Svalbard/
    // high-Arctic, Pacific islands crossing the antimeridian), now embedded
    // directly rather than orchestrated via three separate shell scripts.
    // Deliberately excludes the continental US (3DEP is authoritative there —
    // load that separately with source USGS3DEP) and Antarctica (minimal
    // Copernicus coverage, no permanent civil GA population).
    //
    // Returns false only if every single region failed to load anything
    // (a fully-connected run will normally return true even if a handful of
    // individual tiles within a region were unavailable, same as load).
    bool loadGlobal(int dest_srid = 3857,
                    int threads = 10,
                    bool verbose = true);
};
