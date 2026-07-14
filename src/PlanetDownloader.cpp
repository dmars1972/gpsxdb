#include "PlanetDownloader.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <sys/statvfs.h>
#include <filesystem>
#include <curl/curl.h>

namespace {

constexpr const char* kPlanetUrl    = "https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf";
constexpr const char* kPlanetMd5Url = "https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf.md5";
constexpr const char* kMinuteStateUrl = "https://planet.openstreetmap.org/replication/minute/state.txt";

// Runs cmd, returning its stdout with trailing whitespace trimmed (empty on
// failure to launch). Used for small, fast commands only (HEAD request,
// md5sum) — never for the multi-hour planet download itself, which uses
// plain system() so its own progress meter/output stays live on the
// terminal instead of being buffered through a pipe.
std::string captureOutput(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

// Remote file size via a HEAD request, following redirects. -1 on failure.
// Redirect chains can each carry their own Content-Length header; tail -1
// keeps the final (actual resource) one.
long long remoteFileSize(const std::string& url) {
    std::string cmd = "curl -sI -L \"" + url + "\" | grep -i '^content-length:' | tail -1";
    std::string line = captureOutput(cmd);
    auto pos = line.find(':');
    if (pos == std::string::npos) return -1;
    try { return std::stoll(line.substr(pos + 1)); }
    catch (...) { return -1; }
}

// HEAD request for the remote Last-Modified time (Unix epoch seconds).
// Returns -1 if the server doesn't report one or the request fails.
long remoteLastModifiedEpoch(const std::string& url) {
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

// Downloads and parses a replication state.txt (osmosis .properties
// format: "sequenceNumber=N" and "timestamp=2026-07-14T23\:02\:22Z", with
// ':' escaped as '\:' since it's the .properties key/value separator).
// Returns false if either field can't be found/parsed.
bool fetchReplicationState(const std::string& url, int64_t& seq_out, time_t& time_out) {
    std::string tmp = "/tmp/osm_state_probe.txt";
    std::string cmd = "curl -fsL -o " + tmp + " \"" + url + "\"";
    if (system(cmd.c_str()) != 0) return false;

    std::ifstream f(tmp);
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
    std::remove(tmp.c_str());
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

} // namespace

bool downloadLatestPlanet(const std::string& dest_path, bool verbose) {
    namespace fs = std::filesystem;
    fs::path dest(dest_path);
    fs::path dir = dest.has_parent_path() ? dest.parent_path() : fs::path(".");
    system(("mkdir -p " + dir.string()).c_str());

    if (verbose) std::cout << "[Planet] checking remote file size...\n";
    long long remote_size = remoteFileSize(kPlanetUrl);
    if (remote_size <= 0) {
        std::cerr << "[Planet] could not determine remote file size — network issue? aborting\n";
        return false;
    }
    if (verbose)
        std::cout << "[Planet] remote size: " << (remote_size / (1024*1024*1024)) << " GB\n";

    std::error_code ec;
    long long existing = fs::exists(dest, ec) ? static_cast<long long>(fs::file_size(dest, ec)) : 0;

    struct statvfs vfs;
    if (statvfs(dir.string().c_str(), &vfs) == 0) {
        long long avail = static_cast<long long>(vfs.f_bavail) * vfs.f_frsize;
        long long needed = remote_size - existing;  // resuming needs less
        if (needed > 0 && needed > avail) {
            std::cerr << "[Planet] insufficient disk space at " << dir.string() << ": need "
                      << (needed / (1024*1024*1024)) << " GB more, only "
                      << (avail / (1024*1024*1024)) << " GB available\n";
            return false;
        }
    }

    if (verbose)
        std::cout << "[Planet] downloading " << kPlanetUrl << "\n"
                   << "[Planet]   -> " << dest_path
                   << (existing > 0 ? " (resuming partial download)\n" : "\n");

    // -C - resumes automatically if dest_path already partially exists
    // (and is a plain restart if it doesn't). --fail turns HTTP error
    // status codes into a non-zero exit rather than writing the error
    // page's body to dest_path as if it were data. Deliberately no
    // --silent: curl's own progress meter is exactly what you want visible
    // for a transfer this size.
    std::ostringstream cmd;
    cmd << "curl -f -L -C - --retry 5 --retry-delay 10 -o " << dest_path << " " << kPlanetUrl;
    int rc = system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << "[Planet] download failed (curl exit status " << rc
                  << ") — re-run to resume from where it left off\n";
        return false;
    }

    if (verbose) std::cout << "[Planet] download complete, verifying checksum...\n";
    std::string md5_tmp = dest_path + ".md5";
    std::ostringstream md5cmd;
    md5cmd << "curl -fsL -o " << md5_tmp << " " << kPlanetMd5Url;
    if (system(md5cmd.str().c_str()) != 0) {
        std::cerr << "[Planet] warning: could not fetch " << kPlanetMd5Url
                  << " — skipping checksum verification\n";
        return true;
    }

    std::ifstream md5_file(md5_tmp);
    std::string expected_md5;
    md5_file >> expected_md5;
    md5_file.close();
    std::remove(md5_tmp.c_str());

    std::string actual_md5 = captureOutput("md5sum " + dest_path + " | cut -d' ' -f1");

    if (expected_md5.empty() || actual_md5.empty() || expected_md5 != actual_md5) {
        std::cerr << "[Planet] CHECKSUM MISMATCH — expected " << expected_md5
                  << ", got " << actual_md5
                  << ". File is likely corrupted/truncated; re-run to retry.\n";
        return false;
    }

    if (verbose) std::cout << "[Planet] checksum verified OK (" << actual_md5 << ")\n";
    return true;
}

int64_t estimatePlanetReplicationSequence() {
    long dump_mtime = remoteLastModifiedEpoch(kPlanetUrl);
    if (dump_mtime <= 0) {
        std::cerr << "[Planet] could not determine planet-latest.osm.pbf's Last-Modified time\n";
        return -1;
    }

    int64_t tip_seq;
    time_t tip_time;
    if (!fetchReplicationState(kMinuteStateUrl, tip_seq, tip_time)) {
        std::cerr << "[Planet] could not fetch/parse " << kMinuteStateUrl << "\n";
        return -1;
    }

    constexpr int64_t kSecondsPerSequence = 60;             // minute granularity
    constexpr int64_t kSafetyMarginSeconds = 3LL * 24 * 3600; // 3 days, err early not late

    int64_t seconds_back = (static_cast<int64_t>(tip_time) - dump_mtime) + kSafetyMarginSeconds;
    if (seconds_back < 0) seconds_back = 0;
    int64_t seq_back = seconds_back / kSecondsPerSequence;
    int64_t estimated = tip_seq - seq_back;
    return estimated > 0 ? estimated : 0;
}
