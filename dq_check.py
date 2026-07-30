#!/usr/bin/env python3
"""Data quality spot-check for the nav database.

Samples points globally (a fixed set of major/minor airports and named
landmarks on multiple continents, plus N random points rejection-sampled
to real OSM coverage, sphere-correctly distributed so latitude doesn't
oversample near the poles) and checks against each: nearest airport,
FAA obstacles (US only -- there's no global obstacle dataset), charted
airspace (FAA class/special-use in the US, OpenAIP everywhere else), WMM
magnetic declination, nearby roads (ways), and nearby land-use areas
(areas). Cross-checks WMM declination against pygeomag (an independent
NOAA WMM implementation, not this project's own WMMLoader.cpp) and a
handful of stable, well-known public facts (see GOLDEN_FACTS below) --
including, for a couple of landmarks, that the *identity* of the nearest
airport we report matches independent research (not just a numeric
elevation/length tolerance check) -- so a future bad import/regression has
a chance of being caught even without a human eyeballing the SQL results.

Meant to be rerun occasionally after a fresh import (or on a schedule),
not part of the build. Writes an HTML report; pass --json to also dump
the raw per-point results (always complete, unlike the HTML table below).

At large sample sizes (thousands of points) the HTML report caps how many
individual random-point rows it renders (see --max-table-rows) so the page
stays a reasonable size to load in a browser -- aggregate stats always
cover the full sample regardless, and every fixed point (airports,
landmarks) is always shown in full.

Requires: psycopg2-binary, pygeomag (see requirements-dq_check.txt)
"""
import argparse
import json
import math
import random
import sys
import time
from collections import defaultdict
from contextlib import contextmanager

import psycopg2
from pygeomag import GeoMag

# ---- Fixed sample points ----

MAJOR_AIRPORT_IDENTS = [
    "KATL", "KORD", "KLAX", "KJFK", "KDFW",  # US
    "EGLL",  # London Heathrow
    "LFPG",  # Paris Charles de Gaulle
    "EDDF",  # Frankfurt
    "RJTT",  # Tokyo Haneda
    "ZBAA",  # Beijing Capital
    "VHHH",  # Hong Kong
    "YSSY",  # Sydney
    "OMDB",  # Dubai
    "CYYZ",  # Toronto Pearson
    "FAOR",  # Johannesburg O.R. Tambo
    # Added for elevation-based GOLDEN_FACTS below -- notable/extreme-elevation
    # airports with well-documented, independently-verified public figures.
    "KDEN",  # Denver Intl
    "SLLP",  # El Alto Intl, La Paz -- highest international airport in the world
    "ZULS",  # Lhasa Gonggar -- one of the highest airports in the world
    "KLXV",  # Leadville, CO -- highest public-use airport in North America
    "KTEX",  # Telluride, CO -- highest commercial airport in North America
    "MMMX",  # Mexico City
    "SEQM",  # Quito
    "KASE",  # Aspen/Pitkin County
    "VNKT",  # Kathmandu Tribhuvan
    "ZUBD",  # Qamdo Bangda -- one of the highest airports in the world
    "KL06",  # Furnace Creek, Death Valley -- lowest airport in North America
    "KMSY",  # New Orleans
    "EHAM",  # Amsterdam Schiphol -- below sea level
]

LANDMARKS = [
    (38.8977, -77.0365, "White House, Washington DC"),
    (37.8199, -122.4783, "Golden Gate Bridge, San Francisco"),
    (40.6892, -74.0445, "Statue of Liberty, NYC"),
    (47.2530, -97.2905, "KVLY-TV mast, Blanchard ND (tall-tower obstacle check)"),
    (48.8584, 2.2945, "Eiffel Tower, Paris"),
    (29.9792, 31.1342, "Great Pyramid of Giza"),
    (-33.8568, 151.2153, "Sydney Opera House"),
    (-22.9519, -43.2105, "Christ the Redeemer, Rio de Janeiro"),
    (51.5007, -0.1246, "Big Ben, London"),
    (55.7520, 37.6175, "Red Square, Moscow"),
    (35.6586, 139.7454, "Tokyo Tower"),
    (-13.1631, -72.5450, "Machu Picchu, Peru"),
]

# Sampling extent for random points: excludes the extreme polar caps
# (minimal/degenerate OSM coverage above ~85 deg either way) but is
# otherwise the whole planet -- this DB covers 3DEP (US) + Copernicus
# DEM (everywhere else) for terrain and a full-planet OSM import, so
# unlike the original CONUS-only version there's no reason to restrict
# sampling to the US.
WORLD_BBOX = (-180.0, -85.0, 180.0, 85.0)  # min_lon, min_lat, max_lon, max_lat

# Very rough continent bucketing for the summary breakdown only -- not
# meant to be geopolitically precise, just enough to sanity-check that
# random sampling actually landed on multiple continents.
def rough_region(lat, lon):
    if lat > 7 and -170 <= lon <= -50:
        return "North America"
    if lat <= 7 and lon <= -34:
        return "South America"
    if -35 <= lat <= 37 and -20 <= lon <= 52:
        return "Africa"
    if 35 <= lat <= 72 and -25 <= lon <= 45:
        return "Europe"
    if lon > 45 or lon < -170:
        return "Asia/Oceania"
    return "Other"


