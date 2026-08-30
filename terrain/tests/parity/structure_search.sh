#!/bin/bash
# structure_search.sh - find chunks whose STRUCTURE_STARTS contain a target
# structure (the structure analog of biome_search). Drives golden region
# selection for the structure-parity batches.
#
# Runs the Java harness at --phases 0-1 (placement + piece layout only, no
# terrain generation) over a chunk area and reports every start chunk of the
# requested structure id.
#
# Usage:
#   ./structure_search.sh --structure minecraft:village_plains \
#       [--seed 12345] [--center 0 0] [--radius 32] [--keep-dump <file>]
#
# Output lines:  START <cx> <cz> refs=<n> bb=<minX,minY,minZ,maxX,maxY,maxZ>
# Exit 0 if at least one start found, 1 if none, 2 on error.

set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

STRUCTURE=""
SEED=12345
CENTER_X=0; CENTER_Z=0
RADIUS=32
KEEP_DUMP=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --structure) STRUCTURE="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --center) CENTER_X="$2"; CENTER_Z="$3"; shift 3 ;;
        --radius) RADIUS="$2"; shift 2 ;;
        --keep-dump) KEEP_DUMP="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

if [ -z "$STRUCTURE" ]; then
    echo "Error: --structure <id> is required (e.g. minecraft:village_plains)"
    exit 2
fi

export JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home -v 25)}"
export PATH="$JAVA_HOME/bin:$PATH"

DUMP="${KEEP_DUMP:-$(mktemp /tmp/structure_search_XXXXXX.txt)}"
cleanup() { [ -z "$KEEP_DUMP" ] && rm -f "$DUMP"; }
trap cleanup EXIT

"$SCRIPT_DIR/run_minecraft_async_test.sh" \
    --radius "$RADIUS" --center "$CENTER_X" "$CENTER_Z" \
    --phases 0-1 --seed "$SEED" --dump-full \
    --output "$DUMP" --quiet >/dev/null

FOUND=$(awk -F, -v target="$STRUCTURE" '
    $1 == "C" { cx = $2; cz = $3 }
    $1 == "S" && $2 == target {
        printf "START %s %s refs=%s bb=%s,%s,%s,%s,%s,%s\n", cx, cz, $3, $4, $5, $6, $7, $8, $9
    }' "$DUMP")

if [ -z "$FOUND" ]; then
    echo "No $STRUCTURE starts in radius $RADIUS around ($CENTER_X, $CENTER_Z), seed $SEED"
    exit 1
fi
echo "$FOUND"
echo "Total: $(echo "$FOUND" | wc -l | tr -d ' ') start(s) of $STRUCTURE (seed $SEED, r$RADIUS @ $CENTER_X,$CENTER_Z)"
