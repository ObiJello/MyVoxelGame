#!/bin/bash
# run_full_parity.sh - the canonical Java-vs-C++ parity driver.
#
# Runs the Java reference harness (headless server) and the C++ async pipeline
# with identical arguments, both emitting the canonical dump (FORMAT.md), then
# compares with compare_parity.py. Exit code propagates from the comparator
# (0 = identical, 1 = mismatches, 2 = structural/runtime error).
#
# Usage:
#   ./run_full_parity.sh --single X Z   [--phases 3-7] [--seed S] [options]
#   ./run_full_parity.sh --radius R     [--center X Z] [--phases 3-7] [--seed S] [options]
#
# Options:
#   --determinism      Run the C++ side twice (fresh processes) and require
#                      byte-identical dumps before comparing against Java.
#   --golden <dir>     Use/store golden Java dumps: if <dir>/<tag>.txt.gz exists
#                      the Java run is skipped and the golden is used; otherwise
#                      the fresh Java dump is stored there.
#   --results <dir>    Output directory (default: tests/parity/results)
#   --first <n>        Mismatch samples per (class,type) (default 5)

set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"

SEED=12345
PHASES="3-7"
MODE=""
SINGLE_X=0; SINGLE_Z=0
RADIUS=0; CENTER_X=0; CENTER_Z=0
DETERMINISM=0
GOLDEN_DIR=""
FIRST=5

while [[ $# -gt 0 ]]; do
    case $1 in
        --single) MODE="single"; SINGLE_X="$2"; SINGLE_Z="$3"; shift 3 ;;
        --radius) MODE="radius"; RADIUS="$2"; shift 2 ;;
        --center) CENTER_X="$2"; CENTER_Z="$3"; shift 3 ;;
        --phases) PHASES="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --determinism) DETERMINISM=1; shift ;;
        --golden) GOLDEN_DIR="$2"; shift 2 ;;
        --results) RESULTS_DIR="$2"; shift 2 ;;
        --first) FIRST="$2"; shift 2 ;;
        --dump-block-entities) BE=1; shift ;;
        --dimension) DIMENSION="$2"; shift 2 ;;
        --world-type) WORLD_TYPE="$2"; shift 2 ;;
        --flat-preset) FLAT_PRESET="$2"; shift 2 ;;
        --flat-layers) FLAT_LAYERS="$2"; shift 2 ;;
        --single-biome) SINGLE_BIOME="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

if [ -z "$MODE" ]; then
    echo "Error: must specify --single X Z or --radius R"
    exit 2
fi

# Custom flat layer strings contain '*'; nothing below needs globbing.
set -f

mkdir -p "$RESULTS_DIR"

if [ "$MODE" = "single" ]; then
    TAG="s${SEED}_single_${SINGLE_X}_${SINGLE_Z}_p${PHASES}"
else
    TAG="s${SEED}_r${RADIUS}_c${CENTER_X}_${CENTER_Z}_p${PHASES}"
fi
if [ "${BE:-0}" = "1" ]; then
    TAG="${TAG}_be"
    BE_FLAG="--dump-block-entities"
else
    BE_FLAG=""
fi
if [ -n "${DIMENSION:-}" ] && [ "$DIMENSION" != "overworld" ]; then
    TAG="${TAG}_d${DIMENSION}"
    DIM_FLAG="--dimension $DIMENSION"
else
    DIM_FLAG=""
fi

# World-type tag segment + passthrough flags (overworld world presets).
WT_FLAGS=""
if [ -n "${WORLD_TYPE:-}" ] && [ "$WORLD_TYPE" != "default" ]; then
    TAG="${TAG}_wt${WORLD_TYPE}"
    WT_FLAGS="--world-type $WORLD_TYPE"
    if [ -n "${FLAT_PRESET:-}" ]; then
        TAG="${TAG}-${FLAT_PRESET}"
        WT_FLAGS="$WT_FLAGS --flat-preset $FLAT_PRESET"
    fi
    if [ -n "${FLAT_LAYERS:-}" ]; then
        # Custom layer strings are not tag-safe; tag with a short hash.
        LAYERS_HASH=$(printf '%s' "$FLAT_LAYERS" | shasum -a 256 | cut -c1-8)
        TAG="${TAG}-x${LAYERS_HASH}"
        WT_FLAGS="$WT_FLAGS --flat-layers $FLAT_LAYERS"
    fi
    if [ -n "${SINGLE_BIOME:-}" ]; then
        TAG="${TAG}-$(printf '%s' "$SINGLE_BIOME" | sed 's/^minecraft://; s/[^a-z0-9_]/_/g')"
        WT_FLAGS="$WT_FLAGS --single-biome $SINGLE_BIOME"
    fi
