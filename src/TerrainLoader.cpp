#include "TerrainLoader.h"
#include "Regions.h"
#include "ConvenienceFunctions.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <curl/curl.h>
#include <unistd.h>

namespace {

size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* f = static_cast<std::ofstream*>(stream);
    f->write(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// A fresh handle per tile, not reused across a thread's whole run: reusing
// one handle kept a small number of long-lived TCP/TLS connections open to
// the same S3 IP, which was observed to get bandwidth-throttled the longer
// each connection stayed alive (a one-off fresh connection to the same
// bucket got full speed while the app's reused connections had degraded to
// a crawl) -- so a fresh connection per tile, despite the handshake cost,
// is actually the faster and safer choice here.
bool downloadFile(const std::string& url, const std::string& dest) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::ofstream f(dest, std::ios::binary);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // treat HTTP 4xx/5xx as failure
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    f.close();
    return res == CURLE_OK;
}

struct Tile { int lat; int lon; std::string name; std::string url; };

const char* sourceName(TerrainSource source) {
    return source == TerrainSource::USGS3DEP ? "3dep" : "copernicus";
}

// Source SRID of the downloaded GeoTIFFs: 3DEP ships NAD83 (EPSG:4269),
// Copernicus DEM GLO-30 ships WGS84 (EPSG:4326).
int sourceSrid(TerrainSource source) {
    return source == TerrainSource::USGS3DEP ? 4269 : 4326;
}

// 3DEP tiles are named by their NW corner: "n40w105" covers lat [39,40] x
// lon [-105,-104] — the "n" value is the tile's NORTH edge, but the "w"
// value is the tile's WEST edge (confirmed empirically: n40w105 contains
// Denver at 39.7N/-105.0W). So the row loop variable (a tile's south edge,
// i.e. floor(lat)) needs +1 to become the naming convention's north edge;
// the column loop variable (a tile's west edge, i.e. floor(lon)) is used
// as-is. US-only coverage, so always north/west.
//
// Copernicus DEM GLO-30 tiles use the more common SW-corner convention —
// "N39_00_W105_00" covers lat [39,40] x lon [-105,-104], i.e. floor(lat)
// and floor(lon) used directly with no adjustment (confirmed empirically:
// N39_00_W105_00 contains Denver at 39.7N/-105.0W too, same cell as 3DEP's
// n40w105 — different naming convention, same physical tile). Global
// coverage, so needs N/S and E/W signs instead of always north/west.
std::vector<Tile> tilesForBBox(TerrainSource source,
                               double min_lon, double min_lat,
                               double max_lon, double max_lat) {
    std::vector<Tile> tiles;
    int lat0 = static_cast<int>(std::floor(min_lat));
    int lat1 = static_cast<int>(std::ceil(max_lat));
    int lon0 = static_cast<int>(std::floor(min_lon));
    int lon1 = static_cast<int>(std::ceil(max_lon));

    auto addTile = [&](int lat, int lon) {
        char buf[48];
        std::string name, url;
        if (source == TerrainSource::USGS3DEP) {
            snprintf(buf, sizeof(buf), "n%02dw%03d", lat + 1, -lon);
            name = buf;
            url = "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1/TIFF/current/"
                  + name + "/USGS_1_" + name + ".tif";
        } else {
            char ns = lat >= 0 ? 'N' : 'S';
            char ew = lon >= 0 ? 'E' : 'W';
            snprintf(buf, sizeof(buf), "%c%02d_00_%c%03d_00",
                    ns, std::abs(lat), ew, std::abs(lon));
            name = buf;
            std::string folder = "Copernicus_DSM_COG_10_" + name + "_DEM";
            url = "https://copernicus-dem-30m.s3.amazonaws.com/" + folder + "/" + folder + ".tif";
        }
        tiles.push_back({lat, lon, name, url});
    };

    for (int lat = lat0; lat < lat1; ++lat) {
        if (lon0 <= lon1) {
            for (int lon = lon0; lon < lon1; ++lon) addTile(lat, lon);
        } else {
            // Antimeridian wrap (min_lon > max_lon, e.g. oceania: 110 to
            // -150) -- cover [lon0,180) and [-180,lon1) instead of the
            // single [lon0,lon1) range, which is empty/backwards here and
            // silently produced zero tiles for this bbox shape before.
            for (int lon = lon0; lon < 180; ++lon) addTile(lat, lon);
            for (int lon = -180; lon < lon1; ++lon) addTile(lat, lon);
        }
    }
    return tiles;
}

} // namespace

