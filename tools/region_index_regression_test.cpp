// Regression gate for task #47 (factoring RegionIndex out of
// regional_export.cpp, generalizing covered_by -> intersects): re-derives
// the exact pre-refactor algorithm inline (verbatim from the git history of
// regional_export.cpp before this session's refactor -- bbox pre-filter via
// inRange(), rtree query, boost::geometry::covered_by exact test) and
// compares its verdict against RegionIndex::matches() for a batch of
// hand-picked points against the real production WKT polygons in
// data/regions/. Point-vs-Polygon intersects() and covered_by() are both
// boundary-inclusive in Boost.Geometry, so these should agree on every
// input; any mismatch here is a refactor bug, not an expected algorithm
// change (that only applies to non-Point candidates, exercised later by
// regional_table_export, not by this tool).
//
// Not part of the production build -- compiled/run directly, see task #47.
#include "GeoUtils.h"
#include "RegionIndex.h"
#include "RegionPolygons.h"
#include "Regions.h"
#include "WkbDecode.h"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int g_srid = 3857;  // GeoUtils.h extern, unused here

namespace bgi = boost::geometry::index;

using OldBox = boost::geometry::model::box<RegionPolygons::Point>;
using OldRtree = bgi::rtree<std::pair<OldBox, size_t>, bgi::quadratic<16>>;

struct OldRegionGeom {
    std::string name;
    double min_x, min_y, max_x, max_y;
    RegionPolygons::MultiPolygon polygon_mercator;
    OldRtree rtree;
};

bool oldInRange(double v, double lo, double hi) {
    return lo <= hi ? (v >= lo && v <= hi) : (v >= lo || v <= hi);
}

void projectToMercator(RegionPolygons::MultiPolygon& mp) {
    boost::geometry::for_each_point(mp, [](RegionPolygons::Point& p) {
        auto [x, y] = toMercator(p.x(), p.y());
        p.x(x);
        p.y(y);
    });
}

bool oldMatches(const OldRegionGeom& g, double lon_m, double lat_m) {
    if (!oldInRange(lon_m, g.min_x, g.max_x)) return false;
    if (lat_m < g.min_y || lat_m > g.max_y) return false;
    RegionPolygons::Point pt{lon_m, lat_m};
    std::vector<std::pair<OldBox, size_t>> candidates;
    g.rtree.query(bgi::intersects(pt), std::back_inserter(candidates));
    for (auto& c : candidates) {
        if (boost::geometry::covered_by(pt, g.polygon_mercator[c.second])) return true;
    }
    return false;
}

int main() {
    std::string regions_dir = "data/regions";
    auto polygons = RegionPolygons::load(regions_dir);

    std::vector<OldRegionGeom> old_regions;
    std::vector<RegionIndex::Entry> new_regions;
    for (const auto& r : allExportRegions()) {
        RegionPolygons::MultiPolygon poly_old = polygons.at(r.name);
        projectToMercator(poly_old);
        auto [min_x, min_y] = toMercator(r.min_lon, r.min_lat);
        auto [max_x, max_y] = toMercator(r.max_lon, r.max_lat);

        OldRegionGeom og;
        og.name = r.name;
        og.min_x = min_x; og.min_y = min_y; og.max_x = max_x; og.max_y = max_y;
        og.polygon_mercator = poly_old;
        for (size_t i = 0; i < og.polygon_mercator.size(); ++i) {
            OldBox box;
            boost::geometry::envelope(og.polygon_mercator[i], box);
            og.rtree.insert({box, i});
        }
        old_regions.push_back(std::move(og));

        RegionPolygons::MultiPolygon poly_new = polygons.at(r.name);
        projectToMercator(poly_new);
        new_regions.push_back(RegionIndex::build(r.name, min_x, min_y, max_x, max_y, std::move(poly_new)));
    }

    // (name, lon, lat) -- clearly-inside points for every primary region,
    // a couple of border-region overlap points, antimeridian-wrap cases
    // for oceania (both sides), and open-ocean points that pass a bbox
    // pre-filter but must fail the exact polygon test.
    struct TestPoint { const char* label; double lon, lat; };
    std::vector<TestPoint> points = {
        {"north_america interior (Kansas)",      -98.0,  39.0},
        {"south_america interior (Brazil)",      -55.0, -10.0},
        {"europe interior (Germany)",              10.0,  51.0},
        {"africa interior (Chad)",                 18.0,  15.0},
        {"west_asia interior (Iran)",              53.0,  32.0},
        {"south_asia interior (India)",            78.0,  22.0},
        {"east_asia interior (China)",            110.0,  35.0},
        {"southeast_asia interior (Thailand)",    101.0,  15.0},
        {"north_asia interior (Siberia)",         100.0,  60.0},
        {"oceania interior (Australia)",          135.0, -25.0},
        {"oceania antimeridian east side",        170.0, -18.0},
        {"oceania antimeridian west side",       -170.0, -15.0},
        {"prime-meridian gap (not oceania)",        0.0,   0.0},
        {"balochistan/west_asia overlap (Iran)",   58.0,  30.0},
        {"suez/africa overlap (Egypt)",            32.0,  27.0},
        {"open ocean, mid-Pacific",               -160.0, 10.0},
        {"open ocean, south Atlantic",             -20.0, -30.0},
        {"open ocean, Indian Ocean",                70.0, -30.0},
        {"Bering strait area (Alaska)",           -165.0,  65.0},
        {"Panama border corridor",                 -79.0,   9.0},
    };

    int mismatches = 0;
    int total_matches_old = 0, total_matches_new = 0;
    for (const auto& tp : points) {
        auto [lon_m, lat_m] = toMercator(tp.lon, tp.lat);
        std::vector<std::string> old_hits, new_hits;
        std::vector<std::pair<RegionIndex::Box, size_t>> scratch;
        WkbDecode::GeomVariant pt = RegionIndex::Point{lon_m, lat_m};
        for (size_t i = 0; i < old_regions.size(); ++i) {
            bool o = oldMatches(old_regions[i], lon_m, lat_m);
            bool n = RegionIndex::matches(new_regions[i], pt, scratch);
            if (o) old_hits.push_back(old_regions[i].name);
            if (n) new_hits.push_back(new_regions[i].name);
            if (o != n) {
                std::printf("FAIL: %s vs region %s: old=%d new=%d\n",
                            tp.label, old_regions[i].name.c_str(), o, n);
                ++mismatches;
            }
        }
        total_matches_old += static_cast<int>(old_hits.size());
        total_matches_new += static_cast<int>(new_hits.size());
        std::printf("OK: %-40s old_hits=%zu new_hits=%zu [", tp.label, old_hits.size(), new_hits.size());
        for (size_t i = 0; i < new_hits.size(); ++i) std::printf("%s%s", i ? "," : "", new_hits[i].c_str());
        std::printf("]\n");
    }

    std::printf("\ntotal old matches=%d, total new matches=%d, mismatches=%d\n",
                total_matches_old, total_matches_new, mismatches);
    return mismatches == 0 ? 0 : 1;
}
