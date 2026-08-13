#pragma once
#include <pqxx/pqxx>

// (Re)creates the read-only SQL convenience functions (nearest_airport,
// obstacles_nearby, declination_at_point, elevation_at_point_ft/
// elevation_along_bearing_ft, airspace_at_point/military_routes_nearby)
// used by clients to query loaded data without hand-writing spatial SQL.
//
// Shared by two call sites: the loader that owns the underlying data
// (AirportsLoader, FAAObstacleLoader, WMMLoader, TerrainLoader,
// AirspaceLoader — called right after a fresh load) and regional_install.cpp
// (called after installing a region bundle, whose tables come pre-populated
// rather than freshly loaded, so there's no "loader" object to hang this
// off of). Deliberately pqxx-only, no curl/PROJ/other dependency, so it can
// link into the Windows regional_install build without pulling those in.
//
// Each function is idempotent (CREATE OR REPLACE) and non-fatal on error
// (caught and logged, never thrown) — see the comment on
// createAirspaceQueryFunctions for why that guard matters.

void createNearestAirportFunction(pqxx::connection& conn);
void createObstaclesNearbyFunction(pqxx::connection& conn);
void createDeclinationFunction(pqxx::connection& conn);

// Only safe to call once public.terrain exists — a SQL-language function
// body has its table references resolved at CREATE FUNCTION time, so
// calling this before the table exists throws pqxx::undefined_table.
void createElevationFunctions(pqxx::connection& conn);

// (Re)creates airspace_at_point() (a UNION ALL across whichever of
// class_airspace/special_use_airspace/international_airspace/
// national_defense_tfr currently exist) and military_routes_nearby().
// Queries information_schema.tables itself first so it stays correct
// regardless of which subset of source tables is actually present (e.g. a
// region bundle or a master DB with no OpenAIP key configured, so
// international_airspace never got created).
void createAirspaceQueryFunctions(pqxx::connection& conn);
