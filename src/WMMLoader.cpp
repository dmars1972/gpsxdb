#include "WMMLoader.h"
#include <pqxx/pqxx>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>

double currentDecimalYear() {
    std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    int year = tm_utc.tm_year + 1900;
    return year + tm_utc.tm_yday / 365.25;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// WMM2025 spherical-harmonic Gauss coefficients (n, m, gnm, hnm, dgnm, dhnm),
// degree/order 12, epoch 2025.0, valid 2025.0-2030.0. Embedded verbatim from
// NOAA/NCEI's public-domain WMM.COF
// (https://www.ncei.noaa.gov/products/world-magnetic-model) rather than
// requiring a runtime data file — a future NOAA model release (WMM2030
// etc.) needs this table (and kWMMEpoch below) swapped for the new one.
struct WMMCoefRow { int n, m; double gnm, hnm, dgnm, dhnm; };
constexpr double kWMMEpoch = 2025.0;
constexpr int kWMMMaxOrd = 12;
constexpr WMMCoefRow kWMMCoefs[] = {
    {1, 0, -29351.8, 0.0, 12.0, 0.0},
    {1, 1, -1410.8, 4545.4, 9.7, -21.5},
    {2, 0, -2556.6, 0.0, -11.6, 0.0},
    {2, 1, 2951.1, -3133.6, -5.2, -27.7},
    {2, 2, 1649.3, -815.1, -8.0, -12.1},
    {3, 0, 1361.0, 0.0, -1.3, 0.0},
    {3, 1, -2404.1, -56.6, -4.2, 4.0},
    {3, 2, 1243.8, 237.5, 0.4, -0.3},
    {3, 3, 453.6, -549.5, -15.6, -4.1},
    {4, 0, 895.0, 0.0, -1.6, 0.0},
    {4, 1, 799.5, 278.6, -2.4, -1.1},
    {4, 2, 55.7, -133.9, -6.0, 4.1},
    {4, 3, -281.1, 212.0, 5.6, 1.6},
    {4, 4, 12.1, -375.6, -7.0, -4.4},
    {5, 0, -233.2, 0.0, 0.6, 0.0},
    {5, 1, 368.9, 45.4, 1.4, -0.5},
    {5, 2, 187.2, 220.2, 0.0, 2.2},
    {5, 3, -138.7, -122.9, 0.6, 0.4},
    {5, 4, -142.0, 43.0, 2.2, 1.7},
    {5, 5, 20.9, 106.1, 0.9, 1.9},
    {6, 0, 64.4, 0.0, -0.2, 0.0},
    {6, 1, 63.8, -18.4, -0.4, 0.3},
    {6, 2, 76.9, 16.8, 0.9, -1.6},
    {6, 3, -115.7, 48.8, 1.2, -0.4},
    {6, 4, -40.9, -59.8, -0.9, 0.9},
    {6, 5, 14.9, 10.9, 0.3, 0.7},
    {6, 6, -60.7, 72.7, 0.9, 0.9},
    {7, 0, 79.5, 0.0, -0.0, 0.0},
    {7, 1, -77.0, -48.9, -0.1, 0.6},
    {7, 2, -8.8, -14.4, -0.1, 0.5},
    {7, 3, 59.3, -1.0, 0.5, -0.8},
    {7, 4, 15.8, 23.4, -0.1, 0.0},
    {7, 5, 2.5, -7.4, -0.8, -1.0},
    {7, 6, -11.1, -25.1, -0.8, 0.6},
    {7, 7, 14.2, -2.3, 0.8, -0.2},
    {8, 0, 23.2, 0.0, -0.1, 0.0},
    {8, 1, 10.8, 7.1, 0.2, -0.2},
    {8, 2, -17.5, -12.6, 0.0, 0.5},
    {8, 3, 2.0, 11.4, 0.5, -0.4},
    {8, 4, -21.7, -9.7, -0.1, 0.4},
    {8, 5, 16.9, 12.7, 0.3, -0.5},
    {8, 6, 15.0, 0.7, 0.2, -0.6},
    {8, 7, -16.8, -5.2, -0.0, 0.3},
    {8, 8, 0.9, 3.9, 0.2, 0.2},
    {9, 0, 4.6, 0.0, -0.0, 0.0},
    {9, 1, 7.8, -24.8, -0.1, -0.3},
    {9, 2, 3.0, 12.2, 0.1, 0.3},
    {9, 3, -0.2, 8.3, 0.3, -0.3},
    {9, 4, -2.5, -3.3, -0.3, 0.3},
    {9, 5, -13.1, -5.2, 0.0, 0.2},
    {9, 6, 2.4, 7.2, 0.3, -0.1},
    {9, 7, 8.6, -0.6, -0.1, -0.2},
    {9, 8, -8.7, 0.8, 0.1, 0.4},
    {9, 9, -12.9, 10.0, -0.1, 0.1},
    {10, 0, -1.3, 0.0, 0.1, 0.0},
    {10, 1, -6.4, 3.3, 0.0, 0.0},
    {10, 2, 0.2, 0.0, 0.1, -0.0},
    {10, 3, 2.0, 2.4, 0.1, -0.2},
    {10, 4, -1.0, 5.3, -0.0, 0.1},
    {10, 5, -0.6, -9.1, -0.3, -0.1},
    {10, 6, -0.9, 0.4, 0.0, 0.1},
    {10, 7, 1.5, -4.2, -0.1, 0.0},
    {10, 8, 0.9, -3.8, -0.1, -0.1},
    {10, 9, -2.7, 0.9, -0.0, 0.2},
    {10, 10, -3.9, -9.1, -0.0, -0.0},
    {11, 0, 2.9, 0.0, 0.0, 0.0},
    {11, 1, -1.5, 0.0, -0.0, -0.0},
    {11, 2, -2.5, 2.9, 0.0, 0.1},
    {11, 3, 2.4, -0.6, 0.0, -0.0},
    {11, 4, -0.6, 0.2, 0.0, 0.1},
    {11, 5, -0.1, 0.5, -0.1, -0.0},
    {11, 6, -0.6, -0.3, 0.0, -0.0},
    {11, 7, -0.1, -1.2, -0.0, 0.1},
    {11, 8, 1.1, -1.7, -0.1, -0.0},
    {11, 9, -1.0, -2.9, -0.1, 0.0},
    {11, 10, -0.2, -1.8, -0.1, 0.0},
    {11, 11, 2.6, -2.3, -0.1, 0.0},
    {12, 0, -2.0, 0.0, 0.0, 0.0},
    {12, 1, -0.2, -1.3, 0.0, -0.0},
    {12, 2, 0.3, 0.7, -0.0, 0.0},
    {12, 3, 1.2, 1.0, -0.0, -0.1},
    {12, 4, -1.3, -1.4, -0.0, 0.1},
    {12, 5, 0.6, -0.0, -0.0, -0.0},
    {12, 6, 0.6, 0.6, 0.1, -0.0},
    {12, 7, 0.5, -0.1, -0.0, -0.0},
    {12, 8, -0.1, 0.8, 0.0, 0.0},
    {12, 9, -0.4, 0.1, 0.0, -0.0},
    {12, 10, -0.2, -1.0, -0.1, -0.0},
    {12, 11, -1.3, 0.1, -0.0, 0.0},
    {12, 12, -0.7, 0.2, -0.1, -0.1},
};

// Direct port of NOAA/NCEI's public-domain WMM reference algorithm (the
// same one distributed as GeomagnetismLibrary.c, and used by e.g. the
// pygeomag package) — validated by hand against NOAA's own published
// WMM2025_TestValues.txt (18 test points spanning 2025.0-2029.5) to within
// 0.005 degrees before this port existed. Deliberately mirrors the
// reference's variable names/structure/iteration order closely rather than
// being "improved" or simplified, since subtle reordering is exactly how
// normalization or recursion bugs creep into this kind of code.
class WMMModel {
public:
    WMMModel() { loadCoefficients(); }

    // Geomagnetic declination in degrees (east positive) at a geodetic
    // position/time. lat/lon in degrees, alt_km referenced to the WGS84
    // ellipsoid, year as decimal (e.g. 2026.5). Pure function of its
    // arguments and this object's (read-only, set-once) coefficient
    // tables — safe to call concurrently from multiple threads sharing one
    // WMMModel instance, since all mutable recursion state below is local.
    double declinationDeg(double lat_deg, double lon_deg, double alt_km, double year) const {
        constexpr int N = kWMMMaxOrd;
        constexpr int SZ = N + 1;

        double tc[SZ][SZ] = {};
        double dp[SZ][SZ] = {};
        double sp[SZ] = {};
        double cp[SZ] = {};
        double pp[SZ] = {};
        double P[SZ * SZ] = {};  // flat, P[n + m*SZ] — same indexing as the
                                  // reference's reused snorm/legendre array,
                                  // but local per call (see class comment)

        sp[0] = 0.0;
        cp[0] = pp[0] = 1.0;
        P[0] = 1.0;  // seeds P(0,0) = 1, matching the reference's snorm[0]
        dp[0][0] = 0.0;

        constexpr double a = 6378.137, b = 6356.7523142, re = 6371.2;
        constexpr double a2 = a * a, b2 = b * b, c2 = a2 - b2;
        constexpr double a4 = a2 * a2, b4 = b2 * b2, c4 = a4 - b4;

        double dt = year - kWMMEpoch;

        double rlon = lon_deg * kPi / 180.0;
        double rlat = lat_deg * kPi / 180.0;
        double srlon = std::sin(rlon), crlon = std::cos(rlon);
        double srlat = std::sin(rlat), crlat = std::cos(rlat);
        double srlat2 = srlat * srlat, crlat2 = crlat * crlat;
        sp[1] = srlon;
        cp[1] = crlon;

        // Geodetic (WGS84) to geocentric spherical coordinates.
        double q = std::sqrt(a2 - c2 * srlat2);
        double q1 = alt_km * q;
        double q2r = (q1 + a2) / (q1 + b2);
        double q2 = q2r * q2r;
        double ct = srlat / std::sqrt(q2 * crlat2 + srlat2);
        double st = std::sqrt(1.0 - ct * ct);
        double r2 = alt_km * alt_km + 2.0 * q1 + (a4 - c4 * srlat2) / (q * q);
        double r = std::sqrt(r2);
        double dd = std::sqrt(a2 * crlat2 + b2 * srlat2);
        double ca = (alt_km + dd) / r;
        double sa = c2 * crlat * srlat / (r * dd);

        for (int m = 2; m <= N; ++m) {
            sp[m] = sp[1] * cp[m - 1] + cp[1] * sp[m - 1];
            cp[m] = cp[1] * cp[m - 1] - sp[1] * sp[m - 1];
        }

        double aor = re / r;
        double ar = aor * aor;
        double br = 0, bt = 0, bp = 0, bpp = 0;
        (void)br;

        for (int n = 1; n <= N; ++n) {
            ar *= aor;
            for (int m = 0; m <= n; ++m) {
                // Unnormalized associated Legendre polynomials and
                // derivatives via recursion (normalization was already
                // folded into c_/cd_ once, in loadCoefficients).
                if (n == m) {
                    P[n + m * SZ] = st * P[(n - 1) + (m - 1) * SZ];
                    dp[m][n] = st * dp[m - 1][n - 1] + ct * P[(n - 1) + (m - 1) * SZ];
                } else if (n == 1 && m == 0) {
                    P[n + m * SZ] = ct * P[(n - 1) + m * SZ];
                    dp[m][n] = ct * dp[m][n - 1] - st * P[(n - 1) + m * SZ];
                } else if (n > 1 && n != m) {
                    if (m > n - 2) {
                        P[(n - 2) + m * SZ] = 0.0;
                        dp[m][n - 2] = 0.0;
                    }
                    P[n + m * SZ] = ct * P[(n - 1) + m * SZ] - k_[m][n] * P[(n - 2) + m * SZ];
                    dp[m][n] = ct * dp[m][n - 1] - st * P[(n - 1) + m * SZ] - k_[m][n] * dp[m][n - 2];
                }

                // Time-adjust the Gauss coefficients via secular variation.
                tc[m][n] = c_[m][n] + dt * cd_[m][n];
                if (m != 0) tc[n][m - 1] = c_[n][m - 1] + dt * cd_[n][m - 1];

                double par = ar * P[n + m * SZ];
                double temp1, temp2;
                if (m == 0) {
                    temp1 = tc[m][n] * cp[m];
                    temp2 = tc[m][n] * sp[m];
                } else {
                    temp1 = tc[m][n] * cp[m] + tc[n][m - 1] * sp[m];
                    temp2 = tc[m][n] * sp[m] - tc[n][m - 1] * cp[m];
                }
                bt -= ar * temp1 * dp[m][n];
                bp += fm_[m] * temp2 * par;
                br += fn_[n] * temp1 * par;

                // Special case at the geographic poles (st == 0), where the
                // usual bp /= st below would divide by zero.
                if (st == 0.0 && m == 1) {
                    if (n == 1) pp[n] = pp[n - 1];
                    else pp[n] = ct * pp[n - 1] - k_[m][n] * pp[n - 2];
                    double parp = ar * pp[n];
                    bpp += fm_[m] * temp2 * parp;
                }
            }
        }

        if (st == 0.0) bp = bpp;
        else bp /= st;

        // Rotate from geocentric spherical (br, bt, bp) to geodetic (bx
        // north, by east, bz down) — only bx/by are needed for declination.
        double bx = -bt * ca - br * sa;
        double by = bp;

        return std::atan2(by, bx) * 180.0 / kPi;
    }

private:
    static constexpr int SZ = kWMMMaxOrd + 1;
    double c_[SZ][SZ] = {};
    double cd_[SZ][SZ] = {};
    double fn_[SZ] = {};
    double fm_[SZ] = {};
    double k_[SZ][SZ] = {};

    void loadCoefficients() {
        for (auto& row : kWMMCoefs) {
            int n = row.n, m = row.m;
            c_[m][n] = row.gnm;
            cd_[m][n] = row.dgnm;
            if (m != 0) {
                c_[n][m - 1] = row.hnm;
                cd_[n][m - 1] = row.dhnm;
            }
        }

        // Fold Schmidt quasi-normalization into c_/cd_ once here, so the
        // per-call recursion above can use plain unnormalized Legendre
        // polynomials.
        std::vector<double> snorm(static_cast<size_t>(SZ) * SZ, 0.0);
        snorm[0] = 1.0;
        fm_[0] = 0.0;
        for (int n = 1; n <= kWMMMaxOrd; ++n) {
            snorm[n] = snorm[n - 1] * static_cast<double>(2 * n - 1) / static_cast<double>(n);
            int j = 2;
            for (int m = 0; m <= n; ++m) {
                k_[m][n] = static_cast<double>((n - 1) * (n - 1) - m * m) /
                           static_cast<double>((2 * n - 1) * (2 * n - 3));
                if (m > 0) {
                    double flnmj = static_cast<double>((n - m + 1) * j) / static_cast<double>(n + m);
                    snorm[n + m * SZ] = snorm[n + (m - 1) * SZ] * std::sqrt(flnmj);
                    j = 1;
                    c_[n][m - 1] = snorm[n + m * SZ] * c_[n][m - 1];
                    cd_[n][m - 1] = snorm[n + m * SZ] * cd_[n][m - 1];
                }
                c_[m][n] = snorm[n + m * SZ] * c_[m][n];
                cd_[m][n] = snorm[n + m * SZ] * cd_[m][n];
            }
            fn_[n] = static_cast<double>(n + 1);
            fm_[n] = static_cast<double>(n);
        }
        k_[1][1] = 0.0;
    }
};

// Fixed cell size for the resumable global grid — see WMMLoader.h. Named by
// SW corner, same convention as Copernicus DEM tiles (floor(lat)/floor(lon)
// used directly).
constexpr int kCellDeg = 10;

struct Cell { int lat; int lon; std::string name; };

std::vector<Cell> cellsForBBox(double min_lon, double min_lat, double max_lon, double max_lat) {
    std::vector<Cell> cells;
    int lat0 = static_cast<int>(std::floor(min_lat / kCellDeg)) * kCellDeg;
    int lat1 = static_cast<int>(std::ceil(max_lat / kCellDeg)) * kCellDeg;
    int lon0 = static_cast<int>(std::floor(min_lon / kCellDeg)) * kCellDeg;
    int lon1 = static_cast<int>(std::ceil(max_lon / kCellDeg)) * kCellDeg;
    for (int lat = lat0; lat < lat1; lat += kCellDeg) {
        for (int lon = lon0; lon < lon1; lon += kCellDeg) {
            char buf[32];
            char ns = lat >= 0 ? 'N' : 'S';
            char ew = lon >= 0 ? 'E' : 'W';
            snprintf(buf, sizeof(buf), "%c%02d%c%03d", ns, std::abs(lat), ew, std::abs(lon));
            cells.push_back({lat, lon, buf});
        }
    }
    return cells;
}

} // namespace

