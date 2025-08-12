Player Session Management

ServerPlayerState Fields

The ServerPlayerState struct tracks all per-player information needed for chunk streaming and game simulation:

    // In src/server/IntegratedServer.hpp:36-44
    struct ServerPlayerState {
        uint32_t playerId = 0;                    // Unique player identifier
        glm::vec3 position{0.0f};                 // World position (x,y,z)
        glm::vec2 rotation{0.0f};                 // Pitch, yaw rotation
        Game::Math::ChunkPos currentChunk{0, 0};  // Chunk player is standing in

        // Chunk streaming state
        std::unordered_set<ChunkPos, ChunkPosHash> loadedChunks;  // Player's view set

        // Network synchronization  
        uint32_t lastMoveSequenceNumber = 0;     // Anti-cheat movement validation
        std::chrono::steady_clock::time_point lastUpdateTime;     // Activity tracking
    };

Field Usage and Updates

Position and Movement
- Updated via UpdatePlayerState() when PlayerMoveC2SPacket received
- Used for chunk distance calculations and view set updates
- Validated against speed limits and physics constraints

Chunk Tracking
- currentChunk updated when player crosses chunk boundaries
- loadedChunks maintains set of chunks sent to this player
- Used to determine when to send/unload chunks

Synchronization
- lastMoveSequenceNumber incremented per movement packet (prevents replay attacks)
- lastUpdateTime tracks connection activity for timeout detection

Per-Tick Processing Steps

The server processes each player during the 20 TPS tick loop with disciplined ordering:

1. Connection Packet Draining (First)


    // In IntegratedServer::ServerTick()
    if (m_networkServer) {
        auto connections = m_networkServer->GetConnections();
        for (auto& conn : connections) {
            conn->tick();  // Drain incoming packets from this player
        }
    }

What happens per connection:
- Process all queued PlayerMoveC2SPacket
- Process all queued BlockActionC2SPacket
- Process all queued ChatMessageC2SPacket
- Validate packet rates and impose limits

2. Player State Updates (Second)


    // Called from packet handlers during connection drain
    void UpdatePlayerState(const PlayerMoveC2SPacket& packet) {
        m_playerState.position = packet.position;
        m_playerState.rotation = packet.rotation;
        m_playerState.lastMoveSequenceNumber = packet.sequenceNumber;
        m_playerState.lastUpdateTime = std::chrono::steady_clock::now();

        // Check for chunk boundary crossing
        auto newChunk = WorldToChunkPos(packet.position.x, packet.position.z);
        if (newChunk != m_playerState.currentChunk) {
            UpdatePlayerChunkPosition(packet.position);
        }
    }

3. World Simulation (Third)


    // In IntegratedServer::ServerTick()
    if (m_world) {
        m_world->WorldLoop(deltaTime);
    }

World operations affecting player:
- Process block changes from BlockActionC2SPacket
- Update chunk loading around player position
- Run game logic (physics, entities, weather)
- Generate new chunks if needed

4. Chunk Streaming (Fourth)


    // During world simulation
    void UpdatePlayerChunkPosition(const glm::vec3& newPosition) {
        auto newChunk = WorldToChunkPos(newPosition.x, newPosition.z);
        if (newChunk != m_playerState.currentChunk) {
            m_playerState.currentChunk = newChunk;

            // Chunk loading happens in World::WorldLoop()
            // New chunks automatically sent when loaded
        }
    }

5. Outbound Packet Generation (Fifth)

Based on world changes and player state, enqueue packets for network transmission:

    // Send new chunks that entered view range
    void SendChunkDataS2CPacket(ChunkDataS2CPacket&& packet) {
        if (m_networkServer && !m_shouldStop.load()) {
            auto data = Network::Serialization::Serialize(packet);
            m_networkServer->BroadcastPacket(static_cast<uint8_t>(PacketId::ChunkDataS2C), data);
            // Note: In multiplayer, would send to specific connections watching this chunk
            m_stats.packetsSent++;
        }
    }

