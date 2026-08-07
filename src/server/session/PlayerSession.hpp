// File: src/server/session/PlayerSession.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/BlockInteraction.hpp"  // Game::UseResult
#include "common/network/PacketTypes.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include <glm/glm.hpp>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <queue>
#include <memory>
#include <chrono>
#include <mutex>
#include <atomic>

namespace Game {
    class World;
}

namespace Server {

    // Forward declarations
    class ServerPlayer;
    class ServerConnection;
    class ChunkTicketManager;
    class ChunkWatchIndex;
    class ChunkStatusManager;
    class SendScheduler;

    // Coalesced block changes for a chunk section
    struct SectionDiffs {
        Game::Math::ChunkPos chunkPos;
        int sectionIndex;
        std::unordered_map<uint32_t, Game::BlockID> changes; // localIndex -> blockId
        std::chrono::steady_clock::time_point lastUpdate;
        
        uint32_t MakeIndex(uint8_t x, uint8_t y, uint8_t z) const {
            return (y << 8) | (z << 4) | x;
        }
        
        void AddChange(uint8_t x, uint8_t y, uint8_t z, Game::BlockID blockId) {
            changes[MakeIndex(x, y, z)] = blockId;
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    // Player session managing watch sets and streaming
    class PlayerSession {
    public:
        // Configuration
        struct Config {
            int simulationDistance = 8;    // Chunks kept loaded around player
            int viewDistance = 8;          // Chunks sent to client (≤ simulationDistance)
            int maxChunksPerTick = 12;     // Max chunks to send per tick
            int maxBytesPerTick = 1048576; // 1MB per tick max
            int maxDiffBytesPerTick = 524288; // 512KB for block changes per tick
            bool compressPackets = true;   // Enable compression
            int compressionLevel = 3;      // zlib compression level (1-9)
        };

        // Session state
        enum class State {
            CONNECTING,      // Initial handshake
            AUTHENTICATING,  // Login process
            JOINING,         // Sending join game packet
            PLAYING,         // Normal gameplay
            RESPAWNING,      // Respawn in progress
            DISCONNECTING    // Cleanup in progress
        };

        PlayerSession(uint32_t playerId, uint32_t connectionId);
        ~PlayerSession();

        // === LIFECYCLE ===

        // Initialize session after successful login
        void Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos);
        
        // Attach player entity to this session
        void AttachPlayer(ServerPlayer* player);
        
        // Update connection ID (for integrated server late binding)
        void SetConnectionId(uint32_t connectionId) { m_connectionId = connectionId; }

        // Set send scheduler for packet delivery
        void SetSendScheduler(SendScheduler* scheduler) { m_sendScheduler = scheduler; }
        
        // Detach player entity (on disconnect)
        void DetachPlayer();
        
        // Set connection reference
        void SetConnection(ServerConnection* connection) { m_connection = connection; }
        
        // Process one server tick
        void Tick(int64_t serverTick);
        
        // Cleanup on disconnect
        void Cleanup();

        // === PLAYER STATE (delegated to ServerPlayer) ===

        // Update player position (from client packets) - delegates to ServerPlayer
        void UpdatePosition(const glm::vec3& position, const glm::vec2& rotation);
        
        // Update player's chunk position for view management
        void UpdateChunkPosition(Game::Math::ChunkPos newChunk);
        
        // Change dimension
        void ChangeDimension(int newDimensionId, const glm::vec3& targetPos);
        
        // Respawn player
        void Respawn(const glm::vec3& spawnPos);

        // === VIEW CONFIGURATION ===

        // Update view distance (client request or server override)
        void SetViewDistance(int distance);
        
        // Update simulation distance (server config)
        void SetSimulationDistance(int distance);

        // === WATCH SET MANAGEMENT ===

        // Compute and apply watch set changes
        void UpdateWatchSet();
        
        // Check if chunk is in watch set
        bool IsWatching(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk has been sent
        bool HasSentChunk(Game::Math::ChunkPos chunk) const;

        // Get count of sent chunks
        size_t GetSentChunkCount() const { return m_sentChunks.size(); }

        // Mark a chunk as sent (for legacy IntegratedServer path)
        void MarkChunkSent(Game::Math::ChunkPos pos) { m_sentChunks.insert(pos); }

        // === CHUNK SENDER (Minecraft's PlayerChunkSender) ===

        // Mark a chunk as loaded and ready to send (moves from pending loads to pending sends)
        void MarkChunkReadyToSend(Game::Math::ChunkPos pos);

        // Drop a chunk: remove from pending, or send unload if already sent
        void DropChunk(Game::Math::ChunkPos pos);

        // Send next batch of chunks (called once per tick from IntegratedServer)
        void SendNextChunks(Game::World* world);

        // Handle client's batch acknowledgment (updates send rate)
        void OnChunkBatchAck(float desiredRate);

        // Check if this session is waiting for a chunk to load
        bool IsWaitingForChunk(Game::Math::ChunkPos pos) const { return m_pendingChunkLoads.count(pos) > 0; }

        // Get pending chunk loads (chunks waiting for generation/loading)
        const std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>& GetPendingChunkLoads() const { return m_pendingChunkLoads; }
        size_t GetPendingChunkLoadsCount() const { return m_pendingChunkLoads.size(); }

        // Chunk sender getters
        size_t GetPendingChunksToSendCount() const { return m_pendingChunksToSend.size(); }
        float GetDesiredChunksPerTick() const { return m_desiredChunksPerTick; }
        int GetUnackedBatches() const { return m_unackedBatches; }
        float GetBatchQuota() const { return m_batchQuota; }
        int GetMaxUnackedBatches() const { return m_maxUnackedBatches; }
        const auto& GetPendingChunksToSend() const { return m_pendingChunksToSend; }

        // Send unload packet for chunk
        void SendChunkUnload(Game::Math::ChunkPos chunk);

        // === BLOCK UPDATES ===

        // Queue block change diff
        void QueueBlockChange(int worldX, int worldY, int worldZ, Game::BlockID newBlock);
        
        // Queue multi-block change for a section
        void QueueSectionChanges(Game::Math::ChunkPos chunk, int section, 
                                const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes);
        
        // Process and send diffs for this tick
        void ProcessDiffs(SendScheduler* scheduler);

        // === PACKET HANDLING ===

        // Handle incoming packets (delegates gameplay to ServerPlayer)
        void HandlePlayerMove(const Network::PlayerMoveC2SPacket& packet);
        // Fall-distance + exhaustion accounting off the move packet (called
        // by HandlePlayerMove before the position write).
        void UpdateMovementStats(const Network::PlayerMoveC2SPacket& packet);
        void HandleBlockAction(const Network::BlockActionC2SPacket& packet);
        void HandleUseItemOn(const Network::UseItemOnC2SPacket& packet);  // Minecraft-correct naming
        // Use item in air — mirrors ServerGamePacketListenerImpl.handleUseItem
        // (ServerGamePacketListenerImpl.java:1329-1354) + the useItem game-mode
        // logic (ServerPlayerGameMode.java:290-327).
        void HandleUseItem(const Network::UseItemC2SPacket& packet);
        // Release-use / drop / swap-offhand — mirrors handlePlayerAction
        // (ServerGamePacketListenerImpl.java:1191-1248).
        void HandlePlayerAction(const Network::PlayerActionC2SPacket& packet);
        void HandleHeldItemChange(const Network::HeldItemChangeC2SPacket& packet);
        void HandleKeepAlive(const Network::KeepAliveC2SPacket& packet);
        void HandleInventoryClick(const Network::InventoryClickC2SPacket& packet);
        void HandleInventoryClose(const Network::InventoryCloseC2SPacket& packet);
        // Fly-state toggle — mirrors handlePlayerAbilities
        // (ServerGamePacketListenerImpl.java: only the FLYING bit is honored,
        // and only when mayFly; otherwise a corrective abilities resend).
        void HandlePlayerAbilities(const Network::PlayerAbilitiesC2SPacket& packet);

        // Send full 46-slot inventory snapshot to this player's client.
        void SendInventoryFull();

        // The `Item.use` dispatch for one hand — mirrors
        // ServerPlayerGameMode.useItem (ServerPlayerGameMode.java:290-327):
        // runs the per-item `use` callback (or the component-driven
        // Item_DefaultUse), then applies MC's result-stack rules. Called from
        // HandleUseItem (air use) and from HandleUseItemOn's tail step (MC's
        // client falls through useItemOn → useItem locally, Minecraft.java:1656;
        // we run the fallthrough server-side instead). Returns the item's
        // UseResult so HandleUseItemOn's fallthrough can ack correctly.
        Game::UseResult DispatchUseItem(uint32_t hand);
        
        // Helper for resyncing on placement failure
        void ResyncAndAck(const glm::ivec3& clicked, const glm::ivec3& target, uint32_t sequence);
        
        // Send packets to client
        void SendPositionSync(); // Send authoritative position
        void SendBlockUpdate(const glm::ivec3& pos, Game::BlockID block);
        void SendSingleBlockChange(const Network::BlockChangeS2CPacket& packet);
        void SendSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);
        void SendInventoryUpdate(int slot); // TODO: Implement with inventory system
        void AckInteraction(uint32_t sequence, bool success);

        // Container revision (MC AbstractContainerMenu.stateId). Bumped
        // whenever the server mutates the player's inventory or cursor, and
        // stamped onto every InventoryFullS2C so the client can tell a fresh
        // authoritative snapshot from a stale one.
        void BumpContainerState() { ++m_containerStateId; }

        // Diff the authoritative container against m_remote* (our model of
        // what the client believes) and send ONLY the slots that disagree,
        // updating the model as we go. Mirrors MC
        // AbstractContainerMenu.broadcastChanges + synchronizeCarriedToRemote.
        void BroadcastContainerChanges();
        uint32_t ContainerStateId() const { return m_containerStateId; }

        // Force the next BroadcastContainerChanges to resend `index` even
        // though the authoritative stack has not changed. Needed when the
        // CLIENT mutated its copy on a prediction the server rejected: the
        // server's slot still matches m_remoteSlots, so the plain diff would
        // find nothing to correct and the bad prediction would stick.
        // MC's equivalent is RemoteSlot.force with a non-matching value.
        void InvalidateRemoteSlot(int index);

        // Client block-prediction acknowledgement (MC
        // ServerGamePacketListenerImpl.ackBlockChangesUpTo). Interaction
        // handlers only RECORD the sequence; the packet is emitted by
        // FlushBlockChangeAck once this tick's block updates have gone out.
        // Sending it any earlier lets the client retire a prediction before
        // the correcting block update arrives, which shows up as a flicker.
        void AckBlockChangesUpTo(uint32_t sequence);
        void FlushBlockChangeAck();


        // Track packet acknowledgments
        void OnChunkSendComplete(Game::Math::ChunkPos chunk);
        void OnChunkUnloadComplete(Game::Math::ChunkPos chunk);

        // === STATISTICS ===

        struct Stats {
            // Streaming stats
            size_t chunksInWatch = 0;
            size_t chunksSent = 0;
            size_t chunksInFlight = 0;
            size_t chunksPending = 0;
            
            // Bandwidth stats
            size_t bytesOutThisTick = 0;
            size_t totalBytesOut = 0;
            float averageBytesPerTick = 0;
            
            // Diff stats
            size_t diffsQueued = 0;
            size_t diffsSent = 0;
            size_t diffsDropped = 0;
            
            // Timing stats
            float lastTickTime = 0;
            float averageTickTime = 0;
            
            // Connection stats
            float latency = 0;
            std::chrono::steady_clock::time_point lastKeepAlive;
        };
        
        Stats GetStats() const;
        void ResetStats();

        // === GETTERS ===

        uint32_t GetPlayerId() const { return m_playerId; }
        uint32_t GetConnectionId() const { return m_connectionId; }
        ServerPlayer* GetPlayer() const { return m_player; }
        ServerConnection* GetConnection() const { return m_connection; }
        State GetState() const { return m_state; }
        
        // Position getters (delegate to ServerPlayer if attached)
        glm::vec3 GetPosition() const;
        glm::vec2 GetRotation() const;
        int GetDimensionId() const;
        
        // View management getters
        Game::Math::ChunkPos GetChunkPosition() const { return m_currentChunk; }
        Game::Math::ChunkPos GetAnchorChunk() const { return m_anchorChunk; }
        
        int GetViewDistance() const { return m_viewDistance; }
        int GetSimulationDistance() const { return m_simulationDistance; }

        // Watch delta getters (consumed by PlayerSessionManager to update ChunkWatchIndex)
        const std::vector<Game::Math::ChunkPos>& GetPendingWatchAdds() const { return m_pendingWatchAdds; }
        const std::vector<Game::Math::ChunkPos>& GetPendingWatchRemoves() const { return m_pendingWatchRemoves; }
        void ClearPendingWatchDeltas() { m_pendingWatchAdds.clear(); m_pendingWatchRemoves.clear(); }

    private:
        // === IDENTIFIERS ===
        uint32_t m_playerId;
        uint32_t m_connectionId;
        
        // === REFERENCES ===
        ServerPlayer* m_player = nullptr;        // Non-owning pointer to player entity
        ServerConnection* m_connection = nullptr; // Non-owning pointer to network connection
        SendScheduler* m_sendScheduler = nullptr; // For sending packets (set by PlayerSessionManager)
        
        // === STATE ===
        std::atomic<State> m_state{State::CONNECTING};
        Config m_config;
        
        // === VIEW POSITION ===
        // Note: Authoritative position is in ServerPlayer, these are for view management
        Game::Math::ChunkPos m_currentChunk{0, 0};
        Game::Math::ChunkPos m_anchorChunk{0, 0}; // Center for watch calculations
        Game::Math::ChunkPos m_lastKnownChunk{0, 0};
        
        // === DISTANCES ===
        int m_simulationDistance = 8;
        int m_viewDistance = 8;
        
        // === WATCH SETS ===
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_watchSet;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_sentChunks;

        // === CHUNK SENDER STATE (Minecraft's PlayerChunkSender) ===
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_pendingChunkLoads;    // Chunks waiting for generation/loading
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_pendingChunksToSend;  // Chunks loaded and ready to send
        float m_desiredChunksPerTick = 9.0f;
        float m_batchQuota = 0.0f;
        int m_unackedBatches = 0;
        int m_maxUnackedBatches = 1;  // Bumps to 10 after first ack

        // === WATCH INDEX SYNCHRONIZATION ===
        // Deltas to apply to ChunkWatchIndex (consumed by PlayerSessionManager)
        std::vector<Game::Math::ChunkPos> m_pendingWatchAdds;
        std::vector<Game::Math::ChunkPos> m_pendingWatchRemoves;
        
        // === DIFF MANAGEMENT ===
        std::unordered_map<Game::Math::ChunkPos, 
                          std::unordered_map<int, SectionDiffs>, 
                          Game::Math::ChunkPosHash> m_pendingDiffs;
        std::queue<std::pair<Game::Math::ChunkPos, int>> m_diffQueue; // (chunk, section) pairs
        
        // === BUDGETS ===
        size_t m_bytesOutThisTick = 0;
        size_t m_chunksOutThisTick = 0;
        size_t m_diffsOutThisTick = 0;
        
        // === STATISTICS ===
        mutable std::mutex m_statsMutex;
        Stats m_stats;
        std::chrono::steady_clock::time_point m_lastTickTime;
        
        // === TIMING ===
        std::chrono::steady_clock::time_point m_lastKeepAliveRx;
        std::chrono::steady_clock::time_point m_lastKeepAliveTx;
        int64_t m_lastServerTick = 0;
        
        // === INTERACTION TRACKING ===
        uint32_t m_lastInteractionSequence = 0;  // For acknowledging client predictions

        // Highest interaction sequence processed since the last ack flush.
        // 0 = nothing to ack (client sequences start at 1).
        uint32_t m_ackBlockChangesUpTo = 0;

        // Monotonic container revision — see BumpContainerState.
        uint32_t m_containerStateId = 1;

        // Server-side model of the CLIENT's container state (MC
        // AbstractContainerMenu.remoteSlots / remoteCarried). Updated from
        // three places: what we send it, and what the client tells us it
        // predicted. Diffing the truth against this is what lets a correct
        // prediction cost zero packets while still catching a slot the client
        // wrongly wrote — the client reports that write, so the model shows
        // the disagreement.
        std::array<Game::ItemStack, 46> m_remoteSlots{};
        Game::ItemStack                 m_remoteCarried{};

        // Last-sent stat triple for the SetHealthS2C dirty-check — mirrors
        // ServerPlayer.lastSentHealth / lastSentFood / lastSaturationLevel.
        // Health init -1e8 forces a send on the first PLAYING tick.
        float m_lastSentHealth     = -1.0e8f;
        int   m_lastSentFood       = -1;
        float m_lastSentSaturation = -1.0f;
        
        // === FLAGS ===
        bool m_isChangingDimension = false;
        bool m_isRespawning = false;
        bool m_needsWatchUpdate = true;
        
        // === INTERNAL METHODS ===
        
        // Watch set computation
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> 
            ComputeWatchSet(Game::Math::ChunkPos anchor, int viewDistance) const;
        
        // Watch delta computation
        void ComputeWatchDeltas(
            const std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>& newWatch,
            std::vector<Game::Math::ChunkPos>& toAdd,
            std::vector<Game::Math::ChunkPos>& toRemove
        ) const;
        
        // Diff coalescing
        void CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                uint8_t localX, uint8_t localY, uint8_t localZ,
                                Game::BlockID blockId);
        
        // Packet size estimation
        size_t EstimatePacketSize(const Network::ChunkDataS2CPacket& packet) const;
        size_t EstimatePacketSize(const Network::MultiBlockChangeS2CPacket& packet) const;
        
        // Cleanup helpers
        void ClearWatchSets();
        void ClearQueues();
        void ClearDiffs();
    };

} // namespace Server