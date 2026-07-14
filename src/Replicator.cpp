#include "Replicator.h"
#include "NavDB.h"
#include "AirportsLoader.h"
#include "FAAObstacleLoader.h"
#include "WMMLoader.h"
#include "AirspaceLoader.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <ctime>
#include <curl/curl.h>

static std::atomic<bool> g_stop{false};

static void sigintHandler(int) { g_stop.store(true); }

// ---- helpers ----

static bool downloadFile(const std::string& url, const std::string& dest) {
    std::string cmd = "curl -sfL --retry 3 --retry-delay 5 -o " +
                      dest + " \"" + url + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

// HEAD request for the remote Last-Modified time (Unix epoch seconds).
// Returns -1 if the server doesn't report one or the request fails.
static long remoteLastModified(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    long filetime = -1;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime);
    curl_easy_cleanup(curl);
    return filetime;
}

// ---- Replicator ----

Replicator::Replicator(DeltaApplier& applier, NavDB& db,
                        ReplicationGranularity granularity,
                        std::string server, std::string user,
                        std::string database)
    : applier_(applier), db_(db), granularity_(granularity),
      server_(std::move(server)), user_(std::move(user)),
      database_(std::move(database)) {}

std::string Replicator::baseUrl() const {
    switch (granularity_) {
        case ReplicationGranularity::Minute:
            return "https://planet.openstreetmap.org/replication/minute";
        case ReplicationGranularity::Hour:
            return "https://planet.openstreetmap.org/replication/hour";
        case ReplicationGranularity::Day:
            return "https://planet.openstreetmap.org/replication/day";
    }
    return "";
}

// Convert sequence number to 3-level path e.g. 5123456 -> "005/123/456"
std::string Replicator::sequenceToPath(int64_t seq) const {
    std::ostringstream ss;
    ss << std::setw(3) << std::setfill('0') << (seq / 1000000) << "/"
       << std::setw(3) << std::setfill('0') << ((seq / 1000) % 1000) << "/"
       << std::setw(3) << std::setfill('0') << (seq % 1000);
    return ss.str();
}

void Replicator::applyFile(const std::string& path) {
    std::cout << "Applying " << path << "\n";
    OSCReader reader(path);
    int64_t n = reader.parse([&](OSCChange&& c) {
        applier_.apply(std::move(c));
    });
    applier_.flush();
    std::cout << "Applied " << n << " changes ("
              << "created=" << applier_.created()
              << " modified=" << applier_.modified()
              << " deleted=" << applier_.deleted() << ")\n";
}

int64_t Replicator::remoteSequence() {
    std::string url  = baseUrl() + "/state.txt";
    std::string tmp  = "/tmp/osm_state.txt";
    if (!downloadFile(url, tmp))
        throw std::runtime_error("Failed to download " + url);

    std::ifstream f(tmp);
    std::string line;
    while (std::getline(f, line)) {
        // Strip carriage return in case of Windows line endings (\r\n)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("sequenceNumber=") == 0) {
            return std::stoll(line.substr(15));
        }
    }
    // Log the file contents to help diagnose unexpected formats
    std::cerr << "[Replicator] state.txt contents:\n";
    std::ifstream dbg(tmp);
    std::string dbg_line;
    while (std::getline(dbg, dbg_line)) std::cerr << "  " << dbg_line << "\n";
    throw std::runtime_error("Could not parse sequenceNumber from state.txt");
}

bool Replicator::downloadAndApply(int64_t seq) {
    std::string path = sequenceToPath(seq);
    std::string url  = baseUrl() + "/" + path + ".osc.gz";
    std::string tmp  = "/tmp/osm_delta_" + std::to_string(seq) + ".osc.gz";

    std::cout << "Downloading sequence " << seq << " (" << url << ")\n";

    if (!downloadFile(url, tmp)) {
        std::cerr << "Failed to download sequence " << seq << "\n";
        return false;
    }

    try {
        applyFile(tmp);
        setSequence(seq);
        std::remove(tmp.c_str());
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error applying sequence " << seq << ": " << e.what() << "\n";
        std::remove(tmp.c_str());
        return false;
    }
}

