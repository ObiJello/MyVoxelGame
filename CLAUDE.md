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

`data/` is NOT copied into the app bundle (only `assets/` is), so recipes are
baked into C++ instead of parsed at runtime. After overwriting
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

**`NoiseChunk.cpp`**: Replace `__restrict__` with `#ifdef _MSC_VER __restrict` (MSVC uses different keyword).

### MapBlockType thread safety
`MyTerrainGenerator::MapBlockType()` is called from multiple server worker threads. It is lock-free by design: each thread keeps a `thread_local` Block*→BlockID cache plus a last-block memo (see MyTerrainGenerator.cpp). The caches are guarded by `s_blockMapEpoch`, bumped in `Initialize()` — `Blocks::bootstrap()` may recreate Block objects on world reload, so stale cached pointers must be invalidated. Do NOT replace this with a shared mutex-protected map: the old design took ~98k lock/unlock per converted chunk with all workers contending (measured 6.66ms/chunk conversion cost).

### DensityFunctionRegistry re-bootstrap (quit-to-title support)
`DensityFunctionRegistry::clear()` must NOT delete the `zero()` density function — it is `Constant::ZERO()`, a process-lifetime singleton whose static accessor caches the pointer. Deleting it leaves the cache dangling; the next `bootstrap()` (second world in one process via quit-to-title → rejoin) re-registers the freed pointer and the first `NoiseRouterData::overworld()` build segfaults.

**Source** (`src/levelgen/DensityFunctionRegistry.cpp`): in `clear()`, skip the delete when `pair.second == zero()`.

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