# Stable, independently-verifiable public facts to check the live DB
# against on every run. These don't change over time, so a mismatch here
# is a real signal of a data problem, not just a note. Every public_value
# below was checked against current published sources (not just recalled
# from memory) rather than trusted as "probably right" -- airport
# elevations especially are precise, low-tolerance numbers where a wrong
# guess would be worse than no check at all. Tolerances are wider where
# independent sources genuinely disagree by more than a few feet (e.g.
# Lhasa, Quito, La Paz -- all extreme/remote high-altitude airports with
# more citation variance than a sea-level city airport), not just as a
# blanket safety margin.
#
# The two obstacle-height facts are inherently US-specific (FAA Digital
# Obstacle File has no global equivalent -- see the obstacles check
# generally). Airport elevations and the runway-length fact are globally
# sourced. The last two facts hit elevation_at_point_ft() directly (see
# TerrainLoader.cpp) rather than the airports table, so they're the only
# ones exercising the terrain raster itself.
GOLDEN_FACTS = [
    {
        "title": "KVLY-TV mast, Blanchard ND",
        "check": lambda pts: pts["kvly"]["obstacles"]["max_agl_ft"],
        "public_value": 2063,  # FCC/FAA public record; one of the tallest man-made structures on Earth
        "unit": "ft AGL",
        "tolerance": 15,
    },
    {
        "title": "Tallest obstacle within 5nm of the Statue of Liberty (One World Trade Center)",
        "check": lambda pts: pts["wtc_area"]["obstacles"]["max_agl_ft"],
        "public_value": 1776,  # One World Trade Center's publicly cited spire height
        "unit": "ft AGL",
        "tolerance": 30,  # antenna/lighting apparatus can sit a bit above the cited architectural height
    },
    {
        "title": "Denver Intl (KDEN) elevation",
        "check": lambda pts: pts["KDEN"]["nearest_airport"]["elevation_ft"],
        "public_value": 5433,  # FAA AIP
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "El Alto Intl, La Paz (SLLP) elevation -- highest international airport in the world",
        "check": lambda pts: pts["SLLP"]["nearest_airport"]["elevation_ft"],
        "public_value": 13325,  # Wikipedia/public record; some source variance at this altitude
        "unit": "ft",
        "tolerance": 40,
    },
    {
        "title": "Lhasa Gonggar (ZULS) elevation",
        "check": lambda pts: pts["ZULS"]["nearest_airport"]["elevation_ft"],
        "public_value": 11713,  # public sources range ~11713-11800ft for this one
        "unit": "ft",
        "tolerance": 100,
    },
    {
        "title": "Leadville/Lake County (KLXV) elevation -- highest public-use airport in North America",
        "check": lambda pts: pts["KLXV"]["nearest_airport"]["elevation_ft"],
        "public_value": 9934,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Telluride Regional (KTEX) elevation -- highest commercial airport in North America",
        "check": lambda pts: pts["KTEX"]["nearest_airport"]["elevation_ft"],
        "public_value": 9078,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Mexico City Benito Juarez (MMMX) elevation",
        "check": lambda pts: pts["MMMX"]["nearest_airport"]["elevation_ft"],
        "public_value": 7316,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Quito Mariscal Sucre (SEQM) elevation",
        "check": lambda pts: pts["SEQM"]["nearest_airport"]["elevation_ft"],
        "public_value": 7841,  # public sources range ~7841-7910ft
        "unit": "ft",
        "tolerance": 40,
    },
    {
        "title": "Amsterdam Schiphol (EHAM) elevation -- below sea level",
        "check": lambda pts: pts["EHAM"]["nearest_airport"]["elevation_ft"],
        "public_value": -11,
        "unit": "ft",
        "tolerance": 5,
    },
    {
        "title": "New Orleans Louis Armstrong (KMSY) elevation",
        "check": lambda pts: pts["KMSY"]["nearest_airport"]["elevation_ft"],
        "public_value": 4,
        "unit": "ft",
        "tolerance": 5,
    },
    {
        "title": "Aspen/Pitkin County (KASE) elevation",
        "check": lambda pts: pts["KASE"]["nearest_airport"]["elevation_ft"],
        "public_value": 7820,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Kathmandu Tribhuvan (VNKT) elevation",
        "check": lambda pts: pts["VNKT"]["nearest_airport"]["elevation_ft"],
        "public_value": 4390,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Qamdo Bangda (ZUBD) elevation -- one of the highest airports in the world",
        "check": lambda pts: pts["ZUBD"]["nearest_airport"]["elevation_ft"],
        "public_value": 14219,
        "unit": "ft",
        "tolerance": 20,
    },
    {
        "title": "Furnace Creek, Death Valley (KL06) elevation -- lowest airport in North America",
        "check": lambda pts: pts["KL06"]["nearest_airport"]["elevation_ft"],
        "public_value": -210,
        "unit": "ft",
        "tolerance": 10,
    },
    {
        "title": "Toronto Pearson (CYYZ) elevation",
        "check": lambda pts: pts["CYYZ"]["nearest_airport"]["elevation_ft"],
        "public_value": 569,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "London Heathrow (EGLL) elevation",
        "check": lambda pts: pts["EGLL"]["nearest_airport"]["elevation_ft"],
        "public_value": 83,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Johannesburg O.R. Tambo (FAOR) elevation",
        "check": lambda pts: pts["FAOR"]["nearest_airport"]["elevation_ft"],
        "public_value": 5558,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Atlanta Hartsfield-Jackson (KATL) elevation -- world's busiest airport",
        "check": lambda pts: pts["KATL"]["nearest_airport"]["elevation_ft"],
        "public_value": 1026,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Paris Charles de Gaulle (LFPG) elevation",
        "check": lambda pts: pts["LFPG"]["nearest_airport"]["elevation_ft"],
        "public_value": 392,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Dubai Intl (OMDB) elevation",
        "check": lambda pts: pts["OMDB"]["nearest_airport"]["elevation_ft"],
        "public_value": 62,
        "unit": "ft",
        "tolerance": 15,
    },
    {
        "title": "Denver Intl (KDEN) runway 16R/34L length -- longest public-use runway in North America",
        "check": lambda pts: pts["kden_16r34l"]["length_ft"],
        "public_value": 16000,
        "unit": "ft",
        "tolerance": 5,
    },
    {
        "title": "Lowest point in the Netherlands, Nieuwerkerk aan den IJssel (terrain raster)",
        "check": lambda pts: pts["nl_lowest"]["elevation_ft"],
        "public_value": -22,  # 6.67m below NAP (~mean sea level)
        "unit": "ft",
        # Wider tolerance than the airport facts: this checks a 30m-resolution
        # DEM pixel against a specific surveyed polder-bottom marker, not an
        # airport's own official reference elevation.
        "tolerance": 25,
    },
    {
        "title": "Mexico City historic center / Zocalo (terrain raster)",
        "check": lambda pts: pts["cdmx_zocalo"]["elevation_ft"],
        "public_value": 7349,
        "unit": "ft",
        # Wide tolerance: cited city elevation genuinely varies ~7316-7350ft
        # across different sources/neighborhoods, not just DEM imprecision.
        "tolerance": 50,
    },
    # ---- Nearest-airport identity checks ----
    # nearest_airport (see query_point()) has no type filter -- it's the
    # single closest row in the whole `airports` table by raw distance,
    # heliports/seaplane bases and all, not "the airport you'd fly into for
    # this city". That makes it genuinely easy to get wrong by memory or by
    # trusting a generic travel-site "nearest airport" answer, which almost
    # always means "nearest airport with scheduled service" instead -- e.g.
    # a naive check would expect Paris Le Bourget (LFPB, ~16km) for the
    # Eiffel Tower, but the actual closest OurAirports entry by distance is
    # a heliport 3-4km away. Verified here via live web search against each
    # candidate's own OurAirports listing (not recalled from memory), and
    # deliberately narrower in scope than the numeric facts above -- only
    # added for landmarks where the closest-by-raw-distance answer could be
    # confirmed with real confidence. (Golden Gate Bridge and the Statue of
    # Liberty were both considered and dropped: the former has a
    # since-1974-closed former Army airfield nearby whose presence/type in
    # OurAirports couldn't be confirmed, and the latter's nearest heliport
    # candidate distance couldn't be pinned down precisely enough -- a
    # wrong guess here would be worse than no check at all, same principle
    # as the numeric facts above.)
    {
        "title": "Nearest airport to the Eiffel Tower (by raw distance, any type)",
        "check": lambda pts: pts["Eiffel Tower, Paris"]["nearest_airport"]["ident"] if pts["Eiffel Tower, Paris"]["nearest_airport"] else None,
        "kind": "ident",
        # Paris Issy-les-Moulineaux Heliport -- an active, OurAirports-listed
        # heliport ~3-4km from the tower, well inside Le Bourget's ~16km
        # (the answer a naive "nearest major airport" search would give).
        "public_value": "LFPI",
    },
    {
        "title": "Nearest airport to the Sydney Opera House (by raw distance, any type)",
        "check": lambda pts: pts["Sydney Opera House"]["nearest_airport"]["ident"] if pts["Sydney Opera House"]["nearest_airport"] else None,
        "kind": "ident",
        # Rose Bay Seaplane Base -- an active, OurAirports-listed water
        # aerodrome a few km from the Opera House, well inside Sydney
        # Kingsford Smith's (SYD) ~9.8km (the "obvious" airport-code answer).
        "public_value": "RSE",
    },
]

# ---- Structural / referential-integrity checks (not per-point) ----
# Run once against the whole database rather than per sampled point --
# these check the database's own internal consistency and known
# regression classes this project has actually hit, so (unlike
# GOLDEN_FACTS) no external ground truth is needed: a mismatch here is
# unambiguously a bug, not a matter of interpretation.

# Every parent+child tag-table pair in the schema (see NavDB.h) -- a
# tags row whose parent id doesn't exist is orphaned (e.g. a
# DeltaApplier bug where a delete cleaned up one table but not the
# other), and a parent row with zero tags rows isn't itself a problem
# (untagged geometry is normal) so this only checks the orphan
# direction, not the reverse.
REFERENTIAL_INTEGRITY_PAIRS = [
    ("way_tags", "ways"),
    ("area_tags", "areas"),
    ("road_tags", "roads"),
    ("relation_tags", "relations"),
    ("node_tags", "nodes"),
]

# Rough sanity floor/ceiling on total row counts -- meant to catch a
# catastrophically incomplete import (died partway through, a `-R`
# resume that silently skipped a phase) even if the point-sampling
# checks above happen to still land on real data, not to precisely
# bound normal variation. `ways` bounds are grounded in this database's
# own last observed full-planet count (~353M as of 2026-07); the others
# don't have as solid a historical baseline from this project
# specifically, so they're deliberately much wider (order-of-magnitude
# "not essentially empty" floors) rather than tuned ranges -- tighten
# them once a few more real full-planet counts have been observed.
ROW_COUNT_BOUNDS = {
    "ways":      (150_000_000, 700_000_000),
    "areas":     (20_000_000, 500_000_000),
    "relations": (1_000_000, 50_000_000),
    # "roads" only holds route=road/highway=* RELATIONS promoted via
    # main.cpp's insertRoad() -- a small curated set, not every tagged
    # highway way (those stay in "ways" only). Confirmed ~300k is the
    # correct order of magnitude for this table, not a shortfall -- the
    # previous bound (5M-150M) wrongly assumed "roads" meant all highways.
    "roads":     (50_000, 2_000_000),
    "nodes":     (20_000_000, 1_000_000_000),  # tagged nodes only, not nodes.dat's ~10.7B populated coordinates
}

