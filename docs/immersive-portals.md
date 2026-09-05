# Immersive portals

See-through, walk-through portals in the style of the Immersive Portals mod
(iPortalTeam/ImmersivePortalsMod, branch `1.21`, LGPL — clone it for reference,
do not decompile). A portal is a planar surface in one dimension that shows,
and leads to, another place in the same or another dimension, with no
loading screen.

Feature flag: `ENABLE_IMMERSIVE_PORTALS` (`src/common/core/Features.hpp`),
distinct from `ENABLE_PORTAL_GUN`. In immersive mode the gun's linked pairs
are mirrored into this system (phase 8).

Server toggle: `immersivePortals` in the world's `level.dat`
(`/gamerule immersive_portals true|false`, `--vanilla-portals` at launch for
a new world). Off = vanilla purple nether portals, vanilla gun; the code for
both stays compiled in.

All eight phases were written in one pass on 2026-09-01 **without a build**;
the gates below are what to test, in order, once it compiles.

## Phase plan

| # | Phase | Gate |
|---|---|---|
| 1 | **Data model, server registry, client mirror, sync, `/portal`** | `/portal make` shows a surface; it survives relog and save; leaves with its chunk |
| 2 | **Client multi-dimension** — `ClientLevel` per dimension (chunks, meshes, GPU data, entities, portals), swappable active level, `DimensionScopeS2C` stream marker, `ChangeDimensionS2C::KeepPrevious` | Nether round trip keeps both levels resident, nothing leaks (30 s GC of empty levels) |
| 3 | **Server per-player chunk loaders** — own view + one per nearby portal at the far side (radius falloff, direct loaders uncapped, indirect ones capped at 8) + indirect quarter loaders, per-dimension send state, throttled tickets, entity tracking per loader, process-wide entity ids | Far-side chunks arrive when approaching a portal, unload when leaving |
| 4 | **Rendering** — `ImmersivePortalRenderer`: stencil-layer recursion (depth 5), far-plane depth clear, clip plane + depth clamp (no oblique matrix), per-dimension sky/fog, portal-bounded frustum, far-side entities, render-yourself | Nether visible through a frame at full frame rate |
| 5 | **Seamless teleport** — per-frame eye-segment crossing (`ImmersivePortalTraveler`), `PortalTeleportC2S` validated server-side, no respawn, dimension on `PlayerMoveC2S` | Walk through with no hitch or flicker |
| 6 | **Nether frames** — any closed obsidian loop (`ImmersiveFrame`), air interior, far-side search with tickets, adaptive matching (exact loop, or rectangles at integer scale), frame build on ground, 4-record clusters, integrity sweep, break = whole cluster | Light an obsidian loop, walk into the Nether |
| 7 | **Entities and interaction** — mobs/items/orbs cross (`EntityPortalTravel`), mobs chase through, cross-portal collision (passthrough + far-side solidity hooks), dig/place through portals (`dimensionId` on `BlockActionC2S` / `UseItemOnC2S`, `PlayerSession::InteractionWorld`), particles per level | Throw an item through; a zombie follows you; stand on the far floor before your eye crosses; break a block through a portal |
| 8 | **Gun across dimensions; command rotation/scale/mirror** — gun pairs mirrored into the immersive registry as oval bi-way clusters (`PortalRegistry` immersive section); `/portal make_mirror`, `set_rotation`, `set_scale` | Blue in the Overworld, orange in the Nether: see and walk through; a rotated portal turns you; a mirror shows you |

## How the pieces fit (phases 2–8)

### Client levels — `src/client/world/ClientLevel.hpp`

One `ClientLevel` per dimension owns the chunk manager, mesh manager, chunk
renderer, block access, item/orb/mob/falling-block managers and the portal
store. The old globals (`g_clientChunkManager`, `g_clientMeshManager`,
`g_chunkRenderer`, `g_clientBlockAccess`, the entity managers, the raycast
block access) are raw pointers rebound by `ClientLevels::Bind`. The **active**
level is the one the player stands in; the **bound** level is whatever the
current code is looking at — a packet's dimension (`DimensionScopeS2C` sets
`PacketDimension`, `BindForPacket` before each apply), a portal view
(`WithLevel(dim, fn)`), or the far level a dig reaches into. Mesh results and
GPU uploads are routed by the `dimension` stamped on the job.

### Server loaders — `src/server/world/watch/ChunkLoader.hpp`

`IntegratedServer::ComputeChunkLoaders(session)` builds the loader list every
watch pass; `PlayerSession::UpdateChunkTracking` diffs it per dimension and
`SendNextChunks` walks all dimensions nearest-loader-first. Everything that
used to ask "is the session in this dimension" asks `LoadsDimension` /
`IsWatching(dim, chunk)` / `HasSentChunk(dim, chunk)` instead, and
`ServerConnection::SendPacketIn(dim, …)` scopes every positional packet.

### Rendering — `src/client/renderer/portal/ImmersivePortalRenderer.hpp`

