# Engineering notes

Dated session notes, feature walkthroughs, and profiling findings moved out of CLAUDE.md on 2026-09-07. Copied verbatim; trim as you like.

## Feature flows

**Player color flow** (added 2026-05): launcher persists slug to `launcher.json` `"player_color"` → passes `--color <slug>` on launch → `PlatformMain` parses via `Game::ParsePlayerColorName` (`src/common/entity/PlayerColors.{hpp,cpp}`) → stored on `ClientPlayer.color` → forwarded to network as a uint8_t via `NetworkClient::SetPlayerColor` → tail-appended byte after username in the `LoginStart` packet → server's `LoginPacketListener::onLoginStart` calls `m_connection.SetPlayerColor(packet.colorId)` → IntegratedServer's `OnPlayerJoined` reads `connection->GetPlayerColor()` onto the new `ServerPlayer.colorId` → broadcast in both `PlayerInfoS2C ADD` paths (existing-players-to-new-client and new-player-to-all) → client writes onto `RemotePlayer.color` → `PlayerRenderer` looks up the RGB via `Game::LookupPlayerColor` and passes it to `BuildStickFigure`. Same for the local inventory preview via `PlayerInventoryPreview`. Default (id=0) is the historical neon green so old clients/servers stay compatible.

To add a new color, append one row to `Game::kPlayerColorTable` in `src/common/entity/PlayerColors.hpp` and bump `PlayerColorId::Count`. The launcher swatch grid auto-includes it.