# elevation_at_point_ft() coverage spot-checks (see TerrainLoader.cpp),
# split by which DEM source should cover each point -- 3DEP is US +
# territories only, Copernicus DEM GLO-30 is everywhere else. Plain city
# centers/well-known flat land points, deliberately not extreme
# peaks/depressions (already covered by the GOLDEN_FACTS elevation
# facts) -- these only check that a value exists at all, not what it is,
# so the only failure mode being tested for is a coverage gap (this
# project has hit one for real before -- see the code comment on
# nl_lowest/cdmx_zocalo above).
TERRAIN_COVERAGE_POINTS = [
    ("3DEP (US)", 38.9717, -95.2353, "Lawrence, KS (flat prairie)"),
    ("3DEP (US)", 39.9612, -82.9988, "Columbus, OH"),
    ("3DEP (US)", 45.5152, -122.6784, "Portland, OR"),
    ("Copernicus (non-US)", 51.5072, -0.1276, "London, UK"),
    ("Copernicus (non-US)", -1.2921, 36.8219, "Nairobi, Kenya"),
    ("Copernicus (non-US)", 13.7563, 100.5018, "Bangkok, Thailand"),
]

# external_data_state.checked_at staleness thresholds, matching each
# source's own documented refresh cadence in Replicator.cpp (with slack)
# -- these are INFORMATIONAL (reported, not counted toward the hard
# pass/fail exit code, and via run_staleness_checks() specifically kept
# separate from run_structural_checks()): a poll process legitimately
# stopped for maintenance (e.g. a fresh reimport in progress) makes every
# one of these "stale" without anything actually being wrong, so this
# isn't a fact about correctness the way the structural checks are.
STALENESS_THRESHOLDS_HOURS = {
    "osm": 6,            # minutely poll; a few missed cycles is still fine
    "wmm": 24 * 120,      # ~3-month cadence, +30 days slack
    "airspace": 24 * 45,  # ~1-month cadence, +2 weeks slack
    "airports": 30,       # 6-hour check cadence, generous slack
    "faa_obstacles": 30,
}


def read_regional_node_map_header(path):
    """Reads just the 64-byte header of a RegionalNodeMap file (see
    include/RegionalNodeMap.h) without touching the (potentially huge)
    records that follow. Returns None if the file doesn't exist or
    doesn't look like a RegionalNodeMap file at all (wrong magic)."""
    import os
    import struct
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        header = f.read(64)
    if len(header) < 64 or header[0:8] != b"GPSXRNM1":
        return None
    version, = struct.unpack_from("<I", header, 8)
    record_count, = struct.unpack_from("<Q", header, 12)
    region_name = header[20:44].rstrip(b"\x00").decode("utf-8", errors="replace")
    min_lon, min_lat, max_lon, max_lat = struct.unpack_from("<4f", header, 44)
    created_at, = struct.unpack_from("<I", header, 60)
    return {"version": version, "record_count": record_count, "region_name": region_name,
            "bbox": (min_lon, min_lat, max_lon, max_lat), "created_at": created_at}


def run_structural_checks(cur, region_names=None, nodes_file=None):
    """Whole-database structural checks -- see the module comments above
    each constant for what's being verified and why. Returns a list of
    {title, pass, detail} dicts; contributes to the hard pass/fail exit
    code (unlike run_staleness_checks()).

    region_names: non-empty for a regional install (see
    run_regional_checks()/public.installed_regions) -- the row-count
    bounds, MultiPolygon floor, and fixed terrain-coverage points below
    are all tuned for full-planet scale and don't translate to an
    arbitrary region's much smaller data, so those are skipped and two
    region-specific checks (membership, node-file sanity) run instead."""
    results = []

    # -- Referential integrity: no orphaned tag rows -- applies regardless
    # of global vs. regional scope, so this always runs.
    for child, parent in REFERENTIAL_INTEGRITY_PAIRS:
        cur.execute(f"""
            SELECT count(*) FROM {child} c
            WHERE NOT EXISTS (SELECT 1 FROM {parent} p WHERE p.id = c.id)
        """)
        orphans = cur.fetchone()[0]
        results.append({
            "title": f"No orphaned {child} rows (parent {parent} missing)",
            "pass": orphans == 0,
            "detail": "0 orphaned rows" if orphans == 0 else f"{orphans} orphaned {child} row(s) with no matching {parent}.id",
        })

    if not region_names:
        # -- Row-count sanity bounds --
        for table, (lo, hi) in ROW_COUNT_BOUNDS.items():
            cur.execute(f"SELECT count(*) FROM {table}")
            n = cur.fetchone()[0]
            ok = lo <= n <= hi
            results.append({
                "title": f"{table} row count within sanity bounds",
                "pass": ok,
                "detail": f"{n:,} rows (expected {lo:,}-{hi:,})",
            })

        # -- MultiPolygon presence (regression check for the WkbDecode
        # type-6 gap this project hit in production: 764K areas rows,
        # 0.3% of the table, silently excluded from every regional export
        # before the fix -- see WkbDecode.h's own comment on why type 6
        # was added). Areas from multipolygon relations use synthetic
        # negative ids (see NavDB.cpp), so this counts negative-id areas
        # whose geometry is genuinely a MultiPolygon (ST_NumGeometries >
        # 1, not just a single-part polygon that happens to be typed
        # MultiPolygon) -- floor of 10,000 is well below the ~764K
        # historically affected, wide margin for genuine data variation,
        # but far enough above zero to catch "this class of geometry
        # silently isn't decoding again". Full-planet-scale only -- an
        # arbitrary region could have anywhere from zero to millions of
        # multipolygon relations, no floor here would be meaningful.
        cur.execute("""
            SELECT count(*) FROM areas
            WHERE id < 0 AND ST_GeometryType(geog) = 'ST_MultiPolygon' AND ST_NumGeometries(geog) > 1
        """)
        multipolygon_count = cur.fetchone()[0]
        results.append({
            "title": "MultiPolygon relation areas present (WkbDecode type-6 regression check)",
            "pass": multipolygon_count >= 10_000,
            "detail": f"{multipolygon_count:,} genuine multi-part areas from relations (expected >= 10,000)",
        })

        # -- Terrain coverage -- fixed global points, not meaningful for a
        # regional install (which only has terrain loaded for its own
        # area, so these would legitimately show no coverage).
        for source, lat, lon, label in TERRAIN_COVERAGE_POINTS:
            cur.execute("SELECT elevation_at_point_ft(%s, %s)", (lon, lat))
            elev = cur.fetchone()[0]
            results.append({
                "title": f"Terrain coverage: {label} ({source})",
                "pass": elev is not None,
                "detail": f"{elev} ft" if elev is not None else "NULL -- no DEM coverage at this point",
            })
    else:
        # -- Region membership: every row in ways/areas should intersect
        # SOME registered region's polygon. Uses ST_Intersects (not
        # ST_Within) to match the actual export-time inclusion criterion
        # (see RegionIndex.h) -- a way legitimately straddling a region
        # border is correctly included without being fully inside it, so
        # ST_Within would produce false failures for exactly the
        # border-crossing case task #35's whole "fix at the source" effort
        # was about. ORDER BY random() LIMIT is a real cost on a huge
        # table but this tool is explicitly "meant to be rerun
        # occasionally... not part of the build" -- favoring a simple,
        # obviously-correct query here over a cheaper but subtler one
        # (e.g. TABLESAMPLE, which can return zero rows by chance on a
        # small table) is the right trade for an occasional QA tool.
        for table in ("ways", "areas"):
            cur.execute(f"""
                WITH region_union AS (
                    SELECT ST_Union(ST_Transform(ST_SetSRID(ST_GeomFromText(wkt), 4326), 3857)) AS geom
                    FROM installed_regions
                ),
                sample AS (
                    SELECT geog FROM {table} WHERE geog IS NOT NULL ORDER BY random() LIMIT 2000
                )
                SELECT count(*), count(*) FILTER (WHERE NOT ST_Intersects(sample.geog, region_union.geom))
                FROM sample, region_union
            """)
            total, violations = cur.fetchone()
            results.append({
                "title": f"{table}: sampled rows fall within an installed region ({', '.join(region_names)})",
                "pass": total == 0 or violations == 0,
                "detail": (f"no rows to sample" if total == 0 else
                           f"{violations}/{total} sampled row(s) don't intersect any installed region polygon"),
            })

        # -- RegionalNodeMap sanity: the node coordinate file should be
        # non-empty and roughly proportional to the region's own table
        # sizes. No precise expected ratio is established (every way has
        # a different vertex count, and the file is deliberately widened
        # beyond just this region's own nodes -- see RegionalDeltaApplier's
        # top-of-file comment), so this is a deliberately loose floor:
        # only catches "the file is basically empty/truncated", not a
        # precise correctness check.
        if nodes_file is None:
            results.append({
                "title": "RegionalNodeMap sanity",
                "pass": True,
                "detail": "skipped -- no --nodes-file given",
            })
        else:
            header = read_regional_node_map_header(nodes_file)
            if header is None:
                results.append({
                    "title": "RegionalNodeMap sanity",
                    "pass": False,
                    "detail": f"{nodes_file} not found or not a valid RegionalNodeMap file (bad magic)",
                })
            else:
                cur.execute("SELECT (SELECT count(*) FROM ways) + (SELECT count(*) FROM areas)")
                table_rows = cur.fetchone()[0]
                ok = header["record_count"] > 0 and header["record_count"] >= table_rows * 0.5
                results.append({
                    "title": "RegionalNodeMap sanity",
                    "pass": ok,
                    "detail": f"{header['record_count']:,} node record(s) in {nodes_file} vs. "
                              f"{table_rows:,} ways+areas rows (expect node records to be at least half that)",
                })

    return results