`Render(...)` takes a `LevelRenderFn` (PlatformMain's `renderLevelView`)
that draws whatever level is bound from a `ViewContext` (camera, view,
projection, portal-bounded frustum, layer, portal). Per portal: mark the
stencil (IncrClamp), clear depth inside the mask by drawing the surface with a
far-plane projection, `WithLevel(dest)` + view × inverse(TransformMatrix),
`ChunkRenderer::SetPortalClipPlane(InnerClipPlane)`, `SetStencilOverride`,
recurse, restore depth by drawing the surface again, clamp the stencil back.
Mirrors use `SetCullInvert`. `OBEY_PORTAL_OUTLINES=1` draws outlines.

### Crossing — `src/client/portal/ImmersivePortalTraveler.hpp`

Per frame, the segment from last frame's eye to this frame's is tested
against every teleportable portal of the active level (`RaytraceSegment`,
front→back only). On a hit the player's feet, velocity, yaw and pitch go
through the portal's transform, `ClientLevels::SetActive(dest, keepPrevious)`
swaps the level without dropping the old one, and `PortalTeleportC2S` tells
the server, which validates (≤16 blocks from its position, ≤20 from the
portal) and moves the `ServerPlayer` with `ChangeDimension(..., keepPrevious)`
— no respawn, no client-load restart.

### Nether — `src/server/portal/NetherPortalGeneration.hpp`

Fire on a closed obsidian loop (`ImmersiveFrame::Find`, flood fill in the
three planes) removes the fire, keeps the interior AIR, requests the far
chunks under tickets and completes when they are resident (or after 600
ticks): link an existing frame of the same shape (rectangles also at integer
scale → `Portal::scale`), else build one on the nearest ground. Four records
per link. Every ~233 ticks each nether portal's frame is checked; a missing
obsidian or a filled interior removes the cluster (`World::SetBlock` reports
removed obsidian immediately).

### Entities — `src/server/portal/EntityPortalTravel.hpp`

After each level's `TickPortals`: mobs (eye segment oldPosition→position),
items and orbs (pos−vel→pos) that pierced a surface are transformed; a
cross-dimension one is `Extract`ed from the old manager and `AddExisting` /
`AdoptWithId`'d into the new one with the same id, after the old level's
tracker announced the removal. `OnClientPortalTeleport` sends the mobs that
targeted the player to the portal (`Chase` records); on arrival they retarget
the player's view in the new level.

### Collision — `src/client/portal/ImmersivePortalCollision.hpp`

Two physics hooks (`SetPortalPassthroughFn`, `SetPortalExtraSolidFn`): cells
of this world behind a portal the player is entering do not collide; the same
cells are solid where the far world is solid at their transformed position.

### Interaction

`ClientPlayer::UpdateRaycast` continues the ray through the first interactable
surface into the far level (`WithLevel`) and records `lastBlockHitDimension`
/ `lastBlockHitPortalId`; the controller ticks with that level bound; the
packets carry the dimension; `PlayerSession::InteractionWorld` resolves the
world and maps the eye through the nearest portal into it for the reach test,
and latches the world for resyncs. The outline and crack overlay draw inside
the portal view. Particles remember their level (`MobParticleSystem`), are
drained from every level, and are drawn per view.

### Gun — `src/server/portal/PortalRegistry.cpp` (immersive section)

In immersive mode a placed pair is mirrored into the immersive registry as
`AddBiWay` of a 1×2 oval (`PortalKind::PortalGun`, tag `gun:<id>`), rotation =
blue's (right, up, n) → orange's (−right, up, −n). The client gun manager
receives no `PortalSetS2C`, so the gun's own renderer, ghosts and prediction
stay idle; the gun's own renderer draws the rim per level (also inside portal
views), the particle system draws its sparks, and the teleport flash, floor
exit fling and arrival rules are applied by the immersive paths. Gun pairs and
the gun item persist (`data/portal_gun.json`, the item's instance id component).
See the parity sheet (artifact "Immersive Portals Parity") for the full
feature status and test walk.

### `/portal` — `src/server/commands/PortalCommand.cpp`

```
/portal make        <w> <h> <dim> <x> <y> <z>   one-way, one face
/portal make_biway  <w> <h> <dim> <x> <y> <z>   two-way (front + reverse)
/portal make_full   <w> <h> <dim> <x> <y> <z>   two-way, two faces (4 records)
/portal make_mirror <w> <h>                     a mirror in front of you
/portal set_rotation <ax> <ay> <az> <degrees>   nearest portal (+ its cluster)
/portal set_scale    <scale>                    nearest portal (+ its cluster)
/portal list | info | remove <id> | remove_all
```

## Testing (after the first successful build)

1. **Phase 1/4** — `/portal make_full 2 3 overworld ~ ~ ~20`: a see-through
   surface 1.5 blocks ahead shows the place 20 blocks away; walking around
   it shows the flipped face. `/portal remove <id>` removes the cluster.
2. **Phase 2/3** — `/portal make_full 3 3 nether ~ ~ ~`: the Nether appears
   in the surface as its chunks stream (log `[Session] … loaders`). Stand
   still: chunks around the far side arrive up to the loader radius.
3. **Phase 5** — walk into it: no loading screen, the sky and fog switch,
   the portal behind you shows the Overworld. Walk back.
4. **Phase 6** — build any closed obsidian loop (an L, a circle), light it:
   the fire vanishes and the far side appears in the hole after the Nether
   chunks load; walk through. Break a frame block: both sides vanish.
   `/gamerule immersive_portals false` + relog: vanilla purple portals.
5. **Phase 7** — throw an item through; drop XP through; let a zombie chase
   you through; place a `/portal make_full` with a floor on the far side and
   a pit on the near side, walk onto it: you stand on the far floor; break
   and place blocks through the surface (outline drawn inside the view).
6. **Phase 8** — portal gun: blue here, walk through a nether frame, orange
   there; both are see-through ovals linked across dimensions.
   `/portal set_rotation 0 1 0 90` on a command portal turns you 90° on the
   way through; `/portal set_scale 2` makes the far side twice as large;
   `/portal make_mirror 2 3` shows you.

## Phase 1 — what exists

### Data model — `src/common/portal/ImmersivePortal.hpp`

`Game::Immersive::Portal`: `origin`, `axisW`, `axisH`, `width`, `height` on
this side; `destDimension`, `destination`, `rotation` (quat), `scale` on the
other; `flags` (`PortalFlag::*`), `kind`, `specificPlayerId`, cluster links
(`reversePortalId`, `flippedPortalId`, `parallelPortalId`), `shape`
(rectangle or a 2-D triangle mesh in the surface plane), `tag`.

Conventions match the mod exactly so its math carries over:

- `Normal() = normalize(cross(axisW, axisH))`. A viewer on the positive side
  sees through; an entity crosses from positive to negative.
- `TransformPoint(p) = destination + rotation · (scale · (p − origin))`.
  A mirror (`PortalFlag::Mirror`) reflects across its own plane instead.
- `ContentDirection() = rotation · (−normal)` is where the far world lies;
  `InnerClipPlane()` is the destination plane with that kept side,
  `OuterClipPlane()` the origin plane with the normal kept.
- `RaytraceSegment(from, to)` is the one crossing test (front → back, inside
  the shape).
- `MakeReverse()` / `MakeFlipped()` build cluster partners. A two-way,
  two-faced portal is four records (`AddBiWayBiFaced`), as in the mod's
  `generateBiWayBiFacedPortal`.

### Server — `src/server/portal/ImmersivePortalRegistry.hpp`

One registry per server (ids are global; partners live across dimensions),
owned by `IntegratedServer` (`ImmersivePortals()`). Secondary index by
(dimension, origin chunk).

Visibility rule: a portal is sent to every client that has been **sent** its
origin chunk. It goes out right behind the chunk data
(`PlayerSession::SendNextChunks` → `IntegratedServer::OnChunkSentToClient` →
`SyncChunkToClient`), and adds/updates/removes are broadcast to the same set.
The client drops a portal when its chunk unloads, so its lifetime is its
chunk's — no "forget everything" pass is needed on dimension change.

Persistence: `<save>/data/immersive_portals.json`, saved with `level.dat`
(shutdown, autosave, pause).

### Wire — `packets/game/ImmersivePortalPackets.hpp`

`ImmersivePortalSyncS2C` (0x4A, full record upsert) and
`ImmersivePortalRemoveS2C` (0x4B). Positions are doubles, axes floats
(re-orthonormalised on read).

### Client — `src/client/portal/ClientImmersivePortals.hpp`

Dumb mirror keyed by id with a per-chunk index; written only by the packet
handler and `ClientChunkManager` (unload / clear). Until phase 2 it is
keyed by chunk position alone, like the chunk manager.

The see-through pass is `ImmersivePortalRenderer` (phase 4); `OBEY_PORTAL_OUTLINES=1`
adds the phase-1 wireframe outline on top.

## Existing pieces the later phases build on

- Portal-gun renderer: stencil see-through on GL and Vulkan, `SetStencilOverride`,
  clip plane via `gl_ClipDistance[0]` on both backends, oblique projection on GL
  only (`kVkZCorrect` breaks it on Vulkan — use the clip plane there, as the mod
  does everywhere). Recursion was removed for flicker; phase 4 redoes it the
  mod's way (clip plane + depth clamp, no oblique matrix).
- `ChunkRenderer::RenderAll(camera, frustum, projectionOverride)` is
  re-entrant; `kReachableSlots = 4` caches per camera section;
  `m_useProjectionOverride` marks "not the main view"; `EntityFrame` lets the
  entity renderers run once per recursion.
- Every entity renderer takes explicit view/projection; terrain vertices are
  absolute world space, so a second camera costs draw calls only.

## World options: world wrap and dimension stack (fixed 2026-09-03)

Both are **global portals** (`PortalFlag::Global`): world-sized, chunkless
surfaces the server rebuilds from the world options at startup
(`IntegratedServer::EnsureGlobalPortals`, all three dimensions, idempotent per
tag; never saved). They are sent to a client with the first chunk it receives
of the dimension, and again whenever its chunk set for that dimension had
emptied (`PlayerSession::MarkGlobalPortalsSynced`).

- **World wrap** (`worldWrapSize` blocks across, the Nether at an eighth):
  one bi-way surface per axis at ±half, so the east border shows and leads to
  the west border of the same dimension. The corner quadrant beyond both
  borders is not covered (neither is it in the mod).
- **Dimension stack**: each dimension's floor (its `DimensionMinY`) is a
  bi-way surface onto the ceiling (`minY + logicalHeight`) of the next —
  Overworld over Nether over End over Overworld, 1:1 coordinates. Bedrock in
  MC's bedrock bands becomes **obsidian** (`ChunkProvider::CompleteChunkLoad`,
  generated or loaded chunks alike). The way through is DUG: standing on the
  Overworld's floor you stand on the Nether's roof (cross-portal collision),
  mine it through the seam under your feet (`Interactable`), and drop in when
  your eye crosses. The End is above the Overworld: y = 320 leads to the End's
  y = 0 (void — fly up to the islands), the Nether's y = 0 to the End's y = 256.

