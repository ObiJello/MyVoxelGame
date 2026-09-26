# CLAUDE.md

Guidance for Claude Code when working in this repository.

MyVoxelGame (shipped as **ObeyCraft**) is a Minecraft-compatible voxel engine in C++20 with a client-server architecture: single-player runs an integrated server, multiplayer connects to a dedicated one. It loads Minecraft Java Edition worlds (1.18+, Anvil `.mca` regions, NBT), and uses MC-compatible block models and a texture atlas.

Minecraft's decompiled source is in the project root: `minecraft_code_26.3-pre-2/` (26.3 Pre-Release 2, world version 5018) is the current reference; `minecraft_code_26.1-snapshot-1/` (26.1 Snapshot 1, world version 4764) is the version the terrain library was first ported from and that saves are stamped with.
## Working rules

- **Do not build after making changes.** Say "try building with the new changes and let me know if there are any errors." Only run a build when the user reports errors and asks you to diagnose.
- **Do not push unless asked.**
- **No Claude attribution in commits.** Never add `Co-Authored-By` lines.
- Do not add dated session notes, per-feature walkthroughs, or profiling results to this file.

## Repo layout

- `src/common/` — shared code (core, entity, world, network protocol)
- `src/client/` — renderer, networking client, dev tooling (`src/client/dev/`)
- `src/server/` — dedicated + integrated server
- `src/platform/PlatformMain.cpp` — entry point; parses all command-line args
- `src/launcher/` — ObeyCraft launcher (config in `LauncherConfig.hpp`)
- `ext/` — vendored deps: GLFW, GLAD, GLM, ImGui, zlib, OpenAL, stb_image, Boost.Asio (header-only). nlohmann/json and Tracy come in via FetchContent.
- `tools/` — build/release scripts, profiling report scripts, `play.sh`, `friends_server/`
- `docs/` — design docs and engineering notes (`thread-model.md`, `immersive-portals.md`, `engineering-notes.md`, `redstone.md`, `fluids.md`)
- `assets/` — game and launcher assets

Runtime data lives in the obeycraft directory (`~/Library/Application Support/obeycraft` on macOS): `launcher.json`, `game/`, `logs/`, `crash-reports/`, `recordings/`.

## Building

CMake 3.19+ with Ninja; CLion is the primary IDE. Targets macOS (universal), Windows, Linux.

```bash
cmake -B cmake-build-debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-debug   --target MyVoxelGame -j$(sysctl -n hw.ncpu)
cmake --build cmake-build-release --target MyVoxelGame -j$(sysctl -n hw.ncpu)
```

| Build dir | Notes |
|---|---|
| `cmake-build-debug` | Tracy enabled |
| `cmake-build-tracy` | Tracy enabled, for profiling runs |
| `cmake-build-release` | No Tracy |
| `cmake-build-universal` | Distribution build, `-DJALIN=ON` default, no Tracy |

**Distribution build** (universal arm64+x86_64, uploads dSYMs to Sentry): add `-DJALIN=ON` and set `VULKAN_SDK=/Users/obey/VulkanSDK/1.4.341.1/macOS`. Use the Vulkan SDK, not Homebrew MoltenVK (arm64-only).

**Run:** `./build/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame` (macOS) or `./build/bin/MyVoxelGame` (Linux/Windows). `tools/play.sh` wraps common dev launches.

**Sounds:** `python3 tools/extract_mc_sounds.py` copies Minecraft's sound assets from the local Minecraft install into git-ignored `assets/sounds/` + `assets/sounds.json` (run once; `--check` reports staleness). The build stages them via `cmake/StageSounds.cmake` only when the extraction changed (`-DBUNDLE_MC_SOUNDS=OFF` leaves them out). Every sound site uses `ILevelWrite`/`EntityLevel::PlaySound(except, ...)` with MC's `except` argument — the acting player's client plays it while predicting, the server sends it to everyone else (`common/sound/LevelSound.hpp`). Engine/mod events map onto vanilla sounds in `assets/sound_overlays/<ns>/*.json`, never in code.

### Windows/MSVC portability

Code that builds clean on Clang fails on MSVC in three recurring ways — check these before blaming the toolchain:

- **Transitive includes.** libc++ pulls in `<stdexcept>`, `<string>`, `<deque>` through other headers; MSVC's STL does not. Include what you use.
- **Zero-length arrays.** `static const T k_foo[] = {};` is a Clang extension (MSVC C2466). Emit `nullptr` + count 0 instead.
- **`class` vs `struct` in forward declarations.** MSVC mangles them differently; a mismatch links on Clang and gives LNK2019 on MSVC. Match the tag to the definition.

