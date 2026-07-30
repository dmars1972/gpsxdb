#pragma once
#include "IDeltaApplier.h"
#include "OSCReader.h"
#include "RegionalNodeMap.h"
#include "RegionIndex.h"
#include "NavDB.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <utility>

// Region-scoped counterpart to DeltaApplier: applies OSC changes against a
// single region's local database and node coordinate store, without ever
// depending on the master (full-planet) database or nodes.dat. See
// /home/daniel/.claude/plans/elegant-skipping-parasol.md ("Phase B") for
// the full design rationale -- summary of what differs from DeltaApplier:
//
//  - Node coordinate resolution uses RegionalNodeMap (read-only, mmap'd —
//    see that header; already widened at export time to include every
//    vertex of an in-region way/area/road, not just nodes whose own
//    coordinate falls inside the region polygon) instead of OSMMMap, with
//    an in-memory overlay_ for nodes referenced by an in-region entity but
//    not present in the widened export (e.g. a node created after the last
//    export and immediately spliced into an in-region way). overlay_ is
//    NOT persisted across process restarts -- self-heals at the next
//    quarterly rebuild (see below) or the next time the same node is
//    referenced again.
//
//  - Same-diff promotion: every incoming node create/modify is buffered in
//    pending_ (mirroring DeltaApplier's unconditional osmmap_.update()) but
//    NOT written into overlay_ directly. Only once a way/area/road that
//    references it turns out to be in-region does it get promoted from
//    pending_ into overlay_ (see upsertWay). This is what makes "a mapper
//    adds a new node while extending an existing in-region way" -- the
//    common real-world edit pattern -- resolve correctly without ever
//    touching the master DB, while bounding memory to entries actually
//    used by this region (unreferenced pending_ entries are discarded at
//    the end of each diff in flush(), not accumulated indefinitely, which
//    would otherwise defeat the whole point of a regional node store).
//
//  - Residual gap (a pre-existing, out-of-region node spliced into an
//    in-region way in a DIFFERENT diff than its own last edit) is left
//    exactly as DeltaApplier already handles a missing node ref: dropped
//    from the linestring, no throw (see buildWayGeom). No live DB fallback
//    -- self-heals at the next quarterly full-bundle rebuild.
//
//  - Region membership filtering: every way/area/road/relation change is
//    tested against this install's own region polygon before deciding to
//    upsert or delete, matching regional_db_export --big-tables-only's own
//    inclusion criterion -- including deleting a previously-in-region entity that
//    edited its way across the region boundary. Node DB rows (as opposed
//    to pure coordinate resolution) follow the same in-region-only rule,
//    independent of pending_/overlay_ promotion.
class RegionalDeltaApplier : public IDeltaApplier {
public:
    // `regions` is every region installed in the target database (see
    // NavDB::loadInstalledRegions()) -- usually one, but a database can have
    // more than one region installed (regional_install.cpp is explicitly
    // safe to run multiple times with different/overlapping regions), in
    // which case a single poll process covers all of them: membership is
    // tested against ANY of them, not just the first.
    RegionalDeltaApplier(RegionalNodeMap& node_map, NavDB& db,
                          const std::vector<RegionIndex::Entry>& regions);

    void apply(OSCChange&& change) override;
    void flush() override;

    int64_t created()  const override { return created_; }
    int64_t modified() const override { return modified_; }
    int64_t deleted()  const override { return deleted_; }

private:
    RegionalNodeMap&        node_map_;
    NavDB&                  db_;
    const std::vector<RegionIndex::Entry>& regions_;

    int64_t created_  = 0;
    int64_t modified_ = 0;
    int64_t deleted_  = 0;

    // Persists for the lifetime of this process (not across restarts —
    // see top-of-file comment). Only ever grows via promotion from
    // pending_ in upsertWay, never written to directly by node events.
    std::unordered_map<int64_t, std::pair<double,double>> overlay_;

    // Every incoming node create/modify lands here first, discarded at the
    // end of each diff (flush()) unless promoted into overlay_.
    std::unordered_map<int64_t, std::pair<double,double>> pending_;

    // Populated by resolve() with every id it served out of pending_
    // (rather than node_map_/overlay_) during the most recent buildWayGeom
    // call — read and cleared by upsertWay to decide what to promote.
    mutable std::vector<int64_t> last_pending_hits_;

    std::optional<std::pair<double,double>> resolve(int64_t id) const;

    // ---- node operations ----
    void createNode(NodeEntry& n);
    void modifyNode(NodeEntry& n);
    void deleteNode(int64_t id);
    bool nodeInRegion(double lon_m, double lat_m) const;

    // ---- way/area operations ----
    void upsertWay(WayEntry& w);
    void deleteWay(int64_t id);

    // ---- relation operations ----
    void upsertRelation(RelationEntry& r);
    void deleteRelation(int64_t id);

    // ---- helpers ----
    std::string buildWayGeom(const WayEntry& w, bool& is_closed);
    std::string buildRelationGeom(const RelationEntry& r);

    // Decodes a WKB-hex geometry (this class's own build output, or a way's
    // stored geometry fetched via db_.getWay()) and tests it against every
    // entry in regions_, true on the first match. A decode failure is
    // treated as not-in-region (conservative — matches WkbDecode's own
    // "never silently treat as a match" contract).
    bool inRegion(const std::string& wkb_hex) const;
};
