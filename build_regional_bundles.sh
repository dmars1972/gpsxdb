#!/bin/bash
# Builds <region>.gpsxdb.tar.gz installer bundles: runs regional_db_export
# --big-tables-only (the 5 big parent+child table pairs, single client-side
# pass covering every requested region -- also writes each region's
# extra_vertices.bin; see regional_db_export.cpp's top-of-file comment),
# then regional_export (one pass over nodes.dat covers every requested
# region -- cheap, not the disk-space concern; folds in extra_vertices.bin
# to widen each region's node file with border-crossing way/area/road
# vertices -- see that tool's top-of-file comment for why), then runs
# regional_db_export --small-tables-only (remaining, smaller tables) +
# bundling ONE REGION AT A TIME, deleting each region's uncompressed
# staging data immediately after its bundle is written.
#
# regional_db_export (originally two separate tools -- regional_table_export
# + regional_db_export, merged 2026-07-30 since every real invocation needed
# both anyway) still has to run in two passes here, NOT because it's two
# binaries anymore, but because the two halves have genuinely different
# disk-usage profiles: --big-tables-only must cover every requested region
# in one call (that's the whole point of the single pass), while
# --small-tables-only is run once per region specifically so this script can
# bundle+delete each region's staging data right after, instead of letting
# every region's uncompressed table dumps pile up on disk at once. An
# earlier all-regions-then-bundle version of this script blew through 1.4TB
# of free disk partway through Africa, before a single bundle had been
# written -- --big-tables-only reintroduces that same all-regions-at-once
# exposure for the 5 big tables specifically (unavoidable, it's a single
# pass), so --region-batch-size is the safety valve for that: it splits the
# region list into batches and runs --big-tables-only once per batch instead
# of once for everything, trading some of the single-pass win (each batch
# re-scans the 5 big tables) for bounded disk usage. Measure actual
# aggregate big-table output size for a representative region subset before
# trusting a full unbatched run; if it's uncomfortably close to available
# disk, use this flag.
#
# Usage: ./build_regional_bundles.sh -s <host> -d <db> -u <user> \
#            -f <master nodes.dat path> -n <max_id> --out-dir <dir> \
#            [--regions name1,name2,...] [--region-batch-size N] [-v]
#
# Requires ~/.pgpass for authentication. Output: <out-dir>/<region>.gpsxdb.tar.gz
# per region. Per-region staging data is deleted right after each bundle is
# written; only the small shared terrain.schema.sql (re-fetched per region,
# harmless), any not-yet-processed regions' .nodes.dat slices, and (with
# --region-batch-size) not-yet-bundled batches' big-table dumps remain
# under <out-dir>/staging while the run is in progress.
set -euo pipefail

HOST=""; DB=""; USER_=""; NODES_FILE=""; MAX_ID="20000000000"
OUT_DIR="."; REGIONS=""; VERBOSE=""; REGION_BATCH_SIZE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s) HOST="$2"; shift 2 ;;
        -d) DB="$2"; shift 2 ;;
        -u) USER_="$2"; shift 2 ;;
        -f) NODES_FILE="$2"; shift 2 ;;
        -n) MAX_ID="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --regions) REGIONS="$2"; shift 2 ;;
        --region-batch-size) REGION_BATCH_SIZE="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="-v"; shift ;;
        -h|--help)
            echo "Usage: $0 -s <host> -d <db> -u <user> -f <nodes.dat> -n <max_id> --out-dir <dir> [--regions n1,n2,...] [--region-batch-size N] [-v]"
            exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$HOST" || -z "$DB" || -z "$USER_" || -z "$NODES_FILE" ]]; then
    echo "Error: -s, -d, -u, -f are required" >&2
    exit 1
fi

BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build"
STAGING_DIR="$OUT_DIR/staging"
mkdir -p "$STAGING_DIR"

REGION_ARGS=()
[[ -n "$REGIONS" ]] && REGION_ARGS=(--regions "$REGIONS")

