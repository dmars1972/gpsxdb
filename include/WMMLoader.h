#pragma once
#include <string>
#include "DbClient.h"

// Today's date as a decimal year (e.g. 2026.51), suitable for load's
// `year` parameter — WMM's secular-variation coefficients mean declination
// drifts slowly even at a fixed epoch, so callers that just want "current"
// declination (rather than a specific historical/future date) should use
// this rather than hardcoding a year. Doesn't need a DB connection, so
// stays a free function rather than a method.
double currentDecimalYear();

class WMMLoader : public DbClient {
public:
    WMMLoader(std::string host, std::string user, std::string database)
        : DbClient(std::move(host), std::move(user), std::move(database)) {}

    // Computes World Magnetic Model declination (magnetic variation) globally
    // and loads it into a `wmm` PostGIS raster table (one band, degrees, east
    // positive), analogous to `terrain`. The spherical-harmonic evaluation is a
    // direct port of NOAA/NCEI's public-domain WMM reference algorithm, using
    // the WMM2025 coefficient set (epoch 2025.0, valid 2025.0-2030.0) embedded
    // in WMMLoader.cpp — validated against NOAA's own WMM2025_TestValues.txt to
    // within 0.005 degrees. Past 2030.0 the model silently extrapolates via its
    // secular-variation coefficients with degrading accuracy; a future NOAA
    // model release (WMM2030 etc.) would need its coefficients swapped in.
    //
    // Unlike terrain, there's no external tile source and no "already loaded"
    // per-bbox skip — WMM is a single global mathematical model, not downloaded
    // tiled data, so a cell already computed is only skipped if it's already in
    // `wmm_cells` (see below); re-running with a different `year` does NOT
    // recompute existing cells, so bump/truncate wmm/wmm_cells first if you
    // want to refresh with a new epoch/year.
    //
    // The globe is internally divided into fixed-size cells (10 degrees square)
    // for resumability, tracked in `wmm_cells(cell_name, loaded_at)` exactly
    // like terrain_tiles — a killed/interrupted global load just resumes,
    // skipping already-computed cells. Each cell's PostGIS raster row is built
    // directly via ST_MakeEmptyRaster/ST_AddBand/ST_SetValues (no GDAL/
    // raster2pgsql involved, since there's no source file to reproject — the
    // grid is generated natively in WGS84/EPSG:4326, which is the only sane
    // choice for a lat/lon-uniform grid; ST_Transform is applied when producing
    // wmm_bands polygons if dest_srid differs).
    //
    // min/max_lon/lat: bbox to (re)compute, default the whole globe.
    // grid_deg: resolution of the evaluation/raster grid in degrees (e.g. 0.25).
    // WMM's field is smooth enough that this doesn't need DEM-tile resolution,
    // though smaller values track the (rapidly-changing) magnetic poles better.
    // year: decimal year (e.g. 2026.5) for secular-variation time adjustment.
    // band_deg: declination band width in degrees for the derived wmm_bands
    // polygons (see buildBands below). Pass 0 to skip band generation.
    // dest_srid: SRID for the derived wmm_bands polygons (3857 or 4326) — the
    // underlying wmm raster itself is always stored in 4326.
    // simplify_m / threads / verbose: same meaning as the terrain loader.
    //
    // Returns false if no cell in the bbox could be computed (should not
    // normally happen — computation can't fail the way a tile download can).
    bool load(double year,
              double min_lon = -180, double min_lat = -90,
              double max_lon = 180, double max_lat = 90,
              double grid_deg = 0.25,
              int dest_srid = 3857,
              double band_deg = 0.25,
              double simplify_m = 50.0,
              int threads = 4,
              bool verbose = true);

    // Derives declination-band area polygons from the entire `wmm` raster table
    // (not just newly-computed cells), analogous to buildTerrainBands. Each row
    // is one contiguous polygon within a single [band_min_deg, band_max_deg)
    // declination range — the whole table is rebuilt from scratch on every
    // call (TRUNCATE + regenerate), same cost/rationale as buildTerrainBands.
    //
    // band_deg: band width in degrees. Defaults to matching grid_deg (0.25) —
    // narrower bands than the underlying raster's own grid resolution don't add
    // real precision, just slice the same data into more buckets and expose
    // the raster's pixel staircase as band-boundary noise.
    // dest_srid: SRID for the output polygons (the wmm raster itself is 4326).
    // simplify_m: ST_SimplifyPreserveTopology tolerance in meters, same
    // rationale as buildTerrainBands (smooths raster-pixel-aligned staircase
    // edges). Pass 0 to disable.
    // threads: worker-thread count, same pattern as buildTerrainBands — each
    // thread owns its own pqxx::connection (via newConnection()) and pulls
    // the next unprocessed band off a shared atomic counter.
    //
    // Returns false if `wmm` is empty (nothing to band) or on a DB error.
    bool buildBands(double band_deg = 0.25,
                     int dest_srid = 3857,
                     double simplify_m = 50.0,
                     int threads = 4,
                     bool verbose = true);
};
