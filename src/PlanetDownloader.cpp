#include "PlanetDownloader.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cctype>
#include <sys/statvfs.h>
#include <filesystem>

namespace {

constexpr const char* kPlanetUrl    = "https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf";
constexpr const char* kPlanetMd5Url = "https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf.md5";

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
