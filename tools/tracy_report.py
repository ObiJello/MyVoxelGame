#!/usr/bin/env python3
"""Whole-capture report for a Tracy trace: every thread, not just the render loop.

    tools/tracy_report.py capture.tracy            # runs csvexport itself
    tools/tracy_report.py --csv all.csv            # from an existing `-u` export

Needs tracy-csvexport (built from cmake-build-tracy/_deps/tracy-src/csvexport;
set TRACY_CSVEXPORT to its path, default looks in the session scratchpad and PATH).

Sections: thread map (auto-classified by dominant zone), main-thread frame
budget and phase breakdown with exact self times, stalls, server tick budget
and breakdown with exact self times, chunk pipeline (generation, disk load,
prebuild/serialize, send) with worker utilisation, mesh workers, occlusion
BFS, GPU-side waits. Two passes over the export: aggregate, then a stack
sweep over the main and server threads for self times.
"""
import csv, os, sys, subprocess, statistics as st, collections, bisect, shutil, tempfile

def find_csvexport():
    p = os.environ.get("TRACY_CSVEXPORT")
    if p and os.path.exists(p): return p
    for c in ("tracy-csvexport",):
        w = shutil.which(c)
        if w: return w
    import glob
    hits = glob.glob("/private/tmp/claude-501/*/*/scratchpad/csvexport-build/tracy-csvexport")
    return hits[0] if hits else None

def pct(xs, p):
    if not xs: return 0.0
    xs = sorted(xs); return xs[min(len(xs) - 1, int(p * len(xs)))]

def fmt_ms(ns): return f"{ns/1e6:8.2f}"

