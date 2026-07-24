// Client-side single-pass export of the 5 big parent+child table pairs
// (ways/way_tags, areas/area_tags, roads/road_tags, relations/
// relation_tags, nodes/node_tags) into --out-dir/<region>/<table>.bin —
// the single-pass counterpart to regional_db_export.cpp's remaining
// (smaller) tables, which still uses one ST_Intersects query per region
// per table. See /home/daniel/.claude/plans/elegant-skipping-parasol.md
// (task #48) for the full design rationale: Postgres's planner drives a
// per-region ST_Intersects predicate as a per-region indexed scan no
// matter how the SQL is structured (confirmed via EXPLAIN this session),
// so for these 5 pairs -- read up to 22 times each under the old
// approach -- this tool instead does ONE unfiltered COPY BINARY pass per
// table (a plain, cheap PK range scan server-side), decodes each row's
// geometry client-side (WkbDecode.h), and classifies it against every
// region's real polygon in-process (RegionIndex.h, the same
// rtree-per-region infrastructure regional_export.cpp uses for
// nodes.dat).
//
// Usage: regional_table_export -s <host> -d <db> -u <user> --out-dir <dir>
//                               [--regions-dir <dir>] [--regions name1,name2,...]
//                               [--threads N] [--parallel-table-pairs N]
//                               [--extra-vertices-only] [-v]
//
// Alongside each region's <table>.bin files, the ways/areas/roads/relations
// parent passes also write <region>/extra_vertices.bin: every vertex of a
// matched geometry that falls OUTSIDE that same region's own polygon (a
// border-crossing way/area/road can legitimately reference such vertices).
// This is the "fix at the source" for regional_export.cpp's nodes.dat --
// a node only lands in a region's node store today if its own coordinate is
// inside the polygon, which is incomplete for a region-aware poll process
// that needs to resolve *every* vertex of an in-region way. regional_export
// must run AFTER this tool now (reversed from the original pipeline order)
// so it can fold extra_vertices.bin into its node classification -- see
// that file's top-of-file comment. extra_vertices.bin is a flat array of
// raw {double lon_m; double lat_m;} pairs, no header -- an internal
// artifact consumed only by regional_export, never shipped in a bundle.
// --extra-vertices-only skips writing the normal table/tag output (and
// skips the child-table pass and the `nodes` pair entirely, since neither
// contributes vertices) -- used for the one-time re-run needed to widen
// already-built bundles' node files without re-exporting already-correct
// table data. See /home/daniel/.claude/plans/elegant-skipping-parasol.md
// for the full design rationale.
//
// Per table pair: the parent table's id range (MIN/MAX id) is split into
// `--threads` disjoint windows; each worker gets its own raw libpq
// connection (PgCopyBinary::Reader) doing `COPY (SELECT * FROM
// public.<parent> WHERE id BETWEEN <lo> AND <hi>) TO STDOUT WITH (FORMAT
// binary)` -- a cheap indexed PK range scan, full parallelism
// client-side. Each worker builds its own local sorted (id, region
// bitmask) vector, storing only ids that matched at least one region (a
// row matching zero regions, or a relation with a NULL geog, is simply
// absent -- see WkbDecode::GeomVariant's std::monostate case). The child
// pass reuses the IDENTICAL id-range partition (same worker index means
// the same [lo,hi] window, so a child row's id -- always some existing
// parent id -- falls in that worker's own local map, no cross-thread
// lookups needed). Matched rows from both passes go to per-thread-per-
// region temp files (headerless, just length-prefixed raw COPY row bytes
// -- see TempRowWriter), merged into the final <region>/<table>.bin at
// the end with one real COPY-binary header/trailer (mergeTempFiles).
//
// `--parallel-table-pairs` (default 1) controls how many of the 5 pairs
// run concurrently; each pair's local maps are freed once that pair's
// merge completes, bounding peak memory to whichever pairs are
// concurrently in flight rather than holding all 5 pairs' id maps live
// at once. Region polygons are projected once per distinct SRID
// encountered across the 5 pairs (cached), not once per pair.
#include "GeoUtils.h"
#include "PgCopyBinary.h"
#include "RegionIndex.h"
#include "RegionPolygons.h"
#include "Regions.h"
#include "WkbDecode.h"

#include <boost/geometry.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <pqxx/pqxx>

// GeoUtils.h declares this extern — defined once per executable.
int g_srid = 3857;

