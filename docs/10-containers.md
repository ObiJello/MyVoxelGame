# System Containers (C4 Level 2)

## Container Overview

MyVoxelGame consists of three primary containers running in a single process, coordinated through message queues and shared memory.

```mermaid
graph TB
    subgraph "MyVoxelGame Process"
        subgraph "Client Container"
            CR[Client Render Thread<br/>~60 FPS]
            CW[Client Worker Pool<br/>Mesh Building]
            CM[ClientChunkManager]
            CMM[ClientMeshManager]
        end
        
        subgraph "Server Container" 
            ST[Server Thread<br/>20 TPS]
            SW[Server Worker Pool<br/>Chunk Loading]
            IS[IntegratedServer]
            W[World]
        end
        
        subgraph "Network Container"
            IO[I/O Thread<br/>Continuous]
            NS[NetworkServer]
            SC[ServerConnections]
        end
    end
    
    subgraph "External Systems"
        GPU[GPU/OpenGL]
        FS[File System<br/>Anvil .mca files]
        NET[Network Interface<br/>localhost:25565]
    end
    
    %% Data Flow
    CR --> GPU
    ST --> FS
    IO <--> NET
    
    %% Inter-container Communication  
    ST -.-> CR : "ChunkDataS2C packets"
    CR -.-> ST : "PlayerMove, BlockAction packets"
    IO -.-> ST : "Decoded packets (tick drain)"
    ST -.-> IO : "Outbound packets (strand queuing)"
    
    %% Worker Communication
    CW -.-> CR : "Mesh build results"
    SW -.-> ST : "Loaded chunks"
    
    classDef clientStyle fill:#e1f5fe
    classDef serverStyle fill:#f3e5f5  
    classDef networkStyle fill:#e8f5e8
    classDef externalStyle fill:#fff3e0
    
    class CR,CW,CM,CMM clientStyle
    class ST,SW,IS,W serverStyle
    class IO,NS,SC networkStyle
    class GPU,FS,NET externalStyle
```

## Client Container
**Responsibility**: Real-time rendering, input processing, and mesh management
- **Primary Thread**: Client render thread running at ~60 FPS target
- **Key Components**:
    - `ClientChunkManager` - Manages client-side chunk state (UNLOADED→LOADED)
    - `ClientMeshManager` - Coordinates mesh building and GPU uploads
    - Input system, camera, and OpenGL rendering pipeline
- **Worker Pool**: Background mesh building (CPU-intensive greedy meshing)
- **External Dependencies**: GPU/OpenGL for rendering

### Threading Model
- **Main Loop**: Input → Packet Drain → Mesh Scheduling → GPU Upload → Render → Present
- **Frame Budget**: 16.67ms target (60 FPS), with fallback to 30 FPS if exceeded
- **GPU Coordination**: VBO/IBO uploads limited to 2-3ms per frame

## Server Container
**Responsibility**: Game world simulation, chunk management, and player state
- **Primary Thread**: Server thread running at fixed 20 TPS (50ms intervals)
- **Key Components**:
    - `IntegratedServer` - Main server controller and player session management
    - `World` - Block storage, chunk loading, and game logic
    - `ChunkProvider` - Interfaces with storage and generation systems
- **Worker Pool**: Background chunk loading from Anvil files or procedural generation
- **External Dependencies**: File system for world storage (.mca region files)

### Threading Model
- **Tick Loop**: Connection Drain → World Simulation → Outbound Packet Enqueue
- **Tick Budget**: 50ms hard limit (20 TPS), ideally under 30ms for stability
- **Storage I/O**: Async chunk loading via `ServerWorkerPool` to avoid blocking ticks

## Network Container
**Responsibility**: Client-server communication using Minecraft protocol
- **Primary Thread**: I/O thread running `boost::asio::io_context::run()`
- **Key Components**:
    - `NetworkServer` - TCP acceptor and connection management
    - `ServerConnection` - Per-connection packet handling and protocol state
    - Packet encoding/decoding, compression, and framing
- **External Dependencies**: Network interface (binds to localhost:25565)

### Threading Model
- **I/O Loop**: Continuous `io_context::run()` with work guard to prevent exit
- **No Polling**: Server tick thread never calls `poll()` - I/O thread handles all async operations
- **Pipeline Management**: Protocol state changes and compression flips happen on I/O thread

## Inter-Container Communication

### Client ↔ Server
- **Method**: Message queues with packet serialization
- **Client→Server**: `PlayerMoveC2SPacket`, `BlockActionC2SPacket` via network
- **Server→Client**: `ChunkDataS2CPacket`, `BlockChangeS2CPacket` via network
- **Processing**: Packets decoded on I/O thread, applied on respective main threads

### Worker Pool Integration
- **Client Workers**: Submit `MeshBuildResult` to `ResultQueue`, processed during client frame
- **Server Workers**: Submit loaded chunks to server thread for integration into `World`
- **Queue Bounds**: All queues have size limits to prevent memory exhaustion

### Thread Safety
- **Strand Pattern**: Each `ServerConnection` uses Boost.Asio strand for thread-safe async operations
- **Message Queues**: Thread-safe with bounded capacity and back-pressure handling
- **Shared Data**: Minimal shared state, prefer message passing over shared memory

## External System Integration

### GPU (OpenGL)
- **Context**: Single OpenGL context owned by client render thread
- **Upload Pattern**: Mesh data uploaded as VBO/IBO during frame processing
- **Memory Management**: GPU resources cleaned up when chunks unload

### File System (Anvil Format)
- **Read Path**: Minecraft world files (.mca) loaded via `ServerWorkerPool`
- **Write Path**: Async chunk saving to maintain world persistence
- **Format**: Compatible with Minecraft Java Edition 1.18+ world format

### Network (Localhost)
- **Protocol**: Custom packet format based on Minecraft protocol structure
- **Transport**: TCP on localhost:25565 for singleplayer (Note: Vanilla Minecraft uses in-process LocalChannel for singleplayer; TCP binding is our current implementation)
- **Future**: Should add LocalConnection path alongside TCP for true Minecraft fidelity; architecture supports remote connections with minimal changes