def run_staleness_checks(cur):
    """external_data_state freshness -- informational only, see
    STALENESS_THRESHOLDS_HOURS' comment for why this doesn't affect the
    hard pass/fail exit code."""
    results = []
    for name, max_hours in STALENESS_THRESHOLDS_HOURS.items():
        cur.execute("SELECT value, checked_at, EXTRACT(EPOCH FROM (now() - checked_at)) / 3600.0 "
                    "FROM external_data_state WHERE name = %s", (name,))
        row = cur.fetchone()
        if not row:
            results.append({"title": name, "fresh": False, "detail": "no external_data_state row -- never checked"})
            continue
        value, checked_at, age_hours = row
        fresh = age_hours is not None and age_hours <= max_hours
        results.append({
            "title": name,
            "fresh": fresh,
            "detail": f"checked_at={checked_at} ({age_hours:.1f}h ago, threshold {max_hours}h), value={value}",
        })
    return results


def declination(gm, year, lat, lon):
    return gm.calculate(glat=lat, glon=lon, alt=0, time=year).d


def angle_diff(a, b):
    """Smallest angular difference between two bearings, wrapped to [0, 180].

    A plain abs(a - b) is wrong near the +/-180 deg seam: two nearly-identical
    bearings like -179.97 and 179.68 are ~0.36 deg apart on a compass, not the
    ~359.6 deg a naive subtraction gives. Declination legitimately sits near
    +/-180 close to the magnetic poles (confirmed by a genuine Antarctica
    sample point), so without wrapping this shows up as a false ~360 deg
    "discrepancy" between two models that actually agree closely.
    """
    d = abs(a - b) % 360
    return min(d, 360 - d)


def _sample_lat(rng, min_lat, max_lat):
    # Uniform-on-sphere latitude sampling: naive uniform-in-degrees
    # sampling clusters points near the poles (a degree of longitude
    # spans much less true surface area up there), so sample uniformly
    # in sin(latitude) instead and invert.
    s_min = math.sin(math.radians(min_lat))
    s_max = math.sin(math.radians(max_lat))
    return math.degrees(math.asin(rng.uniform(s_min, s_max)))


def build_points(cur, n_random, seed, region_bbox=None):
    """region_bbox: (min_lon, min_lat, max_lon, max_lat) -- when given (a
    regional install, see run_regional_checks()), the global fixed points
    (MAJOR_AIRPORT_IDENTS, minor airports, LANDMARKS) are skipped entirely
    -- they're essentially guaranteed to fall outside a single installed
    region and would just clutter the report with "no coverage" noise --
    and random sampling is restricted to this bbox instead of WORLD_BBOX.
    """
    points = []

    if region_bbox is None:
        cur.execute(
            """
            SELECT ident, name,
                   ST_X(ST_Transform(ST_SetSRID(geog,3857),4326)),
                   ST_Y(ST_Transform(ST_SetSRID(geog,3857),4326))
            FROM airports WHERE ident = ANY(%s)
            """,
            (MAJOR_AIRPORT_IDENTS,),
        )
        for ident, name, lon, lat in cur.fetchall():
            points.append({"lat": lat, "lon": lon, "kind": "major_airport", "label": f"{ident} {name}"})

        cur.execute(
            """
            SELECT ident, name,
                   ST_X(ST_Transform(ST_SetSRID(geog,3857),4326)),
                   ST_Y(ST_Transform(ST_SetSRID(geog,3857),4326))
            FROM airports
            WHERE type='small_airport' AND iso_country='US' AND ident ~ '^K[A-Z0-9]{3}$'
            ORDER BY random() LIMIT 5
            """
        )
        for ident, name, lon, lat in cur.fetchall():
            points.append({"lat": lat, "lon": lon, "kind": "minor_airport", "label": f"{ident} {name}"})

        for lat, lon, label in LANDMARKS:
            points.append({"lat": lat, "lon": lon, "kind": "landmark", "label": label})

    bbox = region_bbox if region_bbox is not None else WORLD_BBOX
    rng = random.Random(seed)
    attempts = 0
    target = len(points) + n_random
    # Global hit-rate (land + OSM way coverage) is much lower than the
    # original CONUS-only version's (~100%, CONUS is essentially fully
    # OSM-mapped) -- oceans, ice caps, and remote unmapped land all
    # reject. 300x gives generous headroom before giving up early. A
    # region_bbox has a much smaller/denser search space than the whole
    # world, so the same multiplier is if anything more generous there.
    max_attempts = n_random * 300
    while len(points) < target and attempts < max_attempts:
        attempts += 1
        lon = rng.uniform(bbox[0], bbox[2])
        lat = _sample_lat(rng, bbox[1], bbox[3])
        cur.execute(
            """
            SELECT count(*) FROM ways
            WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 2000)
            LIMIT 1
            """,
            (lon, lat),
        )
        if cur.fetchone()[0] > 0:
            points.append({"lat": lat, "lon": lon, "kind": "random", "label": f"random #{len(points) - (target - n_random) + 1}"})

    return points, attempts


@contextmanager
def _timed(timings, label):
    """Records one elapsed-time sample for `label` into timings (a label ->
    list-of-seconds dict) -- used to report per-query-category averages
    (see --time-report) without changing what query_point returns."""
    t0 = time.perf_counter()
    yield
    if timings is not None:
        timings[label].append(time.perf_counter() - t0)


