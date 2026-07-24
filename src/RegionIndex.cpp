#include "RegionIndex.h"

#include <variant>
#include <vector>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

namespace RegionIndex {

bool inRange(double v, double lo, double hi) {
    return lo <= hi ? (v >= lo && v <= hi) : (v >= lo || v <= hi);
}

namespace {

// Generalizes inRange() from a single value to an interval-overlap test,
// for the bbox pre-filter against a candidate's own envelope rather than
// a single point.
bool rangesOverlap(double lo, double hi, double v0, double v1) {
    if (lo <= hi) return v1 >= lo && v0 <= hi;
    // Antimeridian wrap: region occupies [lo, +inf) union (-inf, hi] --
    // the excluded gap is (hi, lo), so overlap fails only if the
    // candidate's own range sits entirely inside that gap.
    return !(v0 > hi && v1 < lo);
}

struct EnvelopeVisitor {
    Box* out;
    bool operator()(std::monostate) const { return false; }
    template <typename G>
    bool operator()(const G& g) const { bg::envelope(g, *out); return true; }
};

struct IntersectsVisitor {
    const RegionPolygons::Polygon* poly;
    bool operator()(std::monostate) const { return false; }
    template <typename G>
    bool operator()(const G& g) const { return bg::intersects(g, *poly); }
};

} // namespace

Entry build(std::string name, double min_x, double min_y, double max_x, double max_y,
            RegionPolygons::MultiPolygon polygon) {
    Entry e;
    e.name = std::move(name);
    e.min_x = min_x;
    e.min_y = min_y;
    e.max_x = max_x;
    e.max_y = max_y;
    e.polygon = std::move(polygon);
    for (size_t i = 0; i < e.polygon.size(); ++i) {
        Box box;
        bg::envelope(e.polygon[i], box);
        e.rtree.insert({box, i});
    }
    return e;
}

bool matches(const Entry& region, const WkbDecode::GeomVariant& candidate,
             std::vector<std::pair<Box, size_t>>& scratch) {
    Box candidate_box;
    if (!std::visit(EnvelopeVisitor{&candidate_box}, candidate)) return false;  // NULL geometry

    const Point& lo_pt = candidate_box.min_corner();
    const Point& hi_pt = candidate_box.max_corner();
    if (!rangesOverlap(region.min_x, region.max_x, lo_pt.x(), hi_pt.x())) return false;
    if (!(hi_pt.y() >= region.min_y && lo_pt.y() <= region.max_y)) return false;

    scratch.clear();
    region.rtree.query(bgi::intersects(candidate_box), std::back_inserter(scratch));
    for (auto& h : scratch) {
        if (std::visit(IntersectsVisitor{&region.polygon[h.second]}, candidate)) return true;
    }
    return false;
}

} // namespace RegionIndex
