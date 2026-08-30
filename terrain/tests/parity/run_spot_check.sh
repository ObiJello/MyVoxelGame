#!/bin/bash
# run_spot_check.sh - fresh-Java random-seed parity spot check.
#
# The golden-based regression net (run_regression.sh) only proves parity
# against frozen Java output for a fixed set of (seed, region) combos. This
# script guards against that set being a lucky subset: it generates brand-new
# random 64-bit seeds and random centers, runs BOTH sides fresh (Java is
# always re-run), and requires zero diffs + C++ determinism.
#
# Run after each optimization category lands and once before declaring done.
#
# Usage: ./run_spot_check.sh [--count N] [--radius R] [--phases P]
#   --count N    Number of random (seed, center) combos (default 2)
#   --radius R   Region radius per combo (default 2)
#
# Results append to results/spot_checks.log (seed/center recorded so any
# failure is reproducible). Exit 0 = all green.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/results/spot_checks.log"

COUNT=2; RADIUS=2; PHASES="3-7"
while [[ $# -gt 0 ]]; do
    case $1 in
        --count) COUNT="$2"; shift 2 ;;
        --radius) RADIUS="$2"; shift 2 ;;
        --phases) PHASES="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

random_i64() {
    python3 -c "import secrets; v=secrets.randbits(64); print(v - (1<<64) if v >= (1<<63) else v)"
}
random_center() {
    # Half the combos near origin, half far out (float-precision stress).
    python3 -c "
import random
if random.random() < 0.5:
    print(random.randint(-64, 64), random.randint(-64, 64))
else:
    print(random.choice([-1, 1]) * random.randint(55000, 65000),
          random.choice([-1, 1]) * random.randint(55000, 65000))"
}

mkdir -p "$(dirname "$LOG")"
PASS=0; FAIL=0; FAILURES=()

for i in $(seq 1 "$COUNT"); do
    SEED="$(random_i64)"
    read -r CX CZ <<< "$(random_center)"
    TAG="seed=$SEED r$RADIUS @ ($CX,$CZ) p$PHASES"
    echo "=== SPOT CHECK [$i/$COUNT]: $TAG (fresh Java) ==="
    "$SCRIPT_DIR/run_full_parity.sh" --radius "$RADIUS" --center "$CX" "$CZ" \
        --seed "$SEED" --phases "$PHASES" --determinism
    RC=$?
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) seed=$SEED center=$CX,$CZ radius=$RADIUS phases=$PHASES rc=$RC" >> "$LOG"
    if [ $RC -eq 0 ]; then
        PASS=$((PASS+1))
        # fresh dumps are regenerable; drop them on success to save disk
        rm -f "$SCRIPT_DIR/results/java_s${SEED}_r${RADIUS}_c${CX}_${CZ}_p${PHASES}.txt" \
              "$SCRIPT_DIR/results/cpp_s${SEED}_r${RADIUS}_c${CX}_${CZ}_p${PHASES}.txt"
    else
        FAIL=$((FAIL+1)); FAILURES+=("$TAG rc=$RC")
    fi
done

echo ""
echo "=== SPOT CHECK SUMMARY: $PASS passed, $FAIL failed ==="
if [ $FAIL -gt 0 ]; then
    for f in "${FAILURES[@]}"; do echo "  FAIL: $f"; done
    echo "(dumps kept in results/ for debugging; combos logged in $LOG)"
    exit 1
fi
exit 0
