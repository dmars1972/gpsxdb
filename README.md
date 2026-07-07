# gpsxdb

A navigation database builder for **GA (General Aviation) Experimental
aircraft GPS systems**, written in C++20. It imports and combines
OpenStreetMap, FAA, OpenAIP, NOAA, and USGS/Copernicus data into a single
PostGIS database that a homebuilt-aircraft glass panel or EFB can query
directly — roads and terrain for moving-map display, magnetic declination
for heading correction, charted airspace boundaries, and FAA obstacle data,
alongside a full worldwide airport/navaid database.

At its core is a high-performance OpenStreetMap PBF importer (the original
purpose of this project) with a custom Mercator schema, parallel
processing, and efficient random-access node lookup via a memory-mapped
flat file — built to handle a full-planet import, not just regional
extracts.

---

## Features

- **Multi-threaded node processing** — parallel Mercator projection across configurable node threads
- **LZ4-compressed per-thread shard files** — compact on-disk node store during import, merged into a flat mmap for O(1) random coordinate lookup during way/relation processing
- **Direct PostgreSQL COPY** — no ORM, no temp tables, no ON CONFLICT overhead; bulk-loads via `pqxx::stream_to`
- **Custom Mercator schema** — all coordinates stored as EPSG:3857 (Web Mercator) to match tile rendering pipelines
- **Normalized tag tables** — separate `*_tags` tables per entity type rather than hstore columns
- **Phase-resumable** — blob-aligned PBF offset checkpoints allow restarting at any phase (nodes, merge, ways, relations, indexing, airports, FAA obstacles, WMM, airspace) without re-processing earlier phases
- **Delta/replication support** — applies OSC change files (`.osc`/`.osc.gz`) with tag diffing; polling mode tracks sequences via the database
- **Integrated OurAirports data** — loads airports, runways, frequencies, navaids, countries, and regions from [OurAirports](https://ourairports.com/) as a final step
- **FAA obstacle data** — all known US man-made obstacles affecting aeronautical charting (towers, antennas, wind turbines, etc.)
- **Terrain elevation** — USGS 3DEP (US) and Copernicus DEM GLO-30 (worldwide) raster elevation, plus derived elevation-band polygons for sectional-chart-style terrain tinting
- **WMM magnetic declination** — a direct port of NOAA's public WMM spherical-harmonic model, computed globally and stored as both a raster and declination-band polygons
- **Charted airspace** — FAA Class Airspace and Special Use Airspace (US), plus OpenAIP international airspace (rest of world)
- **Post-import data quality check** — spot-checks a sample of points against public records (real airport/obstacle facts, an independent WMM implementation) after each import
- **GiST spatial indexes** — built after all data is loaded, not during import, for maximum efficiency

---

## Schema

### OSM tables

| Table | Description |
|---|---|
| `nodes` | Point geometries with Mercator coordinates |
| `ways` | Linear geometries (roads, paths, rivers, etc.) |
| `areas` | Polygon geometries — closed ways (positive IDs) and multipolygon/boundary relations (negative IDs = `-relation_id`). The negative ID convention disambiguates the two since OSM way and relation IDs are separate namespaces that can share the same integers. |
| `roads` | Route relations tagged `route=road` or `highway=*` |
| `relations` | Relation geometries (MultiLineString, nullable) |
| `node_tags` | Key/value tags for nodes |
| `way_tags` | Key/value tags for ways |
| `area_tags` | Key/value tags for areas |
| `road_tags` | Key/value tags for roads |
| `relation_tags` | Key/value tags for relations |
| `osm_replication_state` | Tracks last applied replication sequence number |

### OurAirports tables

| Table | Description |
|---|---|
| `airports` | Airports with Mercator point geometry |
| `frequencies` | Radio frequencies per airport |
| `runways` | Runway endpoints with Mercator geometry |
| `navaids` | Navigation aids with Mercator geometry |
| `countries` | Country reference data |
| `regions` | Region reference data |
| `tags` | Unified key/value tag table for airports and navaids |

### FAA obstacle tables

| Table | Description |
|---|---|
| `faa_obstacles` | FAA Digital Obstacle File — all known man-made obstacles affecting aeronautical charting (towers, antennas, wind turbines, buildings, etc.) with AGL/AMSL heights, lighting, marking, accuracy codes, and point geometry |

All geometry columns use PostGIS `geometry` type. The default SRID is EPSG:3857 (Web Mercator). Pass `-L` at import time to store as EPSG:4326 (WGS84) instead.

### Terrain tables

| Table | Description |
|---|---|
| `terrain` | USGS 3DEP elevation data as PostGIS `raster` tiles (US-only). Query point elevation via `ST_Value(rast, point)`. |
| `terrain_tiles` | Tracks which 1-degree tiles have been loaded, for idempotent/incremental loading. |
| `terrain_bands` | Elevation-band area polygons (`band_min_ft`, `band_max_ft`, `geog`) derived from `terrain` — a color-tint layer suited to a sectional-chart-style terrain display, much smaller than the raw raster. Rebuilt from scratch each time `terrain_load` runs (unless `--no-bands`). |

Loaded separately via the standalone `terrain_load` tool — see below.

### WMM (magnetic declination) tables

| Table | Description |
|---|---|
| `wmm` | Magnetic declination as PostGIS `raster` tiles, one 10-degree cell per row, always EPSG:4326 regardless of the destination SRID used elsewhere. Query point declination via `ST_Value(rast, point)`. |
| `wmm_cells` | Tracks which 10-degree cells have been computed, for idempotent/incremental (re)loading. |
| `wmm_bands` | Declination-band area polygons (`band_min_deg`, `band_max_deg`, `geog`) derived from `wmm` — analogous to `terrain_bands`. Rebuilt from scratch each time `wmm_load` runs (unless `--no-bands`). |

Loaded separately via the standalone `wmm_load` tool — see below.

### Airspace tables

| Table | Description |
|---|---|
| `class_airspace` | FAA Class Airspace (US + territories) — Class A/B/C/D/E/G surface areas, Mode-C veils, with floor/ceiling altitudes. |
| `special_use_airspace` | FAA Special Use Airspace (US + territories) — Military Operations Areas, Restricted, Warning, Alert, and Prohibited areas, with times-of-use. |
| `international_airspace` | OpenAIP airspace covering every country except the US (FAA data is authoritative there). `type`/`icao_class`/`lower_unit`/`lower_ref`/`upper_unit`/`upper_ref` are OpenAIP's raw numeric codes, not decoded to names — see `include/AirspaceLoader.h`. |

Loaded separately via the standalone `airspace_load` tool — see below.

---

## Dependencies

```bash
sudo apt install \
  build-essential cmake \
  libboost-program-options-dev \
  libexpat1-dev \
  zlib1g-dev \
  libpqxx-dev \
  libpq-dev \
  libproj-dev \
  protobuf-compiler \
  libprotobuf-dev \
  liblz4-dev \
  postgresql postgresql-contrib postgis
```

On Raspberry Pi, if osmium reports a missing boost library:
```bash
sudo ln -s /usr/lib/aarch64-linux-gnu/libboost_program_options.so.1.90.0 \
           /usr/lib/aarch64-linux-gnu/libboost_program_options.so.1.83.0
sudo ldconfig
```

---

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Produces two binaries:
- `build/osm_import` — main importer
- `build/airports_load` — standalone OurAirports loader (for testing)

---

## Database Setup

```bash
# Create database and enable PostGIS
createdb <your_db>
psql -d <your_db> -c "CREATE EXTENSION postgis;"

# Create OSM schema
psql -h localhost -U <your_user_id> -d <your_db> -f create.sql

# Create OurAirports schema (imported automatically at end of import)
psql -h localhost -U <your_user_id> -d <your_db> -f create_airports.sql
```

### PostgreSQL tuning (recommended for import)

Add to `postgresql.conf` and restart **before** starting the import:

```
shared_buffers = 1GB
wal_buffers = 16MB
work_mem = 64MB
max_wal_size = 1GB
synchronous_commit = off
```

The importer automatically disables `autovacuum` at startup and re-enables
it after `VACUUM ANALYZE` completes — you do not need to manage this manually.

---

## Usage

### Initial import

```bash
# Get max node ID from PBF
osmium fileinfo -e planet-latest.osm.pbf | grep "Max ID"

./build/osm_import \
  -i planet-latest.osm.pbf \
  -s <your_db_server> -d <your_db> -u <your_user_id> \
  -I \
  -n 20000000000 \
  -f nodes.dat \
  -v -l osm.log
```

Don't already have `planet-latest.osm.pbf`? Pass `--download-planet` to have
`osm_import` fetch the latest one from planet.openstreetmap.org before
importing (saved to the `-i` path, `./planet-latest.osm.pbf` if `-i` is
omitted). The file is on the order of 100GB, so this takes hours — the
download is resumable (safe to re-run after an interruption or crash) and
checksum-verified against upstream's published MD5 before the import
proceeds, and it happens before `-I` touches the database, so a failed
download never leaves you with a wiped schema and nothing to import:

```bash
./build/osm_import \
  -i planet-latest.osm.pbf --download-planet \
  -s <your_db_server> -d <your_db> -u <your_user_id> \
  -I \
  -n 20000000000 \
  -f nodes.dat \
  -v -l osm.log
```

`-I` drops and recreates all tables before the import begins — equivalent
to running `create.sql` and `create_airports.sql` manually. Safe to use on
a fresh database or when reimporting from scratch. **Do not use `-I` with
`-R` resume flags** — it will wipe data from phases already completed.

The defaults (`-t 1 -w 6`) are tuned conservatively for low-power hardware
such as a Raspberry Pi. On a proper desktop/server, increase both for much
higher throughput — see [Hardware notes](#hardware-notes) below.

### Options

| Flag | Description | Default |
|---|---|---|
| `-i <file>` | Input PBF file | required |
| `-s <host>` | PostgreSQL host | required |
| `-d <db>` | Database name | required |
| `-u <user>` | Database user | required |
| `-t <n>` | Node threads | 1 (see [Hardware notes](#hardware-notes)) |
| `-w <n>` | Way threads | 6 (see [Hardware notes](#hardware-notes)) |
| `-q <n>` | Queue size per node thread | 10000 |
| `-f <file>` | Node mmap file path | `nodes.dat` |
| `-n <id>` | Max node ID (sizes `nodes.dat`) | 20000000000 |
| `-S <dir>` | Shard file directory | `.` |
| `-l <file>` | Log file | `osm_import.log` |
| `-v` | Verbose logging | off |
| `-I` | Initialize schema (drop + recreate all tables) before import | off |
| `-L` | Store coordinates as WGS84 lon/lat (EPSG:4326) instead of Web Mercator (EPSG:3857) | off (Mercator is default) |
| `-R <phase>` | Resume at phase (see below) | `nodes` |

### Resume support

If an import is interrupted, resume at any phase without reprocessing earlier ones:

```bash
# Resume from merge phase (nodes already written to shards)
./build/osm_import -i planet.osm.pbf -s <your_db_server> -d <your_db> -u <your_user_id> \
  -n 20000000000 -f nodes.dat -R merge

# Resume from ways (nodes.dat already fully merged)
./build/osm_import -i planet.osm.pbf -s <your_db_server> -d <your_db> -u <your_user_id> \
  -n 20000000000 -f nodes.dat -R ways

# Resume from relations (ways already in DB)
./build/osm_import -i planet.osm.pbf -s <your_db_server> -d <your_db> -u <your_user_id> \
  -n 20000000000 -f nodes.dat -R relations

# Re-run spatial indexing only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R indexing

# Re-run airports loading only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R airports

# Re-run WMM declination loading only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R wmm

# Re-run airspace loading only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R airspace

# Re-run terrain elevation loading only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R terrain

# Re-run VACUUM ANALYZE only
./build/osm_import -s <your_db_server> -d <your_db> -u <your_user_id> -R vacuum
```

Resume phases: `nodes` | `merge` | `ways` | `reindex` | `relations` | `indexing` | `airports` | `faa` | `wmm` | `airspace` | `terrain` | `vacuum`

On the first successful import, two checkpoint files are written alongside `nodes.dat`:
- `nodes.dat.offset` — byte offset of first non-node blob in the PBF (skips node section on resume)
- `nodes.dat.relations_offset` — byte offset of first relation blob (skips nodes + ways on `-R relations`)

### Delta mode

Apply a single OSC change file:
```bash
./build/osm_import -m delta -o changes.osc.gz \
  -s <your_db_server> -d <your_db> -u <your_user_id> \
  -f nodes.dat -n 20000000000
```

### Poll mode

Continuously apply replication diffs from planet.openstreetmap.org:
```bash
./build/osm_import -m poll -r minute \
  -s <your_db_server> -d <your_db> -u <your_user_id> \
  -f nodes.dat -n 20000000000

# Start from a specific sequence number
./build/osm_import -m poll -r minute -Q 5123456 \
  -s <your_db_server> -d <your_db> -u <your_user_id> \
  -f nodes.dat -n 20000000000
```

While polling, osm_import also checks every 6 hours whether the OurAirports
or FAA obstacle datasets have been refreshed upstream (via HTTP
`Last-Modified`) and automatically reloads whichever has changed — no
separate `-R airports`/`-R faa` run needed. The same 6-hour check also
triggers a WMM declination refresh every ~3 months (matching how slowly
secular variation actually drifts declination) and an airspace refresh
every month (simpler than matching FAA's ~8-week NASR/SUA publish cycle,
still current enough) — neither has a single upstream "has this changed"
signal to check the way airports/FAA obstacles do, so these are
fixed-interval instead. The last-seen timestamp for each is tracked in the
`external_data_state` table.

Replication granularities: `minute` | `hour` | `day`

### FAA Obstacles standalone loader

```bash
./build/faa_obstacles_load -s <your_db_server> -d <your_db> -u <your_user_id>

# Use WGS84 coordinates instead of Mercator
./build/faa_obstacles_load -s <your_db_server> -d <your_db> -u <your_user_id> -4
```

### OurAirports standalone loader

```bash
# Run create_airports.sql first if not already done
psql -h localhost -U <your_user_id> -d <your_db> -f create_airports.sql

./build/airports_load -s <your_db_server> -d <your_db> -u <your_user_id>
```

### Terrain elevation loader

Loads USGS 3DEP 1 arc-second (~30m) elevation tiles into a PostGIS raster
table (`terrain`), queryable via `ST_Value(rast, point)`. US-only coverage
(3DEP doesn't cover outside the US/territories). Requires the `postgis`
package (for `raster2pgsql`, already a listed dependency) and the
`postgis_raster` extension (created automatically by `-I`, or run
`CREATE EXTENSION postgis_raster;` manually otherwise).

Takes an explicit bounding box rather than defaulting to full-CONUS —
coverage area × resolution drives storage size directly (a few thousand
tiles, 100+ GB, for the whole lower 48). Already-loaded tiles are tracked
in `terrain_tiles` and skipped on re-run, so calling this again with an
overlapping or expanded bbox only fetches what's new.

```bash
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox <min_lon>,<min_lat>,<max_lon>,<max_lat>

# Example: Colorado
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox -109,37,-102,41

# Use WGS84 coordinates instead of Mercator
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox -109,37,-102,41 -4
```

After loading tiles, `terrain_load` also (re)builds `terrain_bands` — elevation-band
area polygons derived from the whole `terrain` raster (not just newly-loaded tiles),
so a feature spanning multiple tiles comes out as one seamless shape. This is a
full rebuild each run (cost grows with total coverage, not just what's new), split
across a worker-thread pool (same pattern as the main import's node/way pools —
each thread owns its own connection, no shared-lock contention since bands are
disjoint). Each polygon is simplified before *and* after the cross-tile union —
simplifying only the final huge unioned shape can pathologically hang over rugged
terrain (thousands of tiny raster-pixel-aligned fragments), so small per-chunk
fragments are smoothed first, which also cuts total storage substantially.

```bash
# Override the 500ft default band width, simplification tolerance, or thread count
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox -109,37,-102,41 --band-ft 1000 --simplify-m 100 --threads 8

# Skip band generation entirely (raw raster only)
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox -109,37,-102,41 --no-bands
```

3DEP is US-only; pass `--source copernicus` to load Copernicus DEM GLO-30
instead for coverage anywhere else in the world. For the whole planet at
once, pass `--global` instead of `--bbox`:

```bash
./build/terrain_load -s <your_db_server> -d <your_db> -u <your_user_id> --global --threads 10
```

This loads Copernicus DEM GLO-30 for all 19 populated non-US regions in one
call — the same regions previously split across the (now superseded)
`load_copernicus_regions.sh`/`load_copernicus_global_rest.sh`/
`load_copernicus_final.sh` scripts. Deliberately excludes the continental
US (3DEP is authoritative there — run a separate `--source 3dep` pass) and
Antarctica (minimal Copernicus coverage, no permanent civil GA population).
Band generation is suppressed per-region and runs once at the very end
instead of 19 times over; pass `--no-bands` to skip it entirely.

### WMM (magnetic declination) loader

Computes World Magnetic Model declination directly from NOAA's public
WMM2025 coefficients (embedded in `WMMLoader.cpp`, no external data file or
network fetch needed for the model itself) and loads it globally by
default — unlike terrain, there's no per-region tile source, so a full
global (re)load takes only a few seconds. The globe is divided into
resumable 10-degree cells (`wmm_cells`), same idea as `terrain_tiles`.

```bash
./build/wmm_load -s <your_db_server> -d <your_db> -u <your_user_id>

# Restrict to a bbox, override band width, or use a specific date
./build/wmm_load -s <your_db_server> -d <your_db> -u <your_user_id> \
  --bbox -109,37,-102,41 --band-deg 1 --year 2027.5
```

### Airspace loader

Loads FAA Class Airspace and Special Use Airspace (US + territories, no
API key needed) and OpenAIP international airspace (everywhere else,
requires a free API key from [openaip.net](https://www.openaip.net/) —
read from `~/.openaip_api_key` by default, never committed to the repo).

```bash
./build/airspace_load -s <your_db_server> -d <your_db> -u <your_user_id>

# Load just one source
./build/airspace_load -s <your_db_server> -d <your_db> -u <your_user_id> --class-only
./build/airspace_load -s <your_db_server> -d <your_db> -u <your_user_id> --sua-only
./build/airspace_load -s <your_db_server> -d <your_db> -u <your_user_id> --intl-only

# Skip international airspace (e.g. no API key available)
./build/airspace_load -s <your_db_server> -d <your_db> -u <your_user_id> --no-intl
```

---

## Data quality check

`dq_check.py` spot-checks a live `nav` database after an import: it samples
100 points across the continental US (5 major hub airports, 5 minor GA
fields, 4 named landmarks, and 86 randomly-sampled points rejection-tested
for real OSM coverage) and checks six data layers at each — nearest
airport, FAA obstacles, charted airspace (class + special use), WMM
magnetic declination, nearby roads (`ways`), and nearby land-use areas
(`areas`). WMM declination is additionally cross-checked against
[pygeomag](https://pypi.org/project/pygeomag/) (an independent NOAA WMM
implementation, not this project's own `WMMLoader.cpp`), and a handful of
stable public facts are checked directly (e.g. the real height of the
KVLY-TV mast) so a future regression has a chance of being caught without
a human eyeballing the SQL results. Meant to be rerun occasionally after a
fresh import, not part of the build.

```bash
pip install -r requirements-dq_check.txt

python3 dq_check.py -s <your_db_server> -d <your_db> -u <your_user_id>
# writes dq_report.html; add --json results.json to also dump raw per-point data
```

**Initial run (2026-07-06, after the first full-planet import):** all 6
golden-fact checks passed, all 5 major hubs correctly showed Class B
airspace, WMM matched the independent model on all 100 points (mean
deviation 0.032°, max 0.095° — consistent with the underlying raster's
0.25° grid resolution, not a defect). Two standout matches against famous
public height records: the KVLY-TV mast came back at 2,060 ft AGL against
its well-known public figure of 2,063 ft, and the tallest obstacle within
5nm of the Statue of Liberty (One World Trade Center) came back at 1,792 ft
AGL against its iconic 1,776 ft spire height. Real street names surfaced
next to the White House (Pennsylvania Avenue NW, E Street NW), and nearby
airport lookups correctly found the White House South Lawn Helipad and a
`closed`-status Crissy Field next to the Golden Gate Bridge, matching its
real decommissioned status.

---


## Status line

During import, a status line is printed to stdout once per second:

```
0:45:12 60.2%  N:2.1B A:0 W:0 R:0 Q:847 M:42.3% | 487.2K/s  [Nodes]
```

| Field | Description |
|---|---|
| `0:45:12` | Total elapsed time since process start (H:MM:SS) |
| `60.2%` | PBF file read progress (bytes read / file size) |
| `N:2.1B` | Total nodes processed (running total, never resets) |
| `A:137.4M` | Total areas processed (running total, never resets) |
| `W:85.5M` | Total ways processed (running total, never resets) |
| `R:122.9K` | Total relations processed (running total, never resets) |
| `Q:847` | Way queue depth (entries waiting to be processed by way threads) |
| `M:42.3%` | Merge progress — only shown during the Merging phase |
| `487.2K/s` | Throughput for the **current phase only** — computed from the count accrued since the phase began, so it reflects current speed rather than being diluted by time spent in earlier phases |
| `[Nodes]` | Current phase |

### Phases

| Phase | Description |
|---|---|
| `[Nodes]` | Reading nodes from PBF, projecting to Mercator, writing to shard files and `nodes` |
| `[Merging]` | Decompressing and merging per-thread shard files into the flat `nodes.dat` mmap |
| `[Ways]` | Reading ways/areas from PBF, looking up node coordinates from mmap, writing to `ways`/`areas` |
| `[Reindexing]` | Rebuilding primary key indexes on way/area/road/relation tables |
| `[Relations]` | Processing relation members, merging geometries, writing to `relations`/`roads` |
| `[Spatial Indexing]` | Building GiST spatial indexes on all geometry columns |
| `[Loading Airports]` | Downloading and loading OurAirports data |
| `[Loading FAA Obstacles]` | Downloading and loading FAA Digital Obstacle File |
| `[Loading WMM Declination]` | Computing and loading World Magnetic Model declination, globally |
| `[Loading Airspace]` | Downloading and loading FAA Class/Special Use Airspace + OpenAIP international airspace |
| `[Loading Terrain]` | Downloading and loading 3DEP (US) + Copernicus DEM GLO-30 (rest of world) elevation, plus `terrain_bands` |
| `[Vacuuming]` | Running `VACUUM ANALYZE` on all tables |
| `[Done]` | Import complete |

## Architecture

### Import pipeline

```
PBF file
   │
   ▼
OSMReader (producer, single thread)
   │  parses PBF blobs, emits OSMEntry variants
   │
   ├──▶ node_queues[0..N]
   │         │
   │         ▼
   │    nodeThread × N
   │    toMercator() + pointWKB()   ← parallelized across N threads
   │    OSMMMap::insert()           ← writes to per-thread LZ4 shard files
   │    NavDB::insertNode()         ← bulk COPY to nodes
   │         │
   │    node_barrier
   │    OSMMMap::merge()            ← decompress + merge shards into flat mmap
   │    OSMMMap::setRandomAccessHint()
   │
   └──▶ way_q
             │
             ▼
        wayThread × M
        OSMMMap::select()        ← O(1) random coord lookup via mmap
        buildWayGeom()
        NavDB::insertWay/Area()  ← bulk COPY to ways / areas
             │
        way_barrier
             │
             ▼
        RelationEntry processing
        NavDB::getWay()          ← SELECT geog FROM ways UNION ALL areas
        mergeWayGeoms()          ← filters to LineString/MultiLineString only
        NavDB::insertRelation()
             │
             ▼
        NavDB::createGistIndexes()  ← GiST spatial indexes on all geog columns
             │
             ▼
        loadAirportsData()          ← downloads + loads OurAirports CSVs
```

### Node store

The node store (`nodes.dat`) is a flat binary file: `node_id × 16 bytes = (lon_m: f64, lat_m: f64)`. Node ID is the implicit array index, so `select(id)` is a single pointer dereference with zero search overhead.

During the node phase, coordinates are written to **per-thread LZ4-compressed shard files** to avoid write contention. After all nodes are processed, shards are decompressed and merged into the flat mmap in a single pass.

For a full planet import, `nodes.dat` is sized at `max_node_id × 16 bytes` (a sparse file — actual disk usage reflects only written node IDs). With `-n 20000000000`, the logical size is ~298 GB but actual usage is ~200 GB for a 2026 planet dump.

---

## Performance

Tested on Raspberry Pi 5 (16GB RAM, dual NVMe via PCIe 2.0) running Ubuntu 26.04,
with conservative thread settings (`-t 1 -w 6`) required for stability:

- **Node phase**: ~500K nodes/sec
- **Way phase**: throughput primarily I/O-bound on node coordinate lookups from `nodes.dat`
- **Full planet import**: ~20 hours end-to-end
- **Full North America import**: ~3-4 hours end-to-end

On a proper desktop/server with PCIe 3.0/4.0 NVMe, significantly higher throughput
is expected — see [Hardware notes](#hardware-notes) for guidance on scaling up.

#
## Tuning thread counts

The status line's `Q:` field (way queue depth) is the primary feedback
mechanism for tuning `-t` (node threads) and `-w` (way threads). The
goal is to keep the queue neither constantly empty nor constantly full.

### Node phase — tuning `-t`

During the **Nodes** phase, the producer (single-threaded PBF parser) feeds
node entries into per-thread queues consumed by `-t` node threads. Each node
thread does Mercator projection, mmap writes, and COPY buffering.

| Queue depth | What it means | Action |
|---|---|---|
| Consistently 0 | Producer is the bottleneck — node threads are faster than the PBF parser can feed them. Adding more node threads won't help. | Reduce `-t` to free up resources, or leave as-is |
| Consistently at max (10000) | Node threads are the bottleneck — they can't keep up with the producer. | Increase `-t` if the system can handle the extra I/O load |
| Fluctuates 0–max | Roughly balanced — this is ideal | Leave as-is |

On an RPi5, `-t 1` is recommended. With a single node thread, `Q:` will
typically sit at 0 (producer-bound), meaning the PBF parser is the ceiling
— adding threads won't improve throughput but will increase I/O pressure and
risk system instability. On a desktop/server, try `-t 3` or higher and watch
whether `Q:` rises.

### Ways/Relations phase — tuning `-w`

During the **Ways** and **Relations** phases, the producer feeds way/relation
entries into a single shared queue consumed by `-w` way threads. Each way
thread does random node-coordinate lookups from `nodes.dat` (mmap), geometry
building, and PostgreSQL COPY.

| Queue depth | What it means | Action |
|---|---|---|
| Consistently 0 | Way threads are faster than the producer can feed them. More threads won't help and will only increase I/O/DB pressure. | Reduce `-w` |
| Consistently at max (10000) | Way threads are the bottleneck — they can't process fast enough. | Increase `-w` if the system is stable |
| Fluctuates 0–max | Balanced — ideal | Leave as-is |

On an RPi5, `-w 6` has been found to be a reasonable balance between
throughput and system stability. Higher values (`-w 12`) increase random I/O
concurrency and can trigger kernel-level instability on this platform even
though the queue appears to have room. On desktop hardware with a fast NVMe,
`-w 12` or higher should be stable and beneficial.

### General rule of thumb

- **Q: always 0** → you have more threads than the bottleneck can feed;
  reduce thread count or accept that you're at the throughput ceiling
- **Q: always at max** → threads are the bottleneck; increase thread count
  if the system is stable under the load
- **System instability (lockups/crashes)** → reduce thread count regardless
  of queue depth; the I/O pressure from concurrent threads, not just
  throughput, is what triggers hardware/driver instability on constrained
  platforms like the RPi5

## Tuning recommendations

- Place `nodes.dat` on your fastest storage (NVMe preferred)
- Use `-S <dir>` to place shard files on a separate drive from `nodes.dat` if available
- PostgreSQL should be on a separate drive from `nodes.dat` if possible
- On desktop hardware, try `-t 3 -w 12` and scale from there

### Known issue: kernel lockups on Raspberry Pi with kernel 7.0.0-1011-raspi

A kernel regression in `7.0.0-1011-raspi` (Ubuntu 26.04) causes full system lockups under sustained heavy mmap write workloads. Workarounds applied in this codebase:

- `CHUNK_SIZE` reduced from 16MB to 4MB to limit write burst size
- Periodic `fsync`/`msync` every 64 chunks to bound dirty page accumulation
- `MADV_RANDOM` hint on the merged node file is applied **after** merge completes rather than at construction time (applying it early on a large sparse mapping triggers the lockup)
- Default thread counts (`-t 1 -w 6`) are tuned conservatively for RPi5 stability

If you experience lockups during the node phase on other platforms, try reducing `CHUNK_SIZE` further in `OSMMMap.cpp`.

---


## Hardware notes

This importer was originally developed and tuned on a **Raspberry Pi 5**
with dual NVMe drives attached via PCIe 2.0. That combination turned out to
be a poor fit for this workload — not because of anything in the import
logic itself, but because **sustained heavy I/O (large concurrent writes
during the node phase, large concurrent random reads during ways/relations)
reliably triggered full system lockups** requiring a hard power cycle. This
happened across many different phases and code paths, which pointed at a
platform-level reliability issue (kernel/driver/PCIe-adapter related) rather
than an application bug.

### What helped on the Pi

- Reducing `CHUNK_SIZE` for the LZ4 shard writer (16MB → 4MB) and adding
  periodic `fsync`/`msync` calls bounded dirty-page accumulation and fixed
  lockups during the node phase.
- Reducing thread counts (`-t 1`, `-w 6`) reduced concurrent I/O pressure and
  allowed runs to progress significantly further before any instability.
- None of this fully eliminated the lockups under all conditions — they
  still occurred at lower frequency, in different phases, as data volume
  increased.

### Recommended hardware

If you're running this on anything other than a Pi — a normal x86_64
desktop or server with a proper PCIe 3.0/4.0 NVMe slot — none of the above
should be necessary, and you can scale thread counts up substantially:

```bash
./build/osm_import -i planet-latest.osm.pbf -s <host> -d <db> -u <user> \
  -t 3 -w 12 -n 20000000000 -f nodes.dat -v -l osm.log
```

Minimum specs for a comfortable full-planet import:
- **CPU**: any modern 4+ core x86_64 CPU — this workload is I/O-bound, not
  CPU-bound, so raw core count/clock speed matters far less than storage
- **RAM**: 16GB minimum, 32GB+ recommended (helps PostgreSQL buffer/cache
  hit rates and OS page cache for the node mmap file)
- **Storage**: a real PCIe 3.0 x4 (or better) M.2 NVMe slot, directly on the
  motherboard — avoid PCIe 2.0 and avoid USB/adapter-based NVMe enclosures
  for the node store file, since random-access throughput there is the
  primary bottleneck during the ways/relations phases
- A second NVMe/SSD for PostgreSQL's data directory, separate from the node
  store file, is recommended if available

If you do hit instability on non-Pi hardware, the same mitigations apply:
reduce `-t`/`-w`, and consider shrinking `CHUNK_SIZE` in `OSMMMap.cpp`
further.

## Data sources

- **OSM planet/extracts**: [planet.openstreetmap.org](https://planet.openstreetmap.org) or [Geofabrik](https://download.geofabrik.de)
- **OurAirports**: [davidmegginson.github.io/ourairports-data](https://davidmegginson.github.io/ourairports-data/) (downloaded automatically at import time)
- **FAA Digital Obstacle File**: [aeronav.faa.gov/Obst_Data/DDOF.zip](https://aeronav.faa.gov/Obst_Data/DDOF.zip) — daily-updated file of all US man-made obstacles affecting aeronautical charting (downloaded automatically at import time)
- **USGS 3DEP**: 1 arc-second (~30m) elevation, US + territories only
- **Copernicus DEM GLO-30**: [copernicus-dem-30m.s3.amazonaws.com](https://copernicus-dem-30m.s3.amazonaws.com/) — ~30m elevation, worldwide, public AWS Open Data, no API key
- **NOAA/NCEI World Magnetic Model**: [ncei.noaa.gov/products/world-magnetic-model](https://www.ncei.noaa.gov/products/world-magnetic-model) — WMM2025 coefficients, embedded in this project's source rather than fetched at runtime
- **FAA Class/Special Use Airspace**: FAA Aeronautical Information Services' public ArcGIS open data portal (adds-faa.opendata.arcgis.com), downloaded automatically
- **OpenAIP**: [openaip.net](https://www.openaip.net/) — crowd-sourced international airspace, CC BY-NC 4.0 (noncommercial use only), requires a free API key

---

## License

MIT License — see [LICENSE](LICENSE)

---

## Contributing

Issues and pull requests welcome. The codebase targets C++20 and is tested on aarch64 (Raspberry Pi 5) and x86_64 Linux.
