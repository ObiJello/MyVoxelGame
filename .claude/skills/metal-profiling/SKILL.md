---
name: metal-profiling
description: Metal GPU profiling, capture and instrumentation for ObeyCraft's native --metal backend on macOS — Xcode GPU capture and its Performance view, Instruments / xctrace (Metal System Trace, counter templates), the Metal Performance HUD (per-encoder GPU timing), MTL_* validation and capture env vars, Apple GPU counter/limiter reading, the engine's OBEY_MTL_* switches, and every trap met on this M4 / Xcode 26.2 machine. Use for any request to measure, capture, trace, profile or optimize the Metal renderer, or to read .gputrace / .trace / HUD results.
---

# Metal profiling for ObeyCraft

Apple's tools first (user rule), the engine's own counters second, OBEY_SKIP subtraction only as a cross-check. Every number comes with the method that produced it; serial Xcode percentages and live overlapped milliseconds are never mixed in one sentence.

The machine this was written on: M4 MacBook Air (fanless, throttles ~5 % per run slot), macOS 26.6.2, Xcode 26.2 (17C52), Metal Toolchain installed (`xcrun metal`). Apple's `game-porting-skills` plugin is installed; its `translating-to-metal4-api/references/tbdr-architecture.md`, `presenting-metal-drawables/references/frame-pacing.md` and `using-metal-validation` skills are the authority on TBDR costs, pacing and validation — read them rather than restating them here.

## The three measurements, and which question each answers

| Question | Tool | Output | Caveat |
|---|---|---|---|
| What does a frame cost *live*, per encoder, with real overlap? | Metal Performance HUD with encoder timing (recipe 2), or the engine's `OBEY_MTL_GPU_TIMERS=1` plots | ms per encoder per frame | HUD encoder timing is blank while the app attaches counter sample buffers — keep `OBEY_MTL_GPU_TIMERS` off for HUD runs |
| *Why* does an encoder cost that (limiters, overdraw, per-draw, per-line)? | Xcode GPU capture → Profile after replay (recipe 1) | 226 counters per encoder, per-draw counters, per-line shader costs, heat map | The replay profiles ONE frame at the **Medium** performance state by default (session popover: clock icon → Performance State Maximum, Execution Mode Overlapping/Serial). Its percentages are each encoder's share of the replayed frame's GPU work; a live frame also overlaps with its neighbours (frame N+1's vertex work under frame N's fragments), so they never convert to "percent of a live frame" |
| *When* in the frame does CPU/GPU work happen; presents, drawable waits, thermal state? | Instruments Metal System Trace / Game Performance (recipe 3) | timeline, 13 live limiter counters | Only "Performance Limiters" is offered live on this M4, and only ATTACHED: that counter set crashes Instruments' GPUPlugin on all-process/launched recordings (trap 4) |

## Recipe 1 — Xcode GPU capture (the deep one)

```bash
# MTL_CAPTURE_ENABLED must be in the LAUNCH environment (Metal reads it when the framework loads)
open -W --env MTL_CAPTURE_ENABLED=1 "$HOME/Applications/MyVoxelGame-tracy.app" --args \
  --metal --world "OG with Structs" --replay tour --replay-hold 8 --quit-after 70 \
  --exec-at 1 "/weather clear 1d" --env OBEY_MTL_CAPTURE_AT=45
# -> ~/Library/Application Support/obeycraft/captures/metal-HH-MM-SS.gputrace (one frame)
open -a Xcode "<that .gputrace>"
```

In Xcode: tick **Profile after replay**, **Replay**. The profiler session's settings live behind the clock icon at the top right of the Performance view (popover: Performance State Medium/Maximum, GPU Execution Mode Overlapping/Serial, **Profile** re-runs it); the replay screen itself has no state control. Then:
- Summary: draws, vertices, GPU time at the chosen state; Insights.
- Performance → Overview: per-pipeline cost %, SIMD groups, registers; Counters → Encoders → share button → **Export Encoder Counters** (CSV; `scripts/counters_csv.py` prints it readably). Per-draw counters fill in over ~1 h — leave Xcode idle, clicking around it stalls the collection.
- Shaders → double-click (real mouse, `scripts/xc.sh dbl x y`) → per-line costs. Needs the metallib compiled for the host OS: automatic in Tracy builds (`TRACY_ENABLE` implies `METAL_SHADER_PROFILING`; the log line "Metal shader profiling: shaders.metallib targets macOS …" confirms it); a 12.7.6-targeted metallib shows "per line performance data not available".
- Memory: sort by Allocated; Dependencies: pass graph; Heat Map: per-pixel cost / overdraw.
- The debug navigator groups draws under our `GpuDebugGroup` names (Sky, Opaque terrain, …) with cost % per group.