namespace {
constexpr double kFeetPerMeter = 3.28084;
} // namespace

bool TerrainLoader::load(double min_lon, double min_lat, double max_lon, double max_lat,
                         TerrainSource source, int dest_srid,
                         int threads, bool verbose, const Bbox* skip_bbox) {
    auto tiles = tilesForBBox(source, min_lon, min_lat, max_lon, max_lat);
    if (skip_bbox) {
        tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [&](const Tile& t) {
            return t.lon >= skip_bbox->min_lon && t.lon + 1 <= skip_bbox->max_lon &&
                   t.lat >= skip_bbox->min_lat && t.lat + 1 <= skip_bbox->max_lat;
        }), tiles.end());
    }
    if (tiles.empty()) {
        std::cerr << "[Terrain] bounding box produced no tiles\n";
        return false;
    }

    {
        pqxx::work txn(conn_);
        txn.exec("CREATE EXTENSION IF NOT EXISTS postgis_raster");
        txn.exec(
            "CREATE TABLE IF NOT EXISTS terrain_tiles ("
            "  tile_name text PRIMARY KEY,"
            "  source text NOT NULL DEFAULT '3dep',"
            "  loaded_at timestamptz NOT NULL DEFAULT now())");
        // Backfills the column on a table created before source-tracking
        // existed (all pre-existing rows are 3DEP, matching the default).
        txn.exec("ALTER TABLE terrain_tiles ADD COLUMN IF NOT EXISTS source text NOT NULL DEFAULT '3dep'");
        txn.commit();
    }

    bool table_exists;
    {
        pqxx::work txn(conn_);
        auto r = txn.exec(
            "SELECT EXISTS (SELECT 1 FROM information_schema.tables "
            "WHERE table_schema='public' AND table_name='terrain')");
        txn.commit();
        table_exists = r[0][0].as<bool>();
    }

    // Point/corridor elevation lookups direct against the raw raster — the
    // consuming application (a separate Python service) needs precise
    // elevation at a point and along a short lookahead corridor, not
    // classified elevation-band polygons (terrain_bands and its band-rebuild
    // machinery were removed — point/corridor lookups are the only need).
    // CREATE OR REPLACE so these stay current on every terrain_load run.
    //
    // Deferred instead of running unconditionally up front: these are
    // LANGUAGE sql functions, and Postgres parses/analyzes a SQL function's
    // body — including resolving public.terrain — at CREATE FUNCTION time
    // (unlike plpgsql, which defers). On a fresh -I reload the terrain
    // table doesn't exist yet at this point in load(), so creating these
    // before the table exists throws pqxx::undefined_table and takes down
    // the whole process. Only call this once table_exists is actually true.
    // Shared with regional_install.cpp -- see ConvenienceFunctions.h.
    if (table_exists) createElevationFunctions(conn_);

    std::vector<Tile> to_load;
    for (auto& t : tiles) {
        pqxx::work txn(conn_);
        auto r = txn.exec("SELECT 1 FROM terrain_tiles WHERE tile_name=$1",
                          pqxx::params{t.name});
        txn.commit();
        if (r.empty()) to_load.push_back(t);
        else if (verbose) std::cout << "  " << t.name << " already loaded, skipping\n";
    }

    if (to_load.empty()) {
        if (verbose) std::cout << "All requested tiles already loaded.\n";
        if (table_exists) createElevationFunctions(conn_);
        return true;
    }

    // Keyed by PID, not a fixed shared directory -- same collision class
    // just found and fixed in AirportsLoader.cpp's /tmp paths (a stale
    // file left behind by one process/user silently blocks every other
    // process from ever writing that path again). Tile filenames here are
    // content-derived (e.g. "n47w074.tif"), not process-specific, so any
    // two overlapping invocations sharing one directory -- this loader
    // running twice concurrently, or a leftover process from an earlier
    // kill/restart cycle -- could read/overwrite each other's in-flight
    // downloads. Not confirmed as the cause of a batch-load failure wave
    // seen in production (raster2pgsql/GDAL TIFFReadEncodedTile errors,
    // consistent with a truncated file), but this closes off that
    // possibility regardless of whether it was the actual cause.
    const std::string tmp_dir = "/tmp/terrain_tiles_" + std::to_string(getpid());
    system(("mkdir -p " + tmp_dir).c_str());

    // libcurl requires curl_global_init() to happen once, before any thread
    // uses it; curl_easy_init() does this automatically on first call, but
    // that auto-init isn't safe if multiple threads race into it simultaneously.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Process in small batches rather than downloading everything up front
    // and issuing one giant raster2pgsql call: at CONUS scale (1000+ tiles)
    // that both risks filling /tmp (a tmpfs, so this is RAM, not disk) with
    // every raw GeoTIFF held at once, and overwhelms raster2pgsql itself.
    // Batching also means one bad batch doesn't cost you tiles that already
    // loaded fine, and terrain_tiles only ever records a tile once its own
    // batch's load is independently confirmed.
    constexpr size_t kBatchSize = 25;
    size_t n_batches = (to_load.size() + kBatchSize - 1) / kBatchSize;
    std::atomic<long long> total_loaded{0};
    std::atomic<long long> total_retried_batches{0};
    std::mutex io_mu;      // guards stdout/stderr so per-tile/batch lines don't interleave
    std::mutex raster_mu;  // serializes raster2pgsql invocation only — see below

    enum class BatchResult { Success, NoTiles, Failed };

    // Downloads+loads a single batch, exactly once (no retry -- see
    // runBatch below, which wraps this with the retry-then-abort policy).
    // use_append selects -a (append to an existing terrain table) vs -I -M
    // (create it) — the caller is responsible for only passing false when
    // the table doesn't exist yet. db is the caller's own connection (each
    // parallel worker owns one, so this can run concurrently across
    // batches with no shared DB state besides the terrain/terrain_tiles
    // tables themselves, which tolerate concurrent appends fine).
    auto runBatchOnce = [&](size_t batch_idx, pqxx::connection& db, bool use_append) -> BatchResult {
        size_t start = batch_idx * kBatchSize;
        size_t end = std::min(start + kBatchSize, to_load.size());

        std::vector<std::string> downloaded_paths;
        std::vector<Tile> downloaded_tiles;
        for (size_t i = start; i < end; ++i) {
            const Tile& t = to_load[i];
            std::string dest = tmp_dir + "/" + t.name + ".tif";
            bool ok = downloadFile(t.url, dest);
            if (verbose) {
                std::lock_guard lk(io_mu);
                std::cout << "  downloading " << t.name << "... "
                          << (ok ? "OK" : "not available, skipping") << "\n";
            }
            if (!ok) {
                // Not fatal — a requested bbox may extend past the source's
                // coverage (ocean, or non-US territory for 3DEP), so a
                // missing tile is expected, not an error worth aborting over.
                std::remove(dest.c_str());
                continue;
            }
            downloaded_paths.push_back(dest);
            downloaded_tiles.push_back(t);
        }

        if (downloaded_paths.empty()) return BatchResult::NoTiles;  // whole batch unavailable, not an error

        // raster2pgsql generates SQL to a file, then psql loads that file, as
        // two separate steps with independently-checked exit codes — piping
        // them directly (`raster2pgsql ... | psql ...`) hides raster2pgsql's
        // exit status entirely (a shell pipe's exit code is only the last
        // command's), so a crashed/failed raster2pgsql with psql still
        // exiting 0 on empty input was previously read as a full success,
        // silently recording tiles as loaded with zero actual data written.
        //
        // Deliberately no -C (standard constraints): reprojecting tiles
        // spanning a range of latitudes into Mercator gives each tile a
        // slightly different pixel scale, and 256x256 tiling gives edge
        // blocks non-uniform width/height. Fixed-value CHECK constraints
        // derived from one load batch then reject any later append whose
        // tiles happen to have different dimensions/scale — not needed for
        // ST_Value point queries, which only rely on the GIST index from -I.
        std::string sql_file = tmp_dir + "/batch_" + std::to_string(batch_idx) + ".sql";
        std::ostringstream gen_cmd;
        // Tried -Y (COPY instead of INSERT, for faster loading) here --
        // raster2pgsql flatly rejects it in combination with -s
        // FROM_SRID:TO_SRID ("Invalid argument combination"), which this
        // loader always uses (source tiles are never already in dest_srid).
        // So plain INSERT output it is.
        // raster2pgsql prints its own "Processing N/M: <file>" progress to
        // stderr regardless of our own verbose flag (no CLI switch to turn
        // it off) -- captured to a per-batch file instead of left to
        // inherit our stderr, so the happy path stays on the one status
        // line the rest of the import uses. Surfaced on failure below
        // (this captured text is what actually diagnosed the -Y/-s
        // incompatibility bug, not the bare exit code).
        std::string batch_log = tmp_dir + "/batch_" + std::to_string(batch_idx) + ".rlog";
        gen_cmd << "raster2pgsql -s " << sourceSrid(source) << ":" << dest_srid << " -t 256x256 -F";
        gen_cmd << (use_append ? " -a" : " -I -M");
        for (auto& p : downloaded_paths) gen_cmd << " " << p;
        gen_cmd << " terrain > " << sql_file << " 2>" << batch_log;

        if (verbose) {
            std::lock_guard lk(io_mu);
            std::cout << "  batch " << (batch_idx + 1) << "/" << n_batches
                      << ": loading " << downloaded_paths.size() << " tile(s)...\n";
        }

        // Serialize only the raster2pgsql invocation (downloads above stay
        // concurrent, and so does the psql load below). Running many
        // raster2pgsql instances at once was observed to intermittently
        // produce corrupted/truncated SQL output (parse errors like
        // "unterminated quoted string" on otherwise-valid tiles) — a race
        // in raster2pgsql/GDAL under concurrent invocation, not something
        // this loader can fix, so avoid triggering it instead. psql loading
        // the already-generated SQL file has no GDAL involvement at all —
        // it's pure I/O against Postgres, which handles concurrent COPY/
        // INSERT sessions fine, so it doesn't need to share that lock and
        // was only ever serialized as a side effect of both steps sharing
        // one system() sequence.
        //
        // No password handling here (or anywhere in this codebase) —
        // auth relies on ~/.pgpass, which psql honors automatically for
        // this shell-out same as pqxx does for direct connections.
        int gen_rc, load_rc = -1;
        {
            std::lock_guard lk(raster_mu);
            gen_rc = system(gen_cmd.str().c_str());
        }
        if (gen_rc == 0) {
            std::ostringstream load_cmd;
            load_cmd << "psql -h " << host_ << " -U " << user_ << " -d " << database_
                      << " -q -v ON_ERROR_STOP=1 -f " << sql_file << " 2>>" << batch_log;
            load_rc = system(load_cmd.str().c_str());
        }

        for (auto& p : downloaded_paths) std::remove(p.c_str());
        std::remove(sql_file.c_str());

        if (gen_rc != 0 || load_rc != 0) {
            std::lock_guard lk(io_mu);
            std::cerr << "[Terrain] batch " << (batch_idx + 1) << "/" << n_batches
                      << " load failed (raster2pgsql rc=" << gen_rc
                      << ", psql rc=" << load_rc << ") for tiles:";
            for (auto& t : downloaded_tiles) std::cerr << " " << t.name;
            std::cerr << "\n";
            std::ifstream log_in(batch_log);
            std::string line;
            while (std::getline(log_in, line)) std::cerr << "    " << line << "\n";
            std::remove(batch_log.c_str());
            return BatchResult::Failed;
        }
        std::remove(batch_log.c_str());

        {
            pqxx::work txn(db);
            for (auto& t : downloaded_tiles)
                txn.exec("INSERT INTO terrain_tiles(tile_name, source) VALUES ($1, $2) "
                         "ON CONFLICT DO NOTHING", pqxx::params{t.name, sourceName(source)});
            txn.commit();
        }
        long long done = total_loaded.fetch_add(static_cast<long long>(downloaded_tiles.size()),
                               std::memory_order_relaxed) + static_cast<long long>(downloaded_tiles.size());
        progress_cb_(done, static_cast<int64_t>(to_load.size()));
        return BatchResult::Success;
    };

    // Wraps runBatchOnce with a retry-then-abort policy: a batch that
    // fails (raster2pgsql/psql error, not "no tiles available") gets one
    // immediate retry; if the retry also fails, that's treated as a hard
    // failure for the whole import rather than a tile silently left
    // unloaded for a human to notice and rerun later. _exit(1) here is
    // safe even though runBatch executes inside worker threads (below) --
    // it's the same "we're aborting, nothing needs C++ cleanup, the OS
    // reclaims everything" reasoning already used for other fatal
    // conditions in main.cpp (e.g. planet download failure).
    auto runBatch = [&](size_t batch_idx, pqxx::connection& db, bool use_append) -> bool {
        BatchResult r = runBatchOnce(batch_idx, db, use_append);
        if (r == BatchResult::Success) return true;
        if (r == BatchResult::NoTiles) return false;  // legitimate no-op, not an error

        {
            std::lock_guard lk(io_mu);
            std::cerr << "[Terrain] batch " << (batch_idx + 1) << "/" << n_batches
                      << " failed, retrying once...\n";
        }
        total_retried_batches.fetch_add(1, std::memory_order_relaxed);
        r = runBatchOnce(batch_idx, db, use_append);
        if (r == BatchResult::Success) return true;
        if (r == BatchResult::NoTiles) return false;

        std::lock_guard lk(io_mu);
        std::cerr << "[Terrain] batch " << (batch_idx + 1) << "/" << n_batches
                  << " failed again after retry -- aborting the import "
                     "(fix the underlying issue, then re-run with -R terrain "
                     "to resume; already-loaded tiles are unaffected)\n";
        std::cerr.flush();
        _exit(1);
    };

    // Bootstrap: if `terrain` doesn't exist yet, the batch that creates it
    // (-I) must run alone first — otherwise multiple threads could race to
    // -I it simultaneously. Try batches one at a time (skipping any that
    // turn out to have zero available tiles) until one actually succeeds.
    size_t first_parallel_batch = 0;
    if (!table_exists) {
        while (first_parallel_batch < n_batches) {
            if (runBatch(first_parallel_batch, conn_, false)) {
                table_exists = true;
                ++first_parallel_batch;
                break;
            }
            ++first_parallel_batch;
        }
    }

    // Remaining batches are independent (each appends, no shared mutable
    // state besides the DB tables, which tolerate concurrent writers), so —
    // same pattern as buildTerrainBands — a fixed thread pool pulls the next
    // unprocessed batch off a shared atomic counter, each with its own
    // connection.
    if (table_exists && first_parallel_batch < n_batches) {
        std::atomic<size_t> next_batch{first_parallel_batch};
        auto worker = [&]() {
            // An exception escaping a std::thread's function (e.g. a
            // transient connection failure) calls std::terminate() and
            // aborts the ENTIRE process, silently losing all other threads'
            // progress — caught here so one bad connection just drops this
            // thread's remaining work instead.
            try {
                pqxx::connection wconn = newConnection();
                while (true) {
                    size_t b = next_batch.fetch_add(1, std::memory_order_relaxed);
                    if (b >= n_batches) break;
                    runBatch(b, wconn, true);
                }
            } catch (const std::exception& e) {
                std::lock_guard lk(io_mu);
                std::cerr << "[Terrain] worker thread error: " << e.what() << "\n";
            }
        };
        int nthreads = std::max(1, std::min(threads,
                                            static_cast<int>(n_batches - first_parallel_batch)));
        if (verbose) std::cout << "[Terrain] loading remaining batches with " << nthreads << " thread(s)\n";
        std::vector<std::thread> workers;
        workers.reserve(nthreads);
        for (int t = 0; t < nthreads; ++t) workers.emplace_back(worker);
        for (auto& w : workers) w.join();
    }

    if (total_loaded.load() == 0) {
        std::cerr << "[Terrain] no tiles in this bounding box could be loaded\n";
        return false;
    }

    // total_loaded > 0 guarantees at least one batch's raster2pgsql call
    // succeeded, which means public.terrain now exists (either it already
    // did, or the bootstrap batch's -I -M just created it).
    createElevationFunctions(conn_);

    if (verbose) {
        std::cout << "Terrain data loaded (" << total_loaded.load() << " new tile(s)";
        // Any batch that failed and stayed failed after its retry already
        // aborted the whole process (see runBatch) -- reaching here means
        // every batch either succeeded outright or recovered on retry.
        if (total_retried_batches.load() > 0)
            std::cout << ", " << total_retried_batches.load() << " batch(es) needed a retry";
        std::cout << ").\n";
    }

    return true;
}

