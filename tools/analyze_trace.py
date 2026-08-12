#!/usr/bin/env python3
"""
Analyse a Tracy .tracy capture without opening the GUI.

Tracy's timeline is great for looking at ONE frame, but bad at the question we
actually keep asking: "this zone spiked once — how often does that happen, and
what else was running when it did?"  That needs every instance of every zone,
which is what `tracy-csvexport -u` dumps and what this script chews on.

    python3 tools/analyze_trace.py capture.tracy
    python3 tools/analyze_trace.py capture.tracy --zone PollEvents
    python3 tools/analyze_trace.py capture.tracy --zone PollEvents --factor 10

Three reports:

  1. SUMMARY   — every zone by total time, with a "spike" column (max / mean).
                 A high spike ratio with a low mean is a STALL, not a cost;
                 those are the rows worth chasing.
  2. OUTLIERS  — every instance of the focus zone above `factor` x median,
                 with when it happened.
  3. OVERLAP   — what ran on OTHER threads during those outliers, ranked by how
                 much of the stall window each zone covers. This is the part
                 the GUI cannot answer without a lot of manual scrubbing.

Needs tracy-csvexport. Built at cmake-build-tracy/tools/ (see CLAUDE.md); pass
--csvexport to point somewhere else.
"""

import argparse
import bisect
import os
import shutil
import subprocess
import sys
from collections import defaultdict

SEP = "\t"  # zone names can contain commas; tabs they cannot.

# tracy-csvexport -u column order (csvexport.cpp:408). Asserted at runtime
# against the real header so a Tracy upgrade that reorders columns fails loudly
# instead of silently mis-parsing.
UNWRAP_COLUMNS = [
    "name", "src_file", "src_line",
    "ns_since_start", "exec_time_ns", "thread", "value",
]


def find_csvexport(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit(f"error: --csvexport path does not exist: {explicit}")
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, "..", "cmake-build-tracy", "tools", "tracy-csvexport"),
        os.path.join(here, "tracy", "tracy-csvexport"),
    ]
    for c in candidates:
        c = os.path.normpath(c)
        if os.path.isfile(c):
            return c
    found = shutil.which("tracy-csvexport")
    if found:
        return found
    sys.exit(
        "error: tracy-csvexport not found.\n"
        "  Build it from the fetched Tracy source:\n"
        "    cd cmake-build-tracy/_deps/tracy-src/csvexport\n"
        "    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\n"
        "  Then copy it to cmake-build-tracy/tools/, or pass --csvexport."
    )


def run_export(csvexport, trace, unwrap, zone_filter=None):
    """Yield parsed rows. Streams — traces get big."""
    cmd = [csvexport, "-s", SEP]
    if unwrap:
        cmd.append("-u")
    if zone_filter:
        cmd += ["-f", zone_filter]
    cmd.append(trace)

    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
    )
    header = proc.stdout.readline().rstrip("\n")
    if not header:
        err = proc.stderr.read().strip()
        sys.exit(f"error: tracy-csvexport produced no output.\n{err}")
    cols = header.split(SEP)

    if unwrap and cols[:len(UNWRAP_COLUMNS)] != UNWRAP_COLUMNS:
        sys.exit(
            "error: unexpected tracy-csvexport column layout.\n"
            f"  expected: {UNWRAP_COLUMNS}\n"
            f"  got:      {cols}\n"
            "  A Tracy upgrade probably reordered them; update UNWRAP_COLUMNS."
        )

    for line in proc.stdout:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split(SEP)
        if len(parts) < len(cols):
            continue
        yield dict(zip(cols, parts))

    proc.stdout.close()
    rc = proc.wait()
    if rc != 0:
        sys.exit(f"error: tracy-csvexport exited {rc}\n{proc.stderr.read().strip()}")


def ms(ns):
    return float(ns) / 1e6


def fmt_time(ns):
    ns = float(ns)
    if ns >= 1e6:
        return f"{ns / 1e6:8.2f} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:8.2f} us"
    return f"{ns:8.0f} ns"


