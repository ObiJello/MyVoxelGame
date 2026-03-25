# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Network Info

- **Mac Public IP:** 74.105.150.36

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
`MyTerrainGenerator::MapBlockType()` is called from multiple server worker threads. The `m_blockIdCache` unordered_map must be protected with `m_blockIdCacheMutex` (already in MyTerrainGenerator.hpp/cpp, not in the terrain library).

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