# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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