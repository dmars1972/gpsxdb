// Verification for task #62 (auto-grow nodes.dat in poll/delta mode):
// exercises OSMMMap::growMergedIfNeeded() and the constructor's
// growFileIfNeeded() restart-durability fix directly, without needing a
// live poll process or real OSM data. Not part of the production build --
// compiled/run directly, see task #62.
#include "OSMMMap.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; } \
    else std::fprintf(stdout, "ok: %s\n", msg); \
} while (0)

static long fileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

int main() {
    const std::string base = "/tmp/osmmmap_autogrow_test.dat";
    const std::string dat = base;
    const std::string bmp = base + ".bmp";
    std::remove(dat.c_str());
    std::remove(bmp.c_str());

    const int64_t small_max_id = 100;

    // --- Test 1: grow-on-write ---
    OSMMMap::createFile(base, small_max_id, /*num_shards=*/1, "/tmp");
    long dat_before = fileSize(dat);
    long bmp_before = fileSize(bmp);
    CHECK(dat_before == (small_max_id + 1) * 16, "initial .dat sized from small max_id");
    CHECK(bmp_before == (small_max_id + 1 + 7) / 8, "initial .bmp sized from small max_id");

    const int64_t big_id = 10'000; // well beyond small_max_id
    {
        OSMMMap m(base, small_max_id, 1, "/tmp", /*open_shards_for_write=*/false);
        m.update(big_id, -122.4, 37.7);

        auto v = m.select(big_id);
        CHECK(v.has_value(), "select() finds the just-grown-and-written id in the same instance");
        if (v) CHECK(v->first == -122.4 && v->second == 37.7, "coordinates round-trip correctly");
    }
    long dat_after = fileSize(dat);
    long bmp_after = fileSize(bmp);
    CHECK(dat_after > dat_before, ".dat file actually grew on disk");
    CHECK(bmp_after > bmp_before, ".bmp file actually grew on disk");
    CHECK(dat_after >= (big_id + 1) * 16, ".dat grew to at least cover big_id");

    // --- Test 2: restart durability with the ORIGINAL small max_id ---
    {
        OSMMMap m2(base, small_max_id, 1, "/tmp", /*open_shards_for_write=*/false);
        auto v = m2.select(big_id);
        CHECK(v.has_value(), "restart with stale small -n still finds the previously-grown id");
        if (v) CHECK(v->first == -122.4 && v->second == 37.7, "restart-recovered coordinates still correct");

        // A normal small id written before the grow should also still be there.
        m2.update(5, 1.0, 2.0);
        auto v5 = m2.select(5);
        CHECK(v5.has_value() && v5->first == 1.0 && v5->second == 2.0, "small ids unaffected by the grow");
    }
    long dat_after_restart = fileSize(dat);
    CHECK(dat_after_restart == dat_after, "restart does not shrink or re-grow the file unnecessarily");

    // --- Test 3: sanity ceiling rejection ---
    {
        OSMMMap m3(base, small_max_id, 1, "/tmp", /*open_shards_for_write=*/false);
        long dat_before_ceiling = fileSize(dat);
        const int64_t insane_id = 200'000'000'000LL; // past kMaxSaneNodeId (100B)
        m3.update(insane_id, 0.0, 0.0);
        long dat_after_ceiling = fileSize(dat);
        CHECK(dat_after_ceiling == dat_before_ceiling, "implausible id does not trigger a grow");
        auto v = m3.select(insane_id);
        CHECK(!v.has_value(), "implausible id is not stored (select() returns nullopt)");
    }

    std::remove(dat.c_str());
    std::remove(bmp.c_str());

    if (g_failures == 0) {
        std::fprintf(stdout, "\nALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
    return 1;
}
