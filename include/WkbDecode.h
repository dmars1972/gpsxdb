#pragma once

#include "RegionPolygons.h"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/multi_linestring.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

// Decodes the EWKB variant this codebase writes (see GeoUtils.cpp, the
// reference encoder): byte 0 = 0x01 (little-endian only -- this codebase
// never writes big-endian, so a 0x00 here means corrupt/unsupported data,
// not something to guess at), bytes 1-4 = uint32 LE type code with the
// 0x20000000 SRID flag bit set, bytes 5-8 = uint32 LE SRID, then
// type-specific coordinate data:
//   Point (1):           2x float64 LE (x, y)
//   LineString (2):      uint32 LE point count + that many (x,y) float64 LE pairs
//   Polygon (3):         uint32 LE ring count + per ring [uint32 LE point count + points]
//   MultiLineString (5): uint32 LE sub-geometry count + that many full nested
//                        single-geometry WKBs (each with its own byte-order
//                        +type+SRID header) -- always LineString sub-parts
//                        in practice, since GeoUtils::mergeWayGeoms only
//                        ever wraps LineStrings.
//   MultiPolygon (6):    uint32 LE sub-geometry count + that many full nested
//                        single-geometry WKBs, same nested-full-header
//                        pattern as MultiLineString -- always Polygon
//                        sub-parts. Added after task #48's first full
//                        production run found ~764K public.areas rows
//                        (0.3% of the table) failing to decode: multipolygon
//                        relations (imported with synthetic negative ids)
//                        can legitimately produce a MultiPolygon `areas.geog`
//                        (e.g. a country with islands, a lake with several
//                        parts) -- narrower than the original plan assumed
//                        ("areas: LineString/Polygon" only).
namespace WkbDecode {

using Point = RegionPolygons::Point;
using Polygon = RegionPolygons::Polygon;
using LineString = boost::geometry::model::linestring<Point>;
using MultiLineString = boost::geometry::model::multi_linestring<LineString>;
using MultiPolygon = RegionPolygons::MultiPolygon;

// std::monostate is unused by decode() itself (a NULL geometry field is
// never handed to decode() at all -- see PgCopyBinary's -1 length-prefix
// convention) but included so callers have one variant type to
// std::visit over uniformly regardless of whether a row had geometry.
using GeomVariant = std::variant<std::monostate, Point, LineString, Polygon, MultiLineString, MultiPolygon>;

struct Decoded {
    GeomVariant geom;
    int32_t srid;
};

// Decodes `len` bytes at `data` as this codebase's EWKB variant. Returns
// std::nullopt on any structural error (truncated buffer, unrecognized
// type code, big-endian marker) -- callers must treat this as a hard
// failure (loud warning + skip that row), never silently as "no region
// match" (a silently-dropped row is a correctness bug in a way a decode
// error report isn't).
std::optional<Decoded> decode(const uint8_t* data, size_t len);

} // namespace WkbDecode
