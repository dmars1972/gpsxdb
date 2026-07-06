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

    static const char* h = "0123456789ABCDEF";
    std::string out; out.reserve(buf.size() * 2);
    for (uint8_t b : buf) { out += h[b>>4]; out += h[b&0xf]; }
    return out;
}
