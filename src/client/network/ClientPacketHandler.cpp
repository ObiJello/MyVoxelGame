// File: src/client/network/ClientPacketHandler.cpp
#include "ClientPacketHandler.hpp"
#include "../world/ClientChunkManager.hpp"
#include "../entity/Player.hpp"
#include "../entity/RemotePlayerManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/ClientMobManager.hpp"
#include "common/entity/ItemEntity.hpp"   // Game::IsItemEntityId
#include "common/entity/Entity.hpp"       // Game::IsMobEntityId
#include "common/core/Mth.hpp"            // rotation unpacking
#include "NetworkClient.hpp"
#include "ClientConnection.hpp"
#include "common/core/Log.hpp"
#include "../renderer/gui/ChatScreen.hpp"   // SetServerCommandNames
#include <cmath>
#if ENABLE_PORTAL_GUN
#include "../portal/ClientPortalManager.hpp"
#endif

// Forward declaration: defined in
// src/client/renderer/gui/AbstractContainerScreen.cpp. Lets the inventory
// carried-item update flow without pulling the GUI header here. All three write
// the shared player inventory menu, so they land whichever screen is open.
namespace Render {
    void SetInventoryScreenCarriedItem(const Game::ItemStack& stack);
    void SetInventoryScreenStateId(uint32_t id);
    void SetInventoryScreenContainerId(uint32_t id);
    // Container contents + menu swapping. Slot indices are MENU indices; see
    // AbstractContainerScreen.hpp for why they can't be written straight into
    // the inventory.
    void ApplyContainerSlot(int menuIndex, const Game::ItemStack& stack);
    void ApplyContainerData(uint32_t containerId, uint16_t index, int32_t value);
    void ApplyContainerFullSync(Game::MenuType menuType, uint32_t containerId,
                                const std::vector<Game::ItemStack>& slots);
    void OpenClientContainerScreen(Game::MenuType type, uint32_t containerId,
                                   const std::string& title);
    // Defined in screens/DeathScreen.cpp — death flow hooks (health<=0 opens,
    // health>0 closes). Same no-GUI-header convention as above.
    void ShowDeathScreen();
    void DismissDeathScreen();
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

