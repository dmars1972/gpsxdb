#include "AirportsLoader.h"
#include <pqxx/pqxx>
#include <proj.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cmath>

// ---- Mercator projection (thread-local PROJ context, RAII cleanup) ----
// Mirrors OSMReader.cpp's ProjContext pattern. The earlier version of this
// function used bare thread_local PJ_CONTEXT*/PJ* pointers that were never
// freed via proj_destroy()/proj_context_destroy() — PROJ's internal static
// teardown at process exit could then conflict with the leaked thread-local
// state, producing "double free or corruption" on exit. RAII fixes this.

struct ProjContextAP {
    PJ_CONTEXT* ctx;
    PJ* pj;
    ProjContextAP() {
        ctx = proj_context_create();
        pj  = proj_create_crs_to_crs(ctx, "EPSG:4326", "EPSG:3857", nullptr);
        if (!pj) throw std::runtime_error("PROJ: failed to create CRS transform");
        pj = proj_normalize_for_visualization(ctx, pj);
    }
    ~ProjContextAP() {
        proj_destroy(pj);
        proj_context_destroy(ctx);
    }
};

static thread_local ProjContextAP tl_proj_ap;

static std::pair<double,double> toMercatorAP(double lon, double lat) {
    PJ_COORD in  = proj_coord(lon, lat, 0, 0);
    PJ_COORD out = proj_trans(tl_proj_ap.pj, PJ_FWD, in);
    return {out.xy.x, out.xy.y};
}

// OurAirports is a crowd-sourced, free dataset -- occasional bad rows are
// expected (found live: a runway end with le_longitude_deg=3824610.0, a
// clear data-entry error upstream). toMercatorAP()/PROJ doesn't validate
// its input and doesn't report failure through its return value either
// (a bad coordinate just gets logged by PROJ itself, e.g. "webmerc:
// Invalid longitude", while still returning -- typically inf/nan -- as if
// nothing were wrong), so garbage in means garbage silently written to
// the geog column unless the input is checked before ever reaching it.
static bool validLonLatAP(double lon, double lat) {
    return std::isfinite(lon) && std::isfinite(lat) &&
           lon >= -180.0 && lon <= 180.0 && lat >= -90.0 && lat <= 90.0;
}

// ---- WKB point ----

// Embeds SRID 3857 (toMercatorAP always reprojects to EPSG:3857, regardless
// of any global -L/--wgs84 flag) via the SRID-flagged WKB point type
// (0x20000001), matching GeoUtils.cpp's shared pointWKB(). Previously this
// omitted the SRID flag entirely (plain 0x00000001 point), leaving every
// airports/navaids/runways geog column stored with SRID 0 despite holding
// real Mercator-meter coordinates -- harmless until something ran an
// SRID-aware operation (ST_Transform, mixing with a properly-tagged
// geometry) against them, which is how this was caught.
static std::string pointWKBAP(double lon_m, double lat_m) {
    std::vector<uint8_t> b;
    b.push_back(1);                                            // little-endian byte order
    b.push_back(0x01); b.push_back(0x00); b.push_back(0x00); b.push_back(0x20); // POINT with SRID flag
    uint32_t srid = 3857;
    uint8_t sb[4]; memcpy(sb, &srid, 4); for (auto c : sb) b.push_back(c);
    uint8_t buf[8];
    memcpy(buf, &lon_m, 8); for (auto c : buf) b.push_back(c);
    memcpy(buf, &lat_m, 8); for (auto c : buf) b.push_back(c);
    std::ostringstream ss;
    for (auto c : b)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    return ss.str();
}

// ---- CSV parser ----

static std::vector<std::string> parseCsvLineAP(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i+1 < line.size() && line[i+1] == '"') { field += '"'; ++i; }
                else in_quotes = false;
            } else { field += c; }
        } else {
            if      (c == '"') in_quotes = true;
            else if (c == ',') { fields.push_back(field); field.clear(); }
            else               field += c;
        }
    }
    fields.push_back(field);
    return fields;
}

// ---- Helpers ----

static std::optional<int>    optInt   (const std::string& s) {
    if (s.empty()) return std::nullopt;
    try { return std::stoi(s); } catch (...) { return std::nullopt; }
}
static std::optional<double> optDouble(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try { return std::stod(s); } catch (...) { return std::nullopt; }
}
static std::optional<std::string> optStr(const std::string& s) {
    return s.empty() ? std::nullopt : std::make_optional(s);
}
static bool parseBool(const std::string& s) {
    return s == "1" || s == "yes" || s == "true";
}

