#include "OSMReader.h"
#include "OSMMMap.h"
#include "NavDB.h"
#include "Log.h"

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
#include <stdexcept>
#include <algorithm>
#include <iomanip>

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

    // Peek at front without removing; returns false if empty/stopped
    bool peek(T& out) {
        std::unique_lock lk(mu_);
        cv_not_empty_.wait(lk, [&]{ return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = q_.front();
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

struct Status {
    std::atomic<int64_t> nodes{0};
    std::atomic<int64_t> areas{0};
    std::atomic<int64_t> ways{0};
    std::atomic<int64_t> relations{0};
    std::atomic<double>  progress{0.0};
};

// ---- WKB helpers ----

static std::string buildWayGeom(const std::vector<std::pair<double,double>>& coords,
                                bool& is_closed) {
    if (coords.size() < 2) return "";
    is_closed = (coords.size() >= 4 &&
                 coords.front().first  == coords.back().first &&
                 coords.front().second == coords.back().second);

    auto toHex = [](const std::vector<uint8_t>& b) {
        static const char h[] = "0123456789ABCDEF";
        std::string s; s.reserve(b.size()*2);
        for (auto x : b) { s += h[x>>4]; s += h[x&0xF]; }
        return s;
    };
    auto wd = [](std::vector<uint8_t>& buf, double v) {
        uint8_t t[8]; memcpy(t, &v, 8);
        buf.insert(buf.end(), t, t+8);
    };
    auto wu32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(v&0xFF); buf.push_back((v>>8)&0xFF);
        buf.push_back((v>>16)&0xFF); buf.push_back((v>>24)&0xFF);
    };

    std::vector<uint8_t> buf;
    buf.push_back(0x01);
    if (is_closed) { wu32(buf, 3); wu32(buf, 1); }
    else             wu32(buf, 2);
    wu32(buf, static_cast<uint32_t>(coords.size()));
    for (auto& [x, y] : coords) { wd(buf, x); wd(buf, y); }
    return toHex(buf);
}

static std::string mergeWayGeoms(const std::vector<std::string>& wkb_hexes) {
    auto fromHex = [](const std::string& hex) {
        std::vector<uint8_t> b; b.reserve(hex.size()/2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto n = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c-'0';
                if (c >= 'A' && c <= 'F') return c-'A'+10;
                if (c >= 'a' && c <= 'f') return c-'a'+10;
                return 0;
            };
            b.push_back((n(hex[i]) << 4) | n(hex[i+1]));
        }
        return b;
    };
    auto toHex = [](const std::vector<uint8_t>& b) {
        static const char h[] = "0123456789ABCDEF";
        std::string s; s.reserve(b.size()*2);
        for (auto x : b) { s += h[x>>4]; s += h[x&0xF]; }
        return s;
    };
    auto wu32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(v&0xFF); buf.push_back((v>>8)&0xFF);
        buf.push_back((v>>16)&0xFF); buf.push_back((v>>24)&0xFF);
    };

    std::vector<std::vector<uint8_t>> parts;
    for (auto& h : wkb_hexes)
        if (!h.empty()) parts.push_back(fromHex(h));
    if (parts.empty()) return "";

    std::vector<uint8_t> buf;
    buf.push_back(0x01);
    wu32(buf, 5);
    wu32(buf, static_cast<uint32_t>(parts.size()));
    for (auto& p : parts) buf.insert(buf.end(), p.begin(), p.end());
    return toHex(buf);
}

// ---- Args ----

struct Args {
    std::string infile;
    std::string server;
    std::string database;
    std::string user;
    std::string log_file       = "osm_import.log";
    std::string nodes_file     = "nodes.dat";
    int64_t node_count_hint    = 1'000'000'000LL;
    bool verbose               = false;
    int queue_size             = 10000;
    int node_threads           = 4;
    int way_threads            = 8;
};