def query_point(cur, gm, year, lat, lon, timings=None):
    row = {}

    # This project's own public.* query functions (see AirportsLoader.cpp /
    # FAAObstacleLoader.cpp / AirspaceLoader.cpp / WMMLoader.cpp) rather than
    # hand-rolled SQL here -- these are the same functions the consuming app
    # calls, so dq_check now exercises exactly what production actually
    # uses instead of a parallel/divergent query path.
    with _timed(timings, "nearest_airport"):
        cur.execute("SELECT * FROM nearest_airport(%s, %s)", (lon, lat))
        r = cur.fetchone()
    row["nearest_airport"] = (
        {"ident": r[0], "name": r[1], "type": r[2], "elevation_ft": r[3], "dist_km": round(r[4], 2)}
        if r else None
    )

    # FAA Digital Obstacle File is US + territories only -- zero results
    # elsewhere reflects lack of source data, not a data quality problem.
    with _timed(timings, "faa_obstacles"):
        cur.execute(
            "SELECT count(*), max(amsl_ht), max(agl_ht) FROM obstacles_nearby(%s, %s)",
            (lon, lat),
        )
        r = cur.fetchone()
    row["obstacles"] = {"count": r[0], "max_amsl_ft": r[1], "max_agl_ft": r[2]}

    # airspace_at_point() unions FAA class/special-use airspace and OpenAIP
    # international airspace into one call -- split back out by source here
    # only because render_html's fmt_airspace() renders each source with
    # slightly different chip styling, not because the query needs it.
    with _timed(timings, "airspace_at_point"):
        cur.execute(
            "SELECT source, class, type, name, country FROM airspace_at_point(%s, %s)",
            (lon, lat),
        )
        airspace_rows = cur.fetchall()
    row["class_airspace"] = [
        {"class": a[1], "type": a[2], "name": a[3]} for a in airspace_rows if a[0] == "FAA_CLASS"
    ][:5]
    row["sua"] = [
        {"type": a[2], "name": a[3]} for a in airspace_rows if a[0] == "FAA_SUA"
    ][:5]
    # class here is already the decoded ICAO letter (A-G) -- airspace_at_point
    # does that decode in SQL now, not ICAO_CLASS_LABELS in Python.
    row["intl_airspace"] = [
        {"name": a[3], "class": a[1], "country": a[4]} for a in airspace_rows if a[0] == "OPENAIP"
    ][:5]

    with _timed(timings, "wmm_db_lookup"):
        cur.execute("SELECT declination_at_point(%s, %s)", (lon, lat))
        r = cur.fetchone()
    db_decl = r[0] if r else None
    with _timed(timings, "wmm_model_calc"):
        model_decl = declination(gm, year, lat, lon)
    row["wmm"] = {
        "db_declination": round(db_decl, 3) if db_decl is not None else None,
        "model_declination": round(model_decl, 3),
        "diff": round(angle_diff(db_decl, model_decl), 3) if db_decl is not None else None,
    }

    with _timed(timings, "ways"):
        cur.execute(
            """
            SELECT name, count(*) FROM ways
            WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 500) AND name IS NOT NULL
            GROUP BY name ORDER BY count(*) DESC LIMIT 3
            """,
            (lon, lat),
        )
        row["ways"] = [{"name": a, "n": b} for a, b in cur.fetchall()]
        cur.execute(
            "SELECT count(*) FROM ways WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 500)",
            (lon, lat),
        )
        row["ways_total"] = cur.fetchone()[0]

    with _timed(timings, "areas"):
        cur.execute(
            """
            SELECT name, count(*) FROM areas
            WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 500) AND name IS NOT NULL
            GROUP BY name ORDER BY count(*) DESC LIMIT 3
            """,
            (lon, lat),
        )
        row["areas"] = [{"name": a, "n": b} for a, b in cur.fetchall()]
        cur.execute(
            "SELECT count(*) FROM areas WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 500)",
            (lon, lat),
        )
        row["areas_total"] = cur.fetchone()[0]

    # Coverage-only, not accuracy -- checked at every sampled point (unlike
    # the fixed golden-fact/TERRAIN_COVERAGE_POINTS spot-checks elsewhere,
    # which verify a handful of specific known elevations). A NULL here is
    # expected and not a bug for points over open ocean (Copernicus/3DEP
    # tiles simply don't exist there) -- see aggregate reporting in main().
    with _timed(timings, "terrain_elevation"):
        cur.execute("SELECT elevation_at_point_ft(%s, %s)", (lon, lat))
        r = cur.fetchone()
    row["terrain_elevation_ft"] = r[0] if r and r[0] is not None else None

    return row


def esc(s):
    if s is None:
        return ""
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


KIND_LABEL = {"major_airport": "Major airport", "minor_airport": "Minor airport", "landmark": "Landmark", "random": "Random point"}


