#include "TerrainLoader.h"
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
#include <curl/curl.h>

namespace {

size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* f = static_cast<std::ofstream*>(stream);
    f->write(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

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
    for (int lat = lat0; lat < lat1; ++lat) {
        for (int lon = lon0; lon < lon1; ++lon) {
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
                         int threads, bool verbose) {
    auto tiles = tilesForBBox(source, min_lon, min_lat, max_lon, max_lat);
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
    // Feet is the unit convention throughout this codebase (kFeetPerMeter),
    // so results are converted from the raster's native meters.
    //
    // Deferred to a lambda instead of running unconditionally up front:
    // these are LANGUAGE sql functions, and Postgres parses/analyzes a SQL
    // function's body — including resolving public.terrain — at CREATE
    // FUNCTION time (unlike plpgsql, which defers). On a fresh -I reload
    // the terrain table doesn't exist yet at this point in load(), so
    // creating these before the table exists throws pqxx::undefined_table
    // and takes down the whole process. Only call this once table_exists
    // is actually true.
    auto createElevationFunctions = [&]() {
        pqxx::work txn(conn_);
        // p_srid defaults to 4326 (lon/lat) but accepts 3857 (Web Mercator,
        // the raster's native SRID) directly too — ST_Transform is a no-op
        // when source and target SRID already match, so passing 3857 costs
        // nothing extra. DROP first: adding a parameter changes the
        // signature, so CREATE OR REPLACE alone would leave the old 2-arg
        // overload in place instead of replacing it.
        txn.exec("DROP FUNCTION IF EXISTS public.elevation_at_point_ft(double precision, double precision)");
        txn.exec(
            "CREATE OR REPLACE FUNCTION public.elevation_at_point_ft("
            "  p_x double precision, p_y double precision, p_srid integer DEFAULT 4326"
            ") RETURNS double precision "
            "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
            "  SELECT ST_Value(t.rast, 1, pt.g) * 3.28084 "
            "  FROM public.terrain t, "
            "       (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 3857) AS g) pt "
            "  WHERE t.rast && pt.g AND ST_Value(t.rast, 1, pt.g) IS NOT NULL "
            "  LIMIT 1 "
            "$f$");
        // Samples elevation every p_interval_m meters along a geodesic
        // bearing out to p_distance_km, for terrain-ahead lookahead
        // profiles. ST_Project does the geodesic (spheroid) point
        // projection so bearing/distance stay accurate at any latitude —
        // it requires geography (always lon/lat internally), so a
        // non-4326 origin is transformed to 4326 once up front.
        txn.exec(
            "DROP FUNCTION IF EXISTS public.elevation_along_bearing_ft("
            "double precision, double precision, double precision, double precision, double precision)");
        txn.exec(
            "CREATE OR REPLACE FUNCTION public.elevation_along_bearing_ft("
            "  p_x double precision, p_y double precision, "
            "  p_bearing_deg double precision, p_distance_km double precision, "
            "  p_interval_m double precision DEFAULT 500, "
            "  p_srid integer DEFAULT 4326"
            ") RETURNS TABLE(distance_m double precision, lon double precision, "
            "                lat double precision, elevation_ft double precision) "
            "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
            "  WITH origin AS ("
            "    SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 4326)::geography AS g"
            "  ), pts AS ("
            "    SELECT gs::double precision AS distance_m, "
            "           ST_Project(origin.g, gs, radians(p_bearing_deg)) AS geog "
            "    FROM origin, generate_series(0::bigint, (p_distance_km * 1000)::bigint, "
            "                         greatest(p_interval_m, 1)::bigint) AS gs"
            "  ) "
            "  SELECT pts.distance_m, "
            "         ST_X(pts.geog::geometry) AS lon, "
            "         ST_Y(pts.geog::geometry) AS lat, "
            "         public.elevation_at_point_ft(ST_X(pts.geog::geometry), ST_Y(pts.geog::geometry)) AS elevation_ft "
            "  FROM pts "
            "  ORDER BY pts.distance_m "
            "$f$");
        txn.commit();
    };

    if (table_exists) createElevationFunctions();

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
        if (table_exists) createElevationFunctions();
        return true;
    }

    const std::string tmp_dir = "/tmp/terrain_tiles";
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
    std::atomic<long long> total_failed_batches{0};
    std::mutex io_mu;      // guards stdout/stderr so per-tile/batch lines don't interleave
    std::mutex raster_mu;  // serializes raster2pgsql/psql invocation — see below

    // Downloads+loads a single batch. use_append selects -a (append to an
    // existing terrain table) vs -I -M (create it) — the caller is
    // responsible for only passing false when the table doesn't exist yet.
    // db is the caller's own connection (each parallel worker owns one, so
    // this can run concurrently across batches with no shared DB state
    // besides the terrain/terrain_tiles tables themselves, which tolerate
    // concurrent appends fine).
    auto runBatch = [&](size_t batch_idx, pqxx::connection& db, bool use_append) -> bool {
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

        if (downloaded_paths.empty()) return false;  // whole batch unavailable, not an error

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
        gen_cmd << "raster2pgsql -s " << sourceSrid(source) << ":" << dest_srid << " -t 256x256 -F";
        gen_cmd << (use_append ? " -a" : " -I -M");
        for (auto& p : downloaded_paths) gen_cmd << " " << p;
        gen_cmd << " terrain > " << sql_file;

        if (verbose) {
            std::lock_guard lk(io_mu);
            std::cout << "  batch " << (batch_idx + 1) << "/" << n_batches
                      << ": loading " << downloaded_paths.size() << " tile(s)...\n";
        }

        // Serialize the actual raster2pgsql/psql invocation (downloads above
        // stay concurrent — only this part is locked). Running many
        // raster2pgsql instances at once was observed to intermittently
        // produce corrupted/truncated SQL output (parse errors like
        // "unterminated quoted string" on otherwise-valid tiles) — a race
        // in raster2pgsql/GDAL under concurrent invocation, not something
        // this loader can fix, so avoid triggering it instead.
        //
        // No password handling here (or anywhere in this codebase) —
        // auth relies on ~/.pgpass, which psql honors automatically for
        // this shell-out same as pqxx does for direct connections.
        int gen_rc, load_rc = -1;
        {
            std::lock_guard lk(raster_mu);
            gen_rc = system(gen_cmd.str().c_str());
            if (gen_rc == 0) {
                std::ostringstream load_cmd;
                load_cmd << "psql -h " << host_ << " -U " << user_ << " -d " << database_
                          << " -q -v ON_ERROR_STOP=1 -f " << sql_file;
                load_rc = system(load_cmd.str().c_str());
            }
        }

        for (auto& p : downloaded_paths) std::remove(p.c_str());
        std::remove(sql_file.c_str());

        if (gen_rc != 0 || load_rc != 0) {
            std::lock_guard lk(io_mu);
            std::cerr << "[Terrain] batch " << (batch_idx + 1) << "/" << n_batches
                      << " load failed (raster2pgsql rc=" << gen_rc
                      << ", psql rc=" << load_rc << ") for tiles:";
            for (auto& t : downloaded_tiles) std::cerr << " " << t.name;
            std::cerr << " — not marked as loaded, will retry on next run\n";
            total_failed_batches.fetch_add(1, std::memory_order_relaxed);
            return false;  // leave these tiles unmarked so a future run retries them
        }

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
        return true;
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
    createElevationFunctions();

    if (verbose) {
        std::cout << "Terrain data loaded (" << total_loaded.load() << " new tile(s)";
        if (total_failed_batches.load() > 0)
            std::cout << ", " << total_failed_batches.load() << " batch(es) failed and will retry next run";
        std::cout << ").\n";
    }

    return true;
}

namespace {
struct GlobalRegion { const char* name; double min_lon, min_lat, max_lon, max_lat; };
// Same 19 regions previously split across load_copernicus_regions.sh,
// load_copernicus_global_rest.sh, and load_copernicus_final.sh — see
// loadGlobalTerrain's doc comment in TerrainLoader.h for the rationale
// behind each group and what's deliberately excluded (US, Antarctica).
constexpr GlobalRegion kGlobalRegions[] = {
    // GA-relevant (formerly load_copernicus_regions.sh)
    {"canada",            -141, 41, -52, 60},
    {"mexico",            -118, 14, -86, 33},
    {"central_america",   -93, 7, -77, 18},
    {"caribbean",          -85, 10, -59, 27},
    // Rest of world (formerly load_copernicus_global_rest.sh)
    {"south_america",      -82, -56, -34, 13},
    {"europe",             -25, 34, 40, 71},
    {"africa",             -18, -35, 52, 38},
    {"middle_east",         25, 12, 63, 42},
    {"south_asia",          60, 5, 100, 38},
    {"east_asia",           95, 15, 150, 55},
    {"southeast_asia",      90, -11, 141, 25},
    {"oceania_australia",  110, -47, 180, -10},
    {"russia_north_asia",   40, 45, 180, 78},
    // Final gaps (formerly load_copernicus_final.sh)
    {"northern_canada",   -141, 60, -52, 84},
    {"alaska",            -170, 51, -129, 72},
    {"greenland",          -75, 58, -10, 84},
    {"svalbard_high_arctic", -10, 70, 65, 84},
    {"pacific_islands_west", 170, -25, 180, 25},
    {"pacific_islands_east", -180, -25, -150, 25},
};
} // namespace

bool TerrainLoader::loadGlobal(int dest_srid, int threads, bool verbose) {
    bool any_loaded = false;
    for (const auto& r : kGlobalRegions) {
        if (verbose)
            std::cout << "[Terrain] === " << r.name << " (" << r.min_lon << "," << r.min_lat
                      << "," << r.max_lon << "," << r.max_lat << ") ===\n";
        bool ok = load(r.min_lon, r.min_lat, r.max_lon, r.max_lat,
                       TerrainSource::CopernicusGLO30, dest_srid, threads, verbose);
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