static int safeInt(const char* val, const char* flag) {
    try { return std::stoi(val); }
    catch (...) {
        std::cerr << "Error: " << flag << " expects an integer, got: " << val << "\n";
        exit(1);
    }
}

static int64_t safeInt64(const char* val, const char* flag) {
    try { return std::stoll(val); }
    catch (...) {
        std::cerr << "Error: " << flag << " expects an integer, got: " << val << "\n";
        exit(1);
    }
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "--infile"       || arg == "-i") && i+1 < argc) a.infile          = argv[++i];
        else if ((arg == "--server"       || arg == "-s") && i+1 < argc) a.server          = argv[++i];
        else if ((arg == "--database"     || arg == "-d") && i+1 < argc) a.database        = argv[++i];
        else if ((arg == "--user"         || arg == "-u") && i+1 < argc) a.user            = argv[++i];
        else if ((arg == "--node-count"   || arg == "-n") && i+1 < argc) a.node_count_hint = safeInt64(argv[++i], "--node-count");
        else if ((arg == "--nodes-file"   || arg == "-f") && i+1 < argc) a.nodes_file      = argv[++i];
        else if ((arg == "--log-file"     || arg == "-l") && i+1 < argc) a.log_file        = argv[++i];
        else if ((arg == "--queue-size"   || arg == "-q") && i+1 < argc) a.queue_size      = safeInt(argv[++i], "--queue-size");
        else if ((arg == "--node-threads" || arg == "-t") && i+1 < argc) a.node_threads    = safeInt(argv[++i], "--node-threads");
        else if ((arg == "--way-threads"  || arg == "-w") && i+1 < argc) a.way_threads     = safeInt(argv[++i], "--way-threads");
        else if  (arg == "--verbose"      || arg == "-v")                a.verbose         = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: osm_import -i <file.osm.pbf> -s <host> -d <db> -u <user>\n"
                "  [-t node_threads    (default 4)]\n"
                "  [-w way_threads     (default 8)]\n"
                "  [-q queue_size      (default 10000)]\n"
                "  [-n node_count_hint (default 1000000000)]\n"
                "  [-f nodes_file      (default nodes.dat)]\n"
                "  [-l log_file] [-v]\n";
            exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            exit(1);
        }
    }
    if (a.infile.empty() || a.server.empty() || a.database.empty() || a.user.empty()) {
        std::cerr << "Missing required arguments. Use --help.\n";
        exit(1);
    }
    return a;
}

// ---- Node-phase thread ----
// Consumes NodeEntry items until nodes_done is set, then exits.
// The first thread to see a non-node sets nodes_done so all other
// node threads stop competing for items on the queue.

void nodeThread(int tid,
                BlockingQueue<OSMEntry>& q,
                Status& status,
                std::barrier<>& node_barrier,
                std::atomic<bool>& nodes_done,
                OSMMMap& osmmap,
                std::mutex& mmap_mu,
                std::mutex& db_flush_mu,
                const Args& args) {
    LOGI(tid, "node thread started");
    NavDB db(tid, args.server, args.user, args.database, db_flush_mu, args.queue_size);
    LOGI(tid, "db connected");

    OSMEntry entry;
    while (!nodes_done.load(std::memory_order_acquire)) {
        if (!q.pop(entry)) break;

        if (!std::holds_alternative<NodeEntry>(entry)) {
            // First non-node seen — push it back and signal all node threads to stop
            LOGI(tid, "first non-node seen, setting nodes_done");
            nodes_done.store(true, std::memory_order_release);
            q.push(std::move(entry));
            break;
        }

        auto& item = std::get<NodeEntry>(entry);
        {
            std::lock_guard lk(mmap_mu);
            osmmap.insert(item.id, item.lon_m, item.lat_m);
        }
        db.insertNode(item.id, item.name, item.lon_m, item.lat_m,
                      item.tags, item.geog_wkb_hex);
        status.nodes.fetch_add(1, std::memory_order_relaxed);
    }

    LOGI(tid, "flushing nodes, waiting at node_barrier");
    db.finalize_nodes();
    db.finalize_tags("node");
    node_barrier.arrive_and_wait();
    LOGI(tid, "node thread done");
}