// kGlobalRegions moved to include/Regions.h (shared with regional_export) —
// see loadGlobalTerrain's doc comment in TerrainLoader.h for the rationale
// behind each group and what's deliberately excluded (US, Antarctica).

bool TerrainLoader::loadGlobal(int dest_srid, int threads, bool verbose) {
    bool any_loaded = false;
    constexpr int kRegionCount = static_cast<int>(sizeof(kGlobalRegions) / sizeof(kGlobalRegions[0]));
    int region_idx = 0;
    for (const auto& r : kGlobalRegions) {
        region_progress_cb_(++region_idx, kRegionCount);
        // north_america now includes CONUS (folded in during the natural-
        // boundary region consolidation) alongside Canada/Mexico/Central
        // America/Caribbean/Alaska/Greenland, which still need Copernicus —
        // so the old whole-region skip no longer works. Skip only the
        // CONUS tile cells (kConusBbox, 3DEP is authoritative there) at the
        // tile level instead, via load()'s skip_bbox.
        const Bbox* skip = (std::string(r.name) == "north_america") ? &kConusBbox : nullptr;
        if (verbose)
            std::cout << "[Terrain] === " << r.name << " (" << r.min_lon << "," << r.min_lat
                      << "," << r.max_lon << "," << r.max_lat << ") ===\n";
        bool ok = load(r.min_lon, r.min_lat, r.max_lon, r.max_lat,
                       TerrainSource::CopernicusGLO30, dest_srid, threads, verbose, skip);
        any_loaded = any_loaded || ok;
        if (verbose)
            std::cout << "[Terrain] " << r.name << ": " << (ok ? "OK" : "FAILED (no tiles loaded)") << "\n";
    }

    if (!any_loaded) {
        std::cerr << "[Terrain] loadGlobalTerrain: no region loaded any tiles\n";
        return false;
    }

    return true;
}