namespace {

// One parent+child pair. `parent_geom_field` is the 0-based field index
// of the `geog` column in `SELECT * FROM public.<parent>` — id is always
// field 0 for every table here (NavDB.cpp's DDL puts the bigint PK/FK
// first in every case), but geog's position varies: nodes has two extra
// coordinate columns before it (id, name, longitude_m, latitude_m, geog),
// the other 4 parents don't (id, name, geog). The child table's join
// column is always its own field 0 too (its "id" column, a foreign
// reference to the parent's id, not a unique key of its own).
struct TablePair {
    const char* parent;
    int parent_geom_field;
    const char* child;
};

const std::vector<TablePair> kTablePairs = {
    {"ways",      2, "way_tags"},
    {"areas",     2, "area_tags"},
    {"roads",     2, "road_tags"},
    {"relations", 2, "relation_tags"},
    {"nodes",     4, "node_tags"},
};

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdirP(const std::string& path) {
    return system(("mkdir -p '" + path + "'").c_str()) == 0;
}

// Identical logic to regional_db_export.cpp's lookupSrid(), duplicated
// rather than shared -- both tools are otherwise self-contained (no
// shared non-header source between them), and this is a handful of
// lines. Every column here is a plain `geog` (no raster tables in this
// tool's scope), so the ST_ConvexHull(rast) branch that version has isn't
// needed.
int lookupSrid(pqxx::connection& conn, const std::string& table) {
    pqxx::work txn(conn);
    auto r = txn.exec(
        "SELECT srid FROM geometry_columns WHERE f_table_schema='public' "
        "AND f_table_name=$1 AND f_geometry_column='geog'",
        pqxx::params{table});
    // NavDB's DDL declares columns as untyped `public.geometry` (no SRID
    // typmod), so geometry_columns reports srid=0 even though every
    // stored value carries a real embedded SRID.
    if (!r.empty() && r[0][0].as<int>() != 0) return r[0][0].as<int>();
    auto r2 = txn.exec("SELECT ST_SRID(geog) FROM public." + table + " WHERE geog IS NOT NULL LIMIT 1");
    return r2.empty() ? 3857 : r2[0][0].as<int>();
}

// Region polygons are stored WGS84-degree in data/regions/*.wkt. Only two
// SRIDs are ever produced by this codebase's own loader (see
// GeoUtils.h's g_srid: 3857 Mercator by default, 4326 if -L/--wgs84 was
// used) -- project for Mercator, pass through as-is for 4326, and warn
// loudly (rather than silently mis-projecting) on anything else.
RegionPolygons::MultiPolygon projectForSrid(RegionPolygons::MultiPolygon mp, int srid) {
    if (srid == 4326) return mp;
    if (srid != 3857) {
        std::cerr << "[regional_table_export] WARNING: unexpected SRID " << srid
                  << " (expected 3857 or 4326) -- treating region polygons as WGS84 degrees, "
                     "matching may be wrong\n";
        return mp;
    }
    boost::geometry::for_each_point(mp, [](RegionPolygons::Point& p) {
        auto [x, y] = toMercator(p.x(), p.y());
        p.x(x);
        p.y(y);
    });
    return mp;
}

// One worker's local classification result from the parent pass: only
// ids that matched at least one region are stored (a zero-bitmask id is
// simply absent, same convention as a NULL-geometry row -- see the
// top-of-file comment). Sorted by id after the parent pass completes, so
// the child pass can binary-search it.
struct LocalMap {
    std::vector<std::pair<int64_t, uint32_t>> entries;
};

uint32_t lookupBitmask(const LocalMap& m, int64_t id) {
    auto it = std::lower_bound(m.entries.begin(), m.entries.end(), id,
                                [](const std::pair<int64_t, uint32_t>& e, int64_t v) { return e.first < v; });
    if (it != m.entries.end() && it->first == id) return it->second;
    return 0;
}

// Headerless per-thread-per-region scratch file: each row is a uint32_t
// length prefix (host byte order -- private framing, never touches the
// network or another process) followed by that many raw bytes (a row's
// exact, unmodified COPY-binary field-count+fields payload as returned by
// PgCopyBinary::Reader). No COPY-binary file header/trailer here -- those
// get written exactly once, at merge time (mergeTempFiles), analogous to
// regional_export.cpp's fixed-24-byte-record temp files, generalized to
// variable-length rows.
class TempRowWriter {
public:
    explicit TempRowWriter(const std::string& path) : f_(fopen(path.c_str(), "wb")) {}
    ~TempRowWriter() { if (f_) fclose(f_); }
    TempRowWriter(const TempRowWriter&) = delete;
    TempRowWriter& operator=(const TempRowWriter&) = delete;

