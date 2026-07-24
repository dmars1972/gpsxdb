// Throwaway validation for task #46: read a handful of real rows from
// public.nodes via PgCopyBinary::Reader and confirm (a) the bigint `id`
// field decodes correctly via readBigintBE -- the plan's flagged
// byte-order gotcha, wrong-endian id wouldn't crash, it'd silently poison
// the id->bitmask map -- by cross-checking against the same ids read
// through ordinary pqxx text-mode, and (b) the `geog` field decodes
// cleanly via WkbDecode and lands on plausible lon/lat. Not part of the
// production build -- compiled/run directly, see task #46.
#include "PgCopyBinary.h"
#include "WkbDecode.h"

#include <pqxx/pqxx>

#include <cstdio>
#include <set>
#include <string>
#include <unistd.h>  // _exit -- see the pqxx static-destructor double-free note below

int run(int argc, char** argv);

int main(int argc, char** argv) {
    int rc;
    try {
        rc = run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: uncaught exception: %s\n", e.what());
        rc = 1;
    }
    // regional_db_export.cpp already worked around a confirmed pqxx
    // static-destructor double-free on this system (any pqxx-linked
    // program crashes at normal exit via glibc's malloc corruption
    // check -- reproduced with a 3-line pqxx-only program, unrelated to
    // this tool's own code) by calling _exit() instead of returning from
    // main(). Same fix needed here since this test also links pqxx for
    // its ground-truth cross-check.
    std::fflush(stdout);
    _exit(rc);
}

int run(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string host = "localhost", db, user;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" && i + 1 < argc) host = argv[++i];
        else if (arg == "-d" && i + 1 < argc) db = argv[++i];
        else if (arg == "-u" && i + 1 < argc) user = argv[++i];
    }
    if (db.empty() || user.empty()) {
        std::fprintf(stderr, "usage: pgcopy_roundtrip_test -s <host> -d <db> -u <user>\n");
        return 2;
    }

    std::string conninfo = "host=" + host + " dbname=" + db + " user=" + user;
    int failures = 0;

    // Ground truth: same ids, read via ordinary pqxx text mode.
    std::set<int64_t> expected_ids;
    {
        pqxx::connection conn(conninfo);
        pqxx::work txn(conn);
        auto res = txn.exec("SELECT id FROM public.nodes ORDER BY id LIMIT 20");
        for (auto row : res) expected_ids.insert(row[0].as<int64_t>());
    }
    if (expected_ids.empty()) {
        std::fprintf(stderr, "FAIL: public.nodes returned no rows -- can't validate against an empty table\n");
        return 1;
    }
    std::printf("OK: read %zu ground-truth ids via pqxx\n", expected_ids.size());

    PgCopyBinary::Reader reader(conninfo, "SELECT id, geog FROM public.nodes ORDER BY id LIMIT 20");
    std::set<int64_t> got_ids;
    int row_count = 0;
    int geom_decoded = 0;
    while (auto row = reader.next()) {
        ++row_count;
        if (row->fields.size() != 2) {
            std::printf("FAIL: row %d has %zu fields, expected 2\n", row_count, row->fields.size());
            ++failures;
            continue;
        }
        int64_t id = PgCopyBinary::readBigintBE(row->fields[0]);
        got_ids.insert(id);

        const auto& geom_field = row->fields[1];
        if (geom_field.data == nullptr) {
            std::printf("row %d: id=%lld geog=NULL\n", row_count, static_cast<long long>(id));
            continue;
        }
        auto decoded = WkbDecode::decode(geom_field.data, static_cast<size_t>(geom_field.len));
        if (!decoded) {
            std::printf("FAIL: row %d id=%lld geog failed to decode (%d bytes)\n",
                        row_count, static_cast<long long>(id), geom_field.len);
            ++failures;
            continue;
        }
        auto* pt = std::get_if<WkbDecode::Point>(&decoded->geom);
        if (!pt) {
            std::printf("FAIL: row %d id=%lld geog decoded but is not a Point\n",
                        row_count, static_cast<long long>(id));
            ++failures;
            continue;
        }
        ++geom_decoded;
        std::printf("OK: row %d id=%lld srid=%d lon=%.6f lat=%.6f\n",
                    row_count, static_cast<long long>(id), decoded->srid, pt->x(), pt->y());
    }

    if (row_count != static_cast<int>(expected_ids.size())) {
        std::printf("FAIL: read %d rows via PgCopyBinary, expected %zu\n", row_count, expected_ids.size());
        ++failures;
    } else {
        std::printf("OK: row count matches (%d)\n", row_count);
    }

    if (got_ids != expected_ids) {
        std::printf("FAIL: id set mismatch between PgCopyBinary and pqxx ground truth "
                    "-- likely a byte-order bug in readBigintBE\n");
        ++failures;
    } else {
        std::printf("OK: id set matches pqxx ground truth exactly (byte order confirmed correct)\n");
    }

    if (geom_decoded == 0) {
        std::printf("FAIL: zero geometries decoded successfully\n");
        ++failures;
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
