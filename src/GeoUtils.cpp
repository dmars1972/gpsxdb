#include "GeoUtils.h"
#include <proj.h>
#include <vector>
#include <cstring>
#include <stdexcept>

// ---- Mercator projection (thread-local PROJ context, RAII cleanup) ----

struct ProjContext {
    PJ_CONTEXT* ctx;
    PJ* pj;
    ProjContext() {
        ctx = proj_context_create();
        pj  = proj_create_crs_to_crs(ctx, "EPSG:4326", "EPSG:3857", nullptr);
        if (!pj) throw std::runtime_error("PROJ: failed to create CRS transform");
        pj = proj_normalize_for_visualization(ctx, pj);
    }
    ~ProjContext() {
        proj_destroy(pj);
        proj_context_destroy(ctx);
    }
};

static thread_local ProjContext tl_proj;

std::pair<double,double> toMercator(double lon, double lat) {
    PJ_COORD in  = proj_coord(lon, lat, 0, 0);
    PJ_COORD out = proj_trans(tl_proj.pj, PJ_FWD, in);
    return {out.xy.x, out.xy.y};
}

// ---- WKB point builder ----

std::string toHex(const std::vector<uint8_t>& buf) {
    static const char* h = "0123456789ABCDEF";
    std::string out;
    out.reserve(buf.size() * 2);
    for (uint8_t b : buf) { out += h[b>>4]; out += h[b&0xf]; }
    return out;
}

std::string pointWKB(double x, double y) {
    std::vector<uint8_t> buf;
    auto wu32 = [&](uint32_t v) {
        buf.push_back(v & 0xff); buf.push_back((v>>8)&0xff);
        buf.push_back((v>>16)&0xff); buf.push_back((v>>24)&0xff);
    };
    auto wf64 = [&](double v) {
        uint64_t u; memcpy(&u, &v, 8);
        for (int i = 0; i < 8; ++i) buf.push_back((u>>(8*i))&0xff);
    };
    buf.push_back(0x01);
    wu32(0x20000001); // POINT with SRID flag
    wu32(static_cast<uint32_t>(g_srid));
    wf64(x); wf64(y);
    return toHex(buf);
}

// ---- Way/relation WKB construction ----

std::string buildWayGeom(const std::vector<std::pair<double,double>>& coords,
                          bool& is_closed) {
    if (coords.size() < 2) return "";
    is_closed = (coords.size() >= 4 &&
                 coords.front().first  == coords.back().first &&
                 coords.front().second == coords.back().second);

    auto wd = [](std::vector<uint8_t>& buf, double v) {
        uint8_t t[8]; memcpy(t, &v, 8);
        buf.insert(buf.end(), t, t+8);
    };
    auto wu32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(v&0xFF); buf.push_back((v>>8)&0xFF);
        buf.push_back((v>>16)&0xFF); buf.push_back((v>>24)&0xFF);
    };

    std::vector<uint8_t> buf;
    buf.push_back(0x01);
    if (is_closed) {
        wu32(buf, 0x20000003); // WKB polygon with SRID flag
        wu32(buf, static_cast<uint32_t>(g_srid));
        wu32(buf, 1);          // 1 ring
    } else {
        wu32(buf, 0x20000002); // WKB linestring with SRID flag
        wu32(buf, static_cast<uint32_t>(g_srid));
    }
    wu32(buf, static_cast<uint32_t>(coords.size()));
    for (auto& [x, y] : coords) { wd(buf, x); wd(buf, y); }
    return toHex(buf);
}

