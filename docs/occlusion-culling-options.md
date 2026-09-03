# Occlusion Culling: Current State and Line-of-Sight Options

Written 2026-08-30, after the render-optimization push. Context for a future
"make occlusion culling stronger" project: what exists, what the sound options
are, and which to pick for which symptom.

## What the engine has today

`src/client/renderer/culling/SectionOcclusionGraph.*` implements Minecraft's
air-connectivity BFS (Tommaso's algorithm): a section is *reachable* when an
air path connects it to the camera's section, stepping face-to-face through
each section's compiled `VisibilitySet` (`VisGraph.*`, flood fill over
non-occluding blocks at mesh time), with direction accumulation and a
distant line-of-sight raycast.

> **History (2026-08-30):** a parity audit found our step rules more
> permissive than MC's (missing backtrack bit, ungated source-direction
> widening, raycast skipped on partial updates) and applied MC-verbatim
> fixes. In practice they produced FALSE CULLING — first angle-specific
> holes behind large structures (vanilla's own single-ray raycast fallacy),
> and after disabling the raycast, worse random holes — because this
> engine's cached-slot + partial-update graph is not structurally identical
> to MC's rebuild-the-graph flow, so MC's step rules are not drop-in. The
> audit was fully reverted the same day. Lesson for any future attempt:
> MC-verbatim step-rule changes must be validated with a hole-hunt (flat
> world + large solid structure, orbit angles) before shipping, and the
> permissive behaviour is the safe default. The phantom-caves over-draw it
> would have fixed is cosmetic in the F+C inspection view, not a real cost. The reachable set is
then frustum-filtered per frame.

Measured effect (M4 Air, RD 32, "OG with Structs" ground view):
~8,000+ meshed sections → 4,902 reachable → 3,171 in frustum. Live numbers:
Render Controls panel, "occlusion: X reachable → Y in frustum".

Two things that LOOK like bugs but are correct:

- **Caves the player cannot actually see still render.** Reachability through
  any winding air path is not line of sight. This is exactly vanilla's
  behaviour and the gap this document is about.
- **Fully solid sections adjacent to reachable caves render.** Their faces ARE
  the cave's walls; the wall you see from inside a cave lives in the solid
  section next to it.

Design invariant to preserve in ANY change: **over-draw is acceptable,
under-draw is not.** Every scheme below is judged first on "can it ever hide
something the player can see" (holes). The blind fallbacks (unstreamed camera
chunk → draw everything; see the sky-flash notes in CLAUDE.md memory) must
survive any rework.

## Why per-section eye rays are unsound (the tempting wrong answer)

"Cast a ray from the eye to each section; cull it if the ray hits terrain."

The asymmetry: a ray that REACHES the section proves visibility; a ray that
MISSES proves nothing. A section is visible if any sight line reaches any of
its points through any gap — including a one-block hole at an oblique angle.
No finite set of rays (center, 8 corners, N samples) is conservative, so the
test can only say "definitely visible", never "definitely hidden" — and a
culler that cannot say "hidden" culls nothing. Making it "safe" by treating a
miss as visible removes all culling; treating a miss as hidden makes holes.
Cost would actually be fine (~5k sections × a DDA march ≈ a few ms on a worker
thread); correctness is what is unfixable. Do not build this.

## Option A — Cone-narrowing BFS (recommended first step)

Keep the existing BFS, but carry a shrinking *sight cone* (a conservative
angular/AABB bound of directions sight could still travel) along each path.
Each traversed section clips the cone by the geometry of the faces the path
entered/exited; when the cone pinches closed, traversal stops. A path that
winds through bends cannot carry a straight sight line, and the cone knows it.

- **Kills:** the "caves under my feet render" class — deep winding cave
  networks, staircases, anything behind more than a couple of bends.
- **Does nothing for:** open terrain (surface shell, the 750-blocks-up view) —
  everything there really is visible.
- **Safety:** conservative BY CONSTRUCTION if the cone bound is kept
  conservative (always clip to a superset of true sight directions). Holes are
  only possible via an implementation bug, not by design.
- **Cost:** a few ops per BFS step, on the same worker thread that runs the
  BFS today. No GPU work, no latency, no popping.
- **Effort:** moderate. Touches only SectionOcclusionGraph. Known technique
  (MC experimented ~1.9; the "advanced cave culling" articles by Tommaso
  Checchi describe the family).
- **Verify with:** the F+C free camera (gap-bridged draws are already disabled
  under it, so what renders IS the reachable set) + the reachable/visible
  panel counters + `OBEY_SKIP`-style interleaved fps A/B.

## Option B — Hi-Z / depth-pyramid occlusion (the big hammer, Vulkan-only)

Industry standard GPU occlusion: take last frame's depth buffer, build a mip
pyramid (each mip = max depth of the 4 texels below), test every candidate
section's AABB against the pyramid; sections entirely behind terrain fail.
Pairs naturally with the VK indirect draw path (`vkCmdDrawIndexedIndirect` —
the GPU can cull its own command list, zero CPU readback).

- **Kills:** everything behind terrain — mountains hide what is behind them,
  and it is the only option that helps top-down sky views (the terrain surface
  occludes the caves below it in actual depth).
- **Safety caveat:** it tests against LAST frame's depth, so a fast camera cut
  can pop sections one frame late. Standard mitigations: reproject the
  pyramid by the camera delta, inflate AABBs, treat "no depth data" as
  visible. This is an artifact class you manage, not eliminate.
- **Platform:** Vulkan only here — macOS GL is 4.1 (no compute shaders). GL
  would keep the BFS-only path.
- **Effort:** the largest of the options — depth pyramid pass, culling
  compute pass, indirect-buffer plumbing, and the swapchain/depth lifetime
  details (our depth attachment is currently `storeOp = DONT_CARE`; it would
  need storing or a dedicated pre-pass).

## Option C — Per-section GPU occlusion queries (ruled out)

MC's old "Advanced OpenGL". One boolean query per section drawn against the
depth buffer. Ruled out on this stack: per-query overhead on Apple's GL driver
is brutal (a single GL timer query measured ~2.3 ms of forced flush here —
see CLAUDE.md profiling notes), thousands of queries need heavy batching, and
the result latency forces the same one-frame-late compromises as Hi-Z with
none of its throughput. Strictly dominated by Option B.

## Recommendation

1. If underground over-draw is the complaint: **Option A**. Conservative,
   cheap, self-contained, verifiable with existing tooling. Note the honest
   ceiling first: those cave sections are mostly small meshes — measure the
   fps delta with the panel counters before and after, and expect the win to
   be modest fps-wise even when the counter drops a lot.
2. If the goal is the sky-view / behind-the-mountain case: **Option B**, after
   Option A, Vulkan-only, budgeted as a real project.
3. Never per-section eye rays (see above); never Option C on Apple drivers.

Related instrumentation already in place: Render Controls occlusion counters,
F+C free camera (exact draw set), `Sections/Reachable` / `Sections/Visible`
Tracy plots, Occlusion Culling live kill-switch checkbox.
