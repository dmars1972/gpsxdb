#include "RegionalDeltaApplier.h"

#include "OSMReader.h"
#include "WkbDecode.h"

#include <cstring>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <vector>

// ---- WKB helpers (duplicated from DeltaApplier.cpp for standalone use —
// see that file's own top-of-file comment for the same rationale) ----

static std::string toHexLocal(const std::vector<uint8_t>& b) {
    std::ostringstream ss;
    for (auto c : b) ss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(c);
    return ss.str();
}

static void writeLE32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 24) & 0xFF);
}

static void writeDouble(std::vector<uint8_t>& b, double v) {
    uint8_t buf[8];
    memcpy(buf, &v, 8);
    for (auto c : buf) b.push_back(c);
}

static std::string buildLineWKB(const std::vector<std::pair<double,double>>& pts,
                                 bool closed) {
    if (pts.size() < 2) return "";

    std::vector<uint8_t> b;
    b.push_back(1); // little endian
    if (closed && pts.size() >= 4) {
        writeLE32(b, 0x20000003); // Polygon with SRID
        writeLE32(b, static_cast<uint32_t>(g_srid));
        writeLE32(b, 1); // 1 ring
        writeLE32(b, static_cast<uint32_t>(pts.size()));
    } else {
        writeLE32(b, 0x20000002); // LineString with SRID
        writeLE32(b, static_cast<uint32_t>(g_srid));
        writeLE32(b, static_cast<uint32_t>(pts.size()));
    }
    for (auto& [x, y] : pts) { writeDouble(b, x); writeDouble(b, y); }
    return toHexLocal(b);
}

static uint8_t fromHexChar(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

static std::vector<uint8_t> hexToBytes(const std::string& h) {
    std::vector<uint8_t> raw(h.size() / 2);
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<uint8_t>((fromHexChar(h[i*2]) << 4) | fromHexChar(h[i*2+1]));
    return raw;
}

static std::string mergeLinestrings(const std::vector<std::string>& hexes) {
    if (hexes.empty()) return "";

    std::vector<std::vector<uint8_t>> parts;
    for (auto& h : hexes) {
        if (h.size() < 10) continue;
        auto byte = [&](size_t i) -> uint8_t {
            return static_cast<uint8_t>((fromHexChar(h[i*2]) << 4) | fromHexChar(h[i*2+1]));
        };
        uint32_t gtype = byte(1) | (byte(2)<<8) | (byte(3)<<16) | (byte(4)<<24);
        gtype &= 0xFFFF;
        if (gtype != 2 && gtype != 5) continue; // skip polygons

        parts.push_back(hexToBytes(h));
    }
    if (parts.empty()) return "";

    std::vector<uint8_t> b;
    b.push_back(1);
    writeLE32(b, 0x20000005); // MultiLineString with SRID
    writeLE32(b, static_cast<uint32_t>(g_srid));
    writeLE32(b, static_cast<uint32_t>(parts.size()));
    for (auto& p : parts)
        b.insert(b.end(), p.begin(), p.end());
    return toHexLocal(b);
}

// ---- RegionalDeltaApplier ----

RegionalDeltaApplier::RegionalDeltaApplier(RegionalNodeMap& node_map, NavDB& db,
                                            const std::vector<RegionIndex::Entry>& regions)
    : node_map_(node_map), db_(db), regions_(regions) {}

std::optional<std::pair<double,double>> RegionalDeltaApplier::resolve(int64_t id) const {
    if (auto pt = node_map_.select(id)) return pt;
    if (auto it = overlay_.find(id); it != overlay_.end()) return it->second;
    if (auto it = pending_.find(id); it != pending_.end()) {
        last_pending_hits_.push_back(id);
        return it->second;
    }
    return std::nullopt;
}

bool RegionalDeltaApplier::nodeInRegion(double lon_m, double lat_m) const {
    WkbDecode::GeomVariant pt = RegionIndex::Point{lon_m, lat_m};
    std::vector<std::pair<RegionIndex::Box, size_t>> scratch;
    for (const auto& region : regions_)
        if (RegionIndex::matches(region, pt, scratch)) return true;
    return false;
}

bool RegionalDeltaApplier::inRegion(const std::string& wkb_hex) const {
    if (wkb_hex.empty()) return false;
    std::vector<uint8_t> bytes = hexToBytes(wkb_hex);
    auto decoded = WkbDecode::decode(bytes.data(), bytes.size());
    if (!decoded) return false;
    std::vector<std::pair<RegionIndex::Box, size_t>> scratch;
    for (const auto& region : regions_)
        if (RegionIndex::matches(region, decoded->geom, scratch)) return true;
    return false;
}

// ---- geometry helpers ----

std::string RegionalDeltaApplier::buildWayGeom(const WayEntry& w, bool& is_closed) {
    last_pending_hits_.clear();
    std::vector<std::pair<double,double>> coords;
    for (int64_t ref : w.node_refs) {
        auto pt = resolve(ref);
        if (pt) coords.push_back(*pt);
    }
    if (coords.size() < 2) { is_closed = false; return ""; }
    is_closed = (coords.front() == coords.back() && coords.size() >= 4);
    return buildLineWKB(coords, is_closed);
}

std::string RegionalDeltaApplier::buildRelationGeom(const RelationEntry& r) {
    std::vector<std::string> geoms;
    for (auto& m : r.way_members) {
        std::string g = db_.getWay(m.id);
        if (!g.empty()) geoms.push_back(std::move(g));
    }
    return mergeLinestrings(geoms);
}

// ---- apply dispatcher ----

void RegionalDeltaApplier::apply(OSCChange&& change) {
    std::visit([&](auto&& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, NodeChange>) {
            switch (c.type) {
                case ChangeType::Create: createNode(c.node); ++created_; break;
                case ChangeType::Modify: modifyNode(c.node); ++modified_; break;
                case ChangeType::Delete: deleteNode(c.node.id); ++deleted_; break;
            }
        } else if constexpr (std::is_same_v<T, WayChange>) {
            switch (c.type) {
                case ChangeType::Create: upsertWay(c.way); ++created_; break;
                case ChangeType::Modify: upsertWay(c.way); ++modified_; break;
                case ChangeType::Delete: deleteWay(c.way.id); ++deleted_; break;
            }
        } else if constexpr (std::is_same_v<T, RelationChange>) {
            switch (c.type) {
                case ChangeType::Create: upsertRelation(c.relation); ++created_; break;
                case ChangeType::Modify: upsertRelation(c.relation); ++modified_; break;
                case ChangeType::Delete: deleteRelation(c.relation.id); ++deleted_; break;
            }
        }
    }, std::move(change));
}

