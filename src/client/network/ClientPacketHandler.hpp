// File: src/client/network/ClientPacketHandler.hpp
#pragma once

#include "common/network/PacketTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include <memory>

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
        
        // IPacketListener interface
        const char* getName() const override { return "ClientPacketHandler"; }
        
        // Visitor pattern packet handlers (called from packet apply())
        void onChunkDataS2C(const Network::ChunkDataS2CPacket& packet) override { handleChunkData(packet); }
        void onUnloadChunkS2C(const Network::UnloadChunkS2CPacket& packet) override { handleChunkUnload(packet); }
        void onServerChunkData(const Network::ServerChunkDataPacket& packet) override { handleServerChunkData(packet); }
        void onServerChunkUnload(const Network::ServerChunkUnloadPacket& packet) override { handleServerChunkUnload(packet); }
        void onBlockChangeS2C(const Network::BlockChangeS2CPacket& packet) override { handleBlockChange(packet); }
        void onMultiBlockChangeS2C(const Network::MultiBlockChangeS2CPacket& packet) override { handleMultiBlockChange(packet); }
        void onPlayerUpdateS2C(const Network::PlayerUpdateS2CPacket& packet) override { handlePlayerUpdate(packet); }
        void onDisconnect(const std::string& reason) override { handleDisconnect(reason); }
        void onKeepAlive(uint64_t id) override { handleKeepAlive(id); }

        // ========================================================================
        // PACKET HANDLERS (called via IPacket::apply on main thread)
        // ========================================================================
        
        // Chunk management
        void handleChunkData(const Network::ChunkDataS2CPacket& packet);
        void handleChunkUnload(const Network::UnloadChunkS2CPacket& packet);
        void handleServerChunkData(const Network::ServerChunkDataPacket& packet);
        void handleServerChunkUnload(const Network::ServerChunkUnloadPacket& packet);
        
        // Block updates
        void handleBlockChange(const Network::BlockChangeS2CPacket& packet);
        void handleMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet);
        
        // Player updates
        void handlePlayerUpdate(const Network::PlayerUpdateS2CPacket& packet);
        
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

    private:
        HandlerStats m_stats;
        
        // Cached references (set during initialization)
        ClientChunkManager* m_chunkManager = nullptr;
        // Note: NetworkClient is accessed via g_networkClient global
    };

} // namespace Client