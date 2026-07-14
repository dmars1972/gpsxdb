#pragma once
#include <string>
#include <cstdint>

// Downloads the latest OSM planet PBF file from
// https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf to dest_path.
//
// Resumable: if dest_path already exists (e.g. from a previous interrupted
// run), the download picks up from where it left off via curl's -C -
// rather than restarting from scratch — meaningful given the file is on
// the order of 100GB and the transfer can take hours.
//
// Verifies the completed download against the upstream
// planet-latest.osm.pbf.md5 checksum file. Returns false — leaving
// dest_path in place so a re-run can resume/retry — on insufficient disk
// space, a network/curl failure, or a checksum mismatch. If the checksum
// file itself can't be fetched, verification is skipped (logged as a
// warning) rather than treated as failure, since the download itself may
// still be perfectly good.
//
// Checks free disk space at dest_path's directory against the remote
// file's Content-Length before starting, refusing to begin (rather than
// failing partway through a multi-hour transfer) if there isn't room —
// accounting for bytes already present if resuming a partial download.
bool downloadLatestPlanet(const std::string& dest_path, bool verbose = true);

// Estimates the OSM *minutely* replication sequence number corresponding
// to planet-latest.osm.pbf's actual data cutoff, for auto-seeding the
// poll service's starting point after a fresh -I --download-planet
// import — without this, poll mode has no starting sequence at all and a
// human has to figure out and pass -Q manually every time.
//
// There's no per-dump state.txt published at a predictable path for
// planet-latest.osm.pbf (checked: /pbf/state.txt 404s), so this
// interpolates instead: fetch the planet file's remote Last-Modified time
// (its actual publish/data-cutoff moment) and the CURRENT replication tip
// (https://.../replication/minute/state.txt — sequence + timestamp, ~1
// sequence per minute), then linearly extrapolate backwards from the tip.
// A multi-day safety margin is subtracted so the estimate lands a bit
// EARLIER than the true cutoff rather than later — replaying a few
// already-included changes is harmless (idempotent upserts), silently
// skipping real ones is not.
//
// Returns -1 on any failure (network, parse error) — caller should treat
// that as "couldn't auto-seed, leave it for a human to set via -Q".
int64_t estimatePlanetReplicationSequence();
