# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Network Info

- **Mac Public IP:** 108.35.220.113 (was 74.105.150.36; residential IP — re-check with `curl https://api.ipify.org` and update `src/common/core/FriendsServiceConfig.hpp` when it rotates)

## Git Policy

- **Never add Co-Authored-By lines** to commit messages. No Claude attribution in commits.
- Do not build unless asked. Do not push unless asked.

## Build Policy

**Do NOT build after making changes.** Instead, say "try building with new changes and if there are any errors let me know." Only run the build command when the user reports errors and asks you to build to diagnose them.

## Build Commands

The project uses CMake with Ninja (CLion is the primary IDE).

### Quick Build Commands
```bash
# Build (debug, CLion-style)
cmake --build cmake-build-debug --target MyVoxelGame -j$(sysctl -n hw.ncpu)

# Build (release)
cmake --build cmake-build-release --target MyVoxelGame -j$(sysctl -n hw.ncpu)

# Configure from scratch (if needed)
cmake -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Distribution Build
For distributing to users (uploads dSYMs to Sentry, builds universal binary):
- CLion profile: add `-DJALIN=ON` to CMake options
- Environment: `VULKAN_SDK=/Users/obey/VulkanSDK/1.4.341.1/macOS`
- Setting `VULKAN_SDK` enables universal binary (arm64 + x86_64) with Vulkan support

### Vulkan SDK
- Location: `/Users/obey/VulkanSDK/1.4.341.1/macOS`
- Homebrew MoltenVK is arm64-only; use the Vulkan SDK for universal builds

### Running the Game
```bash
# macOS (app bundle)
./build/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame

# Linux/Windows
./build/bin/MyVoxelGame
```

### Command-line arguments

All parsed in `src/platform/PlatformMain.cpp:Run()`. The launcher (`src/launcher/LauncherApp.cpp`) builds these from the user's settings + Play/Join buttons and passes them to the game at launch (via `system("open ... --args ...")` on macOS, `ShellExecuteA` on Windows). Anything you add here, you also need a `build*Arg` lambda + UI control in the launcher.

| Flag | Value | Meaning |
|---|---|---|
| `--vulkan` | (none) | Use the Vulkan render backend instead of OpenGL. Launcher: "Use Vulkan" checkbox in settings. |
| `--crash-test` | (none) | Trigger a deliberate crash at startup — for testing the Sentry pipeline. Not exposed in the launcher. |
| `--server` | `host[:port]` | Connect to a remote dedicated server instead of running the integrated one. Defaults to port 25565. Launcher: "Join Server" dialog (IP + port). |
| `--name` | `<string>` | Player name. Empty/missing → server auto-assigns "PlayerN" based on connection ID. Launcher: "Username" field. |
| `--color` | `<slug>` | Player stick-figure colour. Slug is one of `default`, `red`, `orange`, `yellow`, `blue`, `purple`, `pink`, `white`, `black`, `brown` (case-insensitive). Empty/missing/`default` → neon green (`#00FF3C`). Launcher: swatch grid in settings. |
| `--env` | `NAME=VALUE` | Sets an environment variable at startup, before anything reads it — how the `OBEY_*` switches (`OBEY_NO_FACE_CULL`, `OBEY_NO_TWO_SIDED`, `OBEY_MESH_CENSUS`, ...) get through `tools/play.sh`, whose LaunchServices launch drops the environment. Repeatable. Not in the launcher. |
| `--record` | `<name>` | Dev harness: record the player's pose (feet, yaw/pitch, dimension, sneak/sprint/fly/scale/speed) every frame from level-load until the session ends, to `<obeycraft>/recordings/<name>.rec`. In-game: `/record <name>`, `/record stop`. |
| `--replay` | `<name>` | Dev harness: drive the player along that recording BY TIME (same views at the same seconds at any fps), then quit (a `--quit-after` wins). `[Replay] t=` lines each second next to the `[Harness]` fps lines, `Replay/Time` Tracy plot. In-game: `/replay <name> [hold]`, `/replay stop` (no quit). |
| `--replay-hold` | `<sec>` | Seconds the replay parks the player on the first pose before the clock starts, so chunk streaming at t=0 is the same every run (default 8). |
| `--session` | `<token>` | Friends-service session token from a launcher login. Missing → guest (Friends UI disabled). Launcher: added automatically when logged in. |
| `--account-id` | `<n>` | Friends-service account id paired with `--session`. |
| `--friends-service` | `host[:port]` | Overrides the friends-service address (defaults in `src/common/core/FriendsServiceConfig.hpp`). Launcher: forwarded from the `friends_service` key in `launcher.json`. |

**Player color flow** (added 2026-05): launcher persists slug to `launcher.json` `"player_color"` → passes `--color <slug>` on launch → `PlatformMain` parses via `Game::ParsePlayerColorName` (`src/common/entity/PlayerColors.{hpp,cpp}`) → stored on `ClientPlayer.color` → forwarded to network as a uint8_t via `NetworkClient::SetPlayerColor` → tail-appended byte after username in the `LoginStart` packet → server's `LoginPacketListener::onLoginStart` calls `m_connection.SetPlayerColor(packet.colorId)` → IntegratedServer's `OnPlayerJoined` reads `connection->GetPlayerColor()` onto the new `ServerPlayer.colorId` → broadcast in both `PlayerInfoS2C ADD` paths (existing-players-to-new-client and new-player-to-all) → client writes onto `RemotePlayer.color` → `PlayerRenderer` looks up the RGB via `Game::LookupPlayerColor` and passes it to `BuildStickFigure`. Same for the local inventory preview via `PlayerInventoryPreview`. Default (id=0) is the historical neon green so old clients/servers stay compatible.