// curl's own --retry only covers one invocation's worth of transient
// network hiccups; this adds a second layer on top (a few attempts with a
// short sleep between them, mirroring AirspaceLoader's downloadFileRetry)
// so a blip that outlasts curl's own retries doesn't silently kill the
// entire airports load for an otherwise-successful multi-hour import (this
// happened for real: one of these six downloads failed after curl's own
// retries, and the failure surfaced no detail about which file or why).
static bool downloadAP(const std::string& url, const std::string& dest) {
    constexpr int kAttempts = 3;
    for (int i = 0; i < kAttempts; ++i) {
        std::string cmd = "curl -sf --retry 3 -o \"" + dest + "\" \"" + url + "\"";
        int rc = system(cmd.c_str());
        if (rc == 0) return true;
        std::cerr << "[AirportsLoader] download attempt " << (i + 1) << "/" << kAttempts
                  << " failed (curl exit status " << rc << ") for " << url << "\n";
        if (i + 1 < kAttempts) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return false;
}

// ---- Table loaders ----

static void loadCountries(pqxx::connection& conn, const std::string& path, bool verbose) {
    std::ifstream f(path); std::string line; std::getline(f, line);
    pqxx::work txn(conn);
    auto s = pqxx::stream_to::table(txn, {"countries"},
                 {"id","code","name","continent"});
    int n = 0;
    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 4) continue;
        s.write_values(std::stoi(v[0]), optStr(v[1]), optStr(v[2]), optStr(v[3]));
        ++n;
    }
    s.complete(); txn.commit();
    if (verbose) std::cout << "  countries: " << n << "\n";
}

static void loadRegions(pqxx::connection& conn, const std::string& path, bool verbose) {
    std::ifstream f(path); std::string line; std::getline(f, line);
    pqxx::work txn(conn);
    auto s = pqxx::stream_to::table(txn, {"regions"},
                 {"id","code","local_code","name","continent","iso_country"});
    int n = 0;
    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 6) continue;
        s.write_values(std::stoi(v[0]),
            optStr(v[1]), optStr(v[2]), optStr(v[3]),
            optStr(v[4]), optStr(v[5]));
        ++n;
    }
    s.complete(); txn.commit();
    if (verbose) std::cout << "  regions: " << n << "\n";
}

static const std::vector<std::pair<int,std::string>> AIRPORT_TAG_COLS = {
    {16, "home_link"},
    {17, "wikipedia_link"},
    {18, "keywords"},
};

static void loadAirports(pqxx::connection& conn, const std::string& path, bool verbose) {
    // Two passes: first load airports, then tags (pqxx allows only one active stream)
    struct AirportRow {
        int id; std::optional<std::string> ident, type, name, continent,
            iso_country, iso_region, municipality, icao, iata, gps, local, geog;
        std::optional<double> lat_m, lon_m;
        std::optional<int> elev;
        bool scheduled;
    };
    struct TagRow { std::string ident, entity_type, key, val; };

    std::vector<AirportRow> airports;
    std::vector<TagRow> tags;

    std::ifstream f(path); std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 19) continue;
        auto lat = optDouble(v[4]), lon = optDouble(v[5]);
        std::optional<double> lat_m, lon_m;
        std::optional<std::string> geog;
        if (lat && lon && validLonLatAP(*lon, *lat)) {
            auto [mx,my] = toMercatorAP(*lon, *lat);
            lon_m = mx; lat_m = my; geog = pointWKBAP(mx, my);
        }
        int id = std::stoi(v[0]);
        std::string ident = v[1];
        airports.push_back({id,
            optStr(v[1]), optStr(v[2]), optStr(v[3]),
            optStr(v[7]), optStr(v[8]), optStr(v[9]), optStr(v[10]),
            optStr(v[12]), optStr(v[13]), optStr(v[14]), optStr(v[15]),
            geog, lat_m, lon_m, optInt(v[6]), parseBool(v[11])});
        for (auto& [col, key] : AIRPORT_TAG_COLS)
            if (col < static_cast<int>(v.size()) && !v[col].empty())
                tags.push_back({ident, "airport", key, v[col]});
    }

    {
        pqxx::work txn(conn);
        auto s = pqxx::stream_to::table(txn, {"airports"},
            {"id","ident","type","name",
             "latitude_m","longitude_m","elevation_ft",
             "continent","iso_country","iso_region","municipality",
             "scheduled_service","icao_code","iata_code","gps_code","local_code",
             "geog"});
        for (auto& r : airports)
            s.write_values(r.id, r.ident, r.type, r.name,
                r.lat_m, r.lon_m, r.elev,
                r.continent, r.iso_country, r.iso_region, r.municipality,
                r.scheduled, r.icao, r.iata, r.gps, r.local, r.geog);
        s.complete(); txn.commit();
    }
    {
        pqxx::work txn(conn);
        auto ts = pqxx::stream_to::table(txn, {"tags"},
            {"airport_ident","entity_type","key_name","key_value"});
        for (auto& t : tags)
            ts.write_values(t.ident, t.entity_type, t.key, t.val);
        ts.complete(); txn.commit();
    }
    if (verbose) std::cout << "  airports: " << airports.size() << "\n";
}

