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

// ---- WKB point ----

static std::string pointWKBAP(double lon_m, double lat_m) {
    std::vector<uint8_t> b;
    b.push_back(1);
    b.push_back(1); b.push_back(0); b.push_back(0); b.push_back(0);
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

static bool downloadAP(const std::string& url, const std::string& dest) {
    std::string cmd = "curl -sf --retry 3 -o \"" + dest + "\" \"" + url + "\"";
    return system(cmd.c_str()) == 0;
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
        if (lat && lon) {
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
        if (le_lat && le_lon) {
            auto [mx,my] = toMercatorAP(*le_lon, *le_lat);
            le_lon_m = mx; le_lat_m = my; le_geog = pointWKBAP(mx, my);
        }
        if (he_lat && he_lon) {
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
        if (lat && lon) {
            auto [mx,my] = toMercatorAP(*lon, *lat);
            lon_m = mx; lat_m = my; geog = pointWKBAP(mx, my);
        }
        if (dme_lat && dme_lon) {
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

bool loadAirportsData(const std::string& server, const std::string& user,
                      const std::string& database, const std::string& password,
                      bool verbose) {
    const std::string base = "https://davidmegginson.github.io/ourairports-data/";
    const std::string tmp  = "/tmp/ourairports_";

    struct FileSpec { std::string name, url, dest; };
    std::vector<FileSpec> files = {
        {"countries",   base + "countries.csv",           tmp + "countries.csv"},
        {"regions",     base + "regions.csv",             tmp + "regions.csv"},
        {"airports",    base + "airports.csv",            tmp + "airports.csv"},
        {"frequencies", base + "airport-frequencies.csv", tmp + "frequencies.csv"},
        {"runways",     base + "runways.csv",             tmp + "runways.csv"},
        {"navaids",     base + "navaids.csv",             tmp + "navaids.csv"},
    };

    if (verbose) std::cout << "Downloading OurAirports data...\n";
    for (auto& f : files) {
        if (verbose) { std::cout << "  " << f.name << "... "; std::cout.flush(); }
        if (!downloadAP(f.url, f.dest)) {
            std::cerr << "FAILED — skipping airports load\n"; return false;
        }
        if (verbose) std::cout << "OK\n";
    }

    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;

    pqxx::connection conn(connstr);
    { pqxx::work txn(conn); txn.exec("SET synchronous_commit = off"); txn.commit(); }

    // Truncate before reload so this is safe to call more than once against
    // a live database (e.g. periodic upstream-update checks in poll mode),
    // not just once against freshly-created empty tables.
    {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE countries, regions, airports, tags, frequencies, runways, navaids");
        txn.commit();
    }

    if (verbose) std::cout << "Loading airports data...\n";
    loadCountries  (conn, files[0].dest, verbose);
    loadRegions    (conn, files[1].dest, verbose);
    loadAirports   (conn, files[2].dest, verbose);
    loadFrequencies(conn, files[3].dest, verbose);
    loadRunways    (conn, files[4].dest, verbose);
    loadNavaids    (conn, files[5].dest, verbose);
    if (verbose) std::cout << "Airports data loaded.\n";
    return true;
}
