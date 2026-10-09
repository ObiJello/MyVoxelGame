#!/usr/bin/env python3
"""Read the encoder-counter CSV that Xcode's GPU-capture Performance view
exports (Counters tab -> share button -> "Export Encoder Counters") and
print it transposed: one row per counter, one column per encoder.

    counters_csv.py encoder_counters.csv [--all] [--min 50] [--grep TEXT]

Default: the limiter / utilization rows plus the geometry, overdraw and
memory rows that matter for a tile GPU, with every limiter at or above
--min percent flagged. --all prints every counter (226 distinct on an M4
with Xcode 26.2; the file has 247 columns — 5 labels plus duplicates).
"""

import argparse
import csv
import sys

KEEP = ("Limiter", "Utilization", "Occupancy", "Overdraw", "Primitives", "Vertices",
        "Pixels", "FS Invocations", "Helper", "Bandwidth", "Bytes Read From Device",
        "Bytes Written To Device", "Texture Sample Calls", "Explicit Gradient",
        "Anisotropic", "Mipmap", "PreZ", "Tiles Processed", "Fragments Rasterized")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--min", type=float, default=50.0, help="flag limiters at or above this percent")
    ap.add_argument("--grep", help="only counters containing this text (case-insensitive)")
    args = ap.parse_args()

    rows = list(csv.reader(open(args.csv, newline="")))
    if len(rows) < 2:
        sys.exit("no encoder rows")
    header = rows[0]
    encoders = [r[3] if len(r) > 3 else f"enc{i}" for i, r in enumerate(rows[1:])]
    print("encoders: " + " | ".join(encoders))
    seen = set()
    for i, name in enumerate(header):
        if i < 5 or not name.strip() or name in seen:
            continue
        seen.add(name)
        if args.grep and args.grep.lower() not in name.lower():
            continue
        if not args.all and not any(k in name for k in KEEP):
            continue
        vals = [r[i] if i < len(r) else "" for r in rows[1:]]
        flag = ""
        if "Limiter" in name:
            try:
                if max(float(v) for v in vals if v) >= args.min:
                    flag = "  <-- limiter"
            except ValueError:
                pass
        print(f"{name[:58]:58s} " + " | ".join(f"{v:>14}" for v in vals) + flag)


if __name__ == "__main__":
    main()
