#!/usr/bin/env python3
"""GPU-side report for an Instruments Metal System Trace recorded by
tools/play.sh --gpu-trace (or any xctrace trace of the game that carries the
Metal GPU tables).

    tools/gpu_report.py gpu-03-47-19.trace                       # GPU timeline only
    tools/gpu_report.py gpu-03-47-19.trace --tracy Untitled.tracy  # + aligned Tracy capture
    tools/gpu_report.py gpu-03-47-19.trace --hud-log hud.log      # + Metal HUD per-second CSV
    tools/gpu_report.py gpu-03-47-19.trace --no-cpu               # skip the CPU sample profile

What it prints, in order:
  DEVICE           GPU name, thermal state, performance state (throttling check)
  GPU FRAMES       per-frame Vertex/Fragment execution time (the real GPU cost),
                   encoders per frame, GPU-side fps, worst frames
  GPU ENCODERS     per encoder LABEL ("Frame", "Target 12 (1710x1073)", "OIT pass
                   3", "Texture flush", ...): vertex / fragment ms per frame,
                   how often it runs, and how much of it overlaps other encoders —
                   the native Metal backend labels every encoder, so this is the
                   per-stage breakdown for passes that own an encoder
  GPU COUNTERS     (template with a counter set) mean / p95 of every limiter and
                   utilization counter over the recording
  GPU UTILISATION  Active vs Idle from the driver's state track
  CPU-SIDE WAITS   "Wait for Next Drawable" and the other Metal application
                   waits (from metal-application-intervals — the CAMetalLayer
                   client-wait table does NOT show the drawable waits),
                   Instruments' hang detector
  PER SECOND       vertex/fragment ms per frame over the session (+ Tracy columns)
  TRACY ALIGNMENT  (--tracy) cross-correlates frame times to find the clock offset,
                   then regresses GPU vertex time against Sections/Visible and, when
                   the capture has them, Geom/Vertices and Geom/Indices — which one
                   predicts the GPU tells you what to shrink
  HUD LOG          (--hud-log) summary of Metal Performance HUD `metal-HUD:` lines
  CPU SAMPLES      Instruments' 1 kHz time profile of the game with REAL symbols
                   (dladdr-free, unlike Tracy's ghost zones): per-thread shares,
                   main-thread leaf functions, and where the samples land inside
                   the functions named by --cpu-fn

Tables are exported once with `xcrun xctrace export` into <trace>.export/ and
reused on later runs (delete the folder to re-export). Facts worth knowing:
  * Only the Vertex and Fragment channels are GPU execution. Through MoltenVK
    the 'Compute' channel carries one interval per command buffer (labelled
    'GL/CL', tens of ms, stacked dozens deep): commit-to-completion lifetime,
    not work. Native Metal has no such intervals; its blit encoders
    ("Uploads", "Texture flush", "Readback", "Copy framebuffer") show on
    the Blit channel.
  * On a tile GPU vertex (tiling) and fragment work of different encoders —
    and of consecutive frames — overlap; the per-frame union is what the
    frame costs, the per-encoder sums are what each pass costs in isolation.
    Live overlapped per-encoder numbers also come from the Metal HUD
    (MTL_HUD_ENCODER_TIMING_ENABLED=1, --hud-log).
  * Trace time 0 is when xctrace attached (~20 s after launch with play.sh),
    not game start; --tracy finds the offset itself.
  * The stock template records one useless counter ('RT Unit Active') on
    Apple silicon; counters need the GUI-saved gpu-limiters.tracetemplate.
    The 3 GB gpu-counter-value table is skipped; the derived
    metal-gpu-counter-intervals table (GPUCounters package) is read instead.
  * The time profile records RUNNING samples only: main-thread running time
    below 100% is time blocked in the fence / drawable / vsync waits.
"""
import csv, os, re, sys, subprocess, statistics as st, bisect, collections, shutil, glob, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tracy_tools
import xml.etree.ElementTree as ET

# ── xctrace export ──────────────────────────────────────────────────────────
TABLES = ["metal-gpu-intervals", "metal-object-label", "metal-gpu-state-intervals",
          "gpu-performance-state-intervals", "device-thermal-state-intervals",
          "metal-application-intervals", "potential-hangs", "device-gpu-info"]
