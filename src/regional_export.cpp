// Exports the regional node coordinate slices used by the regional
// installer: a single sequential pass over the master nodes.dat, testing
// every populated (id, lon_m, lat_m) record against every region's bbox at
// once, writing matches straight into each region's RegionalNodeMap file.
//
// Usage: regional_export -f <nodes.dat path> -n <max_id> --out-dir <dir>
//                         [--regions name1,name2,...] [-v]
//
// Region bboxes are precomputed once in Mercator meters (matching nodes.dat's
// on-disk units — production data is loaded with use_mercator=true, i.e. no
// -L/--wgs84 flag) so no per-record inverse projection is needed.
#include "OSMMMap.h"
#include "RegionalNodeMap.h"
#include "Regions.h"
#include "GeoUtils.h"

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

// GeoUtils.h declares this extern — defined once per executable. Unused
// here (regional_export never builds WKB geometry) but required to link.
int g_srid = 3857;

namespace {

struct RegionTarget {
    std::string name;
    double min_x, min_y, max_x, max_y;  // Mercator meters
    RegionalNodeMap::Bbox wgs84_bbox;
    std::unique_ptr<RegionalNodeMap::Writer> writer;
    uint64_t matched = 0;
};

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

int main(int argc, char** argv) {
    std::string nodes_file = "nodes.dat";
    std::string out_dir = ".";
    int64_t max_id = 20'000'000'000;
    std::vector<std::string> region_filter;  // empty = all
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-f" || arg == "--nodes-file") && i + 1 < argc) nodes_file = argv[++i];
        else if ((arg == "-n" || arg == "--max-id") && i + 1 < argc) max_id = std::stoll(argv[++i]);
        else if ((arg == "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions") && i + 1 < argc) {
            std::string v = argv[++i];
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_export -f <nodes.dat> -n <max_id> --out-dir <dir>\n"
                         "                        [--regions name1,name2,...] [-v]\n"
                         "\n"
                         "Single sequential pass over nodes.dat producing one\n"
                         "<out-dir>/<region>.nodes.dat per region (see include/Regions.h\n"
                         "for the region list). -n must match the max-id nodes.dat was\n"
                         "created with (see nodes.dat's companion .bmp size).\n";
            _exit(0);  // avoid pqxx/PROJ static-destructor double-free on normal return
        }
    }

    if (!dirExists(out_dir)) {
        std::cerr << "Error: --out-dir " << out_dir << " does not exist\n";
        _exit(1);
    }

    std::vector<RegionTarget> targets;
    for (const auto& r : kGlobalRegions) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), r.name) == region_filter.end())
            continue;

        RegionTarget t;
        t.name = r.name;
        t.wgs84_bbox = {r.min_lon, r.min_lat, r.max_lon, r.max_lat};
        auto [min_x, min_y] = toMercator(r.min_lon, r.min_lat);
        auto [max_x, max_y] = toMercator(r.max_lon, r.max_lat);
        t.min_x = min_x; t.min_y = min_y; t.max_x = max_x; t.max_y = max_y;

        std::string path = out_dir + "/" + t.name + ".nodes.dat";
        t.writer = std::make_unique<RegionalNodeMap::Writer>(path, t.name, t.wgs84_bbox);
        targets.push_back(std::move(t));
    }

    if (targets.empty()) {
        std::cerr << "Error: no matching regions (check --regions names against include/Regions.h)\n";
        _exit(1);
    }

    if (verbose) {
        std::cout << "[regional_export] scanning " << nodes_file << " (max_id=" << max_id
                  << ") for " << targets.size() << " region(s)\n";
    }

    // open_shards_for_write=false: read-only access to the merged file, no
    // shard files touched — matches how delta/poll mode opens nodes.dat.
    OSMMMap osmmap(nodes_file, max_id, /*num_shards=*/1, ".", /*open_shards_for_write=*/false);

    uint64_t scanned = 0;
    auto start = std::chrono::steady_clock::now();
    osmmap.forEachPopulated([&](int64_t id, double lon_m, double lat_m) {
        ++scanned;
        for (auto& t : targets) {
            if (lon_m >= t.min_x && lon_m <= t.max_x &&
                lat_m >= t.min_y && lat_m <= t.max_y) {
                t.writer->append(id, lon_m, lat_m);
                ++t.matched;
            }
        }
        if (verbose && (scanned % 100'000'000 == 0)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "[regional_export] " << scanned << " nodes scanned (" << elapsed << "s)\n";
        }
    });

    for (auto& t : targets) {
        t.writer->finalize();
        if (verbose)
            std::cout << "[regional_export] " << t.name << ": " << t.matched << " node(s)\n";
    }

    std::cout << "[regional_export] done — " << scanned << " nodes scanned, "
              << targets.size() << " region file(s) written to " << out_dir << "\n";
    std::cout.flush();
    _exit(0);  // avoid pqxx/PROJ static-destructor double-free on normal return
}
