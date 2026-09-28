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

A Metal System Trace with hardware counters (template
`gpu-counters.tracetemplate`, Performance Limiters) showed every Vulkan frame
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
  --launch` and export the `time-profile` table.

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
It will NEVER engage for a bundle run from this build tree: `gamepolicyd`
identifies a game by reading the bundle's Info.plist, has no Full Disk Access,
and `~/Desktop` is TCC-gated, so the daemon files the app as "not a game"
(seen in its log: no `Found game` line). `tools/play.sh tracy --vulkan` copies
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

### GPU profiling: Metal System Trace via `tools/play.sh --gpu-trace` (2026-09-04)

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
- **No GPU counters from the command line.** The stock template records one
  useless counter (`RT Unit Active`) in a 3 GB table; adding `--instrument
  'Metal GPU Counters'` fails with "Selected counter profile is not
  supported on target device". The GUI-saved `gpu-counters.tracetemplate`
  (project root: Performance Limiters + shader timeline) works with
  `--gpu-template=`: 68 counters every ~22-34 us, ~90 MB per second of
  recording — keep those runs to ~10 s (a 50 s one failed to save). The
  shader-core counters are stamped on a GPU clock (ns, fixed offset from
  trace time); align them by matching VS/FS occupancy > 0 against the
  Vertex/Fragment channels.
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
