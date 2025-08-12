# Major Components

## Client Components

### ClientChunkManager (`src/client/world/ClientChunkManager.hpp`)
**Responsibility**: Client-side chunk state and lifecycle management

**Key Functions**:
- Process `ChunkDataS2CPacket` from server (UNLOADED → LOADED transition)
- Handle `BlockChangeS2CPacket` for individual block updates
- Track chunk access times for memory management
- Mark sections dirty when blocks change (triggers mesh rebuild)

**ChunkDataS2C Wire Format** (for precise Minecraft compatibility):
- Ground-up boolean flag
- 24-bit section bitmask (one bit per section)
- Heightmap data (NBT format)
- Biomes array
- Section data with palette encoding (NBT format per section)

**State Machine**: `ChunkState::UNLOADED` → `ChunkState::LOADED` (simplified client-side)

**Integration Points**:
- `ClientMeshManager` queries for chunk data during mesh building
- Network layer calls `ProcessChunkDataS2CPacket()` when packets arrive
- File: `src/client/world/ClientChunkManager.cpp`

### ClientMeshManager (`src/client/renderer/mesh/ClientMeshManager.hpp`)
**Responsibility**: Coordinate mesh building pipeline and GPU uploads

**Key Functions**:
- Schedule mesh builds for `LOADED` chunks via `ClientWorkerPool`
- Process completed mesh results from worker threads
- Upload mesh data to GPU within frame time budget
- Manage per-section GPU resources (VBO/IBO cleanup)

**Pipeline**: Chunk Data → Mesh Build Request → Worker Processing → GPU Upload → Rendering

**Integration Points**:
- `ClientChunkManager` provides chunk data for meshing
- `ChunkRenderer` queries for GPU data during rendering
- File: `src/client/renderer/mesh/ClientMeshManager.cpp`

### ClientWorkerPool (`src/client/world/ClientWorkerPool.hpp`)
**Responsibility**: Background mesh building on CPU worker threads

**Key Functions**:
- Execute CPU-intensive greedy meshing algorithm
- Submit `MeshBuildResult` to result queue for main thread processing
- Handle mesh build cancellation when chunks unload
- Priority-based work scheduling (near-to-far from player)

**Thread Model**: 2-4 worker threads, communicate via `ResultQueue<MeshBuildResult>`

## Server Components

### IntegratedServer (`src/server/IntegratedServer.hpp`)
**Responsibility**: Main server controller with 20 TPS tick loop

**Key Functions**:
- Run server thread with fixed 20 TPS timing (50ms per tick)
- Process all Client→Server packets by draining connection queues
- Coordinate world simulation and chunk loading
- Send Server→Client packets for chunk streaming and block changes

**Tick Order**: Connection Drain → World Simulation → Outbound Packet Enqueue

**Integration Points**:
- `NetworkServer` for connection management and packet I/O
- `World` for game state and chunk storage
- File: `src/server/IntegratedServer.cpp` (717 lines - main implementation)

### World (`src/common/world/level/World.hpp`)
**Responsibility**: Block storage, chunk management, and game simulation

**Key Functions**:
- `SetBlock()`/`GetBlock()` for world modification
- `UpdateLoadedChunks()` for chunk loading around players
- Interface with `ChunkProvider` for storage and generation
- Coordinate with `DirtyTracker` for change notifications

**Storage Model**: Chunks stored as `shared_ptr<Chunk>` with reference counting

**Integration Points**:
- `IntegratedServer` calls `WorldLoop()` during server ticks
- `ChunkProvider` handles loading from Anvil files or generation
- File: `src/common/world/level/World.cpp`

### ChunkProvider (`src/server/world/ChunkProvider.hpp`)
**Responsibility**: Abstract chunk loading from storage or generation

**Key Functions**:
- `LoadChunk()` from Minecraft Anvil format (.mca files)
- `GenerateChunk()` for procedural terrain generation
- `SaveChunk()` for world persistence
- Interface with `RegionFileCache` for efficient file access

**Loading Strategy**: Try storage first, fall back to generation, cache in memory

**Integration Points**:
- `MinecraftChunkLoaderImpl` for Anvil format support
- `ProceduralChunkGenerator` for terrain generation
- `AsyncChunkSaver` for background save operations
- File: `src/server/world/ChunkProvider.cpp`

### ServerWorkerPool (`src/server/world/ServerWorkerPool.hpp`)
**Responsibility**: Background chunk loading and generation

**Key Functions**:
- Load chunks from disk without blocking server thread
- Generate new chunks procedurally when not found in storage
- Submit completed chunks to server thread for integration
- Priority-based loading (player vicinity first)

**Thread Model**: 2-4 worker threads, communicate via `ChunkGenResultQueue`

## Network Components

### NetworkServer (`src/server/network/NetworkServer.hpp`)
**Responsibility**: TCP server with connection management

**Key Functions**:
- Accept incoming connections on localhost:25565
- Maintain list of active `ServerConnection` instances
- Broadcast packets to all connected clients
- Handle connection lifecycle and cleanup

**Threading**: All operations happen on dedicated I/O thread via `io_context`

**Integration Points**:
- `IntegratedServer` configures callbacks and broadcasts packets
- `ServerConnection` instances handle per-client communication
- File: `src/server/network/NetworkServer.cpp`

### ServerConnection (`src/server/network/ServerConnection.hpp`)
**Responsibility**: Per-client connection with protocol state management

**Key Functions**:
- Handle protocol state transitions (HANDSHAKING → LOGIN → PLAY)
- Decode incoming packets and route to appropriate handlers
- Maintain packet listeners per protocol state
- Manage keep-alive, rate limiting, and timeouts

**State Machine**: Each connection progresses through protocol states with different packet listeners

**Integration Points**:
- `HandshakePacketListener`, `LoginPacketListener` for connection setup
- `IntegratedServer` receives decoded packets via `tick()` method
- File: `src/server/network/ServerConnection.cpp`

### PacketRegistry (`src/common/network/PacketRegistry.hpp`)
**Responsibility**: Packet ID definitions and encoding/decoding utilities

**Key Functions**:
- Define all packet IDs with hex values (0x00-0xFF range)
- `VarInt`/`VarLong` encoding following Minecraft protocol
- `PacketBuffer` and `PacketReader` for binary serialization
- Support for compression headers and pipeline changes

**Packet Categories**:
- 0x00-0x0F: Handshake and setup packets
- 0x10-0x7F: Server → Client packets
- 0x80-0xFF: Client → Server packets

**Integration Points**:
- All network code uses these definitions for packet identification
- Compression and framing logic built on VarInt encoding
- File: `src/common/network/PacketRegistry.hpp` (452 lines of protocol infrastructure)

## Common Components

### MessageQueue (`src/common/network/MessageQueue.hpp`)
**Responsibility**: Thread-safe communication between threads

**Key Functions**:
- Bounded queues with back-pressure (drop packets when full)
- Specialized `ResultQueue` for worker thread communication
- Batch draining with `DrainAll()` for efficient processing
- Statistics tracking for queue depths and drop rates

**Queue Types**:
- Client↔Server packet queues
- Worker result queues (`MeshBuildResult`, `ChunkGenResult`)
- All queues bounded to prevent memory exhaustion

### JobSystem (`src/common/core/JobSystem.hpp`)
**Responsibility**: Thread pool management for both client and server workers

**Key Functions**:
- Initialize worker threads for mesh building and chunk loading
- Submit jobs with priority and cancellation support
- Coordinate shutdown to ensure clean thread termination
- Statistics for job throughput and queue depths

**Integration**: Used by both `ClientWorkerPool` and `ServerWorkerPool` for actual thread management