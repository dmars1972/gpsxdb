#pragma once

#include <vector>

// Natural-boundary (coastline + physical divide) regions, replacing the
// old crude bounding-box list. Used by TerrainLoader (global Copernicus
// DEM coverage, bbox-based tile sweep, unchanged in this scope) and by the
// regional export tooling (regional_export/regional_db_export, polygon-
// based point-in-polygon / ST_Intersects filtering).
//
// Each region's real boundary lives in data/regions/<name>.wkt (MULTIPOLYGON,
// WGS84 degrees) -- the canonical source of truth, generated once by
// tools/prepare_regions.py from Natural Earth coastline data clipped along
// 7 hand-specified physical divide lines (Panama/Darien, Urals+Caucasus+
// Bosporus, Suez, Iran-Pakistan highlands, Pamir/Tian Shan, Himalaya,
// Yunnan-Myanmar, Kazakh steppe/Altai -- see that script for exact
// coordinates). Both regional_export.cpp (in-process Boost.Geometry test,
// via RegionPolygons.h) and regional_db_export.cpp (loaded into the
// region_boundaries PostGIS table) read the same WKT files, so the two
// never drift apart.

// Continental US bbox -- 3DEP is authoritative here (separate, higher-
// accuracy load call in main.cpp: ~10m nationwide, ~1m in lidar-covered
// areas, vs. Copernicus GLO-30's 30m with canopy-height radar bias). Single
// canonical definition -- do not duplicate this bbox anywhere else. Used by
// (a) main.cpp's two 3DEP load() call sites and (b)
// TerrainLoader::loadGlobal()'s north_america Copernicus sweep, which skips
// any candidate tile falling inside this bbox to avoid redundant
// lower-accuracy coverage over the same ground (see TerrainLoader.cpp).
struct Bbox { double min_lon, min_lat, max_lon, max_lat; };
constexpr Bbox kConusBbox = {-125, 24, -66, 50};

// One natural-boundary region. `name` doubles as the id used to load
// data/regions/<name>.wkt and the region_boundaries.name column in Postgres
// -- one name ties the C++ polygon, the SQL polygon, and the customer
// bundle filename together.
//
// min_lon/min_lat/max_lon/max_lat is the polygon's own bounding envelope in
// WGS84 degrees (auto-derived by tools/prepare_regions.py's printed
// `bounds=` output -- never hand-edit independently of the WKT file, with
// one deliberate exception: see `oceania` below). Used only as a cheap AABB
// pre-filter ahead of the real polygon test in regional_export.cpp, and by
// TerrainLoader's bbox tile-sweep (unchanged scope in this task).
//
// NOTE: `oceania`'s natural envelope spans the full -180..180 longitude
// range (Australia, Melanesia, and Pacific islands on both sides of the
// antimeridian all belong to it), which would make the "cheap" bbox
// pre-filter a no-op. Its min_lon/max_lon are hand-set below to the
// deliberate "wraps the antimeridian" convention instead (min_lon > max_lon
// signals wraparound: test is lon>=min_lon || lon<=max_lon, not the usual
// &&) -- see regional_export.cpp's inRange() helper. The underlying
// data/regions/oceania.wkt polygon itself needs no special handling; it's
// just three ordinary non-wrapping pieces (Australia/New Guinea, western
// Pacific islands, eastern Pacific islands).
struct GlobalRegion {
    const char* name;
    double min_lon, min_lat, max_lon, max_lat;
};

constexpr GlobalRegion kGlobalRegions[] = {
    {"north_america",  -180.00, -2.00,  -5.00,  83.60},
    {"south_america",   -81.34, -58.49, -26.26, 11.33},
    {"europe",           -5.11,  34.00,  65.00, 81.85},
    {"africa",          -16.29, -34.79,  55.00, 38.00},
    {"west_asia",        25.05,  10.00,  80.00, 47.00},
    {"south_asia",       61.02,   3.00, 100.00, 40.00},
    {"east_asia",        74.00,  15.00, 150.00, 50.00},
    {"southeast_asia",   92.50, -12.00, 141.00, 29.00},
    {"north_asia",       35.12,  41.79, 180.00, 81.28},
    {"oceania",         110.00, -47.00, -150.00, 22.22},  // wraps antimeridian, see note above
};

