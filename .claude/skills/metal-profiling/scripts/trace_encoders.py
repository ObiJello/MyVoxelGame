#!/usr/bin/env python3
"""Per-process and per-encoder GPU execution from an xctrace export."""
import sys, collections
sys.path.insert(0, sys.path[0])
from _trace_rows import rows

def main(exp, game="MyVoxelGame"):
    labels = {}
    for v in rows(exp + "/metal-object-label.xml"):
        if v[1]: labels[v[1][0]] = v[3][1]
    proc_t = collections.Counter()
    enc = collections.defaultdict(lambda: collections.Counter())
    enc_count = collections.Counter()
    t0, t1 = None, None
    game_frames = set()
    seen_enc = set()
    for v in rows(exp + "/metal-gpu-intervals.xml"):
        if v[0] is None or v[2] is None: continue
        chan = v[2][0]
        if chan not in ("Vertex", "Fragment", "Compute"): continue
        start, dur = int(v[0][0]), int(v[1][0])
        t0 = start if t0 is None else min(t0, start); t1 = max(t1 or 0, start + dur)
        proc = v[10][1] if v[10] else "?"
        pname = proc.split(" (")[0]
        # 'Compute' lifetimes of MoltenVK command buffers are not work; native Metal has none such.
        proc_t[(pname, chan)] += dur
        if pname == game:
            key = v[16][0] if len(v) > 16 and v[16] else None
            lab = labels.get(key, "?")
            if lab.startswith("Target"): lab = "Target " + lab.split("(")[-1].rstrip(")")
            enc[lab][chan] += dur
            if key not in seen_enc:
                seen_enc.add(key); enc_count[lab] += 1
            if v[3] and v[3][0].isdigit(): game_frames.add(int(v[3][0]))
    span = (t1 - t0) / 1e9
    print(f"trace span {span:.1f} s")
    print("GPU execution by process (ms per second of trace = % of one channel):")
    for (p, c), d in sorted(proc_t.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  {p[:28]:28s} {c:9s} {d / 1e6 / span:8.1f} ms/s")
    nf = len(game_frames)
    print(f"game: {nf} frames ({nf / span:.0f} fps in trace)")
    print(f"  {'encoder':26s} {'n/frame':>8s} {'vertex ms/fr':>13s} {'frag ms/fr':>11s}")
    for lab, c in sorted(enc.items(), key=lambda kv: -sum(kv[1].values())):
        print(f"  {lab[:26]:26s} {enc_count[lab] / nf:8.2f} {c['Vertex'] / 1e6 / nf:13.3f} {c['Fragment'] / 1e6 / nf:11.3f}"
              + (f"  compute {c['Compute'] / 1e6 / nf:.3f}" if c['Compute'] else ""))

main(sys.argv[1])
