# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Network Info

- **Mac Public IP:** 108.35.220.113 (was 74.105.150.36; residential IP — re-check with `curl https://api.ipify.org` and update `src/common/core/FriendsServiceConfig.hpp` when it rotates)

## Git Policy

- **Never add Co-Authored-By lines** to commit messages. No Claude attribution in commits.
- Do not build unless asked. Do not push unless asked.

## Build Policy

**Do NOT build after making changes.** Instead, say "try building with new changes and if there are any errors let me know and I will try building to see them." Only run the build command when the user reports errors and asks you to build to diagnose them.

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
| `--session` | `<token>` | Friends-service session token from a launcher login. Missing → guest (Friends UI disabled). Launcher: added automatically when logged in. |
| `--account-id` | `<n>` | Friends-service account id paired with `--session`. |
| `--friends-service` | `host[:port]` | Overrides the friends-service address (defaults in `src/common/core/FriendsServiceConfig.hpp`). Launcher: forwarded from the `friends_service` key in `launcher.json`. |

**Player color flow** (added 2026-05): launcher persists slug to `launcher.json` `"player_color"` → passes `--color <slug>` on launch → `PlatformMain` parses via `Game::ParsePlayerColorName` (`src/common/entity/PlayerColors.{hpp,cpp}`) → stored on `ClientPlayer.color` → forwarded to network as a uint8_t via `NetworkClient::SetPlayerColor` → tail-appended byte after username in the `LoginStart` packet → server's `LoginPacketListener::onLoginStart` calls `m_connection.SetPlayerColor(packet.colorId)` → IntegratedServer's `OnPlayerJoined` reads `connection->GetPlayerColor()` onto the new `ServerPlayer.colorId` → broadcast in both `PlayerInfoS2C ADD` paths (existing-players-to-new-client and new-player-to-all) → client writes onto `RemotePlayer.color` → `PlayerRenderer` looks up the RGB via `Game::LookupPlayerColor` and passes it to `BuildStickFigure`. Same for the local inventory preview via `PlayerInventoryPreview`. Default (id=0) is the historical neon green so old clients/servers stay compatible.

To add a new color, append one row to `Game::kPlayerColorTable` in `src/common/entity/PlayerColors.hpp` and bump `PlayerColorId::Count`. The launcher swatch grid auto-includes it.

