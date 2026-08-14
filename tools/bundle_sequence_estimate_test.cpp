// Verification for regional_install.cpp's estimateBundleReplicationSequence()
// / fetchReplicationState() (added to auto-seed a region-aware poll
// process's starting OSM replication sequence from a bundle's
// manifest.txt "exported_at" field -- see regional_install.cpp step 7).
// Duplicates the two functions under test (they live in regional_install.cpp's
// anonymous namespace, not exposed via a header) rather than #including
// regional_install.cpp directly, matching this repo's convention for this
// kind of algorithm-verification tool (e.g. region_index_regression_test.cpp).
// Requires network access to fetch the real, live replication state.txt.
// Not part of the production build -- compiled/run directly.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; } \
    else std::fprintf(stdout, "ok: %s\n", msg); \
} while (0)

// ---- verbatim copies of the functions under test ----

std::string readManifestField(const std::string& manifest_path, const std::string& key) {
    std::ifstream f(manifest_path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return "";
}

std::string captureOutput(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

bool fetchReplicationState(const std::string& url, int64_t& seq_out, time_t& time_out) {
    std::string cmd = "curl -fsL '" + url + "'";
    std::string output = captureOutput(cmd);
    if (output.empty()) return false;

    std::istringstream f(output);
    std::string line, timestamp_raw;
    int64_t seq = -1;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("sequenceNumber=", 0) == 0) {
            try { seq = std::stoll(line.substr(15)); } catch (...) { return false; }
        } else if (line.rfind("timestamp=", 0) == 0) {
            timestamp_raw = line.substr(10);
        }
    }
    if (seq < 0 || timestamp_raw.empty()) return false;

    std::string ts;
    for (size_t i = 0; i < timestamp_raw.size(); ++i) {
        if (timestamp_raw[i] == '\\' && i + 1 < timestamp_raw.size() && timestamp_raw[i + 1] == ':') {
            ts += ':'; ++i;
        } else {
            ts += timestamp_raw[i];
        }
    }

    struct tm tmv{};
    if (!strptime(ts.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tmv)) return false;
    time_out = timegm(&tmv);
    seq_out = seq;
    return true;
}

int64_t estimateBundleReplicationSequence(const std::string& manifest_path) {
    std::string exported_at_str = readManifestField(manifest_path, "exported_at");
    if (exported_at_str.empty()) return -1;
    int64_t exported_at;
    try { exported_at = std::stoll(exported_at_str); } catch (...) { return -1; }

    int64_t tip_seq;
    time_t tip_time;
    if (!fetchReplicationState("https://planet.openstreetmap.org/replication/minute/state.txt", tip_seq, tip_time))
        return -1;

    constexpr int64_t kSecondsPerSequence = 60;
    constexpr int64_t kSafetyMarginSeconds = 3LL * 24 * 3600;

    int64_t seconds_back = (static_cast<int64_t>(tip_time) - exported_at) + kSafetyMarginSeconds;
    if (seconds_back < 0) seconds_back = 0;
    int64_t seq_back = seconds_back / kSecondsPerSequence;
    int64_t estimated = tip_seq - seq_back;
    return estimated > 0 ? estimated : 0;
}

// ---- tests ----

int main(int argc, char** argv) {
    // Sanity: fetchReplicationState against the real, live endpoint.
    int64_t tip_seq;
    time_t tip_time;
    bool ok = fetchReplicationState("https://planet.openstreetmap.org/replication/minute/state.txt", tip_seq, tip_time);
    CHECK(ok, "fetchReplicationState succeeds against the real live endpoint");
    if (ok) {
        CHECK(tip_seq > 7000000, "live tip sequence is in a plausible range (>7M as of 2026)");
        time_t now = time(nullptr);
        double age_hours = difftime(now, tip_time) / 3600.0;
        CHECK(age_hours >= 0 && age_hours < 24, "live tip timestamp is recent (<24h old)");
    }

    // Missing exported_at (e.g. bundle predates the field) -> -1, non-fatal.
    {
        std::string path = "/tmp/bset_missing.txt";
        std::ofstream f(path);
        f << "region=testregion\nbbox=1,2,3,4\n";
        f.close();
        int64_t seed = estimateBundleReplicationSequence(path);
        CHECK(seed == -1, "missing exported_at field returns -1, not a crash");
        std::remove(path.c_str());
    }

    // Synthetic exported_at exactly matching the live tip's own timestamp
    // -> estimate should land within one safety-margin window of the tip
    // (can't assert an exact value since the live tip keeps advancing,
    // but it must be sane: <= tip_seq, and no more than roughly
    // safety_margin/60 + a few minutes of slack below it).
    if (ok) {
        std::string path = "/tmp/bset_synthetic.txt";
        std::ofstream f(path);
        f << "region=testregion\nbbox=1,2,3,4\nexported_at=" << static_cast<int64_t>(tip_time) << "\n";
        f.close();
        int64_t seed = estimateBundleReplicationSequence(path);
        CHECK(seed >= 0, "synthetic exported_at=tip_time produces a valid (non-negative) estimate");
        CHECK(seed <= tip_seq, "estimate never exceeds the live tip sequence");
        int64_t expected_back = (3LL * 24 * 3600) / 60;  // 3-day margin / 60s per sequence
        int64_t actual_back = tip_seq - seed;
        CHECK(actual_back >= expected_back - 5 && actual_back <= expected_back + 5,
              "estimate is ~3 days' worth of sequences behind the tip (safety margin applied)");
        std::remove(path.c_str());
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "\nALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
    return 1;
}
