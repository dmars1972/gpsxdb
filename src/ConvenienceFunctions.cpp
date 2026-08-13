#include "ConvenienceFunctions.h"

#include <iostream>
#include <string>
#include <vector>

// Non-essential convenience functions, not core import data -- a bug here
// must never be allowed to take down a caller that's mid-import (it already
// has once: an earlier version of airspace_at_point()'s SQL crashed a bulk
// import 7+ hours in, at the very last loader phase, via an uncaught
// pqxx::undefined_table exception -> std::terminate()). Every function below
// keeps its own try/catch for exactly that reason.

void createNearestAirportFunction(pqxx::connection& conn) {
    try {
        pqxx::work txn(conn);
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
        std::cerr << "[ConvenienceFunctions] nearest_airport() creation failed (non-fatal): "
                  << e.what() << "\n";
    }
}

void createObstaclesNearbyFunction(pqxx::connection& conn) {
    try {
        pqxx::work txn(conn);
        txn.exec(
            "CREATE OR REPLACE FUNCTION public.obstacles_nearby("
            "  p_x double precision, p_y double precision, "
            "  p_radius_m double precision DEFAULT 9260, p_srid integer DEFAULT 4326"
            ") RETURNS TABLE(id integer, obstacle_type varchar, agl_ht integer, "
            "                amsl_ht integer, lighting varchar, dist_m double precision) "
            "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
            "  SELECT o.id, o.obstacle_type, o.agl_ht, o.amsl_ht, o.lighting, "
            "         ST_Distance(o.geog, pt.g) "
            "  FROM public.faa_obstacles o, "
            "       (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 3857) AS g) pt "
            "  WHERE ST_DWithin(o.geog, pt.g, p_radius_m) "
            "  ORDER BY o.geog <-> pt.g "
            "$f$");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[ConvenienceFunctions] obstacles_nearby() creation failed (non-fatal): "
                  << e.what() << "\n";
    }
}

void createDeclinationFunction(pqxx::connection& conn) {
    try {
        pqxx::work txn(conn);
        txn.exec(
            "CREATE OR REPLACE FUNCTION public.declination_at_point("
            "  p_x double precision, p_y double precision, p_srid integer DEFAULT 4326"
            ") RETURNS double precision "
            "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
            "  SELECT ST_Value(w.rast, 1, pt.g) "
            "  FROM public.wmm w, "
            "       (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 4326) AS g) pt "
            "  WHERE w.rast && pt.g AND ST_Value(w.rast, 1, pt.g) IS NOT NULL "
            "  LIMIT 1 "
            "$f$");
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[ConvenienceFunctions] declination_at_point() creation failed (non-fatal): "
                  << e.what() << "\n";
    }
}

void createElevationFunctions(pqxx::connection& conn) {
    try {
        pqxx::work txn(conn);
        // p_srid defaults to 4326 (lon/lat) but accepts 3857 (Web Mercator,
        // the raster's native SRID) directly too -- ST_Transform is a no-op
        // when source and target SRID already match. DROP first: adding a
        // parameter changes the signature, so CREATE OR REPLACE alone would
        // leave the old 2-arg overload in place instead of replacing it.
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
        // projection so bearing/distance stay accurate at any latitude --
        // it requires geography (always lon/lat internally), so a non-4326
        // origin is transformed to 4326 once up front.
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
    } catch (const std::exception& e) {
        std::cerr << "[ConvenienceFunctions] elevation_at_point_ft()/elevation_along_bearing_ft() "
                     "creation failed (non-fatal): " << e.what() << "\n";
    }
}

