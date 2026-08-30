#!/usr/bin/env python3
"""compare_perf.py - gate a candidate perf capture against a baseline.

Usage: compare_perf.py baseline.json candidate.json [--tolerance 0.05]

Fails (exit 3) if the candidate's median ms/chunk is more than `tolerance`
slower than the baseline's. Improvement is reported but never fails.
Workload identity (seed/radius/center/phases) must match (exit 2 otherwise).
"""
import argparse
import json
import sys

p = argparse.ArgumentParser()
p.add_argument("baseline")
p.add_argument("candidate")
p.add_argument("--tolerance", type=float, default=0.05)
args = p.parse_args()

base = json.load(open(args.baseline))
cand = json.load(open(args.candidate))

for key in ("seed", "mode", "radius", "center", "phases", "chunks"):
    if base.get(key) != cand.get(key):
        print(f"PERF COMPARE ERROR: workload mismatch on '{key}': "
              f"{base.get(key)} vs {cand.get(key)}")
        sys.exit(2)

b, c = base["ms_per_chunk"], cand["ms_per_chunk"]
delta = (c - b) / b
rss_b, rss_c = base.get("max_rss_bytes"), cand.get("max_rss_bytes")

print(f"perf: baseline {b:.2f} ms/chunk ({base.get('label', '?')}) -> "
      f"candidate {c:.2f} ms/chunk ({cand.get('label', '?')}) "
      f"[{delta:+.1%}]")
if rss_b and rss_c:
    print(f"rss:  {rss_b / 1e9:.2f} GB -> {rss_c / 1e9:.2f} GB "
          f"[{(rss_c - rss_b) / rss_b:+.1%}]")

if delta > args.tolerance:
    print(f"PERF REGRESSION: {delta:+.1%} exceeds tolerance {args.tolerance:.0%}")
    sys.exit(3)
print("perf gate OK")