// ---- Way/relation-phase thread ----

void wayThread(int tid,
               BlockingQueue<OSMEntry>& q,
               Status& status,
               std::barrier<>& way_barrier,
               OSMMMap& osmmap,
               std::mutex& mmap_mu,
               std::mutex& db_flush_mu,
               const Args& args) {
    LOGI(tid, "way thread started");
    NavDB db(tid, args.server, args.user, args.database, db_flush_mu, args.queue_size);
    LOGI(tid, "db connected");

    bool way_phase_done = false;

    OSMEntry entry;
    while (q.pop(entry)) {
        std::visit([&](auto&& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, NodeEntry>) {
                // Shouldn't happen — node threads consumed all nodes.
                // Guard anyway: just insert coords, skip DB write.
                std::lock_guard lk(mmap_mu);
                osmmap.insert(item.id, item.lon_m, item.lat_m);

            } else if constexpr (std::is_same_v<T, WayEntry>) {
                std::vector<std::pair<double,double>> coords;
                coords.reserve(item.node_refs.size());
                {
                    std::lock_guard lk(mmap_mu);
                    for (int64_t nid : item.node_refs) {
                        auto pt = osmmap.select(nid);
                        if (pt) coords.push_back(*pt);
                    }
                }

                bool is_closed = false;
                std::string geog = buildWayGeom(coords, is_closed);
                if (geog.empty()) return;

                if (is_closed) {
                    db.insertArea(item.id, item.name, item.tags, geog);
                    status.areas.fetch_add(1, std::memory_order_relaxed);
                } else {
                    db.insertWay(item.id, item.name, item.tags, geog);
                    status.ways.fetch_add(1, std::memory_order_relaxed);
                }

            } else if constexpr (std::is_same_v<T, RelationEntry>) {
                if (!way_phase_done) {
                    LOGI(tid, "first relation — flushing ways, waiting at way_barrier");
                    db.finalize_ways();
                    way_barrier.arrive_and_wait();
                    LOGI(tid, "way_barrier passed");
                    way_phase_done = true;
                }

                // Store every relation in my_relations
                db.insertRelation(item.id, item.name, item.tags);
                status.relations.fetch_add(1, std::memory_order_relaxed);

                // Also merge route relations into my_roads
                if (item.tags.count("route")) {
                    std::vector<std::string> geoms;
                    for (int64_t wid : item.way_members) {
                        std::string g = db.getWay(wid);
                        if (!g.empty()) geoms.push_back(g);
                    }
                    if (!geoms.empty())
                        db.insertRoad(item.id, item.name, item.tags, mergeWayGeoms(geoms));
                }
            }
        }, entry);

        db.finalize_roads();
    }

    if (!way_phase_done) {
        LOGI(tid, "cleanup: flushing ways, waiting at way_barrier");
        db.finalize_ways();
        way_barrier.arrive_and_wait();
        LOGI(tid, "cleanup: way_barrier passed");
    }

    db.finalize_relations();
    LOGI(tid, "way thread done");
}

// ---- Status printer ----

