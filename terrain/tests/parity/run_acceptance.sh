#!/bin/bash
# Phase E acceptance gate: 5 seeds x 3 r10 regions, phases 3-7, full-state
# comparison with C++ determinism double-runs. Every combo must exit 0 from
# run_full_parity.sh (zero block/biome/heightmap diffs) for the gate to pass.
#
# Seeds: canonical, zero, -1 (sign-extension paths), one large 64-bit value,
# one randomly generated (frozen here for reproducibility).
# Regions: origin, moderate offset, and a far region (~±960k blocks) that
# stresses float precision / trig / overflow paths.
#
# Usage: ./run_acceptance.sh [--quick] [--fresh-java]
#   --quick:      seed 12345 only (smoke run of the harness itself)
#   --fresh-java: ignore stored goldens and re-run the Java reference for
#                 every combo (true held-out verification; multi-hour).
#                 Default uses goldens/ so Java runs only for combos with
#                 no stored golden yet.

set -u
cd "$(dirname "$0")"

SEEDS=(12345 0 -1 8624967637829732226 2014427639945758541)
REGIONS=("0 0" "31 31" "60000 -60000")
GOLDEN_DIR="$PWD/goldens"

for arg in "$@"; do
    case $arg in
        --quick) SEEDS=(12345) ;;
        --fresh-java)
            GOLDEN_DIR="$(mktemp -d /tmp/acceptance_fresh_java.XXXXXX)"
            echo "Fresh-Java mode: goldens written to $GOLDEN_DIR (not reused)" ;;
        *) echo "Unknown option: $arg"; exit 2 ;;
    esac
done

PASS=0
FAIL=0
declare -a FAILURES=()

for seed in "${SEEDS[@]}"; do
    for region in "${REGIONS[@]}"; do
        set -- $region
        cx=$1; cz=$2
        tag="seed=$seed r10 @ ($cx,$cz)"
        echo "=== ACCEPTANCE: $tag ==="
        ./run_full_parity.sh --radius 10 --center "$cx" "$cz" --seed "$seed" \
            --phases 3-7 --determinism --golden "$GOLDEN_DIR"
        rc=$?
        if [ $rc -eq 0 ]; then
            PASS=$((PASS+1))
            echo "=== PASS: $tag ==="
        else
            FAIL=$((FAIL+1))
            FAILURES+=("$tag (exit $rc)")
            echo "=== FAIL: $tag (exit $rc) ==="
        fi
    done
done

echo ""
echo "==================== ACCEPTANCE SUMMARY ===================="
echo "Passed: $PASS   Failed: $FAIL"
if [ $FAIL -gt 0 ]; then
    for f in "${FAILURES[@]}"; do echo "  FAIL: $f"; done
    exit 1
fi
echo "ALL ACCEPTANCE GATES PASSED (zero diffs, deterministic)."
exit 0