// Sources checked for upstream updates. OurAirports refreshes all of its
// CSVs together, so checking airports.csv alone is a reliable proxy for
// "the OurAirports dataset was refreshed".
namespace {
struct ExternalSource { const char* name; const char* url; };
const ExternalSource kExternalSources[] = {
    {"airports",      "https://davidmegginson.github.io/ourairports-data/airports.csv"},
    {"faa_obstacles", "https://aeronav.faa.gov/Obst_Data/DAILY_DOF_CSV.ZIP"},
};
}

// CDNs serving these static files (GitHub Pages/Fastly for OurAirports, at
// least) don't report a perfectly stable Last-Modified across requests —
// observed jitter of ~1 second between consecutive HEAD requests with no
// real content change. Both datasets only actually refresh roughly once a
// day, so a tolerance well under that comfortably absorbs CDN jitter without
// missing real updates.
constexpr int64_t kUpdateToleranceSeconds = 300;

void Replicator::checkExternalData() {
    for (const auto& src : kExternalSources) {
        long remote_mtime = remoteLastModified(src.url);
        if (remote_mtime < 0) {
            std::cerr << "[Replicator] could not check " << src.name
                      << " for updates (no Last-Modified from server)\n";
            continue;
        }

        int64_t known_mtime = db_.getExternalDataTimestamp(src.name);
        if (known_mtime >= 0 &&
            std::abs(remote_mtime - known_mtime) <= kUpdateToleranceSeconds)
            continue;

        std::cout << "[Replicator] " << src.name
                  << " data has been updated upstream, reloading...\n";
        try {
            bool ok = (std::string(src.name) == "airports")
                ? AirportsLoader(server_, user_, database_).load(false)
                : FAAObstacleLoader(server_, user_, database_).load(false);
            if (ok) {
                db_.setExternalDataTimestamp(src.name, remote_mtime);
                std::cout << "[Replicator] " << src.name << " reload complete\n";
            } else {
                std::cerr << "[Replicator] " << src.name
                          << " reload skipped (download failed), will retry next check\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Replicator] " << src.name
                      << " reload failed: " << e.what() << "\n";
        }
    }
}

// ~3 months. Tracked via external_data_state (keyed by "wmm") rather than
// an in-memory timer, so the interval is measured from the last real
// reload's wall-clock time and survives process restarts — an in-memory
// steady_clock timer (like kExternalCheckInterval above) would instead
// reload on every poll() restart regardless of how recently WMM was
// actually refreshed, which is fine for a cheap HTTP HEAD check but not
// for a 3-month cadence.
constexpr int64_t kWMMRefreshSeconds = 90LL * 24 * 3600;

void Replicator::checkWMMRefresh() {
    int64_t now = std::time(nullptr);
    int64_t last = db_.getExternalDataTimestamp("wmm");
    if (last >= 0 && (now - last) < kWMMRefreshSeconds) return;

    std::cout << "[Replicator] refreshing WMM declination data (3-month cadence)...\n";
    try {
        bool ok = WMMLoader(server_, user_, database_).load(currentDecimalYear());
        if (ok) {
            db_.setExternalDataTimestamp("wmm", now);
            std::cout << "[Replicator] WMM refresh complete\n";
        } else {
            std::cerr << "[Replicator] WMM refresh failed, will retry next check\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[Replicator] WMM refresh failed: " << e.what() << "\n";
    }
}

// FAA's NASR/SUA subscription publishes on an ~8-week cycle; OpenAIP is
// crowd-sourced with no fixed cadence at all. A flat 1-month refresh is
// simpler to reason about than matching FAA's exact cycle and still keeps
// data reasonably current. Same fixed-interval, DB-persisted-timestamp
// approach as checkWMMRefresh, for the same reason (no single upstream
// Last-Modified to check against).
constexpr int64_t kAirspaceRefreshSeconds = 30LL * 24 * 3600;

void Replicator::checkAirspaceRefresh() {
    int64_t now = std::time(nullptr);
    int64_t last = db_.getExternalDataTimestamp("airspace");
    if (last >= 0 && (now - last) < kAirspaceRefreshSeconds) return;

    std::cout << "[Replicator] refreshing airspace data (8-week cadence)...\n";
    try {
        AirspaceLoader airspace(server_, user_, database_);
        bool ok = airspace.loadClassAirspace(false);
        ok = airspace.loadSpecialUseAirspace(false) && ok;
        std::string openaip_key = defaultOpenAipApiKey();
        if (!openaip_key.empty()) {
            ok = airspace.loadInternationalAirspace(openaip_key, false) && ok;
        } else {
            std::cerr << "[Replicator] no OpenAIP API key (~/.openaip_api_key) — "
                         "skipping international airspace refresh\n";
        }
        if (ok) {
            db_.setExternalDataTimestamp("airspace", now);
            std::cout << "[Replicator] airspace refresh complete\n";
        } else {
            std::cerr << "[Replicator] airspace refresh had failures, will retry next check\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[Replicator] airspace refresh failed: " << e.what() << "\n";
    }
}

void Replicator::poll(int interval_seconds) {
    signal(SIGINT, sigintHandler);
    signal(SIGTERM, sigintHandler);

    std::cout << "Starting replication polling (interval="
              << interval_seconds << "s)\n";
    std::cout << "Press Ctrl+C to stop\n";

    // Independent of OSM replication cadence, occasionally check whether
    // OurAirports / FAA obstacle data has been refreshed upstream. Checked
    // inline (not just between catch-up passes) so a long initial catch-up
    // doesn't delay this by hours.
    constexpr auto kExternalCheckInterval = std::chrono::hours(6);
    auto last_external_check = std::chrono::steady_clock::now() - kExternalCheckInterval;

    auto maybeCheckExternalData = [&] {
        auto now = std::chrono::steady_clock::now();
        if (now - last_external_check >= kExternalCheckInterval) {
            checkExternalData();
            // checkWMMRefresh()/checkAirspaceRefresh() each have their own
            // internal (DB-persisted) gate — checking every 6h here just
            // decides how promptly a due refresh gets picked up, not how
            // often either actually reloads.
            checkWMMRefresh();
            checkAirspaceRefresh();
            last_external_check = now;
        }
    };

    while (!g_stop.load()) {
        try {
            maybeCheckExternalData();

            int64_t local  = getSequence();
            int64_t remote = remoteSequence();

            if (local < 0) {
                std::cerr << "No local sequence number — set one with -Q, or run "
                             "-I --download-planet first (auto-seeds it)\n";
                break;
            }

            if (local >= remote) {
                std::cout << "Up to date at sequence " << local
                          << " (remote=" << remote << "), sleeping...\n";
                for (int i = 0; i < interval_seconds && !g_stop.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Apply all pending sequences
            for (int64_t seq = local + 1; seq <= remote && !g_stop.load(); ++seq) {
                if (!downloadAndApply(seq)) {
                    std::cerr << "Stopping due to error at sequence " << seq << "\n";
                    g_stop.store(true);
                    break;
                }
                maybeCheckExternalData();
            }

        } catch (const std::exception& e) {
            std::cerr << "Replication error: " << e.what() << ", retrying in "
                      << interval_seconds << "s\n";
            for (int i = 0; i < interval_seconds && !g_stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "\nReplication stopped at sequence " << getSequence() << "\n";
}

int64_t Replicator::getSequence() {
    return db_.getReplicationSequence();
}

void Replicator::setSequence(int64_t seq) {
    db_.setReplicationSequence(seq);
}
