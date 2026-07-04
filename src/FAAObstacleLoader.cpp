#include "FAAObstacleLoader.h"
#include "GeoUtils.h"
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <iomanip>
#include <curl/curl.h>
#include <cstdlib>

// ---- download helper ----

static size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* f = static_cast<std::ofstream*>(stream);
    f->write(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

static bool downloadFile(const std::string& url, const std::string& dest) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::ofstream f(dest, std::ios::binary);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK && f.good();
}

// ---- CSV field parser ----
// FAA CSV may have quoted fields; split on commas respecting quotes.

static std::vector<std::string> splitCSV(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') { in_quotes = !in_quotes; }
        else if (c == ',' && !in_quotes) { fields.push_back(field); field.clear(); }
        else field += c;
    }
    fields.push_back(field);
    return fields;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The DOF CSV is not reliably UTF-8 (city/state names contain stray
// Latin-1/CP1252 bytes), which makes pqxx::stream_to reject the row
// outright. Pass through well-formed UTF-8 untouched; reinterpret any
// byte sequence that isn't valid UTF-8 as Latin-1 and re-encode it.
static std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int len = (c & 0x80) == 0x00 ? 1
                : (c & 0xE0) == 0xC0 ? 2
                : (c & 0xF0) == 0xE0 ? 3
                : (c & 0xF8) == 0xF0 ? 4
                : 0;
        bool valid = len > 0 && i + len <= s.size();
        for (int k = 1; valid && k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) valid = false;

        if (valid) {
            out.append(s, i, len);
            i += len;
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
            i += 1;
        }
    }
    return out;
}

// ---- WKB point builder ----

static std::string obstaclePointWKB(double lon, double lat) {
    double x = lon, y = lat;
    if (g_srid == 3857) {
        auto [mx, my] = toMercator(lon, lat);
        x = mx; y = my;
    }
    return pointWKB(x, y);
}

// ---- main load function ----

// DDOF CSV columns (from FAA documentation):
// OAS_NUMBER, VERIFIED, COUNTRY, STATE, CITY, LAT_DEG, LAT_MIN, LAT_SEC,
// LAT_HEMIS, LON_DEG, LON_MIN, LON_SEC, LON_HEMIS, LAT_DD, LON_DD,
// OBSTACLE_TYPE, QUANTITY, AGL_HT, AMSL_HT, LIGHTING, HOR_ACC, VER_ACC,
// MARK_INDICATOR, FAA_STUDY_NO, ACTION, JULIAN_DATE

