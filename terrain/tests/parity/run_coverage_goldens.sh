#!/bin/bash
# run_coverage_goldens.sh - one-time fresh-Java golden runs for regions that
# close the biome-coverage holes found by coverage_report.py.
#
# Regions were located with build/biome_search on seed 12345 (2026-08-07);
# nearby missing-biome hits are clustered into one region where practical.
# Each run: Java reference once (golden stored), C++ twice (determinism),
# zero diffs required. Raw dumps are deleted on success (goldens keep the
# compressed Java side).

set -u
cd "$(dirname "$0")"

# radius centerX centerZ  # biomes closed
REGIONS=(
  "3 18 13"       # old_growth_spruce_taiga
  "2 -24 -12"     # jagged_peaks
  "2 -42 -16"     # stony_peaks
  "2 42 -32"      # frozen_river
  "2 42 -44"      # ice_spikes
  "4 45 23"       # frozen_ocean + snowy_beach
  "2 -54 -2"      # savanna_plateau
  "2 82 90"       # windswept_gravelly_hills
  "6 -101 -97"    # windswept_forest + mangrove_swamp + swamp
  "2 -4 112"      # windswept_hills
  "2 12 -128"     # flower_forest
  "2 -138 32"     # desert
  "2 -148 40"     # windswept_savanna
  "3 -167 111"    # eroded_badlands + badlands
  "2 250 142"     # mushroom_fields
  "2 -254 -248"   # warm_ocean
  "2 -262 -262"   # wooded_badlands
  "2 18 98"       # deep_frozen_ocean
)
SEED=12345

PASS=0; FAIL=0; FAILURES=()
for entry in "${REGIONS[@]}"; do
    set -- $entry
    R=$1; CX=$2; CZ=$3
    TAG="s${SEED}_r${R}_c${CX}_${CZ}_p3-7"
    if [ -f "goldens/java_${TAG}.txt.gz" ] && [ "${REDO:-0}" != "1" ]; then
        echo "=== SKIP (golden exists): r$R @ ($CX,$CZ) ==="
        continue
    fi
    echo "=== COVERAGE: r$R @ ($CX,$CZ) seed $SEED ==="
    ./run_full_parity.sh --radius "$R" --center "$CX" "$CZ" --seed "$SEED" \
        --phases 3-7 --determinism --golden goldens
    RC=$?
    if [ $RC -eq 0 ]; then
        PASS=$((PASS+1))
        rm -f "results/java_${TAG}.txt" "results/cpp_${TAG}.txt"
    else
        FAIL=$((FAIL+1)); FAILURES+=("r$R @ ($CX,$CZ) rc=$RC")
    fi
done

echo ""
echo "=== COVERAGE GOLDENS: $PASS passed, $FAIL failed ==="
if [ $FAIL -gt 0 ]; then
    for f in "${FAILURES[@]}"; do echo "  FAIL: $f"; done
    exit 1
fi
./promote_goldens.sh --manifest-only
python3 coverage_report.py --quiet
