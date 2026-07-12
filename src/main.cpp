#include "OSMReader.h"
#include "OSMMMap.h"
#include "NavDB.h"
#include "Log.h"
#include "GeoUtils.h"

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <barrier>
#include <atomic>
#include <vector>
#include <queue>
#include <chrono>
#include <cstring>
#include <variant>
#include <mutex>  // for std::once_flag, std::call_once
#include <unistd.h> // for _exit
#include <fstream>
#include "OSCReader.h"
#include "DeltaApplier.h"
#include "Replicator.h"
#include "AirportsLoader.h"
#include "FAAObstacleLoader.h"
#include "WMMLoader.h"
#include "AirspaceLoader.h"
#include "TerrainLoader.h"
#include "PlanetDownloader.h"
#include <stdexcept>
#include <algorithm>
#include <iomanip>
#include <functional>
#include <memory>
#include <unordered_set>

// ---- Bounded blocking queue ----

template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(int max_size) : max_size_(max_size) {}

    void push(T item) {
        std::unique_lock lk(mu_);
        cv_not_full_.wait(lk, [&]{ return static_cast<int>(q_.size()) < max_size_ || stop_; });
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
    }

    bool pop(T& out) {
        std::unique_lock lk(mu_);
        cv_not_empty_.wait(lk, [&]{ return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return true;
    }

    void shutdown() {
        { std::lock_guard lk(mu_); stop_ = true; }
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    int size() {
        std::lock_guard lk(mu_);
        return static_cast<int>(q_.size());
    }

private:
    std::queue<T> q_;
    std::mutex mu_;
    std::condition_variable cv_not_empty_, cv_not_full_;
    int max_size_;
    bool stop_ = false;
};

// ---- Status ----

enum class Phase { Nodes, Merging, Ways, Reindexing, Indexing, Relations, AirportsLoading, FAALoading, WMMLoading, AirspaceLoading, TerrainLoading, Vacuuming, Done };

// Current time in microseconds since epoch — matches phase_start_us units
static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Nodes:      return "Nodes";
        case Phase::Merging:    return "Merging";
        case Phase::Ways:       return "Ways";
        case Phase::Reindexing: return "Reindexing";
        case Phase::Indexing:    return "Spatial Indexing";
        case Phase::Relations:  return "Relations";
        case Phase::AirportsLoading: return "Loading Airports";
        case Phase::FAALoading:     return "Loading FAA Obstacles";
        case Phase::WMMLoading:     return "Loading WMM Declination";
        case Phase::AirspaceLoading: return "Loading Airspace";
        case Phase::TerrainLoading: return "Loading Terrain";
        case Phase::Vacuuming:  return "Vacuuming";
        case Phase::Done:       return "Done";
    }
    return "";
}

// Records timing/count for one completed phase, for the final summary report.
struct PhaseStats {
    Phase phase;
    double elapsed_sec;
    int64_t count; // entities processed during this phase
};

struct Status {
    std::mutex phase_log_mu;
    std::vector<PhaseStats> phase_log;

    void recordPhase(Phase p, double elapsed_sec, int64_t count) {
        std::lock_guard<std::mutex> lk(phase_log_mu);
        phase_log.push_back({p, elapsed_sec, count});
    }

    // Records stats for the phase that just ended, then advances to the
    // next phase with a fresh start time and count baseline. Centralizes
    // what was previously duplicated at every phase transition site.
    void advancePhase(Phase finishing, Phase next) {
        int64_t total = nodes.load(std::memory_order_relaxed)
                      + areas.load(std::memory_order_relaxed)
                      + ways.load(std::memory_order_relaxed)
                      + relations.load(std::memory_order_relaxed);
        int64_t prev_start_us = phase_start_us.load(std::memory_order_relaxed);
        int64_t prev_count    = phase_count_at_start.load(std::memory_order_relaxed);
        int64_t now           = nowUs();

        if (prev_start_us > 0) {
            double elapsed = (now - prev_start_us) / 1'000'000.0;
            recordPhase(finishing, elapsed, total - prev_count);
        }

        phase_count_at_start.store(total, std::memory_order_relaxed);
        phase_start_us.store(now, std::memory_order_relaxed);
        phase.store(static_cast<int>(next), std::memory_order_relaxed);
    }

    // Current phase counters — reset at each phase transition for display
    std::atomic<int64_t> nodes{0};
    std::atomic<int64_t> areas{0};
    std::atomic<int64_t> ways{0};
    std::atomic<int64_t> relations{0};
    std::atomic<double>  progress{0.0};
    std::atomic<int>     phase{static_cast<int>(Phase::Nodes)};
    // Running totals — never reset, used for final summary
    std::atomic<int64_t> total_nodes{0};
    std::atomic<int64_t> total_areas{0};
    std::atomic<int64_t> total_ways{0};
    std::atomic<int64_t> total_relations{0};
    // Phase start time and counter snapshot — used to compute a per-phase
    // rate without resetting the displayed running totals (nodes/areas/
    // ways/relations always show cumulative counts).
    std::atomic<int64_t> phase_start_us{0};     // microseconds since epoch
    std::atomic<int64_t> phase_count_at_start{0}; // nodes+areas+ways+relations at phase start
};

// ---- WKB helpers ----
//
// Process-level SRID — set at startup from -M flag.
// 3857 = Web Mercator (default), 4326 = WGS84
int g_srid = 3857;

// buildWayGeom, mergeWayGeoms, and the ring/polygon WKB primitives used by
// buildMultipolygon below live in GeoUtils now — they're pure geometry
// construction with no OSM-specific dependencies, unlike buildMultipolygon
// itself (needs RelationEntry and NavDB), so they're usable by loaders
// that have nothing to do with OSM relations (AirspaceLoader,
// FAAObstacleLoader already share GeoUtils for pointWKB).

// ---- Args ----

enum class Mode { Import, Delta, Poll };

struct Args {
    std::string infile;
    std::string server;
    std::string database;
    std::string user;
    std::string log_file       = "osm_import.log";
    std::string nodes_file     = "nodes.dat";
    std::string shard_dir      = ".";  // directory for shard files
    int64_t     max_node_id    = 20'000'000'000LL;
    bool        verbose        = false;
    bool        init_schema    = false;
    bool        download_planet = false;  // -i acts as the destination path
    bool        use_mercator   = true;  // false = store as WGS84 (EPSG:4326)
    int         queue_size     = 10000;
    int         node_threads   = 1;  // RPi5-safe default; see README for tuning
    int         way_threads    = 6;  // RPi5-safe default; see README for tuning
    // Delta mode
    Mode        mode           = Mode::Import;
    std::string osc_file;                         // for -m delta
    std::string replication    = "minute";         // minute|hour|day
    int64_t     sequence       = -1;               // starting sequence
    int         poll_interval  = 60;               // seconds
    // Resume support
    Phase       resume_phase   = Phase::Nodes;     // -R: start at this phase
};

