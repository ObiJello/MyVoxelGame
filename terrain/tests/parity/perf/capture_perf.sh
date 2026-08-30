#!/bin/bash
# capture_perf.sh - capture a performance measurement of the C++ pipeline.
#
# Canonical workload: r10 @ (0,0), seed 12345, phases 3-7, full dump (the same
# workload as the primary golden, so a parity run and a perf run see identical
# work). Runs REPS times (default 3) and stores the median-by-generation-time
# run's JSON, augmented with host info and all rep timings.
#
# Usage: ./capture_perf.sh [--label name] [--out file] [--reps N]
#                          [--radius R] [--seed S] [--center X Z]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARITY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(cd "$PARITY_DIR/../.." && pwd)/build"

LABEL="perf"; OUT=""; REPS=3; RADIUS=10; SEED=12345; CX=0; CZ=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --label) LABEL="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --radius) RADIUS="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --center) CX="$2"; CZ="$3"; shift 3 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done
[ -z "$OUT" ] && OUT="$SCRIPT_DIR/perf_${LABEL}.json"

if [ ! -x "$BUILD_DIR/async_chunk_test" ]; then
    (cd "$BUILD_DIR" && make async_chunk_test -j8 >/dev/null)
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/perf_capture.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

echo "=== Perf capture: r$RADIUS @ ($CX,$CZ) seed $SEED, $REPS reps ==="
for i in $(seq 1 "$REPS"); do
    "$BUILD_DIR/async_chunk_test" --radius "$RADIUS" --center "$CX" "$CZ" \
        --phases 3-7 --seed "$SEED" --dump-full \
        --output "$TMP/dump_$i.txt" --perf-json "$TMP/rep_$i.json" --quiet >/dev/null
    rm -f "$TMP/dump_$i.txt"
    python3 -c "import json;d=json.load(open('$TMP/rep_$i.json'));print(f'  rep $i: {d[\"generation_seconds\"]:.2f}s gen, {d[\"ms_per_chunk\"]:.2f} ms/chunk, rss {d[\"max_rss_bytes\"]/1e9:.2f} GB')"
done

python3 - "$TMP" "$REPS" "$OUT" "$LABEL" <<'PYEOF'
import json, platform, subprocess, sys
from datetime import datetime, timezone
tmp, reps, out, label = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
runs = [json.load(open(f"{tmp}/rep_{i}.json")) for i in range(1, reps + 1)]
runs.sort(key=lambda r: r["generation_seconds"])
median = dict(runs[len(runs) // 2])
median.update(
    label=label,
    captured=datetime.now(timezone.utc).isoformat(timespec="seconds"),
    reps=len(runs),
    all_generation_seconds=sorted(r["generation_seconds"] for r in runs),
    all_ms_per_chunk=sorted(r["ms_per_chunk"] for r in runs),
    host={
        "cpu": subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                              capture_output=True, text=True).stdout.strip(),
        "os": f"{platform.system()} {platform.release()}",
    },
)
json.dump(median, open(out, "w"), indent=1)
print(f"Wrote {out}: median {median['generation_seconds']:.2f}s, "
      f"{median['ms_per_chunk']:.2f} ms/chunk")
PYEOF