static void loadFrequencies(pqxx::connection& conn, const std::string& path, bool verbose) {
    std::ifstream f(path); std::string line; std::getline(f, line);
    pqxx::work txn(conn);
    auto s = pqxx::stream_to::table(txn, {"frequencies"},
        {"id","airport_ref","airport_ident","type","description","frequency_mhz"});
    int n = 0;
    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 6) continue;
        s.write_values(std::stoi(v[0]),
            optInt(v[1]), optStr(v[2]), optStr(v[3]),
            optStr(v[4]), optDouble(v[5]));
        ++n;
    }
    s.complete(); txn.commit();
    if (verbose) std::cout << "  frequencies: " << n << "\n";
}

static void loadRunways(pqxx::connection& conn, const std::string& path, bool verbose) {
    std::ifstream f(path); std::string line; std::getline(f, line);
    pqxx::work txn(conn);
    auto s = pqxx::stream_to::table(txn, {"runways"},
        {"id","airport_ref","airport_ident","length_ft","width_ft","surface",
         "lighted","closed",
         "le_ident","le_latitude_m","le_longitude_m","le_elevation_ft",
         "le_heading_degt","le_displaced_threshold_ft","le_geog",
         "he_ident","he_latitude_m","he_longitude_m","he_elevation_ft",
         "he_heading_degt","he_displaced_threshold_ft","he_geog"});
    int n = 0;
    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 20) continue;
        auto le_lat = optDouble(v[9]),  le_lon = optDouble(v[10]);
        auto he_lat = optDouble(v[15]), he_lon = optDouble(v[16]);
        std::optional<double> le_lat_m, le_lon_m, he_lat_m, he_lon_m;
        std::optional<std::string> le_geog, he_geog;
        if (le_lat && le_lon && validLonLatAP(*le_lon, *le_lat)) {
            auto [mx,my] = toMercatorAP(*le_lon, *le_lat);
            le_lon_m = mx; le_lat_m = my; le_geog = pointWKBAP(mx, my);
        }
        if (he_lat && he_lon && validLonLatAP(*he_lon, *he_lat)) {
            auto [mx,my] = toMercatorAP(*he_lon, *he_lat);
            he_lon_m = mx; he_lat_m = my; he_geog = pointWKBAP(mx, my);
        }
        s.write_values(std::stoi(v[0]),
            optInt(v[1]), optStr(v[2]),
            optInt(v[3]), optInt(v[4]), optStr(v[5]),
            parseBool(v[6]), parseBool(v[7]),
            optStr(v[8]),  le_lat_m, le_lon_m, optInt(v[11]),
            optDouble(v[12]), optInt(v[13]), le_geog,
            optStr(v[14]), he_lat_m, he_lon_m, optInt(v[17]),
            optDouble(v[18]), optInt(v[19]), he_geog);
        ++n;
    }
    s.complete(); txn.commit();
    if (verbose) std::cout << "  runways: " << n << "\n";
}

static const std::vector<std::pair<int,std::string>> NAVAID_TAG_COLS = {
    {1,  "filename"},
    {15, "slaved_variation_deg"},
    {16, "magnetic_variation_deg"},
};