    void write(const std::vector<uint8_t>& raw) {
        if (!f_) return;
        uint32_t len = static_cast<uint32_t>(raw.size());
        fwrite(&len, sizeof(len), 1, f_);
        fwrite(raw.data(), 1, raw.size(), f_);
    }

private:
    FILE* f_;
};

// Streams every temp file's rows into one real PgCopyBinary::Writer
// (header once, rows verbatim, trailer once), deleting each temp file as
// it's consumed. Returns the number of rows written (for the manifest).
int64_t mergeTempFiles(const std::string& out_path, const std::vector<std::string>& temp_paths) {
    PgCopyBinary::Writer writer(out_path);
    int64_t count = 0;
    std::vector<uint8_t> buf;
    for (const auto& p : temp_paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            uint32_t len;
            while (fread(&len, sizeof(len), 1, f) == 1) {
                buf.resize(len);
                if (fread(buf.data(), 1, len, f) != len) break;
                writer.writeRow(buf);
                ++count;
            }
            fclose(f);
        }
        unlink(p.c_str());
    }
    writer.close();
    return count;
}

void appendManifestLines(std::mutex& mtx, const std::string& region_dir, const std::string& text) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream mf(region_dir + "/manifest.txt", std::ios::app);
    mf << text;
}

// Headerless per-thread-per-region scratch file for extra-vertex candidates:
// fixed 16-byte {double lon_m; double lat_m;} records, no length prefix
// needed (unlike TempRowWriter's variable-length COPY rows). Deliberately
// not deduplicated here -- a vertex can legitimately appear once per way
// that references it, and regional_export.cpp's consuming hash-map build
// dedups naturally on insert.
class ExtraVertexWriter {
public:
    explicit ExtraVertexWriter(const std::string& path) : f_(fopen(path.c_str(), "wb")) {}
    ~ExtraVertexWriter() { if (f_) fclose(f_); }
    ExtraVertexWriter(const ExtraVertexWriter&) = delete;
    ExtraVertexWriter& operator=(const ExtraVertexWriter&) = delete;

    void write(double lon_m, double lat_m) {
        if (!f_) return;
        double buf[2] = {lon_m, lat_m};
        fwrite(buf, sizeof(buf), 1, f_);
    }

private:
    FILE* f_;
};

// Enumerates every vertex of a decoded geometry (std::monostate contributes
// none). boost::geometry::for_each_point handles a bare Point too, so this
// covers all 6 GeomVariant alternatives uniformly with no per-type code.
void collectVertices(const WkbDecode::GeomVariant& geom, std::vector<RegionIndex::Point>& out) {
    std::visit([&](const auto& g) {
        using T = std::decay_t<decltype(g)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
            boost::geometry::for_each_point(g, [&](const RegionIndex::Point& p) { out.push_back(p); });
        }
    }, geom);
}

// Appends every temp file's raw 16-byte records onto <region_dir>/
// extra_vertices.bin (creating it on first append) and deletes the temp
// files. Locked: with --parallel-table-pairs > 1, multiple pairs' merge
// steps can target the same region's file concurrently (unlike
// mergeTempFiles's per-pair-unique <table>.bin, this file is shared across
// all 4 geometry-bearing pairs).
void appendExtraVertices(std::mutex& mtx, const std::string& region_dir, const std::vector<std::string>& temp_paths) {
    std::lock_guard<std::mutex> lock(mtx);
    FILE* out = fopen((region_dir + "/extra_vertices.bin").c_str(), "ab");
    std::vector<char> buf;
    for (const auto& p : temp_paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            if (out) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    buf.resize(static_cast<size_t>(sz));
                    if (fread(buf.data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz))
                        fwrite(buf.data(), 1, static_cast<size_t>(sz), out);
                }
            }
            fclose(f);
        }
        unlink(p.c_str());
    }
    if (out) fclose(out);
}