void createAirspaceQueryFunctions(pqxx::connection& conn) {
    bool have_class = false, have_sua = false, have_intl = false, have_tfr = false, have_mtr = false;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "SELECT table_name FROM information_schema.tables WHERE table_schema='public' "
            "AND table_name IN ('class_airspace','special_use_airspace','international_airspace',"
            "'national_defense_tfr','military_training_routes')");
        for (auto row : r) {
            std::string t = row[0].as<std::string>();
            if (t == "class_airspace") have_class = true;
            else if (t == "special_use_airspace") have_sua = true;
            else if (t == "international_airspace") have_intl = true;
            else if (t == "national_defense_tfr") have_tfr = true;
            else if (t == "military_training_routes") have_mtr = true;
        }
        txn.commit();
    }

    if (have_class || have_sua || have_intl || have_tfr) {
        std::vector<std::string> branches;
        if (have_class)
            branches.push_back(
                "SELECT 'FAA_CLASS'::text, a.class, a.type_code, a.name, "
                "       a.lower_val, a.lower_uom, a.lower_code, a.upper_val, a.upper_uom, a.upper_code, a.country "
                "FROM public.class_airspace a, pt WHERE ST_Contains(a.geog, pt.g)");
        if (have_sua)
            branches.push_back(
                "SELECT 'FAA_SUA'::text, s.class, s.type_code, s.name, "
                "       s.lower_val, s.lower_uom, s.lower_code, s.upper_val, s.upper_uom, s.upper_code, s.country "
                "FROM public.special_use_airspace s, pt WHERE ST_Contains(s.geog, pt.g)");
        if (have_intl)
            // icao_class (0-6) decoded to the ICAO letter it actually
            // represents (A-G); type/lower_unit/lower_ref/upper_unit/
            // upper_ref are deliberately left as OpenAIP's raw numeric
            // codes, not decoded -- see AirspaceLoader::loadInternationalAirspace's
            // doc comment on why guessing at those specific codes isn't safe.
            branches.push_back(
                "SELECT 'OPENAIP'::text, "
                "       CASE i.icao_class WHEN 0 THEN 'A' WHEN 1 THEN 'B' WHEN 2 THEN 'C' WHEN 3 THEN 'D' "
                "                         WHEN 4 THEN 'E' WHEN 5 THEN 'F' WHEN 6 THEN 'G' ELSE NULL END, "
                "       i.type::text, i.name, "
                "       i.lower_val, i.lower_unit::text, i.lower_ref::text, "
                "       i.upper_val, i.upper_unit::text, i.upper_ref::text, i.country "
                "FROM public.international_airspace i, pt WHERE ST_Contains(i.geog, pt.g)");
        if (have_tfr)
            // national_defense_tfr has no altitude/class fields (it's a TFR
            // area, not a classified airspace layer) -- nulls for those
            // columns, matching how the other branches null out columns
            // they don't have.
            branches.push_back(
                "SELECT 'NDA_TFR'::text, NULL::text, t.type_code, t.name, "
                "       NULL::double precision, NULL::text, NULL::text, "
                "       NULL::double precision, NULL::text, NULL::text, t.country "
                "FROM public.national_defense_tfr t, pt WHERE ST_Contains(t.geog, pt.g)");

        std::string body =
            "WITH pt AS (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 3857) AS g) ";
        for (size_t i = 0; i < branches.size(); ++i) {
            if (i > 0) body += " UNION ALL ";
            body += branches[i];
        }

        try {
            pqxx::work txn(conn);
            txn.exec(
                "CREATE OR REPLACE FUNCTION public.airspace_at_point("
                "  p_x double precision, p_y double precision, p_srid integer DEFAULT 4326"
                ") RETURNS TABLE(source text, class text, type text, name text, "
                "                lower_val double precision, lower_uom text, lower_code text, "
                "                upper_val double precision, upper_uom text, upper_code text, "
                "                country text) "
                "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ " + body + " $f$");
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[ConvenienceFunctions] airspace_at_point() creation failed (non-fatal, "
                         "may be stale or missing): " << e.what() << "\n";
        }
    }

    if (have_mtr) {
        try {
            pqxx::work txn(conn);
            txn.exec(
                "CREATE OR REPLACE FUNCTION public.military_routes_nearby("
                "  p_x double precision, p_y double precision, "
                "  p_radius_m double precision DEFAULT 9260, p_srid integer DEFAULT 4326"
                ") RETURNS TABLE(id integer, ident varchar, mtr_type integer, "
                "                lower_val double precision, upper_val double precision, "
                "                dist_m double precision) "
                "LANGUAGE sql STABLE PARALLEL SAFE AS $f$ "
                "  SELECT m.id, m.ident, m.mtr_type, m.lower_val, m.upper_val, "
                "         ST_Distance(m.geog, pt.g) "
                "  FROM public.military_training_routes m, "
                "       (SELECT ST_Transform(ST_SetSRID(ST_MakePoint(p_x, p_y), p_srid), 3857) AS g) pt "
                "  WHERE ST_DWithin(m.geog, pt.g, p_radius_m) "
                "  ORDER BY m.geog <-> pt.g "
                "$f$");
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[ConvenienceFunctions] military_routes_nearby() creation failed (non-fatal, "
                         "may be stale or missing): " << e.what() << "\n";
        }
    }
}
