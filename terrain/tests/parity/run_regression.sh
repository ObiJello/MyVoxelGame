#!/bin/bash
# run_regression.sh - Java-free parity regression gate.
#
# Runs the C++ pipeline against every stored golden (goldens/MANIFEST.json)
# at or below the requested tier and requires byte-perfect parity plus
# C++ determinism on every combo. This is the gate between optimization
# batches: it never runs Java.
#
# Tiers: 0 = single chunks (~minutes), 1 = r1-r3 (~10-20 min), 2 = r10 (hours).
#
# Usage:
#   ./run_regression.sh [--tier 0|1|2|all] [--label name] [--no-determinism]
#                       [--verify-goldens] [--perf] [--perf-baseline f]
#                       [--tolerance 0.05]
#
#   --tier N          Run goldens with tier <= N (default 1; "all" = 2).
#   --label name      Label for the report JSON (default: timestamp only).
#   --no-determinism  Skip the double-C++-run byte-identity check.
#   --verify-goldens  sha256-verify EVERY golden against the manifest
#                     (default: all tier-0 plus one sample per other tier).
#   --perf            After parity passes, run perf/capture_perf.sh and
#                     compare against perf/baseline.json (or --perf-baseline).
#
# Exit: 0 all green; 1 parity mismatch; 2 structural/determinism/integrity
# failure; 3 perf regression.

set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GOLDEN_DIR="$SCRIPT_DIR/goldens"
MANIFEST="$GOLDEN_DIR/MANIFEST.json"
REG_DIR="$SCRIPT_DIR/results/regression"
BUILD_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)/build"

TIER=1; LABEL=""; DETERMINISM=1; VERIFY_ALL=0; PERF=0
PERF_BASELINE="$SCRIPT_DIR/perf/baseline.json"; TOLERANCE=0.05

while [[ $# -gt 0 ]]; do
    case $1 in
        --tier) TIER="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        --no-determinism) DETERMINISM=0; shift ;;
        --verify-goldens) VERIFY_ALL=1; shift ;;
        --perf) PERF=1; shift ;;
        --perf-baseline) PERF_BASELINE="$2"; shift 2 ;;
        --tolerance) TOLERANCE="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done
[ "$TIER" = "all" ] && TIER=2

[ -f "$MANIFEST" ] || { echo "No $MANIFEST — run ./promote_goldens.sh first"; exit 2; }
mkdir -p "$REG_DIR"

if [ ! -x "$BUILD_DIR/async_chunk_test" ]; then
    echo "Building async_chunk_test..."
    (cd "$BUILD_DIR" && make async_chunk_test -j8 >/dev/null) || exit 2
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT="$REG_DIR/regression_${LABEL:+${LABEL}_}${TS}.json"

# ---- plan + integrity check via python, emits "tag|mode|args..." lines ----
COMBOS="$(python3 - "$MANIFEST" "$TIER" "$VERIFY_ALL" "$GOLDEN_DIR" <<'PYEOF'
import gzip, hashlib, json, random, sys
manifest, tier, verify_all, gdir = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1", sys.argv[4]
m = json.load(open(manifest))
sel = [g for g in m["goldens"] if g["tier"] <= tier]
if not sel:
    print("NO_COMBOS", file=sys.stderr); sys.exit(2)

# integrity: all tier-0 (cheap) + one random sample per higher tier, or all
to_verify = [g for g in sel if g["tier"] == 0] if not verify_all else list(sel)
if not verify_all:
    rng = random.Random()
    for t in (1, 2):
        pool = [g for g in sel if g["tier"] == t]
        if pool:
            to_verify.append(rng.choice(pool))
for g in to_verify:
    p = f"{gdir}/java_{g['tag']}.txt.gz"
    h = hashlib.sha256()
    with gzip.open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 22), b""):
            h.update(blk)
    if h.hexdigest() != g["sha256_plain"]:
        print(f"GOLDEN INTEGRITY FAILURE: {p}", file=sys.stderr); sys.exit(2)
print(f"golden integrity ok ({len(to_verify)} verified)", file=sys.stderr)

