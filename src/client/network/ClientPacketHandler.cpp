// File: src/client/network/ClientPacketHandler.cpp
#include "ClientPacketHandler.hpp"
#include "../world/ClientChunkManager.hpp"
#include "../entity/Player.hpp"
#include "../entity/RemotePlayerManager.hpp"
#include "NetworkClient.hpp"
#include "ClientConnection.hpp"
#include "common/core/Log.hpp"

// Forward declaration: defined in src/client/renderer/gui/InventoryScreen.cpp.
// Lets the inventory carried-item update flow without pulling the GUI header here.
namespace Render {
    void SetInventoryScreenCarriedItem(Game::ItemID id, int count);
}

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
        
        Log::Debug("[ClientPacketHandler] Received chunk (%d, %d) with bitmask 0x%X",
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

    void ClientPacketHandler::handleSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for section block update");
            return;
        }
        
        // Process each packed record
        for (uint32_t packedRecord : packet.packedRecords) {
            uint8_t localX, localY, localZ;
            uint16_t blockId;
            Network::ClientboundSectionBlocksUpdateS2CPacket::UnpackRecord(
                packedRecord, localX, localY, localZ, blockId);
            
            // Convert section-local to world coordinates
            int worldX = packet.chunkPos.x * 16 + localX;
            int worldY = packet.sectionY * 16 + localY - 64;  // Adjust for world height
            int worldZ = packet.chunkPos.z * 16 + localZ;
            
            // Process as regular block change
            Network::BlockChangeS2CPacket singleChange;
            singleChange.worldX = worldX;
            singleChange.worldY = worldY;
            singleChange.worldZ = worldZ;
            singleChange.newBlockId = static_cast<Game::BlockID>(blockId);
            singleChange.playSound = false; // Don't play sound for bulk changes
            singleChange.updateNeighbors = false;
            
            m_chunkManager->ProcessBlockChange(singleChange);
        }
        
        m_stats.blockChanges += packet.packedRecords.size();
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Section block update for chunk (%d, %d) section %d: %zu changes",
                  packet.chunkPos.x, packet.chunkPos.z, packet.sectionY, packet.packedRecords.size());
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
        if (g_remotePlayerManager) {
            g_remotePlayerManager->UpdatePlayer(packet.playerId, packet.position, packet.rotation, packet.isCrouching);
        }
        m_stats.playerUpdates++;
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // ENTITY REMOVAL
    // ========================================================================

    void ClientPacketHandler::handleRemoveEntities(const Network::RemoveEntitiesS2CPacket& packet) {
        if (g_remotePlayerManager) {
            for (int32_t entityId : packet.entityIds) {
                g_remotePlayerManager->RemovePlayer(static_cast<uint32_t>(entityId));
                Log::Info("[ClientPacketHandler] Removed entity %d", entityId);
            }
        }
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

    // ========================================================================
    // CHUNK BATCH (Adaptive Rate Control)
    // ========================================================================

    void ClientPacketHandler::handleChunkBatchStart() {
        m_batchCalculator.onBatchStart();
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleChunkBatchFinished(int batchSize) {
        m_batchCalculator.onBatchFinished(batchSize);
        float desiredRate = m_batchCalculator.getDesiredChunksPerTick();
        desiredRate = std::clamp(desiredRate, 0.01f, 64.0f);

        // Send ack back to server
        if (g_networkClient && g_networkClient->IsConnected()) {
            auto connection = g_networkClient->GetConnection();
            if (connection) {
                Network::ChunkBatchAckC2SPacket ackPacket(desiredRate);
                auto data = Network::Serialization::Serialize(ackPacket);
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchAckC2S), data);
                Log::Debug("[ClientPacketHandler] Sent batch ack: rate=%.2f (batch=%d)", desiredRate, batchSize);
            }
        }

        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // INVENTORY SYNC
    // ========================================================================

    void ClientPacketHandler::handleHotbarSync(const Network::HotbarSyncS2CPacket& packet) {
        Log::Info("[ClientPacketHandler] Received hotbar sync from server");

        if (!m_player) {
            Log::Warning("[ClientPacketHandler] Player not available for hotbar sync");
            return;
        }

        for (int i = 0; i < 9; i++) {
            auto blockId = static_cast<Game::BlockID>(packet.slots[i]);
            // Air slots get count=0; non-Air gets a default stack so the hotbar shows them.
            // (HotbarSync is the legacy path; InventoryFullS2C carries real counts.)
            int count = (blockId == Game::BlockID::Air) ? 0 : 64;
            m_player->inventory.SetSlot(Game::Inventory::HotbarToIndex(i), blockId, count);
            Log::Debug("[ClientPacketHandler] Hotbar slot %d = block %d", i, packet.slots[i]);
        }

        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleInventoryFull(const Network::InventoryFullS2CPacket& packet) {
        if (!m_player) return;
        for (int i = 0; i < Game::Inventory::TOTAL_SIZE; ++i) {
            m_player->inventory.SetSlot(i,
                                        static_cast<Game::ItemID>(packet.itemIds[i]),
                                        static_cast<int>(packet.counts[i]));
        }
        m_player->inventory.SetSelectedSlot(packet.selectedHotbarSlot);
        Log::Debug("[ClientPacketHandler] Inventory full sync: selected=%d carried=%u(%u)",
                   packet.selectedHotbarSlot, packet.carriedItemId, packet.carriedCount);
        // Push the carried portion through the same path so it lands on the screen.
        Network::InventorySetCarriedS2CPacket carried{packet.carriedItemId, packet.carriedCount};
        handleInventorySetCarried(carried);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleInventorySetSlot(const Network::InventorySetSlotS2CPacket& packet) {
        if (!m_player) return;
        m_player->inventory.SetSlot(packet.slotIndex,
                                    static_cast<Game::ItemID>(packet.itemId),
                                    static_cast<int>(packet.count));
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleInventorySetCarried(const Network::InventorySetCarriedS2CPacket& packet) {
        // Defined in InventoryScreen.cpp; forward-declared at file scope at the top of this file
        // (avoids pulling in the GUI header from the network handler).
        ::Render::SetInventoryScreenCarriedItem(static_cast<Game::ItemID>(packet.itemId),
                                                static_cast<int>(packet.count));
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetChunkCacheRadius(int viewDistance) {
        Log::Info("[ClientPacketHandler] Server set chunk cache radius: %d", viewDistance);

        if (g_networkClient) {
            g_networkClient->SetServerViewDistance(viewDistance);
        }

        m_stats.packetsProcessed++;
    }

} // namespace Client