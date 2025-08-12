Chunk Watching System

Watchers vs Watch Sets

Global Watchers Per Chunk

Each chunk maintains a set of players who are watching it:

    // Conceptual structure (not directly implemented but logically tracked)
    struct ChunkWatchers {
        ChunkPos position;
        std::unordered_set<uint32_t> playerIds;  // Players watching this chunk
        bool hasActiveWatchers() const { return !playerIds.empty(); }
    };

Per-Player Watch Set

Each player maintains their personal set of chunks they should receive:

    // In ServerPlayerState (src/server/IntegratedServer.hpp:41)
    std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> loadedChunks;

Relationship:
- Watch Set = chunks player should have loaded
- Watchers = players who should have specific chunk loaded
- Bidirectional mapping maintained for efficient queries

Ticket System

Player Tickets Keep Chunks Loaded

When a player needs a chunk, the server creates a "PLAYER" ticket:

    // When chunk enters player's view range
    void RequestChunkLoad(ChunkPos chunkPos, int priority) {
        // Check if already loaded  
        if (m_world->IsChunkLoaded(chunkPos.x, chunkPos.z)) {
            auto chunk = m_world->GetChunk(chunkPos.x, chunkPos.z);
        if (chunk) {
            SendChunkToClient(chunkPos, chunk);
            return;
        }
    }

        // Create implicit PLAYER ticket by requesting load
        m_world->UpdateLoadedChunks(chunkPos.x, chunkPos.z);
    }

Ticket Types (Conceptual)

- PLAYER: Chunk needed by at least one player's view set
- SPAWN: Spawn chunks kept loaded permanently
- FORCED: Administratively loaded chunks
- TEMPORARY: Short-term loading for operations

Ticket Reference Counting

    // Conceptual ticket management
    class ChunkTicketManager {
        struct ChunkTickets {
        int playerTickets = 0;    // Count of players watching
        int spawnTickets = 0;     // Spawn area tickets
        int forcedTickets = 0;    // Admin force-loaded

            bool shouldKeepLoaded() const {
                return playerTickets > 0 || spawnTickets > 0 || forcedTickets > 0;
            }
        };

        std::unordered_map<ChunkPos, ChunkTickets> m_tickets;
    };

Chunk Sending Logic

Send Criteria

A chunk is sent to a player when ALL conditions are met:
1. In Watch Set: Chunk position within player's render distance
2. Chunk Status: Chunk status == FULL (all sections loaded)
3. Light Ready: Light calculations completed for chunk
4. Not Already Sent: Chunk not in player's loadedChunks set


    // In IntegratedServer::SendChunkToClient()
    void SendChunkToClient(ChunkPos chunkPos, std::shared_ptr<Chunk> chunk) {
        if (!chunk || !chunk->IsFullyLoaded()) {
            return; // Wait until chunk is ready
        }

        // Create ChunkDataS2C packet
        ChunkDataS2CPacket packet;
        packet.chunkX = chunkPos.x;
        packet.chunkZ = chunkPos.z;
        packet.primaryBitmask = chunk->GetSectionBitmask(); // 24-bit mask for 1.18+ (Y:-64..319)
        packet.sections = SerializeChunkSections(chunk);

        // Send via network
        SendChunkDataS2CPacket(std::move(packet));

        // Update player's watch set
        m_playerState.loadedChunks.insert(chunkPos);
        m_stats.chunksSent++;

## ChunkDataS2C Wire Format (1.18+ Compatibility)

For precise Minecraft 1.18+ compatibility, ChunkDataS2C packets contain:
- **Ground-up flag**: Boolean indicating full chunk vs. partial update
- **24-bit section bitmask**: One bit per section (Y levels -64 to 319)
- **Heightmap data**: NBT-encoded heightmaps (MOTION_BLOCKING, etc.)
- **Biomes array**: 4×4×4 biomes per section (1024 biomes per chunk)
- **Section data**: Per-section with palette encoding:
  - Block count (short)
  - Block palette with VarInt indices
  - Bit-packed block array
  - Light data (skylight + blocklight)
    }
    
    Send Priority (Near→Far)

    // In IntegratedServer::GetRequiredChunks()
    std::vector<ChunkPos> GetRequiredChunks() const {
        std::vector<ChunkPos> chunks;
        auto playerChunk = m_playerState.currentChunk;
        int renderDistance = GetRenderDistance();

        // Generate all chunks in range
        for (int dx = -renderDistance; dx <= renderDistance; ++dx) {
            for (int dz = -renderDistance; dz <= renderDistance; ++dz) {
                chunks.push_back({playerChunk.x + dx, playerChunk.z + dz});
            }
        }

        // Sort by distance (near chunks sent first)
        std::sort(chunks.begin(), chunks.end(), [this](const ChunkPos& a, const ChunkPos& b) {
            return CalculateChunkDistance(a) < CalculateChunkDistance(b);
         });

        return chunks;
    }

Chunk Removal Logic

Unload Criteria

