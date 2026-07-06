#include "AirspaceLoader.h"
#include "GeoUtils.h"
#include <boost/json/src.hpp>
#include <boost/json.hpp>
#include <pqxx/pqxx>
#include <iostream>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include <cstdio>
#include <optional>
#include <thread>
#include <chrono>

namespace json = boost::json;

namespace {

size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* f = static_cast<std::ofstream*>(stream);
    f->write(static_cast<const char*>(ptr), static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

size_t curlWriteString(void* ptr, size_t size, size_t nmemb, void* out) {
    std::string* s = static_cast<std::string*>(out);
    s->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// For the OpenAIP paginated JSON API — responses are small enough (a few
// hundred KB per page) to fetch straight into memory rather than a temp
// file, unlike the ~600MB FAA Class Airspace download above.
bool fetchJson(const std::string& url, std::string& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

// timeout_s is much longer for Class Airspace (611MB) than Special Use
// Airspace (29MB) — both come from the same slow-ish ArcGIS download API.
//
// The ArcGIS download endpoint has been observed to occasionally return a
// small HTTP 400 JSON error body ({"message":"Downloads are not supported
// for this item.",...}) instead of the real file, for no apparent reason —
// a retry of the exact same URL immediately afterward succeeds. FAILONERROR
// makes curl itself treat that as a failure (rather than writing the error
// body to `dest` and reporting success), so callers can retry/bail cleanly
// instead of trying to parse an error page as GeoJSON.
bool downloadFile(const std::string& url, const std::string& dest, long timeout_s) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::ofstream f(dest, std::ios::binary);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK && f.good();
}

// Retries the transient-400 case described above a few times before giving
// up for real.
bool downloadFileRetry(const std::string& url, const std::string& dest, long timeout_s) {
    constexpr int kAttempts = 3;
    for (int i = 0; i < kAttempts; ++i) {
        if (downloadFile(url, dest, timeout_s)) return true;
        if (i + 1 < kAttempts) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return false;
}

// Returns the parsed "features" array, or nullptr (with a diagnostic
// message, rather than an uncaught exception) if the response wasn't the
// FeatureCollection we expected — e.g. an API error body that still
// happens to be valid JSON.
const json::array* featuresOf(const json::value& doc, const char* label) {
    if (!doc.is_object()) {
        std::cerr << "[" << label << "] response is not a JSON object\n";
        return nullptr;
    }
    const json::object& o = doc.as_object();
    auto it = o.find("features");
    if (it == o.end() || !it->value().is_array()) {
        std::cerr << "[" << label << "] response has no 'features' array (got: "
                  << json::serialize(doc).substr(0, 200) << ")\n";
        return nullptr;
    }
    return &it->value().as_array();
}

// FAA's ArcGIS export encodes the same logical field inconsistently across
// (and even within) datasets — e.g. Class Airspace's UPPER_VAL is a JSON
// number while Special Use Airspace's is a JSON string — so every accessor
// here tolerates whichever representation shows up rather than assuming one.
bool hasField(const json::object& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && !it->value().is_null();
}

std::string jstr(const json::object& o, const char* k) {
    if (!hasField(o, k)) return "";
    const json::value& v = o.at(k);
    if (v.is_string()) return std::string(v.as_string());
    if (v.is_int64()) return std::to_string(v.as_int64());
    if (v.is_uint64()) return std::to_string(v.as_uint64());
    if (v.is_double()) return std::to_string(v.as_double());
    if (v.is_bool()) return v.as_bool() ? "1" : "0";
    return "";
}

// Returns nullopt (rather than throwing) if the field is absent, null, or
// not parseable as a number — callers pass this straight to
// pqxx::stream_to::write_values, which maps nullopt to SQL NULL.
std::optional<double> jdouble(const json::object& o, const char* k) {
    if (!hasField(o, k)) return std::nullopt;
    const json::value& v = o.at(k);
    try {
        if (v.is_double()) return v.as_double();
        if (v.is_int64())  return static_cast<double>(v.as_int64());
        if (v.is_uint64()) return static_cast<double>(v.as_uint64());
        if (v.is_string()) {
            const std::string s = std::string(v.as_string());
            if (s.empty()) return std::nullopt;
            return std::stod(s);
        }
    } catch (...) {}
    return std::nullopt;
}

double lonOf(const json::value& pt) {
    const json::array& a = pt.as_array();
    return a[0].is_double() ? a[0].as_double() : static_cast<double>(a[0].as_int64());
}
double latOf(const json::value& pt) {
    const json::array& a = pt.as_array();
    return a[1].is_double() ? a[1].as_double() : static_cast<double>(a[1].as_int64());
}

// Projects a GeoJSON ring (array of [lon,lat,(alt)] points, alt ignored)
// into the target SRID, matching g_srid the same way pointWKB does.
std::vector<std::pair<double,double>> projectRing(const json::array& ring) {
    std::vector<std::pair<double,double>> pts;
    pts.reserve(ring.size());
    for (const auto& pt : ring) {
        double lon = lonOf(pt), lat = latOf(pt);
        if (g_srid == 3857) {
            auto [mx, my] = toMercator(lon, lat);
            pts.push_back({mx, my});
        } else {
            pts.push_back({lon, lat});
        }
    }
    return pts;
}

// Normalizes a GeoJSON "Polygon" or "MultiPolygon" geometry into the
// polygons[][]/rings[][]/points[] nesting multiPolygonWKB expects — a bare
// Polygon becomes a 1-element MultiPolygon, so every feature can be stored
// in the same MultiPolygon column regardless of its source geometry type.
std::vector<std::vector<std::vector<std::pair<double,double>>>>
polygonsFromGeometry(const json::object& geom) {
    std::vector<std::vector<std::vector<std::pair<double,double>>>> polygons;
    if (!geom.contains("type") || !geom.contains("coordinates")) return polygons;
    std::string type = std::string(geom.at("type").as_string());
    const json::array& coords = geom.at("coordinates").as_array();

    if (type == "Polygon") {
        std::vector<std::vector<std::pair<double,double>>> rings;
        for (const auto& ring : coords) rings.push_back(projectRing(ring.as_array()));
        polygons.push_back(std::move(rings));
    } else if (type == "MultiPolygon") {
        for (const auto& poly : coords) {
            std::vector<std::vector<std::pair<double,double>>> rings;
            for (const auto& ring : poly.as_array()) rings.push_back(projectRing(ring.as_array()));
            polygons.push_back(std::move(rings));
        }
    }
    return polygons;
}

std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- OpenAIP-specific helpers ----

std::optional<int64_t> jint(const json::object& o, const char* k) {
    if (!hasField(o, k)) return std::nullopt;
    const json::value& v = o.at(k);
    if (v.is_int64()) return v.as_int64();
    if (v.is_uint64()) return static_cast<int64_t>(v.as_uint64());
    if (v.is_double()) return static_cast<int64_t>(v.as_double());
    return std::nullopt;
}

std::optional<bool> jbool(const json::object& o, const char* k) {
    if (!hasField(o, k)) return std::nullopt;
    const json::value& v = o.at(k);
    if (v.is_bool()) return v.as_bool();
    return std::nullopt;
}

// OpenAIP's upperLimit/lowerLimit are nested {value, unit, referenceDatum}
// objects rather than flat fields like the FAA datasets — pulls the three
// out, or all-nullopt if the field is absent.
struct AltLimit { std::optional<double> value; std::optional<int64_t> unit, ref; };
AltLimit jaltLimit(const json::object& o, const char* k) {
    AltLimit r;
    if (!hasField(o, k) || !o.at(k).is_object()) return r;
    const json::object& lim = o.at(k).as_object();
    r.value = jdouble(lim, "value");
    r.unit = jint(lim, "unit");
    r.ref = jint(lim, "referenceDatum");
    return r;
}

} // namespace

bool loadClassAirspace(const std::string& server, const std::string& user,
                       const std::string& database, const std::string& password,
                       bool verbose) {
    const std::string url  = "https://adds-faa.opendata.arcgis.com/api/download/v1/"
                              "items/c6a62360338e408cb1512366ad61559e/geojson?layers=0";
    const std::string path = "/tmp/class_airspace.geojson";

    if (verbose) std::cout << "Downloading FAA Class Airspace data (~600MB)...\n";
    // Much larger/slower download than the other FAA sources this project
    // already loads — 20 minutes, not the usual 5, before giving up.
    if (!downloadFileRetry(url, path, 1200)) {
        std::cerr << "[Class Airspace] download failed from " << url << "\n";
        return false;
    }

    if (verbose) std::cout << "Parsing...\n";
    json::value doc;
    try {
        doc = json::parse(readWholeFile(path));
    } catch (const std::exception& e) {
        std::cerr << "[Class Airspace] JSON parse error: " << e.what() << "\n";
        std::remove(path.c_str());
        return false;
    }

    std::string conn_str = "host=" + server + " dbname=" + database + " user=" + user;
    if (!password.empty()) conn_str += " password=" + password;
    pqxx::connection conn(conn_str);

    {
        pqxx::nontransaction txn(conn);
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS public.class_airspace (
                id          serial PRIMARY KEY,
                ident       varchar(8),
                icao_id     varchar(8),
                name        text,
                class       varchar(8),
                type_code   varchar(16),
                local_type  varchar(32),
                lower_val   double precision,
                lower_uom   varchar(4),
                lower_code  varchar(8),
                upper_val   double precision,
                upper_uom   varchar(4),
                upper_code  varchar(8),
                city        text,
                state       text,
                country     text,
                geog        public.geometry
            )
        )");
        txn.exec("CREATE INDEX IF NOT EXISTS class_airspace_geog_idx  ON public.class_airspace USING GIST (geog)");
        txn.exec("CREATE INDEX IF NOT EXISTS class_airspace_class_idx ON public.class_airspace (class)");
        txn.exec("CREATE INDEX IF NOT EXISTS class_airspace_state_idx ON public.class_airspace (state)");
    }
    {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE public.class_airspace");
        txn.commit();
    }

    pqxx::work txn(conn);
    auto stream = pqxx::stream_to::table(txn, {"class_airspace"}, {
        "ident", "icao_id", "name", "class", "type_code", "local_type",
        "lower_val", "lower_uom", "lower_code",
        "upper_val", "upper_uom", "upper_code",
        "city", "state", "country", "geog"
    });

    int count = 0, skipped = 0;
    const json::array* feats = featuresOf(doc, "Class Airspace");
    if (!feats) { std::remove(path.c_str()); return false; }
    for (const auto& fv : *feats) {
        const json::object& f = fv.as_object();
        if (!f.contains("properties") || !f.contains("geometry") || f.at("geometry").is_null()) {
            ++skipped; continue;
        }
        const json::object& p = f.at("properties").as_object();
        auto polygons = polygonsFromGeometry(f.at("geometry").as_object());
        if (polygons.empty()) { ++skipped; continue; }

        stream.write_values(
            jstr(p, "IDENT"), jstr(p, "ICAO_ID"), jstr(p, "NAME"),
            jstr(p, "CLASS"), jstr(p, "TYPE_CODE"), jstr(p, "LOCAL_TYPE"),
            jdouble(p, "LOWER_VAL"), jstr(p, "LOWER_UOM"), jstr(p, "LOWER_CODE"),
            jdouble(p, "UPPER_VAL"), jstr(p, "UPPER_UOM"), jstr(p, "UPPER_CODE"),
            jstr(p, "CITY"), jstr(p, "STATE"), jstr(p, "COUNTRY"),
            multiPolygonWKB(polygons)
        );
        ++count;
    }
    stream.complete();
    txn.commit();

    if (verbose) std::cout << "  class airspace: " << count << " loaded, " << skipped << " skipped\n";
    std::remove(path.c_str());
    return true;
}

bool loadSpecialUseAirspace(const std::string& server, const std::string& user,
                            const std::string& database, const std::string& password,
                            bool verbose) {
    const std::string url  = "https://adds-faa.opendata.arcgis.com/api/download/v1/"
                              "items/dd0d1b726e504137ab3c41b21835d05b/geojson?layers=0";
    const std::string path = "/tmp/special_use_airspace.geojson";

    if (verbose) std::cout << "Downloading FAA Special Use Airspace data...\n";
    if (!downloadFileRetry(url, path, 300)) {
        std::cerr << "[Special Use Airspace] download failed from " << url << "\n";
        return false;
    }

    if (verbose) std::cout << "Parsing...\n";
    json::value doc;
    try {
        doc = json::parse(readWholeFile(path));
    } catch (const std::exception& e) {
        std::cerr << "[Special Use Airspace] JSON parse error: " << e.what() << "\n";
        std::remove(path.c_str());
        return false;
    }

    std::string conn_str = "host=" + server + " dbname=" + database + " user=" + user;
    if (!password.empty()) conn_str += " password=" + password;
    pqxx::connection conn(conn_str);

    {
        pqxx::nontransaction txn(conn);
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS public.special_use_airspace (
                id            serial PRIMARY KEY,
                name          text,
                type_code     varchar(8),
                class         varchar(8),
                lower_val     double precision,
                lower_uom     varchar(4),
                lower_code    varchar(8),
                upper_val     double precision,
                upper_uom     varchar(4),
                upper_code    varchar(8),
                city          text,
                state         text,
                country       text,
                times_of_use  text,
                remarks       text,
                geog          public.geometry
            )
        )");
        txn.exec("CREATE INDEX IF NOT EXISTS special_use_airspace_geog_idx ON public.special_use_airspace USING GIST (geog)");
        txn.exec("CREATE INDEX IF NOT EXISTS special_use_airspace_type_idx ON public.special_use_airspace (type_code)");
        txn.exec("CREATE INDEX IF NOT EXISTS special_use_airspace_state_idx ON public.special_use_airspace (state)");
    }
    {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE public.special_use_airspace");
        txn.commit();
    }

    pqxx::work txn(conn);
    auto stream = pqxx::stream_to::table(txn, {"special_use_airspace"}, {
        "name", "type_code", "class",
        "lower_val", "lower_uom", "lower_code",
        "upper_val", "upper_uom", "upper_code",
        "city", "state", "country", "times_of_use", "remarks", "geog"
    });

    int count = 0, skipped = 0;
    const json::array* feats = featuresOf(doc, "Special Use Airspace");
    if (!feats) { std::remove(path.c_str()); return false; }
    for (const auto& fv : *feats) {
        const json::object& f = fv.as_object();
        if (!f.contains("properties") || !f.contains("geometry") || f.at("geometry").is_null()) {
            ++skipped; continue;
        }
        const json::object& p = f.at("properties").as_object();
        auto polygons = polygonsFromGeometry(f.at("geometry").as_object());
        if (polygons.empty()) { ++skipped; continue; }

        stream.write_values(
            jstr(p, "NAME"), jstr(p, "TYPE_CODE"), jstr(p, "CLASS"),
            jdouble(p, "LOWER_VAL"), jstr(p, "LOWER_UOM"), jstr(p, "LOWER_CODE"),
            jdouble(p, "UPPER_VAL"), jstr(p, "UPPER_UOM"), jstr(p, "UPPER_CODE"),
            jstr(p, "CITY"), jstr(p, "STATE"), jstr(p, "COUNTRY"),
            jstr(p, "TIMESOFUSE"), jstr(p, "REMARKS"),
            multiPolygonWKB(polygons)
        );
        ++count;
    }
    stream.complete();
    txn.commit();

    if (verbose) std::cout << "  special use airspace: " << count << " loaded, " << skipped << " skipped\n";
    std::remove(path.c_str());
    return true;
}

bool loadInternationalAirspace(const std::string& server, const std::string& user,
                               const std::string& database, const std::string& password,
                               const std::string& api_key, bool verbose) {
    if (api_key.empty()) {
        std::cerr << "[International Airspace] no OpenAIP API key provided\n";
        return false;
    }

    std::string conn_str = "host=" + server + " dbname=" + database + " user=" + user;
    if (!password.empty()) conn_str += " password=" + password;
    pqxx::connection conn(conn_str);

    {
        pqxx::nontransaction txn(conn);
        txn.exec(R"(
            CREATE TABLE IF NOT EXISTS public.international_airspace (
                id          serial PRIMARY KEY,
                openaip_id  varchar(32),
                name        text,
                type        integer,
                icao_class  integer,
                country     varchar(4),
                lower_val   double precision,
                lower_unit  integer,
                lower_ref   integer,
                upper_val   double precision,
                upper_unit  integer,
                upper_ref   integer,
                activity    integer,
                on_demand   boolean,
                on_request  boolean,
                by_notam    boolean,
                updated_at  timestamptz,
                geog        public.geometry
            )
        )");
        txn.exec("CREATE INDEX IF NOT EXISTS international_airspace_geog_idx    ON public.international_airspace USING GIST (geog)");
        txn.exec("CREATE INDEX IF NOT EXISTS international_airspace_country_idx ON public.international_airspace (country)");
        txn.exec("CREATE INDEX IF NOT EXISTS international_airspace_type_idx   ON public.international_airspace (type)");
    }
    {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE public.international_airspace");
        txn.commit();
    }

    pqxx::work txn(conn);
    auto stream = pqxx::stream_to::table(txn, {"international_airspace"}, {
        "openaip_id", "name", "type", "icao_class", "country",
        "lower_val", "lower_unit", "lower_ref",
        "upper_val", "upper_unit", "upper_ref",
        "activity", "on_demand", "on_request", "by_notam",
        "updated_at", "geog"
    });

    constexpr int kPageLimit = 1000;
    constexpr int kMaxPages = 1000; // safety cap well above the real ~28 pages
    int count = 0, skipped = 0, us_skipped = 0;

    for (int page = 1; page <= kMaxPages; ++page) {
        std::ostringstream url;
        url << "https://api.core.openaip.net/api/airspaces?apiKey=" << api_key
            << "&limit=" << kPageLimit << "&page=" << page;

        std::string body;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            ok = fetchJson(url.str(), body);
            if (!ok && attempt < 2) std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        if (!ok) {
            std::cerr << "[International Airspace] page " << page << " download failed, stopping\n";
            break;
        }

        json::value doc;
        try {
            doc = json::parse(body);
        } catch (const std::exception& e) {
            std::cerr << "[International Airspace] page " << page << " JSON parse error: " << e.what() << "\n";
            break;
        }
        if (!doc.is_object() || !doc.as_object().contains("items")) {
            std::cerr << "[International Airspace] page " << page << " response has no 'items' array\n";
            break;
        }

        const json::array& items = doc.as_object().at("items").as_array();
        if (items.empty()) break;

        for (const auto& iv : items) {
            const json::object& it = iv.as_object();
            if (!it.contains("geometry") || it.at("geometry").is_null()) { ++skipped; continue; }

            std::string country = jstr(it, "country");
            // FAA data (class_airspace/special_use_airspace) is the
            // authoritative source for the US — same split as terrain's
            // 3DEP (US) / Copernicus (elsewhere).
            if (country == "US") { ++us_skipped; continue; }

            auto polygons = polygonsFromGeometry(it.at("geometry").as_object());
            if (polygons.empty()) { ++skipped; continue; }

            AltLimit lo = jaltLimit(it, "lowerLimit");
            AltLimit up = jaltLimit(it, "upperLimit");
            std::optional<std::string> updated_at = hasField(it, "updatedAt")
                ? std::make_optional(jstr(it, "updatedAt")) : std::nullopt;

            stream.write_values(
                jstr(it, "_id"), jstr(it, "name"),
                jint(it, "type"), jint(it, "icaoClass"), country,
                lo.value, lo.unit, lo.ref,
                up.value, up.unit, up.ref,
                jint(it, "activity"),
                jbool(it, "onDemand"), jbool(it, "onRequest"), jbool(it, "byNotam"),
                updated_at,
                multiPolygonWKB(polygons)
            );
            ++count;
        }

        if (verbose) std::cout << "  page " << page << ": " << count << " loaded so far\n";

        if (static_cast<int>(items.size()) < kPageLimit) break; // last page
        // No documented rate limit on this free-tier API, but no reason to
        // hammer it either.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    stream.complete();
    txn.commit();

    if (verbose)
        std::cout << "  international airspace: " << count << " loaded, " << skipped
                  << " skipped (no/invalid geometry), " << us_skipped
                  << " skipped (US, covered by FAA data)\n";
    return count > 0;
}
