#!/usr/bin/env python3
"""Whole-capture report for a Tracy trace — every thread, frame, tick and plot.

    tools/tracy_report.py capture.tracy                  # one capture
    tools/tracy_report.py gl.tracy vk.tracy              # A/B comparison
    tools/tracy_report.py gl.tracy vk.tracy --full       # comparison + both full reports
    tools/tracy_report.py capture.tracy --window all     # whole capture, loading included
    tools/tracy_report.py capture.tracy --window 10:40   # seconds 10..40 of the capture

Reads the capture through tracy-export (tools/tracy_tools.py builds the tools
at the Tracy version CMakeLists.txt pins and caches the export per capture).

WINDOW: a capture with the replay harness's `Replay/Time` plot (--replay) is
cut to the replayed path (replay time >= 0), so two runs of one recording
compare like for like; anything else defaults to the whole capture.

Sections: capture info; frame pacing from the FrameMark frames (the real frame
times, not a zone proxy); the main thread's per-frame budget by EXACT self time
(sum of self times = the frame's covered time, no double counting) with p95 per
frame; the worst frames and what filled them; the server tick (TPS, work per
tick without the park, per-tick budget, worst ticks); every worker pool
(busy %, top zones); every plot (engine counters, GPU pass timings, CPU usage);
and GPU zones, locks, messages, memory and samples when the capture has them.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tracy_tools  # noqa: E402

W = 110
MAIN_NAMES = ("Main thread",)
SERVER_NAMES = ("ServerThread",)
SERVER_IDLE = {"Server.Park"}          # the server's sleep until the next tick
FRAME_WAITS = {"Present", "Vk.FenceWait", "Vk.Acquire"}  # swap / vsync / GPU back-pressure on the main thread
WAIT_ZONES = SERVER_IDLE | FRAME_WAITS  # left out of "busy"


def pct(sorted_xs, p):
    if not sorted_xs:
        return 0.0
    return sorted_xs[min(len(sorted_xs) - 1, int(p * len(sorted_xs)))]


def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


def ms(ns):
    return ns / 1e6


def fmt_num(v):
    a = abs(v)
    if a >= 1e9: return f"{v / 1e9:.2f}G"
    if a >= 1e6: return f"{v / 1e6:.2f}M"
    if a >= 1e4: return f"{v / 1e3:.1f}k"
    if a == int(v): return f"{int(v)}"
    return f"{v:.2f}"


def dur(ns):
    if ns >= 1e6: return f"{ns / 1e6:.1f}ms"
    if ns >= 1e3: return f"{ns / 1e3:.0f}us"
    return f"{ns:.0f}ns"


def header(title):
    print("=" * W)
    print(title)


# ── Analysis ────────────────────────────────────────────────────────────────

class Summary:
    """Everything the report prints, for one capture inside one time window."""

    def __init__(self, ex, window_arg):
        self.ex = ex
        self.name = os.path.basename(ex.info.get("trace_path", str(ex.path)))
        self.window, self.window_desc = self._window(window_arg)
        w0, w1 = self.window
        self.span_ns = w1 - w0

        # Threads by role.
        self.main = next((t["tid"] for t in ex.threads if t["name"] in MAIN_NAMES), None)
        self.server = next((t["tid"] for t in ex.threads if t["name"] in SERVER_NAMES), None)

        # Frame sets, cut to the window.
        self.frames = {}
        for fs in ex.frame_set_names():
            self.frames[fs] = [(i, s, e) for i, (s, e) in enumerate(ex.frames(fs)) if s >= w0 and e <= w1]
        self.main_set = "Frames" if "Frames" in self.frames else None
        self.tick_set = "ServerTick" if "ServerTick" in self.frames else None

        # Per-frame zone sums for the frames in the window.
        keep = {}
        for fs, frs in self.frames.items():
            keep[fs] = {i for i, _, _ in frs}
        # per_frame[set][(tid, name)] = {frame: [count, total, self, max]}
        self.per_frame = collections.defaultdict(lambda: collections.defaultdict(dict))
        for r in ex.rows("frame_zones"):
            fs = r["frame_set"]
            fi = int(r["frame"])
            if fi not in keep.get(fs, ()):
                continue
            self.per_frame[fs][(int(r["thread"]), r["name"])][fi] = (
                int(r["count"]), int(r["total_ns"]), int(r["self_ns"]), int(r["max_ns"]))

        # Plot samples in the window.
        self.plots = {}
        for name, pts in ex.plots().items():
            vals = [v for t, v in pts if w0 <= t <= w1]
            if vals:
                self.plots[name] = vals
        self.plot_meta = {p["name"]: p for p in ex.info.get("plots", [])}

    def _window(self, arg):
        ex = self.ex
        last = ex.info.get("last_time_ns", 0)
        if arg == "all":
            return (0, last), "whole capture"
        if arg not in (None, "replay"):
            a, b = arg.split(":")
            s = int(float(a) * 1e9) if a else 0
            e = int(float(b) * 1e9) if b else last
            return (s, e), f"{s / 1e9:.1f}s..{e / 1e9:.1f}s"
        replay = ex.plots(["Replay/Time"]).get("Replay/Time")
        if replay:
            path = [t for t, v in replay if v >= 0]
            if path:
                return (path[0], path[-1]), (f"replayed path (Replay/Time >= 0): {path[0] / 1e9:.1f}s..{path[-1] / 1e9:.1f}s"
                                             f" of the capture")
        if arg == "replay":
            print(f"  note: {self.name} has no Replay/Time plot — using the whole capture")
        return (0, last), "whole capture"

    # Frame pacing -----------------------------------------------------------
    def frame_stats(self, frame_set):
        frs = self.frames.get(frame_set) or []
        d = sorted(e - s for _, s, e in frs)
        if not d:
            return None
        worst1 = d[-max(1, len(d) // 100):]
        return {
            "n": len(d), "mean": mean(d), "p50": pct(d, .5), "p90": pct(d, .9), "p95": pct(d, .95),
            "p99": pct(d, .99), "max": d[-1], "low1_fps": 1e9 / mean(worst1),
            "fps": len(d) / (self.span_ns / 1e9) if self.span_ns else 0,
            "over16": sum(1 for x in d if x > 16.7e6), "over33": sum(1 for x in d if x > 33.3e6),
            "over50": sum(1 for x in d if x > 50e6),
        }

    # Budget of one thread per frame of a set ------------------------------------
    def budget(self, frame_set, tid):
        """{name: stats of its per-frame self / total time} over every frame in the window."""
        frs = self.frames.get(frame_set) or []
        n = len(frs)
        out = {}
        if not n or tid is None:
            return out
        for (t, name), per in self.per_frame[frame_set].items():
            if t != tid:
                continue
            selfs = sorted([v[2] for v in per.values()] + [0] * (n - len(per)))
            totals = [v[1] for v in per.values()]
            out[name] = {
                "self_mean": sum(selfs) / n, "self_p95": pct(selfs, .95), "self_max": selfs[-1],
                "total_mean": sum(totals) / n, "calls": sum(v[0] for v in per.values()) / n,
                "zone_max": max(v[3] for v in per.values()),
            }
        return out

    def frame_self(self, frame_set, tid, names=None, exclude=()):
        """{frame index: sum of self time on `tid`} (optionally only / except some names)."""
        out = collections.defaultdict(int)
        for (t, name), per in self.per_frame[frame_set].items():
            if t != tid or name in exclude or (names is not None and name not in names):
                continue
            for fi, v in per.items():
                out[fi] += v[2]
        return out

    def frame_top(self, frame_set, tid, fi, k=5, exclude=()):
        items = []
        for (t, name), per in self.per_frame[frame_set].items():
            if t == tid and fi in per and name not in exclude:
                items.append((per[fi][2], name, per[fi][0]))
        items.sort(reverse=True)
        return items[:k]

    # Thread pools ---------------------------------------------------------------
    def pools(self):
        """{thread name: {"threads": [tid], "busy": ns, "zones": {name: [count, total, self, max]}}}
        from the main frame set's per-frame sums (zones on every thread are
        attributed to the frame their start falls in, so this is windowed)."""
        fs = self.main_set or next(iter(self.per_frame), None)
        out = {}
        if fs is None:
            return out
        names = self.ex.thread_names
        for (tid, zname), per in self.per_frame[fs].items():
            g = out.setdefault(names.get(tid, str(tid)), {"threads": set(), "busy": 0, "zones": {}})
            g["threads"].add(tid)
            z = g["zones"].setdefault(zname, [0, 0, 0, 0])
            for c, tot, sf, mx in per.values():
                z[0] += c; z[1] += tot; z[2] += sf; z[3] = max(z[3], mx)
                if zname not in WAIT_ZONES:
                    g["busy"] += sf
        # Threads that exist but ran nothing in the window.
        for t in self.ex.threads:
            g = out.get(t["name"])
            if g is not None:
                g["threads"].add(t["tid"])
        return out

    def plot_stats(self, name):
        v = sorted(self.plots.get(name, []))
        if not v:
            return None
        return {"n": len(v), "min": v[0], "mean": mean(v), "p50": pct(v, .5), "p95": pct(v, .95), "max": v[-1]}


# ── Single-capture report ─────────────────────────────────────────────────────

def report(s):
    ex, info = s.ex, s.ex.info
    header(f"CAPTURE  {s.name}")
    host = dict(l.split(": ", 1) for l in info.get("host_info", "").splitlines() if ": " in l)
    print(f"  program {info.get('capture_program')}   captured {info.get('capture_name', '').split('@')[-1].strip()}"
          f"   length {info.get('last_time_ns', 0) / 1e9:.1f}s   Tracy {info.get('trace_file_version')}")
    print(f"  host {host.get('OS', '?')}, {host.get('Arch', '?')}, {host.get('CPU cores', '?')} cores,"
          f" {host.get('RAM', '?')}   zones {info['counts']['zones']:,}   threads {info['counts']['threads']}")
    print(f"  window: {s.window_desc}")

    # ── Frames
    fs = s.frame_stats(s.main_set) if s.main_set else None
    header("FRAMES  (FrameMark — the real frame boundaries)")
    if not fs:
        print("  no FrameMark frames in the window")
    else:
        print(f"  {fs['n']} frames in {s.span_ns / 1e9:.1f}s = {fs['fps']:.1f} fps   1%-low {fs['low1_fps']:.1f} fps")
        print(f"  frame time ms: mean {ms(fs['mean']):.2f}  p50 {ms(fs['p50']):.2f}  p90 {ms(fs['p90']):.2f}  "
              f"p95 {ms(fs['p95']):.2f}  p99 {ms(fs['p99']):.2f}  max {ms(fs['max']):.1f}")
        print(f"  frames >16.7ms {fs['over16']} ({100 * fs['over16'] / fs['n']:.1f}%)   >33.3ms {fs['over33']}   "
              f">50ms {fs['over50']}")

    # ── Main thread budget
    if s.main_set and s.main is not None:
        b = s.budget(s.main_set, s.main)
        n = len(s.frames[s.main_set])
        covered = sum(v["self_mean"] for v in b.values())
        waits = sum(b[x]["self_mean"] for x in FRAME_WAITS if x in b)
        header(f"MAIN THREAD per-frame budget ({ex.thread_label(s.main)}) — EXACT self time per frame, {n} frames")
        print(f"  zoned time per frame {ms(covered):.2f} ms of {ms(fs['mean']) if fs else 0:.2f} ms;"
              f" of it waiting in {'/'.join(sorted(FRAME_WAITS))} {ms(waits):.2f} ms -> CPU work {ms(covered - waits):.2f} ms")
        print(f"  {'zone':38s} {'self/frame':>10s} {'share':>6s} {'p95':>8s} {'max':>8s} {'calls/fr':>9s} {'incl/frame':>10s}")
        for name, v in sorted(b.items(), key=lambda kv: -kv[1]["self_mean"])[:30]:
            print(f"  {name[:38]:38s} {ms(v['self_mean']):9.3f}m {100 * v['self_mean'] / covered if covered else 0:5.1f}%"
                  f" {ms(v['self_p95']):7.2f}m {ms(v['self_max']):7.2f}m {v['calls']:9.1f} {ms(v['total_mean']):9.3f}m")
        # Worst frames.
        frs = sorted(s.frames[s.main_set], key=lambda f: -(f[2] - f[1]))[:10]
        print("  worst frames (what the main thread's self time went to):")
        for fi, st_, en in frs:
            top = ", ".join(f"{nm} {ms(sf):.1f}" + (f"×{c}" if c > 1 else "")
                            for sf, nm, c in s.frame_top(s.main_set, s.main, fi, 5))
            print(f"     t={(st_ - s.window[0]) / 1e9:7.2f}s  {ms(en - st_):7.1f} ms   {top}")

    # ── Server
    if s.tick_set and s.server is not None:
        ts = s.frame_stats(s.tick_set)
        header(f"SERVER TICK ({ex.thread_label(s.server)}) — 50 ms budget, {ts['n'] if ts else 0} ticks")
        if ts:
            work = s.frame_self(s.tick_set, s.server, exclude=SERVER_IDLE)
            wv = sorted(work.get(i, 0) for i, _, _ in s.frames[s.tick_set])
            print(f"  TPS {ts['fps']:.2f}   tick interval ms: p50 {ms(ts['p50']):.1f}  p99 {ms(ts['p99']):.1f}  max {ms(ts['max']):.1f}")
            print(f"  work per tick (self time minus {'/'.join(SERVER_IDLE)}) ms: mean {ms(mean(wv)):.2f}  p50 {ms(pct(wv, .5)):.2f}"
                  f"  p95 {ms(pct(wv, .95)):.2f}  p99 {ms(pct(wv, .99)):.2f}  max {ms(wv[-1]):.2f}"
                  f"   ticks >25ms {sum(1 for x in wv if x > 25e6)}  >50ms {sum(1 for x in wv if x > 50e6)}")
            b = s.budget(s.tick_set, s.server)
            print(f"  {'zone':38s} {'self/tick':>10s} {'p95':>8s} {'max':>8s} {'calls/tick':>10s} {'incl/tick':>10s}")
            for name, v in sorted(b.items(), key=lambda kv: -kv[1]["self_mean"])[:22]:
                if name in SERVER_IDLE:
                    continue
                print(f"  {name[:38]:38s} {ms(v['self_mean']):9.3f}m {ms(v['self_p95']):7.2f}m {ms(v['self_max']):7.2f}m"
                      f" {v['calls']:10.1f} {ms(v['total_mean']):9.3f}m")
            worst = sorted(s.frames[s.tick_set], key=lambda f: -work.get(f[0], 0))[:6]
            print("  heaviest ticks:")
            for fi, st_, en in worst:
                top = ", ".join(f"{nm} {ms(sf):.1f}" for sf, nm, _ in s.frame_top(s.tick_set, s.server, fi, 5, SERVER_IDLE))
                print(f"     t={(st_ - s.window[0]) / 1e9:7.2f}s  work {ms(work.get(fi, 0)):6.1f} ms   {top}")

    # ── Pools
    pools = s.pools()
    header(f"THREADS AND WORKER POOLS (busy = zoned self time minus {'/'.join(sorted(WAIT_ZONES))} / (threads × window);"
           " top zones by self time per second)")
    for gname, g in sorted(pools.items(), key=lambda kv: -kv[1]["busy"]):
        nthreads = len(g["threads"])
        busy = g["busy"] / (nthreads * s.span_ns) if s.span_ns else 0
        top = sorted(g["zones"].items(), key=lambda kv: -kv[1][2])[:4 if nthreads > 1 or gname not in MAIN_NAMES + SERVER_NAMES else 0]
        print(f"  {gname[:18]:18s} ×{nthreads:<2d} busy {100 * busy:5.1f}%")
        for z, v in top:
            print(f"      {z[:40]:40s} {ms(v[2]) / (s.span_ns / 1e9):7.1f} ms/s   n={v[0]:<9d} mean {dur(v[1] / max(1, v[0])):>7s}"
                  f"  max {dur(v[3]):>7s}")

    # ── Plots
    if s.plots:
        header("PLOTS (samples inside the window)")
        print(f"  {'plot':34s} {'n':>7s} {'min':>10s} {'mean':>10s} {'p50':>10s} {'p95':>10s} {'max':>10s}")
        for name in sorted(s.plots):
            p = s.plot_stats(name)
            print(f"  {name[:34]:34s} {p['n']:7d} {fmt_num(p['min']):>10s} {fmt_num(p['mean']):>10s} {fmt_num(p['p50']):>10s}"
                  f" {fmt_num(p['p95']):>10s} {fmt_num(p['max']):>10s}")

    # ── Extras the game may or may not emit
    c = info["counts"]
    if c.get("gpu_zones"):
        header(f"GPU ZONES ({c['gpu_zones']:,}; whole capture)")
        for ctx in info.get("gpu_contexts", []):
            print(f"  context {ctx['index']}: {ctx['name']} ({ctx['type']}), {ctx['zones']} zones")
        rows = [r for r in ex.rows("gpu_stats", typed=True)]
        for r in sorted(rows, key=lambda r: -r["total_ns"])[:20]:
            print(f"  {r['name'][:38]:38s} ctx {r['context']}  n={r['count']:7d} mean {r['mean_ns'] / 1e3:8.1f}us"
                  f"  p99 {r['p99_ns'] / 1e3:8.1f}us  total {r['total_ns'] / 1e9:6.2f}s")
    if c.get("locks"):
        header(f"LOCKS ({c['locks']}; whole capture)")
        for r in sorted(ex.rows("locks", typed=True), key=lambda r: -(r["wait_total_ns"] or 0))[:15]:
            print(f"  {str(r['name'])[:34]:34s} acq {r['acquisitions']:8d}  wait {ms(r['wait_total_ns']):8.2f}ms"
                  f" (max {ms(r['wait_max_ns']):6.2f})  hold {ms(r['hold_total_ns']):8.2f}ms (max {ms(r['hold_max_ns']):6.2f})")
    if c.get("messages"):
        header(f"MESSAGES ({c['messages']:,}; whole capture)")
        sev = collections.Counter(r["severity"] for r in ex.rows("messages"))
        print("  " + ", ".join(f"{k} {v}" for k, v in sev.most_common()))
        msgs = [r for r in ex.rows("messages") if r["severity"] in ("warning", "error", "fatal")] or list(ex.rows("messages"))
        for r in msgs[-10:]:
            print(f"  t={int(r['time_ns']) / 1e9:7.2f}s [{r['severity']}] {r['text'][:90]}")
    if c.get("allocations"):
        header(f"MEMORY ({c['allocations']:,} allocations; whole capture)")
        for p in info.get("memory_pools", []):
            print(f"  {p['name']:20s} allocations {p['allocations']:,}  still live {p['active']:,}  usage {p['usage_bytes'] / 2**20:.1f} MiB")
    if c.get("samples"):
        header(f"SAMPLES ({c['samples']:,}; whole capture) — top leaf functions")
        leaves = collections.Counter(r["leaf"] for r in ex.rows("samples"))
        for leaf, n in leaves.most_common(20):
            print(f"  {100 * n / c['samples']:5.1f}%  {leaf[:95]}")
    if info.get("crash"):
        header("CRASH")
        cr = info["crash"]
        print(f"  {cr['thread_name']} at {cr['time_ns'] / 1e9:.2f}s: {cr['message']}")
        for f in cr["callstack"][-12:]:
            print(f"     {f}")
    print("=" * W)


# ── A/B comparison ────────────────────────────────────────────────────────────

def compare(a, b):
    header(f"A/B  A = {a.name}   B = {b.name}")
    print(f"  A window: {a.window_desc}")
    print(f"  B window: {b.window_desc}")

    def row(label, va, vb, unit="", better="lower", fmt="{:.2f}"):
        if va is None or vb is None:
            return
        d = vb - va
        rel = (100 * d / va) if va else 0
        good = (d < 0) if better == "lower" else (d > 0)
        mark = "" if abs(rel) < 3 else ("  B better" if good else "  A better")
        print(f"  {label:38s} {fmt.format(va):>10s}{unit:3s} {fmt.format(vb):>10s}{unit:3s} {d:+10.2f} ({rel:+6.1f}%){mark}")

    fa, fb = a.frame_stats(a.main_set or ""), b.frame_stats(b.main_set or "")
    if fa and fb:
        header("FRAMES")
        print(f"  {'':38s} {'A':>13s} {'B':>13s} {'B - A':>10s}")
        row("fps", fa["fps"], fb["fps"], better="higher")
        row("1%-low fps", fa["low1_fps"], fb["low1_fps"], better="higher")
        for k in ("mean", "p50", "p90", "p95", "p99", "max"):
            row(f"frame time {k}", ms(fa[k]), ms(fb[k]), "ms")
        row("frames >16.7ms (%)", 100 * fa["over16"] / fa["n"], 100 * fb["over16"] / fb["n"], "%")
        row("frames >33.3ms", fa["over33"], fb["over33"], fmt="{:.0f}")

    if a.main is not None and b.main is not None and a.main_set and b.main_set:
        ba, bb = a.budget(a.main_set, a.main), b.budget(b.main_set, b.main)
        header("MAIN THREAD self time per frame (ms) — biggest differences first")
        names = set(ba) | set(bb)
        rows = []
        for n in names:
            va = ba.get(n, {}).get("self_mean", 0.0)
            vb = bb.get(n, {}).get("self_mean", 0.0)
            if max(va, vb) >= 0.01e6:
                rows.append((abs(vb - va), n, va, vb))
        print(f"  {'zone':38s} {'A':>9s} {'B':>9s} {'B - A':>9s}   {'A p95':>7s} {'B p95':>7s}")
        for _, n, va, vb in sorted(rows, reverse=True)[:32]:
            pa = ba.get(n, {}).get("self_p95", 0.0)
            pb = bb.get(n, {}).get("self_p95", 0.0)
            print(f"  {n[:38]:38s} {ms(va):9.3f} {ms(vb):9.3f} {ms(vb - va):+9.3f}   {ms(pa):7.2f} {ms(pb):7.2f}")
        ta = sum(v["self_mean"] for v in ba.values())
        tb = sum(v["self_mean"] for v in bb.values())
        print(f"  {'(all zoned time)':38s} {ms(ta):9.3f} {ms(tb):9.3f} {ms(tb - ta):+9.3f}")

    if a.server is not None and b.server is not None and a.tick_set and b.tick_set:
        ta, tb = a.frame_stats(a.tick_set), b.frame_stats(b.tick_set)
        if ta and tb:
            header("SERVER")
            wa = sorted(a.frame_self(a.tick_set, a.server, exclude=SERVER_IDLE).values())
            wb = sorted(b.frame_self(b.tick_set, b.server, exclude=SERVER_IDLE).values())
            row("TPS", ta["fps"], tb["fps"], better="higher")
            row("work per tick mean", ms(mean(wa)), ms(mean(wb)), "ms")
            row("work per tick p99", ms(pct(wa, .99)), ms(pct(wb, .99)), "ms")

    pa, pb = a.pools(), b.pools()
    header("THREAD BUSY %")
    for g in sorted(set(pa) | set(pb)):
        ga, gb = pa.get(g), pb.get(g)
        va = 100 * ga["busy"] / (len(ga["threads"]) * a.span_ns) if ga and a.span_ns else 0
        vb = 100 * gb["busy"] / (len(gb["threads"]) * b.span_ns) if gb and b.span_ns else 0
        if max(va, vb) >= 0.5:
            row(g, va, vb, "%")

    common = sorted(set(a.plots) & set(b.plots))
    if common:
        header("PLOTS (mean inside the window)")
        for n in common:
            if n == "Replay/Time":
                continue
            sa, sb = a.plot_stats(n), b.plot_stats(n)
            print(f"  {n[:38]:38s} {fmt_num(sa['mean']):>10s}    {fmt_num(sb['mean']):>10s}    "
                  f"p95 {fmt_num(sa['p95']):>9s} / {fmt_num(sb['p95']):>9s}")
    print("=" * W)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+", help="one capture, or two to compare (A then B)")
    ap.add_argument("--window", default="replay",
                    help="replay (default: the replayed path when there is one), all, or START:END seconds")
    ap.add_argument("--full", action="store_true", help="with two captures, also print each full report")
    args = ap.parse_args()
    if len(args.captures) > 2:
        ap.error("at most two captures")

    sums = [Summary(tracy_tools.export(c), args.window) for c in args.captures]
    if len(sums) == 1:
        report(sums[0])
        return
    if args.full:
        for s in sums:
            report(s)
    compare(*sums)


if __name__ == "__main__":
    main()
