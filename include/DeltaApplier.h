#pragma once
#include "OSCReader.h"
#include "OSMMMap.h"
#include "NavDB.h"
#include <string>
#include <mutex>

// Applies a parsed OSC change to the database and mmap node store.
// Single-threaded — delta files are small enough not to need parallelism.

class DeltaApplier {
public:
    DeltaApplier(OSMMMap& osmmap, NavDB& db);

    void apply(OSCChange&& change);

    // Flush any buffered writes
    void flush();

    // Stats
    int64_t created()  const { return created_; }
    int64_t modified() const { return modified_; }
    int64_t deleted()  const { return deleted_; }

private:
    OSMMMap& osmmap_;
    NavDB&   db_;

    int64_t created_  = 0;
    int64_t modified_ = 0;
    int64_t deleted_  = 0;

    // ---- node operations ----
    void createNode(NodeEntry& n);
    void modifyNode(NodeEntry& n);
    void deleteNode(int64_t id);

    // ---- way/area operations ----
    void createWay(WayEntry& w);
    void modifyWay(WayEntry& w);
    void deleteWay(int64_t id);

    // ---- relation operations ----
    void createRelation(RelationEntry& r);
    void modifyRelation(RelationEntry& r);
    void deleteRelation(int64_t id);

    // ---- helpers ----
    // Build geometry for a way using mmap node lookups
    std::string buildWayGeom(const WayEntry& w, bool& is_closed);

    // Build relation geometry from member ways
    std::string buildRelationGeom(const RelationEntry& r);
};