A chunk is unloaded from a player when:
1. Distance Check: Chunk exceeds render distance from player
2. No Other Watchers: No other players need this chunk
3. Grace Period: Chunk has been out of range for minimum time


    // Called during player position updates
    void UpdatePlayerChunkPosition(const glm::vec3& newPosition) {
        auto newChunk = WorldToChunkPos(newPosition.x, newPosition.z);
        if (newChunk != m_playerState.currentChunk) {
            m_playerState.currentChunk = newChunk;

            // Check for chunks to unload
            CheckChunkUnloads();
        }
    }
    
    void CheckChunkUnloads() {
        int renderDistance = GetRenderDistance();

        for (auto it = m_playerState.loadedChunks.begin(); it != m_playerState.loadedChunks.end();) {
            float distance = CalculateChunkDistance(*it);
            if (distance > renderDistance + 2) { // 2-chunk grace buffer
                SendUnloadChunkS2C(*it);
                it = m_playerState.loadedChunks.erase(it);
            } else {
                ++it;
            }
        }
    }

Unload Packet Sending
    
    // Send UnloadChunkS2C when removing from watch set
    void SendUnloadChunkS2C(ChunkPos chunkPos) {
        UnloadChunkS2CPacket packet;
        packet.chunkX = chunkPos.x;
        packet.chunkZ = chunkPos.z;

        // Send to client
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(PacketId::UnloadChunkS2C), data);
            // Note: In multiplayer, this would be sent per-connection to chunk watchers only
        }
    }

Watch Set Synchronization

Initial Load on Join

When player joins, server sends chunks around spawn position:

    // In IntegratedServer::OnPlayerJoined()
    void OnPlayerJoined(std::shared_ptr<ServerConnection> connection) {
        auto spawnChunk = WorldToChunkPos(m_playerState.position.x, m_playerState.position.z);
        int initialRadius = 8; // chunks

        // Send chunks in spiral pattern (center-out)
        for (int radius = 0; radius <= initialRadius; radius++) {
            for (int dx = -radius; dx <= radius; dx++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    if (abs(dx) == radius || abs(dz) == radius) { // Only edge chunks for this radius
                        ChunkPos chunkPos(spawnChunk.x + dx, spawnChunk.z + dz);
                        RequestChunkLoad(chunkPos, initialRadius - radius); // Higher priority for closer chunks
                    }
                }
            }
        }
    }

Differential Updates

Only send chunks that changed since last update:

    // Track what was sent vs what should be sent
    void UpdatePlayerWatchSet() {
        auto requiredChunks = GetRequiredChunks();
        std::unordered_set<ChunkPos> requiredSet(requiredChunks.begin(), requiredChunks.end());

        // Send new chunks (in required but not in loaded)
        for (const auto& chunkPos : requiredChunks) {
            if (m_playerState.loadedChunks.find(chunkPos) == m_playerState.loadedChunks.end()) {
                RequestChunkLoad(chunkPos);
            }
        }

        // Unload old chunks (in loaded but not in required)
        for (auto it = m_playerState.loadedChunks.begin(); it != m_playerState.loadedChunks.end();) {
            if (requiredSet.find(*it) == requiredSet.end()) {
                SendUnloadChunkS2C(*it);
                it = m_playerState.loadedChunks.erase(it);
            } else {
                ++it;
            }
        }
    }

Block Change Propagation

Send to Watching Players

When a block changes, notify all players who can see the affected chunk:

    // In IntegratedServer::ApplyBlockChange()
    void ApplyBlockChange(int worldX, int worldY, int worldZ, BlockID blockId) {
        if (!m_world->SetBlock(worldX, worldY, worldZ, blockId)) {
            return; // Block change failed
        }

        // Determine affected chunk
        auto chunkPos = WorldToChunkPos(worldX, worldZ);

        // Send to all players watching this chunk
        // (In single-player, just send to the one player)
        if (m_playerState.loadedChunks.count(chunkPos)) {
            BlockChangeS2CPacket packet;
            packet.worldX = worldX;
            packet.worldY = worldY;
            packet.worldZ = worldZ;
            packet.newBlockId = blockId;
            SendPacketToClient(std::move(packet));
        }
    }

Batch Block Changes

For efficiency, multiple block changes in same chunk can be batched:

    // Send MultiBlockChangeS2C for multiple blocks  
    void SendMultiBlockChange(ChunkPos chunkPos, const std::vector<BlockChange>& changes) {
        if (m_playerState.loadedChunks.count(chunkPos)) {
            MultiBlockChangeS2CPacket packet;
            packet.chunkPos = chunkPos;
            packet.changes = changes;
            SendPacketToClient(std::move(packet));
        }
    }

The chunk watching system ensures efficient, consistent world streaming with minimal network overhead and proper resource management.

Implementation references:
- src/server/IntegratedServer.cpp:477-525 - Player join and chunk streaming
- src/server/IntegratedServer.cpp:547-559 - Chunk position updates
- src/server/world/tracking/ - Dirty tracking and change propagation