def select_table_rows(results, max_rows):
    """Pick which rows the HTML table renders when there are more results
    than max_rows. Fixed points (airports/landmarks) always make the cut;
    random points are prioritized by "interesting-ness" (WMM outliers,
    zero-coverage points) then filled out with an evenly-spaced subsample
    for geographic spread. Returns (rows_to_show, n_random_omitted)."""
    fixed = [r for r in results if r["kind"] != "random"]
    randoms = [r for r in results if r["kind"] == "random"]
    if len(fixed) + len(randoms) <= max_rows:
        return results, 0

    budget = max(0, max_rows - len(fixed))
    if budget >= len(randoms):
        return results, 0

    def interesting(r):
        d = r["wmm"]["diff"]
        flags = 0
        if d is not None and d > 0.15:
            flags += 2
        if r["ways_total"] == 0 and r["areas_total"] == 0:
            flags += 1
        return flags

    scored = sorted(randoms, key=interesting, reverse=True)
    n_flagged = sum(1 for r in scored if interesting(r) > 0)
    flagged = scored[:min(n_flagged, budget)]
    remaining_budget = budget - len(flagged)

    rest = [r for r in randoms if r not in flagged]
    if remaining_budget > 0 and rest:
        step = max(1, len(rest) // remaining_budget)
        subsample = rest[::step][:remaining_budget]
    else:
        subsample = []

    shown_randoms = flagged + subsample
    shown_idx = {r["idx"] for r in shown_randoms}
    shown = [r for r in results if r["kind"] != "random" or r["idx"] in shown_idx]
    shown.sort(key=lambda r: r["idx"])
    return shown, len(randoms) - len(shown_randoms)


def render_html(results, golden_results, structural_results, staleness_results, timing_stats,
                mean_diff, max_diff, max_table_rows):
    def fmt_airspace(row):
        parts = []
        seen = set()
        for a in row["class_airspace"]:
            key = ("faa", a["class"], a["name"])
            if key in seen:
                continue
            seen.add(key)
            parts.append(f'<span class="chip chip-cls">{esc(a["class"] or "—")}</span> {esc(a["name"])}')
        for s in row["sua"]:
            parts.append(f'<span class="chip chip-sua">{esc(s["type"])}</span> {esc(s["name"])}')
        for a in row.get("intl_airspace", []):
            # "class" here is already the decoded ICAO letter (A-G) --
            # airspace_at_point() does that decode in SQL now.
            parts.append(f'<span class="chip chip-cls">{esc(a["class"] or "?")}</span> {esc(a["name"])} <span class="muted small">({esc(a["country"])})</span>')
        return "<br>".join(parts) if parts else '<span class="muted">none charted (Class G)</span>'

    def fmt_names(row, key):
        items, total = row[key], row[f"{key}_total"]
        if not items:
            return f'<span class="muted">{total} unnamed</span>' if total else '<span class="muted">none within 500m</span>'
        names = ", ".join(esc(x["name"]) for x in items if x["name"])
        return f'{names} <span class="muted">({total} total)</span>'

    def fmt_obstacles(row):
        o = row["obstacles"]
        if not o["count"]:
            return '<span class="muted">none within 5nm</span>'
        return f'{o["count"]} within 5nm, tallest {o["max_agl_ft"]}ft AGL / {o["max_amsl_ft"]}ft AMSL'

    def fmt_airport(row):
        a = row["nearest_airport"]
        if not a:
            return "—"
        return f'{esc(a["ident"])} {esc(a["name"])} <span class="muted">({a["type"]}, {a["dist_km"]}km)</span>'

    shown, n_omitted = select_table_rows(results, max_table_rows)

    rows_html = []
    for r in shown:
        cls = "pass" if r["wmm"]["diff"] is not None and r["wmm"]["diff"] <= 0.1 else "warn"
        rows_html.append(f"""
        <tr data-kind="{r['kind']}">
          <td class="mono">{r['idx']+1}</td>
          <td><span class="kind-tag kind-{r['kind']}">{KIND_LABEL[r['kind']]}</span><div class="label">{esc(r['label'])}</div><div class="muted mono small">{r['lat']:.4f}, {r['lon']:.4f}</div></td>
          <td>{fmt_airport(r)}</td>
          <td>{fmt_obstacles(r)}</td>
          <td>{fmt_airspace(r)}</td>
          <td class="mono">{r['wmm']['db_declination']}&deg; <span class="chip chip-{cls}">&Delta;{r['wmm']['diff']:.3f}&deg;</span></td>
          <td>{fmt_names(r, 'ways')}</td>
          <td>{fmt_names(r, 'areas')}</td>
        </tr>""")

    golden_html = []
    for g in golden_results:
        v = "match" if g["pass"] else "fail"
        vlabel = "MATCH" if g["pass"] else "MISMATCH"
        tolerance_suffix = f" (&plusmn;{g['tolerance']})" if g["tolerance"] is not None else ""
        golden_html.append(f"""
        <div class="spot-card">
          <div class="spot-head">
            <span class="spot-verdict spot-{v}">{vlabel}</span>
            <h3>{esc(g['title'])}</h3>
          </div>
          <div class="spot-row"><span class="spot-k">Our data</span><span class="spot-v mono">{esc(str(g['actual']))} {g['unit']}</span></div>
          <div class="spot-row"><span class="spot-k">Public record</span><span class="spot-v mono">{esc(str(g['public_value']))} {g['unit']}{tolerance_suffix}</span></div>
        </div>""")

    structural_html = []
    for s in structural_results:
        v = "match" if s["pass"] else "fail"
        vlabel = "PASS" if s["pass"] else "FAIL"
        structural_html.append(f"""
        <div class="spot-card">
          <div class="spot-head">
            <span class="spot-verdict spot-{v}">{vlabel}</span>
            <h3>{esc(s['title'])}</h3>
          </div>
          <div class="spot-row"><span class="spot-k">Detail</span><span class="spot-v mono">{esc(s['detail'])}</span></div>
        </div>""")

    staleness_html = []
    for s in staleness_results:
        v = "match" if s["fresh"] else "fail"
        vlabel = "FRESH" if s["fresh"] else "STALE"
        staleness_html.append(f"""
        <div class="spot-card">
          <div class="spot-head">
            <span class="spot-verdict spot-{v}">{vlabel}</span>
            <h3>{esc(s['title'])}</h3>
          </div>
          <div class="spot-row"><span class="spot-k">Detail</span><span class="spot-v mono">{esc(s['detail'])}</span></div>
        </div>""")

    timing_rows_html = "".join(f"""
        <tr><td class="mono">{esc(t['label'])}</td>
            <td class="mono">{t['avg_ms']:.2f} ms</td>
            <td class="mono">{t['total_s']:.2f} s</td>
            <td class="mono">{t['count']}</td></tr>"""
        for t in timing_stats)
    total_query_s = round(sum(t["total_s"] for t in timing_stats), 2)

    major = [r for r in results if r["kind"] == "major_airport"]
    minor = [r for r in results if r["kind"] == "minor_airport"]
    landmarks = [r for r in results if r["kind"] == "landmark"]
    random_pts = [r for r in results if r["kind"] == "random"]
    class_b = sum(1 for r in major if any(a["class"] == "B" for a in r["class_airspace"]))
    no_class = sum(1 for r in results if not r["class_airspace"] and not r.get("intl_airspace"))
    terrain_covered = sum(1 for r in results if r.get("terrain_elevation_ft") is not None)
    golden_pass = sum(1 for g in golden_results if g["pass"])
    structural_pass = sum(1 for s in structural_results if s["pass"])
    staleness_fresh = sum(1 for s in staleness_results if s["fresh"])

    region_counts = {}
    for r in random_pts:
        reg = rough_region(r["lat"], r["lon"])
        region_counts[reg] = region_counts.get(reg, 0) + 1
    region_stats_html = "".join(
        f'<div class="stat"><span class="n">{n}</span><span class="lbl">{esc(reg)} (random pts)</span></div>'
        for reg, n in sorted(region_counts.items(), key=lambda kv: -kv[1])
    )

    table_note = (
        f'<p class="muted small">Showing {len(shown)} of {len(results)} points '
        f'({n_omitted} random points omitted from this table for size -- prioritized by WMM '
        f'deviation and zero-coverage flags, then an even geographic subsample; pass --json for '
        f'the complete dataset).</p>'
        if n_omitted else ""
    )

    return f"""<title>Data Quality Check — nav database</title>
<style>
:root {{
  --bg: #f4f5f2; --surface: #ffffff; --surface-2: #eceee9; --text: #171b21;
  --text-muted: #5b6572; --accent: #a86a1c; --accent-soft: #f1e2cc;
  --pass: #2f7a4d; --pass-soft: #e3f0e7; --warn: #b3392f; --warn-soft: #f6e2df;
  --border: #d8dbd4; --sua-bg: #e5d9ee; --sua-text: #5b3a7a;
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    --bg: #11151c; --surface: #1a212b; --surface-2: #212a36; --text: #e7ecf2;
    --text-muted: #8e9bad; --accent: #e0a544; --accent-soft: #3a2e18;
    --pass: #4f9d6e; --pass-soft: #1c2b21; --warn: #d1685f; --warn-soft: #2f1f1e;
    --border: #2a3441; --sua-bg: #2a2140; --sua-text: #c9aeee;
  }}
}}
:root[data-theme="dark"] {{
  --bg: #11151c; --surface: #1a212b; --surface-2: #212a36; --text: #e7ecf2;
  --text-muted: #8e9bad; --accent: #e0a544; --accent-soft: #3a2e18;
  --pass: #4f9d6e; --pass-soft: #1c2b21; --warn: #d1685f; --warn-soft: #2f1f1e;
  --border: #2a3441; --sua-bg: #2a2140; --sua-text: #c9aeee;
}}
:root[data-theme="light"] {{
  --bg: #f4f5f2; --surface: #ffffff; --surface-2: #eceee9; --text: #171b21;
  --text-muted: #5b6572; --accent: #a86a1c; --accent-soft: #f1e2cc;
  --pass: #2f7a4d; --pass-soft: #e3f0e7; --warn: #b3392f; --warn-soft: #f6e2df;
  --border: #d8dbd4; --sua-bg: #e5d9ee; --sua-text: #5b3a7a;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--text); font-family: -apple-system, "Segoe UI", "Helvetica Neue", Arial, sans-serif; line-height: 1.5; }}
.mono {{ font-family: ui-monospace, "SF Mono", "Cascadia Code", "DejaVu Sans Mono", monospace; font-variant-numeric: tabular-nums; }}
.small {{ font-size: 0.78rem; }}
.muted {{ color: var(--text-muted); }}
.wrap {{ max-width: 1180px; margin: 0 auto; padding: 2.5rem 1.5rem 5rem; }}
header {{ margin-bottom: 2.25rem; }}
.eyebrow {{ font-family: ui-monospace, monospace; font-size: 0.72rem; letter-spacing: 0.14em; text-transform: uppercase; color: var(--accent); margin: 0 0 0.6rem; }}
h1 {{ font-size: clamp(1.6rem, 3vw, 2.15rem); margin: 0 0 0.5rem; letter-spacing: -0.01em; text-wrap: balance; }}
.subhead {{ color: var(--text-muted); max-width: 62ch; font-size: 0.98rem; }}
.stats {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1px; background: var(--border); border: 1px solid var(--border); border-radius: 10px; overflow: hidden; margin: 1.75rem 0 2.5rem; }}
.stat {{ background: var(--surface); padding: 1.1rem 1.2rem; }}
.stat .n {{ font-family: ui-monospace, monospace; font-size: 1.65rem; font-weight: 700; color: var(--accent); display: block; line-height: 1.1; }}
.stat .lbl {{ font-size: 0.78rem; color: var(--text-muted); margin-top: 0.3rem; }}
section {{ margin-bottom: 2.75rem; }}
h2 {{ font-size: 1.05rem; text-transform: uppercase; letter-spacing: 0.08em; font-weight: 700; border-bottom: 1px solid var(--border); padding-bottom: 0.6rem; margin: 0 0 1.2rem; }}
h2 .count {{ color: var(--text-muted); font-weight: 400; text-transform: none; letter-spacing: normal; font-size: 0.85rem; }}
.spot-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 1rem; }}
.spot-card {{ background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 1.1rem 1.2rem; }}
.spot-head {{ display: flex; align-items: baseline; gap: 0.6rem; margin-bottom: 0.75rem; }}
.spot-head h3 {{ font-size: 0.95rem; margin: 0; text-wrap: balance; }}
.spot-verdict {{ font-family: ui-monospace, monospace; font-size: 0.65rem; letter-spacing: 0.06em; padding: 0.15rem 0.45rem; border-radius: 4px; white-space: nowrap; flex-shrink: 0; }}
.spot-match {{ background: var(--pass-soft); color: var(--pass); }}
.spot-fail {{ background: var(--warn-soft); color: var(--warn); }}
.spot-row {{ display: flex; gap: 0.75rem; font-size: 0.85rem; padding: 0.3rem 0; border-top: 1px solid var(--border); }}
.spot-row:first-of-type {{ border-top: none; }}
.spot-k {{ color: var(--text-muted); flex: 0 0 6.5rem; font-size: 0.72rem; text-transform: uppercase; letter-spacing: 0.04em; padding-top: 0.15rem; }}
.spot-v {{ flex: 1; }}
.table-scroll {{ overflow-x: auto; border: 1px solid var(--border); border-radius: 10px; }}
table {{ border-collapse: collapse; width: 100%; min-width: 1080px; font-size: 0.83rem; background: var(--surface); }}
thead th {{ text-align: left; font-size: 0.7rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-muted); background: var(--surface-2); padding: 0.65rem 0.8rem; position: sticky; top: 0; border-bottom: 1px solid var(--border); }}
tbody td {{ padding: 0.65rem 0.8rem; vertical-align: top; border-bottom: 1px solid var(--border); }}
tbody tr:last-child td {{ border-bottom: none; }}
tbody tr:hover {{ background: var(--surface-2); }}
.label {{ font-weight: 600; margin-top: 0.1rem; }}
.kind-tag {{ font-size: 0.65rem; text-transform: uppercase; letter-spacing: 0.04em; padding: 0.1rem 0.4rem; border-radius: 4px; font-weight: 600; }}
.kind-major_airport {{ background: var(--accent-soft); color: var(--accent); }}
.kind-minor_airport {{ background: var(--pass-soft); color: var(--pass); }}
.kind-landmark {{ background: var(--sua-bg); color: var(--sua-text); }}
.kind-random {{ background: var(--surface-2); color: var(--text-muted); }}
.chip {{ display: inline-block; font-size: 0.68rem; font-family: ui-monospace, monospace; padding: 0.05rem 0.35rem; border-radius: 4px; margin-right: 0.3rem; }}
.chip-cls {{ background: var(--accent-soft); color: var(--accent); }}
.chip-sua {{ background: var(--sua-bg); color: var(--sua-text); }}
.chip-pass {{ background: var(--pass-soft); color: var(--pass); }}
.chip-warn {{ background: var(--warn-soft); color: var(--warn); }}
.filters {{ display: flex; gap: 0.5rem; margin-bottom: 1rem; flex-wrap: wrap; }}
.filter-btn {{ font-family: inherit; font-size: 0.78rem; padding: 0.4rem 0.85rem; border-radius: 999px; border: 1px solid var(--border); background: var(--surface); color: var(--text); cursor: pointer; }}
.filter-btn:hover {{ border-color: var(--accent); }}
.filter-btn.active {{ background: var(--accent); color: var(--surface); border-color: var(--accent); }}
.filter-btn:focus-visible {{ outline: 2px solid var(--accent); outline-offset: 2px; }}
footer {{ color: var(--text-muted); font-size: 0.8rem; border-top: 1px solid var(--border); padding-top: 1.25rem; }}
</style>
<div class="wrap">
  <header>
    <p class="eyebrow">gpsxdb &middot; nav database data quality check</p>
    <h1>Spot check against public records</h1>
    <p class="subhead">{len(results)} points sampled globally, checked against airports, obstacles (FAA, US only),
    charted airspace (FAA class/SUA in the US, OpenAIP elsewhere), magnetic declination (WMM), roads, and
    land-use areas, then cross-referenced against publicly known facts.</p>
  </header>
  <div class="stats">
    <div class="stat"><span class="n">{len(results)}</span><span class="lbl">Points checked</span></div>
    <div class="stat"><span class="n">{class_b} / {len(major)}</span><span class="lbl">Major hubs correctly Class B</span></div>
    <div class="stat"><span class="n">{mean_diff:.3f}&deg;</span><span class="lbl">Mean WMM deviation</span></div>
    <div class="stat"><span class="n">{max_diff:.3f}&deg;</span><span class="lbl">Max WMM deviation</span></div>
    <div class="stat"><span class="n">{no_class}</span><span class="lbl">Points correctly uncharted (Class G)</span></div>
    <div class="stat"><span class="n">{terrain_covered} / {len(results)}</span><span class="lbl">Points with terrain elevation data<br><span class="muted small">(gaps over open ocean are expected, not a bug)</span></span></div>
    <div class="stat"><span class="n">{golden_pass} / {len(golden_results)}</span><span class="lbl">Golden-fact checks passed</span></div>
    <div class="stat"><span class="n">{structural_pass} / {len(structural_results)}</span><span class="lbl">Structural checks passed</span></div>
    <div class="stat"><span class="n">{staleness_fresh} / {len(staleness_results)}</span><span class="lbl">External data fresh</span></div>
    {region_stats_html}
  </div>
  <section>
    <h2>Golden-fact checks</h2>
    <div class="spot-grid">
      {"".join(golden_html)}
    </div>
  </section>
  <section>
    <h2>Structural checks</h2>
    <p class="muted small">Whole-database checks, not tied to a sampled point -- referential integrity, row-count sanity, MultiPolygon regression coverage, and terrain DEM coverage. Contributes to this run's pass/fail exit code.</p>
    <div class="spot-grid">
      {"".join(structural_html)}
    </div>
  </section>
  <section>
    <h2>External data staleness</h2>
    <p class="muted small">Informational only -- does not affect this run's pass/fail exit code. A poll process intentionally stopped for maintenance (e.g. a fresh reimport in progress) will show every source as stale here without anything being wrong.</p>
    <div class="spot-grid">
      {"".join(staleness_html)}
    </div>
  </section>
  <section>
    <h2>Query performance <span class="count">&mdash; {total_query_s:.1f}s total across {len(results)} points</span></h2>
    <p class="muted small">Average/total wall time per query category (includes network round-trip to the DB) -- a category that's unexpectedly slow relative to the others usually means a missing index after a fresh import, not a slow query itself.</p>
    <div class="table-scroll">
      <table>
        <thead><tr><th>Category</th><th>Avg</th><th>Total</th><th>Samples</th></tr></thead>
        <tbody>
          {timing_rows_html}
        </tbody>
      </table>
    </div>
  </section>
  <section>
    <h2>All {len(results)} points <span class="count">&mdash; {len(major)} major airports, {len(minor)} minor airports, {len(landmarks)} landmarks, {len(random_pts)} random global points</span></h2>
    {table_note}
    <div class="filters">
      <button class="filter-btn active" data-filter="all">All ({len(shown)})</button>
      <button class="filter-btn" data-filter="major_airport">Major airports ({len(major)})</button>
      <button class="filter-btn" data-filter="minor_airport">Minor airports ({len(minor)})</button>
      <button class="filter-btn" data-filter="landmark">Landmarks ({len(landmarks)})</button>
      <button class="filter-btn" data-filter="random">Random ({sum(1 for r in shown if r['kind']=='random')})</button>
    </div>
    <div class="table-scroll">
      <table id="results">
        <thead><tr><th>#</th><th>Point</th><th>Nearest airport</th><th>Obstacles</th><th>Charted airspace</th><th>WMM declination</th><th>Ways (500m)</th><th>Areas (500m)</th></tr></thead>
        <tbody>{"".join(rows_html)}</tbody>
      </table>
    </div>
  </section>
  <footer>Generated by dq_check.py against the live <span class="mono">nav</span> database.</footer>
</div>
<script>
document.querySelectorAll('.filter-btn').forEach(btn => {{
  btn.addEventListener('click', () => {{
    document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    const f = btn.dataset.filter;
    document.querySelectorAll('#results tbody tr').forEach(tr => {{
      tr.style.display = (f === 'all' || tr.dataset.kind === f) ? '' : 'none';
    }});
  }});
}});
</script>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--server", default="server")
    ap.add_argument("-d", "--database", default="nav")
    ap.add_argument("-u", "--user", default="daniel")
    ap.add_argument("-p", "--password", default=None)
    ap.add_argument("--n-random", type=int, default=10000, help="random global points, on top of the fixed major/minor airports and landmarks (default 10000)")
    ap.add_argument("--seed", type=int, default=None, help="seed for random point selection (omit for a fresh sample each run)")
    ap.add_argument("--year", type=float, default=None, help="decimal year for WMM cross-check (default: today)")
    ap.add_argument("--max-table-rows", type=int, default=500, help="cap on individual rows rendered in the HTML table (default 500); fixed points always shown in full, random points prioritized by interestingness then subsampled -- full data is always in --json regardless")
    ap.add_argument("-o", "--output", default="dq_report.html")
    ap.add_argument("--json", default=None, help="also write raw per-point results as JSON to this path")
    ap.add_argument("--nodes-file", default=None, help="path to this install's RegionalNodeMap file (see --nodes-file in regional_install.cpp) -- only used for the RegionalNodeMap sanity check on a regional install; ignored against the master database")
    args = ap.parse_args()

    import time as _time
    year = args.year
    if year is None:
        t = _time.gmtime()
        year = t.tm_year + t.tm_yday / 365.25

    conn = psycopg2.connect(host=args.server, dbname=args.database, user=args.user,
                             password=args.password) if args.password else \
           psycopg2.connect(host=args.server, dbname=args.database, user=args.user)
    conn.autocommit = True
    cur = conn.cursor()
    gm = GeoMag()  # defaults to the bundled current WMM coefficients

    # Auto-detect a regional install exactly like RegionalDeltaApplier does
    # (see main.cpp's runDelta()) -- any rows in public.installed_regions
    # mean this is a region-scoped database, not the master planet DB.
    # Tolerant of the table not existing at all (a master DB created before
    # this table was added), matching NavDB::loadInstalledRegions()'s own
    # fallback behavior.
    region_names = []
    region_bbox = None
    try:
        cur.execute("SELECT name, min_lon, min_lat, max_lon, max_lat FROM installed_regions")
        rows = cur.fetchall()
        if rows:
            region_names = [r[0] for r in rows]
            region_bbox = (
                min(r[1] for r in rows), min(r[2] for r in rows),
                max(r[3] for r in rows), max(r[4] for r in rows),
            )
    except Exception:
        conn.rollback()  # failed SELECT leaves the (autocommit-off-during-txn) connection in a bad state otherwise

    if region_names:
        print(f"Regional install detected: {', '.join(region_names)} -- restricting sampling to this region "
              f"and skipping full-planet-scale checks (golden facts, row-count bounds, MultiPolygon/terrain checks)",
              file=sys.stderr)

    points, attempts = build_points(cur, args.n_random, args.seed, region_bbox=region_bbox)
    print(f"Checking {len(points)} points ({attempts} attempts to fill random quota)...", file=sys.stderr)

    results = []
    by_label = {}
    query_timings = defaultdict(list)
    progress_every = 200 if len(points) > 1000 else 20
    for i, p in enumerate(points):
        row = {"idx": i, "lat": round(p["lat"], 5), "lon": round(p["lon"], 5), "kind": p["kind"], "label": p["label"]}
        row.update(query_point(cur, gm, year, p["lat"], p["lon"], timings=query_timings))
        results.append(row)
        by_label[p["label"]] = row
        if (i + 1) % progress_every == 0:
            print(f"  {i+1}/{len(points)}", file=sys.stderr)

    # Per-category average/total time across all points -- lets a slow
    # query category (e.g. a missing index after a fresh import) stand out
    # instead of only seeing the run's overall wall time.
    timing_stats = []
    for label, samples in query_timings.items():
        timing_stats.append({
            "label": label,
            "count": len(samples),
            "avg_ms": round(1000 * sum(samples) / len(samples), 2) if samples else 0.0,
            "total_s": round(sum(samples), 2),
        })
    timing_stats.sort(key=lambda t: t["total_s"], reverse=True)
    print("Average query time by category:", file=sys.stderr)
    for t in timing_stats:
        print(f"  {t['label']:<24} avg={t['avg_ms']:>8.2f}ms  total={t['total_s']:>8.2f}s  n={t['count']}",
              file=sys.stderr)

    golden_results = []
    all_golden_pass = True
    # GOLDEN_FACTS are all tied to specific global airports/landmarks that
    # build_points() doesn't sample at all on a regional install (see
    # region_bbox above) -- nothing to check them against.
    if not region_names:
        kvly = next(r for r in results if "KVLY" in r["label"])
        wtc_area = next(r for r in results if "Statue of Liberty" in r["label"])
        # Every major_airport point's label is "IDENT Name", so any airport in
        # MAJOR_AIRPORT_IDENTS is reachable by ident for a GOLDEN_FACTS check
        # without needing a dedicated next(...) lookup per airport.
        pts_by_key = {"kvly": kvly, "wtc_area": wtc_area}
        for r in results:
            if r["kind"] == "major_airport":
                pts_by_key[r["label"].split(" ", 1)[0]] = r
        # Every point (including every LANDMARKS entry) is also reachable by its
        # exact label -- needed for the nearest-airport ident checks below,
        # which reference landmarks by their full LANDMARKS label rather than
        # a short custom key like "kvly"/"wtc_area".
        pts_by_key.update(by_label)

        # A few golden facts need data outside the standard per-point query
        # (run once here rather than adding a runway lookup / raster call to
        # every one of the thousands of sampled points above).
        cur.execute("SELECT length_ft FROM runways WHERE airport_ident='KDEN' AND le_ident='16R' AND he_ident='34L'")
        r = cur.fetchone()
        pts_by_key["kden_16r34l"] = {"length_ft": r[0] if r else None}

        # Direct raster elevation lookups -- exercises elevation_at_point_ft()
        # (see TerrainLoader.cpp) independently of the airport/obstacle
        # point-sampling above, at well-documented, DEM-friendly (flat, stable,
        # not an extreme peak) ground elevations. Death Valley/Denver were the
        # first choice here (see GOLDEN_FACTS below) but terrain coverage turns
        # out to have a real gap over the CONUS interior -- confirmed via a
        # direct raster query (zero rows in a Colorado bounding box) rather than
        # assumed, so these two points were picked specifically because they're
        # in regions already loaded (Mexico, Europe).
        cur.execute("SELECT elevation_at_point_ft(%s, %s)", (4.6317, 51.9858))
        pts_by_key["nl_lowest"] = {"elevation_ft": cur.fetchone()[0]}
        cur.execute("SELECT elevation_at_point_ft(%s, %s)", (-99.13316, 19.43263))
        pts_by_key["cdmx_zocalo"] = {"elevation_ft": cur.fetchone()[0]}

        for g in GOLDEN_FACTS:
            actual = g["check"](pts_by_key)
            if g.get("kind") == "ident":
                ok = actual is not None and actual == g["public_value"]
                unit, tolerance = "", None
            else:
                ok = actual is not None and abs(actual - g["public_value"]) <= g["tolerance"]
                unit, tolerance = g["unit"], g["tolerance"]
            all_golden_pass &= ok
            golden_results.append({"title": g["title"], "actual": actual, "public_value": g["public_value"],
                                    "unit": unit, "tolerance": tolerance, "pass": ok})

    diffs = [r["wmm"]["diff"] for r in results if r["wmm"]["diff"] is not None]
    mean_diff = sum(diffs) / len(diffs) if diffs else 0.0
    max_diff = max(diffs) if diffs else 0.0

    structural_results = run_structural_checks(cur, region_names=region_names, nodes_file=args.nodes_file)
    all_structural_pass = all(s["pass"] for s in structural_results)
    staleness_results = run_staleness_checks(cur)

    html = render_html(results, golden_results, structural_results, staleness_results, timing_stats,
                        mean_diff, max_diff, args.max_table_rows)
    with open(args.output, "w") as f:
        f.write(html)
    print(f"Wrote {args.output}", file=sys.stderr)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)
        print(f"Wrote {args.json}", file=sys.stderr)

    print(f"\nGolden-fact checks: {sum(g['pass'] for g in golden_results)}/{len(golden_results)} passed", file=sys.stderr)
    for g in golden_results:
        status = "PASS" if g["pass"] else "FAIL"
        tolerance_suffix = f" +/-{g['tolerance']}" if g["tolerance"] is not None else ""
        print(f"  [{status}] {g['title']}: {g['actual']} {g['unit']} (public record: {g['public_value']}{tolerance_suffix})", file=sys.stderr)
    print(f"WMM: mean |diff|={mean_diff:.3f} deg, max |diff|={max_diff:.3f} deg (vs independent pygeomag model)", file=sys.stderr)

    print(f"\nStructural checks: {sum(s['pass'] for s in structural_results)}/{len(structural_results)} passed", file=sys.stderr)
    for s in structural_results:
        status = "PASS" if s["pass"] else "FAIL"
        print(f"  [{status}] {s['title']}: {s['detail']}", file=sys.stderr)

    print(f"\nExternal-data staleness (informational, not counted toward exit code):", file=sys.stderr)
    for s in staleness_results:
        status = "FRESH" if s["fresh"] else "STALE"
        print(f"  [{status}] {s['title']}: {s['detail']}", file=sys.stderr)

    sys.exit(0 if (all_golden_pass and all_structural_pass) else 1)


if __name__ == "__main__":
    main()