static void loadNavaids(pqxx::connection& conn, const std::string& path, bool verbose) {
    std::ifstream f(path); std::string line; std::getline(f, line);
    struct NavaidRow {
        int id;
        std::optional<std::string> ident, name, type, iso_country,
            dme_channel, usage_type, power, assoc_airport, geog;
        std::optional<double> freq, lat_m, lon_m, dme_freq,
            dme_lat_m, dme_lon_m, slaved_var, mag_var;
        std::optional<int> elev, dme_elev;
    };
    struct TagRow { std::string ident, entity_type, key, val; };

    std::vector<NavaidRow> navaids;
    std::vector<TagRow> tags;

    while (std::getline(f, line)) {
        auto v = parseCsvLineAP(line);
        if (v.size() < 20) continue;
        auto lat = optDouble(v[6]), lon = optDouble(v[7]);
        auto dme_lat = optDouble(v[12]), dme_lon = optDouble(v[13]);
        std::optional<double> lat_m, lon_m, dme_lat_m, dme_lon_m;
        std::optional<std::string> geog;
        if (lat && lon && validLonLatAP(*lon, *lat)) {
            auto [mx,my] = toMercatorAP(*lon, *lat);
            lon_m = mx; lat_m = my; geog = pointWKBAP(mx, my);
        }
        if (dme_lat && dme_lon && validLonLatAP(*dme_lon, *dme_lat)) {
            auto [mx,my] = toMercatorAP(*dme_lon, *dme_lat);
            dme_lon_m = mx; dme_lat_m = my;
        }
        int id = std::stoi(v[0]);
        navaids.push_back({id,
            optStr(v[2]), optStr(v[3]), optStr(v[4]), optStr(v[9]),
            optStr(v[11]), optStr(v[17]), optStr(v[18]), optStr(v[19]), geog,
            optDouble(v[5]), lat_m, lon_m,
            optDouble(v[10]), dme_lat_m, dme_lon_m,
            optDouble(v[15]), optDouble(v[16]),
            optInt(v[8]), optInt(v[14])});
        std::string nav_ident = v[19].empty() ? "" : v[19]; // associated_airport
        for (auto& [col, key] : NAVAID_TAG_COLS)
            if (col < static_cast<int>(v.size()) && !v[col].empty())
                tags.push_back({nav_ident, "navaid", key, v[col]});
    }

    {
        pqxx::work txn(conn);
        auto s = pqxx::stream_to::table(txn, {"navaids"},
            {"id","ident","name","type","frequency_khz",
             "latitude_m","longitude_m","elevation_ft","iso_country",
             "dme_frequency_khz","dme_channel",
             "dme_latitude_m","dme_longitude_m","dme_elevation_ft",
             "slaved_variation_deg","magnetic_variation_deg",
             "usage_type","power","associated_airport","geog"});
        for (auto& r : navaids)
            s.write_values(r.id, r.ident, r.name, r.type, r.freq,
                r.lat_m, r.lon_m, r.elev, r.iso_country,
                r.dme_freq, r.dme_channel,
                r.dme_lat_m, r.dme_lon_m, r.dme_elev,
                r.slaved_var, r.mag_var,
                r.usage_type, r.power, r.assoc_airport, r.geog);
        s.complete(); txn.commit();
    }
    {
        pqxx::work txn(conn);
        auto ts = pqxx::stream_to::table(txn, {"tags"},
            {"airport_ident","entity_type","key_name","key_value"});
        for (auto& t : tags)
            ts.write_values(t.ident, t.entity_type, t.key, t.val);
        ts.complete(); txn.commit();
    }
    if (verbose) std::cout << "  navaids: " << navaids.size() << "\n";
}

// ---- Public entry point ----

