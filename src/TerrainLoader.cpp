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

struct Tile { int lat; int lon; std::string name; };

// 3DEP tiles are named by their NW corner: "n40w105" covers lat [39,40] x
// lon [-105,-104] — the "n" value is the tile's NORTH edge, but the "w"
// value is the tile's WEST edge (confirmed empirically: n40w105 contains
// Denver at 39.7N/-105.0W). So the row loop variable (a tile's south edge,
// i.e. floor(lat)) needs +1 to become the naming convention's north edge;
// the column loop variable (a tile's west edge, i.e. floor(lon)) is used
// as-is. US-only coverage, so always north/west.
std::vector<Tile> tilesForBBox(double min_lon, double min_lat,
                               double max_lon, double max_lat) {
    std::vector<Tile> tiles;
    int lat0 = static_cast<int>(std::floor(min_lat));
    int lat1 = static_cast<int>(std::ceil(max_lat));
    int lon0 = static_cast<int>(std::floor(min_lon));
    int lon1 = static_cast<int>(std::ceil(max_lon));
    for (int lat = lat0; lat < lat1; ++lat) {
        for (int lon = lon0; lon < lon1; ++lon) {
            char buf[32];
            snprintf(buf, sizeof(buf), "n%02dw%03d", lat + 1, -lon);
            tiles.push_back({lat, lon, buf});
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
                 int dest_srid, int band_ft, double simplify_m, int band_threads,
                 bool verbose) {
    auto tiles = tilesForBBox(min_lon, min_lat, max_lon, max_lat);
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
            "  loaded_at timestamptz NOT NULL DEFAULT now())");
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
                              band_threads, verbose);
        return true;
    }

    const std::string tmp_dir = "/tmp/terrain_tiles";
    system(("mkdir -p " + tmp_dir).c_str());

    std::vector<std::string> downloaded_paths;
    std::vector<Tile> downloaded_tiles;
    for (auto& t : to_load) {
        std::string url = "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1/TIFF/current/"
                          + t.name + "/USGS_1_" + t.name + ".tif";
        std::string dest = tmp_dir + "/" + t.name + ".tif";
        if (verbose) { std::cout << "  downloading " << t.name << "... "; std::cout.flush(); }
        if (!downloadFile(url, dest)) {
            // Not fatal — a requested bbox may extend past 3DEP's coverage
            // (ocean, non-US territory), so a missing tile is expected, not
            // an error condition worth aborting the whole load over.
            if (verbose) std::cout << "not available, skipping\n";
            std::remove(dest.c_str());
            continue;
        }
        if (verbose) std::cout << "OK\n";
        downloaded_paths.push_back(dest);
        downloaded_tiles.push_back(t);
    }

    if (downloaded_paths.empty()) {
        std::cerr << "[Terrain] no tiles in this bounding box are available from 3DEP\n";
        return false;
    }

    // Deliberately no -C (standard constraints): reprojecting tiles spanning
    // a range of latitudes into Mercator gives each tile a slightly
    // different pixel scale, and 256x256 tiling gives edge blocks
    // non-uniform width/height. Fixed-value CHECK constraints derived from
    // one load batch then reject any later append whose tiles happen to
    // have different dimensions/scale — not needed for ST_Value point
    // queries, which only rely on the GIST index from -I.
    std::ostringstream cmd;
    cmd << "raster2pgsql -s 4269:" << dest_srid << " -t 256x256 -F";
    cmd << (table_exists ? " -a" : " -I -M");
    for (auto& p : downloaded_paths) cmd << " " << p;
    cmd << " terrain | psql -h " << server << " -U " << user << " -d " << database
        << " -q -v ON_ERROR_STOP=1";

    if (verbose)
        std::cout << "Loading " << downloaded_paths.size()
                  << " tile(s) into terrain table...\n";

    if (!password.empty()) setenv("PGPASSWORD", password.c_str(), 1);
    int rc = system(cmd.str().c_str());
    if (!password.empty()) unsetenv("PGPASSWORD");

    for (auto& p : downloaded_paths) std::remove(p.c_str());

    if (rc != 0) {
        std::cerr << "[Terrain] raster2pgsql/psql load failed\n";
        return false;
    }

    {
        pqxx::work txn(conn);
        for (auto& t : downloaded_tiles)
            txn.exec("INSERT INTO terrain_tiles(tile_name) VALUES ($1) "
                     "ON CONFLICT DO NOTHING", pqxx::params{t.name});
        txn.commit();
    }

    if (verbose)
        std::cout << "Terrain data loaded (" << downloaded_tiles.size()
                  << " new tile(s)).\n";

    if (band_ft > 0)
        buildTerrainBands(server, user, database, password, band_ft, simplify_m,
                          band_threads, verbose);

    return true;
}

