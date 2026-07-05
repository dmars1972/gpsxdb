#!/bin/bash
# Sequentially loads Copernicus DEM GLO-30 terrain for GA-relevant regions
# outside the US (which is already covered by 3DEP). Safe to re-run: already
# loaded tiles are skipped (tracked per-tile in terrain_tiles), so a killed
# or interrupted run just picks up where it left off.
#
# Deliberately passes --no-bands on every call: buildTerrainBands rebuilds
# terrain_bands from the ENTIRE terrain table on every invocation, so calling
# it once per region here would mean redoing the (multi-hour, at CONUS scale)
# rebuild N times over. Run terrain_bands rebuild ONCE, separately, after
# this script finishes all regions.

set -e

SERVER=server
DB=nav
USER=daniel
THREADS=10

# name|min_lon,min_lat,max_lon,max_lat
REGIONS=(
  "canada|-141,41,-52,60"
  "mexico|-118,14,-86,33"
  "central_america|-93,7,-77,18"
  "caribbean|-85,10,-59,27"
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