fi

JAVA_OUT="$RESULTS_DIR/java_${TAG}.txt"
CPP_OUT="$RESULTS_DIR/cpp_${TAG}.txt"
SUMMARY="$RESULTS_DIR/summary_${TAG}.json"

# ---------- Java side (or golden) ----------
GOLDEN_FILE=""
if [ -n "$GOLDEN_DIR" ]; then
    mkdir -p "$GOLDEN_DIR"
    GOLDEN_FILE="$GOLDEN_DIR/java_${TAG}.txt.gz"
fi

if [ -n "$GOLDEN_FILE" ] && [ -f "$GOLDEN_FILE" ]; then
    echo "=== Using golden Java dump: $GOLDEN_FILE ==="
    gunzip -c "$GOLDEN_FILE" > "$JAVA_OUT"
else
    echo "=== Running Java reference harness ==="
    export JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home -v 25)}"
    export PATH="$JAVA_HOME/bin:$PATH"
    if [ "$MODE" = "single" ]; then
        "$SCRIPT_DIR/run_minecraft_async_test.sh" \
            --single "$SINGLE_X" "$SINGLE_Z" --phases "$PHASES" --seed "$SEED" \
            $BE_FLAG $DIM_FLAG $WT_FLAGS --output "$JAVA_OUT" --quiet
    else
        "$SCRIPT_DIR/run_minecraft_async_test.sh" \
            --radius "$RADIUS" --center "$CENTER_X" "$CENTER_Z" \
            --phases "$PHASES" --seed "$SEED" --dump-full \
            $BE_FLAG $DIM_FLAG $WT_FLAGS --output "$JAVA_OUT" --quiet
    fi
    if [ -n "$GOLDEN_FILE" ]; then
        gzip -c "$JAVA_OUT" > "$GOLDEN_FILE"
        echo "Stored golden: $GOLDEN_FILE"
    fi
fi

# ---------- C++ side ----------
if [ ! -x "$BUILD_DIR/async_chunk_test" ]; then
    echo "Building async_chunk_test..."
    (cd "$BUILD_DIR" && make async_chunk_test -j8 >/dev/null)
fi

run_cpp() {
    local out="$1"
    if [ "$MODE" = "single" ]; then
        "$BUILD_DIR/async_chunk_test" --single "$SINGLE_X" "$SINGLE_Z" \
            --phases "$PHASES" --seed "$SEED" $BE_FLAG $DIM_FLAG $WT_FLAGS --output "$out" --quiet >/dev/null
    else
        "$BUILD_DIR/async_chunk_test" --radius "$RADIUS" --center "$CENTER_X" "$CENTER_Z" \
            --phases "$PHASES" --seed "$SEED" --dump-full $BE_FLAG $DIM_FLAG $WT_FLAGS --output "$out" --quiet >/dev/null
    fi
}

echo "=== Running C++ harness ==="
run_cpp "$CPP_OUT"

if [ "$DETERMINISM" = "1" ]; then
    echo "=== Determinism check (second C++ run) ==="
    run_cpp "${CPP_OUT}.run2"
    if ! cmp -s "$CPP_OUT" "${CPP_OUT}.run2"; then
        echo "DETERMINISM FAILURE: two C++ runs differ (see ${CPP_OUT}.run2)"
        exit 2
    fi
    rm -f "${CPP_OUT}.run2"
    echo "C++ output is deterministic (byte-identical across runs)."
fi

# ---------- Compare ----------
echo "=== Comparing ==="
RC=0
python3 "$SCRIPT_DIR/compare_parity.py" "$JAVA_OUT" "$CPP_OUT" \
    --first "$FIRST" --summary-json "$SUMMARY" || RC=$?
echo "Summary JSON: $SUMMARY"
exit $RC
