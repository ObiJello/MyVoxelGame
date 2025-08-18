// File: src/client/network/ClientPacketHandler.cpp
#include "ClientPacketHandler.hpp"
#include "../world/ClientChunkManager.hpp"
#include "NetworkClient.hpp"
#include "ClientConnection.hpp"
#include "common/core/Log.hpp"

namespace Client {

    // Global client systems (defined elsewhere)
    extern std::unique_ptr<ClientChunkManager> g_clientChunkManager;
    extern NetworkClient* g_networkClient;

    ClientPacketHandler::ClientPacketHandler() {
        // Cache pointers to global systems
        m_chunkManager = g_clientChunkManager.get();
        // Note: g_networkClient is accessed directly when needed, not cached
    }

    ClientPacketHandler::~ClientPacketHandler() = default;

    // ========================================================================
    // CHUNK MANAGEMENT
    // ========================================================================

    void ClientPacketHandler::handleChunkData(const Network::ChunkDataS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for chunk data");
            return;
        }
        
        // Process chunk data on main thread
        m_chunkManager->ProcessChunkDataS2CPacket(packet);
        m_stats.chunksReceived++;
        m_stats.packetsProcessed++;
        
        // Debug logging to verify chunks are being received
        Log::Info("[ClientPacketHandler] Received chunk (%d, %d) with bitmask 0x%X", 
                  packet.chunkX, packet.chunkZ, packet.primaryBitmask);
    }

    void ClientPacketHandler::handleChunkUnload(const Network::UnloadChunkS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for chunk unload");
            return;
        }
        
        // Unload chunk on main thread
        Game::Math::ChunkPos pos{packet.chunkX, packet.chunkZ};
        m_chunkManager->UnloadChunk(pos);
        m_stats.chunksUnloaded++;
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Unloading chunk (%d, %d)", packet.chunkX, packet.chunkZ);
    }


    // ========================================================================
    // BLOCK UPDATES
    // ========================================================================

    void ClientPacketHandler::handleBlockChange(const Network::BlockChangeS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for block change");
            return;
        }
        
        // Apply block change on main thread
        m_chunkManager->ProcessBlockChange(packet);
        m_stats.blockChanges++;
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Block change at (%d, %d, %d) to %d", 
                  packet.worldX, packet.worldY, packet.worldZ, static_cast<int>(packet.newBlockId));
    }

    void ClientPacketHandler::handleMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for multi block change");
            return;
        }
        
        // Apply multiple block changes
        for (const auto& change : packet.changes) {
            Network::BlockChangeS2CPacket singleChange;
            singleChange.worldX = packet.chunkPos.x * 16 + change.localX;
            singleChange.worldY = change.localY;
            singleChange.worldZ = packet.chunkPos.z * 16 + change.localZ;
            singleChange.newBlockId = change.blockId;
            singleChange.playSound = false; // Don't play sound for bulk changes
            singleChange.updateNeighbors = false;
            
            m_chunkManager->ProcessBlockChange(singleChange);
        }
        
        m_stats.blockChanges += packet.changes.size();
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Multi block change in chunk (%d, %d): %zu changes",
                  packet.chunkPos.x, packet.chunkPos.z, packet.changes.size());
    }

    // ========================================================================
    // PLAYER UPDATES
    // ========================================================================

    void ClientPacketHandler::handlePlayerUpdate(const Network::PlayerUpdateS2CPacket& packet) {
        // TODO: Handle other player positions for multiplayer
        m_stats.playerUpdates++;
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // WORLD STATE
    // ========================================================================

    void ClientPacketHandler::handleTimeUpdate(uint64_t worldAge, uint64_t timeOfDay) {
        // TODO: Update world time for lighting
        m_stats.packetsProcessed++;
        Log::Debug("[ClientPacketHandler] Time update: age=%llu, time=%llu", worldAge, timeOfDay);
    }

    void ClientPacketHandler::handleWeatherChange(uint8_t weatherType, float intensity) {
        // TODO: Handle weather changes
        m_stats.packetsProcessed++;
        Log::Debug("[ClientPacketHandler] Weather change: type=%d, intensity=%.2f", weatherType, intensity);
    }

    // ========================================================================
    // CONNECTION MANAGEMENT
    // ========================================================================

    void ClientPacketHandler::handleLoginSuccess(uint32_t playerId, const std::string& playerName) {
        m_stats.packetsProcessed++;
        Log::Info("[ClientPacketHandler] Login success: player=%s, id=%u", playerName.c_str(), playerId);
    }

    void ClientPacketHandler::handleDisconnect(const std::string& reason) {
        m_stats.packetsProcessed++;
        Log::Info("[ClientPacketHandler] Disconnected: %s", reason.c_str());
        
        // Clean up chunk manager
        if (m_chunkManager) {
            m_chunkManager->ClearAllChunks();
        }
    }

    void ClientPacketHandler::handleKeepAlive(uint64_t id) {
        Log::Debug("[ClientPacketHandler] Received keep-alive with ID: %llu", id);
        
        // Send keep-alive response immediately using global NetworkClient
        if (!g_networkClient) {
            Log::Warning("[ClientPacketHandler] g_networkClient is null, cannot send keep-alive response");
        } else if (!g_networkClient->IsConnected()) {
            Log::Warning("[ClientPacketHandler] Client not connected, cannot send keep-alive response");
        } else {
            auto connection = g_networkClient->GetConnection();
            if (!connection) {
                Log::Warning("[ClientPacketHandler] Connection is null, cannot send keep-alive response");
            } else {
                Log::Debug("[ClientPacketHandler] Sending keep-alive response with ID: %llu", id);
                connection->SendKeepAliveResponse(id);
                Log::Info("[ClientPacketHandler] Successfully sent keep-alive response with ID: %llu", id);
            }
        }
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // PLAYER ABILITIES
    // ========================================================================

    void ClientPacketHandler::handlePlayerAbilities(uint8_t flags, float flySpeed, float walkSpeed) {
        // TODO: Update player controller with abilities
        m_stats.packetsProcessed++;
        Log::Debug("[ClientPacketHandler] Player abilities: flags=0x%02X, fly=%.2f, walk=%.2f", 
                  flags, flySpeed, walkSpeed);
    }

    void ClientPacketHandler::handleWorldSpawn(int32_t x, int32_t y, int32_t z) {
        // TODO: Set world spawn point
        m_stats.packetsProcessed++;
        Log::Info("[ClientPacketHandler] World spawn set to (%d, %d, %d)", x, y, z);
    }

    // ========================================================================
    // CHAT
    // ========================================================================

    void ClientPacketHandler::handleChatMessage(const std::string& message, uint8_t position) {
        // TODO: Display chat message in UI
        m_stats.packetsProcessed++;
        Log::Info("[CHAT] %s", message.c_str());
    }

} // namespace Client