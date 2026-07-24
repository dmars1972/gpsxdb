#pragma once

#include "RegionPolygons.h"
#include "WkbDecode.h"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <string>
#include <utility>
#include <vector>

// Shared spatial-matching infrastructure: for each region, a cheap AABB
// bbox pre-filter + an rtree of the region's constituent polygons'
// envelopes (so a MultiPolygon like north_america's, with hundreds of
// islands, isn't a linear scan of every ring on every candidate) + the
// exact geometry test against only the rtree's candidate hits.
//
// Factored out of regional_export.cpp (task #47), which originally
// hardcoded Mercator-only bare-Point candidates tested via
// boost::geometry::covered_by(). That's only equivalent to SQL's
// ST_Intersects for points -- a LineString/Polygon/MultiLineString
// candidate that merely crosses a region's border (one endpoint outside)
// must still match, which covered_by()/within() would wrongly exclude
// for those types. Generalized here to boost::geometry::intersects(),
// used uniformly for every candidate type including Point, dispatched via
// std::visit over WkbDecode::GeomVariant so one Entry serves both
// regional_export's plain (id, lon_m, lat_m) points and
// regional_table_export's decoded table rows.
//
// Callers are responsible for projecting both the region polygons (via
// build()'s `polygon` parameter) and every candidate geometry into the
// same coordinate system before calling matches() -- this module does no
// projection itself. regional_export projects into Mercator meters (its
// nodes.dat on-disk unit); a future SRID-aware caller may need a
// different system per table.
namespace RegionIndex {

using Point = RegionPolygons::Point;
using Box = boost::geometry::model::box<Point>;
using PolyRtree = boost::geometry::index::rtree<std::pair<Box, size_t>, boost::geometry::index::quadratic<16>>;

// One region's matchable geometry, immutable after build() returns and
// safe to query concurrently from multiple threads (Boost.Geometry's
// rtree supports concurrent reads once built; nothing here is mutated
// afterward).
struct Entry {
    std::string name;
    double min_x, min_y, max_x, max_y;  // caller's coordinate system
    RegionPolygons::MultiPolygon polygon;
    PolyRtree rtree;  // one entry per polygon[i], keyed by its envelope
};

// lo <= hi: ordinary range test. lo > hi: wraps the antimeridian (see
// Regions.h's oceania entry) -- test is "at or east of lo, OR at or west
// of hi" instead of the usual &&.
bool inRange(double v, double lo, double hi);

// Builds one Entry. `polygon` must already be projected into the target
// coordinate system (see the namespace comment); ownership is moved in.
Entry build(std::string name, double min_x, double min_y, double max_x, double max_y,
            RegionPolygons::MultiPolygon polygon);

// Tests a candidate geometry against one region: bbox-overlap pre-filter
// (generalizing inRange() from a single point to a candidate envelope),
// then an rtree envelope query, then an exact intersects() test against
// only the rtree's candidate polygons. std::monostate (NULL geometry, see
// WkbDecode::GeomVariant) never matches -- callers should skip NULL rows
// before calling this rather than relying on that, since it's also the
// point where a NULL-geometry row should be excluded from every region's
// id->bitmask map, not just silently tested and rejected here.
//
// `scratch` is caller-owned reusable storage for the rtree query's hit
// list (cleared internally on every call) -- this runs per-candidate,
// per-region, potentially billions of times across a full nodes.dat scan
// (see regional_export.cpp), so a fresh heap allocation per call here
// would be a severe, easily-avoidable regression versus the pre-refactor
// code's reused scratch vector.
bool matches(const Entry& region, const WkbDecode::GeomVariant& candidate,
             std::vector<std::pair<Box, size_t>>& scratch);

} // namespace RegionIndex
