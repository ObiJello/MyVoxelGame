# Architecture Context & Glossary

## System Overview (C4 Level 1)

MyVoxelGame is a Minecraft-compatible voxel engine built in C++ using an integrated server architecture. The system supports single-player gameplay with future multiplayer extensibility, featuring real-time world streaming, procedural generation, and GPU-accelerated rendering.

### Core Purpose
- **Primary**: Provide a high-performance, Minecraft-compatible voxel game engine
- **Secondary**: Demonstrate modern C++ networking patterns (Boost.Asio) in game development
- **Future**: Support multiplayer servers with minimal architectural changes

### Key Architectural Decisions
1. **Integrated Server Model**: Server runs in same process as client (like Minecraft singleplayer)
2. **Thread Separation**: Dedicated threads for rendering (~60 FPS), server ticks (20 TPS), and network I/O
3. **Minecraft Compatibility**: Supports Anvil world format, chunk streaming protocol, and block models
4. **Modern C++**: Uses C++20 features with Boost.Asio for networking (mirrors Java's Netty)

## Glossary

### Threading Terms
- **Client Thread**: Main render thread running at ~60 FPS, handles input/rendering/GPU uploads
- **Server Thread**: Game logic thread running at fixed 20 TPS (50ms intervals)
- **I/O Thread**: Dedicated network thread running `io_context::run()` with work guards
- **Worker Pools**: Background threads for chunk loading/generation and mesh building

### Networking Terms
- **Packet Listener**: Handler object that processes decoded packets for specific protocol states
- **Protocol State**: Connection phase (HANDSHAKING → LOGIN → PLAY)
- **Strand**: Boost.Asio construct ensuring single-threaded access to connection resources
- **Pipeline Flip**: Changing packet decode/encode behavior mid-connection (compression, encryption)
- **VarInt/VarLong**: Variable-length integer encoding used in Minecraft protocol

### World Terms
- **Chunk**: 16×16×384 block column from Y=-64 to Y=319 (24 sections)
- **Section**: 16×16×16 block cube within a chunk (numbered 0-23 from bottom to top)
- **Chunk Streaming**: Protocol for sending world data from server to client
- **Watch Set**: Per-player set of chunks that should be loaded and sent
- **Ticket**: Reference that keeps a chunk loaded in server memory
- **Ground-up**: Complete chunk data transmission (vs. partial updates)

### Rendering Terms
- **Section Mesh**: GPU vertex/index data for one 16×16×16 section
- **Meshing Pipeline**: Process of converting block data to renderable geometry
- **GPU Upload**: Transfer of mesh data from CPU to GPU memory (VBO/IBO)
- **Render Layer**: Rendering pass (opaque → cutout → translucent)
- **Frustum Culling**: Skip rendering sections outside camera view

### Data Structures
- **Block ID**: 16-bit identifier for block type (0-65535) - Note: This is our engine's internal representation; Minecraft uses palette-compressed state indices per section on the wire
- **BlockState**: Block ID + metadata (rotation, water level, etc.)
- **ChunkPos**: 2D coordinate identifying a chunk (x, z)
- **SectionKey**: 3D coordinate identifying a section (chunkX, chunkZ, sectionY)
- **NBT**: Named Binary Tag format used by Minecraft for structured data

### File Paths (Key Components)
- `src/server/IntegratedServer.cpp` - Main server loop and 20 TPS tick processing
- `src/client/world/ClientChunkManager.cpp` - Client-side chunk state management
- `src/server/network/ServerConnection.cpp` - Per-connection packet handling and protocol states
- `src/common/network/PacketRegistry.hpp` - Packet ID definitions and VarInt encoding
- `src/client/renderer/mesh/ClientMeshManager.cpp` - Mesh building coordination and GPU uploads

### Protocol States
- **HANDSHAKING**: Initial connection, client declares intent (status/login)
- **STATUS**: Server list ping, MOTD, no gameplay (add dedicated STATUS listener for completeness)
- **LOGIN**: Authentication and compression setup
- **PLAY**: Active gameplay, chunk streaming, player movement

### Performance Budgets (Target)
- **Server Tick**: 50ms max (20 TPS), ideally <30ms
- **Client Frame**: 16.67ms max (60 FPS), ideally <12ms
- **Mesh Building**: 1-2ms per frame budget for CPU meshing
- **GPU Upload**: 2-3ms per frame budget for VBO/IBO transfers
- **Packet Processing**: <5ms per tick for all packet draining