# Present only when the template records counters; an empty export is fine.
OPTIONAL_TABLES = ["metal-gpu-counter-intervals"]
CPU_TABLE = "time-profile"

def export_tables(trace, outdir, tables, optional=()):
    os.makedirs(outdir, exist_ok=True)
    for t in list(tables) + list(optional):
        path = os.path.join(outdir, t + ".xml")
        if os.path.exists(path) and (os.path.getsize(path) > 0 or t in optional): continue
        sys.stderr.write(f"exporting {t} ...\n")
        with open(path, "w") as out:
            r = subprocess.run(["xcrun", "xctrace", "export", "--input", trace, "--xpath",
                                f'/trace-toc/run[@number="1"]/data/table[@schema="{t}"]'],
                               stdout=out, stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            if t in optional:
                open(path, "w").close()   # mark as "not in this trace"
                continue
            os.remove(path)
            sys.exit(f"xctrace export failed for {t}: {r.stderr.strip()}\n"
                     "(a trace that is still being finalised by xctrace gives 'Document Missing Template Error' — wait for it;"
                     " a trace whose recording crashed, or whose game quit before the time limit, stays unreadable)")

def _text(el):
    if el.attrib.get("fmt") is not None and len(el) > 0: return el.attrib["fmt"]
    return el.text if el.text is not None else el.attrib.get("fmt", "")

def read_table(path, want=None):
    """Rows as dicts keyed by column mnemonic. xctrace dedups values with id=/ref=."""
    if not os.path.exists(path) or os.path.getsize(path) == 0: return
    ids, cols = {}, []
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag == "schema":
            cols = [c.find("mnemonic").text for c in el.findall("col")]
        elif el.tag == "row":
            row = {}
            for i, child in enumerate(el):
                if child.tag == "sentinel": val = None
                elif "ref" in child.attrib: val = ids.get(child.attrib["ref"])
                else:
                    for sub in child.iter():
                        if "id" in sub.attrib: ids[sub.attrib["id"]] = _text(sub)
                    val = _text(child)
                if i < len(cols) and (want is None or cols[i] in want): row[cols[i]] = val
            yield row
            el.clear()

# ── helpers ─────────────────────────────────────────────────────────────────
def pct(xs, p):
    if not xs: return 0.0
    xs = sorted(xs); return xs[min(len(xs) - 1, int(p * len(xs)))]

def dist(name, xs, unit="ms"):
    if not xs: return f"  {name:36s} (none)"
    return (f"  {name:36s} n={len(xs):6d}  mean {st.mean(xs):7.2f}  p50 {pct(xs,.5):7.2f}  "
            f"p95 {pct(xs,.95):7.2f}  p99 {pct(xs,.99):7.2f}  max {max(xs):7.2f} {unit}")

def union_ms(iv):
    if not iv: return 0.0
    iv = sorted(iv); tot = 0.0; cs, ce = iv[0]
    for s, e in iv[1:]:
        if s > ce: tot += ce - cs; cs, ce = s, e
        else: ce = max(ce, e)
    return tot + ce - cs

def bucket(series, w):
    d = collections.defaultdict(list)
    for t, v in series: d[int(t // w)].append(v)
    return d

def corr(xs, ys):
    mx, my = st.mean(xs), st.mean(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs); syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0: return 0.0, 0.0, my
    return sxy / (sxx * syy) ** 0.5, sxy / sxx, my - (sxy / sxx) * mx

BLIT_WORDS = ("Copy", "Blit", "Upload", "Texture flush", "Readback", "Clear RT")

def encoder_key(label):
    """One row per KIND of encoder: digits (target handles, OIT pass numbers,
    sizes) become #, so 'Target 573 (1710x1073)' and 'Target 574 (1710x1073)'
    fold together while 'Frame' and 'Frame (resumed)' stay apart."""
    return re.sub(r"\d+", "#", label or "?")

# ── main ────────────────────────────────────────────────────────────────────
def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"): print(__doc__); sys.exit(1)
    trace = args[0].rstrip("/"); tracy = None; do_cpu = True; export_dir = None; hud_log = None
    process = "MyVoxelGame"
    cpu_fns = ["ScheduleMeshBuildsWithSnapshots", "PrepareVisibleSections", "RenderLayerPass",
               "ImmersivePortalRenderer", "vkQueueSubmit", "MetalBackend::EndFrame",
               "SubmitMergedRuns", "glfwPollEvents", "ClientChunkManager::Update"]
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--tracy": tracy = args[i + 1]; i += 2
        elif a == "--hud-log": hud_log = args[i + 1]; i += 2
        elif a == "--no-cpu": do_cpu = False; i += 1
        elif a == "--export-dir": export_dir = args[i + 1]; i += 2
        elif a == "--process": process = args[i + 1]; i += 2
        elif a == "--cpu-fn": cpu_fns = args[i + 1].split(","); i += 2
        else: sys.exit(f"unknown option {a}")
    export_dir = export_dir or trace + ".export"
    export_tables(trace, export_dir, TABLES + ([CPU_TABLE] if do_cpu else []), OPTIONAL_TABLES)
    T = lambda name: os.path.join(export_dir, name + ".xml")
    W = 100
    print("=" * W)

    # ── DEVICE ──────────────────────────────────────────────────────────
    print("DEVICE")
    for r in read_table(T("device-gpu-info")):
        print(f"  {r['device-name']} ({r['vendor-name']}), recommended working set {r['recommended-max-working-set-size']}")
    therm = collections.Counter(); span_end = 0
    for r in read_table(T("device-thermal-state-intervals")):
        therm[r["thermal-state"]] += int(r["duration"]) / 1e9
        span_end = max(span_end, (int(r["start"]) + int(r["duration"])) / 1e9)
    print("  thermal state:  " + ", ".join(f"{k} {v:.1f}s" for k, v in therm.most_common()) +
          "   (anything but Nominal = the fanless Air was throttling; do not compare against a cool run)")
    perf = collections.Counter()
    for r in read_table(T("gpu-performance-state-intervals")):
        perf[r["gpu-performance-state"]] += int(r["duration"]) / 1e9
    print("  GPU performance state:  " + ", ".join(f"{k} {v:.1f}s" for k, v in perf.most_common()))
    print(f"  trace length {span_end:.1f}s (time 0 = xctrace attach, ~20 s after launch with play.sh)")

    # ── GPU FRAMES + ENCODERS ───────────────────────────────────────────
    labels = {}
    for r in read_table(T("metal-object-label"), want={"object-id", "label"}):
        labels[r["object-id"]] = r["label"] or ""
    frames = collections.defaultdict(lambda: {"V": [], "F": [], "encV": 0, "encF": 0})
    # per encoder (object id): channel -> [(s, e)], label, frame
    encs = collections.defaultdict(lambda: {"V": [], "F": [], "B": [], "label": "?", "frame": None})
    lifetimes = collections.defaultdict(list); blits = []
    for r in read_table(T("metal-gpu-intervals"),
                        want={"process", "channel-name", "frame-number", "start", "duration", "cmdbuffer-id", "encoder-id", "event-label"}):
        if process not in (r["process"] or ""): continue
        s = int(r["start"]) / 1e6; d = int(r["duration"]) / 1e6; ch = r["channel-name"]
        enc_id = r.get("encoder-id"); cb_lab = labels.get(r["cmdbuffer-id"], "?")
        enc_lab = labels.get(enc_id) or r.get("event-label") or cb_lab
        if ch in ("Vertex", "Fragment"):
            if r["frame-number"] is None: continue   # a stray interval with no frame (seen once per channel)
            f = frames[int(r["frame-number"])]
            if ch == "Vertex": f["V"].append((s, s + d)); f["encV"] += 1
            else: f["F"].append((s, s + d)); f["encF"] += 1
            if enc_id:
                e = encs[enc_id]; e["label"] = enc_lab; e["frame"] = int(r["frame-number"])
                e["V" if ch == "Vertex" else "F"].append((s, s + d))
        elif ch == "Blit" or any(w in (enc_lab or "") for w in BLIT_WORDS) or any(w in cb_lab for w in BLIT_WORDS):
            blits.append(d)
            if enc_id:
                e = encs[enc_id]; e["label"] = enc_lab; e["B"].append((s, s + d))
                if r["frame-number"] is not None: e["frame"] = int(r["frame-number"])
        else: lifetimes[encoder_key(cb_lab)].append(d)
    rows = []   # (start, vert, frag, encV, encF, frame)
    for n, f in sorted(frames.items()):
        allv = f["V"] + f["F"]
        if not allv: continue
        rows.append((min(s for s, e in allv), union_ms(f["V"]), union_ms(f["F"]), f["encV"], f["encF"], n))
    if not rows: sys.exit(f"no Vertex/Fragment intervals for process '{process}' (use --process)")
    vert = [r[1] for r in rows]; frag = [r[2] for r in rows]; both = [r[1] + r[2] for r in rows]
    starts = [r[0] for r in rows]; iv = [b - a for a, b in zip(starts, starts[1:])]
    nframes = len(rows)
    print("=" * W)
    print(f"GPU FRAMES — {nframes} frames with GPU work for {process}")
    print("  a 'frame' here is Metal's frame-number = one PRESENT (drawable); with the vsync-off mailbox several")
    print("  rendered frames land in one present, so divide the per-frame ms by 'encoders per frame' for a rendered")
    print("  frame, and multiply the fps by it for the rendered rate (Tracy's FrameMark counts rendered frames).")
    print("  per frame, execution time on the channel (overlapping encoders merged):")
    print(dist("Vertex (tiler: vertex shading + binning)", vert))
    print(dist("Fragment (pixel shading + resolve)", frag))
    print(dist("Vertex + Fragment", both))
    print(f"  encoders per frame: vertex {st.mean(r[3] for r in rows):.2f}  fragment {st.mean(r[4] for r in rows):.2f}"
          f"   (native Metal: 'Frame' + one per render target / OIT pass; MoltenVK: one render pass = one of each)")
    print(dist("GPU frame interval", iv) + f"   -> {1000 / st.mean(iv):.0f} fps mean, 1%-worst {1000 / pct(iv, .99):.0f} fps")
    bound = "VERTEX-heavier" if st.mean(vert) > st.mean(frag) * 1.3 else ("FRAGMENT-heavier" if st.mean(frag) > st.mean(vert) * 1.3 else "balanced")
    print(f"  verdict: {bound} — vertex {st.mean(vert):.2f} ms vs fragment {st.mean(frag):.2f} ms per frame"
          f" (the channels overlap on a tile GPU; the frame interval, not their sum, is the ceiling:"
          f" ~{1000 / max(st.mean(vert), st.mean(frag)):.0f} fps at this load if the longer channel were the only limit)")
    if blits: print(dist("Blit encoders (uploads, copies)", blits) + "   [not per frame]")
    for lab, v in sorted(lifetimes.items(), key=lambda kv: -len(kv[1])):
        print(f"  submission lifetimes, not execution: {lab[:48]:48s} n={len(v):6d} mean {st.mean(v):6.2f} p99 {pct(v, .99):7.2f} max {max(v):7.2f} ms")
    print("  worst frames by vertex+fragment time:")
    for r in sorted(rows, key=lambda r: -(r[1] + r[2]))[:8]:
        print(f"    t={r[0] / 1000:6.2f}s  frame {r[5]:6d}  vertex {r[1]:6.2f} ms  fragment {r[2]:6.2f} ms")

    if encs:
        per = collections.defaultdict(lambda: {"V": [], "F": [], "B": [], "n": 0, "overlap": []})
        # Overlap: how much of this encoder's span is also covered by OTHER encoders of the same frame.
        by_frame = collections.defaultdict(list)
        for eid, e in encs.items():
            if e["frame"] is not None: by_frame[e["frame"]].append(eid)
        for eid, e in encs.items():
            k = encoder_key(e["label"]); p = per[k]; p["n"] += 1
            p["V"].append(union_ms(e["V"])); p["F"].append(union_ms(e["F"])); p["B"].append(union_ms(e["B"]))
            own = e["V"] + e["F"] + e["B"]
            if own and e["frame"] is not None:
                others = [iv_ for o in by_frame[e["frame"]] if o != eid for iv_ in encs[o]["V"] + encs[o]["F"] + encs[o]["B"]]
                span = union_ms(own)
                if span > 0:
                    s0 = min(s for s, _ in own); e0 = max(e_ for _, e_ in own)
                    clipped = [(max(s, s0), min(e_, e0)) for s, e_ in others if e_ > s0 and s < e0]
                    p["overlap"].append(100 * union_ms(clipped) / (e0 - s0) if e0 > s0 else 0.0)
        print("=" * W)
        print(f"GPU ENCODERS — per encoder label, ms per FRAME (n/frame = how many such encoders a frame has)")
        print(f"  {'encoder':34s} {'n/frame':>8s} {'vertex':>8s} {'fragment':>9s} {'blit':>7s} {'p99 V+F':>8s} {'overlap':>8s}")
        for k, p in sorted(per.items(), key=lambda kv: -(sum(kv[1]['V']) + sum(kv[1]['F']) + sum(kv[1]['B']))):
            vf = [a + b for a, b in zip(p["V"], p["F"])]
            print(f"  {k[:34]:34s} {p['n'] / nframes:8.2f} {sum(p['V']) / nframes:8.3f} {sum(p['F']) / nframes:9.3f}"
                  f" {sum(p['B']) / nframes:7.3f} {pct(vf, .99):8.3f} {st.mean(p['overlap']) if p['overlap'] else 0:7.0f}%")
        print("  overlap = share of the encoder's GPU span during which other encoders of the same frame were also"
              " executing; a pass that overlaps 100% costs the frame less than its own ms")

    # ── GPU COUNTERS ────────────────────────────────────────────────────
    cnt = collections.defaultdict(list)
    for r in read_table(T("metal-gpu-counter-intervals"), want={"name", "value", "percent-value", "is-percentage"}) or []:
        name = r.get("name") or "?"
        v = r.get("percent-value") if (r.get("is-percentage") in ("1", "true", "True")) else r.get("value")
        try: cnt[name].append(float(str(v).replace(",", "").rstrip("%")))
        except (TypeError, ValueError): pass
    if cnt:
        print("=" * W)
        print("GPU COUNTERS (template counter set over the whole recording; limiters are % of the unit's peak)")
        print(f"  {'counter':44s} {'n':>7s} {'mean':>8s} {'p95':>8s} {'max':>8s}")
        for name, vs in sorted(cnt.items(), key=lambda kv: -st.mean(kv[1])):
            print(f"  {name[:44]:44s} {len(vs):7d} {st.mean(vs):8.1f} {pct(vs, .95):8.1f} {max(vs):8.1f}")

    # ── GPU UTILISATION ────────────────────────────────────────────────
    state = collections.Counter(); chans = collections.Counter()
    for r in read_table(T("metal-gpu-state-intervals")):
        d = int(r["duration"]) / 1e3
        state[r["state"]] += d; chans[r["label"]] += d
    tot = sum(state.values()) or 1
    print("=" * W)
    print("GPU UTILISATION (driver state track, whole system)")
    print("  " + "  ".join(f"{k} {v / 1e6:.1f}s ({100 * v / tot:.0f}%)" for k, v in state.most_common()))
    print("  " + "  ".join(f"[{k}: {100 * v / tot:.0f}%]" for k, v in chans.most_common()))
    print("  Active above ~90% with vertex+fragment near the frame interval = the GPU is the frame-rate ceiling")

    # ── CPU-SIDE WAITS ─────────────────────────────────────────────────
    waits = collections.defaultdict(list)
    for r in read_table(T("metal-application-intervals"), want={"process", "event-label", "duration"}):
        if process not in (r["process"] or ""): continue
        lab = r["event-label"] or "?"
        if "Wait" in lab or "wait" in lab:
            waits[encoder_key(lab)].append(int(r["duration"]) / 1e6)
    print("=" * W)
    print("CPU-SIDE WAITS (Metal application intervals of the game)")
    if waits:
        for lab, v in sorted(waits.items(), key=lambda kv: -sum(kv[1])):
            print(dist(lab[:36], v) + f"   total {sum(v) / 1000:.2f}s = {100 * sum(v) / (span_end * 1000):.0f}% of the trace")
    else:
        print("  no wait intervals recorded")
    hangs = list(read_table(T("potential-hangs")) or [])
    if hangs:
        print("  Instruments hang detector (main thread unresponsive):")
        for r in hangs:
            print(f"    t={int(r['start']) / 1e9:6.2f}s  {int(r['duration']) / 1e6:7.1f} ms  {r['hang-type']}")
    else: print("  no hangs detected")

    # ── TRACY ALIGNMENT ────────────────────────────────────────────────
    tr = None
    if tracy:
        ex = tracy_tools.export(tracy)
        main_tid = next((t["tid"] for t in ex.threads if t["name"] == "Main thread"), None)
        tframes_raw = ex.frames("Frames")
        if main_tid is None or not tframes_raw: sys.exit("no main thread / FrameMark frames in the Tracy capture")
        # Main-thread time per frame (ms) of a zone, stamped with the frame's start (ms).
        wanted = {"Vk.FenceWait", "Mtl.FrameWait", "Mtl.NextDrawable", "Present", "ImmersivePortalRender", "ChunkPass.Main"}
        per = collections.defaultdict(dict)
        for r in ex.rows("frame_zones"):
            if r["frame_set"] == "Frames" and r["name"] in wanted and int(r["thread"]) == main_tid:
                per[r["name"]][int(r["frame"])] = per[r["name"]].get(int(r["frame"]), 0.0) + int(r["total_ns"]) / 1e6
        def tracy_rows(name):
            return [(s / 1e6, per[name].get(i, 0.0)) for i, (s, _) in enumerate(tframes_raw)]
        plots = {k: [(t / 1e6, v) for t, v in pts] for k, pts in
                 ex.plots(["Sections/Visible", "Geom/Vertices", "Geom/Indices"]).items()}
        tframes = [s / 1e6 for s, _ in tframes_raw]
        B = 50.0
        span = max(tframes[-1], starts[-1]) + 60000
        def bins(ts):
            n = int(span / B) + 1; a = [0] * n
            for t in ts:
                k = int(t / B)
                if 0 <= k < n: a[k] += 1
            return a
        gb = bins(starts); best = (0, -2)
        for off in range(-60000, 60001, int(B)):      # tracy_time = gpu_time + off
            tb = bins([t - off for t in tframes]); n = min(len(gb), len(tb))
            c, _, _ = corr(gb[:n], tb[:n])
            if c > best[1]: best = (off, c)
        off, c = best
        print("=" * W)
        print(f"TRACY ALIGNMENT — {os.path.basename(tracy)}: Tracy time = GPU time + {off / 1000:.2f}s (frame-rate correlation {c:.3f}; below 0.8 = not the same session)")
        fence_name = next((n for n in ("Vk.FenceWait", "Mtl.FrameWait") if per.get(n)), "Present")
        drawable = tracy_rows("Mtl.NextDrawable") if per.get("Mtl.NextDrawable") else None
        tr = {"off": off, "fence_name": fence_name,
              "fence": tracy_rows(fence_name), "drawable": drawable,
              "portal": tracy_rows("ImmersivePortalRender"),
              "chunk": tracy_rows("ChunkPass.Main"), "frames": tframes,
              "vis": plots.get("Sections/Visible", []), "verts": plots.get("Geom/Vertices", []),
              "idx": plots.get("Geom/Indices", [])}
        # per Tracy frame: first Sections/Visible point is the main view (ChunkPass.Main runs before the portal pass)
        def per_frame(points):
            d = collections.defaultdict(list)
            for t, v in points:
                k = bisect.bisect_right(tframes, t) - 1
                if k >= 0: d[k].append(v)
            return d
        pf = per_frame(tr["vis"])
        if pf:
            first = [v[0] for v in pf.values()]; rest = [sum(v[1:]) for v in pf.values()]; total = [sum(v) for v in pf.values()]
            print(f"  sections drawn per frame: main view mean {st.mean(first):.0f} (p50 {pct(first, .5):.0f}, max {max(first):.0f});"
                  f" portal views mean {st.mean(rest):.0f} (p95 {pct(rest, .95):.0f}, max {max(rest):.0f});"
                  f" portal share {100 * sum(rest) / max(1, sum(total)):.0f}%; views/frame {st.mean(len(v) for v in pf.values()):.2f}")
        pfv = per_frame(tr["verts"])
        if pf and pfv:
            vt = [sum(v) for v in pfv.values()]; secs = sum(sum(v) for v in pf.values())
            print(f"  drawn vertices per frame: mean {st.mean(vt) / 1000:.0f} k (p95 {pct(vt, .95) / 1000:.0f} k);"
                  f" per section drawn: {sum(vt) / max(1, secs):.0f}")
        gpuv = bucket([(r[0], r[1]) for r in rows], 250); gpuf = bucket([(r[0], r[2]) for r in rows], 250)
        nfr = bucket([(r[0], 1) for r in rows], 250)
        for label, series, unit in (("Sections/Visible", tr["vis"], "section"), ("Geom/Vertices", tr["verts"], "kvertex"),
                                    ("Geom/Indices", tr["idx"], "kindex")):
            if not series: print(f"  {label}: not in this capture" + (" (added 2026-09-04; recapture)" if label != "Sections/Visible" else "")); continue
            sb = bucket([(t - off, v) for t, v in series], 250)
            xs, ys, fs = [], [], []
            for b in sorted(gpuv):
                # Buckets with a handful of frames (a stall, the trace edges)
                # divide a full bucket of plot points by one or two frames and
                # land 10x off the line; they carry no information anyway.
                if b in sb and len(nfr[b]) >= 10:
                    scale = 1000.0 if unit != "section" else 1.0
                    xs.append(sum(sb[b]) / len(nfr[b]) / scale); ys.append(st.mean(gpuv[b])); fs.append(st.mean(gpuf[b]))
            if len(xs) < 10: continue
            r_, slope, icpt = corr(xs, ys)
            rf, _, _ = corr(xs, fs)
            print(f"  GPU vertex ms vs {label} per frame (250 ms buckets, n={len(xs)}): r={r_:.3f}, "
                  f"{slope * 1000:.2f} us per {unit}, {icpt:.2f} ms fixed;  fragment r={rf:.3f}")
            q = sorted(zip(xs, ys, fs)); k = len(q) // 4
            for j in range(4):
                part = q[j * k:(j + 1) * k] if j < 3 else q[j * k:]
                print(f"      {label} {part[0][0]:8.0f}..{part[-1][0]:8.0f}/frame  vertex {st.mean(p[1] for p in part):5.2f} ms  fragment {st.mean(p[2] for p in part):5.2f} ms")

    # ── PER SECOND ─────────────────────────────────────────────────────
    print("=" * W)
    hdr = f"{'sec':>4} {'fps':>4} {'vert/fr':>8} {'frag/fr':>8}"
    if tr:
        hdr += f" {tr['fence_name'][:7]:>7s}"
        if tr["drawable"]: hdr += f" {'drawbl':>6s}"
        hdr += f" {'portal':>7} {'chunk':>6} {'sect/fr':>8}"
    print("PER SECOND (trace time; ms per frame" + (", Tracy columns are main-thread CPU ms per frame" if tr else "") + ")")
    print(hdr)
    gs = bucket([(r[0], (r[1], r[2])) for r in rows], 1000)
    def sec_mean(series, s):
        v = [x for t, x in series if s * 1000 <= t - tr["off"] < (s + 1) * 1000]
        return st.mean(v) if v else 0.0
    for s in sorted(gs):
        v = gs[s]; line = f"{s:4d} {len(v):4d} {st.mean(x[0] for x in v):8.2f} {st.mean(x[1] for x in v):8.2f}"
        if tr:
            vis = [x for t, x in tr["vis"] if s * 1000 <= t - tr["off"] < (s + 1) * 1000]
            line += f" {sec_mean(tr['fence'], s):7.2f}"
            if tr["drawable"]: line += f" {sec_mean(tr['drawable'], s):6.2f}"
            line += f" {sec_mean(tr['portal'], s):7.2f} {sec_mean(tr['chunk'], s):6.2f} {sum(vis) / max(1, len(v)):8.0f}"
        print(line)

    # ── HUD LOG ────────────────────────────────────────────────────────
    if hud_log:
        print("=" * W)
        print(f"HUD LOG — {hud_log} (Metal Performance HUD, per PRESENTED frame; encoder times need MTL_HUD_ENCODER_TIMING_ENABLED=1)")
        header = None; cols = collections.OrderedDict()
        for line in open(hud_log, errors="replace"):
            m = re.search(r"metal-HUD:\s*(.*)$", line)
            if not m: continue
            fields = [f.strip() for f in m.group(1).split(",")]
            if header is None and any(f and not re.fullmatch(r"-?[\d.]+(e[-+]?\d+)?", f) for f in fields):
                header = fields; continue
            names = header if header and len(header) == len(fields) else [f"col{i}" for i in range(len(fields))]
            for n, v in zip(names, fields):
                try: cols.setdefault(n, []).append(float(v))
                except ValueError: pass
        if not cols: print("  no metal-HUD lines found")
        for n, vs in cols.items():
            print(f"  {n[:40]:40s} n={len(vs):5d}  mean {st.mean(vs):10.3f}  p95 {pct(vs, .95):10.3f}  max {max(vs):10.3f}")

    # ── CPU SAMPLES ────────────────────────────────────────────────────
    if do_cpu:
        ids, btcache = {}, {}
        def frames_of(bt):
            if bt.attrib.get("id") in btcache: return btcache[bt.attrib["id"]]
            out = []
            for fr in bt.findall("frame"):
                if "ref" in fr.attrib: fr = ids.get(fr.attrib["ref"], fr)
                src = fr.find("source")
                out.append((fr.attrib.get("name", "?"), src.attrib.get("line", "") if src is not None else ""))
            out = tuple(out)
            if "id" in bt.attrib: btcache[bt.attrib["id"]] = out
            return out
        per_thread = collections.Counter(); main = []
        for _, el in ET.iterparse(T(CPU_TABLE), events=("end",)):
            if el.tag != "row": continue
            for sub in el.iter():
                if "id" in sub.attrib: ids[sub.attrib["id"]] = sub
            kids = list(el)
            if len(kids) < 7: el.clear(); continue
            th, bt = kids[1], kids[6]
            if "ref" in th.attrib: th = ids.get(th.attrib["ref"])
            if "ref" in bt.attrib: bt = ids.get(bt.attrib["ref"])
            if th is None or bt is None: el.clear(); continue
            thn = th.attrib.get("fmt", "")
            if process in thn:
                short = thn.split(" (")[0]; per_thread[short] += 1
                if short.startswith("Main Thread"):
                    fr = frames_of(bt)
                    if fr: main.append(fr)
            el.clear()
        print("=" * W)
        print(f"CPU SAMPLES (Instruments time profile, 1 ms, running threads only; {sum(per_thread.values())} samples in {process})")
        print("  busiest threads (samples ≈ ms running):")
        for k, v in per_thread.most_common(8): print(f"    {v:7d}  {k}")
        if main:
            print(f"  main thread: running {len(main) / (span_end * 1000) * 100:.0f}% of the trace (the rest is blocked in GPU fence / drawable / vsync waits)")
            leaf = collections.Counter(s[0][0][:92] for s in main)
            print("  main-thread leaf functions (self time):")
            for k, v in leaf.most_common(25): print(f"    {100 * v / len(main):5.1f}%  {k}")
            for fn in cpu_fns:
                inc = [s for s in main if any(fn in f[0] for f in s)]
                if not inc: continue
                print(f"  == {fn}: {100 * len(inc) / len(main):.1f}% of main running time; where the samples land:")
                lf = collections.Counter((s[0][0][:84], s[0][1]) for s in inc)
                for k, v in lf.most_common(6): print(f"      {100 * v / len(inc):5.1f}%  {k[0]}  :{k[1]}")

if __name__ == "__main__":
    main()