static int safeInt(const char* val, const char* flag) {
    try { return std::stoi(val); }
    catch (...) {
        std::cerr << "Invalid integer for " << flag << ": " << val << "\n";
        std::cout.flush(); std::cerr.flush();
        _exit(1);
    }
}

static int64_t safeInt64(const char* val, const char* flag) {
    try { return std::stoll(val); }
    catch (...) {
        std::cerr << "Invalid integer for " << flag << ": " << val << "\n";
        std::cout.flush(); std::cerr.flush();
        _exit(1);
    }
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "--infile"       || arg == "-i") && i+1 < argc) a.infile       = argv[++i];
        else if ((arg == "--server"       || arg == "-s") && i+1 < argc) a.server       = argv[++i];
        else if ((arg == "--database"     || arg == "-d") && i+1 < argc) a.database     = argv[++i];
        else if ((arg == "--user"         || arg == "-u") && i+1 < argc) a.user         = argv[++i];
        else if ((arg == "--nodes-file"   || arg == "-f") && i+1 < argc) a.nodes_file   = argv[++i];
        else if ((arg == "--shard-dir"    || arg == "-S") && i+1 < argc) a.shard_dir    = argv[++i];
        else if ((arg == "--max-node-id"  || arg == "-n") && i+1 < argc) a.max_node_id  = safeInt64(argv[++i], "--max-node-id");
        else if ((arg == "--log-file"     || arg == "-l") && i+1 < argc) a.log_file     = argv[++i];
        else if ((arg == "--queue-size"   || arg == "-q") && i+1 < argc) a.queue_size   = safeInt(argv[++i], "--queue-size");
        else if ((arg == "--node-threads" || arg == "-t") && i+1 < argc) a.node_threads = safeInt(argv[++i], "--node-threads");
        else if ((arg == "--way-threads"  || arg == "-w") && i+1 < argc) a.way_threads  = safeInt(argv[++i], "--way-threads");
        else if  (arg == "--verbose"      || arg == "-v")                a.verbose      = true;
        else if  (arg == "--init"         || arg == "-I")                a.init_schema  = true;
        else if  (arg == "--download-planet")                            a.download_planet = true;
        else if  (arg == "--wgs84"        || arg == "-L")                a.use_mercator = false;
        else if ((arg == "--mode"         || arg == "-m") && i+1 < argc) {
            std::string m = argv[++i];
            if      (m == "import") a.mode = Mode::Import;
            else if (m == "delta")  a.mode = Mode::Delta;
            else if (m == "poll")   a.mode = Mode::Poll;
            else { std::cerr << "Unknown mode: " << m << "\n"; std::cerr.flush(); _exit(1); }
        }
        else if ((arg == "--osc-file"     || arg == "-o") && i+1 < argc) { a.osc_file     = argv[++i]; a.mode = Mode::Delta; }
        else if ((arg == "--replication"  || arg == "-r") && i+1 < argc) a.replication   = argv[++i];
        else if ((arg == "--sequence"     || arg == "-Q") && i+1 < argc) a.sequence      = safeInt64(argv[++i], "--sequence");
        else if ((arg == "--poll-interval"|| arg == "-p") && i+1 < argc) a.poll_interval = safeInt(argv[++i], "--poll-interval");
        else if ((arg == "--resume"       || arg == "-R") && i+1 < argc) {
            std::string ph = argv[++i];
            if      (ph == "nodes")      a.resume_phase = Phase::Nodes;
            else if (ph == "merge")      a.resume_phase = Phase::Merging;
            else if (ph == "ways")       a.resume_phase = Phase::Ways;
            else if (ph == "reindex")    a.resume_phase = Phase::Reindexing;
            else if (ph == "relations")  a.resume_phase = Phase::Relations;
            else if (ph == "indexing")   a.resume_phase = Phase::Indexing;
            else if (ph == "airports")   a.resume_phase = Phase::AirportsLoading;
            else if (ph == "faa")        a.resume_phase = Phase::FAALoading;
            else if (ph == "wmm")        a.resume_phase = Phase::WMMLoading;
            else if (ph == "airspace")   a.resume_phase = Phase::AirspaceLoading;
            else if (ph == "terrain")    a.resume_phase = Phase::TerrainLoading;
            else if (ph == "vacuum")     a.resume_phase = Phase::Vacuuming;
            else { std::cerr << "Unknown resume phase: " << ph << "\n"; std::cerr.flush(); _exit(1); }
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: osm_import -s <host> -d <db> -u <user> [options]\n"
                "  Common (all modes):\n"
                "    -s server          PostgreSQL host (required)\n"
                "    -d database        Database name (required)\n"
                "    -u user            Database user (required)\n"
                "    -m mode            import|delta|poll (default import)\n"
                "    -l log_file        (default osm_import.log)\n"
                "    -v                 Verbose logging\n"
                "    -I                 Initialize (drop+recreate) all tables before import\n"
                "    -L                 Store coordinates as WGS84 lon/lat (EPSG:4326) instead of\n"
                "                       Web Mercator (EPSG:3857). Default is Mercator.\n"
                "    -h                 Show this help and exit\n"
                "  Import mode (default):\n"
                "    -i <file.osm.pbf>  Input PBF file\n"
                "    --download-planet  Download the latest planet-latest.osm.pbf from\n"
                "                       planet.openstreetmap.org before importing, saving it\n"
                "                       to the -i path (default ./planet-latest.osm.pbf if -i\n"
                "                       omitted). Resumable (safe to re-run after an\n"
                "                       interruption) and checksum-verified. The file is ~100GB\n"
                "                       and takes hours to download -- make sure the -i path is\n"
                "                       on a volume with enough free space.\n"
                "    -t node_threads    (default 1, see README for tuning)\n"
                "    -w way_threads     (default 6, see README for tuning)\n"
                "    NOTE: watch Q: (queue depth) in the status line to tune\n"
                "          threads — Q:0 means producer-bound (reduce threads),\n"
                "          Q:max means consumer-bound (increase threads).\n"
                "          On RPi5, reduce threads if system crashes under load.\n"
                "    -q queue_size      (default 10000)\n"
                "    -f nodes_file      (default nodes.dat)\n"
                "    -n max_node_id     (default 20000000000)\n"
                "    -S shard_dir       Directory for shard files (default .)\n"
                "    -R phase           Resume at phase: nodes|merge|ways|reindex|\n"
                "                       relations|indexing|airports|faa|wmm|airspace|terrain|vacuum (default nodes)\n"
                "                       Prerequisites for the chosen phase must\n"
                "                       already be complete (e.g. -R ways requires\n"
                "                       nodes.dat to already contain merged data and\n"
                "                       nodes to be fully populated)\n"
                "  Delta mode (-m delta):\n"
                "    -o <file.osc.gz>   OSC file to apply\n"
                "    -f nodes_file      Existing nodes.dat from initial import\n"
                "    -n max_node_id     Must match initial import\n"
                "  Poll mode (-m poll):\n"
                "    -r minute|hour|day Replication granularity (default minute)\n"
                "    -Q sequence        Starting sequence (default: from DB)\n"
                "    -p poll_interval   Seconds between checks (default 60)\n"
                "    -f nodes_file      Existing nodes.dat from initial import\n"
                "    -n max_node_id     Must match initial import\n";
            std::cout.flush();
            _exit(0);
        } else {
                }
    }
    if (a.server.empty() || a.database.empty() || a.user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n"; std::cerr.flush(); _exit(1);
    }
    if (a.mode == Mode::Import && a.infile.empty()) {
        if (a.download_planet) a.infile = "./planet-latest.osm.pbf";
        else { std::cerr << "Error: -i <file.osm.pbf> required for import mode\n"; std::cerr.flush(); _exit(1); }
    }
    if (a.mode == Mode::Delta && a.osc_file.empty()) {
        std::cerr << "Error: -o <file.osc.gz> required for delta mode\n"; std::cerr.flush(); _exit(1);
    }
    return a;
}