## Command-line arguments

All parsed in `PlatformMain.cpp:Run()`. The launcher builds them from user settings (`src/launcher/LauncherApp.cpp`); **any new flag exposed to players also needs a `build*Arg` lambda + UI control there.**

| Flag | Value | Meaning |
|---|---|---|
| `--vulkan` | — | Vulkan backend instead of OpenGL |
| `--server` | `host[:port]` | Join a dedicated server (default port 25565) |
| `--name` | string | Player name; empty → server assigns `PlayerN` |
| `--color` | slug | Player colour (`Game::kPlayerColorTable`, `src/common/entity/PlayerColors.hpp`) |
| `--session` / `--account-id` | token / n | Friends-service login from the launcher; missing → guest |
| `--friends-service` | `host[:port]` | Override friends-service address (defaults in `src/common/core/FriendsServiceConfig.hpp`) |
| `--allow-private-hosts` | — | Treat private addresses as directly joinable (LAN/local testing) |
| `--env` | `NAME=VALUE` | Set an env var before startup (how `OBEY_*` dev switches get through `play.sh`); repeatable |
| `--exec` / `--exec-delay` / `--quit-after` | cmd / sec / sec | Run a command after load, then quit — for scripted profiling |
| `--quit-capture` | — | With `--quit-after`: leave through the last-world panorama capture (`panorama/last_world`), so a scripted run can be looked at; `OBEY_GAME_DIR` points a run at its own game dir |
| `--record` / `--replay` / `--replay-hold` | name / name / sec | Dev harness: record/replay a player path for reproducible profiling (`src/client/dev/SessionReplay`) |
| `--ui-test` | — | Dev harness: click through pause-menu screens, then quit |
| `--crash-test` | — | Deliberate SIGSEGV at startup to test the crash pipeline |
| `--force-game-mode` | — | Force macOS Game Mode via `gamepolicyctl` |
| `--headless-server` | — | With `--world <name>`: run that world's server with no window (dedicated server); `--port N`, `--guest-commands`. Always writes the `[ServerStats]` report (`logs/server/`) |
| `--bots` | N | With `--server host:port`: N headless fake players (`src/client/dev/BotSwarm.hpp`; `--bot-scenario spread\|fly\|cluster\|idle`, `--bot-*` tuning). Report in `logs/bots/`. `tools/stress_test.sh` runs server + bots together |

## Core invariants

Things that must hold across the codebase; details and rationale in `docs/engineering-notes.md`.

