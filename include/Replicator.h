#pragma once
#include "DeltaApplier.h"
#include <string>
#include <cstdint>

enum class ReplicationGranularity { Minute, Hour, Day };

// Downloads and applies OSM replication files in sequence.
// Tracks the last applied sequence number in the database.

class Replicator {
public:
    // server/user/database/password are only needed to reload OurAirports /
    // FAA obstacle data when checkExternalData() detects an upstream update.
    Replicator(DeltaApplier& applier, NavDB& db,
               ReplicationGranularity granularity,
               std::string server, std::string user,
               std::string database, std::string password = "");

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
    std::string            server_, user_, database_, password_;

    std::string baseUrl() const;
    std::string sequenceToPath(int64_t seq) const;
    bool        downloadAndApply(int64_t seq);
    int64_t     remoteSequence();

    // Checks whether the OurAirports / FAA obstacle datasets have been
    // refreshed upstream (via HTTP Last-Modified) and reloads whichever
    // has changed. Called periodically from poll(), independent of OSM
    // replication cadence.
    void checkExternalData();
};