// ---- PBF resume-offset state file ----
// Records the blob-aligned byte offset where the first non-node entity
// begins, so -R merge/ways/relations can seek past all node blobs.

static std::string offsetFilePath(const Args& args) {
    return args.nodes_file + ".offset";
}

static void writeResumeOffset(const Args& args, std::streampos offset) {
    std::ofstream f(offsetFilePath(args), std::ios::trunc);
    if (!f) {
        std::cerr << "[main] warning: could not write resume offset file\n";
        return;
    }
    f << static_cast<int64_t>(offset) << "\n";
}

static std::streampos readResumeOffset(const Args& args) {
    std::ifstream f(offsetFilePath(args));
    if (!f) return std::streampos(-1);
    int64_t v = -1;
    f >> v;
    if (!f) return std::streampos(-1);
    return std::streampos(v);
}

static std::string relationsOffsetFilePath(const Args& args) {
    return args.nodes_file + ".relations_offset";
}

static void writeRelationsResumeOffset(const Args& args, std::streampos offset) {
    std::ofstream f(relationsOffsetFilePath(args), std::ios::trunc);
    if (!f) {
        std::cerr << "[main] warning: could not write relations resume offset file\n";
        return;
    }
    f << static_cast<int64_t>(offset) << "\n";
}

static std::streampos readRelationsResumeOffset(const Args& args) {
    std::ifstream f(relationsOffsetFilePath(args));
    if (!f) return std::streampos(-1);
    int64_t v = -1;
    f >> v;
    if (!f) return std::streampos(-1);
    return std::streampos(v);
}

// ---- Human-readable number formatting ----

