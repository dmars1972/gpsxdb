#pragma once
#include "IDeltaApplier.h"
#include "NavDB.h"
#include <string>
#include <cstdint>

enum class ReplicationGranularity { Minute, Hour, Day };

// Downloads and applies OSM replication files in sequence.
// Tracks the last applied sequence number in the database.

class Replicator {
public:
    // server/user/database are only needed to reload OurAirports / FAA
    // obstacle data when checkExternalData() detects an upstream update.
    // applier is IDeltaApplier&, not DeltaApplier&, so this same class
    // drives both the global poll (DeltaApplier) and the region-aware poll
    // (RegionalDeltaApplier) — everything here (sequence tracking,
    // download, external-data refresh cadence) is identical either way.
    Replicator(IDeltaApplier& applier, NavDB& db,
               ReplicationGranularity granularity,
               std::string server, std::string user,
               std::string database);

    // Apply a single local .osc/.osc.gz file
    void applyFile(const std::string& path);

    // Poll replication server, applying all pending sequences.
    // Runs until interrupted (SIGINT).
    void poll(int interval_seconds = 60);

    // Get/set the current sequence number stored in DB
    int64_t getSequence();
    void    setSequence(int64_t seq);

private:
    IDeltaApplier&         applier_;
    NavDB&                 db_;
    ReplicationGranularity granularity_;
    std::string            server_, user_, database_;

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

// Seeds fresh external_data_state checkpoints right after -I's own phase
// loaders (AirportsLoader/FAAObstacleLoader/WMMLoader/AirspaceLoader) have
// freshly loaded their data. Without these, external_data_state has no
// record of any of them at all -- only checkExternalData()/
// checkWMMRefresh()/checkAirspaceRefresh() themselves ever write these
// keys -- so poll's first startup after any -I treats "no record" as
// "definitely stale" and redundantly reloads all four sources before it
// ever reaches real OSM replication catch-up. Safe to call even after a
// partial load failure: worst case is one extra redundant reload on the
// next poll startup, not a correctness issue. Free functions rather than
// Replicator methods since they don't need an IDeltaApplier/poll context,
// just a DB handle -- callable directly from osm_import's -I path.
void seedExternalDataTimestamp(NavDB& db, const std::string& source_name); // "airports" or "faa_obstacles"
void seedWMMTimestamp(NavDB& db);
void seedAirspaceTimestamp(NavDB& db);