bool buildTerrainBands(const std::string& server, const std::string& user,
                       const std::string& database, const std::string& password,
                       int band_ft, double simplify_m, int band_threads, bool verbose) {
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

    std::ostringstream frag_expr, geom_expr;
    if (simplify_m > 0) {
        // Simplify each small per-chunk fragment BEFORE the union, not just
        // the final unioned shape. Rugged terrain (e.g. foothills/canyons)
        // fragments into thousands of tiny raster-pixel-aligned polygons —
        // unioning them raw is fine (~seconds), but ST_SimplifyPreserveTopology
        // on the resulting huge multipolygon can pathologically hang (GEOS
        // noding blowup on that many collinear pixel-edge vertices), badly
        // enough that even Postgres's own statement_timeout can't cancel it
        // (stuck inside a single uninterruptible GEOS call). Pre-simplifying
        // each small fragment first (cheap — each is a handful of vertices)
        // cuts total vertex count entering the union by ~75% in testing,
        // which keeps the subsequent union+simplify fast instead of hanging.
        frag_expr << "ST_SimplifyPreserveTopology(d.geom, " << simplify_m << ")";
        geom_expr << "ST_SimplifyPreserveTopology(geom, " << simplify_m << ")";
    } else {
        frag_expr << "d.geom";
        geom_expr << "geom";
    }

    // One band per query rather than one all-bands statement: each band's
    // reclass isolates just its own elevation range (mapped to 1, everything
    // else to nodata), dumps to polygons, unions across every raster
    // row/tile so a feature spanning multiple tiles or 256x256 chunks comes
    // out seamless, simplifies (smooths the 30m pixel-edge staircase), then
    // ST_Dump splits the (possibly disjoint) result into indexable rows.
    //
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
        pqxx::connection wconn(connstr);
        while (true) {
            int i = next_band.fetch_add(1, std::memory_order_relaxed);
            if (i >= n_bands) break;

            int band_min = lo_ft + i * band_ft;
            int band_max = lo_ft + (i + 1) * band_ft;
            double band_lo_m = band_min / kFeetPerMeter;
            double band_hi_m = band_max / kFeetPerMeter;

            std::ostringstream reclass;
            reclass << std::fixed << std::setprecision(4)
                    << "(-999999-" << band_lo_m << "]:0, "
                    << "(" << band_lo_m << "-" << band_hi_m << "]:1, "
                    << "(" << band_hi_m << "-999999]:0";

            std::ostringstream sql;
            sql <<
                "INSERT INTO public.terrain_bands(band_min_ft, band_max_ft, geog) "
                "SELECT " << band_min << ", " << band_max << ", "
                          "(ST_Dump(" << geom_expr.str() << ")).geom "
                "FROM ("
                "  SELECT ST_Union(" << frag_expr.str() << ") AS geom "
                "  FROM public.terrain t, "
                "       LATERAL ST_DumpAsPolygons("
                "         ST_Reclass(t.rast, 1, '" << reclass.str() << "', '8BUI', 0), 1"
                "       ) d "
                "  WHERE d.val = 1"
                ") merged";

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
    };

    int nthreads = std::max(1, std::min(band_threads, n_bands));
    if (verbose)
        std::cout << "[Terrain] using " << nthreads << " thread(s)\n";
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) workers.emplace_back(worker);
    for (auto& w : workers) w.join();

    if (verbose)
        std::cout << "[Terrain] terrain_bands rebuilt: " << total_polygons.load()
                  << " polygon(s) across " << n_bands << " band(s)\n";
    return true;
}
