#include "WkbDecode.h"

#include <cstring>

namespace WkbDecode {

namespace {

// Bounds-checked little-endian cursor over a raw byte buffer.
class Cursor {
public:
    Cursor(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool ok() const { return !failed_; }

    uint8_t u8() {
        if (pos_ + 1 > len_) { failed_ = true; return 0; }
        return data_[pos_++];
    }

    uint32_t u32() {
        if (pos_ + 4 > len_) { failed_ = true; return 0; }
        uint32_t v = static_cast<uint32_t>(data_[pos_])       |
                     (static_cast<uint32_t>(data_[pos_ + 1]) << 8)  |
                     (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                     (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }

    double f64() {
        if (pos_ + 8 > len_) { failed_ = true; return 0.0; }
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        pos_ += 8;
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }

    const uint8_t* cur() const { return data_ + pos_; }
    size_t remaining() const { return pos_ <= len_ ? len_ - pos_ : 0; }
    void advance(size_t n) {
        if (pos_ + n > len_) { failed_ = true; return; }
        pos_ += n;
    }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool failed_ = false;
};

// Reads one geometry's header (byte-order + type + SRID) and dispatches to
// the type-specific body reader. Used both for the top-level geometry and
// recursively for MultiLineString's nested single-geometry sub-parts
// (each of which repeats the full byte-order+type+SRID header, per
// GeoUtils::mergeWayGeoms's output format).
std::optional<Decoded> decodeOne(Cursor& c) {
    uint8_t byte_order = c.u8();
    if (!c.ok() || byte_order != 0x01) return std::nullopt;  // this codebase only ever writes little-endian

    uint32_t raw_type = c.u32();
    if (!c.ok()) return std::nullopt;
    int32_t srid = 0;
    if (raw_type & 0x20000000) {
        srid = static_cast<int32_t>(c.u32());
        if (!c.ok()) return std::nullopt;
    }
    uint32_t type = raw_type & 0xFFFF;

    switch (type) {
        case 1: {  // Point
            double x = c.f64();
            double y = c.f64();
            if (!c.ok()) return std::nullopt;
            return Decoded{Point(x, y), srid};
        }
        case 2: {  // LineString
            uint32_t n = c.u32();
            if (!c.ok()) return std::nullopt;
            LineString ls;
            ls.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                double x = c.f64();
                double y = c.f64();
                if (!c.ok()) return std::nullopt;
                ls.push_back(Point(x, y));
            }
            return Decoded{std::move(ls), srid};
        }
        case 3: {  // Polygon
            uint32_t ring_count = c.u32();
            if (!c.ok()) return std::nullopt;
            Polygon poly;
            for (uint32_t r = 0; r < ring_count; ++r) {
                uint32_t n = c.u32();
                if (!c.ok()) return std::nullopt;
                auto& ring = (r == 0) ? poly.outer() : *poly.inners().emplace(poly.inners().end());
                ring.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    double x = c.f64();
                    double y = c.f64();
                    if (!c.ok()) return std::nullopt;
                    ring.push_back(Point(x, y));
                }
            }
            return Decoded{std::move(poly), srid};
        }
        case 5: {  // MultiLineString -- nested sub-geometries, each with its own full header
            uint32_t sub_count = c.u32();
            if (!c.ok()) return std::nullopt;
            MultiLineString mls;
            mls.reserve(sub_count);
            for (uint32_t i = 0; i < sub_count; ++i) {
                auto sub = decodeOne(c);
                if (!sub) return std::nullopt;
                auto* ls = std::get_if<LineString>(&sub->geom);
                if (!ls) return std::nullopt;  // this codebase only ever nests LineStrings (GeoUtils::mergeWayGeoms)
                mls.push_back(std::move(*ls));
            }
            return Decoded{std::move(mls), srid};
        }
        case 6: {  // MultiPolygon -- nested sub-geometries, each with its own full header
            uint32_t sub_count = c.u32();
            if (!c.ok()) return std::nullopt;
            MultiPolygon mp;
            mp.reserve(sub_count);
            for (uint32_t i = 0; i < sub_count; ++i) {
                auto sub = decodeOne(c);
                if (!sub) return std::nullopt;
                auto* poly = std::get_if<Polygon>(&sub->geom);
                if (!poly) return std::nullopt;  // multipolygon relations always nest plain Polygons
                mp.push_back(std::move(*poly));
            }
            return Decoded{std::move(mp), srid};
        }
        default:
            return std::nullopt;  // unrecognized type code -- hard error, not a silent no-match
    }
}

} // namespace

std::optional<Decoded> decode(const uint8_t* data, size_t len) {
    Cursor c(data, len);
    return decodeOne(c);
}

} // namespace WkbDecode