bool loadFAAObstacles(const std::string& server, const std::string& user,
                      const std::string& database, const std::string& password,
                      bool verbose) {
    const std::string csv_url  = "https://aeronav.faa.gov/Obst_Data/DAILY_DOF_CSV.ZIP";
    const std::string zip_path = "/tmp/faa_ddof_csv.zip";
    const std::string csv_dir  = "/tmp/faa_ddof_csv";
    const std::string csv_path = csv_dir + "/DOF.CSV";

    if (verbose) std::cout << "Downloading FAA Daily Digital Obstacle File (CSV)...\n";

    if (!downloadFile(csv_url, zip_path)) {
        std::cerr << "[FAA DOF] download failed from " << csv_url << "\n";
        return false;
    }

    if (verbose) std::cout << "Extracting...\n";

    // The zip may contain DOF.CSV or a differently-named file — extract all
    std::string cmd = "mkdir -p " + csv_dir +
                      " && unzip -o " + zip_path + " -d " + csv_dir +
                      " 2>/dev/null";
    if (system(cmd.c_str()) != 0) {
        std::cerr << "[FAA DOF] unzip failed\n";
        return false;
    }

    // Find the CSV file (may not be named DOF.CSV exactly)
    std::string actual_csv;
    {
        FILE* p = popen(("find " + csv_dir + " -name '*.CSV' -o -name '*.csv' 2>/dev/null | head -1").c_str(), "r");
        if (p) {
            char buf[512];
            if (fgets(buf, sizeof(buf), p)) {
                actual_csv = trim(std::string(buf));
            }
            pclose(p);
        }
    }
    if (actual_csv.empty()) actual_csv = csv_path;

    std::ifstream f(actual_csv);
    if (!f.is_open()) {
        std::cerr << "[FAA DOF] cannot open CSV file at " << actual_csv << "\n";
        return false;
    }

    if (verbose) std::cout << "Loading FAA obstacle data from " << actual_csv << "...\n";

    std::string conn_str = "host=" + server + " dbname=" + database +
                           " user=" + user;
    if (!password.empty()) conn_str += " password=" + password;
    pqxx::connection conn(conn_str);

    // Create table if it doesn't exist, then truncate
    {
        pqxx::nontransaction txn(conn);
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS public.faa_obstacles (
                id              serial PRIMARY KEY,
                oas_number      varchar(9)  NOT NULL,
                verified        boolean     NOT NULL,
                country         varchar(2),
                state           varchar(2),
                city            varchar(16),
                latitude        double precision NOT NULL,
                longitude       double precision NOT NULL,
                obstacle_type   varchar(18),
                quantity        integer,
                agl_ht          integer,
                amsl_ht         integer,
                lighting        varchar(1),
                horiz_accuracy  varchar(1),
                vert_accuracy   varchar(1),
                marking         varchar(1),
                faa_study_no    varchar(14),
                action          varchar(1),
                julian_date     varchar(7),
                geog            public.geometry
            )
        )");
        txn.exec("CREATE INDEX IF NOT EXISTS faa_obstacles_geog_idx  ON public.faa_obstacles USING GIST (geog)");
        txn.exec("CREATE INDEX IF NOT EXISTS faa_obstacles_type_idx  ON public.faa_obstacles (obstacle_type)");
        txn.exec("CREATE INDEX IF NOT EXISTS faa_obstacles_state_idx ON public.faa_obstacles (state)");
        txn.exec("CREATE INDEX IF NOT EXISTS faa_obstacles_amsl_idx  ON public.faa_obstacles (amsl_ht)");
        txn.exec("CREATE INDEX IF NOT EXISTS faa_obstacles_agl_idx   ON public.faa_obstacles (agl_ht)");
    }
    {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE faa_obstacles");
        txn.commit();
    }

    pqxx::work txn(conn);
    auto stream = pqxx::stream_to::table(txn, {"faa_obstacles"}, {
        "oas_number", "verified", "country", "state", "city",
        "latitude", "longitude", "obstacle_type", "quantity",
        "agl_ht", "amsl_ht", "lighting", "horiz_accuracy", "vert_accuracy",
        "marking", "faa_study_no", "action", "julian_date", "geog"
    });

    // Actual CSV columns (0-based):
    // 0:OAS  1:VERIFIED STATUS  2:COUNTRY  3:STATE  4:CITY
    // 5:LATDEC  6:LONDEC  7:DMSLAT  8:DMSLON  9:TYPE  10:QUANTITY
    // 11:AGL  12:AMSL  13:LIGHTING  14:ACCURACY  15:MARKING
    // 16:FAA STUDY  17:ACTION  18:JDATE

    int count = 0, skipped = 0;
    std::string line;

    // Skip header row
    std::getline(f, line);

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '-') { ++skipped; continue; }

        auto v = splitCSV(sanitizeUtf8(line));
        if (v.size() < 19) {
            if (skipped < 3)
                std::cerr << "[FAA DOF] skip (cols=" << v.size()
                          << "): [" << line.substr(0, 80) << "]\n";
            ++skipped; continue;
        }

        std::string lat_s = trim(v[5]);
        std::string lon_s = trim(v[6]);
        if (lat_s.empty() || lon_s.empty()) {
            if (skipped < 3)
                std::cerr << "[FAA DOF] skip (empty lat/lon): lat=["
                          << lat_s << "] lon=[" << lon_s << "]\n";
            ++skipped; continue;
        }

        double lat, lon;
        try { lat = std::stod(lat_s); lon = std::stod(lon_s); }
        catch (...) {
            if (skipped < 3)
                std::cerr << "[FAA DOF] skip (stod failed): lat=["
                          << lat_s << "] lon=[" << lon_s << "]\n";
            ++skipped; continue;
        }

        std::string geog = obstaclePointWKB(lon, lat);

        int qty = 0, agl = 0, amsl = 0;
        try { qty  = std::stoi(trim(v[10])); } catch (...) {}
        try { agl  = std::stoi(trim(v[11])); } catch (...) {}
        try { amsl = std::stoi(trim(v[12])); } catch (...) {}

        // ACCURACY is a combined 2-char code: [0]=horizontal accuracy
        // category (digit), [1]=vertical accuracy category (letter),
        // e.g. "5D". Split rather than dumping both chars into
        // horiz_accuracy, which overflows the varchar(1) column.
        std::string acc = trim(v[14]);
        std::string horiz_acc = acc.size() >= 1 ? acc.substr(0, 1) : "";
        std::string vert_acc  = acc.size() >= 2 ? acc.substr(1, 1) : "";

        stream.write_values(
            trim(v[0]),           // oas_number
            trim(v[1]) == "O",    // verified
            trim(v[2]),           // country
            trim(v[3]),           // state
            trim(v[4]),           // city
            lat, lon,
            trim(v[9]),           // obstacle_type
            qty, agl, amsl,
            trim(v[13]),          // lighting
            horiz_acc,             // horiz_accuracy
            vert_acc,              // vert_accuracy
            trim(v[15]),          // marking
            trim(v[16]),          // faa_study_no
            trim(v[17]),          // action
            trim(v[18]),          // julian_date
            geog
        );
        ++count;
    }
    stream.complete();
    txn.commit();

    if (verbose)
        std::cout << "  obstacles: " << count
                  << " loaded, " << skipped << " skipped\n";

    // Cleanup temp files
    system(("rm -rf " + zip_path + " " + csv_dir).c_str());
    return true;
}
