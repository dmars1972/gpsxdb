// Throwaway validation: round-trip GeoUtils.cpp's WKB encoders through
// WkbDecode and assert the decoded coordinates match bit-for-bit. Not part
// of the production build -- compiled/run directly, see task #45.
#include "GeoUtils.h"
#include "WkbDecode.h"

#include <cassert>
#include <cstdio>
#include <vector>

int g_srid = 3857;

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> b;
    b.reserve(hex.size() / 2);
    auto n = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back((n(hex[i]) << 4) | n(hex[i + 1]));
    return b;
}

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++failures; }
    else std::printf("OK: %s\n", what);
}

int main() {
    // Point
    {
        auto hex = pointWKB(123.456, -78.9);
        auto bytes = fromHex(hex);
        auto d = WkbDecode::decode(bytes.data(), bytes.size());
        check(d.has_value(), "point decodes");
        if (d) {
            auto* pt = std::get_if<WkbDecode::Point>(&d->geom);
            check(pt != nullptr, "point variant type");
            check(pt && pt->x() == 123.456 && pt->y() == -78.9, "point coords match");
            check(d->srid == 3857, "point srid matches g_srid");
        }
    }

    // LineString (open way)
    {
        std::vector<std::pair<double,double>> coords = {{0,0},{1,1},{2,0},{3,5}};
        bool is_closed = false;
        auto hex = buildWayGeom(coords, is_closed);
        check(!is_closed, "open way not marked closed");
        auto bytes = fromHex(hex);
        auto d = WkbDecode::decode(bytes.data(), bytes.size());
        check(d.has_value(), "linestring decodes");
        if (d) {
            auto* ls = std::get_if<WkbDecode::LineString>(&d->geom);
            check(ls != nullptr, "linestring variant type");
            if (ls) {
                check(ls->size() == coords.size(), "linestring point count matches");
                bool all_match = true;
                for (size_t i = 0; i < coords.size(); ++i) {
                    if ((*ls)[i].x() != coords[i].first || (*ls)[i].y() != coords[i].second) all_match = false;
                }
                check(all_match, "linestring coords match");
            }
        }
    }

    // Polygon (closed way)
    {
        std::vector<std::pair<double,double>> coords = {{0,0},{4,0},{4,4},{0,4},{0,0}};
        bool is_closed = false;
        auto hex = buildWayGeom(coords, is_closed);
        check(is_closed, "closed way marked closed");
        auto bytes = fromHex(hex);
        auto d = WkbDecode::decode(bytes.data(), bytes.size());
        check(d.has_value(), "polygon (from closed way) decodes");
        if (d) {
            auto* poly = std::get_if<WkbDecode::Polygon>(&d->geom);
            check(poly != nullptr, "polygon variant type");
            if (poly) {
                check(poly->outer().size() == coords.size(), "polygon outer ring point count matches");
                check(poly->inners().empty(), "polygon has no inner rings");
            }
        }
    }

    // Polygon with a hole, via writeWkbPolygon directly
    {
        Ring outer = {{0,0},{10,0},{10,10},{0,10},{0,0}};
        Ring inner = {{2,2},{2,4},{4,4},{4,2},{2,2}};
        std::vector<uint8_t> buf;
        writeWkbPolygon(buf, outer, {inner});
        auto d = WkbDecode::decode(buf.data(), buf.size());
        check(d.has_value(), "polygon with hole decodes");
        if (d) {
            auto* poly = std::get_if<WkbDecode::Polygon>(&d->geom);
            check(poly != nullptr, "polygon-with-hole variant type");
            if (poly) {
                check(poly->outer().size() == outer.size(), "outer ring point count matches");
                check(poly->inners().size() == 1, "one inner ring present");
                check(poly->inners()[0].size() == inner.size(), "inner ring point count matches");
            }
        }
    }

    // MultiLineString via mergeWayGeoms
    {
        std::vector<std::pair<double,double>> c1 = {{0,0},{1,1}};
        std::vector<std::pair<double,double>> c2 = {{5,5},{6,6},{7,7}};
        bool closed1 = false, closed2 = false;
        auto h1 = buildWayGeom(c1, closed1);
        auto h2 = buildWayGeom(c2, closed2);
        auto merged_hex = mergeWayGeoms({h1, h2});
        auto bytes = fromHex(merged_hex);
        auto d = WkbDecode::decode(bytes.data(), bytes.size());
        check(d.has_value(), "multilinestring decodes");
        if (d) {
            auto* mls = std::get_if<WkbDecode::MultiLineString>(&d->geom);
            check(mls != nullptr, "multilinestring variant type");
            if (mls) {
                check(mls->size() == 2, "multilinestring has 2 sub-parts");
                check((*mls)[0].size() == c1.size() && (*mls)[1].size() == c2.size(),
                      "multilinestring sub-part point counts match");
            }
        }
    }

    // Malformed input: truncated buffer must fail cleanly, not crash
    {
        std::vector<uint8_t> truncated = {0x01, 0x01, 0x00, 0x00};  // claims Point but cut short
        auto d = WkbDecode::decode(truncated.data(), truncated.size());
        check(!d.has_value(), "truncated buffer decodes to nullopt, not garbage");
    }

    // Malformed input: bad byte-order marker must fail cleanly
    {
        std::vector<uint8_t> bad_endian = {0x00, 0x00, 0x00, 0x00, 0x01};
        auto d = WkbDecode::decode(bad_endian.data(), bad_endian.size());
        check(!d.has_value(), "big-endian marker rejected");
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
