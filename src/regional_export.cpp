// Exports the regional node coordinate slices used by the regional
// installer: a multithreaded pass over the master nodes.dat (each thread
// scans a disjoint id range), testing every populated (id, lon_m, lat_m)
// record against every region's real natural-boundary polygon
// (data/regions/<name>.wkt). Each thread writes matches to a private
// per-region temp file (raw records, no header, ascending id order within
// that thread's range); after all threads finish, the main thread
// concatenates the N threads' temp files (already globally ascending,
// since thread id ranges are disjoint and increasing) into each region's
// final RegionalNodeMap file.
//
// Usage: regional_export -f <nodes.dat path> -n <max_id> --out-dir <dir>
//                         [--regions-dir <dir>] [--regions name1,name2,...]
//                         [--threads N] [-v]
//
// The actual spatial matching (bbox pre-filter, rtree of the region's
// constituent polygon envelopes, exact intersects() test against only the
// rtree's candidates) is RegionIndex (include/RegionIndex.h), shared with
// regional_table_export -- see that header for the covered_by-vs-
// intersects() correctness note. Region polygons are projected to
// Mercator meters once at startup (matching nodes.dat's on-disk units —
// production data is loaded with use_mercator=true, i.e. no -L/--wgs84
// flag), same forward projection as node coordinates, so no per-record
// inverse projection is needed either way.
//
// A single-threaded pass over all ~10.7B populated nodes with real polygon
// testing (vs. the old plain bbox test) was measured to take multiple
// hours for just 2 of 10 regions -- large/dense regions like north_america
// have bboxes covering a huge share of all nodes, so the "cheap" bbox
// pre-filter alone doesn't reject enough to keep the expensive exact test
// rare. Threading the scan (embarrassingly parallel -- each node is
// classified independently) is the fix; --threads defaults to the
// machine's hardware concurrency.
//
// Per-node polygon membership alone is NOT sufficient for a region's node
// store: a way/area/road matched via intersects() (not covered_by()) can
// legitimately cross a region's border, referencing vertices whose own
// coordinate falls just outside the polygon -- a region-aware poll process
// needs those too, to resolve such a way's full geometry locally. This
// tool folds in regional_table_export.cpp's <region>/extra_vertices.bin
// (--extra-vertices-dir, defaulting to --out-dir, i.e. the same directory
// regional_table_export was pointed at) -- exact-match coordinate lookups
// against a hash built from those files, ORed into the same per-node
// region bitmask as the polygon test. Coordinates match bit-for-bit (no
// tolerance/rounding): both regional_table_export's decoded WKB vertices
// and nodes.dat's own records trace back to the same resolved doubles
// written by GeoUtils::buildWayGeom/DeltaApplier at import time, never
// reprojected in between. regional_table_export MUST run first now (the
// reverse of this pipeline's original order) so extra_vertices.bin exists
// before this scan starts.
#include "OSMMMap.h"
#include "RegionalNodeMap.h"
#include "Regions.h"
#include "RegionPolygons.h"
#include "RegionIndex.h"
#include "WkbDecode.h"
#include "GeoUtils.h"

#include <boost/geometry.hpp>

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

// GeoUtils.h declares this extern — defined once per executable. Unused
// here (regional_export never builds WKB geometry) but required to link.
int g_srid = 3857;