To add a new color, append one row to `Game::kPlayerColorTable` in `src/common/entity/PlayerColors.hpp` and bump `PlayerColorId::Count`. The launcher swatch grid auto-includes it.

**Friend hosting without port-forwarding** (added 2026-07): when a player hosts, the game tries to open its port automatically via UPnP IGD (`src/client/network/UPnPPortMapper` — hand-rolled SSDP+SOAP on Asio, no new dependency; runs on a worker thread, failure is non-fatal) and reports the WAN address in its presence. The service then TCP-probes that address to *verify* reachability (`probe_reachable`; a host claiming success isn't enough — double-NAT/ISP filtering). `join_info` returns `mode:"direct"` with the host address when the probe passed, otherwise `mode:"relay"` with a ticket: the service pushes `relay_open` to the host, whose FriendsClient dials OUT to the service and hands the resulting socket to `NetworkServer::AdoptConnection` (native-handle transfer between io_contexts); the joiner sets `NetworkClient::SetConnectPreamble` with a `relay_attach` line and connects to the service, which splices the two streams. Relay traffic rides the SAME port as the control protocol (first-line sniff), so only 25570 needs forwarding on the service machine. `--allow-private-hosts` treats private addresses as directly joinable (LAN-only setups and local testing).

**Friends system flow** (added 2026-07): self-hosted backend `tools/friends_server/friends_service.py` (Python 3 stdlib, port 25570, sqlite; run it next to the game server and port-forward TCP 25570). One port, two protocols sniffed by first byte: HTTP POST `/api` (launcher: signup/login/logout/check_name/rename via `src/launcher/net/FriendsServiceClient`) and persistent NDJSON (game: `src/client/network/FriendsClient` — own io thread, app lifetime, hello(token) → server pushes `roster`/`invite` events; friend-op targets travel in `"friend"`, `"id"` is the correlation field). Launcher login stores `session_token`/`account_id`/`account_name` in `launcher.json`, forces the username to the account name, shows a debounced availability checkmark (rename commits via the service; friendships key on account id so renames propagate). Launch passes `--session`/`--account-id` → PlatformMain constructs `Client::g_friendsClient` (null = guest). Presence transitions live in PlatformMain only: Menu (title) / Hosting(worldName, 25565) / Playing(addr). Friends UI: `screens/FriendsScreen` (title screen's old Realms slot + pause menu). Join posts a `Multiplayer` TitleAction; from in-game the pause branch stashes it in `pendingSessionAction` and the outer session loop auto-joins without showing the title. Transport is plaintext by design (personal scale); the hosting machine's launcher should set `friends_service` to `127.0.0.1` (NAT hairpin).

### MSVC compatibility fixes (Windows build)
The game uses GCC/Clang-specific features that need MSVC equivalents:

## Resource packs (2026-09-05)

Minecraft resource packs work from Options → Resource Packs (`PackSelectionScreen`,
a port of MC's PackSelectionScreen / TransferableSelectionList / PackSelectionModel).
Folders and `.zip` files go in `<gamedir>/resourcepacks/`; ids are `file/<name>`,
the selection is `resourcePacks:[...]` / `incompatibleResourcePacks:[...]` in
options.txt exactly as MC writes them. `src/client/resource/ResourcePacks` is
PackRepository + FolderRepositorySource + PackCompatibility + PackFormat
(game format 76.0, `pack_format` allowed up to 64, `min_format`/`max_format`
above that). Rules that are easy to break:

- **Lookup order** is MC's: the repository list is lowest priority first
  (vanilla at index 0), the screen shows it reversed, a lookup walks from the
  top pack down (`Resources::FindOverride`, `Core::Assets::ListFiles`).
- **Namespace fold**: this engine's `assets/X` is a pack's `assets/minecraft/X`.
  Every loader must open files through `PlatformMain::GetAssetPath` (single
  files) or `Core::Assets::ListFiles` / `Locate` (directory scans) — never a
  bare `"assets/..."` string or a raw `directory_iterator` on an asset dir.
  A texture's `.mcmeta` is looked up from the pack that supplied the texture
  upward, never below it (`LocateFromLayer`).
- **Overlays**: `pack.mcmeta` `overlays.entries` whose range holds the game format
  become extra layers above the pack's base, later entries on top
  (OverlayMetadataSection / CompositePackResources); layer ids are `file/<name>/<dir>`.
- **Zips are extracted once** to `resourcepacks/.extracted/<name>/` (stamp =
  size:mtime); MC reads zips directly, everything here is path-based.
- **Reload** (`PlatformMain::ReloadResources`, on `APPLY_RESOURCE_PACKS`):
  quiesces the mesh workers, rebuilds the block atlas (`AtlasBuilder::
  ReleaseGpuResources` + `BuildFromJSON`, which now clears its tables) and
  colormaps, GUI atlas, font, crosshair, menu sheets, particle sprites, sky
  and clouds, then remeshes every section. Renderer texture caches invalidate
  themselves through `Resources::CacheStale(generation)` — any NEW cache of
  loaded textures must do the same. Block models, blockstates, item
  definitions and lang are read at startup only (shared registries), so those
  parts of a pack apply at the next launch; the screen says so.
- **Init order**: `Resources::Initialize` runs at the top of
  `InitializeGameSystems`, before any asset is read, with the pack lists
  peeked straight from options.txt (`GameSettings::PeekStringFromDisk`) —
  options.txt itself is loaded much later.
- An enabled pack with `optifine/sky` supplies the "Vanilla" sky's layers
  (OptiFine behaviour); the World Settings skybox choice still overrides it.
- **Per-world selection**: a pack change made while IN one of our worlds is
  written to that world's `data/obeycraft.json` (`resourcePacks` /
  `incompatibleResourcePacks`) and applied when the world is opened; the
  session end restores the global list from options.txt. Outside a world the
  screen writes options.txt.
- **Pick block (P)** is server-side (`PickItemC2S` → `PlayerSession::HandlePickItem`,
  a port of ServerGamePacketListenerImpl.tryPickItem + Inventory.addAndPickItem /
  pickSlot / getSuitableHotbarSlot), CREATIVE ONLY by this game's rule: an
  existing stack is selected or swapped into the hotbar; a new item goes to
  the first empty hotbar slot (the selected one when empty); with a full hotbar
  it takes the selected slot and the held stack moves to a free inventory slot,
  or is DROPPED when the inventory is full (MC would overwrite it). The server
  answers with `SetHeldSlotS2C`; no client prediction. New players start with an
  EMPTY inventory (the dev starter hotbar is gone).
- `/locate structure|biome <id|#tag>` (`LocateCommand` + `level/LocateFinder`):
  ports findNearestMapStructure (rings from the precomputed positions, random
  spread walked ring by ring to radius 100, a candidate accepted when
  `isStructureChunk` AND the library's `Structures::generate` yields a valid
  start — the same code a STRUCTURE_STARTS chunk runs, seed-deterministic) and
  findClosestBiome3d (6400 blocks, 32/64 lattice, square spiral). The
  reported Y for structures AND biomes is the terrain surface of the found
  column (`LocateSurfaceY`: loaded chunk's MOTION_BLOCKING heightmap, else
  the generator's noise column; under the nether roof, the first 2-high gap
  below it), not MC's "~" / sampled Y, so click-to-teleport lands on the
  ground; the structure distance stays horizontal. `/locate poi`
  answers that there is no POI index. Structure tags are read from
  data/…/tags/worldgen/structure, biome tags through the library.
- `/dimension <overworld|nether|end>` (alias `/dim`) travels through
  `PortalTravel::TravelToDimension`: the End-portal rule to/from the End, the
  nether-portal rule (x8 scaling, existing-portal search, else the placement
  a new portal would get via `PortalForcer::FindPortalPlacement` — nothing is
  built) between the Overworld and the Nether — not vanilla's plain
  `/execute in ... run tp`.

## Baby mob remodel (MC 26.1–26.2) + Baby Models world setting (2026-09-05)

Two decompiles live side by side: `minecraft_code/` (26.1 Snapshot 1, data
version 4764 — what every generator and port reads) and `minecraft_code2/`
(26.3 Pre-Release 2, data version 5018, FernFlower via IntelliJ's
java-decompiler.jar with `-dgs=1`). The ONLY thing read from the second tree
is the baby remodel, which by 26.2 covers EVERY baby (each `<LAYER>_BABY`
row is a dedicated Baby*Model / SniffletModel class on its own `_baby`
sheet) plus the adult rabbit:

- `tools/gen_entity_models.py` `remodel_meshes()` DERIVES the rows — 41 of
  them — from MC2's LayerDefinitions for every mob that can be a baby here
  (`<slug>_baby_new`, plus `rabbit_new`, `sheep_wool_baby_new`,
  `drowned_outer_baby_new`; nautilus only under the remodel). The class that
  builds each row is what `gen_setup_anim.py` compiles setupAnim from, under
  the same slug; `gen_entity_animations.py` pulls the rabbit and baby
  axolotl definitions from MC2. The classic `<slug>_baby` rows are untouched.
- `tools/gen_baby_textures.py` → `GeneratedBabyTextures.cpp`: the adult→baby
  sheet table, derived from the `*_baby.png` files in assets (RENAMED covers
  the sheets this asset set still carries under pre-26 names — cat/tabby,
  turtle/big_sea_turtle, fox/snow_fox, llama/creamy, dolphin.png…, and the
  snifflet). Copy new `_baby` sheets in and rerun it.
- `Render::BabyModelLook` (MobRenderer.hpp): `New` picks the `_new` meshes,
  the `_baby` sheets (every texture pick in MobRenderer goes through the
  `MobTex` lambda, INCLUDING the type's default sheet) and the 26.1 adult
  rabbit; `Classic` is the old look. Persisted per world as `babyModels`
  ("new"/"classic") in worlds.json + `data/obeycraft.json`, applied at
  session start next to the skybox, editable in Options → World Settings.
  A change bumps a generation the renderer watches to drop its model cache.
- NOT optional (gameplay follows 26.3 either way): the per-mob baby boxes
  and eye heights (`babyWidth/Height` + `babyEyeHeight` columns — the zombie
  family is 0.49x0.98 now, hoglin/zoglin 0.75x0.85, camel 0.6 scale, fox and
  armadillo 0.6, turtle 0.3, happy ghast 0.2375, chicken 0.3x0.4, rabbit
  0.24x0.4), read on `Entity::BaseBbWidth/Height` for EVERY baby (the
  zombie/piglin flag babies had the adult box before); the rabbit's 15-tick
  jump clock + `SetupAnimationStates` (MobAnim `Hop`/`IdleHeadTilt`); the
  baby axolotl's seven exclusive keyframe states (`Axolotl::
  TickBabyAnimations`, MobAnim `Swim`…`PlayDead`).
- Compiler/runner vocabulary added: `state.<x>AnimationState.isStarted()`
  → `StateRef::AnimStarted` (slot in the node's arg) and as a CLIP guard
  (`AnimGuard::AnimStarted` + `GenClip::guardSlot`), sheep `HeadEatPos` /
  `HeadEatAngle`. Part names may be CamelCase (BabyPigModel's `HeadMain`).
  `MobAnim` slots are APPENDED, never reordered.
- Per-model back-face culling, MC's way: `EntityModel::CullBackFaces` /
  `GenModel::cull` (generator `model_culls`, read from the class's
  `super(root, RenderTypes::…)` against a per-tree table — the no-cull
  cutout is EntityModel's default and entity models are NOT closed, so
  culling stays OFF except for the bat, arrow, trident, bee stinger and
  26.3's baby turtle; entityTranslucent (allay/vex/breeze/piglins) is NOT
  culled in either tree; 26.3 renamed entityCutoutNoCull→entityCutout and
  entityCutout→entityCutoutCull). `Batch::cull` flips the pipeline's cull
  mode per batch. That is what stops the bat's zero-thickness ears fighting
  their own back faces. On the NO-cull models a zero-thickness box keeps
  BOTH coplanar faces (the nautilus is two such planes and shows a different
  face per side through the later one's transparent texels) but emits the
  earlier one on the LATER face's vertex order (`ModelPart::Build(...,
  culled)`): same triangles → same depth → the later face wins
  deterministically, vanilla's look with no z-fighting. Never collapse the
  pair to one face — that lost the nautilus's shell. The New baby sheep's
  wool (and the baby drowned's outer layer, whose head/hat cubes MC
  declares at the body's own deformation) rides
  the same mesh as its body (MC submits it with order 1); it is inflated
  0.02 px.
- Baby SOUNDS are not ported: the engine has no mob sound events at all
  (`Game::PlaySound` is a stub), so there is nothing to attach them to.
- Dolphin and squid/glow squid are `AgeableMob`s now (MC's
  AgeableWaterCreature: 10% / 5% pack baby rolls in FinalizeSpawn, baby
  boxes 0.65 scale / 0.5x0.5) and the villager is a `GenericAgeableMob`
  (MC AbstractVillager extends AgeableMob) — so all four have babies, both
  looks. The camel HUSK has no baby mesh in MC at all (CamelHuskRenderer is
  a plain MobRenderer on the adult model), so it keeps the classic
  transform baby in both looks. A remodel row whose MC2 setupAnim did not
  compile (the squids) runs the adult's program (`MakeRemodel`).
- Placing a player outright (player-data load, /tp, portals) goes through
  `ServerPlayer::teleport`, never `setPosition` — the latter is the
  move-packet setter with the 600-block "moved too fast" gate, and it
  silently kept anyone who logged out far from spawn AT spawn on rejoin
  (MC Entity.load → setPos has no such gate).
- `/tick freeze` and `/tick rate` PERSIST per world (`tickFrozen` /
  `tickRate` in the sidecar, `IntegratedServer::PersistTickState` on every
  change, re-applied at world open before any client joins). Vanilla does
  not save its tick state; this is deliberate.
- `/tick freeze` works like vanilla's TickRateManager.isEntityFrozen:
  players tick (attacks, bow charge, item use — both sides), mobs and
  projectiles do not, and the entity TRACKER still runs every tick
  (`SyncMobsToClients`, split out of TickMobs; `MobManager::AbsorbSpawned`
  drains AddFreshEntity's deferred spawns) — so a hit lands at once (then
  the mob's hurt cooldown sits until the thaw, as in MC), arrows shot under
  a freeze appear immediately and all launch on unfreeze.
- `/tick freeze` keeps the client's pick candidates fresh
  (`ClientMobManager::RefreshPickCandidates` in PlatformMain's frozen
  branch) — Tick() is what rebuilt them, so mobs spawned during a freeze
  could not be aimed at or pick-blocked (P).
- Debug: `/spawnall [adults|babies|both] [spacing]` lines every mob up in a
  12-wide grid CENTRED on the sender (a single row outran tracking range):
  columns 2*spacing apart, babies 1.5*spacing in front of their adults, all
  NoAI and facing north so a tick step changes nothing; the log names each
  type's row/column.
- The wither charges up (blue armour, 220 ticks, spawn explosion) ONLY from
  the soul-sand ritual (`CheckWitherSpawn` → `MakeInvulnerable`); /summon
  and the egg give a full-health fighting wither, as vanilla. The client's
  baby flag now goes through the virtual `Mob::SetBaby` for every mob (the
  piglin/zoglin flag babies used to arrive adult-sized). Breeze draws MC's
  wind and eyes layers as their OWN generated meshes: BreezeModel cuts
  its three LayerDefinitions from one base mesh with `PartDefinition.
  retainPartsAndChildren` ({head, rods} on breeze.png, {wind_body} on the
  128x128 breeze_wind.png, {eyes} on breeze_eyes.png), and `run_mesh` now
  applies that family (`retainPartsAndChildren` / `retainExactParts` /
  `clearRecursively`, statement or `.apply(lambda)` form) — before it did,
  every breeze row carried all twelve parts, so the body sheet wore the
  rings' 128-scale texOffs and the wind sheet the head ("textures where
  they shouldn't be"). The wind batch is MC's BREEZE_WIND pipeline:
  blended, depth write on, no per-face shade (NO_CARDINAL_LIGHTING —
  vertex colour flattened), u scrolled by ageInTicks*0.02 on a REPEAT-
  wrapped sheet (`LoadTexture(path, repeatWrap)`), and its quads sorted
  far to near per draw (`SortQuadsBackToFront`, MC's sortOnUpload — the
  sheet has no opaque texel, so the nested rings' order is what shows
  through). The eyes batch blends (entityTranslucentEmissive). The snow
  golem draws its carved pumpkin (SnowGolemHeadLayer's transform over
  `AppendUnitBlockFaces`).
- `gen_entity_types.py` overlays 26.3's EntityTypes.java (`SRC2`, keyed
  `register(EntityTypeIds.X, …)`) on the 26.1 rows for every type both
  trees have: bee 0.55x0.5, rabbit 0.49x0.6 eye 0.59, hoglin notInPeaceful.
  Types only 26.3 registers are NOT added by the overlay — the one such mob,
  the SULFUR CUBE (AbstractCubeMob sibling of slime/magma cube, sulfur-caves
  biome, data-driven archetypes on new attributes), is the only 26.3 mob
  this engine lacks (audit 2026-09-05: every other 26.3 non-MISC type has a
  wire id, a server class via MakeMobForLoad/MakeGenericMob and a mesh).
- **Sulfur cube (2026-09-05)** — 26.3's only new mob, ported STANDALONE
  (no sulfur-caves biome, sulfur blocks or bucket: egg, /summon, /spawnall).
  `src/common/entity/mobs/SulfurCube.{hpp,cpp}` derives from this engine's
  Slime (= 26.3's AbstractCubeMob) the way MagmaCube does, plus the
  AgeableMob half (size 1 = baby, size 2 = adult, health 4·size, splits into
  two babies, grows on slime balls, never attacks). Its behaviour is the
  BLOCK IT SWALLOWS: `tools/gen_sulfur_cube.py` bakes MC's data-driven
  archetype registry (`data/minecraft/sulfur_cube_archetype/*.json` + the
  `sulfur_cube_archetype/<name>` item tags, resolved through
  `data/minecraft/tags/item/*.json`) into `GeneratedSulfurCubeArchetypes` —
  334 swallowable block items as SLUGS resolved once via
  `RecipeManager::ItemFromSlug`. Swallowing swaps five attribute modifiers
  (`ModifierId::SulfurCube*`) on the three 26.3 attributes added for it —
  `AirDragModifier`, `Bounciness`, `FrictionModifier` (default 1/0/1, so
  nobody else changes) — which `LivingEntity::Travel` now honours MC's way
  (`ComputeModifiedFriction`, `OmnidirectionalAirMover`, and the entity half
  of `restituteMovementAfterCollisions` after `Move()`; block bounce stays
  with the block code). A laden cube drops all goals (Clear + RegisterGoals
  on release), its move/look controls idle (yaw snapped to 0/180), it is
  shoved by walking players (`PlayerPush`, known speed = the player view's
  position delta), takes no melee/projectile/fall damage but MC's aim-angle
  knockback (`KnockbackWithBody`, routed through the stashed Hurt attacker
  because `Knockback(power,dx,dz)` has none), and TNT inside lights on
  fire/explosion damage, redstone (`HasNeighborSignal`), flint and steel or
  a fire charge; the fuse explodes through `QueueExplosion`. Shears eject
  the block (100-tick pickup timer). Item pickup needed two new level
  hooks, `GetItemEntitiesInBox` / `TakeFromItemEntity` (item entities are
  NOT Entities here — they live in ItemEntityManager). Wire: size in the
  variant byte, MAX_FUSE in the anim byte (0 = unlit, else fuse+1; the
  client counts down itself), and the swallowed block as
  `Mob::GetCarriedBlockRaw` — AddEntity's data int plus an APPENDED
  `blockStateRaw` on SetEntityData, tracked for change like the variant.
  Rendering: MC's chain folded into `state.modelScale` + the new
  `state.modelOffset` (EntityMatrix's translate after the scales), the
  translucent shell blended + sorted, `sulfur_cube_inner` at order −1, and
  the swallowed block meshed through `BlockCubeEntityRenderer::
  BuildStateMesh` (public for this) under the cube's OWN pose (entityMatrix
  · 16 · rotX180 · 0.5-if-baby · translate(-0.5,-0.518,-0.5)) on the block
  atlas — it must turn with the body while the look control settles the
  cube onto 0/180; the first cut drew it axis-aligned through the block
  proxy pass and looked wrong mid-turn. Spawn size is MC's setSpawnSize
  (2, baby 1 — `FinalizeSpawn` skips the slime's 1/2/4 roll). BUCKET:
  `SULFUR_CUBE_BUCKET` data component (id 14: content slug, age, age lock,
  NoAI — MC's sulfur_cube_content + bucket_entity_data) written by
  `SaveToBucket` when an empty bucket right-clicks the cube (held stack
  becomes the bucket, as the water bucket does), read back by
  `Use_SulfurCubeBucket` (MobBucketItem with Fluids.EMPTY: POV clip, spawn
  in the clicked face's cell via `SpawnMobFromItem`'s new `configure`
  callback → `LoadFromBucket`), persisted under `obeycraft:sulfur_cube_
  bucket` in the item NBT, and shown as "Contains: <block>" in the tooltip
  (SulfurCubeContent.addToTooltip). Filling ANY bucket goes through MC's
  ItemUtils.createFilledResult now (`IUsePlayer::CreateFilledResult`,
  `EntityLevel::CreateFilledResult` for mob interactions; ServerPlayer
  implements it, overflow drops via PlayerSession::FlushPendingDrops or the
  bridge): one bucket of a stack fills, the rest stays, creative keeps the
  stack and gains the filled bucket once — `held = filled` used to turn 16
  buckets into one. Placing the cube uses MC's Fluid.NONE clip: water is
  looked THROUGH, so over shallow water the cube lands on the bottom cell
  and over water deeper than reach nothing happens, exactly as vanilla. Generator
  gap fills: `gen_mob_defs` reads MC2 classes/renderers/attributes
  setdefault-style, `gen_items` has `MC2_ITEMS` (the egg), `gen_entity_types`
  `MC2_ONLY`, `gen_entity_models` `MC2_ONLY_MESHES` + four explicit MC2 rows.
  Not ported: sounds/particles (no systems), the bucket, burning-arrow
  priming (projectile hits hand over the shooter, not the arrow).
- **26.3 blocks and items imported (2026-09-05)** — every block 26.3
  registers that this engine lacked and was not a rename (the `*_crop` and
  `redstone_dust` ids are 26.2 renames of blocks we have): the poplar wood
  set, cinnabar and sulfur stone families (slabs/stairs/walls included),
  golden dandelion, red shrub, shelf mushroom, straw bed, sulfur spike,
  potent sulfur — 56 `BLOCK_DEF` rows APPENDED at the end of BlockDefs.inc
  (a BlockID is the block item's ItemID: save/wire ids, never re-sort), and
  20 items (the 15 explorer maps, music_disc_bounce, poplar boats, the
  sulfur cube bucket) through `gen_items.py`'s `MC2_ITEMS`. Assets, block
  loot tables, recipes, lang keys and TAG ENTRIES came from the 26.3-pre-2
  jar via a union that never overwrote an existing file (custom blocks and
  items are untouched). The block generators gap-fill from
  `minecraft_code2` for slugs 26.1 lacks: `gen_block_hardness` (BLOCKS2,
  `Block(Item)Ids.X` keys, registerSlab/registerWall), `gen_block_shapes`
  and `gen_waterlogged` (MC2 registry pass + class dir fallback),
  `gen_block_states` (ALIAS_EXACT/ALIAS_SUFFIX to the 1.21.6 upstream rows,
  EXPLICIT for potent_sulfur's state and the shelf mushroom's age/facing —
  explicit lists must be name-sorted). Behaviour: the poplar family rides
  the suffix rules (doors, gates, signs, stairs…); potent sulfur, sulfur
  spikes, the shelf mushroom, straw bed, maps, boats and the bucket are
  plain blocks/items with no special behaviour yet.
- **Damage has a direct entity (2026-09-05)** — MC's DamageSource names
  both the shooter (causingEntity → this engine's `attacker`: aggro, kill
  credit) and the arrow (directEntity). `LivingEntity::HurtFrom(source,
  amount, causing, direct)` sets `HurtDirectEntity()` for the duration of
  the virtual Hurt; `Projectile::DealHitDamage` uses it, so knockback and
  the player's hurt tilt follow the arrow's FLIGHT (dealDefaultKnockback's
  calculateHorizontalHurtKnockbackDirection), not the shooter's position,
  and a burning arrow lights an explosive sulfur cube. Plain Hurt() still
  works for melee (both entities are the attacker).
- gen_setup_anim knows 26.3's idioms: bare `ModelPart var10000;` alias
  declarations, `state.getMainHandItemStack() == ItemStack.EMPTY` (folds
  to true — no held items), `state.swingAnimation` (= AttackTime),
  `HumanoidModel.ArmPose` ordinals. Before that the whole zombie-arm block
  of every 26.3 baby program was silently skipped — the arms lost their
  0.1 rad outward turn and sat coplanar with the body (the baby drowned's
  seam z-fight). `SETUP_ANIM_DUMP=<slug,...>` prints a row's skips.

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
- Translucent (`SubmitOrderedRuns`) cannot bridge (blend order), so it stays
  one sub-draw per section: ~0.55 ms per pass on GL.
- `Present` on GL (~1.9 ms) is the swap = GPU wait; Metal System Trace works on
  GL too (Apple GL runs on Metal) for the GPU side.

## Terrain vertex format (packed, 2026-09-04)

Chunk terrain uses the 16-byte `Render::TerrainVertex` (`src/client/renderer/
core/Vertex.hpp`), not the 24-byte `Vertex` every other renderer uses. Why:
the Metal System Trace showed the frame vertex-FETCH-bound (0.57 ns per
32-byte vertex, ~6.5 M vertices a frame), so the vertex was halved.

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

## Hardware scaling (low-end machines)

Every budget that depends on the machine reads `Core::HardwareProfile::Get()`
(`src/common/core/HardwareProfile.hpp`: logical/performance cores, RAM, Intel-vs-Apple
Silicon, and a Low/High tier). The rule for any new budget: **a fast machine must get
exactly the number it had; only weaker hardware scales down.** Consumers today:

- `Core::ThreadAllocator` — ≤6 logical cores use MC's `cores - 1` budget with no floors;
  7+ keep the tuned split (table in `docs/thread-model.md`).
- Mesh upload permits (`PlatformMain`) — `max(128, hw*8)` at 8+ threads, `max(32, hw*16)` below.
- `ClientChunkManager::ComputeRetainBudgetBytes` — `min(1.5 GB, RAM/10)`.
- `GameSettings` — first run on a Low-tier machine applies the Fast graphics preset;
  `retinaFramebuffer` defaults off on Intel Macs (a window-creation hint, read from
  disk before the window exists via `PeekRetinaFramebufferFromDisk`).

Graphics options follow MC's modern model: `graphicsPreset` (fast/fancy/custom) is a
one-shot macro in `GameSettings::ApplyGraphicsPreset`; the individual options
(`cutoutLeaves`, `ao`, `biomeBlendRadius`, `particles`, `mipmapLevels`,
`entityDistanceScaling`, `simulationDistance`, …) are what the engine reads, and each
setter flips the preset to custom. Mesh-time options travel to workers through
`Render::Mesher::SetMeshOptions` (one packed atomic + generation), never through
`g_gameSettings` directly. Simulation distance is per-client on the wire
(`ClientConfigC2S`, trailing field) and independent of view distance in `PlayerSession`.

## Profiling with Tracy

Tracy is the profiler of record for this project. Currently **v0.14.0** (client pinned in
CMakeLists via FetchContent). Everything below was verified against the actual
fetched source in `cmake-build-tracy/_deps/tracy-src`, not from memory — re-verify
against `NEWS` and the client sources after any version bump.

### Setup invariants (all three have bitten us)

1. **Client and viewer versions must match exactly.** Tracy checks a protocol
   version on connect; a mismatch gives a "Protocol mismatch" dialog and no data.
   The `GIT_TAG` in CMakeLists and the `tracy-profiler` app must move together.
2. **`TRACY_ENABLE` must be set as a CACHE var before `FetchContent_MakeAvailable`.**
   As of 0.14 Tracy's own default flipped to OFF
   (`set_option(TRACY_ENABLE "Enable profiling" OFF TracyClient)`). Setting it only via
   `target_compile_definitions` on `MyVoxelGame` compiles profiling OUT of the
   `TracyClient` library itself. 0.14 added macro-mismatch detection that makes this a
   link error rather than a silently dead profiler.
3. **A stale `libTracyClient.a` survives a `GIT_TAG` bump.** FetchContent re-fetches the
   source but will happily relink the old archive. If a version bump doesn't take:
   `rm -f cmake-build-tracy/_deps/tracy-build/libTracyClient.a`, or
   `rm -rf cmake-build-tracy/_deps/tracy-*` and reconfigure.

**Which build dir has Tracy:** `cmake-build-tracy` (and `cmake-build-debug`).
`cmake-build-release` and `cmake-build-universal` have **no Tracy at all** — a binary
from either will never connect. Check before debugging a "won't connect" report.

### Reading a capture without the GUI: `tools/tracy_report.py`

`tools/tracy_report.py capture.tracy` (or `--csv` on an existing
`tracy-csvexport -u` dump) prints the whole picture: thread map auto-classified
by dominant zone (main, server, mesh workers, chunk-load, terrain-gen,
occlusion BFS), main-thread frame budget + per-phase breakdown with EXACT self
times and the worst frames explained, server tick budget + per-tick breakdown +
worst ticks, and worker-pool throughput/utilisation. Needs `tracy-csvexport`
built from `cmake-build-tracy/_deps/tracy-src/csvexport` (`TRACY_CSVEXPORT`
env var, or it looks in the session scratchpad). `-f` in csvexport is a
SUBSTRING filter and `Vk.BeginFrame` (the GPU fence) sits INSIDE `Render`.

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
  supported on target device". For limiter counters save a template from
  the Instruments GUI with the counter set chosen and pass it with
  `--gpu-template=path.tracetemplate`. Until then, attribution is by
  regression against the Tracy plots and by `OBEY_SKIP` A/B.
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

## Architecture Overview

MyVoxelGame is a Minecraft-compatible voxel engine with a client-server architecture designed for both single-player (integrated server) and multiplayer support.

#### Minecraft Compatibility
- Loads existing Minecraft Java Edition worlds (1.18+)
- Supports Anvil region format (.mca files)
- Compatible block models and texture atlas system
- NBT data structure parsing

### Build Requirements
- CMake 3.19+
- C++20 compiler
- Platform: macOS (universal binary), Windows, Linux

### Dependencies
All external dependencies are vendored in `ext/`:
- GLFW (windowing)
- GLAD (OpenGL loader)
- GLM (math)
- ImGui (debug UI)
- zlib (compression)
- OpenAL (audio)
- STB Image (texture loading)
- nlohmann/json (via FetchContent)
- Boost.Asio (networking, header-only)

### Windows/MSVC portability (things Clang accepts and MSVC does not)

Code written on the Mac builds clean under Clang and then fails on Windows in
three recurring ways. Check these before blaming the Windows toolchain:

- **Transitive includes.** libc++ pulls in `<stdexcept>`, `<string>`, `<deque>`
  and friends through other headers; MSVC's STL does not. Include what you use.
- **Zero-length arrays.** `static const T k_foo[] = {};` is a Clang extension —
  MSVC gives C2466. The generators emit `nullptr` + count 0 instead (see
  `tools/gen_mob_loot.py`).
- **`class` vs `struct` in forward declarations.** MSVC mangles the two tags
  differently (`V` vs `U`), so a `class Foo;` forward declaration of a `struct
  Foo` links fine on Clang and gives LNK2019 on MSVC, pointing at a function
  whose definition plainly exists. Match the tag to the definition.

## ObeyCraft Launcher & Distribution

### Automatic Release System (Zero Manual Steps)

Building in the right configuration automatically handles everything:

| Build | What happens automatically |
|-------|--------------------------|
| Game **Debug** | Normal build, nothing extra |
| Game **Release** (no `-DJALIN=ON`) | Normal build, nothing extra |
| Game **Release** (`-DJALIN=ON`) | Version bumps, zips, uploads to GitHub, uploads debug symbols to Sentry |
| Launcher **Debug** | Normal build, nothing extra |
| Launcher **Release** (no `-DJALIN=ON`) | Version bumps, zips, uploads to GitHub |

**Just build in CLion and everything happens.** No scripts to run.

> **Important:** `-DJALIN=ON` is required for game GitHub uploads and Sentry symbol uploads. Without it, Release builds compile normally but don't publish anything. The launcher uploads on any Release build regardless of JALIN.

### How the Auto-Release Works

1. **Version bump**: Build number files (`tools/game_build_number`, `tools/launcher_build_number`) store an integer that auto-increments on each qualifying build. A generated header (`GameBuildVersion.hpp` / `LauncherBuildVersion.hpp`) is created with the version string.
2. **Compile**: The binary picks up the new version from the generated header.
3. **Post-build**: The app is zipped (stays in the build dir, e.g., `cmake-build-universal/bin/`) and uploaded to GitHub via `gh` CLI. Non-fatal — if offline or `gh` isn't authenticated, the build still succeeds.

Version format: `{major}.{minor}.{build_number}` — game uses `0.1.X`, launcher uses `1.0.X`.

### Player-side diagnostics (log file + crash reports)

Two artifacts land in the obeycraft directory (`~/Library/Application Support/obeycraft`
on macOS). Ask a player for these first — they exist whether or not Sentry got through.

| Path | What it is |
|---|---|
| `logs/latest.log` | Current session. Every `Log::` line, timestamped, no ANSI codes |
| `logs/<date>.log` | Previous sessions, newest 5 kept |
| `crash-reports/crash-<date>.txt` | Version, signal + fault address, backtrace, last ~512 log lines |

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

### Sentry Debug Symbols (Game Only)

On Universal Release builds (with `-DJALIN=ON`, which is the default for `cmake-build-universal`):
1. `dsymutil` generates a `.dSYM` bundle from the game binary
2. The binary is stripped of debug symbols (smaller download for users)
3. `sentry-cli debug-files upload` sends the dSYM to Sentry for crash symbolication
4. Requires `sentry-cli` to be installed (`brew install getsentry/tools/sentry-cli`)
5. Sentry release string uses the auto-incremented version: `myvoxelgame@0.1.X`

### GitHub Release Tag Convention

- **Game releases**: `v0.1.1`, `v0.1.2`, ... (auto-created on Universal Release build)
- **Launcher releases**: `launcher-v1.0.1`, `launcher-v1.0.2`, ... (auto-created on Release build)
- Both coexist in the same repo: `ObiJello/MyVoxelGame-Download`
- The launcher knows which is which by the `launcher-v` prefix

### How the Launcher Update System Works

- **GitHub repo**: `ObiJello/MyVoxelGame-Download`
- **Game updates**: Launcher queries `/releases` and picks the latest tag NOT prefixed with `launcher-v`
- **Launcher self-updates**: Launcher queries `/releases` and picks the latest `launcher-v*` tag, compares against its compiled-in version, silently downloads/installs, shows "Restart to update launcher"
- **Version tracking**: `~/Library/Application Support/obeycraft/launcher.json`
- **Game install location**: `~/Library/Application Support/obeycraft/game/`
- **Asset name matching**: Zip filenames must contain a platform tag (`macos-universal`, `macos-arm64`, `windows-x64`) for the launcher to pick the right one

### Creating a DMG for First-Time Distribution (macOS)
```bash
./tools/create_dmg.sh    # Creates ~/Downloads/ObeyCraftLauncher.dmg
```
This is only needed once to distribute the launcher to new users. After that, the launcher updates itself.

### Creating a Windows Installer
```powershell
# Requires Inno Setup 6 installed at %LOCALAPPDATA%\Programs\Inno Setup 6\
powershell -ExecutionPolicy Bypass -File tools/create_installer.ps1
# Output: %USERPROFILE%\Downloads\ObeyCraftLauncherInstaller.exe
```
Run this whenever you need to do a fresh Windows install (e.g. to bypass a broken auto-update). The `.iss` script is at `tools/create_installer.iss`.

### Manual Release Scripts (Optional)
These still exist if you ever need manual control:
```bash
./tools/release_launcher.sh          # Bump patch, rebuild, upload
./tools/release_launcher.sh minor    # Bump minor
./tools/release_game.sh              # Same for game
```

### Key Files
- **Launcher source**: `src/launcher/` — config in `LauncherConfig.hpp`
- **Build numbers**: `tools/game_build_number`, `tools/launcher_build_number`
- **Auto-release scripts**: `tools/bump_version.sh`, `tools/auto_release.sh`, `tools/update_plist_version.sh`
- **Launcher app icon**: `assets/launcher/logo.png` (converted to `AppIcon.icns` via `iconutil`)
- **DMG builder**: `tools/create_dmg.sh`