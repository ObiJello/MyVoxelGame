#!/usr/bin/env python3
"""Paired A/B over an ALTERNATING run series (A B A B ...): the mean of the
per-pair fps deltas with a 95 % t interval, so a cross-build or cross-
backend comparison — where no phase switch can alternate inside one run —
gets a verdict that cancels the slow thermal drift between adjacent runs.
Input: the results file ab_run series write ("== label ..." lines followed
by the report's "frames in" line). Usage: pair_runs.py <file> <A-tag> <B-tag>
"""
import re, sys
from math import sqrt
text = open(sys.argv[1]).read()
a_tag, b_tag = sys.argv[2], sys.argv[3]
runs = re.findall(r"== (\S+) (\S+) (.*?)\n.*?= ([0-9.]+) fps +1%-low ([0-9.]+) fps", text, re.S)
series = {}
for label, backend, extra, fps, low in runs:
    key = extra.strip()
    series.setdefault(key, []).append((label, backend, float(fps), float(low)))
T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31}
for key, rs in series.items():
    rs = [r for r in rs if not r[0].startswith("warm")]
    pairs = []
    for i in range(0, len(rs) - 1, 2):
        a, b = rs[i], rs[i + 1]
        if a_tag in a[1] and b_tag in b[1]:
            pairs.append((100 * (b[2] / a[2] - 1), 100 * (b[3] / a[3] - 1), a, b))
    if not pairs:
        continue
    n = len(pairs)
    for which, idx in (("fps", 0), ("1%-low", 1)):
        d = [p[idx] for p in pairs]
        m = sum(d) / n
        sd = sqrt(sum((x - m) ** 2 for x in d) / (n - 1)) if n > 1 else float("nan")
        ci = T95.get(n - 1, 2.2) * sd / sqrt(n) if n > 1 else float("nan")
        verdict = "REAL" if n > 1 and abs(m) > ci else "within noise"
        print(f"{key:12s} {which:7s} {b_tag} vs {a_tag}: {m:+.1f}% ± {ci:.1f}% ({n} pairs) -> {verdict}")
    for fa, fl, a, b in pairs:
        print(f"    {a[0]:12s} {a[2]:6.1f} fps  {b[0]:12s} {b[2]:6.1f} fps  delta {fa:+.1f}%")