**Friend hosting without port-forwarding** (added 2026-07): when a player hosts, the game tries to open its port automatically via UPnP IGD (`src/client/network/UPnPPortMapper` — hand-rolled SSDP+SOAP on Asio, no new dependency; runs on a worker thread, failure is non-fatal) and reports the WAN address in its presence. The service then TCP-probes that address to *verify* reachability (`probe_reachable`; a host claiming success isn't enough — double-NAT/ISP filtering). `join_info` returns `mode:"direct"` with the host address when the probe passed, otherwise `mode:"relay"` with a ticket: the service pushes `relay_open` to the host, whose FriendsClient dials OUT to the service and hands the resulting socket to `NetworkServer::AdoptConnection` (native-handle transfer between io_contexts); the joiner sets `NetworkClient::SetConnectPreamble` with a `relay_attach` line and connects to the service, which splices the two streams. Relay traffic rides the SAME port as the control protocol (first-line sniff), so only 25570 needs forwarding on the service machine. `--allow-private-hosts` treats private addresses as directly joinable (LAN-only setups and local testing).

**Friends system flow** (added 2026-07): self-hosted backend `tools/friends_server/friends_service.py` (Python 3 stdlib, port 25570, sqlite; run it next to the game server and port-forward TCP 25570). One port, two protocols sniffed by first byte: HTTP POST `/api` (launcher: signup/login/logout/check_name/rename via `src/launcher/net/FriendsServiceClient`) and persistent NDJSON (game: `src/client/network/FriendsClient` — own io thread, app lifetime, hello(token) → server pushes `roster`/`invite` events; friend-op targets travel in `"friend"`, `"id"` is the correlation field). Launcher login stores `session_token`/`account_id`/`account_name` in `launcher.json`, forces the username to the account name, shows a debounced availability checkmark (rename commits via the service; friendships key on account id so renames propagate). Launch passes `--session`/`--account-id` → PlatformMain constructs `Client::g_friendsClient` (null = guest). Presence transitions live in PlatformMain only: Menu (title) / Hosting(worldName, 25565) / Playing(addr). Friends UI: `screens/FriendsScreen` (title screen's old Realms slot + pause menu). Join posts a `Multiplayer` TitleAction; from in-game the pause branch stashes it in `pendingSessionAction` and the outer session loop auto-joins without showing the title. Transport is plaintext by design (personal scale); the hosting machine's launcher should set `friends_service` to `127.0.0.1` (NAT hairpin).

## Updating to a Newer Minecraft Version

The game vendors several MC asset/code snapshots that need to be kept in sync when bumping MC versions. Sources should all come from the **same** MC release/snapshot — mismatches cause silent missing assets (e.g. an `Items.java` from 1.21.6 against textures from 1.21.4 leaves the new copper/spear/nautilus items unstyled).

### Folders/files to overwrite from a fresh MC asset dump

| What | Where in repo | Source in MC jar |
|---|---|---|
| Item model JSONs | `assets/models/item/*.json` | `assets/minecraft/models/item/*` |
| Item textures | `assets/textures/item/*.png` | `assets/minecraft/textures/item/*` |
| Block model JSONs | `assets/models/block/*.json` | `assets/minecraft/models/block/*` |
| Block textures | `assets/textures/block/*.png` | `assets/minecraft/textures/block/*` |
| GUI sprites | `assets/textures/gui/**/*.png` | `assets/minecraft/textures/gui/**` |
| Font atlas | `assets/textures/font/ascii.png` | `assets/minecraft/textures/font/ascii.png` |
| Lang file | `assets/lang/en_us.json` | `assets/minecraft/lang/en_us.json` |
| Items.java decompile | `minecraft_code/decompiled_net/minecraft/world/item/Items.java` | decompiled `net/minecraft/world/item/Items.class` |
| Terrain library snapshot | `src/my_terrain_library/` | (separate vendored project — see "Terrain Library Patches" below) |
| Recipes + item tags | `data/minecraft/recipe/`, `data/minecraft/tags/item/` | server jar `data/minecraft/**` |

After overwriting `Items.java`, **regenerate** the item table (see next section). After overwriting `src/my_terrain_library/`, **re-apply patches** (see "Terrain Library Patches").

### Crafting recipes

Recipes are baked into C++ rather than parsed at runtime. This dates from when
`data/` was not shipped in the app bundle at all — it now **is** copied to
`Contents/Resources/data`, because the terrain library reads block tags
(`BlockPredicate`) and structure NBTs (`FossilTemplate`) from it during chunk
generation. Both resolve via `MC_DATA_ROOT`, which `PlatformMain::Run` sets from
the bundle at startup; without it they walk up from the working directory, which
silently breaks every launcher-started build (`open` gives the app cwd `/`).
Do not drop the `data/` copy from CMakeLists. After overwriting
`data/minecraft/recipe/` and `data/minecraft/tags/item/`, regenerate:

```bash
python3 tools/gen_recipes.py    # → src/common/world/crafting/GeneratedRecipeList.{hpp,cpp}
```

The generator flattens `#minecraft:*` item tags, shrinks shaped patterns the way
`ShapedRecipePattern.shrink` does, and interns duplicate ingredient sets. It
covers `crafting_shaped` / `crafting_shapeless` / `crafting_transmute`; smelting,
stonecutting, smithing and the code-driven `crafting_special_*` types are skipped
(no menus for them yet). Slugs resolve to ItemIDs at startup in
`Game::RecipeManager::Initialize`, so a recipe naming an item we don't have drops
with a log rather than failing the load — the count is printed at boot.

### Mob parity audit — run this instead of finding gaps in game

`tools/mob_audit.py` compares every mob against `minecraft_code/` field by field
and prints what is missing. It exists because every gap found in this port so far
was found by a human noticing something in game — a frog that would not croak, a
camel whose ears did not move, an armadillo that never rolled up. That does not
scale to 90 mobs times a dozen systems.

```bash
python3 tools/mob_audit.py              # summary + every mob with a defect
python3 tools/mob_audit.py --all        # all 89
python3 tools/mob_audit.py frog camel   # named mobs, in full
python3 tools/mob_audit.py --self-test  # prove each check can still fail
python3 tools/mob_audit.py --html=out.html
```

Findings split into **defects** (dead clips, missing mesh/texture, attribute
drift, missing loot — things that are WRONG) and **scope** (MC behaviour not
ported yet). The most valuable check is **dead-clip**: a baked
KeyframeAnimation whose `MobAnim` timer slot is never started anywhere in `src/`
can never play, and that is invisible both in game and in review — it is exactly
the frog-croak bug.

**Always run `--self-test` after touching the audit.** A check that cannot fail
reports a clean bill of health forever, which is worse than no check: the first
draft of the attribute check looked up `DefaultAttributes` by class name when
the table is keyed by slug, so it silently compared nothing across all 89 mobs
and reported zero problems. The self-test injects a fault per check and requires
the count to rise.

### Mob spawn weights and loot tables

Both are baked from the vanilla data files this repo already ships, following
the same "generate to C++" pattern as recipes and block shapes:

```bash
python3 tools/gen_mob_spawns.py   # → src/common/world/spawn/GeneratedMobSpawns.{hpp,cpp}
python3 tools/gen_mob_loot.py     # → src/common/world/loot/GeneratedMobLoot.{hpp,cpp}
```

`gen_mob_spawns.py` reads `data/minecraft/worldgen/biome/*.json` `spawners`;
`gen_mob_loot.py` reads `data/minecraft/loot_table/entities/*.json`. Both drop
entries naming mobs or items this port does not implement, and print what they
skipped — expect ~42 unimplemented mob types and a handful of skipped loot
pools. Two skips are deliberate and permanent:

- **Conditional (rare-drop) pools** — zombie iron ingots, skeleton bows and the
  like are gated on `killed_by_player` plus a looting-scaled chance. Emitting
  them unconditionally would make every zombie drop an ingot, so the whole pool
  is skipped rather than half-modelled.
- **Sheep wool** — MC expresses it as an `alternatives` entry keyed on the
  sheep's dye colour, which cannot be a static row. It is dropped from
  `MobManager::DropLoot` using the sheep's live colour instead.

### Block outline shapes (hitboxes)

**A block's MODEL is not its shape.** MC keeps `BlockBehaviour.getShape`
completely independent of the rendered geometry, and for the whole cross-model
family they disagree wildly: a sapling is drawn as `block/cross` — two planes
spanning the full 16×16 cell — while `SaplingBlock.SHAPE` is
`Block.column(12, 0, 12)`. Mushrooms are `column(6, 0, 6)` against a full-width
model. Deriving the box by unioning model elements (which is what this engine
used to do) hands those blocks a 1×1×1 hitbox.

MC hardcodes shapes per block class in Java, so there is no data file to read —
they come out of the decompiled source:

```bash
python3 tools/gen_block_shapes.py   # → src/common/world/block/GeneratedBlockShapes.{hpp,cpp}
```

It maps slug → class from `Blocks.java`, then resolves each class's `getShape`
when it is a plain `return <CONST>;`, evaluating MC's `Block.box/column/cube/boxZ`
and `Shapes.or/join/block/empty` helpers (all closed-form conversions to a box).
`.move(...)` is stripped deliberately — that is MC applying the per-position
scatter, which this engine applies itself via `GetBlockOffset`, so baking the
moved shape would double the offset.

Blocks whose shape is per-state (`getShapeForEachState`, direction maps: slabs,
stairs, walls, doors, leaf litter, candles) are **deliberately skipped** and keep
the model-derived box, which is correct for them. Expect ~202 rows and a boot log
line reporting how many matched a BlockID. Chest and fire keep hand-written
shapes in `GetBlockShape` because their models are empty / deliberately oversized.

Since the engine stores one AABB per state, multi-box shapes are unioned — the
same simplification the model path already made.

### When MC ships new items

The choice between re-running the generator vs. hand-editing depends on how many items changed:

- **A handful of new items (≤5)** → append manually to `src/common/entity/GeneratedItemList.{hpp,cpp}` + drop the new JSON/PNG into `assets/models/item/` and `assets/textures/item/`. Faster than regenerating, and avoids touching the generator script. Append only — never reorder existing entries (numeric IDs are wire/save stable).
- **A snapshot bump with many additions** → run `python3 tools/gen_items.py` to regenerate `GeneratedItemList.{hpp,cpp}` from the new `Items.java`. The generator is **append-aware**: it keeps existing entries in their existing positions and only appends new slugs at the end (so IDs stay stable). Always inspect the diff before committing to confirm nothing was reordered.

Either way: once the item is in the table, registration is automatic — `ItemRegistry::Initialize` loops `kPureItemTable[]`, the loader picks up the JSON if present, and missing JSON/PNG just falls back to the slug-named texture (broken icon but no crash).

### Items that exist in `Items.java` but lack assets

It's normal for the asset dump to lag the decompile by a few items (snapshot vs. release timing). Missing-asset items still register and appear in the search tab — they just render with a missing-texture sprite. To list current gaps:

```bash
python3 -c '
import os, re
slugs = []
with open("minecraft_code/decompiled_net/minecraft/world/item/Items.java") as f:
    for line in f:
        m = re.search(r"registerItem\(\s*\"([a-z0-9_]+)\"", line)
        if m and "registerBlock" not in line: slugs.append(m.group(1))
missing = [s for s in slugs if not os.path.exists(f"assets/models/item/{s}.json")]
print(f"{len(missing)} items missing JSON model:")
for s in missing: print(f"  {s}")
'
```

## Terrain Library Patches

When updating `src/my_terrain_library/` from a newer snapshot, the following patches must be re-applied:

### ServerChunkCache abort support
The game adds `requestAbort()` / `isAbortRequested()` / `m_abort` to `ServerChunkCache` for clean shutdown. Without this, `getChunk()` blocks forever on exit.

**Header** (`include/server/level/ServerChunkCache.h`):
- Add `#include <atomic>` to includes
- Add public methods: `void requestAbort()` and `bool isAbortRequested()` using `m_abort` atomic
- Add private member: `std::atomic<bool> m_abort{false}`

**Source** (`src/server/level/ServerChunkCache.cpp`):
- In `getChunk()`, change `while (!future->isDone())` to `while (!future->isDone() && !m_abort.load(std::memory_order_acquire))`

### MSVC compatibility fixes (Windows build)
The terrain library uses GCC/Clang-specific features that need MSVC equivalents:

**`CarvingMask.cpp`**: Replace `__builtin_ctzll(bits)` with `#ifdef _MSC_VER` block using `_BitScanForward64`. Add `#include <intrin.h>` for MSVC.

**`Climate.h`, `Palette.h`, `SimpleBitStorage.h`**: Add `#include <string>` — MSVC doesn't include it transitively like GCC/Clang, so `std::to_string` fails.

**`AquaticFeatures.cpp`, `CaveFeatures.cpp`, `OrePlacements.cpp`**: Add `#include <deque>` — same transitive-include difference, for the `static std::deque<PlacedFeature>` pools.

**`NoiseChunk.cpp`**: Replace `__restrict__` with `#ifdef _MSC_VER __restrict` (MSVC uses different keyword).

### MapBlockType thread safety
`MyTerrainGenerator::MapBlockType()` is called from multiple server worker threads. It is lock-free by design: each thread keeps a `thread_local` Block*→BlockID cache plus a last-block memo (see MyTerrainGenerator.cpp). The caches are guarded by `s_blockMapEpoch`, bumped in `Initialize()` — `Blocks::bootstrap()` may recreate Block objects on world reload, so stale cached pointers must be invalidated. Do NOT replace this with a shared mutex-protected map: the old design took ~98k lock/unlock per converted chunk with all workers contending (measured 6.66ms/chunk conversion cost).

### Chunk pipeline: the rate ceilings (measured 2026-08)

How fast chunks become visible is set by a chain of rate limits, not by how fast
anything computes. Measure the rate at EACH handoff before optimising any stage —
twice now the expensive stage was not the limiting one.

| Stage | Where | Ceiling |
|---|---|---|
| Generation | `ServerWorkerPool` size x per-chunk latency | ~116 chunks/s peak |
| Delivery | `PlayerSession` batch quota (`m_desiredChunksPerTick`) | ~180/s, never binding |
| **Meshing** | **`permits.Available()` (10) x mesh-schedule passes/s** | **the usual culprit** |
| Upload | mesh permit pool round-trip, main thread | self-limiting by design |

Meshing bit us twice: `ScheduleClientMeshBuilds` was called from the 20 Hz
ClientTick (20 x 10 = 200 sections/s, and traces showed *exactly* 200/s), and
after moving it to a frame phase the internal period was still sized below
demand. Demand is `chunks/s x sections/chunk (~7.9) x remesh factor (~2.2)`.

The **2.2x remesh factor is MC-faithful** — `ClientPacketListener.enableChunkLight`
calls `setSectionRangeDirty` over the arriving chunk *and its 8 neighbours*, full
Y range, exactly like our `MarkNeighborSectionsDirty`. Budget for it.

**First compiles ARE gated on neighbour availability, because MC gates them.**
`LevelRenderer.compileSections` schedules a section only when

```java
section.isDirty() && (section.getSectionMesh() != CompiledSectionMesh.UNCOMPILED
                      || section.hasAllNeighbors())
```

so a section that has never been compiled waits for all 8 surrounding chunk
columns; one already compiled is always rescheduled. Ported as
`ClientChunkManager::HasAllNeighborChunks`. An earlier note here said the
opposite — do not gate — on the belief that gating was un-MC-like. That was
wrong on the facts. The tradeoff it described is real (frontier sections appear
a little later), but it is the tradeoff vanilla makes, and the gate is what
keeps the uncapped per-frame schedule affordable during a world load: the
frontier is exactly where neighbours are missing.

Useful commands (needs `cmake-build-tracy/tools/`, see the Tracy section):
```bash
# rate at each handoff — the diagnostic that actually finds the bottleneck
for z in LoadChunkInternal ApplyChunkData ProcessMeshJob UploadSection; do
  tracy-csvexport -s $'\t' -u trace.tracy | awk -F'\t' -v Z="$z" \
    '$1==Z{s=int(($4+$5)/1e9); c[s]++} END{for(k in c){n++;t+=c[k];if(c[k]>m)m=c[k]}
     printf "%-18s while_active=%.1f/s peak=%d/s\n", Z, t/n, m}'
done
```
Averages lie here: chunk loading is bursty, so a session average buries the burst
rate under idle seconds. Always compute per-second buckets.

### Translucency re-sort (measured 2026-08)

Two MC divergences were found and fixed here; both are easy to reintroduce.

1. **Sort by precomputed keys.** MC's `VertexSorting.byDistance` fills a flat
   `float[]` in one sequential pass and sorts indices against it. Recomputing the
   subtract + dot *inside* the comparator costs `~2*N*log N` distance evaluations
   instead of `N`, each a 12-byte random load. `TranslucentSort::BuildSortedIndices`
   now matches MC. Never move that math back into the comparator.
2. **Never `glBufferSubData` a re-sort into the shared slab IBO.** Writing a
   buffer with draws in flight makes the GL driver serialise — measured 0.18 ms
   per write, **16x the sort itself**, and 385 frames/session over 4 ms. Re-sorts
   go through `RenderBackend::UpdateBufferUnsynchronized` instead
   (`GL_MAP_UNSYNCHRONIZED_BIT`, no `INVALIDATE_RANGE` — see below).

Result: `Resort.Upload` 0.1813 ms -> **0.0011 ms**, frames >4 ms **385 -> 0**.

**Why unsynchronized is safe HERE only:** a re-sort is a same-length permutation
of one section's own quad range (`UpdateSectionIndices` rejects anything else), so
a torn read is a mix of two valid orderings — every index still points at a real
quad. Worst case is one frame of imperfect blend order, which the design already
tolerates (sorts go stale between re-sorts by construction). `INVALIDATE_RANGE` is
deliberately omitted: it would let the driver treat the old contents as undefined
and break exactly that property. **Do not use this path for mesh uploads** — those
write new geometry, where a torn read is real garbage.

`ChunkMegaBuffer` also supports MC's literal layout (`perSectionIndexBuffers`,
per-section IBOs like `CompiledSectionMesh` -> `SectionBuffers`). It fixes the same
stall but costs the multi-draw: translucent becomes one bind+draw per section
(measured `SubmitMultiDraw` 0.55 -> 0.69 ms, ~11 fps). Kept behind the flag for A/B;
the unsynchronized path is the better trade on this backend.

### Terrain parity check (run this before/after ANY terrain-library change)

`tools/terrain_parity/terrain_parity.cpp` generates a fixed grid of chunks from a
fixed seed and prints a hash of every block state. Same hashes = bit-identical
terrain. It links `terrain_library` alone (no game code) and runs single-threaded
with inline executors, so there is no scheduling nondeterminism.

Optimisations here fail silently — wrong terrain, not a crash — so "I believe it
is equivalent" is not good enough. Build it standalone against
`src/my_terrain_library`, then:

```bash
terrain_parity --seed 12345 --radius 2 > /tmp/before.txt   # BEFORE the change
# ...apply change, rebuild...
terrain_parity --seed 12345 --radius 2 > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt                        # must be empty
```

Needs `MC_DATA_ROOT` (defaults to `data` relative to cwd) or block tags and
structures fail to load and the comparison is meaningless.

### Profiling zones (Tracy)
Chunk generation is the most expensive thing in the program and used to be
invisible. `TerrainLibGetChunk` in `MyTerrainGenerator` times only the CALLER's
wait — off the library's main thread `ServerChunkCache::getChunk` enqueues and
then `sleep_for(100us)` in a loop, so that number is latency, not CPU. The work
runs on `BackgroundExecutor` (ChunkMap's "worldgen"/"light" executor), which had
no thread name and no zones.

- `include/util/TerrainProfiling.h` — `TERRAIN_ZONE_N` / `TERRAIN_THREAD`,
  no-ops without `TRACY_ENABLE`. It depends on **TracyClient only**, on purpose:
  do NOT add `${PROJECT_SOURCE_DIR}/src` to `terrain_library`'s include path, as
  both trees have a top-level `server/` and terrain lib headers could resolve to
  the game's.
- `ChunkStatusTasks.h` — `Gen.Biomes`, `Gen.Noise`, `Gen.Surface`, `Gen.Carvers`,
  `Gen.Features`, `Gen.Spawn`, `Gen.Full`. For the async stages the zone lives
  **inside** the `supplyAsync` lambda; around it would time the dispatch instead.
- `initializeLight` / `light` are no-op stubs here — nothing to measure.

### DensityFunctionRegistry re-bootstrap (quit-to-title support)
`DensityFunctionRegistry::clear()` must NOT delete the `zero()` density function — it is `Constant::ZERO()`, a process-lifetime singleton whose static accessor caches the pointer. Deleting it leaves the cache dangling; the next `bootstrap()` (second world in one process via quit-to-title → rejoin) re-registers the freed pointer and the first `NoiseRouterData::overworld()` build segfaults.

**Source** (`src/levelgen/DensityFunctionRegistry.cpp`): in `clear()`, skip the delete when `pair.second == zero()`.

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

**macOS viewer won't open** ("damaged and can't be opened"): Gatekeeper quarantine, not
a corrupt download. Tracy's macOS binaries are ad-hoc linker-signed, not notarized.
`xattr -dr com.apple.quarantine <folder>` — do the whole folder so the CLI tools
(`tracy-capture`, `tracy-update`, `tracy-merge`, `tracy-capture-daemon`) are cleared too.

### Our instrumentation

Macros live in `src/common/core/Profiling_Tracy.hpp` (`PROFILE_ZONE`, `PROFILE_ZONE_N`,
`PROFILE_PLOT`, `PROFILE_FRAME_MARK`, `PROFILE_THREAD`) and compile to nothing without
`TRACY_ENABLE`.

Main-thread frame phases, in order (`PlatformMain.cpp`): `InputUI` → `ClientTick` →
`GameLogic` → `MeshUpload` → `TexAnimation` → `Render` → `DebugUI` → `Present`.

Threads: `MeshWorker`, `ServerWorker`, `ServerThread`, `OcclusionBFS`.

Plots (prefix-grouped so they sort together in the plot list):

| Plot | Read it as |
|---|---|
| `Upload/QueueDepth` | Results awaiting upload. **Rising = a mesh permit is leaking.** Should return to ~0 each frame |
| `Upload/Bytes` | Bytes handed to the GPU — should correlate with `Present` time |
| `Upload/Sections`, `Upload/InFlight` | Uploads this frame; compile+upload occupancy |
| `Resort/Considered`, `Resort/Uploaded` | Translucency re-sort. Considered should be ~nearby + 15, never thousands |
| `Sections/Reachable`, `Sections/Visible` | Post-BFS and post-frustum section counts |

### Reading zones — lessons that cost us real time

- **A wide zone with ~100% self time and no children means the instrumentation is too
  coarse, not that the code is slow.** `Input` at 107 ms turned out to be ~490 lines
  wrapped in one zone. Split it before theorising about the cause.
- **Zone names lie if they aren't maintained.** `VSync` was really `glfwSwapBuffers`
  (renamed `Present`); `GPUUpload` existed at two different depths. Both sent us chasing
  the wrong thing. Rename on sight.
- **Cost is often paid somewhere other than where it's caused.** GL commands are queued,
  so upload and draw cost lands in `Present` (the swap), not at the call site. A wide
  `Present` means GPU-bound, not "the swap is slow".
- **Budgets that measure CPU time do not bound GPU work.** `glBufferSubData` returns once
  the driver stages the copy. See the mesh permit pool (`MeshUploadPermits.hpp`) for the
  MC-style backpressure that replaced a CPU-millisecond budget.

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

MyVoxelGame is a Minecraft-compatible voxel engine with a client-server architecture designed for both single-player (integrated server) and future multiplayer support.

### Three-Layer Architecture

#### 1. Client Layer (`src/client/`)
- **Input System**: Keyboard/mouse handling, player controller
- **Rendering Pipeline**: 
  - Three-pass rendering (opaque → cutout → translucent)
  - Frustum culling for performance
  - Chunk mesh generation and caching
  - Block highlight and crosshair rendering
- **Client Networking**: Connection management, packet handling
- **Client World Management**: Receives chunks from server, manages local cache

#### 2. Common Layer (`src/common/`)
Shared code between client and server:
- **Core Systems**: Logging, job system for parallelization, configuration
- **World Data Structures**: Chunks (16x16x384), sections (16x16x16), blocks
- **Physics**: Ray casting, AABB collision detection  
- **Block System**: Registry, models (JSON format), texture management
- **Network Protocol**: Packet definitions, message queue
- **World Generation**: Procedural terrain generation interface

#### 3. Server Layer (`src/server/`)
- **Integrated Server**: Runs in same process for single-player
- **Chunk Management**: Loading/unloading, view distance, dirty tracking
- **World Storage**: 
  - Minecraft Anvil format support (read/write)
  - Region file handling (.mca files)
  - NBT parsing for Minecraft compatibility
  - Async chunk saving
- **Server Networking**: Client connections, packet distribution
- **Worker Pools**: Parallel chunk loading/generation

### Key Systems

#### World Coordinate System
- **World Space**: Global 3D positions
- **Chunks**: 16x16 blocks horizontally, 384 blocks tall (-64 to 319)
- **Sections**: 16x16x16 voxel cubes, 24 per chunk
- **Blocks**: Individual voxels with 16-bit IDs

#### Minecraft Compatibility
- Loads existing Minecraft Java Edition worlds (1.18+)
- Supports Anvil region format (.mca files)
- Compatible block models and texture atlas system
- NBT data structure parsing

#### Performance Optimizations
- **Job System**: Thread pool for parallel processing
- **Chunk Caching**: LRU cache for loaded chunks
- **Mesh Optimization**: Greedy meshing, face culling
- **Frustum Culling**: Section-level visibility testing
- **Dirty Tracking**: Only remesh modified chunks

## Development Guidelines

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

### Code Organization
- Platform-specific code isolated in `src/platform/`
- Client-server separation enforced at directory level
- Common code shared via `src/common/`
- No direct file access from client code
- Server handles all world I/O operations

### Testing Approach
Manual testing with debug UI (ImGui integration). No formal unit test framework currently in place.

### Asset Pipeline
- Block models: JSON format in `assets/models/block/`
- Textures: Atlas generation from individual images
- Shaders: GLSL in `shaders/` directory

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