bool loadWMM(const std::string& server, const std::string& user,
             const std::string& database, const std::string& password,
             double year,
             double min_lon, double min_lat, double max_lon, double max_lat,
             double grid_deg, int dest_srid, double band_deg, double simplify_m,
             int threads, bool verbose) {
    auto cells = cellsForBBox(min_lon, min_lat, max_lon, max_lat);
    if (cells.empty()) {
        std::cerr << "[WMM] bounding box produced no cells\n";
        return false;
    }

    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;

    pqxx::connection conn(connstr);
    {
        pqxx::work txn(conn);
        txn.exec("CREATE EXTENSION IF NOT EXISTS postgis_raster");
        txn.exec("CREATE TABLE IF NOT EXISTS public.wmm (rid serial PRIMARY KEY, rast public.raster)");
        txn.exec("CREATE INDEX IF NOT EXISTS wmm_rast_gist ON public.wmm USING GIST (ST_ConvexHull(rast))");
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.wmm_cells ("
            "  cell_name text PRIMARY KEY,"
            "  loaded_at timestamptz NOT NULL DEFAULT now())");
        txn.commit();
    }

    std::vector<Cell> to_load;
    for (auto& c : cells) {
        pqxx::work txn(conn);
        auto r = txn.exec("SELECT 1 FROM wmm_cells WHERE cell_name=$1", pqxx::params{c.name});
        txn.commit();
        if (r.empty()) to_load.push_back(c);
        else if (verbose) std::cout << "  " << c.name << " already loaded, skipping\n";
    }

    if (to_load.empty()) {
        if (verbose) std::cout << "All requested cells already loaded.\n";
        if (band_deg > 0)
            buildWMMBands(server, user, database, password, band_deg, dest_srid, simplify_m, threads, verbose);
        return true;
    }

    int grid_n = static_cast<int>(std::llround(kCellDeg / grid_deg));

    std::atomic<size_t> next_cell{0};
    std::atomic<long long> total_loaded{0};
    std::mutex io_mu;
    WMMModel model;  // shared read-only across threads — see class comment

    auto worker = [&]() {
        // See the equivalent try/catch in TerrainLoader's workers: an
        // exception escaping a std::thread's function aborts the entire
        // process via std::terminate(), so a transient connection failure
        // here must not be allowed to propagate past this thread.
        try {
            pqxx::connection wconn(connstr);
            while (true) {
                size_t idx = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (idx >= to_load.size()) break;
                const Cell& cell = to_load[idx];

                std::vector<double> values;
                values.reserve(static_cast<size_t>(grid_n) * grid_n);
                double north = cell.lat + kCellDeg;
                for (int row = 0; row < grid_n; ++row) {
                    double lat_c = north - (row + 0.5) * grid_deg;
                    for (int col = 0; col < grid_n; ++col) {
                        double lon_c = cell.lon + (col + 0.5) * grid_deg;
                        values.push_back(model.declinationDeg(lat_c, lon_c, 0.0, year));
                    }
                }

                std::ostringstream arr;
                arr << std::fixed << std::setprecision(4) << '{';
                for (int row = 0; row < grid_n; ++row) {
                    if (row) arr << ',';
                    arr << '{';
                    for (int col = 0; col < grid_n; ++col) {
                        if (col) arr << ',';
                        arr << values[static_cast<size_t>(row) * grid_n + col];
                    }
                    arr << '}';
                }
                arr << '}';

                try {
                    pqxx::work txn(wconn);
                    txn.exec(
                        "INSERT INTO public.wmm(rast) VALUES ("
                        "  ST_SetValues("
                        "    ST_AddBand(ST_MakeEmptyRaster($1,$2,$3,$4,$5,$6,0,0,4326),"
                        "               '32BF'::text, 0::double precision, NULL::double precision),"
                        "    1, 1, 1, $7::double precision[][]"
                        "  ))",
                        pqxx::params{grid_n, grid_n, cell.lon, north, grid_deg, -grid_deg, arr.str()});
                    txn.exec("INSERT INTO wmm_cells(cell_name) VALUES ($1) ON CONFLICT DO NOTHING",
                             pqxx::params{cell.name});
                    txn.commit();
                    total_loaded.fetch_add(1, std::memory_order_relaxed);
                    if (verbose) {
                        std::lock_guard lk(io_mu);
                        std::cout << "  " << cell.name << ": computed (" << grid_n << "x" << grid_n << " grid)\n";
                    }
                } catch (const std::exception& e) {
                    std::lock_guard lk(io_mu);
                    std::cerr << "[WMM] cell " << cell.name << " insert failed: " << e.what() << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard lk(io_mu);
            std::cerr << "[WMM] worker thread error: " << e.what() << "\n";
        }
    };

    int nthreads = std::max(1, std::min(threads, static_cast<int>(to_load.size())));
    if (verbose) std::cout << "[WMM] computing " << to_load.size() << " cell(s) with " << nthreads << " thread(s)\n";
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) workers.emplace_back(worker);
    for (auto& w : workers) w.join();

    if (total_loaded.load() == 0) {
        std::cerr << "[WMM] no cells in this bounding box could be computed\n";
        return false;
    }

    if (verbose) std::cout << "WMM data loaded (" << total_loaded.load() << " new cell(s)).\n";

    if (band_deg > 0)
        buildWMMBands(server, user, database, password, band_deg, dest_srid, simplify_m, threads, verbose);

    return true;
}

bool buildWMMBands(const std::string& server, const std::string& user,
                    const std::string& database, const std::string& password,
                    double band_deg, int dest_srid, double simplify_m, int threads, bool verbose) {
    std::string connstr = "host=" + server + " dbname=" + database +
                          " user=" + user + " sslmode=disable";
    if (!password.empty()) connstr += " password=" + password;
    pqxx::connection conn(connstr);

    {
        pqxx::work txn(conn);
        txn.exec(
            "CREATE TABLE IF NOT EXISTS public.wmm_bands ("
            "  id serial PRIMARY KEY,"
            "  band_min_deg double precision NOT NULL,"
            "  band_max_deg double precision NOT NULL,"
            "  geog public.geometry)");
        txn.exec(
            "CREATE INDEX IF NOT EXISTS wmm_bands_geog_idx "
            "ON public.wmm_bands USING GIST (geog)");
        txn.commit();
    }

    double min_deg, max_deg;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT min(s.min), max(s.max) "
            "FROM public.wmm t, LATERAL ST_SummaryStats(t.rast, 1) s");
        txn.commit();
        if (r.empty() || r[0][0].is_null()) {
            if (verbose) std::cout << "[WMM] no wmm data loaded, skipping band generation\n";
            return false;
        }
        min_deg = r[0][0].as<double>();
        max_deg = r[0][1].as<double>();
    }

    double lo_deg = std::floor(min_deg / band_deg) * band_deg;
    double hi_deg = std::ceil(max_deg / band_deg) * band_deg;
    int n_bands = static_cast<int>(std::llround((hi_deg - lo_deg) / band_deg));
    if (n_bands < 1) n_bands = 1;

    if (verbose)
        std::cout << "[WMM] building " << n_bands << " declination band(s) ("
                  << band_deg << " deg each, " << lo_deg << " to " << hi_deg
                  << " deg), merging across all computed cells...\n";

    try {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE public.wmm_bands");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[WMM] buildWMMBands error truncating: " << e.what() << "\n";
        return false;
    }

    // Same rationale as buildTerrainBands: simplify at every tier (fragment,
    // region, final), not just the final unioned shape, to keep every
    // individual ST_SimplifyPreserveTopology/ST_Union call's input small
    // enough to avoid GEOS's pathological-hang scaling behavior.
    auto wrapSimplify = [&](const std::string& expr) -> std::string {
        if (simplify_m <= 0) return expr;
        std::ostringstream o;
        o << "ST_SimplifyPreserveTopology(" << expr << ", " << simplify_m << ")";
        return o.str();
    };

    // The wmm raster is always stored in 4326 (see WMMLoader.h) — transform
    // to dest_srid before simplifying, since simplify_m is a tolerance in
    // dest_srid's own units (meters for 3857), meaningless applied to raw
    // degrees.
    std::string transform_expr = (dest_srid == 4326)
        ? "d.geom"
        : ("ST_Transform(d.geom, " + std::to_string(dest_srid) + ")");

    std::ostringstream frag_expr, geom_expr;
    frag_expr << wrapSimplify(transform_expr);
    geom_expr << wrapSimplify("geom");

    // Hierarchical-union bucket size, in dest_srid's own units — degrees if
    // the bands are left in 4326, meters (matching terrain's own bucket
    // size) otherwise.
    double region_size = (dest_srid == 4326) ? 10.0 : 500000.0;

    std::ostringstream reclass_all;
    reclass_all << std::fixed << std::setprecision(6);
    for (int i = 0; i < n_bands; ++i) {
        double band_lo = lo_deg + i * band_deg;
        double band_hi = lo_deg + (i + 1) * band_deg;
        if (i > 0) reclass_all << ", ";
        reclass_all << "(" << band_lo << "-" << band_hi << "]:" << (i + 1);
    }

    if (verbose)
        std::cout << "[WMM] dumping and simplifying raster fragments for all bands "
                     "(single pass over " << n_bands << " bands)...\n";

    try {
        pqxx::work txn(conn);
        txn.exec("DROP TABLE IF EXISTS public.wmm_frags_staging");
        txn.exec(
            "CREATE TABLE public.wmm_frags_staging ("
            "  band_id integer NOT NULL,"
            "  rx integer NOT NULL,"
            "  ry integer NOT NULL,"
            "  geom public.geometry)");
        std::ostringstream sql;
        sql <<
            "INSERT INTO public.wmm_frags_staging(band_id, rx, ry, geom) "
            "SELECT val, floor(ST_X(ST_Centroid(geom)) / " << region_size << ")::int, "
            "            floor(ST_Y(ST_Centroid(geom)) / " << region_size << ")::int, geom "
            "FROM ("
            "  SELECT d.val::int AS val, " << frag_expr.str() << " AS geom "
            "  FROM public.wmm t, "
            "       LATERAL ST_DumpAsPolygons("
            "         ST_Reclass(t.rast, 1, '" << reclass_all.str() << "', '32BUI', 0), 1"
            "       ) d "
            "  WHERE d.val > 0"
            ") simplified";
        txn.exec(sql.str());
        txn.exec("CREATE INDEX ON public.wmm_frags_staging (band_id)");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[WMM] buildWMMBands error building staging fragments: " << e.what() << "\n";
        return false;
    }

    std::atomic<int> next_band{0};
    std::atomic<long long> total_polygons{0};
    std::mutex io_mu;

    auto worker = [&]() {
        try {
            pqxx::connection wconn(connstr);
            while (true) {
                int i = next_band.fetch_add(1, std::memory_order_relaxed);
                if (i >= n_bands) break;

                double band_min = lo_deg + i * band_deg;
                double band_max = lo_deg + (i + 1) * band_deg;

                std::ostringstream sql;
                sql << std::fixed << std::setprecision(6) <<
                    "WITH regions AS ("
                    "  SELECT " << wrapSimplify("ST_Union(geom)") << " AS geom "
                    "  FROM public.wmm_frags_staging WHERE band_id = " << (i + 1)
                    << "  GROUP BY rx, ry"
                    ") "
                    "INSERT INTO public.wmm_bands(band_min_deg, band_max_deg, geog) "
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
                                  << " (" << band_min << " to " << band_max << " deg): "
                                  << n << " polygon(s)\n";
                    }
                } catch (const std::exception& e) {
                    std::lock_guard lk(io_mu);
                    std::cerr << "[WMM] buildWMMBands band " << (i + 1) << "/" << n_bands
                              << " error: " << e.what() << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard lk(io_mu);
            std::cerr << "[WMM] worker thread error: " << e.what() << "\n";
        }
    };

    int nthreads = std::max(1, std::min(threads, n_bands));
    if (verbose) std::cout << "[WMM] using " << nthreads << " thread(s)\n";
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) workers.emplace_back(worker);
    for (auto& w : workers) w.join();

    try {
        pqxx::work txn(conn);
        txn.exec("DROP TABLE IF EXISTS public.wmm_frags_staging");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[WMM] buildWMMBands error dropping staging table: " << e.what() << "\n";
    }

    if (verbose)
        std::cout << "[WMM] wmm_bands rebuilt: " << total_polygons.load()
                  << " polygon(s) across " << n_bands << " band(s)\n";
    return true;
}