Things every part of the system has to get right for a global surface, and
where each is handled — all of these were wrong once:

| Concern | Where | Rule |
|---|---|---|
| Finding it | `ImmersivePortalRegistry::CollectNear` | Globals are not chunk-indexed; a linear pass answers them, and their extent must never widen the chunk walk (a 200 000-block seam made it 625 M lookups per call) |
| Loading the far side | `IntegratedServer::ComputeChunkLoaders` | The mod's global loader: centred on the player's image, radius `view − ⌊distance/16⌋`, engaged within the view distance. Wrap borders keep the full radius (seen across); stack seams past 32 blocks are capped at 8 (seen only through holes) |
| Standing on the far floor | `Physics.cpp` box check + `ImmersivePortalCollision` | The all-air early-out is skipped while a portal is engaged (`SetPortalCollisionActive`) — the void under the floor IS all air here. An unloaded far chunk counts as solid |
| Digging through | `EnsureGlobalPortals` flags | `Interactable`, so `ClientPlayer::UpdateRaycast` and `PlayerSession::InteractionWorld` reach through |
| Rendering | `ImmersivePortalRenderer::RenderLayer` | No occlusion seed for a global (it would follow the camera along the plane and restart the BFS every section — the wrap flicker); frustum-only, as the mod |
| Meshing the far side of a wrap | `ChunkRenderer::GetPortalViewSections` | Same-level portal views record their sections; the scheduler walks them after the main view's |
| Crossing a wrap border | `PlatformMain` crossing site | `ChunkRenderer::OnCameraTeleport` drops the reachable-set cache (the fallback slot was a world's width away) |
| Arriving inside rock | `PlatformMain` nudge | For a global crossing the nudge may go back toward the plane as a last resort — the reverse surface faces the other way, so it cannot re-cross |
| Editing | `/portal set_*` | `NearestPortal` skips globals |

Two more rules keep the sky sane (both were wrong on the first build — the
Overworld's sky was the End's void, with a Nether-fog band under the horizon):

- **Ceiling seams under an open sky are not drawn.** The reverse of a floor
  seam is the lower dimension's ceiling; under the Nether's roof it is only
  seen through a dug hole and must stay visible (that is how you dig UP), but
  over the Overworld or the End it would replace the whole sky with the
  underside of the world above. Those lose `Visible`, `Interactable` and
  `CrossPortalCollision`; the Overworld's keeps `Teleportable` (the End's
  floor is void, so flying up past 320 is safe), the End's is inert (up
  would land inside the Nether's obsidian floor).
- **Every portal surface is fogged by the near world** (`DrawFogOverlay`,
  `uOutlineMode 3` in `portal.frag` / `portal_vk.frag`): after the far view,
  the surface is drawn again inside its mask with the near world's fog
  colour and the terrain shader's fog value at each pixel as alpha. A window
  at a distance fogs like the wall around it; a floor seam fades into the
  sky at the horizon instead of painting a band of Nether fog there.

## Main-thread cost (Tracy, 2026-09-04)

Capture `untitled.tracy` (Vulkan, VSync on, ~46 s, a screenful of gun and
nether portals plus the stack seams). Per frame on the main thread:

| Zone | mean | p95 | max | note |
|---|---|---|---|---|
| Present (`Vk.QueueSubmit`) | 11.3 ms | 15.7 | 30.8 | VSync wait inside MoltenVK's submit; not CPU work |
| Render | 4.3 ms | 19.7 | 43.8 | of which portals 2.6 / 12.5 / 37.2 |
| chunk passes per frame | 10.8 | 52 | 74 | main view + every portal view, recursion included |
| far-level mesh scheduling | 1.25 ms | 5.9 | 21.0 | was run once per PORTAL into a far level, up to 19×/frame |
| `FrustumFilter` (all views) | 1.1 ms | 4.7 | 6.8 | ~100 µs per view — the view count is the multiplier |
| `Vk.SingleTimeSubmit+Drain` | 0.5 ms × 9/s | | 20.0 | Static-buffer creation mid-game (portal surface meshes) |

