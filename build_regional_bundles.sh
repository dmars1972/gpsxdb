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
#            [--regions name1,name2,...] [--region-batch-size N] \
#            [--small-staging-dir <dir>] [-v]
#
# Requires ~/.pgpass for authentication. Output: <out-dir>/<region>.gpsxdb.tar.gz
# per region. Per-region staging data is deleted right after each bundle is
# written; only the small shared terrain.schema.sql (re-fetched per region,
# harmless), any not-yet-processed regions' .nodes.dat slices, and (with
# --region-batch-size) not-yet-bundled batches' big-table dumps remain
# under <out-dir>/staging while the run is in progress.
#
# --small-staging-dir: point --small-tables-only's own staging (dominated
# by the terrain export) at a different, faster disk than --out-dir/staging.
# Measured on this project's own hardware: <out-dir>/staging lived on a
# single slow spinning disk, which iostat showed pinned at ~97% util /
# 100-200ms average I/O wait during a region's bundling step -- moving
# --small-tables-only's output to the same NVMe Postgres itself runs on
# (plenty of headroom there; the DB's own read/write during these queries
# never exceeded a few percent of that disk's throughput) dropped that
# disk to 0% util and made the step CPU-bound instead of I/O-bound.
# Verified end-to-end on a real region (south_america, ~85GB bundle):
# no correctness difference, meaningfully faster wall-clock. Defaults to
# the same as --out-dir/staging (i.e. a no-op) since not every deployment
# has a second, faster disk available -- set this only if you do.
set -euo pipefail

HOST=""; DB=""; USER_=""; NODES_FILE=""; MAX_ID="20000000000"
OUT_DIR="."; REGIONS=""; VERBOSE=""; REGION_BATCH_SIZE=""; SMALL_STAGING_DIR=""

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
        --small-staging-dir) SMALL_STAGING_DIR="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="-v"; shift ;;
        -h|--help)
            echo "Usage: $0 -s <host> -d <db> -u <user> -f <nodes.dat> -n <max_id> --out-dir <dir> [--regions n1,n2,...] [--region-batch-size N] [--small-staging-dir <dir>] [-v]"
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
[[ -z "$SMALL_STAGING_DIR" ]] && SMALL_STAGING_DIR="$STAGING_DIR"
mkdir -p "$SMALL_STAGING_DIR"

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
    "$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$SMALL_STAGING_DIR" --small-tables-only $VERBOSE --regions "$region"

    region_dir="$SMALL_STAGING_DIR/$region"
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

    # The 5 big table pairs (--big-tables-only, above) always write to
    # STAGING_DIR -- when --small-staging-dir points somewhere else, merge
    # them into region_dir with a move (not a copy): the one necessary read
    # of this data off STAGING_DIR happens either way (here, or later when
    # tar would have read it in place), so moving it first just avoids the
    # bundling step needing to read from two different directories, at the
    # cost of one extra (fast, since the destination is the faster disk)
    # write. A no-op loop when --small-staging-dir wasn't given, since
    # region_dir == STAGING_DIR/$region in that case and every file below
    # is already exactly where it needs to be.
    if [[ "$SMALL_STAGING_DIR" != "$STAGING_DIR" ]]; then
        for f in ways.bin way_tags.bin areas.bin area_tags.bin roads.bin road_tags.bin relations.bin relation_tags.bin nodes.bin node_tags.bin; do
            old_f="$STAGING_DIR/$region/$f"
            if [[ -f "$old_f" ]]; then
                mv "$old_f" "$region_dir/$f"
            else
                echo "[build_regional_bundles] WARNING: no $old_f found -- bundle will be missing $f" >&2
            fi
        done
        rm -rf "$STAGING_DIR/$region"
    fi

    bundle="$OUT_DIR/$region.gpsxdb.tar.gz"
    echo "[build_regional_bundles] bundling $bundle"
    # pigz instead of plain gzip. Isolated small-file test (2M-row ways.bin
    # sample) showed ~7.5x wall-clock speedup (2.2s vs 16.2s) at identical
    # compression ratio (383,762,583 vs 383,403,944 bytes) -- but that
    # doesn't hold at full bundle scale, where disk I/O reading the
    # dominant terrain.bin.gz (already gzip-compressed by
    # copyOutCompressedGz, so re-compressing it is pure waste either way)
    # is the real bottleneck, not CPU. Verified end-to-end on a real
    # region (altai, ~104GB staged input, 78GB of it already-compressed
    # terrain.bin.gz): pigz produced a byte-verified-intact 95GB bundle in
    # 30m11s, vs. gzip's measured ~78min for Africa's similarly-sized
    # input (extrapolated rate applied to altai's size: ~53min) -- a real
    # but more modest ~1.7x, not 7.5x. Still a genuine win given every
    # region pays this cost. The plain .bin table dumps (COPY binary
    # format) still compress ~30% under pigz, same as gzip, so bundle
    # size for customers is unaffected by this change.
    tar cf - -C "$SMALL_STAGING_DIR" "$region" | pigz > "$bundle"

    echo "[build_regional_bundles] cleaning up staging data for $region"
    rm -rf "$region_dir"
done

echo "[build_regional_bundles] done -- bundles in $OUT_DIR"
