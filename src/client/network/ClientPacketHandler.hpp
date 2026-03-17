// File: src/client/network/ClientPacketHandler.hpp
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include <memory>
#include <chrono>
#include <algorithm>

namespace Game {
    class ClientPlayer;
}

namespace Client {

    // Forward declarations
    class ClientChunkManager;
    class NetworkClient;

    // Handles incoming packets on the main thread
    // All methods here run on the render thread and can safely modify game state
    class ClientPacketHandler : public Network::IPacketListener {
    public:
        ClientPacketHandler();
        ~ClientPacketHandler();

        // Set client player reference for inventory sync
        void SetPlayer(Game::ClientPlayer* player) { m_player = player; }
        
        // IPacketListener interface
        const char* getName() const override { return "ClientPacketHandler"; }
        
        // Visitor pattern packet handlers (called from packet apply())
        void onChunkDataS2C(const Network::ChunkDataS2CPacket& packet) override { handleChunkData(packet); }
        void onUnloadChunkS2C(const Network::UnloadChunkS2CPacket& packet) override { handleChunkUnload(packet); }
        void onBlockChangeS2C(const Network::BlockChangeS2CPacket& packet) override { handleBlockChange(packet); }
        void onClientboundSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) override { handleSectionBlocksUpdate(packet); }
        void onMultiBlockChangeS2C(const Network::MultiBlockChangeS2CPacket& packet) override { handleMultiBlockChange(packet); }
        void onPlayerUpdateS2C(const Network::PlayerUpdateS2CPacket& packet) override { handlePlayerUpdate(packet); }
        void onRemoveEntitiesS2C(const Network::RemoveEntitiesS2CPacket& packet) override { handleRemoveEntities(packet); }
        void onDisconnect(const std::string& reason) override { handleDisconnect(reason); }
        void onKeepAlive(uint64_t id) override { handleKeepAlive(id); }
        void onChunkBatchStart() override { handleChunkBatchStart(); }
        void onChunkBatchFinished(int batchSize) override { handleChunkBatchFinished(batchSize); }
        void onHotbarSyncS2C(const Network::HotbarSyncS2CPacket& packet) override { handleHotbarSync(packet); }
        void onSetChunkCacheRadiusS2C(int viewDistance) override { handleSetChunkCacheRadius(viewDistance); }

        // ========================================================================
        // PACKET HANDLERS (called via IPacket::apply on main thread)
        // ========================================================================
        
        // Chunk management
        void handleChunkData(const Network::ChunkDataS2CPacket& packet);
        void handleChunkUnload(const Network::UnloadChunkS2CPacket& packet);
        
        // Block updates
        void handleBlockChange(const Network::BlockChangeS2CPacket& packet);
        void handleSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);
        void handleMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet);
        
        // Player updates
        void handlePlayerUpdate(const Network::PlayerUpdateS2CPacket& packet);

        // Entity removal
        void handleRemoveEntities(const Network::RemoveEntitiesS2CPacket& packet);

        // World state
        void handleTimeUpdate(uint64_t worldAge, uint64_t timeOfDay);
        void handleWeatherChange(uint8_t weatherType, float intensity);
        
        // Connection management
        void handleLoginSuccess(uint32_t playerId, const std::string& playerName);
        void handleDisconnect(const std::string& reason);
        void handleKeepAlive(uint64_t id);
        
        // Player abilities
        void handlePlayerAbilities(uint8_t flags, float flySpeed, float walkSpeed);
        void handleWorldSpawn(int32_t x, int32_t y, int32_t z);
        
        // Chat
        void handleChatMessage(const std::string& message, uint8_t position);

        // Chunk batch (adaptive rate control)
        void handleChunkBatchStart();
        void handleChunkBatchFinished(int batchSize);

        // Inventory sync
        void handleHotbarSync(const Network::HotbarSyncS2CPacket& packet);

        // View distance
        void handleSetChunkCacheRadius(int viewDistance);

        // ========================================================================
        // STATISTICS
        // ========================================================================
        
        struct HandlerStats {
            uint64_t chunksReceived = 0;
            uint64_t chunksUnloaded = 0;
            uint64_t blockChanges = 0;
            uint64_t playerUpdates = 0;
            uint64_t packetsProcessed = 0;
            
            void reset() {
                chunksReceived = chunksUnloaded = blockChanges = 0;
                playerUpdates = packetsProcessed = 0;
            }
        };
        
        const HandlerStats& getStats() const { return m_stats; }
        void resetStats() { m_stats.reset(); }

        // Chunk batch rate getters for pipeline debug panel
        float GetDesiredChunksPerTick() const { return m_batchCalculator.getDesiredChunksPerTick(); }
        float GetAvgNanosPerChunk() const { return static_cast<float>(m_batchCalculator.aggregatedNanosPerChunk); }

    private:
        HandlerStats m_stats;

        // Cached references (set during initialization)
        ClientChunkManager* m_chunkManager = nullptr;
        Game::ClientPlayer* m_player = nullptr;
        // Note: NetworkClient is accessed via g_networkClient global

        // Chunk batch rate calculator (Minecraft's ChunkBatchSizeCalculator)
        struct ChunkBatchSizeCalculator {
            double aggregatedNanosPerChunk = 2000000.0; // 2ms initial estimate
            int oldSamplesWeight = 1;
            std::chrono::steady_clock::time_point batchStartTime;

            void onBatchStart() {
                batchStartTime = std::chrono::steady_clock::now();
            }

            void onBatchFinished(int batchSize) {
                if (batchSize <= 0) return;
                auto elapsed = std::chrono::steady_clock::now() - batchStartTime;
                double batchNanos = std::chrono::duration<double, std::nano>(elapsed).count();
                double nanosPerChunk = batchNanos / batchSize;

                // Clamp to 3x range of current average (reject outliers)
                double lo = aggregatedNanosPerChunk / 3.0;
                double hi = aggregatedNanosPerChunk * 3.0;
                double clamped = std::clamp(nanosPerChunk, lo, hi);

                // Weighted moving average (up to 49 old samples)
                aggregatedNanosPerChunk =
                    (aggregatedNanosPerChunk * oldSamplesWeight + clamped) / (oldSamplesWeight + 1);
                oldSamplesWeight = std::min(49, oldSamplesWeight + 1);
            }

            float getDesiredChunksPerTick() const {
                return static_cast<float>(7000000.0 / aggregatedNanosPerChunk); // 7ms budget
            }
        };
        ChunkBatchSizeCalculator m_batchCalculator;
    };

} // namespace Client