What changed for it:

- **Which portals are drawn, how far a view draws, what the server loads —
  the mod's rules at its "good" performance level, number for number
  (replaced the earlier 16-view budget and screen-coverage floor,
  2026-09-04; the mod's fps / tick-time performance monitors and lag latch
  were deliberately NOT ported).** Client side
  (`ImmersivePortalRenderer::RenderRange` / `PortalRenderDistance`): a
  portal is drawn within `render distance × 16` blocks (16 under the
  "Reduced Portals" video option; divided by the layer past the first
  nesting; multiplied by an enlarging outer portal's scale, at most 512);
  at most 200 portals per frame, every layer counted
  (`OBEY_PORTAL_RENDER_LIMIT`); not the reverse of the portal looked
  through, nor the portal two layers up when it is that reverse. A view
  through a portal draws with the render distance (1.4 × the far area for
  scale > 2; a third under Reduced Portals), with the far fog composed for
  that distance (`ChunkRenderer::SetRenderDistanceOverride`; portal-view
  cache slots are keyed by distance). Server side
  (`IntegratedServer::ComputeChunkLoaders`): portals are looked for within
  8 chunks, indirect ones within 2; direct loaders are the full view
  distance within 15 blocks of the surface and a third beyond — the mod
  has two thirds between 5 and 15 and caps everything at 8; both changed
  2026-09-05 so a 32-chunk render distance shows 32 chunks through a
  portal from anywhere near it; global surfaces load `view distance − distance
  in chunks` at the player's image, 2 to 16; indirect loaders are a quarter
  capped at 8 (16 for scale > 2), a global's min(8, a third).
- **Far levels scheduled once per frame** after the whole pass
  (`m_farLevels`), from the union of every view's sections
  (`ChunkRenderer::GetPortalViewSections`, cleared by that scheduling).
- **Portal surface meshes are Dynamic buffers**: no staging copy, no drain.

Measuring FPS: with VSync on the frame is capped at the display rate and
the Present zone absorbs the slack; compare CPU frame time (Render +
MeshSchedule + uploads) or turn VSync off.

### Second capture (`untitled2.tracy`, VSync off, after the changes above)

| Per frame | mean | p95 | p99 | max |
|---|---|---|---|---|
| frame interval | 4.7 ms (213 fps) | 10.9 | 15.7 | 67.8 (one Cocoa event stall) |
| Render | 2.9 ms | 6.9 | 9.5 | 23.4 |
| chunk passes | 3.3 | 6 | 8 | 12 |
| portal pass | 0.70 ms | 2.3 | 3.3 | 14.0 |
| far-level scheduling | 0.28 ms | 1.6 | 2.1 | 10.8 (≤1 run/frame) |
| `Vk.BeginFrame` (GPU fence) | 1.4 ms | 4.4 | 6.5 | 9.8 |
| Present | 0.9 ms | 2.9 | 12.3 | 28.3 |

Still open, in order: ~1.6 ms of Render had no zone (now zoned: EnvUpdate,
ChunkPass.Main, Render.Mobs, PortalParticles, Particles.*, GunViewmodel);
`FrustumFilter` 0.44 ms/frame (135 µs × 3.3 views); the remaining GPU drains
were `CreateTexture2D` (lazy mob/item textures) inside RemotePlayers and Hud —
now one submit per texture instead of three, and zoned; `MeshSchedule` has a
22 ms outlier outside `ScheduleMeshBuildsWithSnapshots` (worker-position lock?).

### Third capture (`untitled3.tracy`, VSync off, zones added)

254 fps mean; Render 2.1 ms mean / 5.5 p95; portal pass 0.47 / 1.9; main
chunk pass 0.30 / 0.73. The 1.2 ms that no zone covered was the **entity
cut passes** in PlatformMain (`EntityCut` zone now): a near-side and a
far-side entity pass for EVERY visible portal within 64 blocks, on screen or
not, ~0.25 ms per portal. Now only portals whose far view was drawn last
frame (`ImmersivePortalRenderer::DrewLastFrame`) qualify, and the near-side
per-portal pass runs only for portals the first pass saw a straddling box in.
Mob, particle, viewmodel and environment work all measured under 0.05 ms.
Remaining: `FrustumFilter` ~0.4 ms/frame; the GPU fence (0.86 ms mean, 3.5
p95) is now the next ceiling; 18 lazy texture loads per session still drain.

### Fourth capture (`untitled4.tracy`, before Game Mode)

223 fps mean, 1%-low ~64. `EntityCut` is 0.03 ms (was ~0.25 ms per portal).
The "unattributed" Render time of the earlier captures was `Vk.BeginFrame`
(the frames-in-flight fence wait), which sits INSIDE the Render zone: 1.3 ms
mean, 4.4 p95. Real main-thread render work is ~1.2 ms; the whole CPU frame
is ~3 ms; the GPU frame is ~4.5 ms at native Retina resolution — the game is
GPU-bound now. Remaining CPU: MeshSchedule 0.58 ms mean (3.2 p99), Present
spikes (12.8 ms p99, MoltenVK waiting for a drawable when the GPU is behind).

### Fifth capture (`untitled5.tracy`, Game Mode on, `tools/play.sh`)

| | capture 4 (no Game Mode) | capture 5 (Game Mode) |
|---|---|---|
| fps mean | 223 | 250 |
| fps at the 5% / 1% worst frames | 99 / 64 | 126 / 85 |
| frames over 16.7 ms | 0.53 % | 0.18 % |
| Render CPU (minus GPU fence), mean / p99 | 1.06 / 4.06 ms | 0.77 / 2.80 ms |
| Present, p99 | 12.8 ms | 4.6 ms |
| GPU fence, mean | 1.48 ms | 1.71 ms |

Game Mode buys the tail: fewer stalls, Present's drawable waits mostly
gone, main-thread work ~25 % cheaper from less contention. The GPU fence is
unchanged, so the GPU is still the ceiling. One 328 ms frame at 11.9 s is
the FIRST rendered frame after world load: every draw submission and the
block-entity pass took 70-100 ms once — MoltenVK compiling the pipelines on
first use. A pipeline warm-up during the loading screen would remove it.

### Pipeline warm-up (2026-09-04)

The Vulkan backend keeps `cache/vk_pipeline_manifest.txt` next to its
pipeline cache: every (shader paths, PipelineState) it ever built, unioned
across sessions. `RenderBackend::WarmPipelines` (VK only; GL is a no-op)
rebuilds that set on the second loading-screen frame after a world join,
once per process, so the first frame of play no longer stalls on lazy Metal
pipeline compiles. The first session after this change still builds lazily
(no manifest yet); every later one is warm, including for dimensions and
renderers first seen in some earlier session.

Verified in `untitled6.tracy`: the warm-up built 27 manifest pipelines in
3.4 ms behind the overlay; the first frame of play went from 326 ms to an
ordinary one (worst Render frame of the session: 19.5 ms). 24 pipelines the
manifest did not yet know (stencil-layer and blend variants first used that
session) were still built lazily, 5.8 ms in total, 1.1 ms worst — and they
are in the manifest from now on. The one 94 ms interval is `PollEvents`
during the full-screen transition, Cocoa's, not ours.

Seed + version stamp (2026-09-04): `assets/vk_pipeline_manifest.txt` ships a
copy of the developer's manifest so a player's FIRST session is warm
(`tools/refresh_pipeline_manifest.sh` updates it from the local cache before
a release). The player's own manifest is stamped with `GAME_VERSION` and is
discarded on a version change, so retired shaders/states never accumulate;
the seed is read whatever its stamp, since it ships with the build.

### Main-thread CPU trims (2026-09-04, from capture 7)

- `ClientChunkManager::ScheduleMeshBuildsWithSnapshots` now walks the DIRTY
  set and asks the renderer "in view?" (`IsMainViewSection` /
  `IsPortalViewSection`, key sets kept next to the two lists) instead of
  walking the ~4,000-section visible list asking "dirty?". Same candidates,
  cost ∝ what changed. Was 0.9 ms per run, 11 % of main-thread zone time.
- `ChunkRenderer::PrepareVisibleSections` frustum filter tests each chunk
  COLUMN once per frame (memo grid `m_columnCull`, tri-state
  `Frustum::TestAABB`) and only per-section inside columns the frustum edge
  crosses. Was 127 µs per view, 6 %.
- `PlayerSession::SendNextChunks` has a 12 ms per-tick serialization budget
  (bursts of 20-27 ms after joins/crossings were every tick over 25 ms).

### GPU capture (`gpu-03-47-19.trace` + `Untitled.tracy`, 2026-09-04)

First Metal System Trace of a session (`tools/play.sh tracy --gpu-trace
--vulkan`, 47 s of a 74 s flight through portals; read with
`tools/gpu_report.py --tracy`). Apple's system trace has no counters from
the command line, so attribution is by regression against the Tracy plots.

| | value |
|---|---|
| GPU active | 95% of the trace (two channels 90%) — the GPU is the ceiling |
| Vertex per frame | 3.60 ms mean, p99 8.9, max 15.9 |
| Fragment per frame | 1.33 ms mean, p99 2.9 |
| Vertex time model | 1.0 ms + 0.97 µs × sections drawn, r = 0.94 (fragment r = 0.60) |
| Sections drawn per frame | main view 2,715 mean (max 10,461); portal views 389 mean, 13% of the total |
| Drawable waits | 9% of the trace on the main thread (GPU-bound symptom) |
| Thermal / perf state | Nominal; GPU at Maximum 43 of 47 s |

The frame is vertex-stage bound (tiler: vertex shading + binning) and the
cost is proportional to sections drawn, not to portals. Two levers, in
order of measured payoff: fewer sections per frame (the occlusion BFS
already does what MC does; a smaller far-section budget would be a
deviation), and a cheaper section — the 32-byte `TerrainVertex` fetched
through Apple's tiler. The `Geom/Vertices` and `Geom/Indices` plots added
this day are what decides between them: if vertex time follows vertices
better than sections, shrink the vertex; if it follows sections, the
per-draw overhead is the cost.

The trace's 1 kHz symbolised CPU profile also reattributed the main
thread: while streaming, `ScheduleMeshBuildsWithSnapshots` was 27% of the
main thread's running time, most of it `HasAllNeighborChunks` asked once
per dirty SECTION (24 × 8 hash lookups per unbuilt column per frame); it is
now asked once per column per pass.

### Second GPU capture (`gpu-04-53-09.trace` + `Untitled2.tracy`, 2026-09-04)

Same route with the `Geom/*` plots and the scheduler hoist built in.

| | value |
|---|---|
| GPU active | 96%; vertex 3.21 ms mean (p99 10.0), fragment 1.35 ms |
| Vertex time vs Sections/Visible | r = 0.937, 1.27 µs per section |
| Vertex time vs Geom/Vertices | **r = 0.960, 0.57 ns per vertex**, 0.12 ms fixed |
| Vertices per frame | 0.4 M to 21 M, mean ~6.5 M; ~2,300 vertices per visible section |
| Scheduler | 613 µs/run (was 953), MeshSchedule 0.35 ms/frame mean (was 0.61) |

Vertex count predicts the GPU better than section count, and the rate is
the tell: 0.57 ns per 32-byte vertex is ~56 GB/s of vertex fetch, a large
share of an M4's unified-memory bandwidth, with a trivial vertex shader.
The frame is **vertex-fetch-bound**. The lever is the vertex itself:
`TerrainVertex` (32 bytes: float3 pos, float2 uv, RGBA8, 4×unorm16 tile
rect) packed towards 16 bytes, which halves the bytes the tiler streams
per frame. Second lever, smaller: vertex COUNT (merge ratio of the greedy
mesher, `Mesh/QuadsMerged`).

Remaining main-thread CPU (Instruments, running time only, ~38% of wall):
scheduler 20% (was 27%; the per-section view test on far dirty columns is
now one column lookup), PrepareVisibleSections 12%, draw-list sort 10%,
MoltenVK encode 16%, GetLoadedChunkCount 2.8% (now a counter).

### Vertex packing — measured (`gpu-06-12-23.trace` + `Untitled4.tracy`, 2026-09-04)

Same flight as capture 10, packed build, Metal System Trace at 45 s (a 90 s
recording produced a 3.8 GB raw file xctrace could not finalise — keep
recordings at 45 s).

| GPU vertex stage | before (capture 10) | after (capture 12) | change |
|---|---|---|---|
| per vertex (regression on Geom/Vertices) | 0.56 ns, r = 0.99 | 0.42 ns, r = 0.97 | −25% |
| mean per frame, ~2,200 main-view sections both runs | 3.21 ms | 2.64 ms | −18% |
| p99 per frame | 10.0 ms | 6.4 ms | −36% |
| fragment stage, mean | 1.35 ms | 1.26 ms | unchanged |
| GPU-side fps mean / 1%-worst | 209 / 70 | 238 / 103 | |
| Tracy fps mean / 1%-low | 199 / 64 | 235 / 93 | |

Halving the bytes bought a quarter of the per-vertex cost, not half: fetch
was a real part of the vertex-stage bill, but most of it is the tiler's
per-vertex and per-primitive work, which only fewer vertices reduce. The
fixed part of the frame stayed the same and the fragment stage did not
move, as expected for a change that only shrinks vertex reads.

### Vertex packing (2026-09-04) — what was done

Done after the second GPU capture: `TerrainVertex` 32 → 16 bytes (see
CLAUDE.md "Terrain vertex format"). Expected from the 0.57 ns/vertex fetch
model: vertex-stage time roughly halved at equal geometry, i.e. the 7 to
10 ms vertex frames on open vistas becoming 4 to 5 ms. To verify: the same
`tools/play.sh tracy --gpu-trace=90 --vulkan` flight and
`tools/gpu_report.py <trace> --tracy <capture>`; the `Geom/Vertices` slope
(µs per kvertex) is the number to compare — 0.57 before.

### Mesh census and face-direction groups (2026-09-04)

`OBEY_MESH_CENSUS=1` on the packed build, 43k sections built: opaque 73%,
cutout 25%, translucent 1%; stone + deepslate + netherrack 41%; plants
(grass, kelp, seagrass, flowers) ~8%; only **7% of quads greedy-merged**
(stone 4%, leaves 36%). The merger rejects faces whose corner colours
differ, and baked AO makes almost every cave face differ — the AO-pattern
merge (AO corners carried per quad and re-applied per block in the
shader, MC's diagonal rule) is the proposed fix; not yet written.

Written first, as approved: face-direction groups (CLAUDE.md
"Face-direction groups"). Measured (`gpu-07-16-43.trace` + `Untitled5.tracy`
vs capture 12, same route):

| | before | after |
|---|---|---|
| drawn vertices per section | 2,256 | 1,384 (−39%) |
| GPU vertex stage per frame, mean | 2.64 ms | 2.36 ms (−11%, with 5% more sections) |
| vertex time per section (regression) | 0.84 µs | 0.79 µs |
| GPU fence wait per frame | 1.81 ms | 1.28 ms |
| sub-draws per frame | 2,534 | 7,641 |
| vkQueueSubmit CPU per frame | 1.16 ms | 1.39 ms |
| fps mean / 1%-low (Tracy) | 235 / 93 | 244 / 67* |

*the 1%-low is one 135 ms stall in a 55 s session; 5%-worst went 129 → 126.

The lesson: removing 39% of vertices bought ~10% of vertex-stage time,
so the tiler's cost is dominated by FRONT-facing primitives (binning),
not by vertices — back faces were already cheap, culled after shading
and before binning. Fewer front-facing triangles is what pays, which is
exactly what the AO-pattern merge produces (one rectangle for a run of
faces). Kept: pixel-identical, net positive, 0.2 ms of CPU hidden behind
the GPU.

### AO-pattern merge — measured (`gpu-08-05-45.trace` + `untitled6.tracy`, 2026-09-04)

The greedy merger now compares tint * face shade and a per-face AO corner
byte (4 x 2-bit levels) instead of the baked colour; merged rectangles carry
the byte in alpha (TerrainVertex bit 14) and the fragment shader repeats the
block's AO gradient per block with the same two-triangle interpolation the
GPU applies to an unmerged quad (diagonal (1,0)-(0,1) for every face).
Differences from unmerged are limited to 8-bit rounding order (<= 1/255).
Census on the merged build: stone 9% merged rectangles (was 4%), 12% of all
quads (was 7%); the census now reports faces removed rather than rectangles.

Measured against the face-group capture (same route; this run was
thermally throttled, "Fair" for 33 of 42 s, the earlier one was Nominal):

| | face groups (capture 13) | AO merge (capture 14) |
|---|---|---|
| drawn vertices per section | 1,384 | 1,267 (−8.5%) |
| sections drawn per frame (all views) | 2,577 | 2,843 (+10%) |
| drawn vertices per frame | 3.58 M | 3.65 M |
| GPU vertex stage per frame, mean | 2.36 ms | 2.25 ms |
| vertex time per section (regression) | 0.79 µs | 0.71 µs (−10%) |
| vertex time per kvertex (regression) | 0.64 µs | 0.65 µs |
| GPU fragment per frame | 1.42 ms | 1.35 ms |
| GPU-side fps mean / 1%-worst | 244 / 109 | 241 / 92 (throttled) |
| Tracy fps mean / 5%-worst / 1%-worst | 244 / 126 / 67 | 229 / 122 / 84 |

Unlike the face groups, this time vertex time fell in step with vertices
(0.64 → 0.65 µs per kvertex, i.e. unchanged per vertex) — the removed
vertices were front-facing primitives, which is what the tiler bills for.
The per-section cost is the honest number: 0.84 (packed) → 0.79 (face
groups) → 0.71 µs (AO merge). The gain is modest because the merge only
reaches the opaque layer's flat runs; the cutout layer (35% of built
vertices: leaves, plants) merges nothing, and the fps mean on the throttled
run is not comparable.

### Baseline for the vertex-reduction series (`gpu-08-33-54.trace` + `baseline.tracy`, 2026-09-04)

Cool, plugged-in run of the AO-merge build (thermal Nominal for all 40 s,
GPU state Maximum 37 s), the reference every step below is compared to:

| | baseline |
|---|---|
| drawn vertices per section | 1,301 |
| sections drawn per frame (all views) | 2,802 (main 2,493 + portals 309) |
| drawn vertices per frame | 3.66 M |
| GPU vertex stage per frame, mean / p99 | 2.46 ms / 6.04 |
| GPU fragment per frame | 1.47 ms |
| vertex time per section / per kvertex | 0.79 µs / 0.68 µs |
| GPU-side fps mean / 1%-worst | 234 / 102 |
| Tracy fps mean / 5%-worst / 1%-worst | 233 / 120 / 71 |

### Step 1 — face map: merge across AO patterns — measured (`gpu-09-05-34.trace` + `change1.tracy`, 2026-09-04)

The per-block AO byte moved out of the vertex into a **face map** — two
RGBA8 texels per covered block (tint*shade + AO codes; sprite id) stored
behind the section's vertices in the mega-buffer region and read through a
buffer texture over the slab VBO (GL `samplerBuffer`, Vulkan uniform texel
buffer, set 4). The merge test no longer looks at AO, so a cave wall with
a different gradient on every face merges; the fragment shader fetches the
block's record from floor(uv) and rebuilds its own gradient. Records also
carry the sprite and colour per block, so step 2 (merging across block
types) is a merge-rule change only. Pixel-identical by the same argument
as the AO merge (same gradient maths, 8-bit rounding only). Memory: 8
bytes per merged block face, in place of the 64 bytes of vertices it
replaces.

Measured against the baseline (both cool, Nominal throughout):

| | baseline | step 1 face map |
|---|---|---|
| drawn vertices per section | 1,301 | 913 (−30%) |
| sections drawn per frame (all views) | 2,802 | 2,782 |
| drawn vertices per frame | 3.66 M | 2.54 M |
| GPU vertex stage per frame, mean / p99 | 2.46 / 6.04 ms | 1.87 / 4.07 ms (−24%) |
| GPU fragment per frame | 1.47 ms | 1.62 ms (+0.15, the record fetches) |
| vertex time per section | 0.79 µs | 0.57 µs |
| GPU-side fps mean / 1%-worst | 234 / 102 | 259 / 117 |
| Tracy fps mean / 5%-worst / 1%-worst | 233 / 120 / 71 | 268 / 146 / 73 |

The GPU is now "balanced" by the report's verdict (vertex 1.87 vs fragment
1.62 ms): the vertex stage stopped being the sole ceiling. The fragment cost
of the two extra texelFetches per merged pixel is real but small. Bug found
on the way: a new field in the middle of GLBackend's positional-initialised
texture record broke every GL texture bind (black title screen) — fields
go at the end there.