namespace {

// Matches RegionalNodeMap's 24-byte on-disk record layout exactly (int64_t
// id + double lon_m + double lat_m, naturally aligned, no padding) -- each
// worker thread appends raw records here (no header), then the main thread
// streams them into the real RegionalNodeMap::Writer after all threads
// finish (see the merge loop in main()).
struct RawRecord { int64_t id; double lon_m; double lat_m; };

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Projects a WGS84-degree polygon to Mercator meters in place, using the
// same forward projection as node coordinates (see top-of-file comment).
void projectToMercator(RegionPolygons::MultiPolygon& mp) {
    boost::geometry::for_each_point(mp, [](RegionPolygons::Point& p) {
        auto [x, y] = toMercator(p.x(), p.y());
        p.x(x);
        p.y(y);
    });
}

// Exact-bit-pattern key for a (lon_m, lat_m) pair -- see top-of-file
// comment on why bit-exact (not tolerance-based) matching is correct here.
struct CoordKey {
    int64_t lon_bits, lat_bits;
    bool operator==(const CoordKey& o) const { return lon_bits == o.lon_bits && lat_bits == o.lat_bits; }
};
struct CoordKeyHash {
    size_t operator()(const CoordKey& k) const {
        size_t h1 = std::hash<int64_t>()(k.lon_bits);
        size_t h2 = std::hash<int64_t>()(k.lat_bits);
        return h1 ^ (h2 + 0x9E3779B97F4A7C15ULL + (h1 << 6) + (h1 >> 2));
    }
};
CoordKey makeCoordKey(double lon_m, double lat_m) {
    CoordKey k;
    std::memcpy(&k.lon_bits, &lon_m, sizeof(double));
    std::memcpy(&k.lat_bits, &lat_m, sizeof(double));
    return k;
}

// Loads every region's <extra_vertices_dir>/<region>/extra_vertices.bin
// (regional_table_export.cpp's output -- see that file's top-of-file
// comment) into one combined hash, ORing each region's bit into whichever
// coordinates it contributed. A region with no such file (e.g. it borders
// no other region, or regional_table_export hasn't been re-run for it yet)
// contributes nothing -- not an error.
std::unordered_map<CoordKey, uint32_t, CoordKeyHash> loadExtraVertices(
        const std::string& extra_vertices_dir, const std::vector<RegionIndex::Entry>& regions, bool verbose) {
    std::unordered_map<CoordKey, uint32_t, CoordKeyHash> extra;
    for (size_t ri = 0; ri < regions.size(); ++ri) {
        std::string path = extra_vertices_dir + "/" + regions[ri].name + "/extra_vertices.bin";
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        double buf[2];
        uint64_t count = 0;
        while (fread(buf, sizeof(buf), 1, f) == 1) {
            extra[makeCoordKey(buf[0], buf[1])] |= (1u << ri);
            ++count;
        }
        fclose(f);
        if (verbose) std::cout << "[regional_export] " << regions[ri].name << ": " << count
                                << " extra-vertex candidate(s) loaded from " << path << "\n";
    }
    return extra;
}

} // namespace