- **Camera-relative rendering.** `Camera::position` is `dvec3`. Every float position handed to the GPU is `Render::ToRender(world)` (subtract the render origin in double, then narrow). Culling stays in world space. Read `src/client/renderer/core/RenderOrigin.hpp` before touching anything that produces GPU coordinates.
- **Terrain vertex format.** Chunk terrain uses the 20-byte packed `Render::TerrainVertex` (`src/client/renderer/core/Vertex.hpp`; the last word is the MC light coords), not the general 24-byte `Vertex`. Changing its layout touches the mesher, mega buffer, both backends, the `terrain*` shaders and their `_vk` twins — and the `.spv` files must be recompiled with glslc **and committed**.
- **Hardware scaling.** Every machine-dependent budget reads `Core::HardwareProfile::Get()`. Rule for any new budget: a fast machine gets exactly the number it had; only weaker hardware scales down.
- **Graphics settings.** `graphicsPreset` is a one-shot macro (`GameSettings::ApplyGraphicsPreset`); the engine reads individual options, and each setter flips the preset to custom. Mesh-time options reach workers via `Render::Mesher::SetMeshOptions`, never `g_gameSettings` directly.
- **Frames overlap on the GPU (Vulkan).** Nothing a frame writes on the GPU may be an object the previous frame still uses — Metal holds the whole next render encoder (vertex stage included) on any such write. Per-frame-slot depth images, per-frame texture copies for textures updated while in use, per-slot query pools, and MoltenVK argument buffers off + single-queue semaphores (`VK_EXT_layer_settings` in `CreateInstance`) keep it that way. Details in `docs/engineering-notes.md` (Frame overlap on MoltenVK).
- **Face-direction groups.** Every emitter of an opaque/cutout quad must push a facing (`GenerateQuad` does; fluid/greedy emitters push explicitly).
- **Dirty sections.** Use `ClientChunk::AddDirty`/`RemoveDirty`; never touch `dirtySections` directly (`dirtyMask` mirrors it).
- **Crash handler** (`CrashHandler.cpp`) uses only async-signal-safe calls; keep it that way. It must be installed after `sentry_init`.
- **Wire compatibility.** Client and server ship together; trailing-field additions are the pattern for extending packets.
- **Fluids.** A `FluidState` is computed from the block state (`FluidStateOf`), never stored; fluid ticks ride the block-tick queue keyed on `BlockID::Water`/`Lava` and are dispatched on the cell's fluid, not its block; a waterloggable block accepts only a *source* (`Fluids::CanPlaceLiquid`). Details in `docs/fluids.md`.
- **Lighting.** Light lives on `Chunk::light` (26 sky/block `DataLayer`s, sections -5..20); only the server's per-`World` `LevelLightManager` writes it after load (server thread; initial chunk light runs on the gen workers), and the client only swaps in what `ChunkDataS2C`/`LightUpdateS2C` send. A new glowing block is one `ENGINE_BLOCK_LIGHT` row in `EngineBlockLight.inc`; everything drawn in the world multiplies by the lightmap colour for its MC packed light (`Render::Lightmap`, `EntityEnvironment::LightColor`) — never by `skyBrightness`. Details in `docs/engineering-notes.md` (Lighting).
- **Biomes.** A block's biome is MC `getBiome` — the fuzzy zoom (`common/world/biome/BiomeZoom.hpp`, seeded with the hashed world seed the server sends on `LoginSuccess`/`ChangeDimensionS2C`): `World::GetBiome` / `ClientBlockAccess::GetBiome` / the mesher's tint. The raw 4×4×4 cell (`ClientChunkManager::BiomeAtWorld`, `ChunkProvider::GetNoiseBiome`) is only for what MC samples unzoomed: fog, sky, water fog, music.
- **Terrain worldgen is 26.3 data.** Vanilla noise, density functions, noise settings and material rules are the 26.3 datapack JSON under `data/minecraft/worldgen/`, decoded by `levelgen/density/WorldgenRegistries` and `levelgen/material/MaterialRuleRegistry` with the Java codecs' rules — change them there, not in code. The engine's own dimensions build theirs in code (`ModTerrainSettings`, `ModMaterialRules`). The float density engine (`levelgen/density/`) must stay bit-exact with Java: check any change with `terrain/tests/parity/run_density_parity.sh`. Details in `docs/minecraft-26.3-worldgen-port.md`.
- **Terrain library storage.** The library saves its unfinished (proto) chunks into the world's region files through `LibraryChunkStorage`, over the game's shared `AnvilChunkIo` — never give it a storage path of its own (a second handle on an `.mca` corrupts it). FULL chunks are the game's; a proto never overwrites one, and once the game holds a FULL chunk the library releases its copy of the blocks (`ChunkMap::releaseHandedOffChunks`) — nothing may read a converted chunk's blocks back from the library. Details in `docs/terrain-library-persistence.md`.
- **Chunk levels.** The game's `ChunkTicketManager` levels move only in `RunAllUpdates` (MC `DistanceManager.runAllUpdates`: once per level per tick after the sessions, incremental via MC's `ChunkTracker`) — no query ever propagates. Library generation requests only add a ticket; they attach to their holder after the library's next distance pass (`MyTerrainGenerator::AttachPendingRequests`), never one pass per request. Generation work runs between ticks (`PumpChunkPipeline`), not inside the tick. A request's ticket is MC's `PLAYER_LOADING` ticket — level 31, so its 5×5 generates together — held while the chunk is in a player's view, released the moment it leaves every view (`CancelLoadIfUnwanted`); at most 4 requests per level are in flight, each until its `ENTITY_TICKING` future completes (MC `ThrottlingChunkTaskDispatcher(…, 4)` / `ticketsToRelease`). A game chunk unloads once it is more than 13 chunks (MC `MAX_LEVEL` 44 − player ticket level 31) outside every session loader — no timer, no cap (`UnloadUnwatchedChunks`).
- **Block updates.** `World::UpdateFlags` are MC's `Block.UPDATE_*` values, and every neighbour notification (`neighborChanged` AND `updateShape`) goes through the collecting updater in `NeighborUpdater.hpp` — never walk neighbours by hand. `Block::updateShape` is read-only; `Block::neighborChanged` is the writable one. Details in `docs/redstone.md`.

