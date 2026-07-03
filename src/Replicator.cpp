#include "Replicator.h"
#include "NavDB.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_stop{false};

static void sigintHandler(int) { g_stop.store(true); }

// ---- helpers ----

static bool downloadFile(const std::string& url, const std::string& dest) {
    std::string cmd = "curl -sfL --retry 3 --retry-delay 5 -o " +
                      dest + " \"" + url + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

// ---- Replicator ----

Replicator::Replicator(DeltaApplier& applier, NavDB& db,
                        ReplicationGranularity granularity)
    : applier_(applier), db_(db), granularity_(granularity) {}

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

void Replicator::poll(int interval_seconds) {
    signal(SIGINT, sigintHandler);

    std::cout << "Starting replication polling (interval=" 
              << interval_seconds << "s)\n";
    std::cout << "Press Ctrl+C to stop\n";

    while (!g_stop.load()) {
        try {
            int64_t local  = getSequence();
            int64_t remote = remoteSequence();

            if (local < 0) {
                std::cerr << "No local sequence number — set one with -S or run initial import first\n";
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
