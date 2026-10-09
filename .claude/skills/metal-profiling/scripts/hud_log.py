#!/usr/bin/env python3
"""Summarize the Metal Performance HUD's log.

With MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 the HUD (libMTLHud, in-process)
writes one NSLog line about twice a second to the process's STDERR — not to
a unified-log subsystem that `log show` can filter, and `open` drops stderr.
Capture it with `open --stderr <file>` (what `tools/play.sh --hud` does) or
by running the bundle's executable directly:

    ... MyVoxelGame[pid:tid] metal-HUD: <frame>,<Metal MB>,<app MB>,<fi>,<gpu>,<fi>,<gpu>,...

The first three numbers are the frame counter and the HUD's memory
figures; the rest are per-PRESENTED-frame pairs of (frame interval ms,
GPU time ms) since the previous line. Per-encoder times are only drawn in
the overlay (MTL_HUD_ENCODER_TIMING_ENABLED=1), not logged.

    hud_log.py stderr.log [--from SEC] [--to SEC]

prints fps and GPU-time statistics over all frames in the window (seconds
from the first line). Remember that with vsync off the mailbox renders
several frames per present: the HUD's GPU time per presented frame is the
sum of the frames behind that present.
"""

import argparse
import re
import statistics
import sys


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--from", dest="t0", type=float, default=0.0, help="window start, seconds from the first line")
    ap.add_argument("--to", dest="t1", type=float, default=1e9, help="window end")
    args = ap.parse_args()

    intervals, gpu, mem = [], [], []
    first = None
    for line in open(args.log, errors="replace"):
        m = re.search(r"^(\S+ \S+).*metal-HUD:\s*(.*)$", line)
        if not m:
            continue
        stamp = m.group(1)
        try:
            h, mi, s = stamp.split(" ")[1].split(":")
            t = int(h) * 3600 + int(mi) * 60 + float(s)
        except ValueError:
            t = 0.0
        first = t if first is None else first
        rel = t - first
        if rel < args.t0 or rel > args.t1:
            continue
        vals = []
        for f in m.group(2).split(","):
            try:
                vals.append(float(f))
            except ValueError:
                pass
        if len(vals) < 5:
            continue
        mem.append((vals[1], vals[2]))
        pairs = vals[3:]
        for i in range(0, len(pairs) - 1, 2):
            intervals.append(pairs[i])
            gpu.append(pairs[i + 1])
    if not intervals:
        sys.exit("no metal-HUD lines in the window")
    n = len(intervals)
    mean_int = statistics.fmean(intervals)
    print(f"{n} presented frames; Metal {mem[-1][0]:.0f} MB, app {mem[-1][1]:.0f} MB (last line)")
    print(f"presented fps: mean {1000 / mean_int:.1f}   1%-low {1000 / pct(intervals, .99):.1f}   0.1%-low {1000 / pct(intervals, .999):.1f}")
    print(f"frame interval ms: mean {mean_int:.2f}  p50 {pct(intervals, .5):.2f}  p95 {pct(intervals, .95):.2f}  "
          f"p99 {pct(intervals, .99):.2f}  max {max(intervals):.2f}")
    print(f"GPU time ms (per presented frame): mean {statistics.fmean(gpu):.2f}  p50 {pct(gpu, .5):.2f}  "
          f"p95 {pct(gpu, .95):.2f}  p99 {pct(gpu, .99):.2f}  max {max(gpu):.2f}")
    busy = sum(gpu) / sum(intervals) * 100 if sum(intervals) else 0
    print(f"GPU time / wall time: {busy:.0f}%  (over 100% = several rendered frames per present, summed)")


if __name__ == "__main__":
    main()