Integration with Network Layer

ServerConnection Relationship

Each ServerConnection represents a player's network link:

    class ServerConnection {
        // Player identification after login
        std::string m_playerName;
        uint32_t m_playerId = 0;
        bool m_authenticated = false;

        // Called by server thread to drain packets
        void tick() {
            auto packets = m_inboundPackets.DrainAll();
            for (const auto& packet : packets) {
                // Forward to IntegratedServer based on packet type
                ProcessPacketForPlayer(packet);
            }
        }
    };

Player Join Handling

    // In IntegratedServer::OnPlayerJoined()
        void OnPlayerJoined(std::shared_ptr<ServerConnection> connection) {
        // Initialize player state
        m_playerState.playerId = connection->GetPlayerId();
        m_playerState.position = GetSpawnPosition();
        m_playerState.currentChunk = WorldToChunkPos(m_playerState.position.x, m_playerState.position.z);

        // Send initial chunks in radius around spawn
        int chunkRadius = 8;
        for (int dx = -chunkRadius; dx <= chunkRadius; dx++) {
            for (int dz = -chunkRadius; dz <= chunkRadius; dz++) {
                ChunkPos chunkPos(m_playerState.currentChunk.x + dx, m_playerState.currentChunk.z + dz);
                RequestChunkLoad(chunkPos, static_cast<int>(chunkRadius - sqrt(dx*dx + dz*dz)));
            }
        }
    }

Watch Set Management

Chunk Watch Set Updates

The player's loadedChunks set represents their "view set" - chunks the server believes the client has:

    // When sending new chunk to player
    void SendChunkToClient(ChunkPos chunkPos, std::shared_ptr<Chunk> chunk) {
        // Create and send packet
        ChunkDataS2CPacket packet = SerializeChunk(chunk);
        SendChunkDataS2CPacket(std::move(packet));

        // Update player's loaded set
        m_playerState.loadedChunks.insert(chunkPos);
    }
    
    // When player moves far from chunk
    void CheckChunkUnloads() {
        auto playerChunk = m_playerState.currentChunk;
        int renderDistance = GetRenderDistance();

        for (auto it = m_playerState.loadedChunks.begin(); it != m_playerState.loadedChunks.end();) {
            float distance = ChunkDistance(*it, playerChunk);
            if (distance > renderDistance) {
                SendUnloadChunkS2C(*it);
                it = m_playerState.loadedChunks.erase(it);
            } else {
                ++it;
            }
        }
    }

Integration with World Chunk Management

The server coordinates between per-player view sets and global chunk loading:

    // Player tickets keep chunks loaded
    void UpdateLoadedChunks() {
        for (const auto& chunkPos : GetRequiredChunks()) {
            if (!m_world->IsChunkLoaded(chunkPos.x, chunkPos.z)) {
                // Chunk needs loading - will be sent when ready
                RequestChunkLoad(chunkPos);
            }
        }      
    }

Performance Monitoring

Player Session Statistics

    // Track per-player metrics during tick
    struct PlayerSessionStats {
        uint64_t packetsReceived = 0;      // Inbound packet count
        uint64_t packetsSent = 0;          // Outbound packet count  
        uint64_t bytesReceived = 0;        // Network bandwidth used
        uint64_t bytesSent = 0;
        float averageLatency = 0.0f;       // Keep-alive round trip time
        size_t chunksInView = 0;           // Current loaded chunk count
    };

Tick Budget Allocation

- Packet draining: 5-10ms max per tick
- Player updates: 1-2ms max per tick
- Chunk operations: 10-15ms max per tick
- Packet generation: 5-10ms max per tick
- Total player processing: <30ms per tick to maintain 20 TPS

The player session system ensures responsive multiplayer-ready architecture while efficiently managing the single-player integrated server scenario.

Implementation files:
- src/server/IntegratedServer.cpp:455-525 - Player state management
- src/server/network/ServerConnection.cpp - Network integration
- src/server/world/tracking/ - Chunk watching systems
