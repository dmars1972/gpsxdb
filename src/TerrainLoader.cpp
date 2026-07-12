#include "TerrainLoader.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <set>
#include <map>
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

bool loadTerrain(const std::string& server, const std::string& user,
                 const std::string& database, const std::string& password,
                 double min_lon, double min_lat, double max_lon, double max_lat,
                 TerrainSource source, int dest_srid, int band_ft, double simplify_m,
                 int threads, bool verbose) {
    auto tiles = tilesForBBox(source, min_lon, min_lat, max_lon, max_lat);
    if (tiles.empty()) {
        std::cerr << "[Terrain] bounding box produced no tiles\n";
        return false;
    }

    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;

    pqxx::connection conn(connstr);
    {
        pqxx::work txn(conn);
        txn.exec("CREATE EXTENSION IF NOT EXISTS postgis_raster");
        txn.exec(
            "CREATE TABLE IF NOT EXISTS terrain_tiles ("
            "  tile_name text PRIMARY KEY,"
            "  source text NOT NULL DEFAULT '3dep',"
            "  loaded_at timestamptz NOT NULL DEFAULT now())");
        // Backfills the column on a table created before source-tracking
        // existed (all pre-existing rows are 3DEP, matching the default).
        txn.exec("ALTER TABLE terrain_tiles ADD COLUMN IF NOT EXISTS source text NOT NULL DEFAULT '3dep'");
        // Point/corridor elevation lookups direct against the raw raster —
        // the consuming application (a separate Python service) needs
        // precise elevation at a point and along a short lookahead corridor,
        // not the classified elevation-band polygons (terrain_bands is a
        // separate, currently-deprioritized effort). CREATE OR REPLACE so
        // these stay current on every terrain_load run; only depends on
        // public.terrain existing by the time they're actually called, not
        // at creation time. Feet is the unit convention throughout this
        // codebase (band_min_ft/band_max_ft, kFeetPerMeter), so results are
        // converted from the raster's native meters.
        //
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
    }

    bool table_exists;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT EXISTS (SELECT 1 FROM information_schema.tables "
            "WHERE table_schema='public' AND table_name='terrain')");
        txn.commit();
        table_exists = r[0][0].as<bool>();
    }

    std::vector<Tile> to_load;
    for (auto& t : tiles) {
        pqxx::work txn(conn);
        auto r = txn.exec("SELECT 1 FROM terrain_tiles WHERE tile_name=$1",
                          pqxx::params{t.name});
        txn.commit();
        if (r.empty()) to_load.push_back(t);
        else if (verbose) std::cout << "  " << t.name << " already loaded, skipping\n";
    }

    if (to_load.empty()) {
        if (verbose) std::cout << "All requested tiles already loaded.\n";
        if (band_ft > 0)
            buildTerrainBands(server, user, database, password, band_ft, simplify_m,
                              threads, verbose);
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
        // this loader can fix, so avoid triggering it instead. setenv of
        // PGPASSWORD is also process-global, so it must be inside this lock
        // too — concurrent setenv/unsetenv from different threads is itself
        // a race.
        int gen_rc, load_rc = -1;
        {
            std::lock_guard lk(raster_mu);
            gen_rc = system(gen_cmd.str().c_str());
            if (gen_rc == 0) {
                std::ostringstream load_cmd;
                load_cmd << "psql -h " << server << " -U " << user << " -d " << database
                          << " -q -v ON_ERROR_STOP=1 -f " << sql_file;
                if (!password.empty()) setenv("PGPASSWORD", password.c_str(), 1);
                load_rc = system(load_cmd.str().c_str());
                if (!password.empty()) unsetenv("PGPASSWORD");
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
        total_loaded.fetch_add(static_cast<long long>(downloaded_tiles.size()),
                               std::memory_order_relaxed);
        return true;
    };

    // Bootstrap: if `terrain` doesn't exist yet, the batch that creates it
    // (-I) must run alone first — otherwise multiple threads could race to
    // -I it simultaneously. Try batches one at a time (skipping any that
    // turn out to have zero available tiles) until one actually succeeds.
    size_t first_parallel_batch = 0;
    if (!table_exists) {
        while (first_parallel_batch < n_batches) {
            if (runBatch(first_parallel_batch, conn, false)) {
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
                pqxx::connection wconn(connstr);
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

    if (verbose) {
        std::cout << "Terrain data loaded (" << total_loaded.load() << " new tile(s)";
        if (total_failed_batches.load() > 0)
            std::cout << ", " << total_failed_batches.load() << " batch(es) failed and will retry next run";
        std::cout << ").\n";
    }

    if (band_ft > 0)
        buildTerrainBands(server, user, database, password, band_ft, simplify_m,
                          threads, verbose);

    return true;
}

bool buildTerrainBands(const std::string& server, const std::string& user,
                       const std::string& database, const std::string& password,
                       int band_ft, double simplify_m, int threads, bool verbose) {
    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;
    pqxx::connection conn(connstr);

    {
        pqxx::work txn(conn);
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.terrain_bands ("
            "  id serial PRIMARY KEY,"
            "  band_min_ft integer NOT NULL,"
            "  band_max_ft integer NOT NULL,"
            "  geog public.geometry)");
        txn.exec(
            "CREATE INDEX IF NOT EXISTS terrain_bands_geog_idx "
            "ON public.terrain_bands USING GIST (geog)");
        // Tracks the parameters of an in-progress (possibly interrupted)
        // rebuild, plus whether its staging phase has fully finished — see
        // the resumability comment further down. Single row, same idiom as
        // osm_replication_state.
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.terrain_bands_build_state ("
            "  id integer PRIMARY KEY CHECK (id = 1),"
            "  band_ft integer NOT NULL,"
            "  simplify_m double precision NOT NULL,"
            "  lo_ft integer NOT NULL,"
            "  hi_ft integer NOT NULL,"
            "  region_size_m double precision NOT NULL DEFAULT 500000,"
            "  staging_complete boolean NOT NULL DEFAULT false)");
        // Older rows (from before region_size_m existed) predate the
        // resumability rework entirely, but guard anyway: without this a
        // stale row missing the column default's real value could compare
        // equal by accident.
        txn.exec(
            "ALTER TABLE public.terrain_bands_build_state "
            "ADD COLUMN IF NOT EXISTS region_size_m double precision NOT NULL DEFAULT 500000");
        // Whether the tier-1 fine-region materialization pass (see
        // terrain_region_staging below) has finished. Same idiom as
        // staging_complete: an older row predates this column and defaults
        // to false, correctly forcing that pass to run once on the next
        // call even though its own prerequisite (terrain_frags_staging)
        // may already be sitting there complete from before this existed.
        txn.exec(
            "ALTER TABLE public.terrain_bands_build_state "
            "ADD COLUMN IF NOT EXISTS regions_complete boolean NOT NULL DEFAULT false");
        // Which (rx, ry) staging grid cells (see kRegionSizeM below) have
        // already been reclassified/dumped into terrain_frags_staging —
        // the checkpoint list for resuming an interrupted staging phase.
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.terrain_frags_staging_progress ("
            "  rx integer NOT NULL, ry integer NOT NULL, PRIMARY KEY (rx, ry))");
        // Tier-1 output: one pre-unioned+simplified polygon per (band_id,
        // rx, ry) fine region, computed once by a parallel pass (see
        // below) and shared by every band's tier-2/3 union — this is what
        // turns tier 1 from "however many bands happen to be running
        // concurrently" parallelism into full --threads-wide parallelism
        // regardless of how few bands are expensive enough to matter.
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.terrain_region_staging ("
            "  band_id integer NOT NULL, rx integer NOT NULL, ry integer NOT NULL,"
            "  geom public.geometry)");
        // Checkpoint list for the tier-1 materialization pass — which
        // geographic chunks (see kRegionMaterializeChunks below) have been
        // fully computed and written to terrain_region_staging.
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.terrain_region_staging_progress ("
            "  chunk_id integer PRIMARY KEY)");
        txn.commit();
    }

    double min_m, max_m;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT min(s.min), max(s.max) "
            "FROM public.terrain t, LATERAL ST_SummaryStats(t.rast, 1) s");
        txn.commit();
        if (r.empty() || r[0][0].is_null()) {
            if (verbose) std::cout << "[Terrain] no terrain data loaded, skipping band generation\n";
            return false;
        }
        min_m = r[0][0].as<double>();
        max_m = r[0][1].as<double>();
    }

    // Round the observed elevation range out to whole bands so every real
    // value falls strictly inside some (lo, hi] bucket.
    int lo_ft = static_cast<int>(std::floor(min_m * kFeetPerMeter / band_ft)) * band_ft;
    int hi_ft = static_cast<int>(std::ceil(max_m * kFeetPerMeter / band_ft)) * band_ft;
    int n_bands = (hi_ft - lo_ft) / band_ft;
    if (n_bands < 1) n_bands = 1;

    if (verbose)
        std::cout << "[Terrain] building " << n_bands << " elevation band(s) ("
                  << band_ft << "ft each, " << lo_ft << "ft to " << hi_ft
                  << "ft), merging across all loaded tiles...\n";

    // Grid size (meters, Mercator) for both the staging-chunk tier and the
    // regional-bucketing tier further down — see the hierarchical-union
    // comment there, and the tuning note below, for why this value in
    // particular. Defined here (rather than closer to its uses) because the
    // resumability check right after needs it: this value is baked into
    // every (rx, ry) coordinate already written to terrain_frags_staging
    // and terrain_frags_staging_progress, so a resume against
    // differently-gridded leftover data would silently mix incompatible
    // cell numbering — region_size_m below guards against exactly that.
    //
    // Was 500km; shrunk to 100km after a live OOM crash processing a
    // globally-widespread real band (3000-3500ft) — one region's ST_Union
    // input was still large enough at 500km to blow past available memory
    // (~7.6GB RSS + ~12GB shared for a single backend, enough to trigger
    // the kernel OOM killer and take down the whole Postgres cluster with
    // it, since killing any one backend forces a full cluster restart).
    // Finer cells mean smaller per-region unions (lower peak memory per
    // GEOS call) at the cost of more regions to merge in the outer union —
    // a direct trade of total call count for peak-per-call memory, which is
    // the right trade here since peak memory is what caused the crash.
    constexpr double kRegionSizeM = 100000.0;

    // Resumability: this whole function is meant to fully rebuild
    // terrain_bands from scratch on every call (see the doc comment in
    // TerrainLoader.h) — but at global scale, the staging phase below is a
    // multi-hour, CPU-bound operation with no natural checkpoints, and
    // losing all of it to a crash/kill/reboot partway through (observed
    // firsthand: a single global rebuild ran 7+ hours before this fix
    // existed) meant a full restart from zero. terrain_bands_build_state
    // records the parameters of whatever rebuild is currently in flight;
    // if a call arrives whose parameters match a row already there, this
    // is treated as resuming that same interrupted rebuild rather than
    // starting over — staging_complete and terrain_frags_staging_progress
    // say exactly how much of it is already done. Any other case (no row,
    // or parameters differ — e.g. a genuinely new rebuild requested with
    // different band_ft/simplify_m/region_size_m) wipes everything and
    // starts clean, so resumption never silently reuses stale/incompatible
    // data — region_size_m's inclusion here is exactly what makes tuning
    // it (as above) safe to do even with an interrupted run's state still
    // sitting in these tables from before the change.
    bool resuming = false;
    bool staging_complete = false;
    bool regions_complete = false;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT staging_complete, regions_complete FROM public.terrain_bands_build_state "
            "WHERE id = 1 AND band_ft = $1 AND simplify_m = $2 AND lo_ft = $3 AND hi_ft = $4 "
            "AND region_size_m = $5",
            pqxx::params{band_ft, simplify_m, lo_ft, hi_ft, kRegionSizeM});
        txn.commit();
        if (!r.empty()) {
            resuming = true;
            staging_complete = r[0][0].as<bool>();
            regions_complete = r[0][1].as<bool>();
        }
    }

    if (resuming && verbose)
        std::cout << "[Terrain] resuming an interrupted rebuild (staging "
                  << (staging_complete ? "complete" : "incomplete") << ", regions "
                  << (regions_complete ? "complete" : "incomplete") << ")\n";

    if (!resuming) {
        try {
            pqxx::work txn(conn);
            txn.exec("TRUNCATE public.terrain_bands");
            txn.exec("DROP TABLE IF EXISTS public.terrain_frags_staging");
            txn.exec(
                "CREATE TABLE public.terrain_frags_staging ("
                "  band_id integer NOT NULL,"
                "  rx integer NOT NULL,"
                "  ry integer NOT NULL,"
                "  geom public.geometry)");
            txn.exec("TRUNCATE public.terrain_frags_staging_progress");
            txn.exec("TRUNCATE public.terrain_region_staging");
            txn.exec("TRUNCATE public.terrain_region_staging_progress");
            txn.exec(
                "INSERT INTO public.terrain_bands_build_state(id, band_ft, simplify_m, lo_ft, hi_ft, region_size_m, staging_complete, regions_complete) "
                "VALUES (1, $1, $2, $3, $4, $5, false, false) "
                "ON CONFLICT (id) DO UPDATE SET band_ft=$1, simplify_m=$2, lo_ft=$3, hi_ft=$4, region_size_m=$5, staging_complete=false, regions_complete=false",
                pqxx::params{band_ft, simplify_m, lo_ft, hi_ft, kRegionSizeM});
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[Terrain] buildTerrainBands error truncating: " << e.what() << "\n";
            return false;
        }
    }

    // Simplify at every tier (fragment, region, final) rather than just the
    // final unioned shape. Rugged terrain (e.g. foothills/canyons) fragments
    // into thousands of tiny raster-pixel-aligned polygons — unioning them
    // raw is fine (~seconds), but ST_SimplifyPreserveTopology on the
    // resulting huge multipolygon can pathologically hang (GEOS noding
    // blowup on that many collinear pixel-edge vertices), badly enough that
    // even Postgres's own statement_timeout can't cancel it (stuck inside a
    // single uninterruptible GEOS call). Pre-simplifying at each tier keeps
    // every individual simplify/union call's input small.
    auto wrapSimplify = [&](const std::string& expr) -> std::string {
        if (simplify_m <= 0) return expr;
        std::ostringstream o;
        o << "ST_SimplifyPreserveTopology(" << expr << ", " << simplify_m << ")";
        return o.str();
    };
    std::ostringstream frag_expr, geom_expr;
    frag_expr << wrapSimplify("d.geom");
    geom_expr << wrapSimplify("geom");

    // Reclassifying+dumping the whole raster table is itself expensive at
    // country scale (hundreds of thousands of raster rows) — doing that
    // once per band (as an earlier version of this function did) multiplies
    // that cost by n_bands, badly enough that even a single band's
    // reclass+dump alone can exceed a minute at CONUS scale, before any
    // union happens. So it's done exactly once here, for every band at
    // once (one big multi-value reclass, like the 3000-14500 style Colorado
    // used before per-band existed), writing pre-simplified per-chunk
    // fragments — already bucketed into the regional grid used by the
    // hierarchical union below — into a staging table. Each band's worker
    // then only does a cheap indexed filter over this staging table, not a
    // fresh raster scan.
    std::ostringstream reclass_all;
    reclass_all << std::fixed << std::setprecision(4);
    for (int i = 0; i < n_bands; ++i) {
        double band_lo_m = (lo_ft + i * band_ft) / kFeetPerMeter;
        double band_hi_m = (lo_ft + (i + 1) * band_ft) / kFeetPerMeter;
        if (i > 0) reclass_all << ", ";
        reclass_all << "(" << band_lo_m << "-" << band_hi_m << "]:" << (i + 1);
    }

    if (staging_complete) {
        if (verbose) std::cout << "[Terrain] staging already complete, skipping to band union\n";
    } else {
        // Chunked by the same kRegionSizeM grid used one tier down for the
        // hierarchical union, rather than one INSERT...SELECT over the
        // entire terrain table: at global scale that single query is a
        // multi-hour, all-or-nothing operation with no checkpoint (this was
        // observed directly — a global rebuild ran 7+ hours on this one
        // statement before this fix existed) and no parallelism. Chunking
        // by grid cell gives both: each cell commits (and is marked done in
        // terrain_frags_staging_progress) independently, so a crash/kill
        // only loses whatever cell(s) were mid-flight, and — since cells are
        // independent, like bands below — a thread pool processes them
        // concurrently instead of one connection doing the whole table
        // serially. t.rast && envelope uses the GIST index to cheaply skip
        // cells with no matching tiles (open ocean, etc.), so the many empty
        // cells in a near-global bounding box cost little.
        // buildTerrainBands isn't given dest_srid (unlike loadTerrain) — the
        // terrain table's actual SRID (whatever it was loaded with, default
        // 3857 but -4 selects 4326) has to be read back from the data
        // itself rather than assumed, for ST_MakeEnvelope below.
        int terrain_srid;
        {
            pqxx::work txn(conn);
            auto r = txn.exec("SELECT ST_SRID(rast) FROM public.terrain LIMIT 1");
            txn.commit();
            terrain_srid = r[0][0].as<int>();
        }

        double ext_minx, ext_miny, ext_maxx, ext_maxy;
        {
            pqxx::work txn(conn);
            auto r = txn.exec(
                "SELECT ST_XMin(e), ST_YMin(e), ST_XMax(e), ST_YMax(e) "
                "FROM (SELECT ST_Extent(ST_Envelope(rast)) AS e FROM public.terrain) t");
            txn.commit();
            ext_minx = r[0][0].as<double>();
            ext_miny = r[0][1].as<double>();
            ext_maxx = r[0][2].as<double>();
            ext_maxy = r[0][3].as<double>();
        }
        int rx_lo = static_cast<int>(std::floor(ext_minx / kRegionSizeM));
        int rx_hi = static_cast<int>(std::floor(ext_maxx / kRegionSizeM));
        int ry_lo = static_cast<int>(std::floor(ext_miny / kRegionSizeM));
        int ry_hi = static_cast<int>(std::floor(ext_maxy / kRegionSizeM));

        std::vector<std::pair<int,int>> cells;
        {
            pqxx::work txn(conn);
            auto done_rows = txn.exec("SELECT rx, ry FROM public.terrain_frags_staging_progress");
            txn.commit();
            std::set<std::pair<int,int>> done;
            for (const auto& row : done_rows) done.insert({row[0].as<int>(), row[1].as<int>()});
            for (int rx = rx_lo; rx <= rx_hi; ++rx)
                for (int ry = ry_lo; ry <= ry_hi; ++ry)
                    if (!done.count({rx, ry})) cells.push_back({rx, ry});
        }

        if (verbose)
            std::cout << "[Terrain] staging " << cells.size() << " grid cell(s) ("
                      << (rx_hi - rx_lo + 1) * (ry_hi - ry_lo + 1) - static_cast<int>(cells.size())
                      << " already done)...\n";

        std::atomic<size_t> next_cell{0};
        std::atomic<int> cells_failed{0};
        std::mutex stage_io_mu;
        auto stage_worker = [&]() {
            try {
                pqxx::connection wconn(connstr);
                while (true) {
                    size_t idx = next_cell.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= cells.size()) break;
                    auto [rx, ry] = cells[idx];
                    double cx0 = rx * kRegionSizeM, cx1 = (rx + 1) * kRegionSizeM;
                    double cy0 = ry * kRegionSizeM, cy1 = (ry + 1) * kRegionSizeM;
                    // t.rast && envelope alone would let a raster row whose
                    // bbox straddles a cell boundary match more than one
                    // cell's query, reclassifying/dumping (and inserting)
                    // it once per matching chunk — real duplicated geometry
                    // near every 500km grid line, not just an inefficiency.
                    // The && test stays as the cheap GIST-indexed
                    // pre-filter (narrows 3M+ rows down to a small
                    // candidate set fast); ST_Contains on that already-small
                    // set then gives each row to exactly one cell, by which
                    // cell contains its centroid — no duplicates, no gaps.
                    std::ostringstream sql;
                    sql << std::fixed << std::setprecision(4) <<
                        "INSERT INTO public.terrain_frags_staging(band_id, rx, ry, geom) "
                        "SELECT val, " << rx << ", " << ry << ", geom "
                        "FROM ("
                        "  SELECT d.val::int AS val, " << frag_expr.str() << " AS geom "
                        "  FROM public.terrain t, "
                        "       LATERAL ST_DumpAsPolygons("
                        "         ST_Reclass(t.rast, 1, '" << reclass_all.str() << "', '32BUI', 0), 1"
                        "       ) d "
                        "  WHERE d.val > 0 AND t.rast && ST_MakeEnvelope("
                        << cx0 << ", " << cy0 << ", " << cx1 << ", " << cy1 << ", " << terrain_srid << ") "
                        "  AND ST_Contains(ST_MakeEnvelope("
                        << cx0 << ", " << cy0 << ", " << cx1 << ", " << cy1 << ", " << terrain_srid << "), "
                        "ST_Centroid(t.rast::geometry))"
                        ") simplified";
                    try {
                        pqxx::work txn(wconn);
                        txn.exec(sql.str());
                        txn.exec("INSERT INTO public.terrain_frags_staging_progress(rx, ry) VALUES ($1, $2)",
                                 pqxx::params{rx, ry});
                        txn.commit();
                    } catch (const std::exception& e) {
                        std::lock_guard lk(stage_io_mu);
                        std::cerr << "[Terrain] staging cell (" << rx << "," << ry << ") error: "
                                  << e.what() << " — will retry next run\n";
                        cells_failed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard lk(stage_io_mu);
                std::cerr << "[Terrain] staging worker thread error: " << e.what() << "\n";
            }
        };

        int stage_threads = std::max(1, std::min(threads, static_cast<int>(cells.size())));
        if (!cells.empty()) {
            std::vector<std::thread> stage_workers;
            stage_workers.reserve(stage_threads);
            for (int t = 0; t < stage_threads; ++t) stage_workers.emplace_back(stage_worker);
            for (auto& w : stage_workers) w.join();
        }

        if (cells_failed.load() > 0) {
            std::cerr << "[Terrain] " << cells_failed.load()
                      << " staging cell(s) failed — re-run to resume and retry them\n";
            return false;
        }

        try {
            pqxx::work txn(conn);
            txn.exec("CREATE INDEX IF NOT EXISTS terrain_frags_staging_band_idx ON public.terrain_frags_staging (band_id)");
            txn.exec("UPDATE public.terrain_bands_build_state SET staging_complete = true WHERE id = 1");
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[Terrain] buildTerrainBands error finalizing staging: " << e.what() << "\n";
            return false;
        }
    }

    // Tier 1 (fine-region unions), materialized as its own parallel pass
    // rather than computed inline within each band's own query as
    // originally written. Inline, tier 1's parallelism was capped by
    // however many bands happened to be running concurrently — usually
    // fine, but the handful of genuinely globally-widespread bands (e.g.
    // common continental-plains elevations) dominate tier 1's total cost
    // so heavily that this was effectively single-threaded in practice:
    // confirmed live, one such band alone took 1+ hour on a single
    // connection while --threads-1 other workers sat idle, having long
    // since finished their own (much cheaper) bands. Partitioning this
    // pass by geography (chunk_id) instead of by band means every band's
    // share of tier 1 — expensive or not — gets split across the full
    // worker pool.
    if (regions_complete) {
        if (verbose) std::cout << "[Terrain] fine-region materialization already complete, skipping to band union\n";
    } else {
        // Independent of --threads — just checkpoint/parallelism
        // granularity for this pass. Higher than typical thread counts so
        // work stays reasonably balanced even though chunk cost varies
        // (a chunk touching one of the huge bands costs far more than one
        // that doesn't).
        constexpr int kRegionMaterializeChunks = 64;

        std::vector<int> chunks_remaining;
        {
            pqxx::work txn(conn);
            auto done_rows = txn.exec("SELECT chunk_id FROM public.terrain_region_staging_progress");
            txn.commit();
            std::set<int> done;
            for (const auto& row : done_rows) done.insert(row[0].as<int>());
            for (int c = 0; c < kRegionMaterializeChunks; ++c)
                if (!done.count(c)) chunks_remaining.push_back(c);
        }
        if (verbose)
            std::cout << "[Terrain] materializing fine regions: " << chunks_remaining.size()
                      << "/" << kRegionMaterializeChunks << " chunk(s) remaining...\n";

        std::atomic<size_t> next_chunk{0};
        std::atomic<int> chunks_failed{0};
        std::mutex region_io_mu;
        auto region_worker = [&]() {
            // See the equivalent try/catch elsewhere in this function: an
            // exception escaping a std::thread's function aborts the
            // entire process, so a transient connection failure here must
            // not be allowed to propagate past this thread.
            try {
                pqxx::connection wconn(connstr);
                while (true) {
                    size_t idx = next_chunk.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= chunks_remaining.size()) break;
                    int chunk = chunks_remaining[idx];

                    std::ostringstream sql;
                    sql <<
                        "INSERT INTO public.terrain_region_staging(band_id, rx, ry, geom) "
                        "SELECT band_id, rx, ry, " << wrapSimplify("ST_Union(geom)") << " "
                        "FROM public.terrain_frags_staging "
                        "WHERE MOD(ABS(rx*31 + ry*17), " << kRegionMaterializeChunks << ") = " << chunk << " "
                        "GROUP BY band_id, rx, ry";
                    try {
                        pqxx::work txn(wconn);
                        txn.exec(sql.str());
                        txn.exec("INSERT INTO public.terrain_region_staging_progress(chunk_id) VALUES ($1)",
                                 pqxx::params{chunk});
                        txn.commit();
                    } catch (const std::exception& e) {
                        std::lock_guard lk(region_io_mu);
                        std::cerr << "[Terrain] region chunk " << chunk << " error: " << e.what()
                                  << " — will retry next run\n";
                        chunks_failed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard lk(region_io_mu);
                std::cerr << "[Terrain] region materialization worker error: " << e.what() << "\n";
            }
        };

        int region_threads = std::max(1, std::min(threads, static_cast<int>(chunks_remaining.size())));
        if (!chunks_remaining.empty()) {
            std::vector<std::thread> region_workers;
            region_workers.reserve(region_threads);
            for (int t = 0; t < region_threads; ++t) region_workers.emplace_back(region_worker);
            for (auto& w : region_workers) w.join();
        }

        if (chunks_failed.load() > 0) {
            std::cerr << "[Terrain] " << chunks_failed.load()
                      << " region chunk(s) failed — re-run to resume and retry them\n";
            return false;
        }

        try {
            pqxx::work txn(conn);
            txn.exec("CREATE INDEX IF NOT EXISTS terrain_region_staging_band_idx ON public.terrain_region_staging (band_id)");
            txn.exec("UPDATE public.terrain_bands_build_state SET regions_complete = true WHERE id = 1");
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[Terrain] buildTerrainBands error finalizing region materialization: " << e.what() << "\n";
            return false;
        }
    }

    // Bands are independent (disjoint band_min_ft/band_max_ft, no ON
    // CONFLICT/merge step), so — same pattern as main.cpp's node/way worker
    // pools — a fixed pool of threads pulls the next unprocessed band off a
    // shared atomic counter, each with its own pqxx::connection. Unlike the
    // node/way workers there's no shared db_flush_mu equivalent: nothing here
    // needs cross-thread serialization.
    //
    // Each band's INSERT is already one atomic transaction (commits all of
    // that band's polygons or none), so resumability here needs no extra
    // tracking table — a band already present in terrain_bands (by
    // band_min_ft) simply isn't re-added to the work list.
    std::vector<int> band_indices;
    {
        pqxx::work txn(conn);
        auto done_rows = txn.exec("SELECT DISTINCT band_min_ft FROM public.terrain_bands");
        txn.commit();
        std::set<int> done_mins;
        for (const auto& row : done_rows) done_mins.insert(row[0].as<int>());
        for (int i = 0; i < n_bands; ++i)
            if (!done_mins.count(lo_ft + i * band_ft)) band_indices.push_back(i);
    }
    if (verbose && resuming)
        std::cout << "[Terrain] " << (n_bands - static_cast<int>(band_indices.size()))
                  << "/" << n_bands << " band(s) already done, unioning "
                  << band_indices.size() << " remaining\n";

    // Cost-aware admission control via two independent worker pools rather
    // than list-order tricks. Three list-position-based mitigations were
    // tried and each failed live: sequential dispatch let all threads
    // arrive at a costly run of consecutive bands (811-820) simultaneously;
    // a plain shuffle fixed that specific cluster but let a different
    // random grouping of expensive bands (834/836/844/846/852) cause a
    // second crash; a rank->bucket->concatenate scheme (meant to spread the
    // costliest bands into different segments of the queue) still packed
    // the same handful of extreme bands (811-831, all within the top ~20
    // by cost) into the first few positions of every bucket, so fast
    // threads clearing cheap filler caught up to multiple bucket-heads
    // within the same window as the first one was still running — a third
    // near-crash. With only 62 of 869 bands having any real cost and the
    // rest near-instant/empty, there isn't enough cheap filler between
    // expensive bands to buy real wall-clock separation from list position
    // alone, regardless of how cleverly it's ordered. A first attempt at a
    // shared semaphore (any thread blocks on acquire() before running an
    // expensive band) was also wrong in a different way: cost-descending
    // dispatch put all 19 expensive bands at the front of the queue, so all
    // `threads` workers immediately claimed one each and 9 sat idle blocked
    // on the semaphore instead of ever reaching the ~800 cheap bands behind
    // them — safe, but wasted nearly all the parallelism.
    //
    // So: two separate pools instead of one shared queue. Query cost per
    // band up front (terrain_region_staging row count per band_id —
    // exactly this band's input size for the tier-2/3 union below, and a
    // cheap proxy already computed by the tier-1 pass) and split into
    // cheap/expensive lists. The cheap pool (the ~800+ bands at or below
    // kExpensiveBandThreshold) runs on the full --threads pool with no
    // gating. The expensive pool (811-829-ish, the ~19 bands above
    // threshold) runs concurrently but on its own dedicated
    // kExpensiveBandSlots-sized pool, processed cost-descending — capped at
    // 1, since that's the only configuration actually proven safe: the
    // standalone single-band test (band 817, 12,791 regions) ran 13+ hours
    // without OOMing when it was the only thing running, and two bands from
    // this same cluster have roughly double that region count. Neither pool
    // can block the other.
    constexpr long long kExpensiveBandThreshold = 2000;
    constexpr int kExpensiveBandSlots = 1;
    std::map<int, long long> band_cost;  // band_id (1-based) -> region count
    {
        pqxx::work txn(conn);
        auto rows = txn.exec(
            "SELECT band_id, count(*) FROM public.terrain_region_staging GROUP BY band_id");
        txn.commit();
        for (const auto& row : rows) band_cost[row[0].as<int>()] = row[1].as<long long>();
    }
    auto costOf = [&](int i) -> long long {
        auto it = band_cost.find(i + 1);
        return it == band_cost.end() ? 0 : it->second;
    };
    std::vector<int> cheap_indices, expensive_indices;
    for (int i : band_indices)
        (costOf(i) > kExpensiveBandThreshold ? expensive_indices : cheap_indices).push_back(i);
    std::stable_sort(expensive_indices.begin(), expensive_indices.end(),
                      [&](int a, int b) { return costOf(a) > costOf(b); });  // worst first
    if (verbose && !expensive_indices.empty())
        std::cout << "[Terrain] " << expensive_indices.size()
                  << " expensive band(s) (>" << kExpensiveBandThreshold
                  << " regions) will run on a dedicated " << kExpensiveBandSlots
                  << "-thread pool alongside " << cheap_indices.size() << " cheap band(s)\n";

    std::atomic<long long> total_polygons{0};
    std::mutex io_mu;  // guards stdout/stderr so progress lines don't interleave

    // Shared per-band execution, parameterized over which list + counter a
    // pool pulls from so the cheap and expensive pools reuse identical
    // query logic without duplicating it.
    auto runBand = [&](pqxx::connection& wconn, int i) {
        int band_min = lo_ft + i * band_ft;
        int band_max = lo_ft + (i + 1) * band_ft;

            // Four-tier hierarchical union rather than one flat ST_Union
            // across every matching fragment globally: a flat/widespread
            // elevation band (e.g. continental plains/plateau terrain) can
            // touch most of the loaded tiles at once, and even with
            // fragments pre-simplified, a union over hundreds of thousands
            // of fragments at once can still hang for the same underlying
            // GEOS-scaling reason as the rugged-terrain case.
            //
            // Tier 1 (per-(rx,ry) fine-region unions) is NOT computed here
            // — it's already sitting in terrain_region_staging, done by the
            // parallel materialization pass above. This query does tiers
            // 2-4. A single coarser-grouping tier (kSuperRegionFactor=10,
            // ~100 fine regions per group) was enough to stop the original
            // OOM crash, but for the single worst band in the dataset
            // (813, 20,968 fine regions — more than 817's 12,791, which
            // itself needed 13+ hours under that 3-tier query and never
            // reached completion) it still wasn't fast enough: ~210
            // super-region groups (each unioning ~100 already-complex
            // fine-region polygons) and a final flat union of ~210 more
            // ran over 14 hours with no end in sight. Splitting that one
            // 10x jump into two gentler 5x steps (mid-regions, then
            // super-regions) keeps every intermediate ST_Union call bounded
            // to ~25 inputs on average instead of ~100, and for band 813
            // specifically shrinks the truly-final merge from ~210 inputs
            // down to ~34 — GEOS union cost empirically scales worse than
            // linearly with input count/complexity, so more numerous but
            // smaller union calls should beat fewer, larger ones even
            // though the total amount of geometry being merged is the same.
            constexpr int kMidRegionFactor = 5;    // fine (100km) -> mid (~500km)
            constexpr int kSuperRegionFactor = 5;  // mid -> super (~2500km)
            std::ostringstream sql;
            sql <<
                "WITH midregions AS ("
                "  SELECT floor(rx / " << kMidRegionFactor << "::float) AS mrx, "
                "         floor(ry / " << kMidRegionFactor << "::float) AS mry, "
                << wrapSimplify("ST_Union(geom)") << " AS geom "
                "  FROM public.terrain_region_staging WHERE band_id = " << (i + 1)
                << "  GROUP BY floor(rx / " << kMidRegionFactor << "::float), "
                          "floor(ry / " << kMidRegionFactor << "::float)"
                "), superregions AS ("
                "  SELECT " << wrapSimplify("ST_Union(geom)") << " AS geom "
                "  FROM midregions"
                << "  GROUP BY floor(mrx / " << kSuperRegionFactor << "::float), "
                          "floor(mry / " << kSuperRegionFactor << "::float)"
                ") "
                "INSERT INTO public.terrain_bands(band_min_ft, band_max_ft, geog) "
                "SELECT " << band_min << ", " << band_max << ", "
                          "(ST_Dump(" << geom_expr.str() << ")).geom "
                "FROM (SELECT ST_Union(geom) AS geom FROM superregions) merged";

            try {
                pqxx::work txn(wconn);
                auto r = txn.exec(sql.str());
                txn.commit();
                long long n = r.affected_rows();
                total_polygons.fetch_add(n, std::memory_order_relaxed);
                if (verbose) {
                    std::lock_guard lk(io_mu);
                    std::cout << "  band " << (i + 1) << "/" << n_bands
                              << " (" << band_min << "-" << band_max << "ft): "
                              << n << " polygon(s)\n";
                }
            } catch (const std::exception& e) {
                std::lock_guard lk(io_mu);
                std::cerr << "[Terrain] buildTerrainBands band " << (i + 1) << "/" << n_bands
                          << " (" << band_min << "-" << band_max << "ft) error: "
                          << e.what() << "\n";
                // Continue with remaining bands rather than losing the whole rebuild.
            }
    };

    // Bands are independent (disjoint band_min_ft/band_max_ft, no ON
    // CONFLICT/merge step) — same pattern as main.cpp's node/way worker
    // pools, a fixed pool of threads pulls the next unprocessed index off a
    // shared atomic counter, each with its own pqxx::connection. Factored
    // so the cheap and expensive pools (below) share this logic while
    // pulling from separate index lists/counters, since neither pool may
    // block on the other.
    auto makePool = [&](std::vector<int>& indices, int pool_size) {
        auto next = std::make_shared<std::atomic<size_t>>(0);
        std::vector<std::thread> pool;
        pool.reserve(pool_size);
        for (int t = 0; t < pool_size; ++t) {
            pool.emplace_back([&, next]() {
                // See the equivalent try/catch in loadTerrain's worker: an
                // exception escaping a std::thread's function aborts the
                // entire process via std::terminate(), so a transient
                // connection failure here must not be allowed to propagate
                // past this thread.
                try {
                    pqxx::connection wconn(connstr);
                    while (true) {
                        size_t idx = next->fetch_add(1, std::memory_order_relaxed);
                        if (idx >= indices.size()) break;
                        runBand(wconn, indices[idx]);
                    }
                } catch (const std::exception& e) {
                    std::lock_guard lk(io_mu);
                    std::cerr << "[Terrain] worker thread error: " << e.what() << "\n";
                }
            });
        }
        return pool;
    };

    if (!band_indices.empty()) {
        int nthreads = std::max(1, std::min(threads, static_cast<int>(cheap_indices.size())));
        int ex_threads = std::min(kExpensiveBandSlots, static_cast<int>(expensive_indices.size()));
        if (verbose)
            std::cout << "[Terrain] using " << nthreads << " thread(s) for "
                      << cheap_indices.size() << " cheap band(s), " << ex_threads
                      << " thread(s) for " << expensive_indices.size() << " expensive band(s)\n";
        std::vector<std::thread> all_workers;
        if (!cheap_indices.empty())
            for (auto& w : makePool(cheap_indices, nthreads)) all_workers.push_back(std::move(w));
        if (!expensive_indices.empty())
            for (auto& w : makePool(expensive_indices, ex_threads)) all_workers.push_back(std::move(w));
        for (auto& w : all_workers) w.join();
    }

    // Full success (every band processed, whether just now or across a
    // resumed run) — clear the in-progress state so the next call, even
    // with identical parameters, correctly does a fresh rebuild rather than
    // being mistaken for resuming this now-finished one.
    try {
        pqxx::work txn(conn);
        txn.exec("DROP TABLE IF EXISTS public.terrain_frags_staging");
        txn.exec("TRUNCATE public.terrain_frags_staging_progress");
        txn.exec("TRUNCATE public.terrain_region_staging");
        txn.exec("TRUNCATE public.terrain_region_staging_progress");
        txn.exec("DELETE FROM public.terrain_bands_build_state WHERE id = 1");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Terrain] buildTerrainBands error dropping staging table: " << e.what() << "\n";
    }

    if (verbose)
        std::cout << "[Terrain] terrain_bands rebuilt: " << total_polygons.load()
                  << " polygon(s) across " << n_bands << " band(s)\n";
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

bool loadGlobalTerrain(const std::string& server, const std::string& user,
                       const std::string& database, const std::string& password,
                       int dest_srid, int band_ft, double simplify_m,
                       int threads, bool verbose) {
    bool any_loaded = false;
    for (const auto& r : kGlobalRegions) {
        if (verbose)
            std::cout << "[Terrain] === " << r.name << " (" << r.min_lon << "," << r.min_lat
                      << "," << r.max_lon << "," << r.max_lat << ") ===\n";
        bool ok = loadTerrain(server, user, database, password,
                             r.min_lon, r.min_lat, r.max_lon, r.max_lat,
                             TerrainSource::CopernicusGLO30, dest_srid,
                             /*band_ft=*/0, simplify_m, threads, verbose);
        any_loaded = any_loaded || ok;
        if (verbose)
            std::cout << "[Terrain] " << r.name << ": " << (ok ? "OK" : "FAILED (no tiles loaded)") << "\n";
    }

    if (!any_loaded) {
        std::cerr << "[Terrain] loadGlobalTerrain: no region loaded any tiles\n";
        return false;
    }

    if (band_ft > 0)
        buildTerrainBands(server, user, database, password, band_ft, simplify_m, threads, verbose);

    return true;
}