for g in sorted(sel, key=lambda g: (g["tier"], g["tag"])):
    if g["mode"] == "single":
        args = f"--single {g['center'][0]} {g['center'][1]}"
    else:
        args = f"--radius {g['radius']} --center {g['center'][0]} {g['center'][1]}"
    stale = "1" if g.get("summary_stale") else "0"
    if g.get("be"):
        args += " --dump-block-entities"
    if g.get("dimension", "overworld") != "overworld":
        args += f" --dimension {g['dimension']}"
    if g.get("world_type", "default") != "default":
        args += f" --world-type {g['world_type']}"
        if g.get("flat_preset"):
            args += f" --flat-preset {g['flat_preset']}"
        if g.get("single_biome"):
            args += f" --single-biome {g['single_biome']}"
    print(f"{g['tag']}|{stale}|{args} --phases {g['phases']} --seed {g['seed']}")
PYEOF
)" || exit 2

TOTAL=$(echo "$COMBOS" | wc -l | tr -d ' ')
echo "=== Regression: tier<=$TIER, $TOTAL combos, determinism=$DETERMINISM ==="

PASS=0; FAIL=0; FAILED_TAGS=(); RESULTS_JSON=""
i=0
while IFS='|' read -r TAG STALE ARGS; do
    i=$((i+1))
    printf '[%d/%d] %s ... ' "$i" "$TOTAL" "$TAG"
    T0=$(date +%s)
    DET_FLAG=""; [ "$DETERMINISM" = "1" ] && DET_FLAG="--determinism"
    OUT="$("$SCRIPT_DIR/run_full_parity.sh" $ARGS $DET_FLAG \
            --golden "$GOLDEN_DIR" --results "$REG_DIR" 2>&1)"
    RC=$?
    T1=$(date +%s)
    if [ $RC -eq 0 ]; then
        PASS=$((PASS+1)); echo "OK ($((T1-T0))s)"
        # dumps are large and regenerable; keep only summaries on success
        rm -f "$REG_DIR/java_${TAG}.txt" "$REG_DIR/cpp_${TAG}.txt"
    else
        FAIL=$((FAIL+1)); FAILED_TAGS+=("$TAG")
        STALEMARK=""; [ "$STALE" = "1" ] && STALEMARK=" [summary_stale golden — verify against fresh Java before blaming C++]"
        echo "FAIL rc=$RC ($((T1-T0))s)$STALEMARK"
        echo "$OUT" | tail -25 | sed 's/^/    /'
    fi
    RESULTS_JSON="$RESULTS_JSON{\"tag\":\"$TAG\",\"rc\":$RC,\"seconds\":$((T1-T0))},"
done <<< "$COMBOS"

# ---- report ----
python3 - "$REPORT" "$TIER" "$DETERMINISM" "[${RESULTS_JSON%,}]" <<'PYEOF'
import json, sys
report, tier, det, results = sys.argv[1], sys.argv[2], sys.argv[3], json.loads(sys.argv[4])
json.dump({"tier": tier, "determinism": det == "1", "combos": results,
           "pass": sum(1 for r in results if r["rc"] == 0),
           "fail": sum(1 for r in results if r["rc"] != 0)},
          open(report, "w"), indent=1)
PYEOF
echo "Report: $REPORT"

if [ $FAIL -gt 0 ]; then
    echo "=== REGRESSION FAILED: $FAIL/$TOTAL combos (${FAILED_TAGS[*]}) ==="
    exit 1
fi
echo "=== Parity regression PASSED: $PASS/$TOTAL combos ==="

# ---- perf gate ----
if [ "$PERF" = "1" ]; then
    if [ ! -f "$PERF_BASELINE" ]; then
        echo "No perf baseline at $PERF_BASELINE — capturing one now (NOT gating)."
        "$SCRIPT_DIR/perf/capture_perf.sh" --label baseline --out "$PERF_BASELINE" || exit 2
    else
        CAND="$SCRIPT_DIR/perf/perf_${LABEL:-candidate}_${TS}.json"
        "$SCRIPT_DIR/perf/capture_perf.sh" --label "${LABEL:-candidate}" --out "$CAND" || exit 2
        python3 "$SCRIPT_DIR/perf/compare_perf.py" "$PERF_BASELINE" "$CAND" --tolerance "$TOLERANCE" || exit 3
    fi
fi
exit 0