std::string mergeWayGeoms(const std::vector<std::string>& wkb_hexes) {
    auto fromHex = [](const std::string& hex) {
        std::vector<uint8_t> b; b.reserve(hex.size()/2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto n = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c-'0';
                if (c >= 'A' && c <= 'F') return c-'A'+10;
                if (c >= 'a' && c <= 'f') return c-'a'+10;
                return 0;
            };
            b.push_back((n(hex[i]) << 4) | n(hex[i+1]));
        }
        return b;
    };
    auto wu32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(v&0xFF); buf.push_back((v>>8)&0xFF);
        buf.push_back((v>>16)&0xFF); buf.push_back((v>>24)&0xFF);
    };

    // Only include LineString (type 2) and MultiLineString (type 5) —
    // skip Polygon (type 3) and other non-linear geometries
    auto isLinear = [](const std::vector<uint8_t>& b) -> bool {
        if (b.size() < 5) return false;
        // byte 0 = byte order, bytes 1-4 = geometry type (little-endian)
        uint32_t gtype = b[1] | (b[2]<<8) | (b[3]<<16) | (b[4]<<24);
        // strip SRID flag if present
        gtype &= 0xFFFF;
        return gtype == 2 || gtype == 5;
    };

    std::vector<std::vector<uint8_t>> parts;
    for (auto& h : wkb_hexes) {
        if (h.empty()) continue;
        auto b = fromHex(h);
        if (isLinear(b)) parts.push_back(std::move(b));
    }
    if (parts.empty()) return "";

    std::vector<uint8_t> buf;
    buf.push_back(0x01);
    wu32(buf, 0x20000005); // MultiLineString with SRID flag
    wu32(buf, static_cast<uint32_t>(g_srid));
    wu32(buf, static_cast<uint32_t>(parts.size()));
    for (auto& p : parts) buf.insert(buf.end(), p.begin(), p.end());
    return toHex(buf);
}

// Try to stitch a set of open segments into closed rings.
std::vector<Ring> stitchRings(std::vector<Segment> segs) {
    std::vector<Ring> rings;

    while (!segs.empty()) {
        // Start a new ring with the first available segment
        Ring ring = std::move(segs.front());
        segs.erase(segs.begin());

        bool progress = true;
        while (progress) {
            progress = false;
            if (ring.size() >= 4 &&
                ring.front().first  == ring.back().first &&
                ring.front().second == ring.back().second) {
                break; // ring is closed — done
            }
            for (size_t i = 0; i < segs.size(); ++i) {
                auto& seg = segs[i];
                if (seg.empty()) continue;
                auto& rb = ring.back();
                if (seg.front().first  == rb.first &&
                    seg.front().second == rb.second) {
                    // append forward
                    ring.insert(ring.end(), seg.begin() + 1, seg.end());
                    segs.erase(segs.begin() + i);
                    progress = true; break;
                }
                if (seg.back().first  == rb.first &&
                    seg.back().second == rb.second) {
                    // append reversed
                    ring.insert(ring.end(), seg.rbegin() + 1, seg.rend());
                    segs.erase(segs.begin() + i);
                    progress = true; break;
                }
            }
        }

        // Ensure ring is closed
        if (!ring.empty() &&
            (ring.front().first  != ring.back().first ||
             ring.front().second != ring.back().second)) {
            ring.push_back(ring.front());
        }

        if (ring.size() >= 4)
            rings.push_back(std::move(ring));
    }
    return rings;
}

// Write a WKB ring (LinearRing) into a buffer
void writeWkbRing(std::vector<uint8_t>& buf, const Ring& ring) {
    auto wu32 = [&](uint32_t v) {
        buf.push_back(v & 0xff); buf.push_back((v>>8)&0xff);
        buf.push_back((v>>16)&0xff); buf.push_back((v>>24)&0xff);
    };
    auto wf64 = [&](double v) {
        uint64_t u; memcpy(&u, &v, 8);
        for (int i = 0; i < 8; ++i) buf.push_back((u >> (8*i)) & 0xff);
    };
    wu32(static_cast<uint32_t>(ring.size()));
    for (auto& [x, y] : ring) { wf64(x); wf64(y); }
}

// Write a WKB POLYGON (one outer + zero or more inner rings)
void writeWkbPolygon(std::vector<uint8_t>& buf,
                      const Ring& outer,
                      const std::vector<Ring>& inners) {
    auto wu32 = [&](uint32_t v) {
        buf.push_back(v & 0xff); buf.push_back((v>>8)&0xff);
        buf.push_back((v>>16)&0xff); buf.push_back((v>>24)&0xff);
    };
    buf.push_back(1);           // little-endian
    wu32(0x20000003);           // WKB type: POLYGON with SRID
    wu32(static_cast<uint32_t>(g_srid));
    wu32(1 + inners.size());    // ring count
    writeWkbRing(buf, outer);
    for (auto& inner : inners) writeWkbRing(buf, inner);
}
