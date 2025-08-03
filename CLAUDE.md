# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

The project uses CMake with multiple build configurations:

### Quick Build Commands
```bash
# Build main game executable
cd build && make MyVoxelGame

# Build using debug configuration
cd cmake-build-debug && ninja MyVoxelGame

# Build all targets
cd build && make all

# Clean and rebuild
cd build && make clean && make
```

### Other Build Targets
```bash
make install            # Install binaries to system
```

### Running the Game
```bash
# macOS (app bundle)
./build/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame

# Linux/Windows
./build/bin/MyVoxelGame
```

## Architecture Overview

MyVoxelGame is a voxel-based game engine with Minecraft world loading capabilities. The architecture follows a layered design:

### Core Systems
- **Platform Layer** (`src/platform/`): GLFW/OpenGL abstraction, input handling, time management
- **Core Systems** (`src/core/`): Logging, job system, configuration management  
- **Render System** (`src/render/`): OpenGL rendering, mesh management, shaders, chunk rendering
- **Game Logic** (`src/game/`): Player controller, inventory, world coordinates, game math

### World Engine (`src/engine/`)
- **Block System**: Block registry, model loading, texture atlas management
- **World Management**: Chunk loading/unloading, world coordinate system, Minecraft region file parsing
- **Physics**: Ray casting, collision detection
- **Mesh Generation**: Three-layer rendering pipeline (opaque, cutout, translucent)

### Key Components

#### World Loading System
- `MinecraftChunkLoader`: Loads chunks from Minecraft world files
- `ChunkProvider`: Manages chunk loading/unloading with configurable view distances
- `RegionFile` + `NBTParser`: Parses Minecraft region files and NBT data

#### Rendering Pipeline
- **Three-Layer System**: Opaque → Cutout → Translucent rendering for proper transparency
- **Chunk Meshing**: Converts voxel data to optimized triangle meshes
- **Frustum Culling**: Only renders visible sections for performance
- **Block Models**: JSON-based block model system compatible with Minecraft assets

#### Coordinate Systems
- World coordinates: Global 3D positions
- Chunk coordinates: 16x16 horizontal sections
- Section coordinates: 16x16x16 voxel cubes (24 sections per chunk vertically)
- Y-range: -64 to 319 (384 blocks total height)

## Development Notes

### Dependencies
- **External Libraries**: All bundled in `ext/` (GLFW, GLAD, GLM, ImGui, zlib, OpenAL)
- **JSON Processing**: Uses nlohmann/json (FetchContent)
- **Build System**: CMake with ninja/make support
- **Platform Support**: macOS (universal binary) and Windows

### Testing
- `TestRegionDumper`: Tests Minecraft region file parsing
- No formal unit testing framework - uses manual testing approach

### Asset Management
- Minecraft-compatible block models in `assets/models/`
- Texture atlas building from individual textures
- Shaders in `shaders/` directory (GLSL)

### Performance Considerations
- Asynchronous chunk loading system
- Chunk mesh caching and dirty flagging
- Frustum culling for render optimization
- Job system for parallel processing

### Key Configuration
- Chunk loading distance configurable
- Debug rendering options available
- Settings system for runtime configuration changes