# Needed before regional_db_export --big-tables-only can even be invoked
# (batching below, and the final per-region loop). data/regions/*.wkt is
# the same source of truth RegionPolygons::load() reads (see
# include/RegionPolygons.h), so this matches "all regions" exactly when
# --regions wasn't given.
region_list=()
if [[ -n "$REGIONS" ]]; then
    IFS=',' read -ra region_list <<< "$REGIONS"
else
    shopt -s nullglob
    for f in data/regions/*.wkt; do
        region_list+=("$(basename "$f" .wkt)")
    done
    shopt -u nullglob
fi

echo "[build_regional_bundles] running regional_db_export --big-tables-only (5 big table pairs + extra_vertices.bin, single pass)..."
if [[ -z "$REGION_BATCH_SIZE" ]]; then
    "$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$STAGING_DIR" --big-tables-only $VERBOSE "${REGION_ARGS[@]}"
else
    batch=()
    for region in "${region_list[@]}"; do
        batch+=("$region")
        if [[ ${#batch[@]} -ge $REGION_BATCH_SIZE ]]; then
            batch_csv="$(IFS=,; echo "${batch[*]}")"
            echo "[build_regional_bundles] regional_db_export --big-tables-only batch: $batch_csv"
            "$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$STAGING_DIR" --big-tables-only $VERBOSE --regions "$batch_csv"
            batch=()
        fi
    done
    if [[ ${#batch[@]} -gt 0 ]]; then
        batch_csv="$(IFS=,; echo "${batch[*]}")"
        echo "[build_regional_bundles] regional_db_export --big-tables-only batch: $batch_csv"
        "$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$STAGING_DIR" --big-tables-only $VERBOSE --regions "$batch_csv"
    fi
fi

# extra_vertices.bin exists for every region now, so regional_export can
# fold border-crossing way/area/road vertices into each region's node file.
# One pass over nodes.dat still covers every requested region regardless of
# how the --big-tables-only pass above was batched.
echo "[build_regional_bundles] running regional_export (nodes.dat slices, one pass for all regions)..."
"$BIN_DIR/regional_export" -f "$NODES_FILE" -n "$MAX_ID" --out-dir "$STAGING_DIR" --extra-vertices-dir "$STAGING_DIR" $VERBOSE "${REGION_ARGS[@]}"

echo "[build_regional_bundles] processing ${#region_list[@]} region(s) one at a time..."
for region in "${region_list[@]}"; do
    echo "[build_regional_bundles] === $region ==="
    "$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$STAGING_DIR" --small-tables-only $VERBOSE --regions "$region"

    region_dir="$STAGING_DIR/$region"
    nodes_src="$STAGING_DIR/$region.nodes.dat"
    if [[ -f "$nodes_src" ]]; then
        mv "$nodes_src" "$region_dir/"
    else
        echo "[build_regional_bundles] WARNING: no nodes.dat slice found for region '$region' -- bundle will be missing node coordinates" >&2
    fi

    # Embed this region's WKT polygon so regional_install.cpp can register
    # it in public.installed_regions without depending on a local
    # data/regions/ directory matching this export -- see regional_install's
    # --help. Older bundles without this file fall back to --regions-dir.
    wkt_src="data/regions/$region.wkt"
    if [[ -f "$wkt_src" ]]; then
        cp "$wkt_src" "$region_dir/$region.wkt"
    else
        echo "[build_regional_bundles] WARNING: no $wkt_src found -- bundle won't self-register in installed_regions" >&2
    fi

    bundle="$OUT_DIR/$region.gpsxdb.tar.gz"
    echo "[build_regional_bundles] bundling $bundle"
    tar czf "$bundle" -C "$STAGING_DIR" "$region"

    echo "[build_regional_bundles] cleaning up staging data for $region"
    rm -rf "$region_dir"
done

echo "[build_regional_bundles] done -- bundles in $OUT_DIR"