def report_summary(csvexport, trace, top):
    print("\n" + "=" * 78)
    print("SUMMARY  — spike = max/mean. High spike + low mean = a STALL, not a cost.")
    print("=" * 78)

    rows = []
    totals = {}   # zone -> total_ns, the baseline denominator for OVERLAP
    for r in run_export(csvexport, trace, unwrap=False):
        try:
            counts = int(r["counts"])
            mean = float(r["mean_ns"])
            mx = float(r["max_ns"])
            total = float(r["total_ns"])
        except (KeyError, ValueError):
            continue
        rows.append((total, r["name"], counts, mean, mx,
                     float(r.get("min_ns", 0)), float(r.get("std_ns", 0))))
        totals[r["name"]] = total

    if not rows:
        print("  (no zone data)")
        return totals

    rows.sort(reverse=True)
    print(f"\n{'zone':<26}{'count':>8}{'mean':>13}{'max':>13}{'total':>13}{'spike':>9}")
    print("-" * 78)
    for total, name, counts, mean, mx, mn, std in rows[:top]:
        spike = (mx / mean) if mean > 0 else 0
        flag = "  <<<" if spike >= 50 and mean < 1e6 else ""
        print(f"{name[:25]:<26}{counts:>8}{fmt_time(mean):>13}{fmt_time(mx):>13}"
              f"{fmt_time(total):>13}{spike:>8.0f}x{flag}")
    print("\n  '<<<' = mean is sub-millisecond but max is >=50x it. Those are stalls:")
    print("        the code is not slow, something blocked it. Chase with --zone.")
    return totals


def collect_zone(csvexport, trace, zone):
    """Every instance of `zone`. Exact name match — the -f filter is a substring."""
    out = []
    for r in run_export(csvexport, trace, unwrap=True, zone_filter=zone):
        if r["name"] != zone:
            continue
        try:
            out.append((int(r["ns_since_start"]), int(r["exec_time_ns"]), r["thread"]))
        except ValueError:
            continue
    out.sort()
    return out


