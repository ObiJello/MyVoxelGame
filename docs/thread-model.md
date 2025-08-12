# Thread Model

## Thread Responsibilities

### Client Render Thread (~60 FPS)
**DO:**
- Process input events (keyboard, mouse)
- Drain incoming Server→Client packets from network thread
- Schedule mesh builds for newly loaded chunks
- Upload completed mesh data to GPU within time budget (2-3ms)
- Perform frustum culling and render all visible sections
- Present frame and handle VSync

**DON'T:**
- Block on file I/O or network operations
- Perform CPU-intensive mesh building (delegate to worker pool)
- Access server-owned World data directly
- Exceed frame time budget (causes frame drops)

**Order of Operations per Frame:**
1. **Input Processing** - Handle keyboard/mouse, update camera
2. **Packet Drain** - Process all `ChunkDataS2CPacket`, `BlockChangeS2CPacket`
3. **Mesh Scheduling** - Submit mesh build requests to `ClientWorkerPool`
4. **GPU Upload** - Process mesh results and upload to VBO/IBO (budget: 2-3ms)
5. **Culling & Render** - Frustum cull, draw visible sections in 3 passes
6. **Present** - Swap buffers, handle VSync timing

**Frame Budget**: 16.67ms (60 FPS), fallback to 30 FPS if consistently exceeded

### Server Thread (20 TPS Fixed)
**DO:**
- Drain Client→Server packets from all connections via `connection->tick()`
- Run world simulation via `World::WorldLoop()`
- Process chunk loading results from `ServerWorkerPool`
- Schedule chunk sends based on player watch sets
- Enqueue Server→Client packets for network thread transmission

**DON'T:**
- Call `io_context::poll()` or any network I/O directly
- Block on file I/O (use async loading via worker pool)
- Exceed tick time budget (causes server lag)
- Access OpenGL or client-owned resources

**Order of Operations per Tick (50ms):**
1. **Connection Drain** - `connection->tick()` on all active connections
2. **Packet Processing** - Handle `PlayerMoveC2SPacket`, `BlockActionC2SPacket`
3. **World Simulation** - `m_world->WorldLoop(deltaTime)` for game logic
4. **Chunk Management** - Process loaded chunks, update player watch sets
5. **Outbound Packets** - Send `ChunkDataS2CPacket`, `BlockChangeS2CPacket`

**Tick Budget**: 50ms hard limit, ideally <30ms for stability

### I/O Network Thread (Continuous)
**DO:**
- Run `io_context::run()` continuously with work guard
- Accept new connections asynchronously
- Read/write packets asynchronously via per-connection strands
- Decode packets and enqueue for server thread processing
- Handle protocol state changes and compression pipeline flips

**DON'T:**
- Process game logic or modify world state
- Perform OpenGL operations
- Block the I/O loop with synchronous operations
- Access main thread data without proper synchronization

**Operation Flow:**
1. **Accept Loop** - `NetworkServer::StartAccept()` for new connections
2. **Async Read** - `ServerConnection::StartRead()` for each connection
3. **Packet Decode** - Parse VarInt headers, decompress if needed
4. **Enqueue** - Add decoded packets to connection's inbound queue
5. **Async Write** - Send outbound packets via strand coordination

**Work Guard**: Prevents `io_context` from exiting when no pending work

### Client Worker Pool (2-4 threads)
**DO:**
- Process mesh build requests from `ClientMeshManager`
- Perform CPU-intensive greedy meshing algorithm
- Generate vertex/index data for opaque, cutout, translucent layers
- Submit `MeshBuildResult` to result queue for main thread
- Handle mesh build cancellation when chunks unload

**DON'T:**
- Access OpenGL resources directly
- Modify client-owned data structures without synchronization
- Exceed reasonable processing time per mesh build

**Work Distribution**: Priority-based scheduling (near-to-far from player position)

### Server Worker Pool (2-4 threads)
**DO:**
- Load chunks from Anvil files (.mca format) asynchronously
- Generate chunks procedurally when not found in storage
- Parse NBT data and convert to internal chunk format
- Submit completed chunks to server thread via result queue

**DON'T:**
- Modify server World state directly
- Access network connections or send packets
- Block indefinitely on file I/O operations

**Work Distribution**: Priority-based (chunks closer to players loaded first)

## Thread Communication Patterns

### Packet Flow (Network → Main Threads)
```
I/O Thread: Raw Bytes → VarInt Decode → Packet Object → Connection Queue
Main Thread: Drain Queue → Process Packets → Update Game State
```

### Worker Result Flow (Background → Main Threads)
```
Worker Thread: Complete Job → Create Result Object → Submit to ResultQueue  
Main Thread: Check Queue → Process Results → Apply to Game State
```

### Cross-Thread Data Access Rules
- **Read-Only Shared Data**: Configuration, static lookup tables
- **Message Passing Preferred**: Use bounded queues instead of shared mutable state
- **Strand Pattern**: Boost.Asio strands ensure single-threaded access per connection
- **No Polling in Main Loops**: I/O thread handles all async operations

## Thread Startup/Shutdown

### Startup Order
1. **Main Thread** - Initialize OpenGL context, game systems
2. **Worker Pools** - Start background threads with job queues
3. **I/O Thread** - Create `io_context`, bind to port, start work guard
4. **Server Thread** - Begin 20 TPS tick loop

### Shutdown Order
1. **Signal Stop** - Set atomic shutdown flags
2. **Server Thread** - Complete current tick, join thread
3. **I/O Thread** - Reset work guard, stop `io_context`, join thread
4. **Worker Pools** - Cancel pending jobs, join all worker threads
5. **Cleanup** - Release resources on main thread

### Thread Safety Guarantees
- **Packet Queues**: Thread-safe with bounded capacity
- **Connection State**: Protected by per-connection strand
- **Worker Results**: Thread-safe result queues with producer/consumer pattern
- **No Shared Mutation**: Prefer immutable data passed via messages