### Step 2 — merge across block types — measured (`gpu-09-23-06.trace` + `change2.tracy`, 2026-09-04)

With colour, AO and sprite per block in the face map, the merge test has
nothing left to compare: `matches` is now only "same coplanar-partner
state" (`Greedy::QuadKey` returns a constant), so a plane of mixed stone,
andesite, gravel and deepslate — or oak and birch leaves — is one rectangle.
Pixel-identical for the same reason as step 1: every block is shaded from
its own record, and its sprite is sampled with its own rect and gradients.

Measured (cool, Nominal), against step 1:

| | step 1 | step 2 |
|---|---|---|
| drawn vertices per section | 913 | 901 (−1.3%) |
| sections drawn per frame (all views) | 2,782 | 3,093 (+11%, route) |
| GPU vertex stage per frame, mean | 1.87 ms | 1.99 ms |
| vertex time per section | 0.57 µs | 0.60 µs |
| GPU-side fps mean / 1%-worst | 259 / 117 | 250 / 97 |
| Tracy fps mean / 5%-worst | 268 / 146 | 259 / 128 |

Within noise: this route showed 11% more sections and the per-section
vertex count barely moved. Mixed block types on ONE plane are rare —
cave walls are mostly one stone, and stone/deepslate boundaries are a
horizontal band, one plane each. Kept (no cost, pixel-identical), but the
merger is now at its ceiling for full-cube faces; what remains per section
is plane fragmentation (rough caves), the 16-block section edge, and the
cutout layer's cross-model plants.

