ClientChunkManager

Data Layout and Storage

Chunk Storage Structure

    // Per-chunk data structure
    struct ClientChunk {
        Game::Math::ChunkPos position;           // (x, z) coordinates
        std::shared_ptr<Game::Chunk> chunkData;  // Block data (16x16x384)
        ChunkState state;                        // UNLOADED or LOADED

        // Timing metadata
        std::chrono::steady_clock::time_point loadTime;
        std::chrono::steady_clock::time_point lastAccessTime;

        // Dirty tracking for mesh rebuilds
        std::unordered_set<int> dirtySections;   // Y indices of modified sections
    };

Thread-Safe Container

    // src/client/world/ClientChunkManager.hpp:129-131
    mutable std::mutex m_chunksMutex;
    std::unordered_map<ChunkPos, std::unique_ptr<ClientChunk>, ChunkPosHash> m_chunks;

API Reference

Chunk State Management

ProcessChunkDataS2CPacket() - Primary entry point for server data
void ProcessChunkDataS2CPacket(const Network::ChunkDataS2CPacket& packet);
- Enqueued by: I/O network thread when packet received
- Applied on: Client render thread after draining packet queue
- Function: Deserializes packet data, creates/updates ClientChunk
- State transition: UNLOADED → LOADED
- Integration: Updates lastAccessTime, clears dirtySections

ProcessBlockChange() - Handle individual block modifications
void ProcessBlockChange(const Network::BlockChangeS2CPacket& packet);
- Enqueued by: I/O network thread for block updates
- Applied on: Client render thread after draining packet queue
- Function: Updates single block in loaded chunk
- Side effect: Calls MarkSectionDirty() for affected section
- Performance: O(1) block lookup within chunk

Access and Queries

GetChunk() - Thread-safe chunk retrieval
ClientChunk* GetChunk(Game::Math::ChunkPos chunkPos);
const ClientChunk* GetChunk(Game::Math::ChunkPos chunkPos) const;
- Thread safety: Protected by m_chunksMutex
- Return value: nullptr if chunk not loaded or doesn't exist
- Usage: Called frequently by ClientMeshManager and rendering code

IsChunkLoaded() - Quick state check
bool IsChunkLoaded(Game::Math::ChunkPos chunkPos) const;
- Performance: O(1) hash lookup + state check
- Thread safety: Mutex-protected read
- Common use: Before scheduling mesh builds or rendering

Dirty Tracking System

When blocks change, sections become "dirty" and need mesh rebuilds:

MarkSectionDirty() - Flag section for mesh rebuild
void MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY);
- Parameters: sectionY ranges from 0-23 (bottom to top of chunk)
- Effect: Adds sectionY to chunk's dirtySections set
- Integration: ClientMeshManager polls dirty sections for rebuild scheduling

MarkChunkDirty() - Flag entire chunk (all 24 sections)
void MarkChunkDirty(Game::Math::ChunkPos chunkPos);
- Use cases: Complete chunk reload, lighting changes affecting multiple sections
- Implementation: Calls MarkSectionDirty() for sections 0-23

Integration Points

With NetworkClient

Network thread calls into ClientChunkManager when packets arrive:

    // In client packet handler
    void OnChunkDataS2C(const ChunkDataS2CPacket& packet) {
        g_clientChunkManager->ProcessChunkDataS2CPacket(packet);
    }
    
    void OnBlockChangeS2C(const BlockChangeS2CPacket& packet) {
        g_clientChunkManager->ProcessBlockChange(packet);
    }

With ClientMeshManager

Mesh system queries chunk data and dirty state:

    // In mesh scheduling
    for (auto& chunk : nearbyChunks) {
        const ClientChunk* clientChunk = g_clientChunkManager->GetChunk(chunk.pos);
        if (clientChunk && clientChunk->IsLoaded()) {
            // Check for dirty sections needing rebuild
            if (!clientChunk->dirtySections.empty()) {
                ScheduleSectionMeshBuilds(chunk.pos, clientChunk->dirtySections);
            }
        }
    }

With Rendering Pipeline

Renderer checks chunk availability before drawing:

    // In chunk rendering
    bool IsChunkReadyToRender(ChunkPos pos) {
        return g_clientChunkManager->IsChunkLoaded(pos) &&
        g_clientMeshManager->HasGPUData(pos);
    }

Memory Management

Chunk Lifecycle

1. Allocation: ProcessChunkDataS2CPacket() creates new ClientChunk
2. Access tracking: lastAccessTime updated on each GetChunk() call
3. Cleanup: UnloadChunk() called when server sends unload packet

Memory Footprint per Chunk

- Block data: ~192KB (16×16×384 = 98,304 blocks × 2 bytes per BlockState)
- Lighting data: ~96KB (skylight + blocklight, 4 bits each)
- Metadata: ~200 bytes (position, timestamps, dirty tracking)  
- Total per chunk: ~290KB active memory

Unloading Strategy

    void UnloadChunk(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_chunksMutex);

        auto it = m_chunks.find(chunkPos);
        if (it != m_chunks.end()) {
            // Notify mesh manager to clean up GPU resources
            g_clientMeshManager->RemoveChunkGPUData(chunkPos);
    
            // Remove from storage  
            m_chunks.erase(it);
        }
    }

Thread Safety Guarantees

Reader-Writer Pattern

- Multiple readers: GetChunk(), IsChunkLoaded() can be called concurrently
- Exclusive writers: ProcessChunkDataS2CPacket(), UnloadChunk() require exclusive access
- Protection: std::mutex m_chunksMutex guards all container operations

Lock-Free Operations

- State queries: Atomic operations where possible to reduce lock contention
- Dirty tracking: Per-chunk dirty sets minimize lock scope
- Access time: Updated optimistically to reduce mutex pressure

Integration with Main Thread

All ClientChunkManager operations are designed to be called from the client render thread, with I/O thread enqueuing packets that are processed by the client render thread during packet draining. The mutex ensures thread safety during packet reception.

Implementation file: src/client/world/ClientChunkManager.cpp
