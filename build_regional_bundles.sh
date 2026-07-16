#!/bin/bash
# Builds <region>.gpsxdb.tar.gz installer bundles: runs regional_export
# (nodes.dat slice) + regional_db_export (table dumps + manifest) for each
# region, stages both into one directory per region, then tars+gzips.
#
# Usage: ./build_regional_bundles.sh -s <host> -d <db> -u <user> \
#            -f <master nodes.dat path> -n <max_id> --out-dir <dir> \
#            [--regions name1,name2,...] [-v]
#
# Requires ~/.pgpass for authentication. Output: <out-dir>/<region>.gpsxdb.tar.gz
# for each region, plus <out-dir>/staging/<region>/ left in place for
# inspection (not cleaned up automatically -- delete manually once the
# tarballs are verified).
set -euo pipefail

HOST=""; DB=""; USER_=""; NODES_FILE=""; MAX_ID="20000000000"
OUT_DIR="."; REGIONS=""; VERBOSE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s) HOST="$2"; shift 2 ;;
        -d) DB="$2"; shift 2 ;;
        -u) USER_="$2"; shift 2 ;;
        -f) NODES_FILE="$2"; shift 2 ;;
        -n) MAX_ID="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --regions) REGIONS="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="-v"; shift ;;
        -h|--help)
            echo "Usage: $0 -s <host> -d <db> -u <user> -f <nodes.dat> -n <max_id> --out-dir <dir> [--regions n1,n2,...] [-v]"
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

echo "[build_regional_bundles] running regional_export (nodes.dat slices)..."
"$BIN_DIR/regional_export" -f "$NODES_FILE" -n "$MAX_ID" --out-dir "$STAGING_DIR" $VERBOSE "${REGION_ARGS[@]}"

echo "[build_regional_bundles] running regional_db_export (table dumps)..."
"$BIN_DIR/regional_db_export" -s "$HOST" -d "$DB" -u "$USER_" --out-dir "$STAGING_DIR" $VERBOSE "${REGION_ARGS[@]}"

# regional_export writes <staging>/<region>.nodes.dat; regional_db_export
# writes <staging>/<region>/*.bin + manifest.txt. Move the former into the
# latter so each region's directory is self-contained before tarring.
for region_dir in "$STAGING_DIR"/*/; do
    region="$(basename "$region_dir")"
    nodes_src="$STAGING_DIR/$region.nodes.dat"
    if [[ -f "$nodes_src" ]]; then
        mv "$nodes_src" "$region_dir/"
    else
        echo "[build_regional_bundles] WARNING: no nodes.dat slice found for region '$region' -- bundle will be missing node coordinates" >&2
    fi

    bundle="$OUT_DIR/$region.gpsxdb.tar.gz"
    echo "[build_regional_bundles] bundling $bundle"
    tar czf "$bundle" -C "$STAGING_DIR" "$region"
done

echo "[build_regional_bundles] done -- bundles in $OUT_DIR, staging left at $STAGING_DIR"