def report_outliers(instances, zone, factor):
    print("\n" + "=" * 78)
    print(f"OUTLIERS — {zone}")
    print("=" * 78)

    if not instances:
        print(f"  no instances of '{zone}' found (exact name match)")
        return []

    durations = sorted(d for _, d, _ in instances)
    n = len(durations)
    median = durations[n // 2]
    p99 = durations[min(n - 1, int(n * 0.99))]

    print(f"\n  instances : {n}")
    print(f"  median    : {fmt_time(median)}")
    print(f"  p99       : {fmt_time(p99)}")
    print(f"  max       : {fmt_time(durations[-1])}")

    if median <= 0:
        print("\n  median is 0 — cannot compute a ratio; using p99 as the threshold.")
        threshold = max(p99, 1)
    else:
        threshold = median * factor

    outliers = [(s, d, t) for s, d, t in instances if d >= threshold]
    total_stall = sum(d for _, d, _ in outliers)

    print(f"\n  threshold : {fmt_time(threshold)}  ({factor}x median)")
    print(f"  outliers  : {len(outliers)}  ({100.0 * len(outliers) / n:.2f}% of instances)")
    print(f"  time lost : {fmt_time(total_stall)} total in outliers")

    if outliers:
        print(f"\n  {'at (s)':>12}{'duration':>13}   thread")
        print("  " + "-" * 60)
        for s, d, t in sorted(outliers, key=lambda x: -x[1])[:25]:
            print(f"  {s / 1e9:>12.3f}{fmt_time(d):>13}   {t}")
        if len(outliers) > 25:
            print(f"  ... and {len(outliers) - 25} more")

    return outliers


def report_overlap(csvexport, trace, outliers, zone, max_windows, baseline):
    """What ran on OTHER threads during the stalls, versus its normal rate.

    Raw coverage alone is a trap. If a zone runs 80% of the session anyway, then
    "it covered 80% of the stalls" means nothing — you would see that whatever you
    correlated against. The LIFT column divides in-stall coverage by whole-session
    coverage, so 1.0x is 'exactly as usual' and only clearly-above-1 is a signal.
    """
    print("\n" + "=" * 78)
    print(f"OVERLAP  — what ran during {zone} stalls, vs its normal rate")
    print("=" * 78)

    if not outliers:
        print("  (no outliers to correlate)")
        return

    windows = sorted(outliers, key=lambda x: -x[1])[:max_windows]
    stall_threads = {t for _, _, t in windows}
    windows = [(s, s + d) for s, d, _ in windows]
    windows.sort()
    starts = [w[0] for w in windows]
    total_window = sum(e - s for s, e in windows)

    print(f"\n  correlating against the {len(windows)} longest stalls "
          f"({fmt_time(total_window)} of wall time)\n")

    covered = defaultdict(int)
    hits = defaultdict(int)
    threads = defaultdict(set)
    span_ns = 0  # session length, for the baseline denominator

    for r in run_export(csvexport, trace, unwrap=True):
        name = r["name"]
        try:
            s = int(r["ns_since_start"])
            e = s + int(r["exec_time_ns"])
        except ValueError:
            continue
        if e > span_ns:
            span_ns = e
        if name == zone:
            continue

        # Windows are disjoint and sorted; find the last one starting at/before e.
        i = bisect.bisect_right(starts, e) - 1
        while i >= 0:
            ws, we = windows[i]
            if we <= s:
                break
            ov = min(e, we) - max(s, ws)
            if ov > 0:
                covered[name] += ov
                hits[name] += 1
                threads[name].add(r["thread"])
            i -= 1

    if not covered:
        print("  Nothing else ran during the stalls.")
        print("  That is a real result: the main thread was blocked with the rest of")
        print("  the process idle, which points OUTSIDE the process (WindowServer,")
        print("  the GPU driver, or the kernel) rather than at contention.")
        return

    # Rank by LIFT, not raw coverage — the whole point is what is ABNORMAL during
    # a stall. Zones with a trivial in-window presence are noise at any lift.
    rows = []
    for name, ov in covered.items():
        in_pct = 100.0 * ov / total_window
        base_pct = (100.0 * baseline.get(name, 0) / span_ns) if span_ns else 0.0
        lift = (in_pct / base_pct) if base_pct > 0.01 else float("inf")
        rows.append((lift, in_pct, base_pct, name, ov))

    rows.sort(key=lambda r: (-r[0], -r[1]))

    print(f"  {'zone':<24}{'in-stall':>10}{'baseline':>10}{'lift':>8}{'hits':>7}  threads")
    print("  " + "-" * 76)
    for lift, in_pct, base_pct, name, ov in rows[:20]:
        if in_pct < 1.0:
            continue
        th = ", ".join(sorted(threads[name])[:2])
        if len(threads[name]) > 2:
            th += f" +{len(threads[name]) - 2}"
        mark = " *" if set(threads[name]) & stall_threads else ""
        lift_s = "  inf" if lift == float("inf") else f"{lift:5.2f}x"
        flag = "  <<<" if lift >= 2.0 and in_pct >= 10.0 else ""
        print(f"  {name[:23]:<24}{in_pct:>9.1f}%{base_pct:>9.1f}%{lift_s:>8}"
              f"{hits[name]:>7}  {th}{mark}{flag}")

    print("\n  in-stall = % of stall time this zone was running (>100% = several threads).")
    print("  baseline = the same figure across the WHOLE session.")
    print("  lift     = in-stall / baseline. **1.0x means no relationship at all** —")
    print("             the zone was simply running, as it always is. Only '<<<'")
    print("             (lift >=2x with real presence) is evidence of anything.")
    print("  '*' = same thread as the stall: a NESTED cost, not contention.")


def main():
    ap = argparse.ArgumentParser(
        description="Analyse a Tracy .tracy capture from the command line.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("trace", help="path to a .tracy capture")
    ap.add_argument("--zone", help="zone to hunt outliers in (e.g. PollEvents)")
    ap.add_argument("--factor", type=float, default=20.0,
                    help="outlier threshold as a multiple of the median (default 20)")
    ap.add_argument("--top", type=int, default=30, help="summary rows (default 30)")
    ap.add_argument("--windows", type=int, default=10,
                    help="how many of the worst stalls to correlate (default 10)")
    ap.add_argument("--csvexport", help="path to tracy-csvexport")
    args = ap.parse_args()

    if not os.path.isfile(args.trace):
        sys.exit(f"error: no such trace file: {args.trace}")

    csvexport = find_csvexport(args.csvexport)
    print(f"trace      : {args.trace}")
    print(f"csvexport  : {csvexport}")

    baseline = report_summary(csvexport, args.trace, args.top) or {}

    if args.zone:
        instances = collect_zone(csvexport, args.trace, args.zone)
        outliers = report_outliers(instances, args.zone, args.factor)
        report_overlap(csvexport, args.trace, outliers, args.zone,
                       args.windows, baseline)
    else:
        print("\n  Pass --zone <name> to hunt outliers and see what overlapped them.")
    print()


if __name__ == "__main__":
    main()