static std::string hr(int64_t n) {
    if (n >= 1'000'000'000LL) return std::to_string(n / 1'000'000'000LL) + "." +
        std::to_string((n % 1'000'000'000LL) / 100'000'000LL) + "B";
    if (n >= 1'000'000) return std::to_string(n / 1'000'000) + "." +
        std::to_string((n % 1'000'000) / 100'000) + "M";
    if (n >= 1'000) return std::to_string(n / 1'000) + "." +
        std::to_string((n % 1'000) / 100) + "K";
    return std::to_string(n);
}

// Format a duration in seconds as H:MM:SS (or just seconds if under a minute)
static std::string formatDuration(double seconds) {
    if (seconds < 60.0) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << seconds << "s";
        return ss.str();
    }
    int64_t total_sec = static_cast<int64_t>(seconds);
    int64_t h = total_sec / 3600;
    int64_t m = (total_sec % 3600) / 60;
    int64_t s = total_sec % 60;
    std::ostringstream ss;
    if (h > 0) ss << h << ":" << std::setw(2) << std::setfill('0') << m
                  << ":" << std::setw(2) << std::setfill('0') << s;
    else       ss << m << ":" << std::setw(2) << std::setfill('0') << s;
    return ss.str();
}

// Determine whether a closed way should be stored as an area (polygon) or
// a way (linestring). Mirrors the heuristic used by osmium/osm2pgsql:
//
//  1. Not closed → always a way
//  2. area=yes   → area
//  3. area=no    → way
//  4. Has an area-implying tag → area
//  5. Has a linear-implying tag → way
//  6. Default → way (ambiguous closed ways are treated as linear)
//
// This prevents roundabouts, circular paths, and barriers from being
// incorrectly stored as polygons.
static bool isArea(bool is_closed, const Tags& tags) {
    if (!is_closed) return false;

    // Explicit area tag overrides everything
    auto it = tags.find("area");
    if (it != tags.end()) {
        if (it->second == "yes") return true;
        if (it->second == "no")  return false;
    }

    // Tags that imply linear use — closed ways with these are NOT areas
    static const std::unordered_set<std::string> linear_keys = {
        "highway", "railway", "waterway", "power", "man_made",
    };
    static const std::vector<std::pair<std::string,std::string>> linear_tag_values = {
        {"junction", "roundabout"},
        {"junction", "circular"},
        {"barrier",  "fence"},
        {"barrier",  "wall"},
        {"barrier",  "hedge"},
        {"barrier",  "retaining_wall"},
        {"barrier",  "city_wall"},
        {"aeroway",  "runway"},
        {"aeroway",  "taxiway"},
        {"aeroway",  "taxilane"},
    };
    for (auto& [k, v] : tags) {
        if (linear_keys.count(k)) return false;
    }
    for (auto& [k, v] : linear_tag_values) {
        auto it2 = tags.find(k);
        if (it2 != tags.end() && it2->second == v) return false;
    }

    // Tags that imply area use — closed ways with these ARE areas
    static const std::unordered_set<std::string> area_keys = {
        "building", "building:part", "landuse",  "leisure",  "natural",
        "amenity",  "shop",          "tourism",  "historic", "military",
        "boundary", "place",         "sport",    "landcover",
    };
    for (auto& [k, v] : tags) {
        if (area_keys.count(k)) return true;
    }

    return false; // default: ambiguous closed ways are linear
}



void nodeThread(int tid,
                BlockingQueue<OSMEntry>& my_q,
                BlockingQueue<OSMEntry>& way_q,
                Status& status,
                std::barrier<std::function<void()>>& node_barrier,
                OSMMMap& osmmap,
                std::mutex& db_flush_mu,
                const Args& args) {
    LOGI(tid, "node thread started");
    NavDB db(tid, args.server, args.user, args.database, db_flush_mu, args.queue_size);
    LOGI(tid, "db connected");

    OSMEntry entry;
    while (my_q.pop(entry)) {
        if (!std::holds_alternative<NodeEntry>(entry)) {
            // Forward to way queue and stop
            LOGI(tid, "forwarding non-node to way_q");
            way_q.push(std::move(entry));
            break;
        }

        auto& item = std::get<NodeEntry>(entry);
        // Project to Mercator (or keep as WGS84 if -M flag was given)
        if (args.use_mercator) {
            auto [mx, my] = toMercator(item.lon, item.lat);
            item.lon_m = mx;
            item.lat_m = my;
        } else {
            item.lon_m = item.lon;
            item.lat_m = item.lat;
        }
        item.geog_wkb_hex = pointWKB(item.lon_m, item.lat_m);
        osmmap.insert(tid, item.id, item.lon_m, item.lat_m);
        db.insertNode(item.id, item.name, item.lon_m, item.lat_m,
                      item.tags, item.geog_wkb_hex);
        status.nodes.fetch_add(1, std::memory_order_relaxed);
        status.total_nodes.fetch_add(1, std::memory_order_relaxed);
    }

    LOGI(tid, "flushing nodes, waiting at node_barrier");
    db.finalize_nodes();
    db.finalize_tags("node");
    node_barrier.arrive_and_wait();
    LOGI(tid, "node thread done");
}

// ---- Multipolygon assembly ----
//
// OSM multipolygon relations consist of member ways with roles "outer" or
// "inner". Each role group may be made up of multiple ways that need to be
// stitched end-to-end into complete rings. Once stitched, outer rings form
// the polygon shell(s) and inner rings form holes.
//
// This function:
//  1. Fetches the coordinate sequence for each member way from the DB
//  2. Stitches the segments into complete rings per role group
//  3. Builds a PostGIS-compatible WKB MULTIPOLYGON hex string
//
// Returns an empty string if the rings can't be assembled (e.g. missing
// member geometries, disconnected rings), in which case the relation is
// stored with an empty geometry rather than a wrong one.

static std::string buildMultipolygon(
        const RelationEntry& item, NavDB& db) {

    // Collect all member IDs for a single batched coordinate lookup —
    // one round trip for the whole relation instead of one per member.
    std::vector<int64_t> all_ids;
    all_ids.reserve(item.way_members.size());
    for (auto& m : item.way_members)
        all_ids.push_back(m.id);

    auto coords_map = db.getWayCoordsMap(all_ids);

    // Separate outer and inner member segments
    std::vector<Segment> outers, inners;
    for (auto& m : item.way_members) {
        auto it = coords_map.find(m.id);
        if (it == coords_map.end() || it->second.empty()) continue;
        if (m.role == "inner")
            inners.push_back(it->second);
        else // "outer" or "" (default is outer)
            outers.push_back(it->second);
    }

    if (outers.empty()) return "";

    auto outer_rings = stitchRings(std::move(outers));
    auto inner_rings = stitchRings(std::move(inners));

    if (outer_rings.empty()) return "";

    // Build WKB MULTIPOLYGON (each outer ring paired with any inner rings
    // it contains — for simplicity, assign all inners to all outers if
    // there's only one outer; for multiple outers, do a simple
    // point-in-polygon check to assign inners correctly)
    std::vector<uint8_t> buf;
    auto wu32 = [&](uint32_t v) {
        buf.push_back(v & 0xff); buf.push_back((v>>8)&0xff);
        buf.push_back((v>>16)&0xff); buf.push_back((v>>24)&0xff);
    };

    if (outer_rings.size() == 1) {
        // Single polygon — combine all inners as holes
        writeWkbPolygon(buf, outer_rings[0], inner_rings);
    } else {
        // Multiple outer rings — MULTIPOLYGON, assign inners by containment
        // (simplified: use bounding-box containment check)
        buf.push_back(1);       // little-endian
        wu32(0x20000006);       // WKB type: MULTIPOLYGON with SRID
        wu32(static_cast<uint32_t>(g_srid));
        wu32(static_cast<uint32_t>(outer_rings.size()));

        for (auto& outer : outer_rings) {
            // Find inners contained within this outer using bbox check
            double ox0 = outer[0].first, ox1 = outer[0].first;
            double oy0 = outer[0].second, oy1 = outer[0].second;
            for (auto& [x,y] : outer) {
                ox0=std::min(ox0,x); ox1=std::max(ox1,x);
                oy0=std::min(oy0,y); oy1=std::max(oy1,y);
            }
            std::vector<Ring> matched_inners;
            for (auto& inner : inner_rings) {
                // Check if inner centroid is within outer bbox
                double ix = inner[0].first, iy = inner[0].second;
                if (ix >= ox0 && ix <= ox1 && iy >= oy0 && iy <= oy1)
                    matched_inners.push_back(inner);
            }
            writeWkbPolygon(buf, outer, matched_inners);
        }
    }

    return toHex(buf);
}

// ---- Way/relation-phase thread ----

void wayThread(int tid,
               BlockingQueue<OSMEntry>& q,
               Status& status,
               std::barrier<>& way_barrier,
               OSMMMap& osmmap,
               std::mutex& db_flush_mu,
               const Args& args,
               bool skip_ways = false) {
    LOGI(tid, "way thread started");
    NavDB db(tid, args.server, args.user, args.database, db_flush_mu, args.queue_size);
    LOGI(tid, "db connected");

    bool way_phase_done = skip_ways;

    OSMEntry entry;
    while (q.pop(entry)) {
        std::visit([&](auto&& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, NodeEntry>) {
                // Shouldn't happen — ignore
            } else if constexpr (std::is_same_v<T, WayEntry>) {
                if (skip_ways) return;  // -R relations: ways already in DB

                // Thread 0 disables indexes at start of way phase
                static std::once_flag indexes_disabled;
                // disableIndexes() disabled — avoids catastrophic index rebuild
                // burst at reindexing phase that was triggering kernel lockups
                // std::call_once(indexes_disabled, [&db]{ db.disableIndexes(); });

                std::vector<std::pair<double,double>> coords;
                coords.reserve(item.node_refs.size());
                for (int64_t nid : item.node_refs) {
                    auto pt = osmmap.select(nid);
                    if (pt) coords.push_back(*pt);
                }

                bool is_closed = false;
                std::string geog = buildWayGeom(coords, is_closed);
                if (geog.empty()) return;

                if (isArea(is_closed, item.tags)) {
                    db.insertArea(item.id, item.name, item.tags, geog);
                    status.areas.fetch_add(1, std::memory_order_relaxed);
                    status.total_areas.fetch_add(1, std::memory_order_relaxed);
                } else {
                    db.insertWay(item.id, item.name, item.tags, geog);
                    status.ways.fetch_add(1, std::memory_order_relaxed);
                    status.total_ways.fetch_add(1, std::memory_order_relaxed);
                }

            } else if constexpr (std::is_same_v<T, RelationEntry>) {
                if (!way_phase_done) {
                    LOGI(tid, "first relation — flushing ways, waiting at way_barrier");
                    db.finalize_ways();
                    way_barrier.arrive_and_wait();
                    LOGI(tid, "way_barrier passed");
                    // Thread 0 re-enables indexes before relation phase
                    static std::once_flag indexes_enabled;
                    std::call_once(indexes_enabled, [&db, &status]{
                        // enableIndexes() disabled — primary keys maintained inline
                        // db.enableIndexes();
                        status.advancePhase(Phase::Ways, Phase::Relations);
                    });
                    way_phase_done = true;
                }

                // Check if this is a multipolygon or boundary relation
                auto type_it = item.tags.find("type");
                bool is_multipolygon = (type_it != item.tags.end() &&
                    (type_it->second == "multipolygon" ||
                     type_it->second == "boundary"));

                if (is_multipolygon) {
                    // Assemble rings from member ways and store as an area.
                    // Use negative relation ID to avoid primary key collision
                    // with way IDs — OSM way and relation IDs share the same
                    // integer space but are separate namespaces; storing both
                    // in the areas table requires disambiguation.
                    std::string geog = buildMultipolygon(item, db);
                    if (!geog.empty()) {
                        db.insertArea(-item.id, item.name, item.tags, geog);
                        status.areas.fetch_add(1, std::memory_order_relaxed);
                        status.total_areas.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        LOGI(tid, "skipping multipolygon relation id=", item.id,
                             " — ring assembly failed");
                    }
                } else {
                    // Non-multipolygon relation — merge member geometries as
                    // linestrings (route relations, restriction relations, etc.)
                    std::vector<std::string> geoms;
                    for (auto& m : item.way_members) {
                        std::string g = db.getWay(m.id);
                        if (!g.empty()) geoms.push_back(std::move(g));
                    }
                    std::string rel_geog  = geoms.empty() ? "" : mergeWayGeoms(geoms);
                    std::string road_geog = rel_geog;

                    db.insertRelation(item.id, item.name, item.tags, rel_geog);
                    status.relations.fetch_add(1, std::memory_order_relaxed);
                    status.total_relations.fetch_add(1, std::memory_order_relaxed);

                    // Store route=road or highway=* relations in roads
                    if (!road_geog.empty()) {
                        auto route_it   = item.tags.find("route");
                        auto highway_it = item.tags.find("highway");
                        bool is_road = (route_it   != item.tags.end() && route_it->second == "road") ||
                                       (highway_it != item.tags.end());
                        if (is_road)
                            db.insertRoad(item.id, item.name, item.tags, road_geog);
                    }
                }
            }
        }, entry);

        db.finalize_roads();
    }

    if (!way_phase_done) {
        LOGI(tid, "cleanup: flushing ways, waiting at way_barrier");
        db.finalize_ways();
        way_barrier.arrive_and_wait();
    }

    db.finalize_relations();
    LOGI(tid, "way thread done");
}

// ---- Status printer ----

void statusThread(const Status& s, std::atomic<bool>& done,
                  BlockingQueue<OSMEntry>& q,
                  const OSMMMap& osmmap,
                  const std::atomic<bool>& merge_done) {
    auto start = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_relaxed)) {
        auto now      = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        int64_t total  = s.nodes + s.areas + s.ways + s.relations;
        // Rate is computed from only the count accrued since this phase
        // began, using a snapshot of the cumulative total taken at the
        // phase transition — this keeps N/A/W/R as running totals for
        // display while making the displayed rate phase-local.
        int64_t phase_start = s.phase_start_us.load(std::memory_order_relaxed);
        int64_t phase_count = total - s.phase_count_at_start.load(std::memory_order_relaxed);
        double phase_elapsed = phase_start > 0
            ? std::chrono::duration<double>(
                std::chrono::microseconds(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch()).count() - phase_start)).count()
            : elapsed;
        double rps = phase_elapsed > 0 ? phase_count / phase_elapsed : 0;
        // Format elapsed as H:MM:SS
        int64_t secs = static_cast<int64_t>(elapsed);
        int h = secs / 3600, m = (secs % 3600) / 60, sec = secs % 60;
        char elapsed_str[16];
        snprintf(elapsed_str, sizeof(elapsed_str), "%d:%02d:%02d", h, m, sec);
        std::cout << "\r" << elapsed_str
                  << " " << std::fixed << std::setprecision(1)
                  << s.progress.load() << "%  "
                  << "N:" << hr(s.nodes) << " A:" << hr(s.areas)
                  << " W:" << hr(s.ways) << " R:" << hr(s.relations)
                  << " Q:" << q.size();
        if (!merge_done.load(std::memory_order_relaxed)) {
            int64_t mp = osmmap.mergeProgress();
            int64_t mt = osmmap.mergeTotal();
            double  mp_pct = mt > 0 ? (mp * 100.0 / mt) : 0.0;
            std::cout << " M:" << std::fixed << std::setprecision(1) << mp_pct << "%";
        }
        Phase ph = static_cast<Phase>(s.phase.load(std::memory_order_relaxed));
        std::cout << " | " << hr(static_cast<int64_t>(rps)) << "/s  "
                  << "[" << phaseName(ph) << "]   "
                  << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ---- Delta / Poll entry point ----

static int runDelta(const Args& args) {
    // Open existing merged mmap read-only — delta mode only needs select()
    // for node coordinate lookups, never writes to shards.
    // open_shards_for_write=false skips shard file open/creation entirely.
    OSMMMap osmmap(args.nodes_file, args.max_node_id, 1, args.shard_dir,
                   /*open_shards_for_write=*/false);
    osmmap.setRandomAccessHint();

    std::mutex db_flush_mu;
    NavDB db(0, args.server, args.user, args.database, db_flush_mu);
    DeltaApplier applier(osmmap, db);

    if (args.mode == Mode::Delta) {
        // Apply a single OSC file
        OSCReader reader(args.osc_file);
        int64_t n = reader.parse([&](OSCChange&& c) {
            applier.apply(std::move(c));
        });
        applier.flush();
        std::cout << "Applied " << n << " changes ("
                  << "created=" << applier.created()
                  << " modified=" << applier.modified()
                  << " deleted=" << applier.deleted() << ")\n";
    } else {
        // Poll mode
        ReplicationGranularity gran = ReplicationGranularity::Minute;
        if (args.replication == "hour") gran = ReplicationGranularity::Hour;
        if (args.replication == "day")  gran = ReplicationGranularity::Day;

        Replicator replicator(applier, db, gran,
                              args.server, args.user, args.database);

        if (args.sequence >= 0)
            replicator.setSequence(args.sequence);

        replicator.poll(args.poll_interval);
    }

    std::cout.flush();
    _exit(0); // see comment near other _exit(0) calls in main()
}

// ---- main ----

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    if (args.verbose)
        Log::get().open(args.log_file);

    // Dispatch to delta/poll mode if requested
    if (args.mode != Mode::Import)
        return runDelta(args);

    // Set global SRID for all WKB geometry builders
    g_srid = args.use_mercator ? 3857 : 4326;

    // --download-planet: fetch the latest planet file before touching the
    // DB at all, so a failed/incomplete download doesn't leave the schema
    // wiped (-I) with nothing to import.
    if (args.download_planet) {
        LOGI(-1, "downloading latest planet file to ", args.infile);
        if (!downloadLatestPlanet(args.infile, true)) {
            std::cerr << "Planet download failed — aborting before touching the database. "
                         "Re-run with the same arguments to resume.\n";
            std::cerr.flush();
            _exit(1);
        }
        LOGI(-1, "planet file downloaded and verified");
    }

    // -I: drop and recreate all tables before starting the import
    if (args.init_schema) {
        std::cout << "Initializing schema...\n";
        std::mutex mu;
        NavDB db(0, args.server, args.user, args.database, mu);
        db.initializeSchema();
        std::cout << "Schema initialized.\n";
    }

    // Disable autovacuum for the duration of the import — bulk loading
    // with autovacuum running wastes I/O on tables that will be
    // vacuumed explicitly at the end anyway.
    {
        std::mutex mu;
        NavDB db(0, args.server, args.user, args.database, mu);
        db.setAutovacuum(false);
    }

    LOGI(-1, "starting infile=", args.infile,
         " node_threads=", args.node_threads,
         " way_threads=", args.way_threads,
         " queue=", args.queue_size,
         " nodes_file=", args.nodes_file,
         " max_node_id=", args.max_node_id);

    auto start = std::chrono::steady_clock::now();
    std::mutex db_flush_mu_early;

    // -R indexing / -R airports / -R faa / -R wmm / -R airspace / -R terrain / -R vacuum: no PBF processing needed at all
    if (args.resume_phase == Phase::Indexing || args.resume_phase == Phase::AirportsLoading
        || args.resume_phase == Phase::FAALoading || args.resume_phase == Phase::WMMLoading
        || args.resume_phase == Phase::AirspaceLoading || args.resume_phase == Phase::TerrainLoading
        || args.resume_phase == Phase::Vacuuming) {
        if (args.resume_phase == Phase::Indexing) {
            LOGI(-1, "resume: creating GiST spatial indexes");
            NavDB db(0, args.server, args.user, args.database, db_flush_mu_early);
            db.createGistIndexes();
            LOGI(-1, "GiST indexes done");
        }
        if (args.resume_phase == Phase::Indexing || args.resume_phase == Phase::AirportsLoading
            || args.resume_phase == Phase::FAALoading) {
            LOGI(-1, "resume: loading airports data");
            {
                NavDB db(0, args.server, args.user, args.database, db_flush_mu_early);
                db.truncateForResume("airports");
            }
            AirportsLoader(args.server, args.user, args.database).load(false);
            LOGI(-1, "airports data loaded");
        }
        if (args.resume_phase == Phase::FAALoading || args.resume_phase == Phase::Vacuuming) {
            LOGI(-1, "resume: loading FAA obstacle data");
            FAAObstacleLoader(args.server, args.user, args.database).load(false);
            LOGI(-1, "FAA obstacle data loaded");
        }
        if (args.resume_phase == Phase::FAALoading || args.resume_phase == Phase::WMMLoading
            || args.resume_phase == Phase::Vacuuming) {
            LOGI(-1, "resume: loading WMM declination data");
            WMMLoader(args.server, args.user, args.database).load(currentDecimalYear(),
                    -180, -90, 180, 90, 0.25, 3857, 0.25, 50.0, 4, false);
            LOGI(-1, "WMM declination data loaded");
        }
        if (args.resume_phase == Phase::WMMLoading || args.resume_phase == Phase::AirspaceLoading
            || args.resume_phase == Phase::Vacuuming) {
            LOGI(-1, "resume: loading airspace data");
            {
                AirspaceLoader airspace(args.server, args.user, args.database);
                airspace.loadClassAirspace(false);
                airspace.loadSpecialUseAirspace(false);
                std::string openaip_key = defaultOpenAipApiKey();
                if (!openaip_key.empty())
                    airspace.loadInternationalAirspace(openaip_key, false);
                else
                    LOGI(-1, "no OpenAIP API key found (~/.openaip_api_key) — skipping international airspace");
            }
            LOGI(-1, "airspace data loaded");
        }
        if (args.resume_phase == Phase::AirspaceLoading || args.resume_phase == Phase::TerrainLoading
            || args.resume_phase == Phase::Vacuuming) {
            LOGI(-1, "resume: loading terrain elevation data");
            {
                TerrainLoader terrain(args.server, args.user, args.database);
                terrain.load(-125, 24, -66, 50, TerrainSource::USGS3DEP, 3857, 500, 50.0, 4, false);
                terrain.loadGlobal(3857, 500, 50.0, 4, false);
            }
            LOGI(-1, "terrain elevation data loaded");
        }
        LOGI(-1, "resume: running VACUUM ANALYZE on all tables");
        {
            NavDB db(0, args.server, args.user, args.database, db_flush_mu_early);
            db.vacuumAnalyze();
        }
        LOGI(-1, "VACUUM ANALYZE complete");
        {
            NavDB db(0, args.server, args.user, args.database, db_flush_mu_early);
            db.setAutovacuum(true);
        }

        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "\nResumed run done in " << elapsed << "s\n";
        std::cout.flush();
        // _exit() rather than return: skips static/global destructor
        // teardown across PROJ/protobuf/pqxx, which has been observed to
        // cause "double free or corruption" crashes after all real work has
        // already completed successfully. Since nothing meaningful needs
        // cleanup at this point (all data is committed to PostgreSQL), this
        // is safe.
        _exit(0);
    }

    // Resuming at merge/ways/relations: nodes.dat and shard files already
    // exist from the previous run — do NOT truncate them.
    if (args.resume_phase == Phase::Nodes) {
        OSMMMap::createFile(args.nodes_file, args.max_node_id, args.node_threads, args.shard_dir);
    } else {
        LOGI(-1, "resume: reusing existing nodes.dat / shard files (no createFile)");
    }
    // Resuming at ways/relations: shard data is no longer needed at all
    // (merge already happened in a prior run); for -R merge, shards must be
    // preserved (open read-only) so merge() can consume them.
    bool open_shards_for_write = (args.resume_phase == Phase::Nodes);
    OSMMMap osmmap(args.nodes_file, args.max_node_id, args.node_threads,
                   args.shard_dir, open_shards_for_write);

    // One queue per node thread — no contention between node threads
    std::vector<std::unique_ptr<BlockingQueue<OSMEntry>>> node_queues;
    node_queues.reserve(args.node_threads);
    for (int i = 0; i < args.node_threads; ++i)
        node_queues.push_back(std::make_unique<BlockingQueue<OSMEntry>>(args.queue_size));

    // Shared queue for way/relation phase
    BlockingQueue<OSMEntry> way_q(args.queue_size);

    Status              status;
    status.phase_start_us.store(nowUs(), std::memory_order_relaxed);
    std::atomic<bool>   nodes_done{false};
    std::atomic<bool>   merge_done{false};
    std::barrier<std::function<void()>> node_barrier(
        args.node_threads,
        [&osmmap, &merge_done, &status]() noexcept {
            Log::get().write(-1, "INFO", "merge starting");
            status.advancePhase(Phase::Nodes, Phase::Merging);
            osmmap.merge();
            osmmap.setRandomAccessHint();
            merge_done.store(true, std::memory_order_release);
            status.advancePhase(Phase::Merging, Phase::Ways);
            Log::get().write(-1, "INFO", "merge complete");
        }
    );
    std::barrier<>      way_barrier(args.way_threads);
    std::mutex          db_flush_mu;
    std::atomic<bool>   status_done{false};

    // On resume, the target phase's tables must be empty — otherwise the
    // upcoming COPY operations could collide with rows already committed by
    // a previous attempt that crashed partway through, causing duplicate-key
    // errors. truncateForResume() is idempotent (TRUNCATE on empty tables is
    // a harmless no-op), so this is always safe to run.
    if (args.resume_phase == Phase::Ways) {
        NavDB resume_db(0, args.server, args.user, args.database, db_flush_mu);
        resume_db.truncateForResume("ways");
    } else if (args.resume_phase == Phase::Relations) {
        NavDB resume_db(0, args.server, args.user, args.database, db_flush_mu);
        resume_db.truncateForResume("relations");
    }

    LOGI(-1, "launching ", args.way_threads, " way threads");
    std::vector<std::thread> way_workers;
    way_workers.reserve(args.way_threads);
    bool skip_ways_for_relations = (args.resume_phase == Phase::Relations);
    for (int i = 0; i < args.way_threads; ++i)
        way_workers.emplace_back(wayThread,
                                 args.node_threads + i,
                                 std::ref(way_q), std::ref(status),
                                 std::ref(way_barrier),
                                 std::ref(osmmap),
                                 std::ref(db_flush_mu), std::cref(args),
                                 skip_ways_for_relations);

    bool skip_nodes = (args.resume_phase == Phase::Ways ||
                       args.resume_phase == Phase::Relations);
    bool skip_merge = skip_nodes; // mmap already merged on disk in these modes

    std::vector<std::thread> node_workers;
    if (!skip_nodes) {
        LOGI(-1, "launching ", args.node_threads, " node threads");
        node_workers.reserve(args.node_threads);
        for (int i = 0; i < args.node_threads; ++i)
            node_workers.emplace_back(nodeThread, i,
                                      std::ref(*node_queues[i]),
                                      std::ref(way_q),
                                      std::ref(status),
                                      std::ref(node_barrier),
                                      std::ref(osmmap),
                                      std::ref(db_flush_mu), std::cref(args));
    } else {
        LOGI(-1, "resume: skipping node threads (resume_phase=", phaseName(args.resume_phase), ")");
        // Shut down node queues immediately — nothing will be pushed to them
        for (auto& nq : node_queues) nq->shutdown();
        nodes_done.store(true, std::memory_order_relaxed);
        if (skip_merge) {
            osmmap.setRandomAccessHint();
            merge_done.store(true, std::memory_order_release);
            // Nodes/Merging were already completed in a prior run, so there's
            // nothing to record for them here — just start the clock fresh
            // for whichever phase we're actually resuming into.
            Phase target = (args.resume_phase == Phase::Relations)
                ? Phase::Relations : Phase::Ways;
            status.advancePhase(target, target);
        }
    }

    auto t_status = std::thread(statusThread, std::cref(status),
                                std::ref(status_done), std::ref(way_q),
                                std::cref(osmmap), std::cref(merge_done));

    OSMReader reader(args.infile);
    auto file_size = reader.getSize();
    std::vector<OSMEntry> batch;
    int64_t batch_count = 0;

    LOGI(-1, "reading PBF file_size=", static_cast<int64_t>(file_size));

    int64_t rr = 0;  // round-robin index — must be 64-bit to handle >2^31 nodes
    // -R merge: don't re-parse/re-insert nodes — shards already on disk from
    // the previous run. We still need the node threads to reach node_barrier
    // so merge() runs, which happens naturally once node_phase ends below.
    bool resume_skip_node_emit = (args.resume_phase == Phase::Merging);
    bool node_phase = !skip_nodes;

    // -R relations: prefer the relations-offset (skips ways AND nodes
    // entirely). Falls back to the node-offset (skip nodes, still read ways
    // to discard them) if no relations offset has been recorded yet.
    if (args.resume_phase == Phase::Relations) {
        std::streampos roff = readRelationsResumeOffset(args);
        if (roff >= 0) {
            LOGI(-1, "resume: seeking to saved relations offset ", static_cast<int64_t>(roff),
                 " (skipping node and way blobs)");
            reader.seekTo(roff);
        } else {
            std::streampos off = readResumeOffset(args);
            if (off >= 0) {
                LOGI(-1, "resume: no relations offset yet — seeking to node offset ",
                     static_cast<int64_t>(off), " (skipping node blobs, will discard ways)");
                reader.seekTo(off);
            } else {
                LOGI(-1, "resume: no offset files found — falling back to full re-read with skips");
            }
        }
    }
    // -R merge/ways: seek past all node blobs using the saved resume offset,
    // if available, so we don't decompress/parse them at all.
    else if (skip_nodes || resume_skip_node_emit) {
        std::streampos off = readResumeOffset(args);
        if (off >= 0) {
            LOGI(-1, "resume: seeking to saved offset ", static_cast<int64_t>(off),
                 " (skipping node blobs)");
            reader.seekTo(off);
        } else {
            LOGI(-1, "resume: no offset file found (", offsetFilePath(args),
                 ") — falling back to full re-read with node-skip");
        }
    }

    bool relations_offset_written = false;
    std::streampos blob_start = reader.getPosition();
    while (reader.next(batch)) {
        for (auto& entry : batch) {
            if (node_phase && std::holds_alternative<NodeEntry>(entry)) {
                if (resume_skip_node_emit) continue; // drop — shards already complete
                node_queues[rr % args.node_threads]->push(std::move(entry));
                ++rr;
            } else {
                if (node_phase) {
                    // First non-node — shut down node queues and wait for merge
                    node_phase = false;
                    for (auto& nq : node_queues) nq->shutdown();
                    // Record the blob-aligned offset of the first non-node
                    // entity so future -R merge/ways/relations runs can seek
                    // straight here, skipping all node blobs.
                    writeResumeOffset(args, blob_start);
                    LOGI(-1, "node phase complete, waiting for merge");
                    // Spin until merge() completes before feeding way_q
                    while (!merge_done.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    LOGI(-1, "merge done, switching to way phase");
                }
                if (args.resume_phase == Phase::Relations &&
                    std::holds_alternative<WayEntry>(entry))
                    continue; // -R relations: ways already in DB, skip re-reading

                // Record the blob-aligned offset of the first relation so
                // future -R relations runs can seek straight here, skipping
                // both node AND way blobs entirely.
                if (std::holds_alternative<RelationEntry>(entry) &&
                    !relations_offset_written) {
                    writeRelationsResumeOffset(args, blob_start);
                    relations_offset_written = true;
                }

                way_q.push(std::move(entry));
            }
        }
        batch.clear();

        ++batch_count;
        if (batch_count % 500 == 0)
            LOGI(-1, "producer batch=", batch_count,
                 " pos=", static_cast<int64_t>(reader.getPosition()),
                 " wq=", way_q.size());

        double pos = static_cast<double>(reader.getPosition());
        double sz  = static_cast<double>(file_size);
        status.progress.store(sz > 0 ? (pos / sz * 100.0) : 0.0,
                              std::memory_order_relaxed);

        // Track the start of the next blob, for resume-offset purposes
        blob_start = reader.getPosition();
    }

    LOGI(-1, "producer done — shutting down node queues");
    for (auto& nq : node_queues) nq->shutdown();
    LOGI(-1, "waiting for node threads");
    for (auto& w : node_workers) w.join();
    LOGI(-1, "node threads done — shutting down way queue");

    way_q.shutdown();
    LOGI(-1, "waiting for way threads");
    for (auto& w : way_workers) w.join();
    LOGI(-1, "all threads done");

    // Relations phase ends here (way threads just joined)
    status.advancePhase(Phase::Relations, Phase::Indexing);
    LOGI(-1, "creating GiST spatial indexes");
    {
        NavDB db(0, args.server, args.user, args.database, db_flush_mu);
        db.createGistIndexes();
    }
    LOGI(-1, "GiST indexes done");

    status.advancePhase(Phase::Indexing, Phase::AirportsLoading);
    LOGI(-1, "loading airports data");
    AirportsLoader(args.server, args.user, args.database).load(false);
    LOGI(-1, "airports data loaded");

    status.advancePhase(Phase::AirportsLoading, Phase::FAALoading);
    LOGI(-1, "loading FAA obstacle data");
    FAAObstacleLoader(args.server, args.user, args.database).load(false);
    LOGI(-1, "FAA obstacle data loaded");

    status.advancePhase(Phase::FAALoading, Phase::WMMLoading);
    LOGI(-1, "loading WMM declination data");
    WMMLoader(args.server, args.user, args.database).load(currentDecimalYear(),
            -180, -90, 180, 90, 0.25, 3857, 0.25, 50.0, 4, false);
    LOGI(-1, "WMM declination data loaded");

    status.advancePhase(Phase::WMMLoading, Phase::AirspaceLoading);
    LOGI(-1, "loading airspace data");
    {
        AirspaceLoader airspace(args.server, args.user, args.database);
        airspace.loadClassAirspace(false);
        airspace.loadSpecialUseAirspace(false);
        std::string openaip_key = defaultOpenAipApiKey();
        if (!openaip_key.empty())
            airspace.loadInternationalAirspace(openaip_key, false);
        else
            LOGI(-1, "no OpenAIP API key found (~/.openaip_api_key) — skipping international airspace");
    }
    LOGI(-1, "airspace data loaded");

    status.advancePhase(Phase::AirspaceLoading, Phase::TerrainLoading);
    LOGI(-1, "loading terrain elevation data");
    {
        TerrainLoader terrain(args.server, args.user, args.database);
        terrain.load(-125, 24, -66, 50, TerrainSource::USGS3DEP, 3857, 500, 50.0, 4, false);
        terrain.loadGlobal(3857, 500, 50.0, 4, false);
    }
    LOGI(-1, "terrain elevation data loaded");

    status.advancePhase(Phase::TerrainLoading, Phase::Vacuuming);
    {
        NavDB db(0, args.server, args.user, args.database, db_flush_mu);
        db.vacuumAnalyze();
    }
    LOGI(-1, "VACUUM ANALYZE complete");
    {
        NavDB db(0, args.server, args.user, args.database, db_flush_mu);
        db.setAutovacuum(true);
    }

    status.advancePhase(Phase::Vacuuming, Phase::Done);
    status_done.store(true, std::memory_order_relaxed);
    t_status.join();

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    int64_t total = status.total_nodes + status.total_areas + status.total_ways + status.total_relations;

    LOGI(-1, "done elapsed=", elapsed, "s total=", total);

    // Per-phase breakdown table
    std::cout << "\n--- Phase summary ---\n";
    {
        std::lock_guard<std::mutex> lk(status.phase_log_mu);
        std::cout << std::left  << std::setw(18) << "Phase"
                  << std::right << std::setw(12) << "Time"
                  << std::right << std::setw(14) << "Count"
                  << std::right << std::setw(14) << "Rate" << "\n";
        for (auto& p : status.phase_log) {
            std::cout << std::left  << std::setw(18) << phaseName(p.phase)
                       << std::right << std::setw(12) << formatDuration(p.elapsed_sec);
            if (p.count > 0) {
                double rate = p.elapsed_sec > 0 ? p.count / p.elapsed_sec : 0;
                std::cout << std::right << std::setw(14) << hr(p.count)
                           << std::right << std::setw(11) << hr(static_cast<int64_t>(rate)) << "/s";
            } else {
                std::cout << std::right << std::setw(14) << "-"
                           << std::right << std::setw(14) << "-";
            }
            std::cout << "\n";
        }
    }
    std::cout << "----------------------\n";

    std::cout << "\nDone in " << elapsed << "s | "
              << "Nodes: " << hr(status.total_nodes) << ", "
              << "Areas: " << hr(status.total_areas) << ", "
              << "Ways: "  << hr(status.total_ways)  << ", "
              << "Relations: " << hr(status.total_relations) << " | "
              << hr(static_cast<int64_t>(total / elapsed)) << "/s\n";
    std::cout.flush();
    // _exit() rather than return: see comment in the -R indexing/airports
    // resume branch above — skips static teardown that has been observed
    // to crash with "double free or corruption" after all work completes.
    _exit(0);
}
