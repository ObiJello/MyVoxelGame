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