Xcode's own UI is SwiftUI; AppleScript `click` on its tables does nothing — use `scripts/mclick` (CGEvent) via `scripts/xc.sh`.

## Recipe 2 — Metal Performance HUD (live per-encoder GPU time)

```bash
tools/play.sh tracy --hud --metal --world "OG with Structs" --replay tour ...   # Game Mode copy + HUD + log
# by hand:
open -W --env MTL_HUD_ENABLED=1 --env MTL_HUD_ENCODER_TIMING_ENABLED=1 --env MTL_HUD_LOG_ENABLED=1 \
  --env MTL_HUD_ELEMENTS=fps,gputime,frameinterval,toplabeledencoders,gputimeline,shaders,metalcpu \
  --stderr hud.log "$HOME/Applications/MyVoxelGame-tracy.app" --args --metal ...
scripts/hud_log.py hud.log
```
The HUD's log is NSLog on the process's **stderr** (`... metal-HUD: frame,MetalMB,appMB,interval,gpu,interval,gpu,...` per presented frame), so it must be captured with `open --stderr` or by running the executable directly — `log show` never sees it (verified 2026-10-06). The per-encoder times (Encoder GPU, Vertex/Fragment/Blit ms and %) are overlay-only: read them off a screenshot. FPS/GPU time are per **presented** frame: with vsync off the mailbox renders several frames per present, so the GPU time is their sum. `MTL_HUD_INSIGHTS_ENABLED=1` flags encoder splits, blit-induced splits, serial shader compiles. `OBEY_MTL_HUD=1` turns the overlay on from inside the game (`developerHUDProperties`), without the env. Full variable list: `references/apple-tools.md`.

## Recipe 3 — Instruments / xctrace

- GUI: Instruments → Metal System Trace (or **Game Performance**, Xcode 26: adds Metal GPU Counters + Metal Performance Overview) → Target: **attach** to the running profiling bundle (`MyVoxelGame-tracy`) when a counter set is on; "All Processes" is fine only with Counter Set **None**. Recording Options → Counter Set / Shader Timeline as the template says → Record; the game must outlive the recording's stop time (quitting first leaves an unreadable trace).
- CLI (attach mode, what `tools/play.sh tracy --gpu-trace --gpu-template=<path>` does): `xcrun xctrace record --template <t> --attach <pid> --time-limit 45s --output x.trace`. Never `--launch` (trap 2) and never `--all-processes` with a counter template (trap 4).
- Export: `xcrun xctrace export --input x.trace --toc`, then `--xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]'`. Useful schemas: `metal-gpu-intervals` (col 16 = encoder object id), `metal-object-label`, `metal-application-intervals` (drawable waits), `gpu-counter-value` / `metal-gpu-counter-intervals` (counters; **not** `gpu-counter-intervals`), `metal-shader-profiler-intervals`, `gpu-performance-state-intervals`, `device-thermal-state-intervals`, `os-signpost`. `scripts/trace_encoders.py <trace>.export` pairs encoders with labels.
- Templates live in the repo root: `gpu-limiters.tracetemplate` (Performance Limiters, timeline off — attach only) and `gpu-shaders.tracetemplate` (no counters, shader timeline on — records in any mode, but see trap 3). The counter set and timeline switch are stored inside the template (`counterprofile`, `shaderprofiler` keys), so a CLI run inherits whatever the GUI saved.

## Recipe 4 — Validation and visual load/store checks

```bash
open -W --env MTL_DEBUG_LAYER=1 --env MTL_DEBUG_LAYER_ERROR_MODE=nslog \
  --env MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1 --env MTL_DEBUG_LAYER_VALIDATE_STORE_ACTIONS=1 \
  --env MTL_SHADER_VALIDATION=1 --env MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 <app> --args --metal ...
```
Fuchsia = a `DontCare` load was read; red/white checkerboard = a `DontCare` store was read later. Shader validation runs at ~8 fps. `man MetalValidation` is the full reference; the Apple `using-metal-validation` skill covers scoping.

## Recipe 5 — Offline shader inspection (no capture needed)

```bash
xcrun metal -std=macos-metal2.4 -Os -gline-tables-only -frecord-sources -c shaders/metal/terrain_vk.vert.metal -o t.air
D=$(dirname "$(xcrun -f metal)"); "$D/applegpu-nt" -archs            # applegpu_g16g = M4 family
"$D/metal-objdump" --disassemble t.air                                 # AIR; native via metal-nt -arch
```
`metal-nt` / `applegpu-nt` translate AIR to native GPU code for a named architecture — static instruction and register counts without running the game. Compile warnings: add `-Weverything`.