### Underground share of drawn sections (2026-09-04, `OBEY_DUMP_VISIBLE=1` + `tools/underground_share.py`)

52 one-second samples of the real view (flying at y 230, a cave descent to
y -14, walking on the surface), scored against the world's OCEAN_FLOOR
heightmaps: of 149k drawn sections, **33% sit entirely below the terrain
surface, 22% by 16 blocks or more**. With the camera above ground the
underground share is 34% mean (median 34%, max 70%); flying high over
hills it is 30-45%. Inside caves it is naturally 60-100%. Those are
sections the BFS reaches through some cave mouth but that rock hides from
this camera; a conservative section occlusion pass (solid-box software
depth) could remove most of the "deep" 22% — the largest lever left, since
every vertex cost scales with sections drawn (0.6 µs/section).

### Step 3 — software occlusion of sections (2026-09-04; first capture `gpu-09-53-22.trace` + `change3.tracy`)

Built as described in CLAUDE.md "Software occlusion of sections": solid
layer runs of visible sections become occluder boxes in a 256-wide CPU
depth buffer per view; a section whose projected rectangle lies entirely
behind them is not drawn. Draw-list only — reachable set, keys and mesh
scheduling untouched. Expected: up to the 22% "deep" share of sections
above ground, at a few hundred µs of render-thread CPU per view
(`OcclusionCull` zone) — the trade to check is that CPU against the GPU
vertex time it removes. To verify: capture (Sections/Occluded plot, the
zone, sections drawn per frame vs baseline) and OBEY_NO_OCCLUSION=1 A/B.