**Friend hosting without port-forwarding** (added 2026-07): when a player hosts, the game tries to open its port automatically via UPnP IGD (`src/client/network/UPnPPortMapper` — hand-rolled SSDP+SOAP on Asio, no new dependency; runs on a worker thread, failure is non-fatal) and reports the WAN address in its presence. The service then TCP-probes that address to *verify* reachability (`probe_reachable`; a host claiming success isn't enough — double-NAT/ISP filtering). `join_info` returns `mode:"direct"` with the host address when the probe passed, otherwise `mode:"relay"` with a ticket: the service pushes `relay_open` to the host, whose FriendsClient dials OUT to the service and hands the resulting socket to `NetworkServer::AdoptConnection` (native-handle transfer between io_contexts); the joiner sets `NetworkClient::SetConnectPreamble` with a `relay_attach` line and connects to the service, which splices the two streams. Relay traffic rides the SAME port as the control protocol (first-line sniff), so only 25570 needs forwarding on the service machine. `--allow-private-hosts` treats private addresses as directly joinable (LAN-only setups and local testing).

**Friends system flow** (added 2026-07): self-hosted backend `tools/friends_server/friends_service.py` (Python 3 stdlib, port 25570, sqlite; run it next to the game server and port-forward TCP 25570). One port, two protocols sniffed by first byte: HTTP POST `/api` (launcher: signup/login/logout/check_name/rename via `src/launcher/net/FriendsServiceClient`) and persistent NDJSON (game: `src/client/network/FriendsClient` — own io thread, app lifetime, hello(token) → server pushes `roster`/`invite` events; friend-op targets travel in `"friend"`, `"id"` is the correlation field). Launcher login stores `session_token`/`account_id`/`account_name` in `launcher.json`, forces the username to the account name, shows a debounced availability checkmark (rename commits via the service; friendships key on account id so renames propagate). Launch passes `--session`/`--account-id` → PlatformMain constructs `Client::g_friendsClient` (null = guest). Presence transitions live in PlatformMain only: Menu (title) / Hosting(worldName, 25565) / Playing(addr). Friends UI: `screens/FriendsScreen` (title screen's old Realms slot + pause menu). Join posts a `Multiplayer` TitleAction; from in-game the pause branch stashes it in `pendingSessionAction` and the outer session loop auto-joins without showing the title. Transport is plaintext by design (personal scale); the hosting machine's launcher should set `friends_service` to `127.0.0.1` (NAT hairpin).

**Structure mobs** (added 2026-09-22). Four mechanisms, MC's own:

- *Worldgen entities.* The terrain library keeps each placed entity on the ProtoChunk as its saved compound (`IChunk::addEntity`, `levelgen/structure/StructureEntities`): template entities (`TemplateEngine::placeEntities` — StructureTemplate.placeEntities: clipped to the decorating chunk, Pos/Rotation re-based through mirror/rotation/pivot, UUID dropped; `TemplatePlaceSettings::finalizeEntities` set by jigsaw pool pieces, `ignoreEntities` by mansion and end city pieces) and piece-code mobs (swamp hut witch + cat, monument elders, ocean-ruin drowned, mansion evoker/vindicator/allays, end-city shulkers). `MyTerrainGenerator::ConvertLibChunk` carries them as binary NBT on `Chunk::worldgenEntities`; `LevelEntityStore::RequestLoad` (asynchronous, as MC's `requestChunkLoad`: the entities/*.mca read runs on the entity I/O worker and `ProcessPendingLoads` adopts it next tick; a Pending chunk can be neither saved nor unloaded) → `AddWorldgenEntities` builds each with `MakeMobForLoad` + `ApplyMobNbt`, runs `FinalizeSpawn(SpawnReason::Structure)` when owed (MC Mobs only — not the armor stand), and adds it after the entities/*.mca read, so it saves with the chunk. `ChunkProvider::TakeWorldgenEntities` hands a list out once per chunk per session (a chunk evicted before it was claimed parks its list); the list is never saved, so a chunk read back from disk never spawns its mobs again.
- *spawn_overrides.* After decoration the library records, per chunk, every start of a structure with `spawn_overrides` that references it (`StructureGeneration::recordSpawnOverrideAreas`: start box, piece boxes over the column with template id, rotation and piece type) → `Chunk::structureSpawnAreas`, saved as the Anvil extension key `ObeyStructureSpawns`. `StructureSpawnOverrides::MobsAt` (hooked into `NaturalSpawner` as `SpawnContext::structureSpawnsAt`) is NaturalSpawner.mobsAt: the fortress rule (MONSTER over nether bricks inside a fortress start → FORTRESS_ENEMIES), then ChunkGenerator.getMobsAt (the first override covering pos, `piece` or `full` box, replaces the biome list; empty = nothing spawns). Two additions: the Twilight Forest's `controlled_spawns` (EntityEvents.gatherPotentialSpawns; hollow hills and the lich tower, by piece spawn index) and the engine's `obeycraft:districts` (template-local boxes of named pieces with their own list — Aurelith's Archive).
- *Monster spawners.* `SpawnerBlockEntity` (common) is BaseSpawner: SpawnData/SpawnPotentials (entity compound kept as binary NBT), delay, limits, server tick, client spin; the server half (build the mob from its compound, the spawn egg's id rewrite, the type's spawn predicate) is installed through `SpawnerServerHooks` (`anvil/SpawnerNbt`, which is also BaseSpawner.load/save). Generated spawners reach it through `AttachGeneratedBlockEntities` (canonical text → binary NBT → `ReadSpawner`). Client: `SpawnerRenderer` draws the cage mob through `MobRenderer::RenderMorphs` (pose tilt), `ParticleKind::Flame` for the fire; the spawn burst (MC LevelEvent 2004) rides block event 200 (no level-event packet). The spawn egg reprograms a spawner (SpawnEggItem's first branch; `spawner_blocks_work`).
- *Structure-designed mobs.* Our structures carry template entities and spawners written by their generators (`gen_hush_structures.py`: `Piece.entity` / `Piece.spawner`; `gen_aurelith.py`: `place_inhabitants`, `aurelith_spawn_overrides`); see docs/the-hush.md.

## Reproducible profiling runs: pose recording + replay (2026-09-04)

`src/client/dev/SessionReplay.{hpp,cpp}` — record a play session's camera path
once, replay it under any build/backend/setting and compare the per-second
`[Harness]` fps lines (and Tracy captures / GPU traces) second by second.

```bash
# 1. record (play normally; recording starts when the level has loaded, ends on quit)
tools/play.sh tracy --vulkan --world "OG with Structs" --record tour1
# 2. replay it for profiling — quits by itself when the recording ends
tools/play.sh tracy --vulkan --world "OG with Structs" --replay tour1
tools/play.sh tracy --vulkan --gpu-trace=40 --world "OG with Structs" --replay tour1
```

- Replay is **time-driven**: the pose is interpolated between the two recorded
  samples around the replay clock and written IN PLACE of the player's
  physics step, so frame rate does not change the path. Mouse-look is masked;
  WASD/jump do nothing to the player; block interaction is not replayed.
  Chunk loading, mesh scheduling, culling, the move packets and the server all
  see an ordinary moving player.
- The recording's first pose is snapped to, then held `--replay-hold` seconds
  (default 8) before t=0, so the chunk-streaming state at the start is the
  same every run. The replay must start in the dimension the recording
  starts in (it logs and aborts otherwise).
- **Portal crossings reproduce.** A sample taken on a crossing frame is
  flagged `c`; replay does not interpolate across it — it extrapolates the
  pre-crossing motion until the real traveler (`ImmersivePortalTraveler`,
  or the gun's prediction) commits a crossing, then snaps onto the recorded
  far-side path. Server teleports (`/tp`, respawn) are flagged `t` and
  snapped when the dimension matches; a cross-dimension `/tp` cannot be
  reproduced and ends the replay.
- File format is plain text (`recordings/<name>.rec`: header lines, then
  `t dim x y z yaw pitch scale speed flags arrival` per frame) — trim or
  splice with a script. Sampling is every frame; 60 s at 200 fps ≈ 1 MB.
- Log tags: `[Record]` (start, every 5 s, crossings/teleports, saved),
  `[Replay]` (loaded, snapped, playing, per second alongside `[Harness]`,
  crossing reproduced / warnings, finished). Tracy plot `Replay/Time`
  (negative during the hold) is what to range-select on.

## Main-thread pass 2026-09-04 (tour1 replay, Vulkan)

Baseline → after (typical open-world segment, mean per frame): CPU-side 2.33 → ~2.0 ms,
frustum filter 0.38 → 0.25, draw submit 0.18 → 0.11, 39 ms hitches gone. The GPU is
the ceiling there (3.05 ms of a 3.4 ms frame, vertex-bound at 0.53 µs per 1k
vertices; `gpu_report.py --tracy` regression), so CPU work only shows in the ~30% of
frames that do not wait on the fence. What changed and the traps:

- `ChunkRenderer::SubmitMergedRuns` merge-loss validator is OFF unless
  `OBEY_MERGE_VALIDATE=1` and is O(n) now. It was entries × runs every 128th
  call: 26 ms at a 23k-entry look-down = one 39 ms frame every 64.
- Draw entries are radix-sorted (`RadixSortDrawEntries`, 32-bit slab<<24|offset
  key). Do NOT drop the sort to save time: it fuses ~12% of sub-draws (a column's
  24 sections are uploaded together), worth 0.1 ms of vkQueueSubmit.
- Frustum filter: a column the frustum edge crosses gets ONE
  `Frustum::SectionRowRange` (closed-form row interval from the six planes)
  instead of 24 box tests. Visible-section identity lives in
  `ChunkRenderer::SectionGrid` (24-bit mask per column around the camera chunk,
  overflow hash set for anything outside) — `IsSectionVisible`,
  `IsMainViewSection/Column`, `IsPortalViewSection/Column` are array reads.
- Mesh scheduler: `ClientChunk::neighborsAllLoaded` caches HasAllNeighborChunks
  (reset for the 8 neighbours in `TransitionChunkState`), and `ClientChunk::dirtyMask`
  mirrors `dirtySections` (use `AddDirty`/`RemoveDirty`, never touch the set
  directly) so the per-frame walk over ~1,100 dirty columns reads one word each.
- Tracy zones added for the next round: `FrustumFilter.Keys`, `MergeRuns.Sort/Flush`,
  `OrderedRuns`, `MeshSchedule.DirtyWalk/Snapshots`, plot `MeshSchedule/DirtyChunks`.
- Remaining CPU-side order (open world): vkQueueSubmit ~0.7 ms (MoltenVK per
  sub-draw, ~0.15 µs each — only fewer draws help), ImmersivePortalRender ~0.35,
  FrustumFilter ~0.25 (24k reachable sections × 3 views), MeshSchedule ~0.2,
  BuildDrawList ~0.13. The lever for BOTH CPU and GPU is drawing fewer sections:
  a third of drawn sections are underground (occlusion culling, see below).

### OpenGL is CPU-bound in the driver (2026-09-04, tour1)

Same replay on GL: open world 6.1 ms/frame (163 fps) vs 3.0 on Vulkan, sky
look-down 29 ms. Apple's GL costs ~0.9 µs of CPU per sub-draw inside
`glMultiDrawElementsBaseVertex` (23 ms of the 29 ms sky frame) and, worse,
serialises every `glBufferSubData` against pending draws of that buffer.
What was done and what to keep in mind:

- **Gap bridging is ON by default on GL** (`ChunkRenderer` init: 8192 indices;
  `OBEY_GAP_BRIDGE=<n>` overrides on either backend, 0 = off). Draws dead space
  between two visible runs of a slab instead of splitting: 3.6× fewer sub-draws
  (3,639 → 1,009 open world, 24.5k → 5.4k sky) for ~15% more vertex work.
  Off on Vulkan, which is GPU-bound. 32k bridged even fewer draws but lost in
  the sky (more vertices than the GPU had headroom for) — 8k is the measured
  optimum.
- **Where GL blocks is not where GL is slow.** Under GPU back-pressure Apple's
  GL driver blocks in whatever call comes next — `glBufferSubData`, the
  multi-draw, or the swap — so a Tracy zone that looks like a CPU hot spot
  (`SubmitMultiDraw` 23 ms in the sky) may be a GPU wait. Read the GL frame
  total, compare runs back to back at the same temperature (a hot Air inflated
  every CPU zone 1.5×), and treat `Present` as the honest GPU wait.
  `OBEY_SYNC_UPLOADS=1` restores the synchronised uploads for A/B.
- **Uploads are unsynchronised on both backends** (`UpdateBufferUnsynchronized`:
  slab VBO/IBO/origins UBO in `ChunkMegaBuffer::TryUploadToSlab`, the retire
  zero-fill, `PlayerRenderer::SubmitFigures`). The delayed-reuse rule
  (`kFreeDelayFrames`) already guaranteed no in-flight frame reads a fresh
  range; the synchronised GL path did not know that and stalled. Back-to-back
  A/B on tour1 (`OBEY_SYNC_UPLOADS=1` vs default, cool machine): open world
  5.16 → 4.75 ms/frame, p99 24.5 → 13.5 ms, player-body draw 0.5 ms/call → 0,
  MeshUpload 0.57 → 0.07 ms. The one new
  hazard is a bridged gap read by an in-flight frame across space just
  allocated: `m_recentIndexAllocs` keeps such ranges hot for
  `IsIndexGapDrawable` for kFreeDelayFrames. Any new per-frame uploader must
  either ring-buffer by frame parity or accept the GL stall.
- **Bridging never crosses a fenced section** (`ChunkMegaBuffer::
  SetSectionNoBridge`, `IsIndexGapDrawable` refuses): a PARKED one — the
  retention cache keeps an unloaded chunk's mesh allocated for instant
  revisits, and bridging it drew terrain of chunks the player had left, stray
  sections that came and went with the view angle (2026-09-24, ~500 sections
  a frame at RD 32 once streaming order scattered slab neighbours) — or one
  OUTSIDE THE RENDERED VIEW: the halo ring the server sends (MC buffer 2) but
  the client never renders (buffer 1), which bridging drew as a thin ring of
  just-left chunks. `ClientMeshManager::Park/UnparkChunkGPUData` and
  `ChunkRenderer::UpdateOutsideViewFence` set them. `OBEY_STRAY_AUDIT=1` logs
  once a second what the bridged gaps held (`[StrayAudit]`: parked / not
  loaded / outside view / loaded-not-visible): the first three must stay 0.
  Free cam (F+C) never bridges, so a difference between it and the player
  view is exactly what bridging adds.
- Translucent (`SubmitOrderedRuns`) cannot bridge (blend order), so it stays
  one sub-draw per section: ~0.55 ms per pass on GL.
- `Present` on GL (~1.9 ms) is the swap = GPU wait; Metal System Trace works on
  GL too (Apple GL runs on Metal) for the GPU side.

## Terrain vertex format (packed, 2026-09-04)

Chunk terrain uses the 20-byte `Render::TerrainVertex` (`src/client/renderer/
core/Vertex.hpp`), not the 24-byte `Vertex` every other renderer uses. Why:
the Metal System Trace showed the frame vertex-FETCH-bound (0.57 ns per
32-byte vertex, ~6.5 M vertices a frame), so the vertex was halved to 16
bytes; the light engine (2026-09-22) grew it by one word — the MC light
coords, see Lighting below.

- Positions are **section-relative** fixed point (1/2048 block, -2..30);
  the world origin comes from a per-slab **section-origin table**, a 16 KB
  uniform block (`SectionOrigins`, 1024 rows) that `ChunkMegaBuffer`
  maintains and `BindSlab` binds through `RenderBackend::BindUniformBuffer`
  (GL block binding 0 / Vulkan set 3). The mega buffer patches each
  section's row into `TerrainVertex::slot` at upload — the mesher writes 0.
  A slab holds at most 1024 sections; the pool moves to the next slab.
- Greedy-merged quads are **tiled** (bit 15 of `slot`): uv is in tile
  space and the fragment shaders `texelFetch` the sprite's atlas rect from
  `AtlasBuilder::GetSpriteTableHandle()` (RGBA32F, 256 per row, bound at
  texture slot 1 = Vulkan set 2 by `ChunkRenderer::BindSpriteTable`).
  Fluid plates carry the sprite id in `v`. Block rectangles are also
  **face-mapped** (bit 14): every block they cover has an 8-byte record —
  one RGBA16 texel: tint*shade (3 bytes), the four 2-bit AO corner codes
  (1 byte), the sprite id (16 bits); written as two uint32 words
  (`TerrainVertex::FaceMapTexel0/1`) — in the layer's **face map**, which
  `ChunkMegaBuffer` stores right behind the section's vertices in the
  same slab region and exposes as an RGBA16 buffer texture over the slab
  VBO (`RenderBackend::CreateBufferTexture`; GL `samplerBuffer` on unit 2,
  Vulkan set 4 = uniform texel buffer; bound by `BindSlab`). One
  texelFetch per pixel (the first version used two RGBA8 texels). The vertex holds the rectangle's tile origin/size (u, v)
  and the record index (colour bytes, patched by the mega buffer at upload
  like `slot`); the fragment shader finds the block from floor(uv) and
  rebuilds its AO gradient with the GPU's own two-triangle interpolation.
  So `Mesher::FlushGreedyQuads` merges across AO patterns and compares
  only sprite + tint*shade (`Greedy::QuadKey` must match the `matches`
  lambda). Coplanar faces in the two layers (a grass side under its
  overlay) are linked so both layers cut identical rectangles —
  `PendingQuad::partnerKey`/`solo`, grid cell -1 = blocked — because
  mismatched tessellations of coplanar quads z-fight. Rows are 10 bits
  (1024 per slab).
- All attributes are normalized unorm16/unorm8; shaders recover the
  integers with `value * 65535` — no integer attribute path on either backend.
- Anything reading terrain vertex bytes on the CPU must decode
  (`TerrainVertex::DecodePos` + the section origin): the worker's translucent
  centroid pass does; the F8 cull dump prints section-relative values.
- Changing the layout: `Vertex.hpp`, `GetTerrainVertexLayout`,
  `ChunkMegaBuffer::VERTEX_STRIDE`, `GLBackend::SetupBlockVertexFormat` +
  `BindVertexBuffer` (both paths), `ClientWorkerPool::CopyVertexLayer`, the
  `terrain*.vert/frag` shaders and their `_vk` twins, then recompile the
  `.spv` with glslc AND commit them (CMake only rebuilds them when it finds
  glslc; the checked-in files are what ships). The face-map record layout
  is `TerrainVertex::FaceMapTexel0/1` + `fetchFaceRecord()` in the three
  fragment shaders; `Region::allocUnits` (vertices + records) is what the
  mega buffer frees, `vertexCount` stays the real vertex count.
- **Light word** (bytes 16..19, attribute location 3, 4 x unorm8): byte 0 =
  MC block light coord, byte 1 = sky light coord (0..240 each — level x 16,
  smooth lighting's fractions in between), bytes 2..3 zero
  (`TerrainVertex::LightWord`). The vertex shader samples the lightmap with
  it (`sampleLightmap`, MC's `sample_lightmap`) and multiplies the vertex
  colour, or hands it to the fragment shader as `fragLight` for face-mapped
  rectangles. A glowing block is simply `kFullBrightLight` (0xF0F0 = MC
  FULL_BRIGHT, for `emissiveRendering` states) — the old alpha-0xFE /
  record-bit / v-bit-15 emissive markers are gone. The mega buffer keeps the
  face-map records 8-byte aligned behind the 20-byte vertex run (one spare
  unit per region).

## Lighting (2026-09-22)

MC's light engine (26.3 `LightEngine` / `BlockLightEngine` /
`SkyLightEngine` / `LevelLightEngine`), ported queue-for-queue, plus MC's
lightmap and per-vertex smooth lighting. Code: `src/common/world/lighting/`
(engine), `src/client/renderer/environment/Lightmap.*` (lightmap),
`Mesher::ComputeFaceLight` (baking).

**Storage.** Every `Game::Chunk` owns `light` (`ChunkLight`): 26 sky + 26
block `DataLayer`s (sections -5..20 — one below and one above the world,
as MC), `ChunkSkyLightSources` (MC's lowest-source heightmap) and
`lightCorrect`. `DataLayer` is MC's 2048-byte nibble array with a
homogeneous default and copy-on-write sharing (a chunk clone costs no light
copy). Light index `li` is block section `li - 1`.

**Per-state properties.** `BlockLightProperties` precomputes, per block
state: emission (MC `lightLevel` rules — lit/candles/sea pickle/respawn
anchor/light block/…), dampening (`getLightBlock`), `canOcclude`,
`solidRender`, `propagatesSkylightDown`, `useShapeForLightOcclusion`,
`emissiveRendering`, and a deduplicated 16x16 face-occlusion mask per
direction for `shapeOccludes`. Vanilla rows come from
`tools/gen_block_light.py` → `src/common/world/block/GeneratedBlockLight.inc`
(parsed from `Blocks.java`); engine and mod blocks are one line each in
`src/common/world/block/EngineBlockLight.inc` —
`ENGINE_BLOCK_LIGHT("slug", level)`, which wins over the generated table.
**A new glowing block = one row there.**

**Threading.**
- Initial light: `ChunkProvider::CompleteChunkLoad` (worker) runs
  `Lighting::LightChunk` on the chunk alone (a thread_local single-chunk
  engine) unless it was loaded `lightCorrect` from disk.
- Live light: one `LevelLightManager` per `World`, **server thread only**
  (block writes happen there). `World::SetBlock` → `OnBlockChanged` (only
  when `HasDifferentLightProperties`) queues a check; the tick's
  `FlushLightUpdates(level)` runs `RunUpdates` (all queued checks,
  propagation to a fixed point, holding the touched chunks' content locks)
  and sends `LightUpdateS2C` for the affected sections
  (`AroundAndAtBlockPos`, MC `SectionPos.aroundAndAtBlockPos`) BEFORE the
  block-delta flush, so the client relights with the block change.
- Chunk arrival: `ProcessAsyncChunkResults` registers the new chunks
  (border reconciliation: each face seeds increases from both sides —
  chunk-local light is a lower bound, so increase-only converges to MC's
  fixed point), runs one `RunUpdates`, drops the new chunks' own affected
  sections (their chunk packet carries the light) and marks changed
  neighbours for save. Eviction removes the chunk from the registry via the
  provider's eviction hook.

**Save/load.** Anvil sections carry `BlockLight`/`SkyLight` (omitted when
empty) for light indices 0..25 plus `isLightOn = 1b`; a chunk without them
(or `isLightOn` false) is relit on load. Missing sky layers above the data
are derived (15 above the top, else a copy of the layer above), MC-style.

**Network.** `ChunkDataS2C` gains a trailing light block; `LightUpdateS2C`
(0x6A) carries the same block for the changed sections. Block format
(`LightNetCodec.hpp`): u8 version (1), u32 sky mask, u32 block mask, then
per set bit a u8 tag — 0..15 = homogeneous layer of that value, 16 = 2048
raw bytes follow. Decoded on the network thread; the client swaps the
layers in and marks the masked non-air sections dirty.

**Client baking.** `RenderRegionCache` copies light for sections -1..24 into
the mesh snapshot. `Mesher::ComputeFaceLight` is MC's `BlockModelLighter`:
smooth = `faceCubic`/`facePartial` with `AdjacencyInfo` corners and 26.3's
`smoothBlend` (sky borrowing, corner substitution with the centre light),
per-vertex weights by the vertex position within the face; flat = light at
the cullface neighbour (or the block itself for non-cubic faces).
Smooth lighting off, a light-emitting state (emission > 0) or an
`emissiveRendering` state → flat, no AO; `emissiveRendering` → FULL_BRIGHT.
Fluids: max of the cell and the cell above (bottom: the cell below). Greedy
merging needs a uniform light across the face; mismatched light breaks a
merge (and a two-sided pair).

**Lightmap.** `Render::Lightmap` is a CPU port of `core/lightmap.fsh` +
`LightmapRenderStateExtractor` into a 16x16 RGBA8 texture (x = block, y =
sky; LINEAR, clamped), recomputed per frame from `EnvironmentFrame`'s
`skyLightFactor` (DAY timeline 1 → 0.24 at night; lightning 1),
`skyLightColor` (white → #7a7aff), `ambientLightColor` (#0a0a0a overworld,
#302821 nether, #3f473f end), `blockLightTint` #ffd88c, the block flicker
(random walk per tick), night vision (#999999 floor), Darkness (scale +
blend) and the Brightness option (gamma, MC default 0.5). Hush keeps its
fixed-night mood (factor 0.42); TF/Aether take their own sky light. Bound on
texture slot 3 (GL unit 3, Vulkan set 5 on the portal layout — the texture
descriptor layout is VERTEX|FRAGMENT). A portal's far-side view gets its
own dimension's lightmap in a second texture (`TextureFor(frame)`).

**Entities & co.** One packed light per draw, MC `getPackedLightCoords`
(block/sky at the eye cell; fire or the blaze-style renderers → block 15),
turned into an RGB lightmap colour on the CPU (`EntityEnvironment::
LightColor`, frame-aware). Mobs: per-mob, stamped on their batches
(`MobBatchScope`). Stick figures, particles, XP orbs (+7 block light, MC)
and falling blocks/TNT (per-instance RGBA8 attribute, location 4) bake it
into vertex colour since they share one draw. Block entities: their cell's
light (`LevelLightCoordsAt`, emission-raised — a lit campfire's food is at
15); glowing sign text is FULL_BRIGHT. Dropped items, the held item (light
at the camera eye) and the portal gun use it per draw. Shader uniforms:
`uEntityLight` / `uDrawLight` (vec3; Vulkan push constants `uScalars.xyz`),
`uBlockEntityLight` (vec3; Vulkan `uScreenSize.xy` + `uLineWidth`).

**Kill switches.** `OBEY_LIGHT=0` — lightmap off: the texels become the old
uniform night dim (terrain and entities look as before the engine; the
engine still runs). `OBEY_LIGHT_ENGINE=0` — the level engine off: chunks
still get their chunk-local initial light on the workers, but borders are
not reconciled and block changes do not relight.

**Profiling.** Tracy zones `Light.RunUpdates` (plot `Light/Entries`),
`Light.InitialChunk` (per chunk, on the gen workers), `ApplyLightUpdate`,
`Lightmap.Update`; `LevelLightManager::Stats` for totals.

**Known gaps.** No client-side light prediction (a placed torch lights one
server round trip later, as with a remote server in MC); the 26.3 AO
shading multipliers come from the existing AO path, not a re-port of
`BlockModelLighter`'s AO; boss-bar darkening is 0; the local player's
fire does not light the hand; shader-pack entity passes still get constant
lmcoords; the far-portal lightmap is one frame late.

## Sprite atlases (2026-09-25)

MC 26's split, packed by MC's own stitcher (`texture/Stitcher.{hpp,cpp}`, a
line-for-line port of `Stitcher` + `SpriteLoader.stitch`):

- **blocks** (`assets/atlases/blocks.json`: block/, conduit, bell, pot,
  enchanting book) — mipmapped, padding 16, ~4096x2048 with the CTM variants.
  `g_atlasBuilder`. Terrain, block models, block items, fluids, break overlay.
- **items** (`items.json`: item/, minus the 1254 px portal gun icon via MC's
  `filter` source) — never mipmapped (MC), padding 1, ~1024x512.
  `g_itemAtlasBuilder`.
- `Render::FindSprite(key)` resolves like MC `MaterialBaker.bake`: items first,
  then blocks, and returns the atlas the draw must bind (`GetAtlasTexture`).
  Block models reach the item atlas only through `particle` (barrier,
  light_NN, structure_void): the block-marker particle and the GUI's flat
  fallback use `FindSprite`.
- Item rendering does NOT read the item atlas: icons, held, dropped and framed
  items draw each item's own PNG (HeldItemSpriteMesh, GuiGraphics
  LoadItemTexture) — an engine difference from MC, not a bug of the split.
- The stitcher rounds every slot to the mip grid, so no sprite can be
  misaligned and lose its mip chain (the old packer lost 1,058 behind the
  portal gun). A sprite whose size cannot halve to level 4 lowers the whole
  atlas's mip level, with MC's warning — that is MC's rule too.
- The block atlas is stitched for level 4 whatever the option; the Mipmap
  Levels option rebuilds the chain in place (MC reloads resources instead).
  MC's anisotropy widening of the padding is left out for the same reason.

## GPU writes into sampled textures (2026-09-25)

Apple's GL driver makes the CPU wait for the GPU when a texture that queued
frames still sample is written with `glTexSubImage2D` — whichever write comes
first in a frame eats the whole wait, and CPU/GPU overlap is lost. A replay
A/B (GL 96 fps vs Vulkan 128) put it at `AnimFrameUpload` (median 12 us,
18-25 ms outliers on 1,389 of 1,954 animating frames) and `Lightmap.Update`.

- **Animated atlas sprites** follow MC's `AnimationState` exactly (per-tick
  frames, `"interpolate": true` blending, the padding ring), but the pixels
  are made on the CPU and only the changed rectangles written. MC draws them
  into the atlas on the GPU; built and measured here, that cost +0.7 ms per
  tick frame on Vulkan (and a freeze while generating) and +5 ms on GL, atlas
  size made no difference, and was removed. `OBEY_SPRITE_ANIM`: `cpu`
  (UpdateTexture2DLevel — Vulkan stages it, the default there) or `pbo` (GL
  default: through a rotating pixel-unpack buffer, +1.8 ms per tick frame vs
  +8.6 ms direct; a three-copy atlas ring measured +2.7 ms and was removed).
  Pixel building is integer (the shader's mix, rounded) with memcpy'd padding:
  0.14 ms per tick on Vulkan. GL's remainder is ~250 glTexSubImage2D calls
  per tick inside Apple's driver; batching the staging-buffer mapping did not
  move it.
- **Lightmap** (GL only): uploads rotate through four textures, so the one
  written was last drawn with four uploads ago. Vulkan writes in place and
  the backend's per-frame copies keep it off the in-flight frame (see
  "Frame overlap on MoltenVK").
- Rule for new code: never CPU-write a texture or buffer the previous frames
  still read on GL; draw into it on the GPU, rotate, or map unsynchronized
  (`UpdateBufferUnsynchronized`).
- **Stream buffers on GL** (2026-09-25): a plain `glBufferSubData` into a
  buffer with draws still queued makes Apple's driver wait for the GPU, and
  so does orphaning (`glBufferData(nullptr)` + write — measured, no gain).
  What works is an UNSYNCHRONIZED map guarded by a fence: GLBackend keeps one
  `GLsync` per frame (placed in `EndFrame`, 8 deep) and
  `RenderBackend::UpdateBufferStreaming` waits for the fence of the frame that
  last wrote the buffer (normally long signalled — `GL.StreamFenceWait` never
  fired) before writing unsynchronized. For RINGS of per-use buffers only (the
  particle slots): a single buffer rewritten every frame would wait for the
  previous frame. MobParticles.Upload went from 7-15 ms stalls in 4-6% of GL
  frames to none; `OBEY_GL_SYNC_STREAM=1` restores the old write. Vulkan uses
  the plain write (its streaming buffers never wait).

## Frame overlap on MoltenVK (2026-09-25)

A Metal System Trace with hardware counters (the Performance Limiters
template, `gpu-limiters.tracetemplate` today) showed every Vulkan frame
as one Metal command buffer — ~5 ms vertex, then ~2.5 ms fragment — and the
next frame's vertex work never starting before the previous frame's fragment
work ended: **0.0% vertex/fragment overlap**, against 19-33% on GL. On a tile
GPU the geometry of frame N+1 can run while frame N shades pixels; during the
vertex phase the shader cores sat nearly idle (VS occupancy ~4%, ALU ~3%,
~45 GB/s), so the overlap is almost free. Five things each forbade it on its
own — removing four of them left 0.0%, removing all five gave 51%:

1. **MoltenVK argument buffers.** With them (and a Metal residency set,
   macOS 15+) MoltenVK orders consecutive command buffers with per-stage
   fences: `MVKCommandEncodingContext::syncFences` publishes every stage's
   fence at the end of a command buffer and the next render encoder waits on
   the fragment fence before its vertex stage (MVKCommandBuffer.mm, 1.4.1).
   Off → resources bound directly, Metal's own hazard tracking orders what
   actually conflicts. Costs ~0.3 ms CPU per frame in `vkQueueSubmit`
   (`bindMetalResources`, per-draw binding) — the price of the overlap.
2. **MTLEvent semaphores.** SINGLE_QUEUE style instead: one VkQueue does
   graphics, uploads and present, so in-queue order is exact.
   Both are passed per instance through `VK_EXT_layer_settings`
   (`CreateInstance`) — MoltenVK reads its environment once, on the first
   Vulkan call (GLFW's), so a `setenv` in the backend is too late. An
   explicit `MVK_CONFIG_*` environment variable still wins, which is the A/B
   switch: `--env MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1 --env
   MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=1` restores the serialised frames.
3. **One depth image.** Now one per frame slot (`m_depthImages`), framebuffers
   per (slot, swapchain image) — `FrameFramebuffer()`. +28 MB at 3420×2146.
4. **Writing a texture the in-flight frame samples** (the lightmap and the
   animated atlas each blocked it alone). A texture that takes a queued
   update after it has been drawn with gets **per-frame copies**
   (`VKTextureInfo::frameCopies`, one per frame slot; +43 MB for the block
   atlas). Frame slot s samples and writes only copy s; the updates of the
   frames it missed are replayed into it from the staging ring, which is why
   that ring is 2 × MAX_FRAMES_IN_FLIGHT slots (see `kTexStagingSlots`). The
   write barriers wait only on earlier transfers, never on shader reads.
5. **The timestamp query pool,** reset every frame even with every GPU timer
   off. Now one pool per frame slot, reset only after a timer used it.

Rule: nothing one frame writes on the GPU may be an object the previous frame
still uses — attachments, textures, query pools. Metal serialises the WHOLE
next render encoder, vertex stage included, on any such write.

Measured (tour replay, full screen, M4): Metal trace 133 → 180 fps (frame
p50 7.7 → 5.6 ms, overlap 0 → 52%); Tracy 115.8 → 151.8 fps, p99 14.3 →
12.1 ms, frames > 16.7 ms 0.39% → 0.18%. New: with vsync off the game now
often renders ~3 frames per display refresh, and Metal's "Wait for Next
Drawable" (inside `vkQueueSubmit` — MoltenVK takes the drawable lazily)
occasionally waits 15-35 ms when the compositor holds all three swapchain
images (2 frames > 33 ms in the 57 s replay). That is the display, not
GPU or CPU work; `ca-client-buffer-wait-interval` does NOT show it, the
"Wait for Next Drawable" rows of `metal-application-intervals` do.

## Presentation on MoltenVK: async submits and the mailbox (2026-10-05)

MoltenVK has no `VK_PRESENT_MODE_MAILBOX_KHR` (FIFO and IMMEDIATE only), and
IMMEDIATE still ties every frame to a free `CAMetalDrawable`: the frame
renders INTO the drawable, MoltenVK fetches it (`nextDrawable`) while it
encodes the frame, and the GPU cannot start the frame before CoreAnimation
hands one back. At RD 8 that wait was ~1 ms of every frame (inside
`Vk.QueueSubmit`; only ~0.34 ms of that zone is encoding). Worse, whenever
WindowServer *composites* the window instead of scanning the drawable out
directly — any window over the fullscreen game does it, e.g. a Notification
Center banner ("Tips", "Game Mode: On") — drawables come back at exactly
2 x refresh and the game is capped at **120 fps** with the GPU half idle
(OpenGL is not affected: it presents outside CAMetalLayer).

- **Asynchronous submits** (`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` false,
  layer setting in `CreateInstance`): MoltenVK encodes and waits for the
  drawable on its queue thread; the main thread waits only in the frame-slot
  fence. Same mean fps, p95/p99 -10..20 %.
- **Mailbox** (`m_mailbox`, vsync off; `OBEY_VK_MAILBOX=0` disables):
  `BeginFrame` decides whether the frame will be shown. If a drawable is
  free right now (`MailboxDrawableFree`) it acquires and the frame draws
  straight into the swapchain image as before; otherwise it draws into its
  frame slot's stand-in image and is never presented. The stand-ins sit in
  front of the real images in `m_swapchainImages`, so every pass,
  framebuffer, blit and read-back works on either.
- **"Free right now"** cannot come from MoltenVK's acquire: it hands out an
  image whether or not CoreAnimation has a drawable, and the encode then
  blocks the queue thread — and every frame behind it. `VK_KHR_present_wait`
  (+ `present_id`, enabled when the device has them; the features need
  `VK_KHR_get_physical_device_properties2` on this 1.0 instance) tells,
  without waiting, whether the last present reached the screen. A frame is
  shown only when it has (`OBEY_VK_MAILBOX_PENDING`, default 1 — 2 blocks
  again) AND at least half a refresh has passed since the last present
  (`OBEY_VK_MAILBOX_HZ` overrides the 2 x refresh rate; presenting faster
  only queues for drawables). 50 ms without a present presents anyway, so a
  present that never reports back cannot freeze the picture.
- Measured (tour replay, RD 8, 100 %, rain, composited window): 120 fps →
  ~380 fps; presenting ~100 frames/s. With presenting switched off entirely
  the same scene ran ~400 fps — the mailbox gets within ~5 % of it. A first
  version copied a stand-in onto the drawable on present; deciding at
  `BeginFrame` removed that copy (+1.3 ms per presented frame at 100 %).

## Metal backend (2026-10-06)

`--metal` selects `MetalBackend` (`src/client/renderer/backend/metal/`,
Objective-C++ with ARC, `ENABLE_METAL`, on by default on macOS). It is the
Vulkan backend's design without the Vulkan machinery:

- **Shaders.** One source: the `_vk` GLSL → SPIR-V (glslc) → MSL
  (`tools/gen_metal_shaders.py`, spirv-cross from the Vulkan SDK, run by the
  build after `compile_shaders`). The script rewrites every (descriptor set,
  binding) to a fixed Metal index first — the table is in its docstring and in
  `metal/MetalBindings.hpp`; a binding with no row fails the generation. The
  `.metal` files are committed. With Apple's Metal toolchain
  (`xcodebuild -downloadComponent MetalToolchain`) CMake compiles each to AIR
  with `-gline-tables-only -frecord-sources` and links one
  `shaders.metallib` into the bundle — what the backend loads, and what lets
  the Metal debugger's shader profiler map GPU cost to source lines. Without
  the toolchain the game compiles the `.metal` sources at startup
  (`newLibraryWithSource`; the OS caches the result). Every shader's entry
  point is named after its file (`terrain_vk.vert` → `terrain_vk_vert`) so
  they link into one library. Each output records the SHA-256 of its SPIR-V
  plus the generator version, so the build-time run costs a hash per file.
- **Uniforms.** The `SetUniform*` name routing onto the push constants and the
  Common / Bones blocks is shared with Vulkan (`backend/SpirvUniforms`). Push
  constants go inline (`setVertexBytes`), Common / Bones into a per-slot ring
  (256-byte windows, a new window only when the data changed — Vulkan's rule).
- **Clip space.** Vulkan's viewport `(x, y, w, h)` lands like Metal's
  `(x, y + h, w, -h)` (`ToMetalViewport`): the flipped viewport every world
  draw uses on Vulkan is a plain one on Metal, so the image is upright and
  counter-clockwise stays front, as on GL. The upscale draw (a plain Vulkan
  viewport) becomes a flipped Metal one.
- **Passes.** A pass is begun as a descriptor and opened on first use
  (`EnsureEncoder`); a `Clear` before that is its load action, after it a
  full-rect quad (Metal has no in-pass clear). The frame's own pass staying
  unopened is what lets the half-res rain draw ahead of it from the previous
  frame's depth, as Vulkan's `EnsureFramePass` does.
- **Invariants kept.** Two frames in flight; per-slot depth, scene and mailbox
  stand-in images; per-frame copies of textures updated while in use (the
  same replay-from-staging-ring scheme, `kStagingSlots` = 2 x frames). Metal's
  hazard tracking would serialise frames exactly as on MoltenVK otherwise.
- **What Metal does for us.** No barriers or layouts; a command buffer retains
  what it uses, so every destroy (deferred or not) is immediate and safe;
  depth/stencil, cull, winding, fill, bias and clip are encoder state, so a
  pipeline keys on shader + blend + attachment formats only.
- **Presentation.** `CAMetalLayer` on GLFW's content view, untagged colour
  space (Vulkan's PASS_THROUGH look), `framebufferOnly = NO` (read-backs, the
  post chains' copy). Vsync off is the same mailbox as Vulkan's, natively: the
  drawable is asked for only when the frame first needs its colour, and only
  if `MailboxDrawableFree` (presented-handler count + half-refresh spacing;
  `OBEY_MTL_MAILBOX=0` off, `OBEY_MTL_MAILBOX_PENDING`); otherwise the frame
  draws into its slot's stand-in. `OBEY_SKIP=mailbox` works as on Vulkan.
- **Intel / AMD Macs.** Depth is `Depth32Float_Stencil8` everywhere (no 24-bit
  depth on Apple GPUs); CPU-written buffers are managed instead of shared on a
  discrete GPU. Nothing Apple-only (memoryless, framebuffer fetch) is used.
- **GPU timers.** Apple GPUs sample counters only at encoder boundaries, so a
  timer measures the frame's command buffer: the first one begun in a frame
  gets it, later ones that frame return `INVALID_GPU_TIMER`.
- **Pipeline warm-up.** `<obeycraft>/cache/mtl_pipeline_manifest.txt`, the
  Vulkan manifest's format plus the attachment config; `WarmPipelines`
  rebuilds it behind the loading screen.

**Apple's tools on this backend.** The full recipes, scripts and traps are
the project skill `.claude/skills/metal-profiling/` (its `references/`
hold Apple's counter definitions and the TBDR cost model); this is the
summary.
- GPU frame capture for Xcode's Metal debugger: launch with the capture layer
  on and a time, `open --env MTL_CAPTURE_ENABLED=1 <app> --args --metal ...
  --env OBEY_MTL_CAPTURE_AT=40` → `<obeycraft>/captures/metal-<time>.gputrace`
  (one frame; double-click to open), or F3+U at runtime. `MTL_CAPTURE_ENABLED`
  must be in the launch environment — Metal reads it when the framework
  loads. In Xcode, "Profile after replay" gives per-pipeline cost, 226
  counters per encoder (share → Export Encoder Counters), per-draw counters
  (~1 h, leave Xcode idle), per-line shader costs (only with a metallib built
  for the host OS: `METAL_SHADER_PROFILING=ON` in CLion's tracy CMake
  profile — an ad-hoc `cmake -D` is reset by CLion's next configure), the
  heat map and the memory/dependency views. Two rules for reading it: the
  replay profiles ONE frame (its own encoders overlapped, but with no
  neighbouring frames — live, frame N+1's vertex work runs under frame N's
  fragments), so its percentages are shares of that frame's GPU work and
  never "percent of a live frame"; and it runs at the Medium performance
  state unless the profiler session popover (clock icon, Performance view)
  is set to Maximum, so quote percentages, not milliseconds, unless you set
  Maximum.
- Render stages are debug groups (`Render::GpuDebugGroup`: opaque, cutout and
  translucent terrain, sky, players, mobs, items, block entities, particles,
  clouds, weather, the rain drawn ahead of the frame, held item, outline,
  post, HUD); encoders and pipelines carry labels. The capture shows the
  frame as those named stages with Xcode's GPU time per stage and draw;
  Vulkan's labels (VK_EXT_debug_utils, on whenever the loader has it) reach
  Metal captures through MoltenVK the same way.
- Metal Performance HUD (`tools/play.sh tracy --hud`, or `open --env
  MTL_HUD_ENABLED=1 --env MTL_HUD_ENCODER_TIMING_ENABLED=1 --env
  MTL_HUD_LOG_ENABLED=1 ...`): live GPU time per ENCODER with real overlap,
  logged once a second as `metal-HUD:` CSV lines — the cheapest honest
  per-encoder number. Its FPS and GPU time are per PRESENTED frame — with
  vsync off the mailbox renders several frames per present, so its GPU time
  is their sum, not one frame's. Encoder timing is blank while the app
  attaches counter sample buffers, which is why the engine's own
  per-encoder timestamps (`OBEY_MTL_GPU_TIMERS=1`, Tracy `Gpu/*` plots) are
  off by default.
- Instruments live: only the "Performance Limiters" counter set exists on
  this M4 (13 counters), and it must be recorded ATTACHED — all-process and
  launched recordings with it crashed GPUPlugin; the shader timeline
  template records anywhere but has yielded no samples (details under "GPU
  profiling" below). Instruments' "Launch" and `xctrace --launch` resolve
  the app by name through LaunchServices and started the installed game
  even after the `~/Applications` copies got their own
  `com.yourcompany.MyVoxelGame.<cfg>` bundle id: launch with `open`, then
  attach.
- Capture `metal-21-15-41` (2026-10-06, RD 32, rain, the labelled build,
  profiled at the **Maximum** state): 5.59 ms for the frame, 3,966 draws,
  3.95 M vertices, 1.31 M primitives; encoders Frame 72 %, rain half-res
  target 27 %, texture flush 1 %. Per pipeline (share of the frame's GPU
  work): `weather_half` fragment 24.9 %, `terrain_opaque` fragment 18.2 %,
  terrain vertex 14.2 % (+ 9.8 % under cutout, 1.7 % under translucent),
  `terrain_cutout` fragment 11.6 %, the rain composite (`entity_outline_blit`)
  8.4 %, `terrain_solid` (translucent) 4.5 %, sky 2.2 %. Registers: the
  terrain fragment shaders allocate 64, the vertex shader 42, the rain
  fragment 40. The rain target is launch-bound (Fragment Shader Launch
  Limiter 94 %, FS occupancy 82 %, 12x overdraw, ~23 k pixels per primitive):
  the lever is fewer covered pixels, not a cheaper shader. The frame encoder
  has no limiter above 37 % (instruction throughput 36 %, fragment launch
  32 %, texture read 27 %, texture filtering 23 %); 31 % of its primitives
  are back-face culled and 9 % zero-area; 47 % of samples take the
  explicit-gradient (greedy `textureGrad`) path; 44 % of vertices are
  reused through the index buffer. Per line (`terrain_vk_vert`): the
  function entry (stage-in fetch) 38 %, the world position from the
  section-origins UBO 11 %, the lightmap sample 8.5 %, `gl_Position` 3.8 %,
  the slot decode 3.1 %, the clip plane 1.4 %. OIT (off in normal settings)
  opens an empty full-res frame pass between every OIT pass.

Validation: a run under `MTL_DEBUG_LAYER=1` (stderr captured by running the
bundle's executable directly — `open` drops it) reports no errors, only
"unused binding" notes (Common / user-UBO / face-map bindings kept across
draws of shaders that do not read them). Shader validation
(`MTL_SHADER_VALIDATION=1`) runs at ~8 fps and is clean.

**Where Metal and Vulkan differ in cost.** The GPU work is the same — MoltenVK
already runs these shaders as Metal — and the scene is GPU-bound, so the two
are within a few percent of each other. Native Metal encodes every draw on the
main thread (~130 ns each), where MoltenVK records them cheaply and encodes on
its own submit thread: `MergeRuns.Flush` (≈1,900 terrain draws) is ~0.25 ms on
Metal against 0.04 ms on Vulkan. Moving encoding off the main thread (a
recorded command list, or an indirect command buffer) is the lever if the CPU
side ever matters.

**Benchmark (2026-10-06, tour replay, RD 32, vsync off, rain, full screen,
Game Mode, M4 Air).** Interleaved, 60 s apart:

| run (in order) | fps | 1%-low | p99 ms | >16.7 ms |
|---|---|---|---|---|
| Metal | 251.7 | 119.4 | 7.64 | 0 % |
| Vulkan | 233.4 | 105.4 | 8.68 | 0 % |
| OpenGL | 93.0 | 52.2 | 17.87 | 2.0 % |
| Metal | 211.0 | 95.4 | 9.81 | 0 % |
| Vulkan | 200.3 | 90.7 | 10.19 | 0 % |
| OpenGL | 90.7 | 47.4 | 19.80 | 4.0 % |

The Air loses ~5 % a run slot as it heats, which flatters whichever backend
runs first; a symmetric Vulkan, Metal, Metal, Vulkan block cancels that:
Metal 226.8 vs Vulkan 221.0 fps (+2.6 %), 1%-low 105.7 vs 101.6, 0.1%-low
95.1 vs 85.4, p99 8.92 vs 9.23 ms, worst frame 10.9-12.0 vs 13.4-13.9 ms;
main-thread CPU 2.47 vs 2.30 ms. Metal and Vulkan are within noise on
average with Metal's lows and spikes slightly better; OpenGL is ~2.4x
slower (driver-bound: `Present` waits 6 ms a frame).

The same session found a Vulkan bug worth remembering: `PipelineState::
blendAlphaLikeColor` (the half-res rain) was packed past bit 63 of
`VKBackend::HashPipelineState`'s 64-bit key and wrapped onto the depth-test
bit, so two rain pipelines shared a key and rebuilt each other on every draw
— `Vk.CreateGraphicsPipeline` 134 times / 9 ms per frame while it rained,
75-84 → 236 fps at RD 32 once fixed. `pack()` now asserts the bit budget, and
the pipeline layout left the key (the shader handle already determines it).

### Streaming buffers and frames in flight (2026-10-07)

Every renderer that rewrites a Dynamic/Streaming buffer each frame (mobs, players, XP orbs, block cubes, leashes, lightning, fishing hooks, End portals, sky blocks, the GUI, fill preview, clouds) kept TWO buffer sets alternated per frame — Vulkan's `MAX_FRAMES_IN_FLIGHT`. Metal's frame pacing (Tier 5) moved to three frames in flight, and `UpdateBuffer` on a host-visible buffer is a bare memcpy on both backends, so frame N+2 overwrote the set frame N was still drawing: entities flashed dark and jumped (a stale set from before the render origin moved is displaced by whole blocks), End portals vanished and shifted, only at vsync off where the frames actually pile up. The count now comes from the backend (`RenderBackend::FramesInFlight`: OpenGL 2, Vulkan 2, Metal 3) into `EntityFrame::Slots()`; the rings are sized `EntityFrame::kMaxSlots` and only `Slots()` sets are created; `EntityFrame::Cursor` cycles `slot` over them. A new streaming renderer uses the cursor, never a hard-coded pair. `ChunkMegaBuffer::kFreeDelayFrames` (3) and the Metal deferred-destroy delay already matched.

### Tear-free vsync off (2026-10-07)

With vsync off the Metal layer now keeps `displaySyncEnabled`: the mailbox already renders every frame and presents only when CoreAnimation can take one, so leaving display sync on changes just WHEN the presented frame flips — at the next refresh instead of mid-scan. Frame rate is unchanged (tour, clear weather, RD 32: tear-free 282.9 fps / tearing 236.0 fps in that heat order; `Mtl/DrawableWaitUs` ≈ 0 µs in both, the loop never waits for a drawable), a shown frame is at most one refresh old (half on average, ~8 ms at 60 Hz) — what Fast Sync / Enhanced Sync give on the PC drivers. `OBEY_MTL_TEARING=1` restores the tearing present for latency tests. Vulkan (MoltenVK) still tears with vsync off.

### Pipeline compiles are not a hitch here (2026-10-07)

Measured before building async pipeline creation + `MTLBinaryArchive`: on the tour (RD 32, clear) the warm-up builds ~25 pipelines in 3–14 ms from the manifest, and the 3–4 pipelines first met mid-run (`Mtl.CreatePipeline` zones after warm-up) cost 0.06–0.10 ms EACH — macOS keeps a compiled-pipeline cache per app (`/private/var/folders/.../com.apple.metal`), and even a fresh metallib (every Tracy build) compiles that fast from AIR on Apple GPUs. There is nothing for an archive or a background build to take off the frame; the item is closed with the numbers. If a future shader rewrite ever shows `Mtl/PipelinesBuilt` with multi-ms zones, revisit.

### Knowing whether a change is real (2026-10-07)

What decides it, in order of trust on this fanless Air:

1. **Within-run phase A/B** (`OBEY_SKIP=<token> OBEY_SKIP_PERIOD=<s>`, `tracy_report.py --phase`): the change is switched on and off every few seconds inside ONE run, so both sides share the machine's clock state and the drift that makes two runs 25 % apart cancels. The report now pairs consecutive (off, on) segments and prints the mean paired fps delta with a 95 % interval — a delta inside its interval is "within noise", not a result. With 5 s periods on the tour the interval is ±13 % because the scene changes between segments; `OBEY_SKIP_PERIOD=2` with `--settle 0.3` gives ~28 pairs and a usable ±4 %. Every change that can have a runtime switch should be measured this way. Today's GPU-driven numbers re-read with it: hot/rain "+3.6 %" → −1.7 % ± 13 % (noise); cooled/clear "−10 %" → −13.4 % ± 13.1 % (barely real).
2. **Same-frame Xcode capture** at one performance state: deterministic serial GPU cost per encoder and shader. It says what the GPU does, not what a frame costs live (the TBDR overlaps stages), so it explains and ranks; it does not accept or reject.
3. **Counters that explain the mechanism** (commands, indices, bytes, SIMD groups, the Tracy plots): a result without a mechanism is suspect; a mechanism without a live result is parked, not shipped.
4. **ABBA across builds** only when no switch is possible, cooled, and only for effects well above 10 %.

**The still-frame paired test** (`STILL=1 ab_run.sh <label> --env OBEY_SKIP=<token> --env OBEY_SKIP_PERIOD=2`, `tracy_report.py --phase --settle 0.3 --window 12:58`): the tour's first pose held for the whole run, the change alternating every 2 s. No scene variation and the pairing cancels heat, so the interval is ±0.3–0.9 % even while the absolute frame rate drifts 394 → 291 fps across a series — the number that does not care about heat. It answers one pose; the tour (±10 %) says whether the gain survives a flight. Re-checks made with it on 2026-10-08 (skip phase = the OLD behaviour): `OBEY_SPLIT_MIN` 900 vs 0 on Metal −1.7 % ± 0.4 % (0 stays); stand-in colour stored vs DontCare −0.6 % ± 0.4 % (a real win, once called null); depth clears vs bands −1.6 % ± 0.3 % (bands stay; clear weather); GPU-driven terrain vs CPU path: CPU +7.8 % ± 0.8 % (the ICB path loses even with nothing moving); half-res rain vs full on Metal (rain, `RAINHALF=1 OBEY_RAIN_HALF=1`): full +4.5 % ± 0.9 % on the mean BUT full's 1 %-low 118 vs 165 fps and p99 7.6 vs 5.7 ms — the mean favours Full, the lows favour Half; the control is hidden on Metal by the user's decision and this is the case for showing it again. Phase tokens now: `gpudraw`, `splitmin`, `standin`, `bands`, `rainhalf`, `mailbox`, `sky`, `weather`, `particles`.

Decisions from before the phase tool existed that rest on ABBA deltas under ~5 % and deserve a re-check when their subject is next on the table: Half-res rain removed on Metal (3 % ABBA, hot), `OBEY_SPLIT_MIN` 0 on Metal (2–3 % in a sweep), Tier 3's fragment work (serial −7 %, live "neutral" on a throttling series). The ones judged null but kept for their serial or bandwidth saving (vertex-stage trims, stand-in DontCare) cost nothing and are not at risk. Three frames in flight was first rejected by heat order and then accepted by a cleaner series — the kind of reversal the paired test exists to prevent.

### Metal 4 path: first measurements (2026-10-08)

`--metal4` (docs/metal4.md; `MetalFour.mm`) against the Metal 3 path, same build, flag only. A cooled ABBA of four still-frame runs was useless: the frame rate fell 412 → 356 → 312 → 296 fps across the four, heat alone. The usable method for a cross-backend comparison is an ALTERNATING series at the thermal plateau (two warm-up runs, then A B A B …) read with `scripts/pair_runs.py`, which pairs adjacent runs the way the phase report pairs segments. Still frame, four pairs each:

| regime | Metal 4 vs Metal 3 | verdict |
|---|---|---|
| render scale 50 % (GPU-light: frame waits ~0.9 ms, CPU work ~1.5 ms) | +8.0 % ± 5.7 % fps, every pair positive (+4.7 … +12.3) | real |
| render scale 100 % (GPU-bound: waits ~2.2 ms) | −0.7 % ± 9.6 % | neutral |

CPU work per frame is the same on both (1.4–1.9 ms either way): the Metal 4 gain at the lighter load is not fewer CPU cycles but shorter frame waits — the GPU finishes frames sooner with the Metal 4 submission (argument tables, no hazard tracking, explicit barriers). GPU-bound at full resolution nothing changes, as expected: same shaders, same passes. Finished path (pacing gate, counter heaps, compiler pipelines, overlay), plateau series on a throttled machine (2026-10-08 01:45–02:10, 200–310 fps): Metal 4 **with its pacing gate** vs Metal 3: −7.1 % ± 3.8 % fps (real), 1 %-low +11.5 % ± 21 %; Metal 4 unpaced vs paced: +6.1 % ± 11 % fps, lows equal — the "previous frame completed" gate costs the mean and buys nothing, so Metal 4 runs unpaced unless `OBEY_MTL_PACING=1`; half resolution: −0.1 % ± 33 % (noise at that heat). Net: Metal 4 unpaced ≈ Metal 3 paced at full resolution. Tour of the unpaced default, two warm-ups then four pairs on the throttled machine (150–176 fps): −1.9 % ± 18.5 % fps, 1 %-low −5.6 % ± 11.6 % — within noise. Verdict 2026-10-08: the complete Metal 4 path is equal to the Metal 3 path on this Mac in every regime measured (full res, half res, tour) with the pacing gate off, and behind it with the gate on; it stays opt-in (`--metal4`) — nothing to gain for players here, the platform work is in place for Macs where the CPU is the limit. Earlier tour (milestones 0–4), three pairs after one warm-up: −5.4 % ± 33.6 % — within noise; the first pair sat on the heating slope (262 → 207 fps), the two at the plateau read +3.2 % and +1.5 %. Verdict for now: Metal 4 is at least neutral everywhere and ahead when the GPU is not the limit; it stays behind `--metal4` until the counter heaps, the pacing gate and the overlay are in and a longer plateau series has run.

### Fragment stage: the late sky, half precision, one lightmap sample (2026-10-08)

The clear-weather tour frame at RD 32 (Xcode, Medium state, overlapping): terrain vertex 37 %, opaque terrain fragment 24 % (58 registers, FS occupancy 33 %), **sky fragment 17.6 %**, cutout fragment 6 %. The sky's two pipelines ran 230 k + 76 k SIMD groups — 7.4 M + 2.4 M fragments, the whole screen and a third of it again — with depth test off, as MC's sky pass draws it, before the terrain covered most of them. The TBDR's hidden-surface removal only spares fragments of opaque, depth-writing, discard-free draws, and the sky is none of those (the sky shader discards on alpha 0, the pack layers blend).

**Late sky.** The main view now draws the sky right AFTER the opaque and cutout terrain (before entities, block entities, translucent terrain and everything blended), depth-tested `LessEqual` with no depth write, and `sky_vk.vert` / the GL twin put every sky vertex on the far plane (`gl_Position.z = gl_Position.w`, depth 1.0 = the clear) — so the sky lands exactly where the terrain left nothing and the early depth test rejects the rest before shading. Output identical by construction (no MSAA; cutout holes keep depth 1.0 and get sky, as before). `SkyRenderer::SetDepthTested` is on only for that call: a portal or panorama view draws its sky first with the test off, as today, because its depth is not cleared under the silhouette. Still-frame paired test (`latesky` token, skip = MC's order): **−7.8 % ± 0.2 % fps for the old order** (23 pairs; GPU frame +10.6 %), i.e. +8.5 % for the late sky at the tour's first pose. `OBEY_LATE_SKY=0` restores MC's order.

**Half-precision terrain fragment shaders.** `terrain_{opaque,cutout,solid}_vk.frag` carry a `TERRAIN_F16` build (`<name>_f16_vk.frag.spv`, CMake `VK_F16_SHADERS`; `h*` types are float16 under the define, float otherwise, so the GL twins and the float .spv read the same source): the atlas texel, the lightmap colours, AO, tint and the fog mixes in half; texture coordinates, fog distances and the record's integer fields stay 32-bit. Metal loads the variant always (`MetalBackend::LoadFunction` tries `<name>_f16_vk_<stage>` first), Vulkan when the device has `shaderFloat16` (`VK_KHR_shader_float16_int8`, enabled in `CreateLogicalDevice`; MoltenVK, AMD GCN3+, NVIDIA Turing+, Intel Gen9+ — Pascal and older take the float .spv; `OBEY_VK_F16=0` forces it). Registers 58 → 44 (opaque/cutout/solid alike); the F32 limiter 22 → 15 % with F16 at 6 %. MC's `clamp` in `sampleLightmap` is gone from the fragment shaders: every light word the mesher writes is a MC light coord (0..240 per axis), so the coordinate is inside the clamp by construction — six ops × four samples per fragment. spirv-cross emits native `half` for `float16_t`; the samples come back `half4(tex.sample(...))`.

**One lightmap sample for a uniformly lit rectangle — tried twice, dropped.** First as a per-fragment test (the record's four corner light words equal → one sample): the same-pose serial capture showed nothing, because in the forest pose a SIMD group straddles blocks with and without uniform light and runs both sides — five samples instead of four. Then decided in the mesher (every corner of every block in a greedy rectangle shares one light word → the word in the vertex's light bytes with a flag byte, bit 19 of `fragSprite` + `fragAux` in the shaders, a flat branch that samples once and skips the light texel): registers 44 → 52 for the two code paths, opaque fragment serial cost +20 %, and live, same bundle, `OBEY_UNIFORM_LIGHT=0` alternating at the still pose: **−0.7 % and −4.6 % fps for the flag** in the two valid pairs (a third pair sat on a cooling gap). The saved samples do not pay for the occupancy the second path costs at this pose; the four-sample path stays, in half precision. The vertex's light word for a mapped rectangle is back to "carried but unread".

**Same pose, serial (Xcode, Medium), old build → late sky + f16:** 4.26 → 3.91 ms; FS invocations 26.0 M → 17.7 M; texture samples 65.1 M → 48.9 M. The capture-to-capture composition is not constant (the world time advances by each run's 45 s and is saved, so the sky-pack layer fades differ), which is why the live paired series decides and the serial numbers only explain. Live, still frame, hot machine (280–340 fps), old build vs late sky + f16, three alternating pairs: **+9.6 / +16.0 / +13.6 % fps (mean +13.1 % ± 8.0 %)**, 1 %-low up in every pair.

Counters after the change (still pose): texture read limiter 31–34 %, texture filtering 25–28 %, instruction throughput 36 %, F32 14 %, FS occupancy 32–36 % at 52 registers. The terrain fragment shaders are texture-bound now, not ALU-bound: 44 % of samples anisotropic, 54 % with explicit gradients (the greedy tiling's `textureGrad`).

**Alpha test before the lighting (cutout, translucent).** The cutout pass shades every layer (no hidden-surface removal with `discard`) and a cross plant is mostly transparent, so the atlas sample and the alpha test now come first and the record's light texel + the four lightmap samples run only for the texels that stay (`FaceRecord` carries the record's texel index, the light is fetched in `shadedVertexColor`); the translucent shader tests `textureColor.a * vertexAlpha` the same way. Exact. Measured against a twin build with the old order once the staging lag was fixed (below): **+4.5 / +1.8 / +3.2 % fps** in three alternating pairs at the plateau — a real small win; kept.

**The bundle's shaders lagged a shader-only build (found 2026-10-08, fixed).** The `.spv` files and `shaders.metallib` were staged into the app bundle by POST_BUILD steps of the executable, which run only when the executable relinks; a build that changes only shaders rebuilds the SPIR-V and the metallib but never relinks, so the bundle kept the previous build's shaders. Every Metal measurement of a shader-only change was therefore of the build before — and a fresh `.spv` next to a stale metallib (the `fragRect` varying) drew no terrain at all before it was understood. Staging is now its own rule (`stage_shaders`, a stamp that depends on the shader outputs, which `MyVoxelGame` depends on), so the bundle is refreshed in the same build whether or not the executable relinks; the generator also became a stamped custom command the `.air` rules depend on. Windows/Linux stage the same way through `$<TARGET_FILE_DIR>` copies and would need the same rule if shader-only builds matter there. Verdicts that were void: the cutout reorder's "neutral" and the eager light fetch's "−1..−2.5 %" (both bundles carried the same lagging shaders); the sky + half-precision series stands (new files not yet in the metallib compiled at startup from the current source) and so does the uniform-light series (one bundle, a mesh-time switch). Check before trusting a Metal shader number: `xcrun metal-source shaders.metallib -o dir` and grep the embedded source for the change.

**Anisotropic filtering is not a lever here**: 16× vs 4× at the still pose, three alternating pairs, −11.9 / −6.9 / +8.1 % — noise on a machine that fell 429 → 235 fps across the series; the hardware's adaptive level averages 2.7 taps, so the setting stays at the user's 16.

**Metal 4, after the changes, still pose (Xcode, Medium):** RD 32 3.93 ms — terrain vertex 41 %, opaque fragment 25 %, cutout fragment 17 %, translucent 6 %, sky 6 %, texture flush 2 %; RD 12 1.76 ms — opaque fragment 34 %, vertex 20 %, cutout 17 %, sky 14 %, translucent 10 %. Tour (Metal 4, clear): RD 32 394.8 fps / GPU frame 5.07 ms (was 375.9 / 5.88 in the morning), RD 12 793.8 fps / 2.85 ms (was 696.0 / 3.34). At RD 32 the vertex stage is the largest serial item and fetch-bound (VS device bandwidth 60 %); at RD 12 the two terrain fragment shaders are half the frame and launch/throughput-bound (FS occupancy 59 %).

**Per-line costs of the opaque fragment shader** (Xcode, still pose, after the changes; opened from a draw's Bound Resources → FragmentFunction, see the skill): `shadedVertexColor` 42.6 %, `fetchFaceRecord` 14.1 %, `sampleTerrainAtlas` 12.0 %, the two-sided mirror and stage-in ~7 %, fog + three mixes ~7 %. Inside them the cost lands on the first USE of each fetch's result — `r.color` (5.5 %) waits for the record texel, `atlasUV` (8.9 %) for the sprite-table rect, the four `param` lines (13 %) for the light texel, the two blends (7.5 %) for the lightmap samples: the shader is bound by a three-deep dependent chain (record → rect → atlas, record → light texel → lightmap) at 32 % occupancy, not by arithmetic. Two things followed. Reading the light texel together with the record in the opaque shader (both in flight at once): −2.5 % / −1.0 % live at the plateau — the compiler already schedules the reads; reverted. Taking the sprite rect out of the chain was tried and reverted: the vertex carried the rectangle's first block's sprite (the mapped vertex's otherwise unread light word), fetched its rect from the sprite table once per vertex and passed it flat as two 16.16-fixed words, and the fragment shader read the table only for a block whose sprite differs (the greedy census on the tour: 8.6 M rectangles, 3.9 M multi-block, 650 k of those mix sprites — 17 %). Live, three plateau pairs: **−1.2 / −3.4 / −4.9 %** — the 8-byte flat varying costs the fetch-bound vertex stage (shaded-vertex writes at 60 % of device bandwidth) more than the fragment's cached table fetch costs. The eager light read, re-measured on correct metallibs: +3.9 / −3.0 / −3.0 %, null; reverted. Both experiments say the dependent fetches are hidden well enough by the other SIMD groups: the per-line stalls are where the shader waits, not what the frame pays for.

What is left on the fragment side, in order of expected value: (1) occupancy — 52 registers in the half-precision build (44 on the Metal 3 compile of the same source; the mapped path keeps the rect, the record, four light words, the uv and its derivatives live), which the per-line view would pin down; (2) the dependent fetch chain record → sprite table → atlas, which only a per-rectangle sprite (merging across sprites off, the rect fetched per vertex) removes, at the cost of fewer merges in mixed-block caves; (3) the sky's two layers over the real sky pixels (disc + pack layer: 6–14 %), which only a pack with an opaque base layer could collapse; (4) a sprite texture array with hardware repeat instead of `fract` + `textureGrad` + the sprite table — a different atlas design, not a tweak. The lightmap's four samples are the look (MC's per-vertex colour interpolation reproduced per block) and stay.

### Main-thread pass after the fragment work (2026-10-08)

Tour at RD 32 on the M4 (Metal 4, clear): 1.58 ms of CPU work per frame against a GPU that delivers one every 2.5 ms. What was there, by self time: the draw submission loop 0.34 ms (2 to 4 thousand `drawIndexedPrimitives` at ~0.12 µs each — the API floor; only fewer draws help, and the draw count is a GPU decision, `OBEY_SPLIT_MIN`), the mesh scheduler's dirty walk 0.20 ms, the three per-layer draw-list builds 0.12 ms (16 ns per section per layer), the frustum filter 0.11 ms, the client tick 0.10 ms, mob rendering 0.08 ms. Two changes:

- **The scheduler's walk inverted.** `ScheduleMeshBuildsWithSnapshots` walked every chunk in the dirty index each pass — 3,355 of them at RD 32, nearly the loaded world, because out-of-view sections stay dirty until looked at — and asked each "is a view reaching you?", 0.6 ms a pass. The admission rule admits a far section only where a view reaches it, the player's own edits and relocations anywhere, and everything within 48 blocks, so the walk now visits exactly those places: the views' columns (`ChunkRenderer::ForEachViewColumn`, a few thousand mask reads), a 9×9 window around the player and around each portal route's far point, and an urgent set fed where a section is dirtied by the player or marked for relocation; a chunk is examined at most once per pass (`ClientChunk::schedulePass`). `OBEY_SCHEDULE_WALK=all` restores the full walk. Draws per frame, sections per frame and uploads per frame are unchanged (3,945 vs 4,019 entries, 1.9 vs 1.7 uploads/frame); the walk fell from 1.8× the frustum filter's time to 0.4× (0.20 → ~0.05 ms per frame).
- **Mob models and textures prewarmed at level load** (`MobRenderer::Prewarm`, after `WarmPipelines` under the loading overlay): every entity type's model and the base sheet, then every PNG under `assets/textures/entity` (956 files, 18 MB decoded, none over 512²) — the variants chosen at draw time at 37 call sites (villager types and professions, parrot and horse colours, fish patterns, eye and clothing layers, armour) each cost a decode and upload on first sight, 1 to 5 ms frames on the tour. 80 ms at load; the mob render zone's worst frame fell 8.5 → 1.4 ms.

**Reading CPU zones:** their times scale with the CPU clock. The same tour an hour later, machine hot, showed every untouched zone +40–50 % (frustum filter 0.111 → 0.162 ms). A CPU change is judged by its zone's ratio to an unchanged zone in the same capture, or by alternating runs, never by absolute milliseconds across two runs. What remains on the main thread is the submission loop at its floor, the draw-list builds and the frustum filter at tens of nanoseconds per section, and occasional load-phase bursts (region snapshots and uploads when a streaming batch lands, the BFS snapshot at 3–4 ms when it runs synchronously).

### The remaining spikes, instrumented and fixed (2026-10-08, evening)

A tour capture after the CPU pass still had frames of 8 to 11 ms whose main-thread time sat in zones with no children. Zones were added until every one attributed, then each source was fixed; the final capture (RD 32, Metal 4, clear, quiet machine) is 390.8 fps, 1 %-low 169.5, 0.1 %-low 129.9, two frames over 8.3 ms (both GPU waits), max 11.0 ms, draws and uploads per frame unchanged.

- **Metal 4 residency commit, 5–10 ms on the main thread.** `Mtl.M4ResidencyCommit` was the 7 ms inside `Mtl.BeginFrame`. Its cost is not the set's allocation count: shrinking the set from ~1,460 allocations to ~415 (sampled textures from 64 MB `MTLHeap`s, `M4HeapTexture`) left the median at 0.6 ms and the peaks at 8–10 ms. It is the page mapping of what became resident since the last commit — a 32 MB terrain slab, with the commit sitting between frames. Three things now: staging buffers live in a second, small residency set (`Metal4::residencyTransient`, commits of 8 µs), so the main set changes only when a real resource comes or goes; allocations of 4 MB and more are added and committed on a serial background queue (`Metal4::residencyQueue`, every mutation under `residencyMutex`), and a command buffer is committed only after `M4WaitResidency` finds the group drained; and `ChunkMegaBuffer` keeps one spare slab created ahead of need once a pool has two live slabs (`PrefersBufferPrefetch`, Metal 4 only; one slab of memory per growing buffer), so the mapping is done by the time the pool grows into it. Result: background commits 0.4–3.6 ms on their own thread, main-thread commits median 63 µs / p99 0.4 ms, the wait zone never fired on the tour. Plots `Mtl/M4Adds`, `Mtl/M4Removes`, `Mtl/M4Commits`, `Mtl/M4Allocations`.
- **Client block-entity tick, 2.2 ms a tick.** `Tick.BlockEntities` was the whole of the client tick's cost and invisible to the overlap tool (present in every tick, so no lift). `TickBlockEntities` deduplicated registrations by a linear scan and rebuilt its list after each tick by a quadratic merge (every listed position against every snapshot position: thousands × thousands at RD 32), with two or three chunk lookups and a `dynamic_cast` per entity. Now: an `unordered_set` mirror for O(1) registration and retirement, registrations made during a tick collected directly (`m_registeredDuringTick`), one lookup per entity, the list rebuilt in O(N). 2.17 → 0.57 ms per tick (p99 3.0 → 0.8); the client tick's p99 1.35 ms.
- **Outside-view fence, 1.1 ms per camera-chunk move.** `SetOutsideViewChunks` flipped the no-bridge flag of all 24 sections of every chunk that crossed the ring, three mega-buffer lookups each, when most of a column is air. Now only sections with GPU data. 1.1 ms → 0.11 ms median.
- **BFS snapshot, 1.6–2.1 ms.** The all-air case first (no GPU-pointer load for most sections) and the loaded chunks walked once (`ForEachLoadedChunk`) instead of a hash probe per grid cell. 1.6 → 1.2 ms median; the rest is the 101 k cells themselves, MC's own snapshot structure.
- **Not a cost: the sound tick.** The overlap tool blamed `Sound.Ambient` for the slow ticks; sub-zones put the whole biome lookup at 1 µs median with one 2 ms first-call load. The blame was coincidence of presence, which is what the lift metric cannot distinguish from cost when the real cost (the block-entity tick) has no zone of its own. Zone the suspect before believing the overlap.

**Trap:** two captures in this pass read 192 and 262 fps with every main-thread zone 3–5× slower and the GPU idle — a Zoom call, then CLion and Rider indexing after the edits. The run script now waits for the 1-minute load average to fall under 1.8 before launching, and a capture whose unchanged zones moved together is a loaded machine, not a change.

### GPU structural work, step 1: half varyings (2026-10-08, evening)

The terrain vertex shader now has a `TERRAIN_F16` twin (`terrain_f16_vk.vert.spv`) whose interpolated outputs — the colour and the two fog distances — are float16 (`StorageInputOutput16`; spirv-cross emits `half4` / `half2` interpolants, native on Apple GPUs), and the chunk-fade visibility rides in `fragAux`'s bits 16..23 instead of its own flat varying: 48 bytes of post-transform vertex instead of 64. The `_f16` fragment variants declare the same inputs, so a pipeline pairs f16 with f16 or float with float, never mixed — which is why the translucent shader's Improved Transparency variants got `_f16` twins too (`terrain_solid_oit_<stage>_f16_vk.frag.spv`), the Vulkan backend selects the f16 family only with `shaderFloat16` AND `storageInputOutput16` (`VK_KHR_16bit_storage`), and `OBEY_VK_F16=0` turns the whole family off. The OpenGL twins keep float varyings and fold the visibility the same way.

Same pose, serial (Xcode, Medium): the frame 3.91–4.04 → 3.67 ms, vertex-stage writes 43.7 → 34.0 MB (−22 %), fragment-stage reads 29.1 → 25.9 MB, device bandwidth 45 → 37 % of budget, and every terrain shader at 40 registers (from 52). Live, still frame, cool machine (455–472 fps), three alternating pairs against the pre-change bundle: **−2.1 / −2.5 / −0.9 % fps (−1.8 % ± 2.1)** — zero inside the interval. The M4 at this pose is not bandwidth-bound, so the saving does not become frames here; it is kept as the structural saving it is for GPUs that are (the bandwidth share is where an Intel iGPU lives), at no measured cost beyond noise. Look parity: panorama diff against the pre-change build within the old-vs-old control. Two generator/build traps met on the way: the generator's source check matched the plain `_f16_vk.frag` suffix before the OIT one and skipped the OIT twins (order fixed), and new `.metal` files enter the metallib only after the glob's re-configure — the next build. The lesson of Tier 2 holds: a vertex-stage saving shows live only if the vertex stage is the critical path, and after the fragment work it still is not on this Mac.

### GPU structural work, step 2: the sprite array (2026-10-08, night)

A greedy-merged terrain quad used to find its texels through the longest fetch chain in the fragment stage: the face record (texel buffer) → the sprite table (its rect in the atlas) → `fract()` → `textureGrad` on the atlas with the derivatives rescaled, 54 % of the opaque shader's samples with explicit gradients and the atlas padding ring doing the wrap. The block atlas now also builds **sprite arrays** (`AtlasBuilder::BuildSpriteArray`, texture slots 4 and 5 — GL units 4/5, Vulkan set 7 bindings 0/1, Metal textures 7/8 — two `sampler2DArray`s in the three terrain fragment shaders): one 16×16 layer per sprite, sprite id s = layer s % cap of array s / cap (cap = the backend's layer limit, `RenderBackend::MaxTextureArrayLayers`, 2048 on Metal; the second array is the overflow, bound as the first again when nothing overflows), sampled with hardware REPEAT and the hardware's own mip selection (`texture(uSpriteArray, vec3(uv, layer))`). The sprite ids are renumbered so the arrays' sprites come first (`BuildSpriteTable`: square, power-of-two, no larger than the pack's usual sprite side, from a texture file before generated variants), `uSpriteLayers` / `uSpriteArrayCap` (Common `uSpriteArray.xy`, the former `_pad`) tell the shader where they end and where the split is, and everything past the count — the 47 32-px sign/shelf sprites and the non-square ones — keeps the atlas path. On Vulkan set 7 is its own two-binding layout with one set per frame slot (`BindSpriteArraySet`: rewritten in the slot's frame when an array's per-frame copy or a rebuilt atlas changes the views — a second per-texture set would have been the ninth set, past MoltenVK's eight); Metal's argument tables grew to nine texture indices. An animated sprite's layer is redrawn by the `TextureAnimator` with its atlas rect (`RenderBackend::UpdateTextureArrayLevel`, the same staged path as the atlas's rect: Vulkan and Metal queue it into the next frame and the array gets per-frame copies once drawn with, OpenGL goes through the pixel-unpack ring), its frame chain built as deep as the layer needs. A layer's chain is the sprite's own (`Mipmap::GenerateMipLevels`, so the cutout rewrites match the atlas's), and a sprite smaller than the array's side is upscaled by a whole power of two into the lower levels so the GPU's LOD lands on the same texels; the array's side is the pack's mode on purpose (vanilla 16) because an upscaled level 0 makes every magnified near sample read 4× the texels. Vanilla: 2349 sprites in 2048 + 301 layers, 5 levels, 3.1 MB (+6 MB of per-frame copies on Metal once the animator writes them). Rebuilt with the mip chain (the options), destroyed with the atlas; `OBEY_SPRITE_ARRAY=0` builds none, `OBEY_SKIP=spritearray` is the in-run phase A/B (arrays resident, unused).

Still-frame paired phase test (the verdict tool; cool machine, 23 pairs), static sprites only: **array 522.9 fps vs atlas path 499.2 — the atlas path is −4.6 % ± 0.3 % (REAL)**; `Gpu/EncFrameUs` 4.34 vs 4.57 ms. With the animated sprites in the array too (water, lava, fire, magma, prismarine, the stems…): **545.7 vs 504.0 fps — the atlas path is −7.9 % ± 1.0 %**, `Gpu/EncFrameUs` 4.22 vs 4.62 ms (−8.6 %). With the overflow array as well (every square sprite in): **548.7 vs 501.5 fps — the atlas path is −8.8 % ± 1.1 %**, `Gpu/EncFrameUs` 4.18 vs 4.62 ms (−9.6 %). Tour (static-only build): +1.2 % ± 7.7 % (noise, as every moving-scene test is), GPU frame 4.62 vs 4.80 ms. The first live fragment-stage win since the late sky: the fetch chain lost a dependent texel fetch, the `fract`, and the explicit-gradient sample. Look: panoramas from one build (array, `OBEY_SPRITE_ARRAY=0`, array again; sprites frozen, time fixed) — array vs atlas 5–8 % of pixels differ by more than 2 levels (mean 0.44–0.63 of 255) against a 3–4 % same-path control (mean 0.2–0.37): sprite seams and distant faces, where the hardware wraps the sprite exactly while the atlas path's anisotropic footprint ran into the padding ring; nothing is mis-mapped and the difference is run-to-run stable. (The first parity triple read 0.02–0.6 % vs a 0.0–0.24 % control, which was too good to be true: zsh does not word-split a `$extra` string, so the "off" run had run with the array on — every run was a control. Switches go to the run script as separate words; skill trap 27.)

### GPU structural work, step 3: quad records (2026-10-09)

The plan in `docs/quad-records.md` is implemented behind `OBEY_QUAD_RECORDS=0` (mesher switch; the records are a mesh-time choice, so a toggle remeshes). Every axis-aligned full cube face — a greedy rectangle or a single face whose four corner lights sit on quarter levels, which in practice is all of them (census over the still frame: 1.65 M rectangles, 1.89 M singles, 0 singles kept as vertices for light) — is one 16-byte `Render::QuadRecord` in the slab's RGBA32UI face-map view instead of four 20-byte vertices and six indices. The record vertex shader (`terrain_rec_vk.vert` / `terrain_rec.vert`, no vertex input) pulls the record by `gl_VertexIndex / 4`, builds the corner from per-side tables, and hands the fragment shaders the same varyings as the vertex path (a rectangle points `fragRecord` at its face-map texel; a single face carries sprite, tint × AO × lightmap in the record itself). The index buffer is one shared 0 1 2 0 2 3 pattern (16384 quads) drawn with the record's first texel × 4 as the base vertex, so a facing run is one `MultiDrawIndexedBaseVertex` entry (`ChunkRenderer::SubmitRecordRuns`, fused when exact-adjacent). The free geometry (plants, stairs, fences, fluids, anything not a full face) and the whole translucent layer stay on vertices, so opaque and cutout now have two streams each.

Still-frame cross-build series (256 k-vertex opaque slabs, cool machine, three alternating pairs): fps −1.0 / −1.4 % in the two clean pairs (one −8.7 % outlier on the first run after the build), `Gpu/EncFrameUs` +1.4–2.1 %, `Mtl/Draws` 5861 → 6827 (+16 %: the second stream), `Mtl/DeviceAllocMB` 764 → 657–684 (−80..−107 MB at RD 32; opaque pool 9 slabs × 6.9 MB used). A second series isolated the pieces: the one-texel face-map change alone ≈ −1.5 % fps (noise-level GPU), and smaller slabs cost draws, not memory (128 k opaque slabs: +9 % sub-draws, reverted) — the opaque pool is slot-bound (`ChunkMegaBuffer::kSlotsPerSlab` 1024), so a slab's vertex capacity is mostly unused once the cube faces leave the vertex stream. Look parity (same build, records on / off / on, frozen sprites, fixed time): structurally identical, differences ≈ 2× the same-path control (mean ≤ 0.7 of 255) — streaks on distant leaves where a single face now samples the sprite array through the record path. Verdict on this Mac: memory −10..14 % of device allocation, frames flat to −1 %, draws +16 %; the vertex stage was never the critical path here (see Tier 2), so the −85 % per-face bytes do not become fps. Default to be decided on the numbers; `Mesh/*UsedMB` now counts a region's full allocation (records included) via `ChunkMegaBuffer::GetUsedBytes`.

### Shader packs: two silent faults found by screenshot (2026-10-09)

Sildur's Vibrant on OpenGL, tested after the user reported "water looks default, grass wrong, black screen at the sun". Two real faults, both silent (no log, no GL error): (1) **water never had the pack's block id** — `ShaderPipeline::EnsureEntityMap` maps `block.properties` ids onto sprites through the block MODEL's faces, and a fluid has none (its faces come from `FluidMeshBuilder` with the fluid sprites), so `mc_Entity` was 0 and the pack drew water as glass: the fluid sprites are now mapped directly; (2) **a gbuffers program's `colortexN` samplers had no unit** — the gbuffers texture layout bound the lightmap, normals, noise, depths, shadows and the block map, but never a colour buffer, so Sildur's water program sampled its reflections (`colortex4`, `colortex6`) from unit 0, the block atlas: rainbow water. The programs' sampler declarations are scanned at load and up to three colour buffers ride units 13..15 (OpenGL 3.3 guarantees 16 a stage). `OBEY_PACK_DUMP=1` logs the pack's frame inputs once a second; at time 1000 every value matched MC (sun angle 297.5°, sunAngle 0.077, eye light 0/15), so the look of a pack is now checked against its own math rather than guessed at. The world's "floating blocks" near the spawn are real blocks (identical on Metal). Frame cost is pixel-bound: 28 fps at 3420×2146 against 59 at Render Resolution 50 % (vsync on, RD 12) — the pack's own buffers follow the scaled scene, so the option is the lever. Diagnostic probes from September (readbacks every 300 frames, a four-frame engine-shader A/B after every load, a debug blit) were removed the same day. A third fault, from the user's screenshot: the short-grass plants drew as solid green squares — a two-sided plant quad (`TerrainVertex::TwoSided`) and a fluid plate carry a packed tile word and a sprite id where an untiled vertex carries its atlas uv, and the pack prelude handed the packed word to `gl_MultiTexCoord0`; `ShaderPackGlsl` now decodes tile × sprite rect into the uv (and the block-id map reads it too, so the plants get their `mc_Entity` and wave). The nine OpenGL shader files the renderers had lost (sky, clouds, gui_textured, gui_color, lines, player_billboard, highlight, crosshair, block_break_overlay) exist again, written from the inline copies the renderers carried, which are gone.

**Where a pack's frame goes** (`OBEY_PACK_DUMP=1` also turns the chunk renderer's pass timers on; GL timer queries, grass spot, RD 12, 3420×2146, vsync off): frame 42 ms, `Present` 32 ms (GPU-bound), CPU 9.5 ms. GPU: the pack's deferred passes (Sildur's lighting, shadows, SSDO) **11.4 ms**, composite + final 4.2, the shadow pass 3.1, terrain opaque 2.0 / cutout 1.2 / translucent 0.4 — 22 ms timed, ~8 ms not yet (the two full-resolution depth blits for depthtex1/depthtex2, sky, clouds and entities under the pack's programs, and the per-pass FBO create/destroy). So the levers, in order: the pack's own quality options (SSDO, PCSS, shadow samples, shadow map resolution — the Shader Options screen), Render Resolution (every pass is full-resolution and scales with it: 28 → 59 fps at 50 %, vsync-capped), then the untimed 8 ms, which is the engine's and recoverable. A Metal or Vulkan port would not shrink the pack's own passes.

### Shader packs on Metal and Vulkan (2026-10-09)

Packs run on all three backends. The port is `docs/shader-packs-port.md`:
glslang + SPIRV-Cross at run time (`client/shader/PackCompiler`), the
Vulkan-GLSL target of `ShaderPackGlsl::Vulkanize` (one std140 block, samplers at
their slots, clip z remapped after the pack's main), `RenderBackend::CreatePackShader`
and the texture-target / override contract both backends implement. The lessons
there are the ones to read before touching the pipeline again: GL-style matrices
everywhere, GL-style target storage, sampler units baked before compile,
feedback snapshots for buffers a program both reads and writes, fast math plus a
`pow` guard (precise math was a 5× regression). `tools/shader_pack_probe.sh`
captures any pack buffer or any expression inside a pack's fragment stage on
any backend.

One ordering rule outside the pipeline: the pack's deferred passes run from
the chunk renderer's before-translucent hook, and that hook must fire even
when the translucent pass has nothing to draw (`DrawTranslucentPass`). It
did not, so any view without translucent geometry — the sky, the sun over
dry land — skipped the pass that paints the pack's sky: a black sky with the
composite's sun halo drawn over it across the whole screen, on every backend.
A constant-colour `OBEY_PACK_PROBE` per program is the quick way to tell "this
pass is not running" from "this pass computes the wrong thing".

### Retina Resolution off on Metal and Vulkan (2026-10-09, night)

The setting only ever worked on OpenGL. Stock GLFW passes the
`GLFW_SCALE_FRAMEBUFFER` hint to the Vulkan surface's `CAMetalLayer` and
to `wantsBestResolutionOpenGLSurface`, but `glfwGetFramebufferSize` on a
`GLFW_NO_API` window keeps answering `convertRectToBacking` — 2× on a
Retina display whatever the hint said. The Metal backend sizes its layer
from that answer (contentsScale 2, the setting a no-op), and on Vulkan the
swapchain came out 1× against an engine working at 2×. Vendored patch,
`ext/glfw/src/cocoa_window.m` `framebufferRectForWindow`: a window with no
client API and the hint off reports its content rect in points, at the
three framebuffer-size sites and the content-scale query; OpenGL windows
keep AppKit's answer. Verified at 1710×1073 on Metal 3, Metal 4 and Vulkan.
What it buys with a pack (Sildur's, M4 Air, still pose, vsync off): 57 fps
at 3420×2146, 168 with Retina off, 165 at Render Resolution 50 % (same
pixel count; Render Resolution keeps the GUI sharp), 283 at 25 %.

### Fragment stage, round two: three tricks measured (2026-10-09)

Still-frame paired phase tests (the verdict tool), M4 Air, RD 32, clear, quad records on. What the fragment stage had left after the sprite arrays were latency (the record → sprite and record → lightmap chains), overdraw (alpha-tested layers) and pixel count; three of the four candidates were built and measured, one each way.

- **Record sprite hand-over — NULL, reverted.** The greedy scan merged only equal sprites (every quad-record rectangle then has one sprite, carried in the record and a flat varying) so the fragment shader could issue the sprite sample without waiting for the block record. Same-build alternating series, three pairs: +2.2 / −0.8 / −2.9 % fps (−0.5 % ± 2.6 %), GPU frame equal; the split-by-sprite merge cost +9 % rectangles and +28 MB of device memory. The record fetch's latency is hidden by occupancy after all — the per-line stall view does not price the chain (the 2026-10-08 lesson again). Removed entirely.
- **Cutout depth pre-pass — LOSS, reverted.** The cutout layer drawn twice: alpha test and depth only with colour writes off, then lit with depth Equal (the classic alpha-tested z-prepass; `invariant gl_Position` for the Equal). Still frame: the pre-pass build was −13.6 % fps / +12 % GPU frame; the moving tour −15.5 % fps. The cutout layer at this pose has little self-overdraw to remove, and on Apple's GPU the second pass re-launches every alpha-tested fragment (the lit shader still carries a discard, so it stays punch-through) — twice the launches for the same shading. Removed entirely, including the depth-only shader variant.
- **One lightmap sample at the blended light coords — WIN, default.** `OBEY_LIGHT_MODE=1`: a face-mapped rectangle's fragment blends the block's four corner LIGHT COORDS over the two triangles and samples the lightmap once, instead of blending four corner lightmap colours (MC's exact form, `OBEY_LIGHT_MODE=0`, `OBEY_SKIP=lightmode` for the in-run A/B). Phase test: **+2.0 % ± 0.3 % fps, GPU frame −6.3 %** (4643 vs 4956 µs). The lightmap's own arithmetic evaluated per fragment instead (MC core/lightmap.fsh inlined, no texture on that path, four more Common vec4s) was −2.0 % ± 0.3 % / GPU +6 % — ALU costs more than the one sample here — and is gone. The look: exact at every block corner; inside a block the blend follows the lightmap curve's curvature instead of MC's straight blend between the corner colours. Visible only where the four corners' light levels differ a lot (cave mouths, torch falloff), and the user checks it in game; `OBEY_LIGHT_MODE=0` restores MC's blend. The Common UBO grew one vec4 (`uTerrainLight`, 400 bytes).
- **Registers to 32 — measured, not reachable by trimming.** Three Xcode captures of the still frame (Performance → Overview; `applegpu-nt` offers no static count without an undocumented translator script): the three terrain fragment shaders allocate **52** registers each (44 on 2026-10-08, before the overflow array, the fog/lighting skips and the light mode), the record vertex shader 46, the vertex-path one 44. Compiling out the exact four-sample light path: still 52. Compiling out the atlas fallback sampling (sprite table + `fract` + `textureGrad`) AND the two-sided mirror as well: still 52. The allocation does not follow the branches removed — the compiler settles on an occupancy tier and spends the registers it has there on scheduling (loads hoisted early), so the next tier needs the core itself to shrink (the record fetch, the sprite sample, the fog and colour pipeline together), not a side path. No change shipped; the number to beat is 52 and it moves only with a structural cut.

### Fog off and World Lighting off skip their work (2026-10-08, night)

Both switches used to change values only: fog off pushed the distances to 1e9 and the shaders still ran the two ramps and the mix per fragment and the two `length()`s per vertex; World Lighting off filled the lightmap with white and the shaders still sampled it (once per vertex, four times plus the light texel per face-mapped fragment). Now `uFogColor.a` is 0 with fog off and `uWorldLighting` (Common `uScalarsD.w`) is 0 with lighting off, and the terrain shaders branch on them (uniform branches: no divergence, no new permutations; the output is identical because the skipped work was a no-op). Still-frame paired tests with the setting OFF, the skip suspended (`OBEY_SKIP=fogskip` / `lightskip`, which keep the old path) vs active: **fog skip +0.8 % fps ± 0.2 (GPU frame −0.9 %)**; **lightmap skip +7.5 % fps ± 1.9 (GPU frame −8.8 %)** — four samples per fragment were that much. With both settings on (the measurement default) nothing changes. `ab_run.sh` takes `FOG=0` / `WORLDLIGHT=0`.

### GPU-driven terrain submission (2026-10-07)

Opaque and cutout terrain on Metal are drawn from indirect command buffers filled by a compute kernel (`shaders/metal/terrain_cull.metal`, hand-written, linked into the metallib with a source fallback). The CPU keeps its BFS + frustum pass and hands the kernel one visibility byte per section slot of a slab; the kernel reads the slab's `ChunkMegaBuffer::CullEntry` row (origin, face-group index ranges, 48 B, written at upload), does the same facing test as the CPU path (`kFacingGroupOrder`: a +X group is visible iff `eye.x > minX`, -X iff `eye.x < minX + 16`), and appends one `render_command` per run of visible groups (at most three per section) with an atomic on the slab's `MTLIndirectCommandBufferExecutionRange`. The render pass then runs `executeCommandsInBuffer:indirectBuffer:` per slab with the slab's vertex stream bound; pipeline and buffers are inherited, so a command is only the draw's arguments. One indirect command buffer per layer and frame slot, sized to the layer's demand (three commands per visible section, +25 %, regrown at `BeginGpuTerrainCull` before any dispatch of that layer) — ~690 B a command on this driver, ~48 MB in all at RD 32; the first design (1024 slots × 3 per slab, every slab) was 219 MB. The culling is its own command buffer committed ahead of the frame's. Translucent, directional (back-to-front) and second views (portals) stay on the CPU path. `OBEY_GPU_DRAW=0` restores the CPU path; `OBEY_SKIP=gpudraw` is the phase A/B.

Two Metal rules met on the way: (1) a pipeline with `supportIndirectCommandBuffers` may not take textures (or texture buffers) as direct function arguments — "Fragment shader cannot be used with indirect command buffers" — so the three terrain shaders (`gen_metal_shaders.py` `ICB_SHADERS`) take theirs through one argument buffer at `MetalBindings::kTextureArgs` (buffer 7; slot k at `[[id(2k)]]`, its sampler at `[[id(2k+1)]]`), encoded by `MetalBackend::BindTextureArgs` for every draw with those shaders, GPU-driven or not (the first panorama check found the CPU path drawing no terrain because the encoding was tied to the switch). spirv-cross cannot be told those ids with `--msl-decoration-binding` (a texture and its sampler would share one id: "Full mutable aliasing ... only works on Metal 3+"), so the generator translates them without it and pins the member ids — in ascending order, which the Metal compiler requires — and the direct buffers' indices from the SPIR-V's own names. (2) Samplers reached through an argument buffer need `supportArgumentBuffers` at creation (validation layer: 825 k messages in one run).

Phase A/B (`OBEY_SKIP=gpudraw OBEY_SKIP_PERIOD=5`, tour, RD 32, rain, hot machine at ~220 fps, GPU-bound): GPU-driven 221.1 fps / 4.52 ms vs CPU 213.5 / 4.68 (+3.6 %), 1 %-low 101.7 vs 82.4 (+23 %), p99 8.80 vs 10.67 ms, frames > 8.3 ms 1.7 % vs 5.0 %; `Mtl/FrameWaitUs` 0.43 vs 1.87 ms, i.e. CPU work per frame 2.65 vs 4.25 ms (−1.6 ms); `Gpu/EncFrameUs` equal (6.75 vs 6.77 ms — the GPU does the same work). Draw calls from the CPU 1005 vs 8932 a frame. Look: still-frame panoramas GPU vs CPU differ by 0.5–5.6 % of pixels (> 31 levels), the CPU path against itself by 0.5–5.9 % — single-pixel edge ties in draw order, run-to-run noise. Validation layer clean. Cooled, CLEAR weather (the second phase A/B, ~330 fps): the GPU path LOSES — 308.3 vs 343.8 fps (−10 %), 1 %-low equal (145 vs 143), `Mtl/GpuFrameUs` 6.41 vs 5.97 ms although the frame encoder itself is shorter (5.12 vs 5.74 ms): the culling command buffer plus ~47 `executeCommandsInBuffer` a frame are a fixed GPU cost that a 3 ms frame cannot hide, and the CPU saving (FrameWait 1.67 vs 0.79 ms) buys nothing while the GPU is the limit. **Back to opt-in (`OBEY_GPU_DRAW=1`)** until the execute count comes down: the candidates are one execute per layer (commands binding their slab's vertex stream and origins themselves — `inheritBuffers` off, so every buffer per command, the push block in a ring instead of `setBytes`) and the cull dispatched into the frame's own command buffer before the pass opens instead of a command buffer of its own. Xcode, same frame on both paths (`metal-20-40-20` GPU-driven, `metal-20-47-58` CPU, both at the Medium state): 4.96 ms vs 3.72 ms serial. Not fixed overhead: the cull encoder is 0.22 ms (4.4 %), the rest is the Frame encoder doing MORE terrain — `terrain_vk_vert` 71,166 vs 51,142 SIMD groups (+39 %), `terrain_opaque_vk_frag` 230,409 vs 188,427 (+22 %), cutout vertex +33 %, 8,659 vs 5,689 GPU commands — while the sky shader is identical (230,179 vs 229,958), so it is the same view. The kernel's facing test is the CPU's line for line, so the extra work is not back-facing groups; the candidates are the ~3,000 extra draw commands (the CPU path fuses exact-adjacent runs, the kernel never does — a per-draw cost in the tiler that shows up as vertex-stage time) and the draw order (slab/slot order instead of the CPU's nearest-first; hidden-surface removal should make opaque shading order-independent, yet the opaque fragment count rose). Next read: the GPU capture's encoder counters (its CSV export failed in the automation) for primitives, overdraw and bytes, then fuse adjacent runs in the kernel (a run that ends where the next slot's begins continues it) and draw slabs in the CPU's distance order. Follow-up the same night, each step measured with the 2 s-period paired phase A/B (±10 %): (1) the kernel now fuses runs across empty groups and across touching neighbours (each slab's visible slots sorted by index offset, `GroupVisibility` + chain ownership) — commands 7,989 → 7,369 a frame, fewer than the CPU path's ~8,400 draws, indices 4.1 M vs 4.5 M; (2) each layer's cull committed as soon as encoded; (3) the cull as the frame command buffer's first compute encoder, before the sky (`PrepareGpuTerrainCull`); (4) the cull on its own queue with an `MTLEvent` the frame waits on (async compute). Results, GPU path vs CPU path: (2) cooled −11.6 % ± 14 %, (3) −27 % ± 10 %, (4) −20 % ± 9.5 % — every variant loses although the GPU path's frame encoder does the same or less work and the CPU does 0.1–1.6 ms less. The frame command buffer's span stays 1.3–2.3 ms longer than its encoder on the GPU path, so frame N's vertex stage is not overlapping frame N−1's fragment stage the way the CPU path's does; the per-command cost of an indirect command buffer on this GPU (Xcode: +39 % vertex-stage SIMD groups for fewer indices) is a second, independent loss. **Conclusion: GPU-driven submission through indirect command buffers is not viable for this workload on this driver** — thousands of small draws whose only saving is CPU time the machine does not lack. The path stays opt-in (`OBEY_GPU_DRAW=1`) for the record and should be removed unless Apple's ICB execution changes; the terrain shaders' argument-buffer textures (made for it) cost nothing measurable and are harmless either way.

## GPU cost per render stage (2026-09-25)

Metal cannot time stages inside one render pass on a tile GPU, so each stage
is measured by subtraction: `OBEY_SKIP=<stage> OBEY_SKIP_PERIOD=2` toggles it
every 2 s inside one run, each toggle is a Points-of-Interest signpost
("DevSkip" on/off; plus one "Frame" signpost per frame while OBEY_SKIP is set,
because Apple's GL presents outside CAMetalLayer), and a Metal System Trace
recorded with `--instrument "Points of Interest"` splits the GPU timeline at
those marks. Cost = GPU busy time per frame (vertex ∪ fragment) of the OFF
phases minus the ON phase between them (pairs cancel thermal drift; use only
phases the GPU recording fully covers — the signposts keep coming after it
stops). Parked at the tour's first pose, full screen, 20 s per stage:

| stage | Vulkan ms/frame | GL ms/frame |
|---|---|---|
| cutout terrain (leaves, plants) | 4.26 (vertex 4.3, fragment 2.1) | 5.84 |
| opaque terrain | 2.26 (vertex 3.0, fragment 0.5) | 4.26 |
| translucent | 0.72 | 0.54 |
| sky | 0.37 | 0.43 |
| held item, HUD, clouds, mobs, players, items, block entities, particles, outline | ≤ 0.07 each (noise ±0.03-0.07) | ≤ 0.08 each |

2026-10-05, RD 8, 100 %, raining, mailbox present (fps-based, `OBEY_SKIP_PERIOD=3`
in one run each — runs minutes apart differ by up to 30 % on the fanless Air,
so only within-run toggles are trusted): weather 2.06 ms/frame (276 → 642
fps; MC's rain is ~441 camera-facing 21-block quads, 7-8 screens of overdraw
at 3420×2146 — the Weather Radius option is the lever: 5 instead of 10 =
+44 %), opaque 0.59, cutout 0.29, translucent 0.21, sky 0.19, the rest
≤ 0.13 (noise floor ~0.07). Render Resolution 50 % = +82 %.

Rain, taken apart (2026-10-05, within-run A/Bs of temporary shader
variants): of its ~2 ms, ~1.2 ms is launching the ~50 M pixels of MC's
overdraw at all (a shader reading one input and keeping nothing), ~0.9 ms the
texture read that finds the streaks (rain.png is 2.3 % visible texels: 93
one-texel-wide streaks in 43 of 64 columns). Tried and reverted, so nobody
repeats them: drawing each column as strips over only its live texture
columns (exact, 30 % fewer pixels) was 0.5 ms SLOWER — tall thin triangles
multiply the tiler's work and waste 2×2-quad lanes along every edge; and
MC's own particle.vsh/.fsh structure (fog distances per vertex, textureLod)
measured the same as the current shader. Per-streak instancing would be
~1 M vertices a frame — more than the saving. The levers are the look:
Weather Radius (MC's option) or a resolution change.

Half-resolution rain (Video Settings → Rain Resolution, built 2026-10-05):
the columns drawn into a ½ × ½ target, depth-tested in the shader against the
farthest of the four full-resolution depth texels (`weather_half_vk.frag`,
`RenderBackend::FrameDepthTexture`), premultiplied, composited over the frame
with One / OneMinusSrcAlpha. Measured within one run: radius 10 +11-12 %
(0.32 ms), radius 5 −5 %. The shading saving is real (~1.4 ms) but on a
tile GPU leaving the frame's render pass and coming back costs **0.60 ms by
itself** at 3420×2146 (colour and depth out to memory and back; measured
with an empty target pass), and Vulkan must also store the frame's depth
(preserved while it rains). That was the split path, now OpenGL's only;
Vulkan draws the half-res rain ahead of the frame's pass from the previous
frame's depth instead (below), which keeps the whole saving.

Also tried in the full-resolution shader, within-run A/Bs (2026-10-05),
both SLOWER on the M4 through MoltenVK and reverted:
- an occupancy bitmask (one bit per texel, alpha > 25, dilated a texel; 16 KB
  uniform block) tested before the texture read: +0.76 ms. Every fragment
  indexes a different word, and divergent loads from Metal constant memory
  cost more than the sampler fetch they replace.
- `layout(early_fragment_tests)`: +0.64 ms; the hardware's own hidden-surface
  removal already handles the occluded fragments.
In-pass depth for a full-screen rain pass is not available either: MoltenVK
serves input attachments by Metal framebuffer fetch (`[[color(n)]]`), which
covers colour attachments only; a depth input attachment means ending the
Metal render pass, i.e. the same 0.6 ms break. The in-pass route would be
every world shader writing its depth to an extra (memoryless) colour
attachment that a full-screen pass then fetches — an engine-wide change.

Last two measured, then rain was left alone (2026-10-05):
- Ray-march upper bound — rain in one full-screen pass inside the frame's
  pass, no depth read at all (every pixel sky), 2D DDA over the column grid,
  columns in the user uniform block: 3.5 ms SLOWER than the rasterized rain
  (164 vs 378 fps). Each pixel visits ~20 cells and needs each one's column
  data whether it hits or not; even from a buffer texture that is ~40
  fetches a pixel against today's ~7.5. Dead on this GPU.
- Option "previous frame's depth, no split": a prototype (a second command
  buffer submitted ahead of the frame's) measured 0.82 ms over full
  resolution — then built properly, below.

**Metal has no Half (2026-10-07).** Once the rain's streak instancing removed
the ~97 % of launched fragments that failed the alpha cutout, Half's own
target pass, composite and previous-depth reprojection cost more than the
fragments it still saves: ABBA on the tour at RD 32, Full 217.1 fps / 4.61 ms
vs Half 209.9 fps / 4.77 ms. `WeatherEffectRenderer::HalfResolutionAvailable()`
is false on Metal — the setting is ignored and Video Settings shows no
control; OpenGL and Vulkan keep it (unmeasured there with the streaks).

### Rain from the previous frame's depth (Vulkan, Rain Resolution: Half)

Half-resolution rain on Vulkan is drawn BEFORE the frame's render pass opens,
depth-tested against the previous frame's depth, so the frame's pass never
splits. Within one run (`OBEY_SKIP=rainhalf`, `tour`, RD 8, 3420×2146, rain):
**3.22 ms vs 4.39 ms a frame for Full — 1.17 ms (228 → 310 fps)**; the Full
phases keep no depth and draw no bands, so that is the whole cost. OpenGL
keeps the split path (FBO 0's depth copied at the weather's place).

The pieces (`WeatherEffectRenderer::RenderAhead`, `VKBackend` depth handoff,
`LateDepthBands.hpp`, `weather_half_vk.frag`):
- **Gate.** Vulkan, Half, main view, precipitation this frame (rain or snow),
  not Improved Transparency. Frame by frame, no tail: the depth images are
  built preservable once while the option is on (`SetFrameDepthCapable`), so
  turning preservation on or off is only the pass's store op — never a
  device idle. Gate off: the frame is drawn exactly as before (checked: a
  frozen clear-weather scene is pixel-identical to the build without this,
  and the clear-weather replay's frame time is unchanged, 2.38 vs 2.39 ms).
- **Handoff.** A gated frame preserves its depth; its EndFrame moves the
  depth it drew the world into (the scaled scene's under Render Resolution)
  to SHADER_READ and stamps the slot (frame number, image, generation,
  size). The next frame's pass opens lazily (`EnsureFramePass` on its first
  draw; a full `Clear` on the unopened pass only sets its clear values — the
  frame's own start-of-frame clear must not open it), so `RenderAhead`, called
  right after the camera is final, binds the half target with no pass to
  break. A stale stamp (first rainy frame, resize, scale change, anything
  drawn first) draws the rain at full resolution in the frame instead. The
  slot's next frame moves the image back to an attachment before its pass.
  No frame-overlap cost: the queue is in order, and two slots are hazard-free.
- **Bands.** The hand, portal gun and GUI's 3D items cleared depth before
  drawing (MC's clear before the hand), which leaves the frame ending on
  depth that is not the world's. On gated frames they draw into viewport
  depth ranges near 0 instead — GUI [0, w), hand [w, 2w), gun [2w, 3w), w =
  0.01 on D32F (Apple GPUs), 0.05 on D24 (Intel/AMD Macs) — nearer wins, so
  the order is the clears' (gun first, the hand over it, the GUI over both).
  The GUI's per-zone clear becomes a depth clear of the zone's own rect to w.
  The rain treats stored depth below 3w as no occluder.
- **Reprojection.** The fragment shader multiplies its render-space position
  by the previous frame's view-projection (moved by the render-origin delta),
  samples the four depth texels there and keeps the farthest (as the split
  path did). Terrain is static, so a turn or a move is exact; off that
  frame's screen or behind its camera the rain is drawn. Fast-turn frames
  (~280 °/s) show no halos or holes at silhouettes, and nothing over the hand.
- **Alpha.** Vulkan pipelines blend alpha One / Zero (the fragment's own
  alpha) unless `PipelineState::blendAlphaLikeColor`; the half target needs
  its alpha accumulated "over" like its colour, or it holds only the last
  layer's and the composite lets too much frame through under the summed
  colour — overlapping streaks read light blue / white (fixed 2026-10-05;
  OpenGL's glBlendFunc always blended alpha like colour).

Rain otherwise stays at MC's geometry (~2 ms at 7 Mpx); Weather Effect Radius
5 (the Fast preset) is the other lever.

Terrain is the GPU frame; cutout costs more than opaque (MC's fancy leaves
draw every leaf face — `LeavesBlock.skipRendering` with `cutoutLeaves`).
Drawing cutout front-to-back (distance order instead of the merge's
slab/offset order) was measured and changed nothing on Vulkan (+0.01 ± 0.03
ms) and cost GL 1.5 ms (more draw calls) — the overdraw is not the problem.

## Greedy merging and light; per-quad layers (2026-09-25)

**Light is per block in the face map.** The light-engine port (2026-09-24)
made light a merge condition: a merged rectangle carried one light word, so
only faces lit alike at all four corners and across the rectangle merged —
next to nothing near shade, trees, caves or torches. A face-map record is now
two RGBA16 texels (`TerrainVertex::kFaceMapWordsPerRecord` = 4 words): colour,
AO and sprite, then the four corner light words in tile-corner order. The
terrain fragment shaders sample the lightmap at each corner, multiply by that
corner's AO and blend the four exactly as the GPU blends an unmerged quad's
vertex colours (MC terrain.vsh lights per vertex), so the image is unchanged
(screenshots vs `OBEY_NO_GREEDY` agree to capture noise). Tour pose, Fast
leaves: 4.85 M → 3.78 M terrain vertices; interleaved A/B against
`OBEY_GREEDY_LIGHT_SPLIT=1` (the old rule, same shaders): −0.4 ms GPU per
frame (vertex −0.9, fragment +0.25 for the per-pixel corner lighting).
Greedy as a whole: `OBEY_NO_GREEDY` draws 6.9 M vertices, +3.5 ms.

**Render layer per quad, as MC 26.3.** `SectionCompiler` puts each quad in
`ChunkSectionLayer.byTransparency` of the texels its UV rectangle covers
(`FaceBakery.computeMaterialTransparency`, OR-ed over an animated sprite's
frames; `force_translucent` materials are translucent; Fast leaves are forced
solid). `AtlasBuilder::QuadTransparency` + `Mesher::FaceTexelLayer` do the
same, cached per model face. ~14% of cutout faces move to the solid pass; at
the tour pose that measured no GPU change (within noise, +1% vertices from
split greedy rectangles) — kept for parity. `OBEY_BLOCK_LAYERS=1` restores the
per-block layer; `[LayerCensus]` in the log counts the moves.

## Fresh-chunk generation throughput (2026-09-26)

Measured in game: `/tp` into never-generated land at view distance 32 (3,725
chunks), Tracy zones per stage, `OBEY_SERVER_STATS=1` for the chunk rate.
Output-identical changes are checked with the harness: `async_chunk_test
--radius 10 --center 0 0 --phases 0-5 --seed 12345 --dump-full` (and r8 at
1500,-900) must produce byte-identical dumps; `--dedicated-lane` runs the lane
the way the game does.

- **The serial worldgen lane paces generation.** MC's `worldgenTaskDispatcher`
  is a consecutive executor, and FEATURES runs on it synchronously, so every
  chunk passes through one serial stage (~2.6 ms per chunk). Sharing the FIFO
  worldgen pool, each lane hop queued behind 5-30 ms terrain tasks: the lane
  was busy ~75% of every second while four of the nine pool threads idled,
  and faster terrain tasks barely moved wall time. The lane now runs on
  `SharedLaneExecutor` (two elevated threads, `WorldgenLane` in Tracy):
  13 s vs 15-16 s to generate the view, t90 11 s vs 13 s, features 1.6 vs
  1.9 ms per chunk. `OBEY_SHARED_LANE=1` restores the shared pool for A/B.
  The 2026-08-30 "no gain" measurement predates the 26.3 engine.
- **Lane bookkeeping (Game Mode, 3 interleaved pairs):** of the lane's time,
  features are ~77%, `Lane.ScheduleLayer` ~9.5%, structure starts ~6%,
  template loading ~4.4%, the executor's task wrapper ~2.6%. Kept, ~1%
  (all received 7.35-7.41 -> 7.30 s): `scheduleChunkInLayer` tests
  `hasCompletedStep` before chasing the chunk's persisted status (same
  outcome: a step's status is written to the chunk before the holder marks it
  complete, and only rises), and the pool's per-task CPU/wall clocks run only
  under `OBEY_POOL_PROBE`. The rest of ScheduleLayer is MC's walk over up to
  529 holders per layer, a cache miss each.
- **Structure-template warmup - tried and reverted.** Preloading every
  template's jigsaw data on a background thread (1,917 templates, 1.6-3 s,
  1.3-2.4 s CPU) and prefetching each piece's full template on the pool when
  the piece is created took template loading off the lane (0.33 -> 0.02 s a
  run) but did not move arrival times (90% 6.40 vs 6.38 s, 100% 7.35 vs
  7.23 s), while the prefetch parsed 2.6x more than the lane had used.
- **Features are at MC's algorithmic cost.** They are ~76% of the lane
  (7.0 of 9.2 s); ore placement is 45-70% of features, and most of that is
  the palette write of each ore block (MC's own work). Tried and reverted, no
  measurable change (1.61 vs 1.62 ms per chunk in game; single-thread harness
  over 15 interleaved pairs -0.4..-1.1%, within noise): a 64-slot per-Block
  memo in `TagMatchTest`, per-Block memo of the tree sweep's name checks. Features
  cannot leave the lane: MC runs them strictly one chunk at a time and
  neighbouring chunks' features overlap.
- **With the lane off the pool, the machine was CPU-saturated** (pool ~7 of
  11 threads, render and I/O on top of 4P+6E cores); after the noise work
  below, wall time is paced by the lane again and Surface (~30 s of CPU for
  the view) is the largest remaining CPU cost.
- **The whole pipeline keeps up with generation** (Tracy, zone end times
  after the teleport): conversion, initial light, send and client receive
  finish within 0.1-0.3 s of each chunk's generation, and meshing's 90% mark
  is 0.1 s behind. Generation is the bottleneck, and inside it the serial
  lane (~78% busy); per-chunk cost roughly doubles over a 10 s run as the Air
  heats (surface 4.7 -> 7.5 ms, features 1.2 -> 2.4 ms).
- **Measure under real Game Mode.** A directly exec'd build-tree binary never
  gets it (nor does `gamepolicyctl game-mode set on` alone: the daemon sees an
  unregistered identity and no gaming session). Clone the bundle to
  `~/Applications`, `lsregister -f`, `open -W --env K=V ... --args ...` (open's
  own --env: the game opens its log before parsing its own `--env`), policy
  automatic, and confirm per run that gamepolicyd logged "Full screen gaming
  session is now active" (`/usr/bin/log show` - zsh's `log` is a builtin).
  Under Game Mode the whole picture shifts: the fresh view took 9 s instead
  of 12-13 s, and conclusions drawn without it can be wrong (below).
- **8 generation requests in flight on high-tier hardware** (MC: 4;
  `OBEY_INFLIGHT` overrides, low-tier `HardwareProfile` keeps 4). Without Game
  Mode 8 showed no gain; under it (3 interleaved runs each): 50/90/100% of the
  view received at 4.6/8.0/8.9 s with 4, 4.0/7.3/7.7 s with 8. 16 was worse
  (farther chunks and border-only chunks crowd out the near ones).
- **Chunk batch rate vs the frame (client).** `ChunkBatchSizeCalculator`
  timed each batch by wall clock like vanilla; the client drains packets once
  per frame, so at 120 Hz vsync a batch arriving across a frame boundary
  measured >= 8.3 ms whatever its size, the requested rate fell to 11-20
  chunks a tick and one run in three stalled delivery at ~250 chunks/s while
  generation ran at 500 (all received at 10-14 s instead of 8). Now a batch
  costs its arrival span on the I/O thread plus its main-thread apply time,
  unless a drain hit its 6 ms budget during the batch (client behind: wall
  time, as before); a slow link still stretches the arrival span. With both
  changes: 3.5/6.3/7.2 s (was 3.9-4.9/7.0-8.9/8.0-10.3 s), minimum requested
  rate 24-78 chunks a tick instead of 11-13.
- **Features are at MC's algorithmic cost.** They are ~76% of the lane
  (7.0 of 9.2 s); ore placement is 45-70% of features, and most of that is
  the palette write of each ore block (MC's own work). Tried and reverted, no
  measurable change (1.61 vs 1.62 ms per chunk): a 64-slot per-Block memo in
  `TagMatchTest`, per-Block memo of the tree sweep's name checks. Features
  cannot leave the lane: MC runs them strictly one chunk at a time and
  neighbouring chunks' features overlap.
- **With the lane off the pool, the machine was CPU-saturated** (pool ~7 of
  11 threads, render and I/O on top of 4P+6E cores); after the noise work
  below, wall time is paced by the lane again and Surface (~30 s of CPU for
  the view) is the largest remaining CPU cost.
- **The whole pipeline keeps up with generation** (Tracy, zone end times
  after the teleport): conversion, initial light, send and client receive
  finish within 0.1-0.3 s of each chunk's generation, and meshing's 90% mark
  is 0.1 s behind. Generation is the bottleneck, and inside it the serial
  lane (~78% busy); per-chunk cost roughly doubles over a 10 s run as the Air
  heats (surface 4.7 -> 7.5 ms, features 1.2 -> 2.4 ms).
- **`OBEY_INFLIGHT` (MC's 4-request throttle) does not bring chunks sooner.**
  With the dedicated lane, 8 / 16 in flight raise lane occupancy to 89% / 95%
  and finish all generation 1-2 s earlier, but the chunks the player gets are
  no earlier (50% at 4.7 s, 90% at 9.3 s with 4 and 8; 5.2-6.1 / 9.6-10.0 s
  with 16): the extra lane work goes to farther chunks and to border chunks
  generated only for their neighbours. A pool that prioritizes by ticket
  level would do the same, so neither was adopted.
- **Per-chunk CPU that was pure overhead** (all output-identical):
  - `BiomeManager::getBiome` (the surface rules ask per block) recomputed
    eight corners' LCG fiddles every call; they come from a per-thread
    direct-mapped corner cache now (`BiomeManager::cornerFiddle`). The
    distance sum is unchanged.
  - `Biomes::get` took the registry mutex on every quart of the biome fill
    and the carvers (~1,000 contended-wait samples); a per-thread cache of
    recent keys serves nearly all of them. Safe because an entry is never
    replaced once created.
  - The carvers sampled the full climate for all 289 source chunks of every
    chunk; MC memoizes that on the source chunk (`ChunkAccess.carverBiome`),
    and so does `ProtoChunk::carverBiome` now (via
    `TerrainContext::carverBiomeRegion`). Carvers 2.7 -> 0.9 ms per chunk.
  - `RegionBiomeSource` did a `dynamic_cast` per lookup; the grid is cast once.
  - Net: Surface 6.9 -> 5.7 ms, Biomes 1.35 -> 0.57 ms, terrain CPU -18%.
- **Noise stage, 7.6 -> 5.2 ms per chunk in game (-32%)**, all verified
  byte-identical (density parity 222/222 vs Java + terrain dumps, four regions
  including a structure-heavy r16 at 3000,3000):
  - `PerlinNoise` / `SmearedPerlinNoise::addToVolume`: the Y terms (wrap,
    floor, smoothstep, smear) depend on indexY alone and the X terms on indexX
    alone; they are computed once per call, not once per cell. The Y walk runs
    over runs of equal floorY (gradients refreshed where a run starts, exactly
    where the old `lastFloorY` test fired) and the run is a branch-free loop
    Clang vectorizes 4-wide (check with `-Rpass=loop-vectorize`). Lanes do the
    scalar operations in the same order and `-ffp-contract=off` forbids FMA,
    so values are bit-identical.
  - `NoiseBasedAquifer`: the 12 cells around an anchor (index + unpacked
    location) are cached per anchor; an anchor spans 16x12x16 blocks.
  - `Beardifier::sampleVolume`: per column, only the pieces/junctions that can
    reach it (beard kernel / bury sphere) are summed, in their order. The
    others add exactly +0.0f and the sum starts at +0.0f (never -0.0f), so
    dropping them is exact; the sphere test uses x*x + z*z (a lower bound of
    the float x*x + y*y + z*z, rounding being monotonic).
  - Single-thread harness (`MC_BG_THREADS=1`, r8): 4.35 -> 3.97 s per run.
  - Wall time for the view did not move (13 s): the serial lane paces it again.
- **Surface stage, ~6.3 -> ~5.9 ms per chunk in game** (about -10% relative
  to the same run's Noise; runs vary with temperature), byte-identical:
  - `BiomeManager::getBiome` keeps the last cell's eight corner fiddles (a
    column walk asks for the same cell 3 times in 4); the corner cache only
    fills a new cell.
  - `ProtoChunk` keeps its heightmaps in a per-type array as well as the map
    (MC: EnumMap), and `ChunkStatus::heightmapsAfterList` is a vector: every
    block write updated the tracked heightmaps through two `std::map` lookups
    each and a `std::set` walk.
  - What remains is MC's rule tree (one virtual evaluator per rule, memoized
    lazy conditions), the biome zoom's eight distance sums, and block
    reads/writes - no redundant work left to remove without changing the
    evaluation itself.
- `terrain/tests/parity/minecraft_classpath.txt` carried the old Desktop path
  after the project move, so `run_density_parity.sh` compared against a stale
  `java.bin` (75/222); fixed, 222/222.
- **Thermal**: this Air throttles within a few runs of full-pool generation
  (per-chunk Surface cost climbs 4.4 -> 8 ms across one 15 s run). Compare
  back-to-back runs only after a 60 s cool-down, and prefer per-stage Tracy
  means over wall time for CPU changes.

## Saved-chunk loading (2026-09-26)

Measured under Game Mode on a saved 32-chunk view (3,725 chunks): join into a
saved world, and `/tp` from one saved area to another; per-stage end times
from Tracy (`LoadFromDisk`, `PrebuildChunk` = received, `ProcessMeshJob`),
frame times from the `Render` zone. Before this pass the view took 1.9-2.6 s
to arrive; now ~1.15-1.25 s, frames unchanged (p50 16.4 ms, p95 ~18 ms).
The fixes, in the order they became the bottleneck:

- **Region reads serialized on one mutex.** `AnvilChunkIo::ReadChunkNbt` held
  the store lock across the file read AND the zlib inflate: 27% of load time
  was lock waiting. Now `AnvilRegion::ReadCompressed` (positional `pread`,
  no shared file position) runs under a `shared_mutex` in shared mode when
  the region is already open (`RegionStore::Find`, no LRU touch); opening,
  evicting, writing and clearing take it exclusively; inflate runs unlocked.
  Writes `fflush`+`fsync` before they return, so a positional read after a
  write sees it. Verified: all 14,784 chunks of a saved world read and parse
  identically through the new path.
- **NBT list parsing was quadratic.** Every list element was parsed from a
  copy of the rest of the buffer with its type byte prepended.
  `NBTTag::ParsePayload` parses elements in place (identical trees on all
  chunks of a saved world; parse ~0.09 ms/chunk cheaper).
- **Chunk-batch pacing on the integrated server.** The client's allowance
  (vanilla 7 ms per tick) held a saved view to ~100 chunks a tick although
  applying one costs ~70 us and the per-frame drain budget (6 ms) already
  guards the frame. `ClientPacketHandler::SetLocalServer`: our own server
  gets 35 ms and up to 512 chunks a tick, and the estimate starts at 0.1 ms
  per chunk instead of 0.25 ms; remote servers keep 7 ms / 256 (a batch
  there costs link time, and a larger allowance queues data ahead of chat on
  a slow link). `OBEY_CHUNK_BUDGET_MS` / `OBEY_CHUNK_RATE_MAX` override.
- **12 ms serialize budget per tick.** `PlayerSession::SendNextChunks` built
  ~130 chunk packets a tick on the server thread (~90 us each). Now in waves
  on `Core::ParallelFor` (the builder only reads the chunk; the server thread
  waits), sent in the same nearest-first order; the 12 ms budget is checked
  between waves. ~200 chunks a tick, server ticks still < 50 ms.
- **Load jobs on the efficiency cores.** `ServerWorkerPool` workers run at
  Throughput QoS; a `CHUNK_LOADING` job runs Elevated. Disk read of the view
  1.1-1.3 s -> 0.8-0.9 s. The class changes only when the job TYPE does
  (`WORLD_IO` - entity reads, entity writes, fresh-chunk conversion - keeps
  the current one): dropping to Throughput after every load made each worker
  wait for a core between loads behind any default-QoS work, and at world
  open that work is the stronghold ring search (MC
  `ChunkGeneratorStructureState.ensureStructuresGenerated`: 128
  `findBiomeHorizontal` scans, ~3.5 s of CPU on the nine-thread worldgen
  pool, ~0.35 s wall). For its whole run the loads trickled - four workers
  mostly idle - and the view's disk reads finished at 0.59-0.69 s; now
  0.43-0.46 s (Game Mode, join and teleport).
- **World open.** The terrain library's block table (`Blocks::bootstrap`,
  ~150 ms, now `call_once` and thread-safe) was built on the server thread
  when a world opened; PlatformMain now builds it on a background thread at
  launch (joined when Run returns). Server start -> first chunk read
  0.26 s -> 0.10 s.
- **Palette decode.** `Anvil::UnpackFromDisk` adopts the disk words directly
  (`PalettedContainer::ReadFrom`) whenever the engine would use a palette of
  the same width (block states <= 256 entries, biomes <= 8) - the layouts are
  identical (MC SimpleBitStorage) - after a range check of every index; the
  unpack + `BuildFrom` path remains for the rest. `BitStorage::ForEach` walks
  words instead of 4096 `Get`s, used by `PalettedContainer::ForEachValue`
  (the census recount on load). Verified identical
  against the old path on 3,984 random palettes of every size, invalid
  indices included. Load cost per chunk -10..17%.
- **NBT parse.** `NBTParser` reads int/long arrays in one bounds check with
  unchecked big-endian loads (they were a checked `ReadInt64BE` per element),
  byte arrays by `memcpy`, ASCII strings without the modified-UTF-8 decoder
  (it maps ASCII to itself), and moves tag names and children instead of
  copying them. 61 -> 35 us per chunk over the saved world's 14,784 chunks,
  canonical tree hash identical on every one. What is left is the tree's
  allocations (a `shared_ptr` per tag, an `unordered_map` per compound).
- **Palette entry memo.** `ResolveBlockStateEntry` (a palette entry -> state
  id through `BlockStateRegistry`: several strings built and hashed per
  entry, per section) is memoised per thread on name + properties in the
  compound's own order; only resolutions that found a block are kept, so an
  unknown block still reaches the unimplemented-block tracker every time.
  `OBEY_NO_PALETTE_MEMO=1` = off. Load cost per chunk ~ -7%.
- **Client settings before play.** The client sends its view/simulation
  distance right after LoginStart (MC's configuration phase sends
  ClientInformation before the game starts); the server's pre-play handler
  parks it and `OnPlayerJoin` applies it as the session is created. Sent
  only on LOGIN SUCCESS it waited for the next tick, so a join tracked the
  7x7 default first: the full view is now requested 13 ms after the session
  exists instead of 52 ms.
- **Chunk packet build.** `SendNextChunks` counted each section's non-air
  blocks by walking all 4096 voxels (`ForEachValue`) per chunk per player;
  it now sends the section's maintained `nonAirCount` (MC
  `LevelChunkSection.write` sends `nonEmptyBlockCount` the same way). Palette
  words are written and read with `PacketBuffer::WriteLongs` /
  `PacketReader::ReadLongs` (one resize / one bounds check instead of 8
  `push_back`s a word). `SerializeChunk` 111-129 -> 15-21 us; the view now
  reaches the client in 0.66-0.76 s (join and teleport), was 0.86-1.26 s.
- **libdeflate for chunk decompression** (FetchContent, v1.24, MIT; runtime
  CPU dispatch, so one build serves Apple silicon, Intel Macs and Windows;
  x86_64 and universal slices compiled and round-trip tested).
  `AnvilRegion::Inflate` decodes zlib/gzip chunk streams through
  `Core::Deflate::Decompress` (a per-thread scratch buffer grown until the
  stream fits). 78.5 -> 17.1 us per chunk on the saved world's 14,784
  chunks (identical output); in game the disk-load cost per chunk -20..28%.
- **zlib and minizip removed (2026-09-26).** Every deflate stream in the
  game, the launcher and the terrain library goes through libdeflate:
  `common/core/Deflate` (compress; exact-size decompress for network frames
  and zip entries; unknown-size decompress that grows and restarts, gzip
  sized from its ISIZE trailer; CRC-32), `common/core/ZipArchive` (read-only
  zip over it: central directory, Zip64, stored/deflate, CRC-checked — the
  resource and shader packs and the launcher's Installer), and the terrain
  library's own `util/Deflate.h` (its standalone parity build fetches the
  same tag). What it costs: no streaming, so a zip entry or a stream is held
  whole in memory (the largest is the game binary in an update zip), and a
  zlib stream of unknown size may be decoded more than once while its buffer
  grows. Sentry's crash handler (crashpad) still carries zlib — the OS's on
  macOS/Linux, its own copy on Windows — which is not ours to swap.
- **Network compression (remote players) also on libdeflate** -
  `NetworkConnection::FrameForWire` / `ProcessFrame`, level 6 zlib format.
  Interop verified both ways
  on 14,784 real chunk bodies (0 failures; 2.8x faster to compress, same
  77.3 MB); live test: headless server + 3 bots over the LAN address,
  compression on, 0 disconnects. Loopback (single player) never compresses.
- **Tried and reverted: letting chunk serialisation use the tick's idle time**
  (a 35 ms shared deadline past each session's 12 ms). No change in arrival:
  after libdeflate the per-tick batches are cut by CPU contention (waves run
  slowly while the loaders and mesh workers saturate the machine), not by the
  budget, and ticks looked the same either way.
- **Local rate estimator starts with a full history weight (9)** - at weight
  1 the first batch (arriving during the client's first frames and pipeline
  builds) was half the estimate and collapsed the requested rate right after
  a join. Teleport arrival 1.06-1.11 -> 0.93-1.01 s; join unchanged.
- **Click to view, measured** (Tracy log messages, `--world` = the world
  list's action; a real click comes from an already full-screen title
  screen): server thread 7 ms, world + terrain generator ready ~32 ms,
  session ~33 ms, full view requested ~45 ms, loading screen gone ~160 ms,
  every chunk read ~0.42-0.55 s, every chunk at the client 0.57-0.69 s,
  90% meshed 0.66-0.70 s (was 0.85-1.05 / 0.9-1.1 s). Teleport: every
  chunk at the client 0.52-0.55 s after the first read.
- **First server tick at once** (MC `runServer`: `nextTickTimeNanos =
  getNanos()` after `initServer`). Starting the schedule a tick out held the
  owner's login - queued since before the world finished opening - for
  ~50 ms.
- **Client chunk decode off the network thread.** `ClientConnection`
  queues each ChunkDataS2C in arrival order as an `AsyncChunkDataS2CPacket`
  whose decode runs on `ChunkDecodePool` (3 Elevated threads, 1 on low-tier
  hardware); applying one waits for its own decode (FIFO, so at most one
  chunk's decode). `ChunkSection::AdoptStatesFromWire` takes the packet's
  non-air count (MC `LevelChunkSection.read`) instead of recounting 4096
  voxels, and recounts only when the palette holds a randomly ticking
  block; light layers are read in place (`PacketReader::ReadSpan`).
  Decode + build ~110 -> ~70 us, now parallel.
- **Chunk batch rate estimator, per stage** (`ClientPacketHandler::
  ChunkBatchSizeCalculator`, the role of MC's `ChunkBatchSizeCalculator`).
  Vanilla's wall time per batch counted the server spreading a batch over
  its send phase, the wait for the next frame's drain and the client's
  startup frames: it read 45-425 us a chunk against a ~3 us apply and held
  a saved view to 250-380 chunks a tick. Now three stages are timed: link =
  (arrival span - the server's own send span, a trailing field on
  `ChunkBatchFinishedS2C`) / n; decode on the pool threads; apply on the
  main thread against a 7 ms-a-tick budget. The client asks for what its
  slowest stage can take (caps 512 local / 256 remote), and scales down
  when more than two ticks' worth of chunks sit unapplied. Fresh estimate
  per connection, light averaging (3 batches). Tracy plots
  `ChunkBatch/*`. Singleplayer asks for 512 from the first batch; over the
  LAN address it measured the server's per-connection compression (level 6,
  ~250 us a chunk on the network thread) and asked for 112-200.
- **Chunk packets deflated on the parallel workers.** A compressing
  connection (remote player) deflated every chunk (level 6, ~250 us) on the
  server's single network thread - the link stage above. `SendNextChunks`
  now builds each chunk with `NetworkConnection::PreparePacket` on the
  wave's workers (body + libdeflate deflate for the connection's current
  threshold) and queues it with `SendPrepared`; the strand still frames at
  write time and uses the prepared deflate only if the stream's threshold is
  the one it was made for (else frames as `SendPacket` would), so the switch
  to compression stays race-free and the bytes are identical. LAN join
  (same machine, LAN address): link 240-640 -> 7-69 us a chunk, requested
  rate 70-190 -> 256 (the remote cap), first-to-last chunk 1.32 -> 1.03 s;
  meshes on the remote client identical.
- **Network I/O threads Elevated** (client `NetworkIOService`, server
  network thread). At the default QoS they lost the cores to the loaders,
  decoders and mesh workers during a burst and a batch trickled in over tens
  of ms on loopback (what the link stage first read as 0.3-2.9 ms a chunk).
- **Light border pass filtered.** `ReconcileFace` queues an increase only
  when the engine's own first step could raise the cell across the border
  (`from - Opacity(to) > to`); air against stone was queued, popped and
  dropped (~0.36 M of 1.9 M entries on the benchmark world; identical
  light). Most of the rest is real: a world saved with unfinished border
  light repairs (and saves) it on its next load - a re-saved copy of the
  benchmark world did ~1,000 entries.
- What is left: the loaders at ~0.35-0.4 ms per chunk (NBT tree allocation,
  palette/section build, light layers), the client's I/O thread decoding
  chunk packets (~90-115 us each, serial), and the whole machine busy during
  the burst (loaders, ring search, mesh workers, render). Tracy builds put
  every log line on the capture timeline (`PROFILE_MESSAGE` in `Log.cpp`;
  `tracy-csvexport -m`), which is how the join hand-off above was timed. Game launch (not world open) is ~1.4 s of sequential
  main-thread init (atlas ~270 ms, block models ~310 ms, sounds ~150 ms,
  asset listing ~140 ms, items ~100-150 ms) - done by the time the title
  screen shows, so it is not part of opening a world from the menu.
- Tools: saved-world benchmark = a private copy of a world saved after a
  `/tp` run (join: player restored in the saved area; teleport: `/tp` to the
  other saved area); `sample` cannot profile startup (attaching early stalls
  the process in dyld) - use `xctrace record --template 'Time Profiler'
  --launch` and export the `time-profile` table — with the caveat that
  `--launch` resolves the app by name through LaunchServices and started
  the INSTALLED game instead of the given build-tree executable (2026-10-06,
  even with a distinct bundle id on the copy); check the process path in
  the trace, or launch with `open` and attach.

## Section meshing (2026-09-26)

Per-section build cost (`BuildSectionMeshFromCache`, saved 32-chunk view,
Game Mode) 0.69-0.80 ms -> 0.44 ms; mesh output verified identical with
`OBEY_MESH_HASH=<file>` (a sorted content hash of every section built,
written at exit; translucent indices excluded - they are camera-sorted; one
section of the benchmark world differs run to run on its own).

- **Biome lookups.** MC's blend (`calculateBlockTint`, radius 2 = 25
  samples) called the fuzzy zoom (`BiomeZoom::NoiseQuartAt`, 8 corner hashes)
  for every sample of every tinted face: 26% of meshing. `Mesher::
  ResolveBiome` caches per build over the section plus the widest blend
  margin (stamped by generation, reset by both fill functions);
  `OBEY_NO_MESH_BIOME_CACHE=1` = off. -33%.
- **Blended tint per block** (MC `BlockTintCache`): the packed colour per
  (channel, block) for the section's own blocks, so a grass block or a leaf
  blends once, not per face. `OBEY_NO_MESH_TINT_CACHE=1` = off. ~ -5%.
- **Per-state lookups.** `BlockStates::FromIndex/Count/Base` and
  `BlockState::Block/Index` are inline over arrays mirrored at `Init`
  (`BlockStates::detail`) - they were out-of-line calls through a
  function-local static, 7% of a build by themselves. `ProcessBlock` reads the
  water flag `DeriveWaterCache` already computed, and resolves models through
  a per-state pointer table (`Mesher::ModelFor`) instead of a state -> name ->
  string-hash lookup per block; the table resets when
  `BlockModelRegistry::Generation()` moves (any load, clear, registration or
  blockstate load). ~ -7% together.
- **Buried blocks** skip the model walk: all six neighbours opaque and every
  face of the state's model naming a cullface (`Mesher::AllFacesCull`, a
  per-state table reset with `ModelFor`'s) means every face would be dropped
  at `ShouldCullFace`, so `ProcessBlock` returns after the fluid half. Not
  with portal faces in the section. `OBEY_NO_BURIED_SKIP=1` = off; check with
  `OBEY_MESH_HASH`.
- **One mesher per worker thread** (`ClientWorkerPool::BuildSectionMesh`,
  `thread_local`): a mesher per job cleared its caches, allocated its fluid
  builder, std::functions and biome/tint caches, and started the per-state
  model table empty, so every state's first use in every section went back
  to the registry's name lookup. Job 611-657 -> 585-637 us (join, -3-4%),
  identical meshes. `OBEY_MESHER_PER_JOB=1` = a mesher per job again.
- Profile after these (Time Profiler, teleport into saved land, inclusive
  share of a build): model walk 78% (face emission 44%), biome tint blend
  13% (zoom lookups the per-build cache misses 8.6%), AO shade lookups 8%,
  greedy flush 8%, face light 7%, AO 6.5%, fluids 5.5%. Outside the build
  a job spends ~7% filling the light cache and ~5% the block cache.
- What is left: `ProcessBlock`'s own work (face loop, culling, per-face
  layer), AO and face light (~15%), the fluid builder (~12%).
- **Upload permits** (`MeshUploadPermits`, PlatformMain): uploads drain once
  a frame, so the pool caps meshing at permits x fps. The 128 floor (7,680/s
  at 60 fps) was the limit - a leave-capture warm-up sat at a flat 768 per
  100 ms; 192 still was (191 jobs started a frame, workers 83% busy at 0.37 ms
  a section, ~225 a frame of capacity on 5 workers). Floor now 256 on 8+
  threads, just above the workers; the per-frame upload (~2 ms at 190) follows
  what the workers deliver, not the permit count.

## Last-world panorama capture (2026-09-26)

Six 90° faces from the eye, drawn and read back before the frame's own view
(PlatformMain `LeaveCapture`); under the pause menu while the world is
paused (the prepass), else after Save and Quit / Quit Game. Rules that were
each a measured bug:

- **Warm-up waits on the faces themselves.** `RecordViewForScheduler`
  returns a face's sections still dirty or building (never-built sections of
  a column without all neighbours excluded: the scheduler will not mesh
  them). Caught up = all six recorded since the warm-up began and at zero —
  the global mesh queue is not the test (at 32 chunks it never empties, and
  early on it is empty only because nothing was scheduled yet).
- **Faces are seeded at the eye** (`SetPortalViewSeed`): the occlusion
  flood's set (the main view's cached slot, the same section), not every
  section in a 90° frustum — unseeded, each face took in ~4,000 cave and
  underground sections nobody can see.
- **Only the sections a face could see at the warm-up's start are waited
  for** (`faceWaitFor`). Meshing a section opens the flood past it (MC:
  UNCOMPILED blocks visibility), so waiting on everything chased cave
  interiors layer by layer for ~2 s even after the player had looked all
  around.
- **A recorded view wakes the scheduler** (`WakeMeshScheduler`): a face is a
  candidate for one pass only, and the idle backoff was skipping most of
  them.
- **Capture**: a row of tiles per frame side by side (2 on a 16:9 window),
  read back pipelined (Vulkan keeps up to 4 read-backs in flight, taken two
  frames later): 24 tiles in ~14 frames. The Vulkan take swaps BGRA to RGBA
  a word at a time over the fork-join pool (a row's take 8.4 -> 4.3 ms of
  the frame). PNGs: libdeflate as stb's deflate, six faces encoded in
  parallel (~3 s -> ~0.2 s; Quit Game waits for them).
- **Tried and dropped: a whole face a frame, off-screen.** Each face drawn
  into a face-sized render target, read back whole and blitted into its
  texture on the GPU: 7 frames instead of 13 (capture 231 -> 153-196 ms),
  but a whole 3090² face is ~20 ms of GPU, so the outro pan ran at ~40 fps
  for its duration instead of 60 — visibly worse for ~50 ms. (Traps met on
  the way: MoltenVK builds a pipeline for a format-converting
  vkCmdBlitImage on first use, a 366 ms stall; a fresh 38 MB vector per
  take is mostly page faults.)
- **Measured, Save and Quit 0.5 s after pausing** (`--ui-test-leave 0.5`,
  bench world, Vulkan, Game Mode, click -> first title frame): 128 permits,
  no buried skip ~0.62 s; + buried skip 0.62 s (permit-bound either way);
  + 256 permits 0.35 s (warm-up after the click 353 -> 86 ms; without the
  buried skip it is 153 ms).
- Measured (Quit Game, 32 chunks): after looking around 0.72 s from the
  leave to the files written (warm-up 13 frames); without, 3.3 s (the
  warm-up meshes everything behind the player). `--ui-test-leave <sec>`
  times Save and Quit through the pause menu.

### Joining back: the world holds still until the hand-over

Rejoining the world the panorama shows (PlatformMain `JoinTransition`), the
picture stays up while the world streams in and eases to the leave look;
the world must not move before the hand-over frame or the picture "jumps".

- **Host (integrated server).** `IntegratedServerConfig::holdOwnerJoin` arms
  `IntegratedServer`'s owner join hold before the server thread starts: the
  owner's session counts as paused, so the server never leaves the pause it
  starts in (no pause transition, no "Saving and pausing" save). Chunk
  loading, generation, sending, light and entity tracking (mobs, items,
  orbs) run as under any pause; the simulation, time, weather and player
  ticks do not. The hand-over frame calls `ReleaseOwnerJoinHold()`; the next
  tick is the ordinary resume ("Resuming game" + time sync). A guest who
  joins meanwhile is not held. Timeout: 60 s server-side.
- **Client.** The hold is part of the world pause
  (`SetWorldPaused((menu || joinTransition.active) && serverPaused)`), so it
  freezes what the pause menu freezes: mobs/items/orbs (partial tick 1),
  animate ticks, block-entity ticks, the sky clock and its partial, cloud
  partial, particles (zero step), texture animations, sounds, the player's
  physics and move packets.
- **Joining someone else's server** nothing can pause their world: only the
  local player is held (physics and move packets) until the hand-over; the
  world under the panorama keeps running as before.
- **The panorama is drawn last in the level** (after the gun-portal pass,
  particles, clouds, weather, the Improved Transparency resolve and a shader
  pack's EndScene; the hand, post effects and HUD are off while joining).
  There is no crossfade — the hand-over is a cut at a matched view — so
  nothing of the live world may be drawn over the picture before it.
- Nothing waits forever: LevelLoadTracker's 30 s escape hatch ends the load
  wait, the ease hands over at the latest 3 s after it began, and the
  server lets itself go after 60 s.

## Camera-relative rendering (2026-09-07)

The world past a few hundred thousand blocks used to jitter (float32 resolves
0.03 blocks at 300,000; every section origin, entity matrix and vertex was an
absolute world float). MC never hands the GPU an absolute coordinate: its
LevelRenderer translates everything by `position - cameraPos` in double. The
engine does the same through ONE origin per view — read
`src/client/renderer/core/RenderOrigin.hpp` first, then:

- `Render::RenderOrigin()` = the drawn camera's INTEGER block position
  (`Camera::PrepareRender()` in the frame loop right before the main view;
  a portal's far camera gets `renderOrigin = RenderOriginFor(pos)` and
  `ActivateRenderOrigin()` around its draws, the enclosing camera's restored
  after). `Camera::position` is `dvec3`; `GetViewMatrix()` is RENDER space
  (eye = position − origin, a small float); `GetWorldViewMatrix()` is for
  frusta only. `viewOverride` is render-space relative to `renderOrigin`.
- RULE: every float position handed to the GPU is `Render::ToRender(world)`
  (subtract in double, then narrow): model translations, per-frame vertices
  (players, orbs, particles, gizmos, end portal, fill preview), instance
  translations, `uCameraPos`. `cameraPos` PARAMETERS stay world floats for
  distance culling. Culling is world space: `Frustum::FromMatrix(proj *
  Render::WorldViewFromRenderView(view))` inside a renderer,
  `camera.GetWorldViewMatrix()` in PlatformMain. `ChunkRenderer`
  warns every 5 s if a camera's `renderOrigin` differs from the global one.
- Terrain: the section-origin table is `ivec4` now and the vertex shaders
  compute `vec3(uOrigins[slot].xyz - uRenderOrigin) + rel` in INTEGER
  arithmetic (GL plain uniform `ivec3 uRenderOrigin`; Vulkan appended
  `ivec4 uRenderOrigin` at 368 in `CommonUBO`, declared in
  `terrain_vk.vert` — recompile the .spv). `RenderBackend::SetUniformIVec3`
  exists for it. Fog compares render-space `fragWorldPos` with a
  render-space `uCameraPos`, so the fragment shaders are unchanged.
- Portal views: a far view is `V_far_render = V_near_render · T(−R_near) ·
  M⁻¹ · T(R_far)` built in double (`Render::FarRenderView`) — never
  `view * mat4(Minv)`. Clip planes are stored in DOUBLE
  (`ChunkRenderer::SetPortalClipPlane(dvec4)`, `HalfSpace::AsClipPlane()`
  is dvec4); `PortalClipPlane()` / `PortalEntityClipPlane()` return the
  render-space vec4 for uniforms, `PortalClipPlaneWorld()` is what you
  save/restore or compose with a transform. `SurfaceDepthMargin` keys its
  float-error term on the distance now, not the world coordinate.
- Player: `PlayerPhysics::position` is `dvec3` and `CollidesAt(const
  AABBd&)` tests in a local integer frame (exact anywhere), the float
  overload wraps it. `PlayerMoveC2S` and `PlayerUpdateS2C` carry doubles
  (the S2C wire changed from float — client and server ship together).
- Also double now: `RemotePlayer` positions, the portal-gun ghost /
  crossing prediction (`GhostInfo` is dmat4 + dvec4 planes,
  `TeleportPrediction::newFeet`), the portal particles' stored positions,
  the pending gun projectile, `Raycast::CastRay` (origin + march +
  `hitPoint`), `BlockHitResult::hitPoint`, and the server's interaction
  eye. `Frustum` planes stay float (culling slack only).

## Tracy: reading captures, macOS Game Mode, and sampling

### Reading a capture without the GUI: `tools/tracy_report.py`

`tools/tracy_report.py capture.tracy` prints the whole picture: frame pacing
from the FrameMark frames (fps, 1%-low, p50..p99, frames over 16.7/33 ms), the
main thread's per-frame budget by EXACT self time with p95 per frame and the
worst frames explained, the server tick (TPS, work per tick minus
`Server.Park`, per-tick budget, heaviest ticks), every thread pool's busy share
and top zones, and every plot. Give it two captures (`gl.tracy vk.tracy`) for an
A/B: frame stats, per-zone self time per frame sorted by the difference, server,
thread busy and plot means side by side. A capture made with `--replay` is cut
to the replayed path (`Replay/Time` >= 0) so two runs of one recording compare
like for like; `--window all|START:END` overrides. "Busy" leaves out the waits
(`Present`, `Vk.FenceWait`, `Vk.Acquire`, `Server.Park`).

Under it, `tools/tracy_tools.py` runs `tracy-export` (source in
`tools/tracy_export/`, built against Tracy's own loader) and caches its output
per capture in `~/Library/Caches/obeycraft-tracy/exports/`. The export is every
table the capture holds — threads with real names, frames, `frame_zones` (per
frame × thread × zone: count, total, self, max — the per-frame breakdown
without the raw zones), `zone_stats` (per zone × thread, percentiles), plots,
messages, GPU zones, locks, memory, context switches, callstack samples (+
folded stacks) and `info.json` — as quoted CSV. The raw per-thread zone tables
(`zones/<tid>_<thread>.csv`, ~80 bytes per zone: a minute of play is 40 M zones,
3 GB) are only written when a script asks (`analyze_trace.py --zone`).

`tools/build_tracy_tools.sh` builds the tools at the tracy `GIT_TAG` in
`CMakeLists.txt` (Tracy checkout + CPM deps cached in
`~/Library/Caches/obeycraft-tracy`), checks every tool reports that version and
stamps `tools/tracy/VERSION`; the scripts call it when the stamp and the pin
differ. Upgrading Tracy is therefore: bump `GIT_TAG`, delete
`cmake-build-*/_deps/tracy-*`, install the matching viewer. A capture saved by
a newer viewer than the pin fails with an explicit "bump GIT_TAG" message
(exit 2 from `tracy-export`); stock `tracy-csvexport` threw
`tracy::UnsupportedVersion` there.

### macOS Game Mode when profiling (2026-09-04)

Game Mode needs `LSApplicationCategoryType` = a games category in Info.plist
(done) and the window in a NATIVE full-screen Space (`ToggleFullscreen` does
that on macOS; F11 needs Fn on a Mac keyboard, Control-Command-F also works).
It never engaged for a bundle run from the build tree while it sat under
`~/Desktop`: `gamepolicyd` identifies a game by reading the bundle's
Info.plist, has no Full Disk Access, and `~/Desktop` is TCC-gated, so the
daemon filed the app as "not a game" (seen in its log: no `Found game`
line). The tree moved to `~/Developer` on 2026-09-25 (in-place launches
untested since); the copy stays the proven path. `tools/play.sh tracy --vulkan` copies
the built bundle to `~/Applications` and launches it there through
LaunchServices with no debugger — Tracy still connects. `--force-game-mode`
forces the policy on via Xcode's `gamepolicyctl` (no menu-bar icon then; the
daemon log still says `Game mode status is now on`). Read the daemon with
`/usr/bin/log show --predicate 'process == "gamepolicyd"' --info --debug` —
`log` alone is a zsh builtin and silently returns nothing. Never `open -n` a
bundle for this: the UUID-suffixed identity skips the game lookup entirely.

### Which tool for which symptom

| Question | Tool |
|---|---|
| Why was *this* frame slow? | Timeline + Zone info (use **Parent zones** — "Zone trace" was removed in 0.14) |
| Is this zone usually slow, or was that a fluke? | **Find Zone** — histogram + distribution across all instances |
| Where does time go overall? | **Statistics**, **Flame graph** (zooms/pans as of 0.14) |
| Did my fix work? | **Compare Traces** — load before/after; much improved in 0.14 |
| What is this counter doing? | Plots |
| Which source line / instruction? | **Sampling** + **Symbol view** |
| Who is blocked on whom? | **Wait stacks** |
| CPU or GPU bound? | GPU zones; also `g_enableGpuPassTimers` (ChunkRenderer.cpp), but it costs ~2.3 ms/call on Apple's GL driver — read it, then turn it off |
| Which part of the session was this? | **Sections** (new in 0.14) — mark world-load vs steady-state, then range-limit stats |

### Per-stage GPU attribution: `OBEY_SKIP` (A/B by subtraction)

Apple's TBDR runs the whole frame as one render pass, so per-pass GPU timestamps
are meaningless there. `src/client/renderer/core/DevRenderSkip.hpp` skips whole
stages instead and you measure the difference in frame time:

```bash
OBEY_SKIP=cutout OBEY_SKIP_PERIOD=4 ./MyVoxelGame --vulkan --world "OG with Structs" \
    --exec "/tp @s 0.5 75 0.5 0 15" --exec-delay 6 --quit-after 58
```

Tokens: `sky opaque cutout translucent players items mobs
blockentities particles clouds helditem hud`. **Always use `OBEY_SKIP_PERIOD`**
(toggles on/off every N s and logs `[DevSkip] phase=`) and pair it with the
per-second `[Harness]` fps lines: this MacBook Air is fanless and throttles within
~40 s of sustained GPU load, so two separate runs are NOT comparable (identical
baselines measured 153 vs 128 fps back-to-back). Pin the camera with `/tp` — the
player otherwise gets shoved by mobs and the saved position drifts run to run.

### GPU profiling: Metal System Trace via `tools/play.sh --gpu-trace` (2026-09-04, MoltenVK era)

Written against the Vulkan backend through MoltenVK. The native Metal
backend's own profiling story (Xcode capture, HUD encoder timing, the
engine's per-encoder timestamps) is in "Apple's tools on this backend"
under the Metal backend section; what follows still applies to the trace
mechanics.

`tools/play.sh tracy --gpu-trace[=SECONDS] --vulkan` attaches Instruments'
Metal System Trace (xctrace) to the running game 20 s after launch and
writes `gpu-<time>.trace` in the project root; `tools/gpu_report.py
<trace> [--tracy capture.tracy]` reads it (docstring lists every section).
Run a Tracy capture at the same time: the report cross-correlates frame
times to align the two clocks and regresses GPU vertex time against the
`Sections/Visible`, `Geom/Vertices` and `Geom/Indices` plots.

- **Keep recordings at 45 s or less.** A 90 s Metal System Trace produced a
  3.8 GB raw capture that xctrace failed to finalise ("Unexpected internal
  error", bundle left with only the .atrc attachment, unreadable).
- **The trace is not readable when the game quits.** xctrace keeps running
  for minutes at 100% CPU turning the raw capture into the document;
  `xctrace export` on it meanwhile fails with `Document Missing Template
  Error`. Wait for `pgrep xctrace` to be empty / "gpu trace saved".
- **Only the Vertex and Fragment channels are GPU execution.** The Compute
  channel shows one 'GL/CL' interval per MoltenVK command buffer, tens of
  ms long and stacked dozens deep: that is commit-to-completion lifetime,
  not work (the driver's state track shows two channels active meanwhile).
- **GPU counters only through a GUI-saved template.** The stock template
  records one useless counter (`RT Unit Active`) in a 3 GB table; adding
  `--instrument 'Metal GPU Counters'` fails with "Selected counter profile
  is not supported on target device". A template saved from the Instruments
  GUI with Counter Set = Performance Limiters (the only live set this M4
  offers; `gpu-limiters.tracetemplate` in the project root) works with
  `--gpu-template=`: 68 counters every ~22-34 us, ~90 MB per second of
  recording — keep those runs to ~10 s (a 50 s one failed to save). The
  shader-core counters are stamped on a GPU clock (ns, fixed offset from
  trace time); align them by matching VS/FS occupancy > 0 against the
  Vertex/Fragment channels. Record it ATTACHED only: with the limiter
  counter set, every all-process or launched recording crashed Instruments'
  GPUPlugin (xctrace four times, Instruments.app once, 2026-10-06, Xcode
  26.2) while the same recordings with counters off survived. The shader
  timeline is its own template (`gpu-shaders.tracetemplate`, counters off):
  it records in any mode but has produced no samples in attached,
  all-process or launched recordings — per-line costs come from the Xcode
  capture instead.
- **xctrace leaves a raw `instruments*.ktrace` (0.5-2 GB) in `$TMPDIR` per
  recording and never deletes it** — 36 GB of them filled the disk on
  2026-09-25. `play.sh` now deletes the ones its recording made.
- The same trace carries a 1 kHz **time profile with real symbols** (the
  `time-profile` table); gpu_report.py prints the main thread's leaf
  functions and where samples land inside `--cpu-fn` functions. This is
  the tool for "what is inside this Tracy zone" — Tracy's own sampling on
  macOS resolves nothing (see ghost zones above).
- Measured 2026-09-04 (M4, 32 chunks, flying): GPU 95% active,
  **vertex-bound** — vertex 3.6 ms vs fragment 1.3 ms per frame, and vertex
  time = 1.0 ms + 0.97 µs × sections drawn (r = 0.94). Portal views were
  13% of sections; the main view's 2,700 (up to 10,000) sections are the
  cost. Recording itself costs ~5-10% CPU (`__kdebug_trace64` inside
  vkQueueSubmit) — do not compare absolute CPU numbers against an
  untraced run.

### Face-direction groups (2026-09-04)

Opaque and cutout index buffers are laid out in seven groups by quad
facing (`SectionMesh.hpp`: kFacingGroupOrder, QuadFacing,
GroupIndicesByFacing, run at the end of every section build), and
`ChunkRenderer::RenderLayerPass` draws only the groups that can face the
eye given the section's bounds — Sodium's chunk face culling. It removes
exactly the triangles back-face culling would discard, so it is
pixel-identical; it is skipped when a layer's `enableBackFaceCulling` is
off, and `OBEY_NO_FACE_CULL=1` is the A/B kill switch. Every emitter of an
opaque/cutout quad MUST push a facing (`GenerateQuad` does; fluid and
greedy emitters push explicitly) — a mismatch is caught by
GroupIndicesByFacing and files the whole layer under Any (drawn as
before), never a hole. **Two-sided plant quads** (2026-09-04): a
zero-thickness element with both faces on its thin axis (cross plants,
seagrass planes) whose two faces match exactly (`Mesher::EmitTwoSided`:
same corners, colours, sprite; back mapping = u-mirrored, v-mirrored or
identical within the face's own uv extent, sub-rect uvs included; normal
axis-aligned or 45°) becomes ONE quad with `TerrainVertex::kTwoSidedFlag`,
its four vertices indexed with both windings (12 indices, two Any
facings) so culling keeps the side facing the camera; the fragment
shaders apply the back mapping (`fragAux`, the colour's alpha byte) when
the camera is behind the quad's stored front normal (6-bit code in `u`),
which also holds in mirrored portal views. A first version used a
separate cull-off group: the pipeline switch and +28% sub-draws cost
0.7 ms of vkQueueSubmit — never do that again. `OBEY_NO_TWO_SIDED=1` is
the kill switch; the greedy debug view paints them violet; Geom/Vertices
counts such a quad as 8 vertices. The backends now implement
`SetCullInvert` by flipping the front-face rule rather than swapping the
cull mode (same triangles culled, `gl_FrontFacing` stays geometric). Translucent keeps its back-to-front order and has no
groups. Measured 2026-09-04 (untitled7): per pass 813 sections → 1,446 entries
after splits → 1,095 sub-draws; one entry per section would fuse to 377,
the slab floor is 16. So two thirds of the sub-draws are the splits, one
third upload-order fragmentation. Fixed-spot A/B the same day (`--env OBEY_NO_FACE_CULL=1` vs on, high
vantage, 4,400 sections): the groups removed 23% of drawn vertices and
saved NO GPU time (3.51 vs 3.49 ms vertex once the throttled run is
corrected by its fragment time) while costing 0.9 ms of render thread —
a sub-draw costs the GPU ~0.2 µs too. So a skipped group between two
visible runs is now BRIDGED (drawn, culled by the GPU as before groups
existed) when it is under `s_splitMinIndices` (900 indices = 150 quads;
`OBEY_SPLIT_MIN=<n>` to tune, 0 = always split). Sub-draw attribution (Tracy builds, `RenderLayerPass`): `Draws/Sections`
(sections with geometry this pass), `Draws/Entries` (after group splits),
`Draws/FusedNoSplit` (what one entry per section would fuse to),
`Draws/Slabs` (the floor) and `Draws/Merged` (actual). Merged −
FusedNoSplit is the cost of the splits, FusedNoSplit − Slabs the cost of
upload-order fragmentation. Side effects to know: `Geom/Vertices` now counts DRAWN vertices;
the F8 cull dump's "submitted" coverage check sees partial ranges as
expected; draw-merge gap bridging (dev checkbox, off) would re-draw skipped
groups if turned on.

### Section occlusion culling — tried and REVERTED (2026-09-04)

A CPU software-depth pass (solid-layer runs of visible columns as
occluder boxes, conservative outline fill, per-section test) culled 14%
of visible sections but cost ~1 ms per view on the render thread; the
CPU became the bottleneck and fps fell 233 → 199, so it was removed. The
measurement stands: with the camera above ground a third of drawn
sections are entirely below the surface (`OBEY_DUMP_VISIBLE=1` +
`tools/underground_share.py`, both kept). If this is revisited, the
notes in docs/immersive-portals.md "Step 3" list what the fill has to
get right (per-box outlines, not per-face; the buried rock the BFS never
lists is the occluder) and where the CPU time went; a worker-thread or
GPU-side design is the only way it pays.

### Mesh census: where the vertices come from (2026-09-04)

`OBEY_MESH_CENSUS=1` makes the mesh workers count every quad by block,
layer and merged/unmerged, and the main thread logs the table every 15 s
(`[MeshCensus]` lines in latest.log: share of all vertices, cumulative
share, removed % = faces the merger took out, dominant layer). Counts are what was BUILT during the
session, the superset of what is drawn. Off = one cached bool per quad.
Use it before any "fewer vertices" work: the 2026-09-04 traces put the GPU
at ~0.4 ns per vertex, so the table is the priority list.

### Culling / meshing debug tools (2026-08-30)

- **F+C in-world** toggles a detached fly camera: view/projection follow the fly
  camera, but ALL culling (terrain frustum, occlusion BFS, entity gating, mesh
  scheduling) stays frozen at the player's view — fly outside the frustum to
  watch sections/entities get culled. The player is frozen and rendered in
  third person; block interaction and gameplay keys are dead while detached.
  Chord caveat: pressing F first fires the offhand swap before the chord forms.
- **Render Controls → "Greedy Mesh View"** (or launch with `OBEY_GREEDY_DEBUG=1`)
  draws terrain as dark-grey solid faces with colored lines on top (remeshes with debug colors baked in): red =
  eligible-but-unmerged 1x1, yellow->green = merged rect by area (log scale to
  16x16), blue-gray = rule-ineligible (partial blocks, sub-rect or rotated
  UVs, translucent; AO never disqualifies since the face map). Panel shows a legend + since-launch merge totals; break blocks
  to watch grouping rebuild. `OBEY_NO_GREEDY=1` disables greedy meshing at mesh time for A/B.

### macOS-specific limits — important when diagnosing stalls

Tracy on macOS has **no context-switch capture**. It cannot tell you whether a thread was
*executing* or *descheduled*. Do not claim starvation from Tracy data alone — cross-check
CPU-usage plots and use `sample <pid> 10 1 -file <out>` (or Instruments) for the real
blocked stack.

**Apple system tracing (0.14, prototype)** — verified in `public/client/apple/TracyMach.cpp`:

- It is **sampling only** (`QueueType::CallstackSample` at 1000 Hz default). It does
  **not** emit context switches.
- `SysTraceStart` gates on `geteuid() == 0` — stock Tracy needs **the GAME** run as root
  (not the viewer). No entitlement or TCC permission exists to grant instead.
- **We patch that check out.** `cmake/PatchTracyAppleSampling.cmake`, wired as a
  `PATCH_COMMAND` on the Tracy `FetchContent_Declare`, so Apple sampling works with no
  `sudo`. Legitimate because upstream's own comment calls the privilege check
  *"technically unnecessary"* — it is user-mode self-sampling via `mach_task_self()`.
  The script is **idempotent** (PATCH_COMMAND re-runs on reconfigure) and warns rather
  than fails if a version bump moves the guard — so after a Tracy upgrade, check the
  configure output for that warning, or you silently lose sampling-without-root.
- **Why not just sudo:** running the game as root creates root-owned files in
  `~/Library/Application Support/obeycraft/` (saves, options.txt, worlds.json), which
  breaks every later normal run. If you ever do run under sudo, follow it with
  `sudo chown -R obey:staff` on that directory.
- **It is OFF by default** — `option(TRACY_APPLE_SAMPLING)` in CMakeLists, which when
  OFF puts `TRACY_NO_SYSTEM_TRACING` on the `TracyClient` target and compiles the whole
  path away. Turning it off does **not** need a re-fetch (the source stays patched, the
  code just compiles out). Safe to set on TracyClient alone, unlike `TRACY_ENABLE`: this
  macro is internal to `TracySysTrace.hpp` and is not macro-mismatch checked. There is no
  runtime off-switch on macOS — the Apple path reads no env var (`TRACY_NO_SAMPLING` is
  honoured only on the Linux path), so CMake is the only knob.
- Sampling suspends each thread per sample, which perturbs the timings being measured.
  **Never compare a sampled capture against an unsampled one.**
- 0.14 colours sample markers: **green** = your program, **blue** = external, **red** =
  kernel. Red inside a stalled zone means blocked in a syscall.

**Ghost zones — the trap that follows from enabling sampling.** Symptom: the timeline
fills with unnamed blocks whose tooltip reads *"👻 Ghost zone / Unknown frame: 0x…"* and
the instrumented zones appear to be gone. Nothing is broken; three facts compound:

1. The viewer's **"Draw ghost zones" option defaults to ON** (`TracyViewData.hpp`,
   `uint8_t ghostZones = true`). Before sampling worked on macOS it was inert, because
   `AreGhostZonesReady()` was always false — there was never any sample data.
2. `TracyTimelineItemThread.cpp` renders ghosts when
   `AreGhostZonesReady() && ( m_ghost || ( vd.ghostZones && thread->timeline.empty() ) )`.
   Ghosts **replace** instrumented zones — any thread with no zones of its own is now
   full of them, and a clickable 👻 icon appears immediately right of *every* thread
   label that has both. That is exactly where you click to expand a thread, so one stray
   click silently swaps a thread's real zones for ghosts (`m_ghost` is per-thread).
3. Frames read "Unknown frame" because macOS is `TRACY_HAS_CALLSTACK 4` → **`dladdr`**,
   which only resolves *exported* symbols. Our binary is not stripped (~121k symbols) but
   they are local, so most game frames never resolve.

**Fix:** Options → uncheck **👻 Draw ghost zones**, or click the 👻 beside the thread name
to flip that one thread back. To make ghost frames actually readable you would need
`-Wl,-export_dynamic`; not worth it — use `sample <pid>` or Instruments instead, which
read the real symbol table.

Also new in 0.14 and worth knowing: several sampling statistics were previously **wrong**
(inclusive counts double-counted inline functions; context-switch samples leaked into
per-symbol lists on load), so do not compare pre-0.14 traces against newer ones.

## Crash handler / diagnostics rationale

**Why this exists alongside Sentry:** the game only ever logged to stdout, which a
launcher-started build discards outright — macOS `open --args` gives the process no
terminal. So a player's "it just closed" came with nothing at all. Sentry is still the
better report when it arrives (symbolicated, aggregated), but it needs network, a live
crashpad process, and a crash of a kind crashpad claims.

**The log file is the primary artifact; the crash report is a bonus.** On macOS Sentry
runs crashpad out-of-process via Mach exception ports, which are delivered *before*
POSIX signals — so for a hard SIGSEGV crashpad can take the exception and the process
dies without our signal handler ever running. The log is written as the game runs, so
it survives regardless. The handler still earns its keep for `std::terminate` (an
uncaught C++ exception is not a Mach exception, so this always fires) and for signals
crashpad doesn't claim.

**`--- log closed cleanly ---` is the tell.** `Log::CloseLogFile` writes it on the
normal exit path only, so a log ending without it means the process died — which is the
one question a crash report can't answer if it never ran.

Implementation notes that are easy to break:
- `CrashHandler.cpp` preallocates the report path, banner and scratch buffer at install
  time and uses only `open`/`write`/`close` + `backtrace_symbols_fd` in the handler.
  `malloc`, `snprintf` and stdio are **not** async-signal-safe, and a crash inside the
  allocator (likely, since heap corruption often *is* the crash) would deadlock on the
  way to writing the report. `backtrace_symbols` allocates; the `_fd` variant does not.
- `Log`'s ring buffer is a fixed `char[512][256]` with an atomic write index for the
  same reason — a `std::deque` behind a mutex is unreadable from a signal handler.
- Install order matters: `InstallCrashHandler` runs **after** `sentry_init`, and chains
  to the previously-installed handler, so Sentry still reports.
- Test with `--crash-test` (SIGSEGV after 3s); it logs the expected report path first.

## Post effects (MC PostChain)

`client/renderer/post/PostChain` runs MC's `post_effect/*.json` chains (vanilla files verbatim
under `assets/post_effect/`, resource-pack overridable) on both backends. Today they drive the
spectated-entity views (creeper / spider / invert, `GameRenderer.checkEntityPostEffect`).

- **Order:** after the world, the hand and `EntityOutline::Composite`, before the GUI — MC's
  `applyPostEffects` spot. On Vulkan the frame copy and every target pass interrupt the frame's
  render pass, so, like the outline, it only runs once nothing in the world needs depth.
- **`minecraft:main`:** reading it copies the frame (`RenderBackend::CopyFramebufferToRenderTarget`:
  GL `glBlitFramebuffer`, VK `vkCmdCopyImage` from the swapchain image); writing it draws onto the frame.
- **Shaders:** `shaders/post_<name>.{vert,frag}` + `_vk` twins, portal pipeline layout on Vulkan
  (sampler "In" = slot 0, the other input = slot 1). The config uniform blocks are packed std140-style
  into sixteen floats (`uPostParams`); inputs not marked `bilinear` are snapped to texel centres in
  the shader, so targets need only a linear sampler (changing a VK sampler mid-session drains the GPU).
- **Frame overlap:** every target exists twice and consecutive runs alternate between the two sets.
- **Shader packs:** skipped (one log line) while a pack is active — OptiFine's behaviour: the pack's
  composite/final programs own post-processing.

## Improved Transparency (MC 26.3 wavelet OIT)

`client/renderer/post/ImprovedTransparency` is MC 26.3's Video Settings "Improved Transparency":
wavelet order-independent transparency (`LevelRenderer.executeOit`, `client/renderer/oit/*`,
`shaders/include/oit*.glsl`, `core/oit_composite.fsh`, `core/oit_depth_bounds_cull.fsh`). It is OFF
by default. Only the option itself or the Fabulous graphics preset turns it on.

- **Off:** nothing exists or runs. There are no targets, variants, passes or backend modes, and the
  frame depth is not preserved. `Defer` returns false, so every feature draws where, how and with
  the shaders it always did. The base `.spv` files are byte-identical to before (the OIT code is
  all `#ifdef OIT`).
- **Features:** translucent terrain, dropped items, mob particles, weather and clouds. When
  active, each is handed to `Defer` at its usual place and drawn at `Resolve` (after the weather,
  before the hand) once per stage. Translucent terrain is captured and replayed
  (`ChunkRenderer::CaptureDeferredTranslucentForOit`), because portal views rebuild the renderer
  in between.
- **Stages:**
  - DEPTH_BOUNDS: RGBA32F, MAX-blended, cleared to (-FLT_MAX,0,0,0).
  - The cull: the bounds are blitted into the culled target, then `oit_depth_bounds_cull` pulls
    the far bound in to the nearest opaque OIT surface and writes its depth.
  - TRANSMITTANCE: 2×RGBA16F, eight rank-2 coefficients, added.
  - ACCUMULATE: RGBA16F, premultiplied colour × transmittance, added.
  - The composite: premultiplied over the frame, depth ALWAYS with write.
- **Clouds** run each stage in passes of their own, against their own depth. The frame's depth is
  blitted in during their depth bounds, which write depth, as MC `CloudRenderer.renderOit` does.
- **Shader variants:** the participating shaders (`terrain_solid`, `block`, `mob_particle`,
  `clouds`) get per-stage defines through `SetOitStage`.
  - GL splices `shaders/oit_lib.glsl` in at `#pragma oit_library`.
  - Vulkan loads `<name>_oit_{db,tr,ac}_vk.frag.spv`, which CMake builds with glslc `-D`. Those use
    layout types 3/4: portal/block plus set 6 (bounds, coefficients, params UBO).
- **Depth convention:** depth is not reversed here. The bounds target's b/a channels therefore
  store 1 − depth ("nearness"), so MAX still selects the nearest. MC's `ProjMat[2][2]`/`[3][2]`
  arrive as `OitProjParams.xy`.
- **Vulkan frame depth:** it is normally discarded at every render-target interruption. While the
  option is on (`SetFrameDepthPreserved`), the frame uses the keep-depth pass variants, and the
  per-slot depth images get SAMPLED usage and sample views. The OIT passes attach the current
  slot's frame depth.
- **Frame overlap:** Vulkan keeps one OIT target set per frame slot, and the next frame never
  writes what the previous one reads. GL has a single set.
- **Memory (on):** per set per pixel:
  - 16 B depth bounds + 16 B culled bounds + 2×8 B coefficients + 8 B accumulation.
  - 8 B for the clouds' depth (D32S8). GL instead has a 4 B frame-depth copy and 4 B D24S8 cloud
    depth.
  - Total: 64 B/px. At 2560×1600 that is ≈ 262 MB per set: ~524 MB on Vulkan (two slots) and
    ~262 MB on GL. The 26.1 sorting-layer chain it replaced used ~590 / ~200 MB.
  - Everything is freed the frame the option goes off.
- **Not covered:** portal views, the panorama capture and shader packs draw their translucency the
  classic way. A shader pack disables the option, as OptiFine does. The portal gun's additive
  sparks also stay classic.

## Render Resolution (scaled scene, 2026-10-02)

Video Settings → Render Resolution (`renderScale`, 25–200 % in 5 % steps, default 100; every
graphics preset writes 100) draws only the level at a scaled size. It is MC's `mainTarget`, made
to exist only when needed:

- **100 % costs nothing.** The level draws straight into the window as before; the only new work
  is one branch in `BeginFrame`. Any other scale: `RenderBackend::RequestScaledScene(w, h)` before
  `BeginFrame` moves "the frame" offscreen, `ResolveScaledScene()` after the post effects stretches
  it over the window (bilinear both ways — a smooth upscale below 100 %, a 2×2 box filter at
  200 %) and returns the frame to the window, depth cleared, for the HUD, screens, chat, F3 and
  ImGui, which stay at the window's resolution. `EndFrame` resolves a scene nobody resolved.
- **Backends.** Everything that names "the frame" follows the scene: GL keeps the frame FBO in
  `m_defaultFbo` (the scene FBO: RGBA8 + a D24S8 *texture*, so the depth copies work as on the
  window); Vulkan routes the frame passes, framebuffer, extent, colour image, final layout and depth
  through `Frame*()` helpers. Vulkan keeps one colour + depth image per frame slot (frames
  overlap), its scene passes are the frame passes ending in `COLOR_ATTACHMENT_OPTIMAL`
  (compatible, so no pipeline is rebuilt). The resolve (2026-10-05) is a full-screen triangle
  sampling the scene (`scene_upscale_vk.*`) that opens `m_renderPassAfterSceneDraw` (colour
  DONT_CARE, depth cleared) for the GUI; the old `vkCmdBlitImage` + LOADed
  `m_renderPassAfterScene` wrote the whole image and read it back — two full-resolution trips
  through memory: 50 % went 381 → 458 fps in one run (`OBEY_SKIP=sceneblit` A/B), 75 % from
  ~255 to ~378. The blit remains the fallback and the above-100 % path. A size change rebuilds every slot after a
  device idle; going back to 100 % frees them. Improved Transparency rebuilds its targets on the
  scene's depth (`m_oitOnScene`).
- **PlatformMain** swaps `width`/`height` to the scene's size from `glfwGetFramebufferSize` to the
  resolve, so every viewport, projection, OIT/outline/post/shader-pack target and read-back of the
  level follows; renderers that asked GLFW themselves read `Render::WorldFramebuffer` instead (block
  outline — whose line width is scaled so it keeps its on-screen weight — chunk aspect, volumetric
  beam depth copy, portal refraction snapshot).
- **Native on purpose:** the leave capture's panorama faces (read back from the frame at the
  window's size) and the join transition (the title panorama must match the title screen pixel
  for pixel). There is no screenshot key; a panorama is always native.
- Mouse picking is a ray from the camera, independent of any pixel grid.

## Sky Block (2026-10-02)

`sky_block` is a full opaque cube for culling, occlusion and light (it breaks, sounds, drops and
blocks light like tinted glass) whose faces show the active sky in screen space, the way the end
portal shows its starfield. The chunk mesher emits nothing for it (`Mesher::ProcessBlock`); its
model is still a cube, so neighbours cull against it and the occlusion graph treats it as solid.
`Render::SkyBlockRenderer` draws its exposed faces **depth-only** from `ChunkRenderer::RenderAll`,
after the view's sky and before its first terrain pass: the colour buffer keeps exactly what the
sky pass drew (vanilla sky, skybox, OptiFine layers, the End's sky, the Nether's fog colour), and
the depth keeps everything behind the face from drawing over it. Because every view goes through
`RenderAll` — the main view, immersive-portal far sides (stencil mask and clip plane applied), gun
portal views, panorama faces — each shows its own dimension's sky. Blocks are found through
`ClientChunk::skyBlocks` and `ClientChunkManager::SkyBlockChunks()` (kept exact on arrival,
`SetBlockLocal`, retention restore, unload and clear), so a world without sky blocks pays one
`empty()` per view. Clouds and weather behind a sky block are hidden, as by any opaque block.