void RegionalDeltaApplier::flush() {
    // End of one diff (see IDeltaApplier::flush's contract) -- discard
    // whatever's left in pending_: nodes that arrived this diff but weren't
    // referenced by any in-region way/area/road (already-promoted entries
    // moved to overlay_ in upsertWay, so this never discards anything
    // actually needed by this region).
    pending_.clear();
    db_.finalize_ways();
    db_.finalize_roads();
    db_.finalize_relations();
}

// ---- node operations ----

void RegionalDeltaApplier::createNode(NodeEntry& n) {
    auto [mx, my] = toMercator(n.lon, n.lat);
    n.lon_m = mx; n.lat_m = my;
    n.geog_wkb_hex = pointWKB(n.lon, n.lat);

    // Buffered unconditionally (mirrors DeltaApplier's unconditional
    // osmmap_.update()) so an in-region way created later in this same
    // diff can resolve it immediately -- see resolve()/upsertWay.
    pending_[n.id] = {n.lon_m, n.lat_m};

    if (n.tags.empty()) return;
    // Matches regional_table_export's own `nodes` table criterion: only a
    // tagged node that's itself geometrically in-region gets a DB row —
    // this is a separate concern from pure coordinate resolution above.
    if (!nodeInRegion(n.lon_m, n.lat_m)) return;
    db_.updateNode(n.id, n.name, n.lon_m, n.lat_m, n.tags, n.geog_wkb_hex);
}

void RegionalDeltaApplier::modifyNode(NodeEntry& n) {
    auto [mx, my] = toMercator(n.lon, n.lat);
    n.lon_m = mx; n.lat_m = my;
    n.geog_wkb_hex = pointWKB(n.lon, n.lat);

    pending_[n.id] = {n.lon_m, n.lat_m};

    if (nodeInRegion(n.lon_m, n.lat_m)) {
        db_.updateNode(n.id, n.name, n.lon_m, n.lat_m, n.tags, n.geog_wkb_hex);
    } else {
        // Possibly moved out of the region since the last edit (or never
        // had a row) -- deleteEntity is a safe no-op either way, same
        // border-crossing handling as upsertWay/upsertRelation below.
        db_.deleteEntity(n.id, "node");
    }
}

void RegionalDeltaApplier::deleteNode(int64_t id) {
    pending_.erase(id);
    overlay_.erase(id);
    db_.deleteEntity(id, "node");
}

// ---- way/area operations ----

void RegionalDeltaApplier::upsertWay(WayEntry& w) {
    bool is_closed = false;
    std::string geom = buildWayGeom(w, is_closed);
    if (geom.empty()) return;

    if (!inRegion(geom)) {
        // Not (or no longer) in this region -- clear any stale row from
        // before it edited its way across the boundary. last_pending_hits_
        // is deliberately NOT promoted here: those nodes aren't actually
        // needed by this region's store.
        db_.deleteEntity(w.id, "way");
        db_.deleteEntity(w.id, "area");
        return;
    }

    // In-region: promote whichever nodes this way's geometry needed out of
    // pending_ into the persistent overlay_ (see class comment) before
    // pending_ gets cleared at the end of this diff.
    for (int64_t id : last_pending_hits_) {
        if (auto it = pending_.find(id); it != pending_.end()) {
            overlay_.emplace(id, it->second);
            pending_.erase(it);
        }
    }

    if (is_closed) {
        db_.deleteEntity(w.id, "way");
        db_.updateArea(w.id, w.name, w.tags, geom);
    } else {
        db_.deleteEntity(w.id, "area");
        db_.updateWay(w.id, w.name, w.tags, geom);
    }
}

void RegionalDeltaApplier::deleteWay(int64_t id) {
    db_.deleteEntity(id, "way");
    db_.deleteEntity(id, "area");
}

// ---- relation operations ----

void RegionalDeltaApplier::upsertRelation(RelationEntry& r) {
    std::string geom = buildRelationGeom(r);

    if (!inRegion(geom)) {
        db_.deleteEntity(r.id, "relation");
        db_.deleteEntity(r.id, "road");
        return;
    }

    db_.updateRelation(r.id, r.name, r.tags, geom);

    auto route_it   = r.tags.find("route");
    auto highway_it = r.tags.find("highway");
    bool is_road = (route_it   != r.tags.end() && route_it->second == "road") ||
                   (highway_it != r.tags.end());
    if (is_road)
        db_.updateRoad(r.id, r.name, r.tags, geom);
    else
        db_.deleteEntity(r.id, "road");
}

void RegionalDeltaApplier::deleteRelation(int64_t id) {
    db_.deleteEntity(id, "relation");
    db_.deleteEntity(id, "road");
}