void statusThread(const Status& s, std::atomic<bool>& done,
                  BlockingQueue<OSMEntry>& q) {
    auto start = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_relaxed)) {
        auto now      = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        int64_t total  = s.nodes + s.areas + s.ways + s.relations;
        double rps     = elapsed > 0 ? total / elapsed : 0;
        std::cout << "\rProgress: " << std::fixed << std::setprecision(2)
                  << s.progress.load() << "%  "
                  << "N:" << s.nodes << " A:" << s.areas
                  << " W:" << s.ways << " R:" << s.relations
                  << " Q:" << q.size()
                  << " | " << static_cast<int64_t>(rps) << " rec/s   "
                  << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ---- main ----

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    if (args.verbose)
        Log::get().open(args.log_file);

    LOGI(-1, "starting infile=", args.infile,
         " node_threads=", args.node_threads,
         " way_threads=", args.way_threads,
         " queue=", args.queue_size,
         " nodes_file=", args.nodes_file);

    LOGI(-1, "creating mmap files max_id=", args.node_count_hint);
    OSMMMap::createFile(args.nodes_file, args.node_count_hint);
    OSMMMap osmmap(args.nodes_file, args.node_count_hint);
    LOGI(-1, "mmap ready");

    BlockingQueue<OSMEntry> q(args.queue_size);
    std::barrier<>      node_barrier(args.node_threads);
    std::barrier<>      way_barrier(args.way_threads);
    std::atomic<bool>   nodes_done{false};
    std::mutex          mmap_mu;
    std::mutex          db_flush_mu;
    Status              status;
    std::atomic<bool>   status_done{false};

    // Launch way threads first — they block on q immediately
    LOGI(-1, "launching ", args.way_threads, " way threads");
    std::vector<std::thread> way_workers;
    way_workers.reserve(args.way_threads);
    for (int i = 0; i < args.way_threads; ++i)
        way_workers.emplace_back(wayThread,
                                 args.node_threads + i,
                                 std::ref(q), std::ref(status),
                                 std::ref(way_barrier),
                                 std::ref(osmmap), std::ref(mmap_mu),
                                 std::ref(db_flush_mu), std::cref(args));

    // Launch node threads
    LOGI(-1, "launching ", args.node_threads, " node threads");
    std::vector<std::thread> node_workers;
    node_workers.reserve(args.node_threads);
    for (int i = 0; i < args.node_threads; ++i)
        node_workers.emplace_back(nodeThread, i,
                                  std::ref(q), std::ref(status),
                                  std::ref(node_barrier),
                                  std::ref(nodes_done),
                                  std::ref(osmmap), std::ref(mmap_mu),
                                  std::ref(db_flush_mu), std::cref(args));

    auto t_status = std::thread(statusThread, std::cref(status),
                                std::ref(status_done), std::ref(q));

    auto start = std::chrono::steady_clock::now();
    OSMReader reader(args.infile);
    auto file_size = reader.getSize();
    std::vector<OSMEntry> batch;
    int64_t batch_count = 0;

    LOGI(-1, "reading PBF file_size=", static_cast<int64_t>(file_size));

    while (reader.next(batch)) {
        for (auto& entry : batch)
            q.push(std::move(entry));
        batch.clear();

        ++batch_count;
        if (batch_count % 500 == 0)
            LOGI(-1, "producer batch=", batch_count,
                 " pos=", static_cast<int64_t>(reader.getPosition()),
                 " q=", q.size());

        double pos = static_cast<double>(reader.getPosition());
        double sz  = static_cast<double>(file_size);
        status.progress.store(sz > 0 ? (pos / sz * 100.0) : 0.0,
                              std::memory_order_relaxed);
    }

    LOGI(-1, "producer done — waiting for node threads");
    for (auto& w : node_workers) w.join();
    LOGI(-1, "node threads done — shutting down queue");

    // Node threads have all passed the barrier. Now shut down so way
    // threads drain the remainder and exit.
    q.shutdown();

    LOGI(-1, "waiting for way threads");
    for (auto& w : way_workers) w.join();
    LOGI(-1, "all threads done");

    status_done.store(true, std::memory_order_relaxed);
    t_status.join();

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    int64_t total = status.nodes + status.areas + status.ways + status.relations;

    LOGI(-1, "done elapsed=", elapsed, "s total=", total);

    std::cout << "\nDone in " << elapsed << "s | "
              << "Nodes: " << status.nodes << ", "
              << "Areas: " << status.areas << ", "
              << "Ways: "  << status.ways  << ", "
              << "Relations: " << status.relations << " | "
              << static_cast<int64_t>(total / elapsed) << " rec/s\n";
    return 0;
}