// 900nm corridors (~100kt x 9hr full flying-day radius in every direction,
// not just a single leg -- see tools/prepare_regions.py) straddling each
// shared border between two adjacent kGlobalRegions entries (11 land
// divides + the Europe/Africa Mediterranean water gap), for pilots based
// near a border who don't want to download two full continent-scale
// bundles (see tools/prepare_regions.py's build_border_regions() for how
// each is derived and task #44 for the design discussion). Sized so a
// border-based pilot gets the same full-day operating radius as someone in
// a region's interior -- no compromise for being near a divide. At this
// width several corridors exceed the area of the smaller primary regions
// they border (e.g. balochistan > west_asia); that's the point, not a
// side effect to minimize. Deliberately overlap their parent regions (and,
// at a few three-way junctions, each other) -- NOT run through the
// primary-region dedup in prepare_regions.py, and every consumer that
// iterates regions for the regional export/bundle/poll-filter product
// (regional_export.cpp, regional_db_export.cpp, RegionPolygons.cpp) treats
// each region independently already, so a point matching multiple regions
// here is expected, not a bug.
//
// Deliberately EXCLUDED from TerrainLoader's Copernicus sweep
// (TerrainLoader.cpp iterates kGlobalRegions only, not this array): a
// border corridor sits entirely within the primary region(s) it straddles
// (300nm is small next to a continent), so its terrain is already covered
// by the primary sweep -- adding these here would just mean redundant
// Copernicus downloads over already-loaded ground.
// Named after the physical feature at each divide (not the country/region
// pair it splits, e.g. "balochistan" not "iran_pakistan") -- see
// tools/prepare_regions.py's build_border_regions() borders dict for which
// pair of kGlobalRegions entries each one straddles.
constexpr GlobalRegion kBorderRegions[] = {
    {"central_america",  -91.26,  -7.49,  10.28,  25.45},  // north_america / south_america
    {"urals",             23.69,  32.01, 109.36,  81.85},  // europe / north_asia
    {"caucasus",           6.45,  25.06,  74.22,  61.90},  // europe / west_asia
    {"suez",              14.77,  14.86,  49.93,  46.30},  // africa / west_asia
    {"balochistan",       42.43,  12.32,  79.58,  51.00},  // west_asia / south_asia
    {"pamir",             56.17,  21.97, 101.37,  59.97},  // west_asia / east_asia
    {"kazakh_steppe",     29.78,  26.99, 100.43,  61.90},  // west_asia / north_asia
    {"himalaya",          55.64,  12.72, 113.98,  49.91},  // south_asia / east_asia
    {"arakan",            76.44,  -2.91, 111.80,  42.02},  // south_asia / southeast_asia
    {"yunnan",             80.86,  7.30, 120.16,  44.01},  // east_asia / southeast_asia
    {"altai",              59.75, 27.82, 162.05,  65.02},  // east_asia / north_asia
    {"mediterranean",    -18.16,  17.13,  51.67,  52.87},  // europe / africa
};

// All regions relevant to the customer-facing export/bundle/poll-filter
// product (primary + border) -- what regional_export.cpp,
// regional_db_export.cpp, and RegionPolygons.cpp should iterate. Excludes
// nothing TerrainLoader needs, since TerrainLoader deliberately uses
// kGlobalRegions directly instead (see kBorderRegions' comment above).
inline std::vector<GlobalRegion> allExportRegions() {
    std::vector<GlobalRegion> v(std::begin(kGlobalRegions), std::end(kGlobalRegions));
    v.insert(v.end(), std::begin(kBorderRegions), std::end(kBorderRegions));
    return v;
}
