    # Portal renderer — differences from Valve's Portal

Honest accounting of what our procedural portal effect is missing or
mistuned compared to the original Portal / Portal 2 visual. Every item is
something we already discussed; this is the consolidated reference.

Scope: only the disc itself (no surrounding bloom — the user explicitly
doesn't care about that).

## Things we have but tuned wrong (FIXED already, listed for reference)

1. **Wisp prominence too high** — our flames read as distinct fire
   tongues. Portal's wisps are more feathered / subtle; the dominant
   shape is a clean ring with soft fringe, not fire.
   *Fix applied:* contrast `smoothstep(0.18, 0.82)` → `smoothstep(0.30, 0.75)`.

2. **Wisps reached too far inward** — `kReachMax = 0.58` (wisps reach
   58% of the disc inward from the rim). Portal's are more like 25–35%;
   the see-through area dominates the disc.
   *Fix applied:* `kFixedReach = 0.30`.

3. **Inner edge of flame too soft** — a `0.18` halo bled tinted color
   into the transparent centre. Portal's transparent centre is clean.
   *Fix applied:* halo removed; `alpha = intensity` exactly.

4. **Procedurally jagged ring shape** — noise drove the per-angle reach,
   making the inner edge of the ring wavy. Portal's ring is essentially a
   clean smooth ring; the wisps are layered ON it, not eating into it.
   *Fix applied:* noise now modulates BRIGHTNESS (78%–100%) inside a
   fixed-thickness ring, not the geometric reach.

5. **Hot crest peak position** — Portal's bright crescent sits *slightly
   inward* from the geometric rim. Ours peaked AT the rim.
   *Fix applied:* Gaussian crest centred at `radial = 0.93` with `sigma
   = 0.025`.

6. **Dark roots desaturation** — luma-mix at 0.78 made the dark inner
   colour grey-blue / grey-orange. Portal stays saturated even in the
   deepest tips.
   *Fix applied:* desaturation step removed.

## Things we DON'T have (NOT YET FIXED)

7. **Subtle radial speed-lines** — Portal has thin lighter streaks
   running outward from the rim, suggesting energy flowing outward.
   *Status:* not implemented. Could be done with a high-frequency angular
   noise term that brightens specific angular slices radially, or with a
   second additive shader pass. Pure shader work, no architecture
   changes needed.

8. **Particle accents** — Portal has tiny bright particle billboards
   spawned at the rim that drift outward and fade.
   *Status:* not implemented. Requires a particle system, which we don't
   have.

9. **Refraction / distortion of the see-through** — looking through a
   Portal portal, what's behind it has a subtle wobble at the rim,
   suggesting an "energy field". Our see-through is geometrically clean.
   *Status:* not implemented. Requires sampling the framebuffer as a
   texture (offscreen render target → fragment-shader sample with UV
   offset). Backend doesn't expose RTTs yet.

10. **Structured inactive-portal vortex** — Portal's inactive portals
    have a distinct *swirling vortex* pattern in the centre, not random
    fBM. The vortex slowly rotates and has clear concentric "energy
    lines".
    *Status:* ours uses a low-frequency fBM swirl. Could be replaced
    with a polar swirl pattern like `fbm(vec2(angle + radial * k,
    radial))` or a similar deterministic spiral.

11. **Entry/exit visual flash** — when something teleports through,
    Portal briefly lights up both portals.
    *Status:* not implemented. Would need a network packet from the
    server (teleport event) → client-side animation timer → shader
    uniform driving a per-portal brightness boost.

## Things requiring major engine changes

12. **HDR framebuffer** — Portal renders in linear HDR and tone-maps to
    display. Our framebuffer is LDR sRGB so the hot crest literally
    cannot exceed white in framebuffer terms.
    *Status:* requires HDR pipeline changes (RGBA16F render target,
    tone-mapping post-process pass). Significant refactor.

13. **Bloom post-process** — the bright crest in Portal blooms outward
    via the engine's bloom filter. (Excluded from scope — user said they
    don't care about lighting around the portal.)
    *Status:* requires post-process pipeline (blur passes on offscreen
    render target).

14. **Real refraction** — see #9. Needs offscreen render targets so the
    portal shader can sample what's behind it.
    *Status:* requires backend RTT support.

15. **GPU particle system** — see #8.
    *Status:* requires particle subsystem (emitters, GPU update,
    billboard rendering).

16. **Audio cue** — Portal portals have an ambient hum and a louder
    tone when one is active.
    *Status:* requires the audio system, which is still TODO in the
    project per CLAUDE.md.

## Color palette

**Mid stops are now Portal-authoritative**, extracted from the actual
Portal 1 source code at `Portal code/sp/src/game/shared/portal/portal_util_shared.cpp:146`
(`UTIL_Portal_Color`). Dark and hot stops are derived from mid (× 0.30
inward, brightened + slightly washed outward).

| Stop | Blue | Orange |
|---|---|---|
| Dark roots | `(19, 48, 76)` | `(76, 48, 10)` |
| **Mid body (Portal-exact)** | **`(64, 160, 255)`** | **`(255, 160, 32)`** |
| Hot crest  | `(140, 210, 255)` | `(255, 210, 130)` |

Both portals' hot crests stay in their hue (light cyan / warm gold),
NEVER going to pure white — this matches Portal's HDR-rendered output
where the crest is bright but tinted.

## Portal-authoritative timings (now matched)

Extracted from `Portal code/sp/src/game/client/portal/c_prop_portal.cpp:219-252,587-595`:

| Animation | Portal value | Our impl |
|---|---|---|
| Open animation duration | **0.5 s** (`m_fOpenAmount += dt * 2.0`) | `kOpenDurationSec = 0.5` in `ClientPortalManager.hpp` |
| Static fade duration | **1.0 s** (`m_fStaticAmount -= dt`) | `kStaticDurationSec = 1.0` in `ClientPortalManager.hpp` |
| Refraction sub-pass | **Only while opening** (`0 < openAmount < 1`) | Gated in `PortalRenderer.cpp::SeeThroughPass` |
| "Static ping" on partner | When firing portal A, partner B's `m_fStaticAmount = 1.0` (fades over 1s) | `OnPortalSet` sets the other side's `staticPingStartTimeSec` |

## Full audit vs. real Portal source (post-source-extraction pass)

Done by reading every file under `Portal code/sp/src/game/client/portal/` and
`Portal code/sp/src/game/shared/portal/` against ours. Items already fixed
above (1–11) are not repeated.

### A. See-through pipeline (the disc itself)

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| A.1 | **Material system vs uniforms.** Portal layers ~7 distinct materials (`portal_X_dynamicmesh`, `renderfix_dynamicmesh`, `depthdoubler`, `portalstaticoverlay_X`, `portal_stencil_hole`, `portal_refract_X`). | `portalrenderable_flatbasic.cpp:30-44` | Single shader with `uOutlineMode` switch. | Major (architectural) |
| A.2 | **DepthDoubler.** When two portals face each other at recursion level 1, samples the previous frame's view rather than re-rendering — cheap fake recursion. | `portalrenderable_flatbasic.cpp:182-189` + `WillUseDepthDoublerThisDraw()` | Missing. | Major |
| A.3 | **PortalStencilHole material.** Dedicated material for rim boundary detail. | `portalrenderable_flatbasic.h:27` | Missing. | Moderate |
| A.4 | **RenderFix mesh.** A second pass over an outset mesh to repair edge artifacts when the portal slightly overhangs the wall. | `portalrenderable_flatbasic.cpp` (`DrawRenderFixMesh`) | Missing. | Moderate |
| A.5 | Refraction gated to open animation. | `portalrenderable_flatbasic.cpp:1140-1210` | `PortalRenderer.cpp:856-857` | ✅ already match |

### B. Animation state

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| B.1 | **`m_fSecondaryStaticAmount`** — third static timer that blends in at the end of the recursion chain (used when transitioning from "no further view" → "view available" without flicker). | `c_prop_portal.cpp` ClientThink + `materialproxy_portalstatic.cpp:63-66` | Missing. | Moderate |
| B.2 | Open anim 0.5 s, static fade 1.0 s. | `c_prop_portal.cpp:241,224` | `kOpenDurationSec=0.5`, `kStaticDurationSec=1.0` | ✅ match |
| B.3 | Static ping on partner re-fire. | `c_prop_portal.cpp:593-596` | `ClientPortalManager.cpp:46,49` | ✅ match |

### C. Lighting / glows

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| C.1 | **TransformedLighting** — colored dynamic light projected from the portal surface onto surrounding walls. The portal "glows" on its environment. | `c_prop_portal.h:63-67`, `c_prop_portal.cpp:484-488,598-600` | Missing entirely. | Major |
| C.2 | **`cl_portal_emit_light_teleport`** — light flare during teleport (gated convar, default off). | `c_prop_portal.cpp:544` | Missing. | Trivial after C.1 |

### D. Particle effects

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| D.1 | **Two continuous effects per portal.** `portal_X_edge` (rim swirl) + `portal_X_particles` (sparks). | `prop_portal.cpp:299-300` + `particles_manifest.txt:29-30` | One unified spark system, no separate swirl effect. | Moderate |
| D.2 | **One-shot placement feedback effects** — `success`, `near`, `overlap`, `badsurface`, `badvolume`, `cleanser`, `close`, `nofit`. | `prop_portal.cpp:503-608` | Missing entirely. | Major (needs particle data + fizzle classification) |
| D.3 | **Charge/muzzle effects on the gun.** `portal_X_charge`. | `weapon_portalgun.cpp:350,355` | N/A (gun is just an item icon). | Out of scope |

### E. Sounds (inventoried, not implementable yet)

From `game_sounds_portal.txt` / `game_sounds_weapons_portal.txt`:
- Placement success (per color)
- Placement fizzle (cleanser, no-fit, bad-surface, bad-volume, near, overlap)
- Teleport whoosh
- Continuous portal hum
- Projectile flight + impact

We have no audio system → all missing. Blocked on audio infra.

### F. Geometry / sizing

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| F.1 | Aspect ratio. | 64×108 in, ratio **1 : 1.6875** | 1×2, ratio **1 : 2** | Trivial — change `kHalfHeight` from 1.0 to ~0.84375 to match exactly (or accept the voxel-grid alignment) |
| F.2 | Mesh tessellation. | Model file (high res) | 96-segment ellipse fan | ✅ adequate |
| F.3 | Collision depth. | `PORTAL_HALF_DEPTH = 2.0f` (~5 cm) | Effectively zero | ✅ acceptable |

### G. Color values (already mostly matching — see palette table above)

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| G.1 | Blue mid `(64,160,255)`, orange mid `(255,160,32)`. | `portal_util_shared.cpp:146-164` | Same constants in `kBluePalette`/`kOrangePalette`. | ✅ exact match |
| G.2 | Dark + hot stops. | Derived from material at runtime (we don't see exact values). | Mid×0.30 (dark), brightened+washed (hot). | Trivial — these are guesses |
| G.3 | Gravity beam color exists in `UTIL_Portal_Color`: `(242,202,167)`. | source case 0 | Not used (we have no gravity beam). | N/A |

### H. Recursion

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| H.1 | True recursion via `GetRemainingPortalViewDepth()`, stencil-stack levels. | `portalrender.cpp` + `portalrenderable_flatbasic.cpp:187` | Single level only — Phase 7 was attempted and removed (flicker + oblique-near-plane interaction). | Major |
| H.2 | Recursion budget (default 5–10 levels, scaled by perf). | `portalrender.cpp` ConVars | N/A | Major |

### I. Ghost renderables (entity body splitting)

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| I.1 | Entities intersecting the portal plane are clipped on entry side AND re-rendered as ghosts on the exit side, using a shared clip plane. Player legs visible on entry, torso on exit. | `c_prop_portal.cpp:259-480`, `C_PortalGhostRenderable.h` | We render the player *through* the portal (Phase 7 add-back) but do NOT render a clipped ghost body that straddles both sides. | Major |
| I.2 | Held weapons / view models also ghost. | `c_prop_portal.cpp:328-344` | Missing. | Major |

### J. Edge clipping / portal-on-overhang

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| J.1 | `PortalMoved()` builds 12 perimeter clip planes around the ellipse + 2 front/back. RenderFix mesh uses these so the portal correctly truncates if it's near a wall edge. | `portalrenderable_flatbasic.cpp:103-177` | We just draw the full ellipse. If a portal hangs over a wall edge, it would render in midair. | Major |
| J.2 | `CalcFrustumThroughPortal()` computes a tighter render frustum so the destination scene only fills the visible silhouette, not the whole ellipse bbox. | `portalrenderable_flatbasic.cpp` | We use the standard frustum. Not visually wrong but renders more chunks than needed. | Moderate (perf, not visuals) |

### K. Misc

| # | Difference | Portal | Ours | Effort |
|---|---|---|---|---|
| K.1 | **Pulse / breathing.** Portal's pulse (if any) lives in shaders we can't see. | unknown | 0.35 Hz scale ±4 %, 1.2 Hz brightness ±5 %. | ✅ likely close enough |
| K.2 | **Vortex pattern source.** Probably texture-based in Portal. | material textures (proprietary VPK) | Procedural sin spirals. | ✅ ours is fine |
| K.3 | **Flash uniform / teleport tint.** | inferred | `flashIntensity` decaying to 0 over 0.35 s. | ✅ already present |
| K.4 | **Per-color material variants.** Portal duplicates material strings per color (`_1` / `_2`). | systemic | We branch on palette in code. | ✅ equivalent |

## Walk-through-the-portal stack (added together)

These four changes ship as a unit so the player can physically walk
*through* the portal instead of "wall-press → teleport snap":

1. **Wall passthrough in the 1×2 opening.** `PhysicsContext::IsBlockSolid`
   consults a global `PortalPassthroughFn` that returns true for the two
   wall blocks behind any FULLY-PAIRED portal. Wired in
   `PlatformMain.cpp` to `ClientPortalManager::IsBlockBehindActivePortal`.
   Server collision is untouched (no hook installed there).
2. **Plane-crossing teleport trigger.** Replaced the old "rising-edge
   inside the slab" detector in `PortalRegistry::Tick` with a true
   prev→curr signed-distance crossing test on the player's waist
   position, plus a lateral check inside the 1×2 oval. Triggers when the
   body's geometric center actually passes through, not when it first
   touches the wall.
3. **Velocity preservation.** Server derives velocity from the player's
   per-tick position delta, rotates it via `mat3(M)`, and ships it in
   the existing `dx/dy/dz` fields of `ClientboundPlayerPositionPacket`
   via the new `Teleport(...,dx,dy,dz)` overload on `ServerConnection`.
   Client teleport callback writes it directly to `physics.velocity`
   instead of zeroing.
4. **Ghost render (half-body emerging).** New `PlayerRenderer` shader
   uniforms `uModel` and `uClipPlane`. When a player straddles a portal
   pair (test in `ClientPortalManager::GetStraddlingGhost`), the
   renderer draws a transformed copy at the destination with a clip
   plane discarding the un-emerged half — visible both in the main pass
   and the see-through pass.

### Top 10 changes ranked by visual payoff

1. **Ghost renderables (split body across portal).** Most iconic "Portal" trick — when you stand in the doorway you see your own back from the other side. — *major* — **shipped (basic version): exit-side half via uClipPlane on PlayerRenderer; entry-side body clipping not yet implemented**
2. **Dynamic light at portal surface.** Colored glow on the wall around the portal. Makes them feel alive. — *major*
3. **Continuous "edge" swirl effect.** Add a second particle stream (or a tangential ribbon) that rotates around the rim, distinct from the spark emitter. — *moderate*
4. **Placement feedback particle effects.** `success`/`fizzle` bursts. Currently the portal just doesn't appear, with no explanation. — *moderate*
5. **RenderFix mesh + edge clipping.** Properly truncates the portal when placed near a wall edge so it doesn't float in midair. — *major* but high correctness value
6. **Portal-in-portal recursion (or DepthDoubler).** Even 2-level recursion would dramatically sell the effect. — *major*
7. **Aspect ratio 1:1.6875 instead of 1:2.** One-line change — tighter visual match. — *trivial*
8. **`m_fSecondaryStaticAmount`.** Clean recursion-chain transitions. — *moderate*
9. **PortalStencilHole / boundary refinement.** Sharpen rim edge with a dedicated stencil-aware pass. — *moderate*
10. **Audio cues** (when audio system lands). The placement *thump* and ambient hum are huge for atmosphere. — *blocked on audio*

Items 1–2 account for ~70 % of the remaining "doesn't quite feel like Portal" perception. Items 3–4 are quick wins. The rest are polish.

## Code locations

- Shader (vert + frag inline): `src/client/renderer/portal/PortalRenderer.cpp`
- Color palettes (`kBluePalette` / `kOrangePalette`): same file, top of `Render()`.
- Pipeline states (StencilMark / DepthClear / DepthRefill / Outline /
  InactivePortal): also in that file's anonymous namespace.

## Tuning knobs (in shader source)

If anything reads slightly off after a code change, these are the dials:

| Knob | Current | Effect |
|---|---|---|
| `kFixedReach` | 0.30 | Ring thickness as fraction of disc radius |
| `kCrestCenter` | 0.93 | Where the bright crescent sits (1 = rim) |
| `kCrestSigma` | 0.025 | Crescent thickness |
| `mix(0.78, 1.00, flame)` | 0.78–1.00 | Brightness modulation range from noise |
| `smoothstep(0.30, 0.75, flame)` | 0.30–0.75 | Noise contrast (lower → softer wisps) |
| `0.13` / `0.08` rotation rates | rad/s | CW / CCW noise rotation speeds |
| `0.30` warp magnitude | — | Domain-warp strength |
| `0.05` × `sin(t * 1.20)` | ±5%, 1.2 Hz | Brightness pulse |
