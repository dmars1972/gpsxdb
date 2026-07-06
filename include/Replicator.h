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

    // Recomputes/reloads WMM declination on a fixed ~3-month wall-clock
    // cadence (tracked in external_data_state like checkExternalData's
    // sources, keyed by "wmm") — unlike airports/FAA there's no upstream
    // "has this changed" check to make (NOAA only publishes a new
    // coefficient model every ~5 years), but the computed declination
    // itself drifts via secular variation as time passes, so it's worth
    // periodically recomputing with the current date regardless. Called
    // alongside checkExternalData() from poll().
    void checkWMMRefresh();

    // Reloads FAA Class/Special Use Airspace + OpenAIP international
    // airspace on a fixed 1-month wall-clock cadence (tracked like
    // checkWMMRefresh, keyed "airspace") — simpler than matching FAA's
    // exact ~8-week NASR/SUA publish cycle, and still keeps data
    // reasonably current. Same reasoning as checkWMMRefresh for using a
    // fixed interval rather than an upstream Last-Modified check:
    // OpenAIP's paginated API has no single Last-Modified to check, and
    // the FAA ArcGIS export API doesn't reliably expose one either.
    void checkAirspaceRefresh();
};