def stats_line(name, xs_ns, unit="ms"):
    if not xs_ns: return f"  {name:34s} (none)"
    div = 1e6 if unit == "ms" else 1e3
    xs = [x / div for x in xs_ns]
    return (f"  {name:34s} n={len(xs):7d}  mean {st.mean(xs):8.2f}  p50 {pct(xs,.5):8.2f}  "
            f"p95 {pct(xs,.95):8.2f}  p99 {pct(xs,.99):8.2f}  max {max(xs):8.2f} {unit}")

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(1)
    if args[0] == "--csv":
        csv_path = args[1]
    else:
        exe = find_csvexport()
        if not exe: print("tracy-csvexport not found; set TRACY_CSVEXPORT"); sys.exit(1)
        csv_path = os.path.join(tempfile.gettempdir(), "tracy_report_export.csv")
        with open(csv_path, "w") as out:
            subprocess.run([exe, "-u", args[0]], stdout=out, stderr=subprocess.DEVNULL, check=True)

    # ── pass 1: per-thread, per-zone aggregates ──────────────────────────
    agg = collections.defaultdict(lambda: [0, 0, 0])      # (thread, name) -> [count, total, max]
    span = collections.defaultdict(lambda: [1 << 62, 0])
    with open(csv_path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            name, s, d, t = row[0], int(row[3]), int(row[4]), row[5]
            a = agg[(t, name)]; a[0] += 1; a[1] += d; a[2] = max(a[2], d)
            sp = span[t]; sp[0] = min(sp[0], s); sp[1] = max(sp[1], s + d)
    by_thread = collections.defaultdict(dict)
    for (t, n), v in agg.items(): by_thread[t][n] = v
    def dominant(t): return max(by_thread[t].items(), key=lambda kv: kv[1][1])[0]
    def classify(t):
        names = by_thread[t]
        if "Render" in names and "Present" in names: return "main"
        if "ServerTick" in names: return "server"
        d = dominant(t)
        return {"ProcessMeshJob": "mesh-worker", "BuildSectionMeshFromCache": "mesh-worker",
                "LoadWithoutGenerating": "chunk-load", "LoadFromDisk": "chunk-load",
                "TerrainTask": "terrain-gen", "PrebuildChunk": "chunk-prebuild",
                "OcclusionBFS": "occlusion-bfs"}.get(d, "other:" + d)
    roles = {t: classify(t) for t in by_thread}
    main_t = next((t for t, r in roles.items() if r == "main"), None)
    server_t = next((t for t, r in roles.items() if r == "server"), None)

    print("=" * 100); print("THREAD MAP  (busy = sum of top-level zone time / thread span)")
    for t in sorted(by_thread, key=lambda t: (roles[t], t)):
        sp = span[t]; total_span = max(1, sp[1] - sp[0])
        top = sorted(by_thread[t].items(), key=lambda kv: -kv[1][1])[:3]
        busy = top[0][1][1] / total_span if top else 0
        print(f"  thread {t:>3s}  {roles[t]:16s} span {total_span/1e9:6.1f}s  top-zone busy {100*busy:5.1f}%  "
              + ", ".join(f"{n} ({v[0]})" for n, v in top))

    # ── pass 2: full event lists for main + server, exact self times ─────
    keep = {main_t, server_t}
    events = {t: [] for t in keep if t}
    with open(csv_path) as f:
        r = csv.reader(f); next(r)
        for row in r:
            t = row[5]
            if t in events: events[t].append((int(row[3]), int(row[4]), row[0]))
    def self_times(evs):
        evs.sort(key=lambda e: (e[0], -e[1]))
        selfs = collections.defaultdict(lambda: [0, 0]); stack = []
        for s, d, n in evs:
            while stack and stack[-1][0] + stack[-1][1] <= s: stack.pop()
            if stack: stack[-1][3] += d           # child time charged to parent
            stack.append([s, d, n, 0])
            # self is finalised when popped; compute lazily below
        # second sweep: self = d - children
        evs2 = sorted(evs, key=lambda e: (e[0], -e[1])); stack = []; out = collections.defaultdict(lambda: [0, 0])
        child = {}
        for i, (s, d, n) in enumerate(evs2):
            while stack and evs2[stack[-1]][0] + evs2[stack[-1]][1] <= s:
                j = stack.pop(); sd = evs2[j][1] - child.get(j, 0); o = out[evs2[j][2]]; o[0] += 1; o[1] += sd
            if stack: child[stack[-1]] = child.get(stack[-1], 0) + d
            stack.append(i)
        while stack:
            j = stack.pop(); sd = evs2[j][1] - child.get(j, 0); o = out[evs2[j][2]]; o[0] += 1; o[1] += sd
        return out
    def within_frames(frames, evs_by_name, names):
        starts = [s for s, _ in frames]; sums = {n: [0] * len(frames) for n in names}
        for n in names:
            for s, d in evs_by_name.get(n, []):
                i = bisect.bisect_right(starts, s) - 1
                if i >= 0 and s < frames[i][0] + frames[i][1]: sums[n][i] += d
        return sums

    # ── MAIN THREAD ──────────────────────────────────────────────────────
    if main_t:
        evs = events[main_t]; byname = collections.defaultdict(list)
        for s, d, n in evs: byname[n].append((s, d))
        frames = sorted(byname.get("Render", []))
        print("=" * 100); print(f"MAIN THREAD (thread {main_t}) — {len(frames)} rendered frames")
        if frames:
            gaps = [(frames[i + 1][0] - frames[i][0]) for i in range(len(frames) - 1)]
            print(stats_line("frame interval", gaps))
            fps = [1e9 / g for g in gaps]
            print(f"  fps: mean {1e9/st.mean(gaps):.0f}   5%-worst-frame {1e9/pct(gaps,.95):.0f}   1%-worst-frame {1e9/pct(gaps,.99):.0f}   "
                  f"frames >16.7ms {sum(1 for g in gaps if g>16.7e6)} ({100*sum(1 for g in gaps if g>16.7e6)/len(gaps):.2f}%)  >33ms {sum(1 for g in gaps if g>33e6)}")
            phases = ["InputUI", "PollEvents", "Network", "ClientTick", "GameLogic", "MeshSchedule", "MeshUpload", "TexAnimation",
                      "Render", "Vk.BeginFrame", "ChunkPass.Main", "ImmersivePortalRender", "EntityCut", "BlockEntities",
                      "RemotePlayers", "HudRender", "DebugUI", "Present", "Vk.WarmPipelines", "Vk.CreateGraphicsPipeline", "Vk.SingleTimeSubmit+Drain"]
            print("  per-frame phases (whole-frame zones; Render includes Vk.BeginFrame, the GPU fence):")
            for n in phases:
                xs = [d for _, d in byname.get(n, [])]
                if xs: print(stats_line(n, xs))
            rend = [d for _, d in frames]
            beg = within_frames(frames, byname, ["Vk.BeginFrame"])["Vk.BeginFrame"]
            print(stats_line("Render CPU (minus GPU fence)", [rend[i] - beg[i] for i in range(len(frames))]))
        selfs = self_times(evs)
        total_self = sum(v[1] for v in selfs.values())
        print("  top main-thread zones by EXACT self time (share of all main-thread zone time):")
        for n, (c, sd) in sorted(selfs.items(), key=lambda kv: -kv[1][1])[:18]:
            print(f"     {n:36s} self {sd/1e9:7.2f}s  {100*sd/total_self:5.1f}%  n={c:8d}  self/call {sd/c/1e3:8.1f}us")
        if frames:
            gaps_i = sorted(range(len(gaps)), key=lambda i: -gaps[i])[:6]
            print("  worst frame intervals and what the frame was doing:")
            for i in gaps_i:
                s0, d0 = frames[i]; inside = collections.Counter()
                lo = bisect.bisect_left(evs, (s0, -1, "")); 
                for s, d, n in evs[lo: lo + 4000]:
                    if s >= s0 + gaps[i]: break
                    if n not in ("Render",): inside[n] = max(inside[n], d)
                top = ", ".join(f"{n} {d/1e6:.1f}ms" for n, d in inside.most_common(4))
                print(f"     t={ (s0-frames[0][0])/1e9:6.1f}s interval {gaps[i]/1e6:6.1f}ms  Render {d0/1e6:5.1f}ms  biggest inside: {top}")

    # ── SERVER THREAD ────────────────────────────────────────────────────
    if server_t:
        evs = events[server_t]; byname = collections.defaultdict(list)
        for s, d, n in evs: byname[n].append((s, d))
        ticks = sorted(byname.get("ServerTick", []))
        print("=" * 100); print(f"SERVER THREAD (thread {server_t}) — {len(ticks)} ticks, budget 50 ms")
        if ticks:
            tg = [(ticks[i + 1][0] - ticks[i][0]) for i in range(len(ticks) - 1)]
            print(stats_line("ServerTick", [d for _, d in ticks]))
            print(stats_line("tick interval", tg))
            print(f"  ticks over 50 ms budget: {sum(1 for _,d in ticks if d>50e6)}   over 25 ms: {sum(1 for _,d in ticks if d>25e6)}   "
                  f"effective TPS {1e9/st.mean(tg):.1f}")
            children = ["WorldLoop", "RandomTick", "MobSystemTick", "MobTick", "NaturalSpawner", "PumpChunkPipeline", "SendNextChunks",
                        "ProcessAsyncChunkResults", "SerializeChunk", "ChunkResult.EntityLoad", "TickPortals", "EntityTravel", "BlockTicks",
                        "FluidTicks", "ItemEntities", "SaveChunks", "UnloadUnwatchedChunks"]
            sums = within_frames(ticks, byname, children)
            print("  per-tick work (sum inside ServerTick):")
            for n in children:
                xs = sums[n]
                if any(xs): print(stats_line(n, xs))
        selfs = self_times(evs); total_self = sum(v[1] for v in selfs.values())
        print("  top server-thread zones by EXACT self time:")
        for n, (c, sd) in sorted(selfs.items(), key=lambda kv: -kv[1][1])[:16]:
            print(f"     {n:36s} self {sd/1e9:7.2f}s  {100*sd/total_self:5.1f}%  n={c:8d}  self/call {sd/c/1e3:8.1f}us")
        if ticks:
            worst = sorted(range(len(ticks)), key=lambda i: -ticks[i][1])[:5]
            print("  worst ticks and their biggest zones:")
            for i in worst:
                s0, d0 = ticks[i]; inside = collections.Counter()
                lo = bisect.bisect_left(evs, (s0, -1, ""))
                for s, d, n in evs[lo: lo + 20000]:
                    if s >= s0 + d0: break
                    if n != "ServerTick": inside[n] = max(inside[n], d)
                print(f"     t={(s0-ticks[0][0])/1e9:6.1f}s  ServerTick {d0/1e6:6.1f}ms  " + ", ".join(f"{n} {d/1e6:.1f}ms" for n, d in inside.most_common(4)))

    # ── CHUNK PIPELINE + WORKERS ─────────────────────────────────────────
    print("=" * 100); print("CHUNK PIPELINE AND WORKER POOLS")
    def pool(role, zone):
        ts = [t for t, r in roles.items() if r == role]
        if not ts: print(f"  {role}: no threads"); return
        n = sum(by_thread[t].get(zone, [0, 0, 0])[0] for t in ts); tot = sum(by_thread[t].get(zone, [0, 0, 0])[1] for t in ts)
        mx = max(by_thread[t].get(zone, [0, 0, 0])[2] for t in ts)
        util = [by_thread[t].get(zone, [0, 0, 0])[1] / max(1, span[t][1] - span[t][0]) for t in ts]
        wall = max(span[t][1] for t in ts) - min(span[t][0] for t in ts)
        print(f"  {role:15s} {len(ts)} threads  {zone}: n={n} mean {tot/max(1,n)/1e6:.2f}ms max {mx/1e6:.1f}ms  "
              f"throughput {n/(wall/1e9):.1f}/s over {wall/1e9:.1f}s  per-thread busy {min(util)*100:.0f}-{max(util)*100:.0f}%")
    pool("terrain-gen", "TerrainTask"); pool("chunk-load", "LoadFromDisk"); pool("chunk-prebuild", "PrebuildChunk")
    pool("mesh-worker", "BuildSectionMeshFromCache"); pool("occlusion-bfs", "OcclusionBFS")
    others = [t for t, r in roles.items() if r.startswith("other:")]
    for t in others: print(f"  other thread {t}: {roles[t][6:]} n={by_thread[t][roles[t][6:]][0]}")
    print("=" * 100)

if __name__ == "__main__":
    main()
