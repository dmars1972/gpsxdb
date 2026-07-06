#!/usr/bin/env python3
"""Data quality spot-check for the nav database.

Samples points across the continental US (a fixed set of major/minor
airports and named landmarks, plus N random points rejection-sampled to
real OSM coverage) and checks six data layers against each: nearest
airport, FAA obstacles, charted airspace (class + special use), WMM
magnetic declination, nearby roads (ways), and nearby land-use areas
(areas). Cross-checks WMM declination against pygeomag (an independent
NOAA WMM implementation, not this project's own WMMLoader.cpp) and a
handful of stable, well-known public facts (see GOLDEN_FACTS below) so a
future bad import/regression has a chance of being caught even without a
human eyeballing the SQL results.

Meant to be rerun occasionally after a fresh import (or on a schedule),
not part of the build. Writes an HTML report; pass --json to also dump
the raw per-point results.

Requires: psycopg2-binary, pygeomag (see requirements-dq_check.txt)
"""
import argparse
import json
import random
import sys

import psycopg2
from pygeomag import GeoMag

# ---- Fixed sample points ----

MAJOR_AIRPORT_IDENTS = ["KATL", "KORD", "KLAX", "KJFK", "KDFW"]

LANDMARKS = [
    (38.8977, -77.0365, "White House, Washington DC"),
    (37.8199, -122.4783, "Golden Gate Bridge, San Francisco"),
    (40.6892, -74.0445, "Statue of Liberty, NYC"),
    (47.2530, -97.2905, "KVLY-TV mast, Blanchard ND (tall-tower obstacle check)"),
]

CONUS_BBOX = (-124.7, 24.5, -66.9, 49.4)  # min_lon, min_lat, max_lon, max_lat

# Stable, independently-verifiable public facts to check the live DB
# against on every run. These don't change over time, so a mismatch here
# is a real signal of a data problem, not just a note. Sources are the
# comments beside each value, not this codebase.
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
]


def declination(gm, year, lat, lon):
    return gm.calculate(glat=lat, glon=lon, alt=0, time=year).d


def build_points(cur, n_random, seed):
    points = []

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

    rng = random.Random(seed)
    attempts = 0
    target = len(points) + n_random
    while len(points) < target and attempts < n_random * 60:
        attempts += 1
        lon = rng.uniform(CONUS_BBOX[0], CONUS_BBOX[2])
        lat = rng.uniform(CONUS_BBOX[1], CONUS_BBOX[3])
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


def query_point(cur, gm, year, lat, lon):
    row = {}

    cur.execute(
        """
        SELECT ident, name, type,
               ST_Distance(ST_SetSRID(geog,3857), ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857)) AS d
        FROM airports
        ORDER BY ST_SetSRID(geog,3857) <-> ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857)
        LIMIT 1
        """,
        (lon, lat, lon, lat),
    )
    r = cur.fetchone()
    row["nearest_airport"] = {"ident": r[0], "name": r[1], "type": r[2], "dist_km": round(r[3] / 1000, 2)} if r else None

    cur.execute(
        """
        SELECT count(*), max(amsl_ht), max(agl_ht) FROM faa_obstacles
        WHERE ST_DWithin(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857), 9260)
        """,
        (lon, lat),
    )
    r = cur.fetchone()
    row["obstacles"] = {"count": r[0], "max_amsl_ft": r[1], "max_agl_ft": r[2]}

    cur.execute(
        """
        SELECT class, type_code, name FROM class_airspace
        WHERE ST_Contains(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857))
        ORDER BY class LIMIT 5
        """,
        (lon, lat),
    )
    row["class_airspace"] = [{"class": a, "type": b, "name": c} for a, b, c in cur.fetchall()]

    cur.execute(
        """
        SELECT type_code, name FROM special_use_airspace
        WHERE ST_Contains(geog, ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),3857))
        LIMIT 5
        """,
        (lon, lat),
    )
    row["sua"] = [{"type": a, "name": b} for a, b in cur.fetchall()]

    cur.execute(
        """
        SELECT ST_Value(rast,1, ST_SetSRID(ST_MakePoint(%s,%s),4326))
        FROM wmm WHERE ST_Intersects(rast, ST_SetSRID(ST_MakePoint(%s,%s),4326))
        """,
        (lon, lat, lon, lat),
    )
    r = cur.fetchone()
    db_decl = r[0] if r else None
    model_decl = declination(gm, year, lat, lon)
    row["wmm"] = {
        "db_declination": round(db_decl, 3) if db_decl is not None else None,
        "model_declination": round(model_decl, 3),
        "diff": round(abs(db_decl - model_decl), 3) if db_decl is not None else None,
    }

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

    return row


def esc(s):
    if s is None:
        return ""
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


KIND_LABEL = {"major_airport": "Major airport", "minor_airport": "Minor airport", "landmark": "Landmark", "random": "Random point"}