First capture (per-section boxes, no merging): only **4% of visible
sections culled** (50 of 1,247 per view) at **294 µs per view** (2.4
views/frame = 0.7 ms of render thread), GPU vertex 1.78 ms, fps 243. The
cause: inner-conservative coverage leaves every pixel straddling the seam
between two touching section boxes empty, and with 16-block boxes such a
seam crosses nearly every section's test rectangle. Rewritten the same
day: coplanar faces of touching boxes are merged into maximal rectangles
before rasterising, depth is the plane's exact per-pixel far-corner depth
(so a merged hillside-sized face is bounded as tightly as a block), boxes
go in 32-block distance bands with hidden boxes dropped, an 8x8 tile
min/max summary short-cuts the tests, faces narrower than a pixel are
skipped. Offline harness (scratchpad occl/test.cpp, brute-force rays
against random terrain): 0 false occlusions in 18k tests, catches 60% of
truly hidden on-screen sections; synthetic 5,400-occluder view 1.6 ms
(in-game candidate counts are 5-10x smaller).

Second capture (`gpu-10-18-07.trace` + `change4.tracy`): still 4% culled
(61 of 1,415 per view) at 350 µs per view. Two causes. (1) Occluders came
only from the BFS's reachable list, which never contains the solid rock
between the surface and a cave — exactly what hides the cave. Now the
renderer walks the whole solid stack of every visible column
(`SectionInfo::solidLayers` across all 24 sections, runs spanning section
boundaries as one box). (2) Inner-conservative coverage per FACE leaves
every seam open, and seams between faces of different planes cannot be
closed by shared-edge bookkeeping (T-junctions, overlaps in projection).
Rewritten per BOX: the whole convex outline of a solid box is filled
(inner coverage), depth = the farthest plane depth of the front faces
touching the pixel; neighbouring boxes' outlines overlap by their shared
flank, so steps are covered. Cost control: boxes clipped vertically to
their exposed flank range plus the visible cap, merged by equal top
height along x and z (with the run bottom as the merge floor), distance
bands with hidden boxes dropped. Test side: a section failing the
one-depth rectangle test gets a per-pixel outline test with per-pixel
near depth. Offline harness: 0 false occlusions, catch rate 85% (was 60%
per-face); synthetic 1,800-box view 2.5 ms — in-game cost is what the
next capture must settle.