## Profiling

Tracy is the profiler of record, pinned to **v0.14.1** via FetchContent — the `GIT_TAG` in `CMakeLists.txt` is the single pin for the game, the viewer and the command-line tools. Three setup invariants that have all bitten us:

1. Client and viewer versions must match exactly (bump `GIT_TAG` and the `tracy-profiler` app together). The CLI tools follow on their own: `tools/build_tracy_tools.sh` builds `tracy-export` (ours, `tools/tracy_export/`), `tracy-capture`, `tracy-csvexport` and `tracy-update` at the pinned tag into git-ignored `tools/tracy/`, and the report scripts run it whenever the pin moves.
2. `TRACY_ENABLE` must be set as a CACHE var before `FetchContent_MakeAvailable`, not via `target_compile_definitions`.
3. A stale `libTracyClient.a` survives a `GIT_TAG` bump — delete `cmake-build-tracy/_deps/tracy-*` and reconfigure if a bump doesn't take.

Tracy on macOS has no context-switch capture; don't claim thread starvation from Tracy alone. Apple sampling is behind `option(TRACY_APPLE_SAMPLING)` (off by default) and a CMake patch that removes Tracy's root check.

Tools: `tools/play.sh tracy --vulkan [--gpu-trace[=SEC]]`, `tools/tracy_report.py capture.tracy [other.tracy]` (one capture, or an A/B of two; cut to the replayed path when the capture has one), `tools/analyze_trace.py`, `tools/gpu_report.py`, `--record`/`--replay` for reproducible runs, `OBEY_SKIP=<stage>` with `OBEY_SKIP_PERIOD` for per-stage GPU A/B. Runtime `OBEY_*` env switches are the standard kill-switch pattern for A/B testing a change. `OBEY_SERVER_STATS=1` turns on the server's once-a-second load report (`Server::ServerStressStats`: tick phases, chunk streaming, per-player send backlog) in a normal hosted game. This MacBook Air throttles within ~40 s of GPU load — compare only back-to-back runs at the same temperature.

Full workflow, gotchas, and past measurements: `docs/engineering-notes.md`.

## Launcher & distribution

Building in the right configuration does everything — no scripts to run:

| Build | What happens |
|---|---|
| Game Release + `-DJALIN=ON` | Version bump, zip, GitHub upload, Sentry dSYM upload |
| Launcher Release (any) | Version bump, zip, GitHub upload |
| Anything else | Normal build |

- Versions: game `0.1.X`, launcher `1.0.X`; build numbers in `tools/game_build_number` / `tools/launcher_build_number`, generated into `GameBuildVersion.hpp` / `LauncherBuildVersion.hpp`.
- Releases go to `ObiJello/MyVoxelGame-Download`. Game tags are `vX.Y.Z`; launcher tags are `launcher-vX.Y.Z`. Zip names must contain a platform tag (`macos-universal`, `macos-arm64`, `windows-x64`).
- Launcher picks the latest non-`launcher-v` tag for game updates and the latest `launcher-v` tag for self-update.
- Uploads are non-fatal if offline or `gh`/`sentry-cli` isn't set up. Sentry release string: `myvoxelgame@0.1.X`.
- First-time installers: `tools/create_dmg.sh` (macOS), `tools/create_installer.ps1` (Windows, needs Inno Setup 6). Manual release: `tools/release_game.sh`, `tools/release_launcher.sh [minor]`.
- Scripts: `tools/bump_version.sh`, `tools/auto_release.sh`, `tools/update_plist_version.sh`.

### Player-side diagnostics

Ask a player for these first; they exist whether or not Sentry got through.

| Path (under the obeycraft dir) | What it is |
|---|---|
| `logs/latest.log` | Current session; a log without `--- log closed cleanly ---` at the end means the process died |
| `logs/<date>.log` | Previous sessions (newest 5) |
| `crash-reports/crash-<date>.txt` | Version, signal, backtrace, last ~512 log lines |

## Friends service

Self-hosted backend in `tools/friends_server/friends_service.py` (Python 3 st
dlib, sqlite, TCP 25570 — the only port the service machine forwards). Launcher talks HTTP `/api` (`src/launcher/net/FriendsServiceClient`); the game keeps a persistent NDJSON connection (`src/client/network/FriendsClient`). Hosting uses UPnP (`src/client/network/UPnPPortMapper`) with a service-side reachability probe, falling back to relay through the service. Plaintext by design (personal scale). Flow details in `docs/engineering-notes.md`.