def render_html(results, golden_results, mean_diff, max_diff):
    def fmt_airspace(row):
        parts = []
        seen = set()
        for a in row["class_airspace"]:
            key = (a["class"], a["name"])
            if key in seen:
                continue
            seen.add(key)
            parts.append(f'<span class="chip chip-cls">{esc(a["class"] or "—")}</span> {esc(a["name"])}')
        for s in row["sua"]:
            parts.append(f'<span class="chip chip-sua">{esc(s["type"])}</span> {esc(s["name"])}')
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

    rows_html = []
    for r in results:
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
        golden_html.append(f"""
        <div class="spot-card">
          <div class="spot-head">
            <span class="spot-verdict spot-{v}">{vlabel}</span>
            <h3>{esc(g['title'])}</h3>
          </div>
          <div class="spot-row"><span class="spot-k">Our data</span><span class="spot-v mono">{g['actual']} {g['unit']}</span></div>
          <div class="spot-row"><span class="spot-k">Public record</span><span class="spot-v mono">{g['public_value']} {g['unit']} (&plusmn;{g['tolerance']})</span></div>
        </div>""")

    major = [r for r in results if r["kind"] == "major_airport"]
    minor = [r for r in results if r["kind"] == "minor_airport"]
    landmarks = [r for r in results if r["kind"] == "landmark"]
    random_pts = [r for r in results if r["kind"] == "random"]
    class_b = sum(1 for r in major if any(a["class"] == "B" for a in r["class_airspace"]))
    no_class = sum(1 for r in results if not r["class_airspace"])
    golden_pass = sum(1 for g in golden_results if g["pass"])

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
    <p class="eyebrow">osmpgsqlimport &middot; nav database data quality check</p>
    <h1>Spot check against public records</h1>
    <p class="subhead">{len(results)} points across the continental US, checked against airports, obstacles,
    charted airspace, magnetic declination (WMM), roads, and land-use areas, then cross-referenced against
    publicly known facts.</p>
  </header>
  <div class="stats">
    <div class="stat"><span class="n">{len(results)}</span><span class="lbl">Points checked</span></div>
    <div class="stat"><span class="n">{class_b} / {len(major)}</span><span class="lbl">Major hubs correctly Class B</span></div>
    <div class="stat"><span class="n">{mean_diff:.3f}&deg;</span><span class="lbl">Mean WMM deviation</span></div>
    <div class="stat"><span class="n">{max_diff:.3f}&deg;</span><span class="lbl">Max WMM deviation</span></div>
    <div class="stat"><span class="n">{no_class}</span><span class="lbl">Points correctly uncharted (Class G)</span></div>
    <div class="stat"><span class="n">{golden_pass} / {len(golden_results)}</span><span class="lbl">Golden-fact checks passed</span></div>
  </div>
  <section>
    <h2>Golden-fact checks</h2>
    <div class="spot-grid">
      {"".join(golden_html)}
    </div>
  </section>
  <section>
    <h2>All {len(results)} points <span class="count">&mdash; {len(major)} major airports, {len(minor)} minor airports, {len(landmarks)} landmarks, {len(random_pts)} random CONUS points</span></h2>
    <div class="filters">
      <button class="filter-btn active" data-filter="all">All ({len(results)})</button>
      <button class="filter-btn" data-filter="major_airport">Major airports ({len(major)})</button>
      <button class="filter-btn" data-filter="minor_airport">Minor airports ({len(minor)})</button>
      <button class="filter-btn" data-filter="landmark">Landmarks ({len(landmarks)})</button>
      <button class="filter-btn" data-filter="random">Random ({len(random_pts)})</button>
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
    ap.add_argument("--n-random", type=int, default=86, help="random CONUS points, on top of 5 major + 5 minor airports + 4 landmarks")
    ap.add_argument("--seed", type=int, default=None, help="seed for random point selection (omit for a fresh sample each run)")
    ap.add_argument("--year", type=float, default=None, help="decimal year for WMM cross-check (default: today)")
    ap.add_argument("-o", "--output", default="dq_report.html")
    ap.add_argument("--json", default=None, help="also write raw per-point results as JSON to this path")
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

    points, attempts = build_points(cur, args.n_random, args.seed)
    print(f"Checking {len(points)} points ({attempts} attempts to fill random quota)...", file=sys.stderr)

    results = []
    by_label = {}
    for i, p in enumerate(points):
        row = {"idx": i, "lat": round(p["lat"], 5), "lon": round(p["lon"], 5), "kind": p["kind"], "label": p["label"]}
        row.update(query_point(cur, gm, year, p["lat"], p["lon"]))
        results.append(row)
        by_label[p["label"]] = row
        if (i + 1) % 20 == 0:
            print(f"  {i+1}/{len(points)}", file=sys.stderr)

    kvly = next(r for r in results if "KVLY" in r["label"])
    wtc_area = next(r for r in results if "Statue of Liberty" in r["label"])
    pts_by_key = {"kvly": kvly, "wtc_area": wtc_area}

    golden_results = []
    all_golden_pass = True
    for g in GOLDEN_FACTS:
        actual = g["check"](pts_by_key)
        ok = actual is not None and abs(actual - g["public_value"]) <= g["tolerance"]
        all_golden_pass &= ok
        golden_results.append({"title": g["title"], "actual": actual, "public_value": g["public_value"],
                                "unit": g["unit"], "tolerance": g["tolerance"], "pass": ok})

    diffs = [r["wmm"]["diff"] for r in results if r["wmm"]["diff"] is not None]
    mean_diff = sum(diffs) / len(diffs) if diffs else 0.0
    max_diff = max(diffs) if diffs else 0.0

    html = render_html(results, golden_results, mean_diff, max_diff)
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
        print(f"  [{status}] {g['title']}: {g['actual']} {g['unit']} (public record: {g['public_value']} +/-{g['tolerance']})", file=sys.stderr)
    print(f"WMM: mean |diff|={mean_diff:.3f} deg, max |diff|={max_diff:.3f} deg (vs independent pygeomag model)", file=sys.stderr)

    sys.exit(0 if all_golden_pass else 1)


if __name__ == "__main__":
    main()