Third capture (`change5.tracy`; the Metal trace bundle failed to
finalise): **14% culled** (177 of 1,279 per view, occluders 918 queued /
356 rasterised) at **1.06 ms per view** mean (p50 0.3, p95 3.7 ms), 2.35
views a frame — the render thread became the bottleneck (fence wait 0.95
→ 0.16 ms, fps 233 → 199). Culling works; the cost does not. Changes for
capture 6: the real view only (portal views are ~10% of sections), the
section tests fanned out over the tick-parallel pool (the buffer is
finished before them, so they are pure reads), and the per-pixel outline
test only for sections whose rectangle is at most 400 pixels; sub-zones
Occl.Build / Occl.Resolve / Occl.Tests split the cost.

**Reverted** (user's call, 2026-09-04): even with those cuts the pass
needs the better part of a millisecond of render-thread time to win back
~0.5 ms of GPU, on a machine where the CPU and GPU are already within a
millisecond of each other. Code removed entirely (SoftwareOcclusion,
solidLayers plumbing, renderer hook); the dump + scoring tools stay.

### Step 4 — two-sided plant quads (2026-09-04, unmeasured)

Cross plants (kelp, cave vines, short grass, flowers) and seagrass planes
are zero-thickness elements drawn as two opposite one-sided faces of the
same rectangle, the second's texture the first's mirrored in u — 9-12% of
built vertices in the census. The mesher now captures both faces, and
when they match bit for bit (corners, colours, sprite, mirrored uv) emits
one quad flagged two-sided in a new index group that the cutout pass
submits with back-face culling off; the fragment shader mirrors u on the
geometric back. Faces that differ (a culled side, AO that differs per
side) still go out as two quads. Mirror portals: `SetCullInvert` now
flips the front-face rule instead of swapping the cull mode, so
`gl_FrontFacing` keeps meaning the geometric front there. Expected:
about half of those plants' vertices, i.e. 4-6% of drawn vertices. To
verify: capture (drawn vertices per section vs 913 after step 1 on a
plains/ocean route) and `OBEY_NO_TWO_SIDED=1` A/B.

First capture (`gpu-12-09-43.trace` + `change6.tracy`, cool): drawn
vertices per section **913 → 840** (−8%), GPU vertex 2.05 ms at 3,107
sections/frame (0.48 µs per section, from 0.60) — the vertex win as
predicted. But the first design drew the two-sided quads in a separate
group with culling off: a pipeline switch per pass and 28% more sub-draws
(Draws/Merged 1,213 → 1,550 per pass) put vkQueueSubmit at 2.13 ms a
frame (from 1.39) and fps at 216. Redesigned the same day: the four
vertices are indexed with BOTH windings (12 indices, filed under Any),
so back-face culling keeps whichever side faces the camera with no state
change and no extra submit, and the fragment shader picks the mirrored
side from the quad's stored front normal against the camera position
(exact for axis-aligned and 45-degree planes, the only ones admitted;
also correct in mirrored portal views). Index fetch is unchanged versus
two quads, vertex fetch halves. Note: `Geom/Vertices` counts 4 vertices
per 6 indices, so it reports a two-sided quad as 8 — the plot understates
this step. To verify: capture 7.

Capture 7 (`gpu-12-22-21.trace` + `change7.tracy`, cool): the redesign
holds. Per-section GPU vertex time **0.43 µs** (0.60 at step 2, 0.48
with the cull-off group), GPU vertex 1.79 ms at 3,425 sections/frame (a
heavier route: +11% sections over change6, +11% over step 2), fragment
1.64 ms, GPU-side fps 262 (step 2: 250). vkQueueSubmit back to 0.52 µs
per section (change6: 0.69, step 2: 0.45) and the GPU fence wait back to
1.5 ms — the render thread has slack again. Vertex ms per 1,000 sections
drawn, the route-independent number: baseline 0.88 → step 1 0.67 → step
2 0.64 → change7 0.52. Tracy fps mean 214 is not comparable (the route
drew a quarter more sections than step 2's 259-fps run).

Then extended (unmeasured): the two-sided pair may have a SUB-RECT uv
(flower-bed petals and stems, leaf litter): the front face's uv goes in
sixteenths, and the back mapping is detected per pair — u mirrored
within the face's extent, v mirrored (top/bottom pairs), or identical
(a model that flips its own uv on the second face) — and carried in the
colour's alpha byte to the shader (`fragAux`). Leaf litter + wildflowers
were ~4% of built vertices. Not captured (user's call).

### Step 5 — single-texel face-map records + sub-draw attribution (2026-09-04, unmeasured)

The fragment stage was the one number the series worsened (1.47 → ~1.6
ms): two RGBA8 texelFetches per merged pixel. The record is now one
RGBA16 texel (same 8 bytes, same CPU layout; `kRecordsPerUnit`), one
fetch. And because the CPU is now within ~1.5x of the GPU (vkQueueSubmit
encoding ~1.85 ms + submit loop 0.4 ms of a 4.7 ms frame), the next lever
is sub-draw count: Tracy plots `Draws/Sections`, `Draws/Entries`,
`Draws/FusedNoSplit`, `Draws/Slabs` next to `Draws/Merged` attribute the
count to facing-group splits vs upload-order fragmentation. To verify:
a Tracy capture (no GPU trace needed) — fragment ms from the next GPU
trace, and the Draws/* plots decide which batching fix to build.

Attribution (untitled7, a GL run — counts only): 813 sections → 1,446
entries → 1,095 sub-draws per pass; one entry per section would fuse to
377; slab floor 16. Two thirds of the sub-draws are the facing splits.

Fixed-spot A/B (`gpu-13-28-21` groups on, thermal Fair; `gpu-13-34-04`
groups off via `--env OBEY_NO_FACE_CULL=1`, Nominal; same spot, same
build, 4,100-4,400 sections, no portals):

| | groups on | groups off |
|---|---|---|
| sub-draws per pass | 5,264 | 1,122 |
| drawn vertices per frame | 5.22 M | 6.79 M |
| GPU vertex ms (on: throttled; 3.51 corrected by the fragment ratio) | 3.81 | 3.49 |
| GPU fragment ms | 2.66 | 2.45 |
| vkQueueSubmit per frame | 1.41 ms | 0.85 ms |
| GPU-side fps | 154 | 168 |

The split saved nothing on the GPU at this spot: a sub-draw costs the
GPU ~0.2 µs, so 4,100 extra sub-draws ate the 23% vertex saving, and the
CPU paid 0.9 ms a frame on top. Fix (step 6, unmeasured): bridge skipped
groups smaller than 900 indices instead of splitting around them
(`OBEY_SPLIT_MIN`), which keeps the split only where it removes real
work (ground level, where groups cull ~39% of vertices).
