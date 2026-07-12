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
    // band_ft: elevation band width in feet for the derived terrain_bands
    // polygons (see buildBands below). Pass 0 to skip band generation
    // entirely and only load the raw raster.
    // simplify_m: see buildBands.
    // threads: worker-thread count, reused for both stages — parallel tile
    // download/load batches here, then handed to buildBands below for
    // parallel band generation. Same rationale both times: independent units
    // of work (batches / bands) with no shared mutable state besides tables
    // that tolerate concurrent writers, so no serialization is needed.
    // verbose: when true, prints download/load progress to stdout.
    //
    // Returns false if no tile could be downloaded/loaded at all. Partial
    // coverage (some tiles missing, e.g. bbox extends past the source's edge)
    // is not treated as failure.
    bool load(double min_lon, double min_lat,
              double max_lon, double max_lat,
              TerrainSource source = TerrainSource::USGS3DEP,
              int dest_srid = 3857,
              int band_ft = 500,
              double simplify_m = 50.0,
              int threads = 4,
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
    // threads: number of worker threads processing bands concurrently,
    // same pattern as the main import's node/way worker pools (main.cpp) — each
    // thread owns its own pqxx::connection (via newConnection()) and pulls the
    // next unprocessed band off a shared atomic counter. Unlike the node/way
    // workers, no shared mutex is needed here: each band writes disjoint
    // (band_min_ft, band_max_ft) rows with no ON CONFLICT/merge step, so
    // there's nothing to serialize.
    //
    // Returns false if `terrain` is empty (nothing to band) or on a DB error.
    bool buildBands(int band_ft = 500,
                    double simplify_m = 50.0,
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
    // Each region is loaded with band generation suppressed (equivalent to
    // --no-bands) — rebuilding terrain_bands after every one of 19 regions
    // would mean redoing that expensive whole-table rebuild 19 times over.
    // If band_ft > 0, a single buildBands call runs once at the very end,
    // across the whole newly-expanded terrain table. Pass band_ft = 0 to
    // skip band generation entirely, same as load.
    //
    // Returns false only if every single region failed to load anything
    // (a fully-connected run will normally return true even if a handful of
    // individual tiles within a region were unavailable, same as load).
    bool loadGlobal(int dest_srid = 3857,
                    int band_ft = 500,
                    double simplify_m = 50.0,
                    int threads = 10,
                    bool verbose = true);
};
