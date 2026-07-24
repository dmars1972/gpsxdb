#pragma once
#include "OSCReader.h"
#include <cstdint>

// Common interface Replicator drives — implemented by DeltaApplier (global,
// full-planet OSMMMap-backed) and RegionalDeltaApplier (regional,
// RegionalNodeMap-backed). Replicator's sequence-tracking/download/
// external-data-refresh logic has nothing to do with which node coordinate
// store or DB-write filtering an applier uses, so it's written once here
// against this interface and shared by both the global and region-aware
// poll processes rather than duplicated.
class IDeltaApplier {
public:
    virtual ~IDeltaApplier() = default;

    virtual void apply(OSCChange&& change) = 0;

    // Called once per fully-processed diff/sequence (see Replicator::
    // applyFile) -- the natural "end of this diff" boundary. A regional
    // applier's same-diff node promotion (see RegionalDeltaApplier) relies
    // on this being called exactly once per diff, not per-change.
    virtual void flush() = 0;

    virtual int64_t created()  const = 0;
    virtual int64_t modified() const = 0;
    virtual int64_t deleted()  const = 0;
};