int main(int argc, char** argv) {
    std::string nodes_file = "nodes.dat";
    std::string out_dir = ".";
    std::string regions_dir = "data/regions";
    std::string extra_vertices_dir;  // empty = not given, defaults to out_dir below
    bool extra_vertices_dir_given = false;
    int64_t max_id = 20'000'000'000;
    std::vector<std::string> region_filter;  // empty = all
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    bool verbose = false;
    bool no_extra_vertices = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-f" || arg == "--nodes-file") && i + 1 < argc) nodes_file = argv[++i];
        else if ((arg == "-n" || arg == "--max-id") && i + 1 < argc) max_id = std::stoll(argv[++i]);
        else if ((arg == "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions-dir") && i + 1 < argc) regions_dir = argv[++i];
        else if ((arg == "--extra-vertices-dir") && i + 1 < argc) { extra_vertices_dir = argv[++i]; extra_vertices_dir_given = true; }
        else if (arg == "--no-extra-vertices") no_extra_vertices = true;
        else if ((arg == "--threads") && i + 1 < argc) threads = std::stoi(argv[++i]);
        else if ((arg == "--regions") && i + 1 < argc) {
            std::string v = argv[++i];
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_export -f <nodes.dat> -n <max_id> --out-dir <dir>\n"
                         "                        [--regions-dir <dir>] [--regions name1,name2,...]\n"
                         "                        [--extra-vertices-dir <dir>] [--no-extra-vertices]\n"
                         "                        [--threads N] [-v]\n"
                         "\n"
                         "Multithreaded pass over nodes.dat producing one\n"
                         "<out-dir>/<region>.nodes.dat per region (see include/Regions.h\n"
                         "for the region list, default --regions-dir data/regions for the\n"
                         "polygon boundaries). -n must match the max-id nodes.dat was\n"
                         "created with (see nodes.dat's companion .bmp size). --threads\n"
                         "defaults to the machine's hardware concurrency.\n"
                         "\n"
                         "Also widens each region's node file with border-crossing way/area/\n"
                         "road vertices from regional_table_export.cpp's <region>/\n"
                         "extra_vertices.bin, read from --extra-vertices-dir (default: same as\n"
                         "--out-dir). regional_table_export must have already been run against\n"
                         "that directory. --no-extra-vertices restores the old polygon-only\n"
                         "behavior (e.g. for a byte-for-byte regression diff against a\n"
                         "pre-widening build).\n";
            std::cout.flush();
            _exit(0);  // avoid pqxx/PROJ static-destructor double-free on normal return
        }
    }
    if (threads < 1) threads = 1;
    if (!extra_vertices_dir_given) extra_vertices_dir = out_dir;

    if (!dirExists(out_dir)) {
        std::cerr << "Error: --out-dir " << out_dir << " does not exist\n";
        _exit(1);
    }

    auto polygons = RegionPolygons::load(regions_dir, region_filter);  // throws on missing/bad WKT

    std::vector<RegionIndex::Entry> regions;
    // wgs84_bboxes[i] pairs with regions[i] -- kept separately since
    // RegionalNodeMap::Writer needs the WGS84-degree bbox for its own file
    // header, while RegionIndex::Entry's bbox is in whatever coordinate
    // system the caller projected into (Mercator meters here).
    std::vector<RegionalNodeMap::Bbox> wgs84_bboxes;
    for (const auto& r : allExportRegions()) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), r.name) == region_filter.end())
            continue;

        RegionPolygons::MultiPolygon polygon_mercator = polygons.at(r.name);
        projectToMercator(polygon_mercator);
        auto [min_x, min_y] = toMercator(r.min_lon, r.min_lat);
        auto [max_x, max_y] = toMercator(r.max_lon, r.max_lat);

        wgs84_bboxes.push_back({r.min_lon, r.min_lat, r.max_lon, r.max_lat});
        regions.push_back(RegionIndex::build(r.name, min_x, min_y, max_x, max_y,
                                              std::move(polygon_mercator)));
    }

    if (regions.empty()) {
        std::cerr << "Error: no matching regions (check --regions names against include/Regions.h)\n";
        _exit(1);
    }

    if (verbose) {
        std::cout << "[regional_export] scanning " << nodes_file << " (max_id=" << max_id
                  << ") for " << regions.size() << " region(s) with " << threads << " thread(s)\n";
    }

    // Read-only for the rest of this run once built -- safe to query
    // concurrently from every worker thread with no locking.
    std::unordered_map<CoordKey, uint32_t, CoordKeyHash> extra_vertices;
    if (!no_extra_vertices) {
        extra_vertices = loadExtraVertices(extra_vertices_dir, regions, verbose);
        if (verbose) std::cout << "[regional_export] " << extra_vertices.size()
                                << " distinct extra-vertex coordinate(s) total\n";
    }

    // open_shards_for_write=false: read-only access to the merged file, no
    // shard files touched — matches how delta/poll mode opens nodes.dat.
    OSMMMap osmmap(nodes_file, max_id, /*num_shards=*/1, ".", /*open_shards_for_write=*/false);

    std::atomic<uint64_t> scanned{0};
    std::atomic<uint64_t> extra_added{0};
    auto start = std::chrono::steady_clock::now();

    // Per-thread temp raw-record files, one per (thread, region).
    std::vector<std::vector<std::string>> thread_region_paths(
        threads, std::vector<std::string>(regions.size()));
    for (int ti = 0; ti < threads; ++ti)
        for (size_t ri = 0; ri < regions.size(); ++ri)
            thread_region_paths[ti][ri] =
                out_dir + "/tmp_" + regions[ri].name + "_" + std::to_string(ti) + ".raw";

    size_t total_bytes = osmmap.merged_bm_size_;
    size_t chunk = (total_bytes + threads - 1) / static_cast<size_t>(threads);

    auto worker = [&](int ti) {
        size_t byte_start = static_cast<size_t>(ti) * chunk;
        size_t byte_end = std::min(byte_start + chunk, total_bytes);
        if (byte_start >= byte_end) return;

        std::vector<FILE*> out(regions.size());
        for (size_t ri = 0; ri < regions.size(); ++ri)
            out[ri] = fopen(thread_region_paths[ti][ri].c_str(), "wb");

        std::vector<std::pair<RegionIndex::Box, size_t>> candidates;  // reused scratch
        uint64_t local_scanned = 0;

        uint64_t local_extra_added = 0;

        osmmap.forEachPopulatedRange(byte_start, byte_end, [&](int64_t id, double lon_m, double lat_m) {
            ++local_scanned;
            WkbDecode::GeomVariant pt = RegionIndex::Point{lon_m, lat_m};
            uint32_t polymask = 0;
            for (size_t ri = 0; ri < regions.size(); ++ri) {
                if (!RegionIndex::matches(regions[ri], pt, candidates)) continue;

                polymask |= (1u << ri);
                RawRecord rec{id, lon_m, lat_m};
                if (out[ri]) fwrite(&rec, sizeof(rec), 1, out[ri]);
            }

            // Fold in border-crossing way/area/road vertices (see
            // top-of-file comment): only for regions this node's own
            // coordinate did NOT already match via the polygon test above,
            // to avoid ever writing a duplicate (id, region) record.
            if (!extra_vertices.empty()) {
                auto it = extra_vertices.find(makeCoordKey(lon_m, lat_m));
                if (it != extra_vertices.end()) {
                    uint32_t extra_only = it->second & ~polymask;
                    if (extra_only != 0) {
                        RawRecord rec{id, lon_m, lat_m};
                        for (size_t ri = 0; ri < regions.size(); ++ri) {
                            if (!(extra_only & (1u << ri))) continue;
                            if (out[ri]) fwrite(&rec, sizeof(rec), 1, out[ri]);
                            ++local_extra_added;
                        }
                    }
                }
            }

            if ((local_scanned & 0xFFFFF) == 0) {  // every ~1M, cheap batched progress update
                auto total = scanned.fetch_add(1'048'576, std::memory_order_relaxed) + 1'048'576;
                if (verbose && (total / 100'000'000) != ((total - 1'048'576) / 100'000'000)) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count();
                    std::cout << "[regional_export] ~" << total << " nodes scanned (" << elapsed << "s)\n";
                }
                local_scanned = 0;
            }
        });
        scanned.fetch_add(local_scanned, std::memory_order_relaxed);
        extra_added.fetch_add(local_extra_added, std::memory_order_relaxed);

        for (auto* f : out) if (f) fclose(f);
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int ti = 0; ti < threads; ++ti) pool.emplace_back(worker, ti);
    for (auto& t : pool) t.join();

    // Merge: for each region, stream all N threads' temp files (in thread
    // order, which is also ascending id order since thread byte ranges are
    // disjoint and increasing) into the final RegionalNodeMap file.
    for (size_t ri = 0; ri < regions.size(); ++ri) {
        const auto& g = regions[ri];
        std::string path = out_dir + "/" + g.name + ".nodes.dat";
        RegionalNodeMap::Writer writer(path, g.name, wgs84_bboxes[ri]);
        uint64_t matched = 0;
        for (int ti = 0; ti < threads; ++ti) {
            const std::string& tmp_path = thread_region_paths[ti][ri];
            FILE* f = fopen(tmp_path.c_str(), "rb");
            if (f) {
                RawRecord rec;
                while (fread(&rec, sizeof(rec), 1, f) == 1) {
                    writer.append(rec.id, rec.lon_m, rec.lat_m);
                    ++matched;
                }
                fclose(f);
            }
            unlink(tmp_path.c_str());
        }
        writer.finalize();
        if (verbose) std::cout << "[regional_export] " << g.name << ": " << matched << " node(s)\n";
    }

    std::cout << "[regional_export] done — ~" << scanned.load() << " nodes scanned, "
              << extra_added.load() << " extra-vertex (id,region) record(s) added, "
              << regions.size() << " region file(s) written to " << out_dir << "\n";
    std::cout.flush();
    _exit(0);  // avoid pqxx/PROJ static-destructor double-free on normal return
}
