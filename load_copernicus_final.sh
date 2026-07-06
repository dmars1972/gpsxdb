#!/bin/bash
# Sequentially loads Copernicus DEM GLO-30 terrain for everything still not
# covered by 3DEP, load_copernicus_regions.sh, or load_copernicus_global_rest.sh:
# northern Canada (Yukon/NWT/Nunavut, above that script's 60N cutoff --
# there are real pilots up there), Alaska, Greenland, the Svalbard/high-Arctic
# gap between Europe and Russia's own coverage, and the Pacific islands that
# cross the antimeridian (handled as two non-wrapping bboxes, since
# tilesForBBox doesn't support a single bbox spanning 180/-180).
#
# Deliberately excludes Antarctica: no permanent civil GA population, almost
# entirely ice, and Copernicus's coverage there is largely a specialized
# ice-surface model rather than useful terrain data.
#
# Alaska uses Copernicus here (not --source 3dep) even though it's US
# territory -- 3DEP hasn't actually been loaded for Alaska by this project
# yet (the original terrain_load run was CONUS-only), so there's no existing
# authoritative US source to defer to here; this just fills the real gap.
# Revisit with a dedicated --source 3dep pass over Alaska if that ever
# happens.
#
# Safe to re-run: already-loaded tiles are skipped (tracked per-tile in
# terrain_tiles), so a killed or interrupted run just picks up where it left
# off.
#
# Deliberately passes --no-bands on every call: buildTerrainBands rebuilds
# terrain_bands from the ENTIRE terrain table on every invocation, so calling
# it once per region here would mean redoing that rebuild N times over. Run
# a terrain_bands rebuild ONCE, separately, after this script finishes.

set -e

SERVER=server
DB=nav
USER=daniel
THREADS=10

# name|min_lon,min_lat,max_lon,max_lat
REGIONS=(
  "northern_canada|-141,60,-52,84"
  "alaska|-170,51,-129,72"
  "greenland|-75,58,-10,84"
  "svalbard_high_arctic|-10,70,65,84"
  "pacific_islands_west|170,-25,180,25"
  "pacific_islands_east|-180,-25,-150,25"
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
