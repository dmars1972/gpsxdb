// Verification for the RegionalNodeMap mmap fix (crash: std::bad_alloc
// loading a 66GB north_america regional file on a 15GB-RAM Raspberry Pi 5
// under the previous owned-buffer design). Exercises Writer -> select()
// and merge() against real files on disk to confirm the mmap-backed read
// path (POSIX) behaves identically to the old owned-buffer semantics. Not
// part of the production build -- compiled/run directly.
#include "RegionalNodeMap.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; } \
    else std::fprintf(stdout, "ok: %s\n", msg); \
} while (0)

int main() {
    const std::string path_a = "/tmp/rnm_test_a.dat";
    const std::string path_b = "/tmp/rnm_test_b.dat";
    const std::string merged = "/tmp/rnm_test_merged.dat";
    std::remove(path_a.c_str());
    std::remove(path_b.c_str());
    std::remove(merged.c_str());

    RegionalNodeMap::Bbox bbox{-10.0, -10.0, 10.0, 10.0};

    // File A: ids 1, 3, 5, 7 (ascending, as required)
    {
        RegionalNodeMap::Writer w(path_a, "testregion", bbox);
        w.append(1, 100.0, 200.0);
        w.append(3, 300.0, 400.0);
        w.append(5, 500.0, 600.0);
        w.append(7, 700.0, 800.0);
        w.finalize();
    }
    // File B: ids 2, 3 (overlapping id 3, dedup should keep one), 4, 8
    {
        RegionalNodeMap::Writer w(path_b, "testregion", bbox);
        w.append(2, 150.0, 250.0);
        w.append(3, 300.0, 400.0);  // same coords as A's id 3 -- dedup case
        w.append(4, 350.0, 450.0);
        w.append(8, 900.0, 999.0);
        w.finalize();
    }

    // Read A back via the mmap-backed constructor and select()
    {
        RegionalNodeMap a(path_a);
        CHECK(a.regionName() == "testregion", "region name round-trips");
        CHECK(a.recordCount() == 4, "record count correct after Writer");
        auto v1 = a.select(1);
        CHECK(v1.has_value() && v1->first == 100.0 && v1->second == 200.0, "select(1) correct");
        auto v5 = a.select(5);
        CHECK(v5.has_value() && v5->first == 500.0 && v5->second == 600.0, "select(5) correct");
        auto vmiss = a.select(6);
        CHECK(!vmiss.has_value(), "select() on missing id returns nullopt");
        auto vlow = a.select(0);
        CHECK(!vlow.has_value(), "select() below range returns nullopt");
        auto vhigh = a.select(100);
        CHECK(!vhigh.has_value(), "select() above range returns nullopt");
    }

    // Merge A and B, verify the combined result
    bool ok = RegionalNodeMap::merge(path_a, path_b, merged);
    CHECK(ok, "merge() returns true");
    {
        RegionalNodeMap m(merged);
        CHECK(m.recordCount() == 7, "merged record count is 7 (8 input - 1 dedup)");
        for (int64_t id : {1, 2, 3, 4, 5, 7, 8}) {  // 6 was never written to either input
            auto v = m.select(id);
            CHECK(v.has_value(), ("merged select() finds id " + std::to_string(id)).c_str());
        }
        CHECK(!m.select(6).has_value(), "merged select() correctly misses never-written id 6");
        auto v3 = m.select(3);
        CHECK(v3.has_value() && v3->first == 300.0 && v3->second == 400.0, "merged dedup'd id 3 has correct coords");
    }

    std::remove(path_a.c_str());
    std::remove(path_b.c_str());
    std::remove(merged.c_str());

    if (g_failures == 0) {
        std::fprintf(stdout, "\nALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
    return 1;
}