// Processes one table pair end-to-end: id-range partition, parent pass
// (classify + write matched rows + build local id->bitmask maps), child
// pass (reuse those maps against the same id-range partition), merge,
// manifest. `regions` must already be projected into this pair's table's
// coordinate system (see the srid cache in main()).
void processPair(const TablePair& pair, const std::string& conninfo, const std::string& out_dir,
                  const std::vector<RegionIndex::Entry>& regions, int threads, bool verbose,
                  bool extra_vertices_only, std::mutex& manifest_mutex, std::mutex& extra_vertices_mutex) {
    // `nodes` rows are bare Points -- a vertex "outside its own matched
    // region" is a contradiction for a Point candidate (matches() already
    // required it inside), so this pair never contributes to
    // extra_vertices.bin. In --extra-vertices-only mode there's therefore
    // nothing at all for this pair to do -- skip the whole (potentially
    // huge) table scan.
    bool emit_extra_vertices = (std::string(pair.parent) != "nodes");
    if (extra_vertices_only && !emit_extra_vertices) {
        if (verbose) std::cout << "[regional_table_export] " << pair.parent << ": skipped (--extra-vertices-only)\n";
        return;
    }

    pqxx::connection ctl(conninfo);
    int64_t min_id = 0, max_id = -1;
    {
        pqxx::work txn(ctl);
        auto r = txn.exec("SELECT MIN(id), MAX(id) FROM public." + std::string(pair.parent));
        if (!r[0][0].is_null()) {
            min_id = r[0][0].as<int64_t>();
            max_id = r[0][1].as<int64_t>();
        }
    }

    if (max_id < min_id) {
        // Empty table -- nothing to scan, but every region still needs a
        // manifest line so regional_install.cpp's binOrGzExists() check
        // (and a future reader eyeballing the manifest) sees an explicit
        // zero rather than a silently-missing entry.
        if (!extra_vertices_only) {
            for (const auto& reg : regions) {
                std::string region_dir = out_dir + "/" + reg.name;
                std::ostringstream lines;
                lines << "table." << pair.parent << ".rows=0\ntable." << pair.parent << ".format=binary\n"
                      << "table." << pair.child << ".rows=0\ntable." << pair.child << ".format=binary\n";
                appendManifestLines(manifest_mutex, region_dir, lines.str());
            }
        }
        if (verbose) std::cout << "[regional_table_export] " << pair.parent << "/" << pair.child << ": empty, skipped\n";
        return;
    }

    int nthreads = std::max(1, threads);
    int64_t span = max_id - min_id + 1;
    int64_t chunk = (span + nthreads - 1) / nthreads;

    std::vector<LocalMap> local_maps(static_cast<size_t>(nthreads));
    std::vector<std::vector<std::string>> parent_temp(static_cast<size_t>(nthreads),
                                                        std::vector<std::string>(regions.size()));
    std::vector<std::vector<std::string>> child_temp(static_cast<size_t>(nthreads),
                                                       std::vector<std::string>(regions.size()));
    std::vector<std::vector<std::string>> extra_vertex_temp(static_cast<size_t>(nthreads),
                                                              std::vector<std::string>(regions.size()));
    for (int ti = 0; ti < nthreads; ++ti) {
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            std::string base = out_dir + "/tmp_" + regions[ri].name + "_" + std::to_string(ti) + "_";
            parent_temp[static_cast<size_t>(ti)][ri] = base + pair.parent + ".raw";
            child_temp[static_cast<size_t>(ti)][ri] = base + pair.child + ".raw";
            if (emit_extra_vertices)
                extra_vertex_temp[static_cast<size_t>(ti)][ri] = base + "extra_vertices.raw";
        }
    }

    auto windowFor = [&](int ti) -> std::pair<int64_t, int64_t> {
        int64_t lo = min_id + static_cast<int64_t>(ti) * chunk;
        int64_t hi = std::min(lo + chunk - 1, max_id);
        return {lo, hi};
    };

    // ---- Parent pass: classify every row against every region, write
    // matches, record (id, bitmask) for the child pass. ----
    auto parentWorker = [&](int ti) {
        auto [lo, hi] = windowFor(ti);
        if (lo > hi) return;

        std::vector<std::unique_ptr<TempRowWriter>> writers;
        if (!extra_vertices_only) {
            writers.reserve(regions.size());
            for (size_t ri = 0; ri < regions.size(); ++ri)
                writers.push_back(std::make_unique<TempRowWriter>(parent_temp[static_cast<size_t>(ti)][ri]));
        }
        std::vector<std::unique_ptr<ExtraVertexWriter>> vwriters;
        if (emit_extra_vertices) {
            vwriters.reserve(regions.size());
            for (size_t ri = 0; ri < regions.size(); ++ri)
                vwriters.push_back(std::make_unique<ExtraVertexWriter>(extra_vertex_temp[static_cast<size_t>(ti)][ri]));
        }

        std::string sql = "SELECT * FROM public." + std::string(pair.parent) +
                           " WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi);
        PgCopyBinary::Reader reader(conninfo, sql);
        std::vector<std::pair<RegionIndex::Box, size_t>> scratch;
        std::vector<RegionIndex::Point> verts;
        auto& map = local_maps[static_cast<size_t>(ti)].entries;

        while (auto row = reader.next()) {
            int64_t id = PgCopyBinary::readBigintBE(row->fields[0]);
            const auto& gf = row->fields[static_cast<size_t>(pair.parent_geom_field)];

            uint32_t bitmask = 0;
            if (gf.data != nullptr) {
                auto decoded = WkbDecode::decode(gf.data, static_cast<size_t>(gf.len));
                if (!decoded) {
                    std::cerr << "[regional_table_export] WARNING: " << pair.parent << " id=" << id
                              << " geog failed to decode -- excluded from every region\n";
                } else {
                    verts.clear();
                    if (emit_extra_vertices) collectVertices(decoded->geom, verts);

                    for (size_t ri = 0; ri < regions.size(); ++ri) {
                        if (RegionIndex::matches(regions[ri], decoded->geom, scratch)) {
                            bitmask |= (1u << ri);
                            if (!extra_vertices_only) writers[ri]->write(row->raw);

                            // Only vertices OUTSIDE this region's own polygon are
                            // "extra" -- vertices inside it are already covered by
                            // regional_export.cpp's own per-node polygon test, and
                            // recording them here would just bloat the hash it
                            // builds from extra_vertices.bin for no benefit.
                            if (emit_extra_vertices) {
                                for (const auto& v : verts) {
                                    WkbDecode::GeomVariant vg = v;
                                    if (!RegionIndex::matches(regions[ri], vg, scratch))
                                        vwriters[ri]->write(v.x(), v.y());
                                }
                            }
                        }
                    }
                }
            }
            if (bitmask != 0) map.emplace_back(id, bitmask);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(nthreads));
        for (int ti = 0; ti < nthreads; ++ti) pool.emplace_back(parentWorker, ti);
        for (auto& t : pool) t.join();
    }
    for (auto& m : local_maps) std::sort(m.entries.begin(), m.entries.end());

    // ---- Child pass: reuses the identical id-range partition, each
    // worker classifying against only its own local map from the parent
    // pass (see top-of-file comment for why that's safe). ----
    auto childWorker = [&](int ti) {
        auto [lo, hi] = windowFor(ti);
        if (lo > hi) return;

        std::vector<std::unique_ptr<TempRowWriter>> writers;
        writers.reserve(regions.size());
        for (size_t ri = 0; ri < regions.size(); ++ri)
            writers.push_back(std::make_unique<TempRowWriter>(child_temp[static_cast<size_t>(ti)][ri]));

        std::string sql = "SELECT * FROM public." + std::string(pair.child) +
                           " WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi);
        PgCopyBinary::Reader reader(conninfo, sql);
        const auto& map = local_maps[static_cast<size_t>(ti)];

        while (auto row = reader.next()) {
            int64_t id = PgCopyBinary::readBigintBE(row->fields[0]);
            uint32_t bitmask = lookupBitmask(map, id);
            if (bitmask == 0) continue;
            for (size_t ri = 0; ri < regions.size(); ++ri)
                if (bitmask & (1u << ri)) writers[ri]->write(row->raw);
        }
    };

    // --extra-vertices-only has no use for the child table at all (tags
    // never affect geometry/vertices) -- skip that whole scan.
    if (!extra_vertices_only) {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(nthreads));
        for (int ti = 0; ti < nthreads; ++ti) pool.emplace_back(childWorker, ti);
        for (auto& t : pool) t.join();
    }

    // ---- Merge + manifest ----
    for (size_t ri = 0; ri < regions.size(); ++ri) {
        std::string region_dir = out_dir + "/" + regions[ri].name;

        if (!extra_vertices_only) {
            std::vector<std::string> p_paths, c_paths;
            p_paths.reserve(static_cast<size_t>(nthreads));
            c_paths.reserve(static_cast<size_t>(nthreads));
            for (int ti = 0; ti < nthreads; ++ti) {
                p_paths.push_back(parent_temp[static_cast<size_t>(ti)][ri]);
                c_paths.push_back(child_temp[static_cast<size_t>(ti)][ri]);
            }
            int64_t prows = mergeTempFiles(region_dir + "/" + pair.parent + ".bin", p_paths);
            int64_t crows = mergeTempFiles(region_dir + "/" + pair.child + ".bin", c_paths);

            std::ostringstream lines;
            lines << "table." << pair.parent << ".rows=" << prows << "\ntable." << pair.parent << ".format=binary\n"
                  << "table." << pair.child << ".rows=" << crows << "\ntable." << pair.child << ".format=binary\n";
            appendManifestLines(manifest_mutex, region_dir, lines.str());

            if (verbose && (prows > 0 || crows > 0)) {
                std::cout << "[regional_table_export] " << regions[ri].name << "/" << pair.parent << ": "
                          << prows << " row(s), " << pair.child << ": " << crows << " row(s)\n";
            }
        }

        if (emit_extra_vertices) {
            std::vector<std::string> v_paths;
            v_paths.reserve(static_cast<size_t>(nthreads));
            for (int ti = 0; ti < nthreads; ++ti)
                v_paths.push_back(extra_vertex_temp[static_cast<size_t>(ti)][ri]);
            appendExtraVertices(extra_vertices_mutex, region_dir, v_paths);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string host, db, user, out_dir = ".";
    std::string regions_dir = "data/regions";
    std::vector<std::string> region_filter;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    int parallel_table_pairs = 1;
    bool verbose = false;
    bool extra_vertices_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      ((arg == "-s") && i + 1 < argc) host = argv[++i];
        else if ((arg == "-d") && i + 1 < argc) db   = argv[++i];
        else if ((arg == "-u") && i + 1 < argc) user = argv[++i];
        else if ((arg == "--out-dir") && i + 1 < argc) out_dir = argv[++i];
        else if ((arg == "--regions-dir") && i + 1 < argc) regions_dir = argv[++i];
        else if ((arg == "--threads") && i + 1 < argc) threads = std::stoi(argv[++i]);
        else if ((arg == "--parallel-table-pairs") && i + 1 < argc) parallel_table_pairs = std::stoi(argv[++i]);
        else if (arg == "--extra-vertices-only") extra_vertices_only = true;
        else if ((arg == "--regions") && i + 1 < argc) {
            std::stringstream ss(std::string(argv[++i]));
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) region_filter.push_back(tok);
        }
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: regional_table_export -s <host> -d <db> -u <user> --out-dir <dir>\n"
                         "                              [--regions-dir <dir>] [--regions name1,name2,...]\n"
                         "                              [--threads N] [--parallel-table-pairs N]\n"
                         "                              [--extra-vertices-only] [-v]\n"
                         "\n"
                         "Single client-side pass over the 5 big parent+child table pairs\n"
                         "(ways/way_tags, areas/area_tags, roads/road_tags, relations/\n"
                         "relation_tags, nodes/node_tags), writing <out-dir>/<region>/<table>.bin\n"
                         "for every region (see include/Regions.h) -- run this before\n"
                         "regional_db_export (which handles the remaining, smaller tables and\n"
                         "appends to the same manifest.txt). Also writes <region>/\n"
                         "extra_vertices.bin (border-crossing way/area/road vertices) --\n"
                         "regional_export.cpp must now run AFTER this tool to fold that into\n"
                         "its node classification. --extra-vertices-only writes only that file\n"
                         "(no table/tag output, no child-table pass, `nodes` pair skipped) --\n"
                         "for widening an already-built region's node file without re-exporting\n"
                         "its already-correct table data.\n";
            std::cout.flush();
            _exit(0);  // avoid pqxx static-destructor double-free on normal return
        }
    }
    if (threads < 1) threads = 1;
    if (parallel_table_pairs < 1) parallel_table_pairs = 1;

    if (host.empty() || db.empty() || user.empty()) {
        std::cerr << "Error: -s, -d, -u are required\n";
        _exit(1);
    }
    if (!dirExists(out_dir) && !mkdirP(out_dir)) {
        std::cerr << "Error: cannot create --out-dir " << out_dir << "\n";
        _exit(1);
    }

    std::string conninfo = "host=" + host + " dbname=" + db + " user=" + user;

    auto polygons = RegionPolygons::load(regions_dir, region_filter);  // WGS84 degrees, throws on missing/bad WKT

    std::vector<GlobalRegion> selected;
    for (const auto& r : allExportRegions()) {
        if (!region_filter.empty() &&
            std::find(region_filter.begin(), region_filter.end(), std::string(r.name)) == region_filter.end())
            continue;
        selected.push_back(r);
    }
    if (selected.empty()) {
        std::cerr << "Error: no matching regions (check --regions names against include/Regions.h)\n";
        _exit(1);
    }

    // Header lines only -- regional_db_export.cpp appends its own
    // table.*.rows/format lines to the same file afterward (task #50),
    // skipping these header lines if the manifest already exists. This
    // tool always runs first in the pipeline, so it owns writing them.
    // Skipped in --extra-vertices-only mode: that mode targets a scratch
    // --out-dir used only to produce extra_vertices.bin for an already-built
    // region (no table/tag output goes there, so no real manifest to own).
    for (const auto& r : selected) {
        std::string region_dir = out_dir + "/" + r.name;
        if (!mkdirP(region_dir)) {
            std::cerr << "Error: cannot create " << region_dir << "\n";
            _exit(1);
        }
        if (extra_vertices_only) continue;
        std::ofstream manifest(region_dir + "/manifest.txt");
        manifest << "region=" << r.name << "\n";
        manifest << "bbox=" << r.min_lon << "," << r.min_lat << "," << r.max_lon << "," << r.max_lat << "\n";
        manifest << "exported_at=" << static_cast<int64_t>(time(nullptr)) << "\n";
    }

    // SRID lookup + region-index cache, built once up front (not lazily
    // inside worker threads) so it needs no locking even when
    // --parallel-table-pairs runs multiple pairs concurrently.
    pqxx::connection ctl(conninfo);
    std::unordered_map<int, std::vector<RegionIndex::Entry>> regions_by_srid;
    std::vector<int> pair_srid(kTablePairs.size());
    for (size_t pi = 0; pi < kTablePairs.size(); ++pi) {
        int srid = lookupSrid(ctl, kTablePairs[pi].parent);
        pair_srid[pi] = srid;
        if (regions_by_srid.count(srid)) continue;

        std::vector<RegionIndex::Entry> entries;
        entries.reserve(selected.size());
        for (const auto& r : selected) {
            RegionPolygons::MultiPolygon mp = projectForSrid(polygons.at(r.name), srid);
            auto [min_x, min_y] = (srid == 4326) ? std::pair{r.min_lon, r.min_lat} : toMercator(r.min_lon, r.min_lat);
            auto [max_x, max_y] = (srid == 4326) ? std::pair{r.max_lon, r.max_lat} : toMercator(r.max_lon, r.max_lat);
            entries.push_back(RegionIndex::build(r.name, min_x, min_y, max_x, max_y, std::move(mp)));
        }
        regions_by_srid.emplace(srid, std::move(entries));
        if (verbose) std::cout << "[regional_table_export] built region index for SRID " << srid << "\n";
    }

    if (verbose) {
        std::cout << "[regional_table_export] " << selected.size() << " region(s), " << kTablePairs.size()
                  << " table pair(s), --threads " << threads << ", --parallel-table-pairs "
                  << parallel_table_pairs << "\n";
    }

    std::mutex manifest_mutex;
    std::mutex extra_vertices_mutex;
    std::atomic<size_t> next_pair{0};
    auto pairDriver = [&]() {
        for (;;) {
            size_t pi = next_pair.fetch_add(1, std::memory_order_relaxed);
            if (pi >= kTablePairs.size()) return;
            const auto& pair = kTablePairs[pi];
            const auto& regions = regions_by_srid.at(pair_srid[pi]);
            if (verbose) std::cout << "[regional_table_export] === " << pair.parent << "/" << pair.child << " ===\n";
            processPair(pair, conninfo, out_dir, regions, threads, verbose, extra_vertices_only,
                        manifest_mutex, extra_vertices_mutex);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(parallel_table_pairs));
        for (int i = 0; i < parallel_table_pairs; ++i) pool.emplace_back(pairDriver);
        for (auto& t : pool) t.join();
    }

    std::cout << "[regional_table_export] done\n";
    std::cout.flush();
    _exit(0);  // avoid pqxx static-destructor double-free on normal return
}
