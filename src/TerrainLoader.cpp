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

    try {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE public.terrain_bands");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Terrain] buildTerrainBands error truncating: " << e.what() << "\n";
        return false;
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

    // Grid size (meters, Mercator) for the regional-bucketing tier below —
    // see buildTerrainBands' hierarchical-union comment further down.
    constexpr double kRegionSizeM = 500000.0;

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

    if (verbose)
        std::cout << "[Terrain] dumping and simplifying raster fragments for all bands "
                     "(single pass over " << n_bands << " bands)...\n";

    try {
        pqxx::work txn(conn);
        txn.exec("DROP TABLE IF EXISTS public.terrain_frags_staging");
        txn.exec(
            "CREATE TABLE public.terrain_frags_staging ("
            "  band_id integer NOT NULL,"
            "  rx integer NOT NULL,"
            "  ry integer NOT NULL,"
            "  geom public.geometry)");
        std::ostringstream sql;
        sql <<
            "INSERT INTO public.terrain_frags_staging(band_id, rx, ry, geom) "
            "SELECT val, floor(ST_X(ST_Centroid(geom)) / " << kRegionSizeM << ")::int, "
            "            floor(ST_Y(ST_Centroid(geom)) / " << kRegionSizeM << ")::int, geom "
            "FROM ("
            "  SELECT d.val::int AS val, " << frag_expr.str() << " AS geom "
            "  FROM public.terrain t, "
            "       LATERAL ST_DumpAsPolygons("
            "         ST_Reclass(t.rast, 1, '" << reclass_all.str() << "', '32BUI', 0), 1"
            "       ) d "
            "  WHERE d.val > 0"
            ") simplified";
        txn.exec(sql.str());
        txn.exec("CREATE INDEX ON public.terrain_frags_staging (band_id)");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Terrain] buildTerrainBands error building staging fragments: " << e.what() << "\n";
        return false;
    }

    // Bands are independent (disjoint band_min_ft/band_max_ft, no ON
    // CONFLICT/merge step), so — same pattern as main.cpp's node/way worker
    // pools — a fixed pool of threads pulls the next unprocessed band off a
    // shared atomic counter, each with its own pqxx::connection. Unlike the
    // node/way workers there's no shared db_flush_mu equivalent: nothing here
    // needs cross-thread serialization.
    std::atomic<int> next_band{0};
    std::atomic<long long> total_polygons{0};
    std::mutex io_mu;  // guards stdout/stderr so progress lines don't interleave

    auto worker = [&]() {
        // See the equivalent try/catch in loadTerrain's worker: an exception
        // escaping a std::thread's function aborts the entire process via
        // std::terminate(), so a transient connection failure here must not
        // be allowed to propagate past this thread.
        try {
            pqxx::connection wconn(connstr);
            while (true) {
                int i = next_band.fetch_add(1, std::memory_order_relaxed);
                if (i >= n_bands) break;

                int band_min = lo_ft + i * band_ft;
                int band_max = lo_ft + (i + 1) * band_ft;

            // Hierarchical (regional) union rather than one flat ST_Union
            // across every matching fragment countrywide: a flat/common
            // elevation band (e.g. the Great Plains) can touch most of the
            // loaded tiles at once, and even with fragments pre-simplified,
            // a union over hundreds of thousands of fragments at once can
            // still hang for the same underlying GEOS-scaling reason as the
            // rugged-terrain case. The staging table is already bucketed
            // into (rx, ry) above, so this just unions+simplifies within
            // each bucket first, and only then unions the (far fewer,
            // already-merged) bucket results — keeps every individual
            // ST_Union call's input small regardless of the band's total
            // footprint.
            std::ostringstream sql;
            sql <<
                "WITH regions AS ("
                "  SELECT " << wrapSimplify("ST_Union(geom)") << " AS geom "
                "  FROM public.terrain_frags_staging WHERE band_id = " << (i + 1)
                << "  GROUP BY rx, ry"
                ") "
                "INSERT INTO public.terrain_bands(band_min_ft, band_max_ft, geog) "
                "SELECT " << band_min << ", " << band_max << ", "
                          "(ST_Dump(" << geom_expr.str() << ")).geom "
                "FROM (SELECT ST_Union(geom) AS geom FROM regions) merged";

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
            }
        } catch (const std::exception& e) {
            std::lock_guard lk(io_mu);
            std::cerr << "[Terrain] worker thread error: " << e.what() << "\n";
        }
    };

    int nthreads = std::max(1, std::min(threads, n_bands));
    if (verbose)
        std::cout << "[Terrain] using " << nthreads << " thread(s)\n";
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) workers.emplace_back(worker);
    for (auto& w : workers) w.join();

    try {
        pqxx::work txn(conn);
        txn.exec("DROP TABLE IF EXISTS public.terrain_frags_staging");
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