## Engine switches (native Metal backend)

| Switch | Effect |
|---|---|
| `OBEY_MTL_CAPTURE_AT=<sec>` | capture one frame at that time (needs `MTL_CAPTURE_ENABLED=1` in the launch env) |
| F3+U | capture the next frame at runtime (same env requirement) |
| `OBEY_MTL_GPU_TIMERS=1` / `=split` | per-encoder GPU timestamps → Tracy plots `Gpu/*`, F3 pass times; `split` makes every top-level debug group its own encoder (exact per-stage, costs a tile store/load per split). Disables the HUD's encoder timing while on |
| `OBEY_MTL_ERRORS=1` | per-encoder execution status on command-buffer errors |
| `OBEY_MTL_HUD=1` | Performance HUD via `developerHUDProperties` (no env plumbing) |
| `OBEY_MTL_MAILBOX=0`, `OBEY_MTL_MAILBOX_PENDING=n` | vsync-off mailbox off / depth |
| `OBEY_SKIP=<stage>` | stage subtraction A/B (cross-check only) |

## Measurement protocol (every reported Metal number)

1. Tracy config built with `METAL_SHADER_PROFILING=ON`; `tools/play.sh tracy` (copies to `~/Applications`, Game Mode engages — verify "Full screen gaming session is now active"), full screen, clear weather (`--exec-at 1 "/weather clear 1d"`; rain only when the rain path is the subject — user rule 2026-10-07, the Tier 1–5 series ran in rain), `tour` replay, RD 32 (edit `options.txt`, restore 8 afterwards), vsync off, notifications closed (`scripts/close_notifs.applescript`), `pmset -g therm` clean, 60 s between runs, ABBA ordering for any A/B.
2. Live cost per encoder: HUD (recipe 2) or `OBEY_MTL_GPU_TIMERS=1` + `tools/tracy_report.py`.
3. Why: Xcode capture at Maximum (recipe 1).
4. Timeline/presents: Instruments (recipe 3).
5. For effects under ~10 %, the A/B goes INSIDE one run: gate the change with `Render::DevSkip("<token>")` (DevRenderSkip.hpp), run once with `--env OBEY_SKIP=<token> --env OBEY_SKIP_PERIOD=5`, and read `tools/tracy_report.py --phase <capture>` — frame stats and the Gpu/* plot means for the skip-on vs skip-off frames of the same run (1 s settle dropped after each switch). The five-run ABBA series drifts 25 % on the fanless Air by day (292 → 211 fps, 2026-10-07) and hides anything smaller. ABBA stays for changes that cannot be toggled at runtime (shader rewrites), paired with a same-frame Xcode capture.
6. Accept a change only on an interleaved A/B with a kill switch; report fps, 1 %-low, 0.1 %-low, per-encoder ms, and a panorama/screenshot parity check. The parity shot is taken against the PRE-CHANGE build (or every new switch off at once), never against one kill switch while another new change stays on: the 2026-10-06 rain-streak parity compared streaks on/off with the depth-less targets on in both, and missed that config 4 was blending additively (Tier 1).

## Traps (each one cost real time)

1. `MTL_CAPTURE_ENABLED=1` must be in the launch environment (`open --env`); `setenv` inside the game is too late.
2. `xctrace --launch` / Instruments "Launch" resolve the app through LaunchServices by name and started the *installed* game — even when given the profiling copy's executable path after that copy got its own `CFBundleIdentifier` (`com.yourcompany.MyVoxelGame.tracy`, set by `play.sh` and the build's `~/Applications` copy). Never launch from Instruments; launch with `open` and attach (or record all processes).
3. The shader timeline (`gpu-shaders.tracetemplate`) has produced **no samples** in any mode tried: attached, all-processes, `--launch`, and all-processes with the game launched under `MTL_CAPTURE_ENABLED=1` all export `metal-shader-profiler-intervals` with 0 rows (the shader list fills, the intervals don't). Not yet tried: Xcode's own "Profile" of an Xcode project. Per-line shader costs come from the Xcode GPU capture instead (recipe 1).
3b. Tracy's `TracyMetal.hmm` (0.14.1) zones never resolve on this GPU (every zone a 5 ns stub): its render-pass attachment samples startOfVertex + endOfFragment with the other two indices `MTLCounterDontSample`, which this device does not write. The engine feeds Tracy's GPU context from its own four-sample attachments instead (`MetalInstrumentation.mm`).
4. The **Performance Limiters counter set** on an all-process or launched recording crashed `GPUPlugin` (SIGSEGV) in xctrace four times and in Instruments.app once (Xcode 26.2); the timeline-only template in the same mode did not. Counter templates work **attached** (the 2026-09/10 counter traces were attached). Test a template once on a short run before relying on it.
5. Xcode replay = ONE frame profiled at the Medium performance state (overlapping execution of its own encoders, but no neighbouring frames to overlap with); convert nothing to "percent of a live frame". Set Maximum in the session popover for real-clock ms.
6. Per-line shader costs need the metallib built for the host OS. CLion resets ad-hoc `cmake -D` flags on its next configure and an `option()` default never overrides a cached value, so the Tracy build implies it in CMake (`TRACY_ENABLE` → host-targeted metallib); other configs keep 12.7.6.
7. Quitting the game before xctrace's `--time-limit` elapses leaves a "Document Missing Template Error" trace.
8. `pgrep -n -x MyVoxelGame` can pick another running copy; attach by the bundle path.
9. Per-draw counter collection in Xcode stalls while you click in the window; it also speeds up over time (~1 h total for 4 k draws).
10. SwiftUI tables ignore accessibility clicks — use `scripts/mclick`; keep the terminal out of the front when scripting (`activate` first). Capture windows report an empty name through System Events (`first window whose name contains "gputrace"` finds nothing) and `Window`-menu items are named after the current view, not the capture — drive one capture window at a time as `front window` (close the others first; `xcode_profile.sh` does).
11. The Air throttles ~5 % per run slot; only within-run or ABBA comparisons are trustworthy; Game Mode changes the GPU clocks — never compare a Game Mode run with a windowed one.
12. The HUD counts presented frames; the mailbox renders more.
13. `MTLDevice.counterSets` on Apple GPUs = `timestamp` only, sampled at stage (encoder) boundaries — per-draw timing in the app is impossible; the engine's timers are per encoder.
14. `xcrun gpudebug`, `gpucapture`, `metalperftrace` (Apple's CLI tools used by the `using-gpudebug` / `using-gpucapture` skills) need macOS 27 — not on this Mac; don't try them.
15. `--replay-hold` must be shorter than `--quit-after` minus the path length: hold 75 + quit 55 parked the player on the first pose for every run of 2026-10-06 (a still frame, no playback). Verify a run moved with `grep '\[Replay\] playing' latest.log`; `tracy_report.py` cuts to the played path only when there is one.
16. A look regression that the user reports but the A/B shots missed: get a one-frame capture at the user's spot (`MTL_CAPTURE_ENABLED=1` launch + F3+U), then read the DRAW's pipeline descriptor in Xcode (Bound Resources → Render Pipeline State → Color Attachments 0): blend factors, write mask, formats — what the GPU actually used, not what the code meant. The rain target's additive blend showed there in two minutes after an hour of code reading.
17. Never edit `ab_run.sh` (or any script) while a run of it is in flight: bash reads the file incrementally, and the edit lands as a syntax error mid-run (`line 69: syntax error near unexpected token`, 2026-10-07 — the B1 trace was saved but its report line was lost; `tracy_report.py <label>.tracy` recovers it). Copy the script before editing if a series is running.
18. The user's in-game settings leak into runs: `ab_run.sh` forces only what it forces (RD 32, vsync off, fog on since 2026-10-07) and the rest is whatever `options.txt` holds — the user toggled vsync and fog between two series and the next ABBA measured a 60 fps cap. Diff the `options.backup.txt` of two series (sorted; the file is rewritten in hash order) before comparing them. Fog was OFF in every series before 2026-10-07 14:45.
19. `xcode_profile.sh` must wait for a STABLE "Profiling GPU Trace" box, not the first change: the spinner animates, so the first poll broke out at once and a frame with indirect command buffers (profiled for minutes) was screenshotted mid-profile with no Performance view, and the next capture was profiled at the Medium state because the Maximum clicks landed early (2026-10-07). Done = two polls 5 s apart equal and different from the pre-replay box. A capture frame that was an unpresented mailbox frame shows a magenta Summary image — the stand-in — which is fine; the Performance view is what is read.
21. **Verdicts come from the still-frame paired test**: `STILL=1 ab_run.sh <label> --env OBEY_SKIP=<token> --env OBEY_SKIP_PERIOD=2` then `tracy_report.py --phase --settle 0.3 --window 12:58` — ±0.3–0.9 % intervals regardless of heat (2026-10-08). The tour run (`OBEY_SKIP_PERIOD=2`, `--settle 0.3`) is ±10 % because the scene changes; use it to confirm a still-frame win survives movement, never alone for a verdict. Anything shipped needs a phase token (DevSkip) so this test can run; a static env switch cannot be paired.
20. Clear weather is the measurement default since 2026-10-07 (`ab_run.sh`; `RAIN=1` for rain work). Every Tier 1–5 number before that was measured in rain and is not comparable with clear-weather runs. A result can flip between regimes: GPU-driven terrain won hot in rain (+3.6 %) and lost cooled in clear weather (−10 %) — measure a candidate default in both before shipping it.

## Files

- `scripts/xc.sh out.png [click x y | dbl x y | key <applescript> | wait s]…` — activate Xcode, real clicks, screenshot (builds `mclick` on first use).
- `scripts/mclick.swift` — CGEvent clicker (`mclick x y [count] [right]`).
- `scripts/movframes.swift` — frames of a `screencapture -V <sec> out.mov` recording as PNGs with each frame's mean RGB (`movframes in.mov out_dir [fps] [maxWidth] [startSec] [endSec]`): the way to catch a one-frame glitch (a flash, a bare frame between screens) that a screenshot loop misses — record at the display rate, print the means, dump the jump at full width. Found the join-in-rain clear-sky frame and the Save-and-Quit bare-world frames on 2026-10-07. Title-screen flows (the panorama hand-over) need real clicks on the title/world list; the `--world` CLI path skips the panorama entirely.
- `scripts/counters_csv.py` — Xcode encoder-counter CSV, transposed with limiters flagged.
- `scripts/hud_log.py` — HUD `metal-HUD:` log summary.
- `scripts/trace_encoders.py` — per-process / per-encoder GPU ms from an xctrace export dir.
- `scripts/metal_device_probe.swift` — counter sets and sampling points of this device.
- `scripts/close_notifs.applescript` — closes Notification Center banners (overlays cap presentation).
- `references/counters.md` — Apple's definition of every limiter/utilization counter and what to do about each.
- `references/apple-tools.md` — API/CLI/env-var facts with Apple URLs (counter sampling, capture, HUD, validation, xctrace schemas, Metal 4).
- `references/tbdr.md` — the five TBDR rules applied to this engine's pass graph.
22. **Per-line shader costs need a hover.** In a profiled capture the source view with per-line % opens only from the jump icon that appears on HOVER at the right end of the function row (Shaders tab → select the pipeline → bottom pane → FragmentFunction); `mclick` clicks without moving the pointer there first, and double-click / Quick Look on the row open nothing (2026-10-08). Add a `move x y` to mclick.swift before trying again; until then the breakdown stops at the per-pipeline table.
23. **Verify the metallib carries the shader change** before measuring it: `xcrun metal-source <bundle>/Contents/Resources/shaders/metal/shaders.metallib -o dir` extracts the embedded sources; grep them for the edit. The bundle's shaders lagged any shader-only build until 2026-10-08 (the POST_BUILD staging ran only on a relink; now the `stage_shaders` rule), and a stale metallib shows up as the OLD source in Xcode's per-line view, or as no terrain when the varyings mismatch. The per-line view itself: select a draw in the API-call navigator → Bound Resources → double-click the FragmentFunction row (no hover needed; the Shaders-tab routes in trap 22 do not open it).
24. **CPU zone times scale with the CPU clock.** Two tour captures an hour apart showed every unchanged main-thread zone (frustum filter, sort, mob render) +40 % on the hotter run. A CPU change is judged by its zone's RATIO to an unchanged zone in the same capture (e.g. DirtyWalk / FrustumFilter), or by alternating runs — never by the zone's absolute ms across two runs. Content checks for a scheduler change: Draws/Entries, Draws/Sections, Upload/Sections per frame must match (2026-10-08).
25. **A loaded machine inflates every CPU zone together.** Zoom (capture + encode) and the IDEs' indexers (CLion, Rider after a burst of edits) took two tour captures to 192 and 262 fps with the GPU idle and ClientTick 5× slower. `ab_run.sh` now waits for the 1-minute load average < 1.8; if unchanged zones all moved by the same factor, discard the run. The overlap tool's lift blames zones that are merely PRESENT in a stall (the sound tick) when the real cost has no zone (the block-entity tick, 2 ms every tick, no lift) — zone the body of the stalling zone until the time attributes (2026-10-08).
