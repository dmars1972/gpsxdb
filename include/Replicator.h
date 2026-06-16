#pragma once
#include "DeltaApplier.h"
#include <string>
#include <cstdint>

enum class ReplicationGranularity { Minute, Hour, Day };

// Downloads and applies OSM replication files in sequence.
// Tracks the last applied sequence number in the database.

class Replicator {
public:
    Replicator(DeltaApplier& applier, NavDB& db,
               ReplicationGranularity granularity);

    // Apply a single local .osc/.osc.gz file
    void applyFile(const std::string& path);

    // Poll replication server, applying all pending sequences.
    // Runs until interrupted (SIGINT).
    void poll(int interval_seconds = 60);

    // Get/set the current sequence number stored in DB
    int64_t getSequence();
    void    setSequence(int64_t seq);

private:
    DeltaApplier&          applier_;
    NavDB&                 db_;
    ReplicationGranularity granularity_;

    std::string baseUrl() const;
    std::string sequenceToPath(int64_t seq) const;
    bool        downloadAndApply(int64_t seq);
    int64_t     remoteSequence();
};
