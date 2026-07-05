#!/bin/bash
# Sequentially loads Copernicus DEM GLO-30 terrain for everything NOT already
# covered by 3DEP (US) or load_copernicus_regions.sh (Canada, Mexico, Central
# America, Caribbean): South America, Europe, Africa, the Middle East, Asia,
# Russia, and Australia/Oceania. This is essentially the rest of the planet's
# land area outside the GA-relevant regions script, run second/separately
# given its much larger size (likely several hundred GB, many hours to run).
#
# Safe to re-run: already-loaded tiles are skipped (tracked per-tile in
# terrain_tiles), so a killed or interrupted run just picks up where it left
# off. Some region bboxes below deliberately overlap slightly at their edges
# (e.g. Middle East / Africa / South Asia) -- harmless, the overlap is just
# re-checked and skipped as already-loaded, not re-downloaded.
#
# NOT covered: Pacific islands east of the antimeridian (Fiji, French
# Polynesia, Kiribati, etc. -- bbox wraparound isn't handled) and
# Antarctica/high-Arctic land (minimal Copernicus coverage there anyway).
# Alaska, Hawaii, and Puerto Rico are US territory covered by 3DEP, not
# included here or in load_copernicus_regions.sh -- load those via
# --source 3dep separately if needed.
#
# Deliberately passes --no-bands on every call: buildTerrainBands rebuilds
# terrain_bands from the ENTIRE terrain table on every invocation, so calling
# it once per region here would mean redoing the (multi-hour, at CONUS scale
# -- and this covers far more area) rebuild N times over. Run a terrain_bands
# rebuild ONCE, separately, after this script finishes all regions.

set -e

SERVER=server
DB=nav
USER=daniel
THREADS=10

# name|min_lon,min_lat,max_lon,max_lat
REGIONS=(
  "south_america|-82,-56,-34,13"
  "europe|-25,34,40,71"
  "africa|-18,-35,52,38"
  "middle_east|25,12,63,42"
  "south_asia|60,5,100,38"
  "east_asia|95,15,150,55"
  "southeast_asia|90,-11,141,25"
  "oceania_australia|110,-47,180,-10"
  "russia_north_asia|40,45,180,78"
)

mkdir -p terrain_load_logs

for entry in "${REGIONS[@]}"; do
  name="${entry%%|*}"
  bbox="${entry##*|}"
  log="terrain_load_logs/${name}.log"
  echo "=== ${name} (${bbox}) ==="
  if ./build/terrain_load -s "$SERVER" -d "$DB" -u "$USER" \
       --bbox "$bbox" --source copernicus --no-bands --threads "$THREADS" \
       > "$log" 2>&1; then
    echo "${name}: OK (see ${log})"
  else
    echo "${name}: FAILED (see ${log})"
  fi
done

echo
echo "All regions processed. terrain_bands was NOT rebuilt (--no-bands used throughout)."
echo "Run a separate terrain_bands rebuild manually when ready:"
echo "  ./build/terrain_load -s $SERVER -d $DB -u $USER --bbox -100,19,-98,20 --source copernicus --threads $THREADS"