    void ClientPacketHandler::handleBlockChangedAck(const Network::BlockChangedAckS2CPacket& packet) {
        if (!m_chunkManager) return;
        // Every interaction up to this sequence is now settled — retire those
        // predictions, snapping back wherever the server disagreed with us.
        m_chunkManager->HandleBlockChangedAck(packet.sequence);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        if (!m_chunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for section block update");
            return;
        }
        
        // Process each packed record
        for (uint64_t packedRecord : packet.packedRecords) {
            uint8_t localX, localY, localZ;
            Game::BlockStateIndex stateIndex;
            uint16_t blockId;
            Network::ClientboundSectionBlocksUpdateS2CPacket::UnpackRecord(
                packedRecord, localX, localY, localZ, blockId, stateIndex);
            
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
            singleChange.newBlockState = stateIndex;
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
            singleChange.newBlockState = change.blockState;
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
            g_remotePlayerManager->SetHurtTime(packet.playerId, packet.hurtTime);
            g_remotePlayerManager->SetDeathTime(packet.playerId, packet.deathTime);
        }
        m_stats.playerUpdates++;
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // ENTITY REMOVAL
    // ========================================================================

    void ClientPacketHandler::handleRemoveEntities(const Network::RemoveEntitiesS2CPacket& packet) {
        // This packet carries both player and dropped-item ids. The id RANGE is
        // what tells them apart — item entities are allocated from
        // Game::kItemEntityIdBase upward, players use their (small) connection
        // ids. Routing every id at the player map, as this used to, would make
        // a despawning item silently evict a player.
        for (int32_t entityId : packet.entityIds) {
            if (Game::IsMobEntityId(entityId)) {
                if (g_clientMobManager) g_clientMobManager->Remove(entityId);
            } else if (Game::IsItemEntityId(entityId)) {
                if (g_itemEntityManager) g_itemEntityManager->Remove(entityId);
            } else if (g_remotePlayerManager) {
                g_remotePlayerManager->RemovePlayer(static_cast<uint32_t>(entityId));
                Log::Info("[ClientPacketHandler] Removed entity %d", entityId);
            }
        }
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // DROPPED ITEM ENTITIES
    // ========================================================================

    void ClientPacketHandler::handleItemEntitySpawn(const Network::ItemEntitySpawnS2CPacket& packet) {
        if (g_itemEntityManager) {
            g_itemEntityManager->Spawn(packet.entityId, packet.position,
                                       packet.velocity, packet.bobOffs,
                                       packet.stack);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleTakeItemEntity(const Network::TakeItemEntityS2CPacket& packet) {
        // MC also plays SoundEvents.ITEM_PICKUP here. This engine has no sound
        // system yet (Game::PlaySound is a logging stub), so the animation goes
        // out silent — that is the one piece of MC's pickup feedback missing.
        if (g_itemEntityManager) {
            g_itemEntityManager->TakeItem(packet.itemEntityId, packet.playerId,
                                          packet.amount);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleItemEntityMove(const Network::ItemEntityMoveS2CPacket& packet) {
        if (g_itemEntityManager) {
            for (const auto& e : packet.entries) {
                g_itemEntityManager->Move(e.entityId, e.position, e.velocity, e.count);
            }
        }
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // MOB ENTITIES
    // ========================================================================

    void ClientPacketHandler::handleAddEntity(const Network::AddEntityS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->Spawn(packet.entityId, packet.entityType, packet.position,
                                      packet.velocity,
                                      Game::Mth::UnpackDegrees(packet.yRot),
                                      Game::Mth::UnpackDegrees(packet.xRot),
                                      Game::Mth::UnpackDegrees(packet.yHeadRot),
                                      packet.health, packet.flags, packet.variantData,
                                      packet.pose, packet.animState);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleMoveEntity(const Network::MoveEntityS2CPacket& packet) {
        if (g_clientMobManager) {
            for (const auto& e : packet.entries) {
                // Deltas are in 1/4096 of a block — the same scale the sender
                // encoded with. Decoding at any other scale produces mobs that
                // drift a fixed fraction behind where the server has them.
                const glm::dvec3 delta(
                    Network::DecodeEntityPos(e.dx),
                    Network::DecodeEntityPos(e.dy),
                    Network::DecodeEntityPos(e.dz));

                g_clientMobManager->MoveDelta(
                    e.entityId, (e.mask & 0x01) != 0, delta, (e.mask & 0x02) != 0,
                    Game::Mth::UnpackDegrees(e.yRot),
                    Game::Mth::UnpackDegrees(e.xRot),
                    Game::Mth::UnpackDegrees(e.yHeadRot),
                    e.onGround);
            }
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleEntityPositionSync(
            const Network::EntityPositionSyncS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->Teleport(packet.entityId, packet.position, packet.velocity,
                                         Game::Mth::UnpackDegrees(packet.yRot),
                                         Game::Mth::UnpackDegrees(packet.xRot),
                                         Game::Mth::UnpackDegrees(packet.yHeadRot),
                                         packet.onGround);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetEntityMotion(
            const Network::SetEntityMotionS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->SetMotion(packet.entityId, packet.velocity);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetEntityData(const Network::SetEntityDataS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->SetData(packet.entityId, packet.health, packet.flags,
                                        packet.variantData, packet.hurtTime, packet.deathTime,
                                        packet.swellDir, packet.swell,
                                        packet.pose, packet.animState);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleHurtAnimation(const Network::HurtAnimationS2CPacket& packet) {
        // MC ClientPacketListener.handleHurtAnimation -> entity.animateHurt(yaw),
        // which for a Player also stores the direction (Player.animateHurt).
        // The server only sends this to the entity's own client, so there is
        // nothing to look up: it is always us.
        if (m_player) {
            m_player->hurtDuration = 10;
            m_player->hurtTime     = m_player->hurtDuration;
            m_player->hurtDir      = packet.yaw;
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleEntityEvent(const Network::EntityEventS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->HandleEvent(packet.entityId, packet.event);
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
#if ENABLE_PORTAL_GUN
        // Drop any portals carried over from this server. The next server's
        // SyncToClient will repopulate from authoritative state.
        GetClientPortalManager().Clear();
#endif
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

    void ClientPacketHandler::handlePlayerAbilities(const Network::PlayerAbilitiesS2CPacket& packet) {
        // Mirrors ClientPacketListener.handlePlayerAbilities — copy the
        // authoritative abilities onto the local player. The extra gameMode
        // byte replaces MC's separate CHANGE_GAME_MODE game event.
        m_stats.packetsProcessed++;
        if (!m_player) return;

        m_player->gameMode      = packet.gameMode;
        m_player->gameModeKnown = true;
        m_player->invulnerable = packet.invulnerable();
        m_player->instabuild   = packet.instabuild();
        m_player->flyingSpeed  = packet.flyingSpeed;
        m_player->physics.mayFly = packet.mayFly();

        // Flight permission revoked (creative → survival): force-land.
        // While permitted, the local double-tap toggle stays authoritative
        // for responsiveness (MC's client also owns abilities.flying).
        if (!packet.mayFly()) {
            m_player->physics.isFlying = false;
        }

        Log::Info("[ClientPacketHandler] Abilities: gameMode=%u mayFly=%d instabuild=%d invulnerable=%d",
                  packet.gameMode, packet.mayFly() ? 1 : 0,
                  packet.instabuild() ? 1 : 0, packet.invulnerable() ? 1 : 0);
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
        // Slots are MENU indices, so they go through the menu rather than
        // straight into the inventory: while a crafting table is open, menu
        // slot 10 is inventory slot 9 and menu slots 0..9 are the table's own
        // grid. ApplyContainerFullSync also brings the client's menu into
        // agreement with `menuType` first, so those indices mean the right
        // thing before anything is written.
        ::Render::ApplyContainerFullSync(packet.menuType, packet.containerId, packet.slots);
        // Not a menu concept — the hotbar selection outlives whatever is open.
        m_player->inventory.SetSelectedSlot(packet.selectedHotbarSlot);
        // Remember the revision this snapshot describes so subsequent clicks
        // can be stamped with it (MC ServerboundContainerClickPacket.stateId).
        ::Render::SetInventoryScreenStateId(packet.stateId);
        // Only the full snapshot carries containerId (it changes whenever a
        // menu opens or closes, and both full-sync), so this is the one place
        // it is learned.
        ::Render::SetInventoryScreenContainerId(packet.containerId);
        Log::Debug("[ClientPacketHandler] Container full sync: menu=%u slots=%zu selected=%d carried=%u(%d)",
                   static_cast<unsigned>(packet.menuType), packet.slots.size(),
                   packet.selectedHotbarSlot, packet.carried.itemId, packet.carried.count);
        // Push the carried portion through the same path so it lands on the screen.
        Network::InventorySetCarriedS2CPacket carried{packet.carried};
        handleInventorySetCarried(carried);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleInventorySetSlot(const Network::InventorySetSlotS2CPacket& packet) {
        if (!m_player) return;
        // MENU index — routed through the menu, same as the full sync.
        ::Render::ApplyContainerSlot(packet.slotIndex, packet.stack);
        ::Render::SetInventoryScreenStateId(packet.stateId);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleOpenScreen(const Network::OpenScreenS2CPacket& packet) {
        // Mirrors ClientPacketListener.handleOpenScreen: build the menu the
        // server named and show its screen. The contents follow immediately in
        // a full snapshot.
        ::Render::OpenClientContainerScreen(packet.menuType, packet.containerId, packet.title);
        Log::Debug("[ClientPacketHandler] Open screen: menu=%u container=%u '%s'",
                   static_cast<unsigned>(packet.menuType), packet.containerId,
                   packet.title.c_str());
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleContainerSetData(
            const Network::ContainerSetDataS2CPacket& packet) {
        // Mirrors ClientPacketListener.handleContainerSetData: write the value
        // into the open menu's data array. The furnace screen reads it back the
        // next frame to size its flame and arrow — nothing else to do.
        ::Render::ApplyContainerData(packet.containerId, packet.id, packet.value);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleInventorySetCarried(const Network::InventorySetCarriedS2CPacket& packet) {
        // Defined in AbstractContainerScreen.cpp; forward-declared at file scope at the top of this file
        // (avoids pulling in the GUI header from the network handler).
        ::Render::SetInventoryScreenCarriedItem(packet.stack);
        ::Render::SetInventoryScreenStateId(packet.stateId);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetHealth(const Network::SetHealthS2CPacket& packet) {
        // Mirrors ClientPacketListener.handleSetHealth — write the
        // authoritative stat triple onto the local player; the HUD reads it
        // each frame (PlatformMain's RenderHUD hookup).
        if (!m_player) return;
        // ceil, not floor — MC's Gui renders Mth.ceil(health). Regen heals in
        // fractional steps (saturated regen heals saturation/6 per burst), so
        // health is often non-integer; flooring made 0.8 health display as
        // ZERO hearts while the server still (correctly) considered the
        // player alive — "empty hearts but no death screen".
        m_player->health     = static_cast<int>(std::ceil(packet.health));
        m_player->food       = static_cast<int>(packet.food);
        m_player->saturation = packet.saturation;

        // Health hitting 0 IS the death signal (MC LocalPlayer.hurtTo →
        // Minecraft.setScreen(new DeathScreen(...)) when health <= 0); a
        // respawn's health refresh closes it again. Runs on the main thread
        // (typed packets apply during DrainIncomingPackets in the client tick).
        if (packet.health <= 0.0f) {
            // Kill any predicted item use — the dead hand drops its food.
            m_player->usingItem        = false;
            m_player->useItemRemaining = 0;
            m_player->useItemDuration  = 0;
            m_player->useAnim          = Game::ItemUseAnimation::NONE;
            ::Render::ShowDeathScreen();
        } else {
            ::Render::DismissDeathScreen();
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleCommands(const std::vector<std::string>& commandNames) {
        Log::Info("[ClientPacketHandler] Server advertised %zu commands for tab-completion",
                  commandNames.size());
        // Fully qualified: this file lives in namespace Client, so a bare
        // `Render::` would look for Client::Render first and not find it.
        ::Render::SetServerCommandNames(commandNames);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetChunkCacheRadius(int viewDistance) {
        Log::Info("[ClientPacketHandler] Server set chunk cache radius: %d", viewDistance);

        if (g_networkClient) {
            g_networkClient->SetServerViewDistance(viewDistance);
        }

        m_stats.packetsProcessed++;
    }

#if ENABLE_PORTAL_GUN
    void ClientPacketHandler::handlePortalSet(const Network::PortalSetS2CPacket& packet) {
        GetClientPortalManager().OnPortalSet(packet);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handlePortalRemove(const Network::PortalRemoveS2CPacket& packet) {
        GetClientPortalManager().OnPortalRemove(packet);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handlePortalTeleportFlash(const Network::PortalTeleportFlashS2CPacket& packet) {
        GetClientPortalManager().OnTeleportFlash(packet);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handlePortalFizzle(const Network::PortalFizzleS2CPacket& packet) {
        GetClientPortalManager().OnPortalFizzle(packet);
        m_stats.packetsProcessed++;
    }
#endif


    // ========================================================================
    // FORMERLY LEGACY-REGISTRY PACKETS
    // ========================================================================
    //
    // Each of these used to be dispatched by raw payload id on the network I/O
    // thread. They are typed packets now, so they arrive here on the client
    // main thread — the property every MC ClientPacketListener handler gets
    // from PacketUtils.ensureRunningOnSameThread.
    //
    // The bodies stay on ClientConnection because they read connection-owned
    // state; MC splits it the same way, with the listener dispatching and
    // `this.connection` holding the state.

    void ClientPacketHandler::onChatMessageS2C(const Network::ChatMessageS2CPacket& packet) {
        if (m_connection) m_connection->HandleChatMessage(packet);
    }

    void ClientPacketHandler::onTimeUpdateS2C(const Network::TimeUpdateS2CPacket& packet) {
        if (m_connection) m_connection->HandleTimeUpdate(packet);
    }

    void ClientPacketHandler::onWorldSpawnS2C(const Network::WorldSpawnS2CPacket& packet) {
        if (m_connection) m_connection->HandleWorldSpawn(packet);
    }

    void ClientPacketHandler::onPlayerInfoS2C(const Network::PlayerInfoS2CPacket& packet) {
        if (m_connection) m_connection->HandlePlayerInfo(packet);
    }

    void ClientPacketHandler::onClientboundPlayerPosition(
            const Network::ClientboundPlayerPositionPacket& packet) {
        if (m_connection) m_connection->HandleClientboundPlayerPosition(packet);
    }

    void ClientPacketHandler::onBlockEntityDataS2C(const Network::BlockEntityDataS2CPacket& packet) {
        if (m_connection) m_connection->HandleBlockEntityData(packet);
    }

    void ClientPacketHandler::onBlockEntityRemoveS2C(const Network::BlockEntityRemoveS2CPacket& packet) {
        if (m_connection) m_connection->HandleBlockEntityRemove(packet);
    }

} // namespace Client