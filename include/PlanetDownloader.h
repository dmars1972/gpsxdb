#pragma once
#include <string>

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