bool AirportsLoader::load(bool verbose) {
    const std::string base = "https://davidmegginson.github.io/ourairports-data/";
    // Keyed by PID, not a fixed shared path: this loader runs as different
    // OS users at different times (a manual daniel-run bulk import vs. the
    // gpsxdb-poll service's own periodic reload) but always against the
    // same "nav" database, so there's no natural database-scoped
    // disambiguator the way Replicator.cpp's /tmp/osm_state_<database>.txt
    // fix had. Whichever user's process ran first left these files behind
    // owned by them (nothing here ever cleaned them up either) at mode 644,
    // silently blocking every other user's curl from ever overwriting them
    // again -- confirmed live: a gpsxdb-owned /tmp/ourairports_countries.csv
    // from 2 days earlier made every subsequent daniel-run import fail this
    // download with CURLE_WRITE_ERROR (23), retries included, since retrying
    // the exact same unwritable path can't help.
    const std::string tmp = "/tmp/ourairports_" + std::to_string(getpid()) + "_";

    struct FileSpec { std::string name, url, dest; };
    std::vector<FileSpec> files = {
        {"countries",   base + "countries.csv",           tmp + "countries.csv"},
        {"regions",     base + "regions.csv",             tmp + "regions.csv"},
        {"airports",    base + "airports.csv",            tmp + "airports.csv"},
        {"frequencies", base + "airport-frequencies.csv", tmp + "frequencies.csv"},
        {"runways",     base + "runways.csv",             tmp + "runways.csv"},
        {"navaids",     base + "navaids.csv",             tmp + "navaids.csv"},
    };

    auto cleanupTempFiles = [&files]() {
        for (auto& f : files) std::remove(f.dest.c_str());
    };

    if (verbose) std::cout << "Downloading OurAirports data...\n";
    for (auto& f : files) {
        if (verbose) { std::cout << "  " << f.name << "... "; std::cout.flush(); }
        if (!downloadAP(f.url, f.dest)) {
            std::cerr << "[AirportsLoader] FAILED downloading " << f.name << " from " << f.url
                      << " — skipping airports load (existing table contents, if any, left untouched)\n";
            cleanupTempFiles();
            return false;
        }
        if (verbose) std::cout << "OK\n";
    }

    { pqxx::work txn(conn_); txn.exec("SET synchronous_commit = off"); txn.commit(); }

    // Truncate before reload so this is safe to call more than once against
    // a live database (e.g. periodic upstream-update checks in poll mode),
    // not just once against freshly-created empty tables.
    {
        pqxx::work txn(conn_);
        txn.exec("TRUNCATE countries, regions, airports, tags, frequencies, runways, navaids");
        txn.commit();
    }

    if (verbose) std::cout << "Loading airports data...\n";
    loadCountries  (conn_, files[0].dest, verbose); progress_cb_(1, 6);
    loadRegions    (conn_, files[1].dest, verbose); progress_cb_(2, 6);
    loadAirports   (conn_, files[2].dest, verbose); progress_cb_(3, 6);
    loadFrequencies(conn_, files[3].dest, verbose); progress_cb_(4, 6);
    loadRunways    (conn_, files[4].dest, verbose); progress_cb_(5, 6);
    loadNavaids    (conn_, files[5].dest, verbose); progress_cb_(6, 6);

    // p_srid defaults to 4326 but accepts 3857 (the table's native SRID)
    // directly too -- ST_Transform is a no-op when source and target SRID
    // already match. geog is passed to the KNN operator (<->) bare, not
    // wrapped in ST_SetSRID(geog,...): that wrapping is a no-op value-wise
    // (geog is already SRID 3857) but produces a different expression than
    // what airports_geog_idx was built on, defeating the index and forcing
    // a full seq scan + sort -- found the hard way via dq_check.py running
    // ~150ms/call instead of a fraction of a ms.
    // Non-essential convenience function, not core import data -- must
    // never be allowed to take down the whole import on a bug (see the
    // identical guard in AirspaceLoader::createQueryFunctions(), added
    // after exactly that happened to airspace_at_point() 7+ hours into a
    // run, via an uncaught pqxx exception -> std::terminate()).
    try {
        pqxx::work txn(conn_);
        txn.exec(
            "CREATE OR REPLACE FUNCTION public.nearest_airport("
            "  p_x double precision, p_y double precision, p_srid integer DEFAULT 4326"
            ") RETURNS TABLE(ident varchar, name varchar, type varchar, "
            "                elevation_ft integer, dist_km double precision) "
            "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
            "  SELECT a.ident, a.name, a.type, a.elevation_ft, "
            "         ST_Distance(a.geog, pt.g) / 1000.0 "
            "  FROM public.airports a, "
            "       (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 3857) AS g) pt "
            "  ORDER BY a.geog <-> pt.g "
            "  LIMIT 1 "
            "$f$");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[AirportsLoader] nearest_airport() function creation failed (non-fatal): "
                  << e.what() << "\n";
    }

    cleanupTempFiles();
    if (verbose) std::cout << "Airports data loaded.\n";
    return true;
}
