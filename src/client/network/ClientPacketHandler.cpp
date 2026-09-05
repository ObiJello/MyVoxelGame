// File: src/client/network/ClientPacketHandler.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif
#include "client/entity/ClientFallingBlocks.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/renderer/mesh/Mesher.hpp"
#include "ClientPacketHandler.hpp"
#include "../world/ClientChunkManager.hpp"
#include "../entity/Player.hpp"
#include "../entity/RemotePlayerManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/XpOrbManager.hpp"
#include "../entity/ClientMobManager.hpp"
#include "common/entity/ItemEntity.hpp"   // Game::IsItemEntityId
#include "common/entity/Entity.hpp"       // Game::IsMobEntityId
#include "common/core/Mth.hpp"            // rotation unpacking
#include "common/core/Config.hpp"         // Config::MinY for AO-region section dirtying
#include "NetworkClient.hpp"
#include "ClientConnection.hpp"
#include "common/core/Log.hpp"
#include "../renderer/gui/BossBarState.hpp"
#include "../renderer/gui/ChatScreen.hpp"   // SetServerCommandNames
#include "../world/LevelLoadTracker.hpp"    // dimension change re-enters the load wait
#include "../renderer/environment/SkyRenderer.hpp"  // per-dimension sky
#include <algorithm>
#include <cmath>
#include <cstdlib>
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

    // The one client boss bar (see BossBarState.hpp).
    BossBarState g_bossBarState;

    // Global client systems (defined elsewhere)
    extern NetworkClient* g_networkClient;

    ClientPacketHandler::ClientPacketHandler() {
        // Note: g_networkClient is accessed directly when needed, not cached
    }

    ClientPacketHandler::~ClientPacketHandler() = default;

    // ========================================================================
    // CHUNK MANAGEMENT
    // ========================================================================

    void ClientPacketHandler::handleChunkData(const Network::ChunkDataS2CPacket& packet) {
        if (!g_clientChunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for chunk data");
            return;
        }
        
        // Process chunk data on main thread
        const auto t0 = std::chrono::steady_clock::now();
        g_clientChunkManager->ProcessChunkDataS2CPacket(packet);
        m_batchCalculator.onChunkApplied(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
        m_stats.chunksReceived++;
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Received chunk (%d, %d) with %zu sections",
                  packet.chunkX, packet.chunkZ, packet.sections.size());
    }

    void ClientPacketHandler::handleChunkUnchanged(const Network::ChunkUnchangedS2CPacket& packet) {
        // Revive the retained copy; if we no longer have it (evicted), ask for
        // the full chunk. Counts as a chunk for the batch calculator either way.
        Game::Math::ChunkPos pos{packet.chunkX, packet.chunkZ};
        const bool restored = g_clientChunkManager && g_clientChunkManager->RestoreRetainedChunk(pos, packet.modStamp);
        if (!restored && g_networkClient && g_networkClient->IsConnected()) {
            if (auto connection = g_networkClient->GetConnection()) {
                Network::ChunkRequestFullC2SPacket req; req.chunkX = packet.chunkX; req.chunkZ = packet.chunkZ;
                req.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(ClientLevels::BoundDimension()));
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkRequestFullC2S),
                                       Network::Serialization::Serialize(req));
            }
        }
        m_stats.chunksReceived++;
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleChunkUnload(const Network::UnloadChunkS2CPacket& packet) {
        if (!g_clientChunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for chunk unload");
            return;
        }
        
        // Unload chunk on main thread
        Game::Math::ChunkPos pos{packet.chunkX, packet.chunkZ};
        g_clientChunkManager->UnloadChunk(pos);
        m_stats.chunksUnloaded++;
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Unloading chunk (%d, %d)", packet.chunkX, packet.chunkZ);
    }


    // ========================================================================
    // BLOCK UPDATES
    // ========================================================================

    void ClientPacketHandler::handleBlockChange(const Network::BlockChangeS2CPacket& packet) {
        if (!g_clientChunkManager) {
            Log::Warning("[ClientPacketHandler] ChunkManager not available for block change");
            return;
        }
        
        // Apply block change on main thread
        g_clientChunkManager->ProcessBlockChange(packet);
        m_stats.blockChanges++;
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Block change at (%d, %d, %d) to %d", 
                  packet.worldX, packet.worldY, packet.worldZ, static_cast<int>(packet.newBlockId));
    }

    void ClientPacketHandler::handleBlockChangedAck(const Network::BlockChangedAckS2CPacket& packet) {
        if (!g_clientChunkManager) return;
        // The sequence counter is per client, but a prediction lives in the
        // level it was made in — the far level when the crosshair reached
        // through a portal — while this packet lands in whichever level the
        // stream is scoped to. Retire the sequence in EVERY level (a no-op
        // where nothing is pending); an unretired far-level prediction was
        // rolled back by the next interaction, resurrecting a broken block.
        ClientLevels::ForEach([&](ClientLevel& level) {
            if (level.Chunks() && level.Chunks() != g_clientChunkManager) {
                level.Chunks()->HandleBlockChangedAck(packet.sequence);
            }
        });
        // Every interaction up to this sequence is now settled — retire those
        // predictions, snapping back wherever the server disagreed with us.
        g_clientChunkManager->HandleBlockChangedAck(packet.sequence);
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        if (!g_clientChunkManager) {
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
            
            g_clientChunkManager->ProcessBlockChange(singleChange);
        }
        
        m_stats.blockChanges += packet.packedRecords.size();
        m_stats.packetsProcessed++;
        
        Log::Debug("[ClientPacketHandler] Section block update for chunk (%d, %d) section %d: %zu changes",
                  packet.chunkPos.x, packet.chunkPos.z, packet.sectionY, packet.packedRecords.size());
    }

    void ClientPacketHandler::handleMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet) {
        if (!g_clientChunkManager) {
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
            
            g_clientChunkManager->ProcessBlockChange(singleChange);
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
            // Scale first: the update's teleport test scales with it.
            g_remotePlayerManager->SetScale(packet.playerId, packet.scale);
            Game::DimensionId dim = Game::DimensionFromRaw(packet.dimensionId);
            glm::vec3 updatePos = packet.position;
            glm::vec2 updateRot = packet.rotation;
#if ENABLE_IMMERSIVE_PORTALS
            // A crossing already pending on this copy takes the update first
            // (a far-side position comes back through the portal).
            dim = g_remotePlayerManager->ApplyPendingCrossing(packet.playerId, dim, updatePos, updateRot);
            // A level change, or a jump too long for walking, that a portal
            // of the player's old level accounts for — its far side is where
            // the new position is, and the position it maps back to is a
            // step from the last one — is a portal crossing: the copy is
            // carried through the portal and keeps walking. Anything else
            // is the teleport it looks like, and UpdatePlayer snaps.
            {
                const auto& players = g_remotePlayerManager->GetPlayers();
                auto it = players.find(packet.playerId);
                if (it != players.end() && it->second.positionInitialized) {
                    const RemotePlayer& rp = it->second;
                    const glm::vec3 jump = updatePos - rp.targetPosition;
                    const float s = std::max(rp.scale, 0.05f);
                    const bool suspicious = dim != rp.dimension ||
                        (jump.x * jump.x + jump.z * jump.z) > (3.0f * s) * (3.0f * s) ||
                        std::abs(jump.y) > 10.0f * s;
                    if (suspicious) {
                        const Game::DimensionId oldDim = rp.dimension;
                        const Game::Immersive::Portal* through = nullptr;
                        double best = 2.5 * std::max(s, 1.0f);   // a few ticks of any walk
                        if (ClientLevel* from = ClientLevels::Get(rp.dimension)) {
                            from->Portals().ForEach([&](const Game::Immersive::Portal& p) {
                                if (p.IsMirror() || !p.Has(Game::Immersive::PortalFlag::Teleportable)) return;
                                if (p.destDimension != dim) return;
                                const glm::dvec3 back = p.InverseTransformPoint(glm::dvec3(updatePos));
                                const double d = glm::length(back - glm::dvec3(rp.targetPosition));
                                if (d < best) { best = d; through = &p; }
                            });
                        }
                        if (through) {
                            // Not mapped yet: the copy keeps walking here and
                            // this update comes back through the portal; the
                            // switch happens when its body is through.
                            g_remotePlayerManager->BeginPortalCrossing(packet.playerId, *through, dim);
                            dim = g_remotePlayerManager->ApplyPendingCrossing(packet.playerId, dim, updatePos, updateRot);
                        }
                        static const bool kPortalDiag = std::getenv("OBEY_PORTAL_DIAG") != nullptr;
                        if (kPortalDiag) {
                            Log::Info("[PortalDiag] player %u update %s -> %s at (%.2f,%.2f,%.2f): %s",
                                      packet.playerId,
                                      std::string(Game::DimensionName(oldDim)).c_str(),
                                      std::string(Game::DimensionName(dim)).c_str(),
                                      packet.position.x, packet.position.y, packet.position.z,
                                      through ? "crossing pending - walking on here until through" : "no portal accounts for it - snapping");
                        }
                    }
                }
            }
#endif
            g_remotePlayerManager->UpdatePlayer(packet.playerId, updatePos, updateRot, packet.isCrouching,
                                                dim);
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
            if (Game::IsXpOrbEntityId(entityId)) {
                if (g_xpOrbManager) g_xpOrbManager->Remove(entityId);
            } else if (Game::IsMobEntityId(entityId)) {
                if (g_clientFallingBlocks && g_clientFallingBlocks->Owns(entityId)) {
                    g_clientFallingBlocks->Remove(entityId);
                } else if (g_clientMobManager) {
                    g_clientMobManager->Remove(entityId);
                }
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
            const bool isNew = g_itemEntityManager->Spawn(packet.entityId, packet.position,
                                                          packet.velocity, packet.bobOffs,
                                                          packet.stack, packet.scale);
            // A first spawn into a level the player is NOT standing in: the
            // far side of a portal. Logged so a "cannot see the item through
            // the portal" report shows whether the client ever got it.
#if ENABLE_IMMERSIVE_PORTALS
            // The same entity may still exist in another level's store: it
            // just crossed a portal server-side (its removal there is on the
            // wire too). Hand it over through the portal it came through so
            // the render state is continuous, and drop the old copy now.
            if (isNew && ClientLevels::HasSession()) {
                const Game::DimensionId here = ClientLevels::BoundDimension();
                ItemEntityManager* store = g_itemEntityManager;
                ClientLevels::ForEach([&](ClientLevel& other) {
                    if (other.Dimension() == here || !other.Items()) return;
                    auto oldIt = other.Items()->GetEntities().find(packet.entityId);
                    if (oldIt == other.Items()->GetEntities().end()) return;
                    const glm::dvec3 oldPos = oldIt->second.sim.pos;
                    const Game::Immersive::Portal* via = nullptr;
                    double best = 3.0;   // within reach of a surface, else no hand-off
                    other.Portals().ForEach([&](const Game::Immersive::Portal& p) {
                        if (p.IsMirror() || p.destDimension != here) return;
                        glm::dvec3 mn, mx;
                        p.BoundingBox(mn, mx, 0.0);
                        const double d = glm::length(glm::clamp(oldPos, mn, mx) - oldPos);
                        if (d < best) { best = d; via = &p; }
                    });
                    if (via) store->CarryOver(packet.entityId, oldIt->second, *via);
                    other.Items()->Remove(packet.entityId);
                });
            }
#endif
            if (isNew && ClientLevels::HasSession() &&
                ClientLevels::BoundDimension() != ClientLevels::ActiveDimension()) {
                Log::Info("[ImmersivePortals] Far item #%d spawned in %s at (%.1f, %.1f, %.1f)",
                          packet.entityId,
                          std::string(Game::DimensionName(ClientLevels::BoundDimension())).c_str(),
                          packet.position.x, packet.position.y, packet.position.z);
            }
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleTakeItemEntity(const Network::TakeItemEntityS2CPacket& packet) {
        // MC also plays SoundEvents.ITEM_PICKUP / EXPERIENCE_ORB_PICKUP here.
        // This engine has no sound system yet (Game::PlaySound is a logging
        // stub), so the animation goes out silent — that is the one piece of
        // MC's pickup feedback missing.
        //
        // The packet serves BOTH entity kinds (MC's does too); the id range
        // says which manager owns it.
        if (Game::IsXpOrbEntityId(packet.itemEntityId)) {
            if (g_xpOrbManager) {
                g_xpOrbManager->TakeOrb(packet.itemEntityId, packet.playerId);
            }
        } else if (g_itemEntityManager) {
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
    // EXPERIENCE ORBS
    // ========================================================================

    void ClientPacketHandler::handleXpOrbSpawn(const Network::XpOrbSpawnS2CPacket& packet) {
        if (g_xpOrbManager) {
            g_xpOrbManager->Spawn(packet.entityId, packet.position,
                                  packet.velocity, packet.value);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleXpOrbMove(const Network::XpOrbMoveS2CPacket& packet) {
        if (g_xpOrbManager) {
            for (const auto& e : packet.entries) {
                g_xpOrbManager->Move(e.entityId, e.position, e.velocity);
            }
        }
        m_stats.packetsProcessed++;
    }

    // ========================================================================
    // MOB ENTITIES
    // ========================================================================

    void ClientPacketHandler::handleAddEntity(const Network::AddEntityS2CPacket& packet) {
        // Every falling block goes to the compact store — see
        // ClientFallingBlocks for why the client needs no Mob for them.
        if (packet.entityType == static_cast<uint16_t>(Game::EntityTypeId::FallingBlock) &&
            g_clientFallingBlocks) {
            g_clientFallingBlocks->Spawn(packet.entityId, packet.position, packet.velocity,
                                         packet.blockStateRaw);
            m_stats.packetsProcessed++;
            return;
        }
        if (g_clientMobManager) {
            g_clientMobManager->Spawn(packet.entityId, packet.entityType, packet.position,
                                      packet.velocity,
                                      Game::Mth::UnpackDegrees(packet.yRot),
                                      Game::Mth::UnpackDegrees(packet.xRot),
                                      Game::Mth::UnpackDegrees(packet.yHeadRot),
                                      packet.health, packet.flags, packet.variantData,
                                      packet.pose, packet.animState,
                                      packet.blockStateRaw);
            g_clientMobManager->SetEntityScale(packet.entityId, packet.scale);
#if ENABLE_IMMERSIVE_PORTALS
            // The same mob may still be in another level's store: it just
            // crossed a portal server-side. Hand it over through that portal
            // and drop the old copy — see the item spawn handler.
            if (ClientLevels::HasSession()) {
                const Game::DimensionId here = ClientLevels::BoundDimension();
                ClientMobManager* store = g_clientMobManager;
                ClientLevels::ForEach([&](ClientLevel& other) {
                    if (other.Dimension() == here || !other.Mobs()) return;
                    auto oldIt = other.Mobs()->All().find(packet.entityId);
                    if (oldIt == other.Mobs()->All().end() || !oldIt->second.mob) return;
                    const glm::dvec3 oldPos = oldIt->second.mob->position;
                    const Game::Immersive::Portal* via = nullptr;
                    double best = 3.0;
                    other.Portals().ForEach([&](const Game::Immersive::Portal& p) {
                        if (p.IsMirror() || p.destDimension != here) return;
                        glm::dvec3 mn, mx;
                        p.BoundingBox(mn, mx, 0.0);
                        const double d = glm::length(glm::clamp(oldPos, mn, mx) - oldPos);
                        if (d < best) { best = d; via = &p; }
                    });
                    if (via) store->CarryOver(packet.entityId, oldIt->second, *via);
                    other.Mobs()->Remove(packet.entityId);
                });
            }
#endif
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

                if (g_clientFallingBlocks && g_clientFallingBlocks->Owns(e.entityId)) {
                    g_clientFallingBlocks->MoveDelta(e.entityId, (e.mask & 0x01) != 0, delta,
                                                     e.onGround);
                    continue;
                }
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
        if (g_clientFallingBlocks && g_clientFallingBlocks->Owns(packet.entityId)) {
            g_clientFallingBlocks->Teleport(packet.entityId, packet.position, packet.velocity,
                                            packet.onGround);
        } else if (g_clientMobManager) {
            g_clientMobManager->Teleport(packet.entityId, packet.position, packet.velocity,
                                         Game::Mth::UnpackDegrees(packet.yRot),
                                         Game::Mth::UnpackDegrees(packet.xRot),
                                         Game::Mth::UnpackDegrees(packet.yHeadRot),
                                         packet.onGround);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleEntityPositionSyncBatch(
            const Network::EntityPositionSyncBatchS2CPacket& packet) {
        for (const auto& e : packet.entries) {
            if (g_clientFallingBlocks && g_clientFallingBlocks->Owns(e.entityId)) {
                g_clientFallingBlocks->Teleport(e.entityId, e.position, e.velocity, e.onGround);
            } else if (g_clientMobManager) {
                g_clientMobManager->Teleport(e.entityId, e.position, e.velocity,
                                             0.0f, 0.0f, 0.0f, e.onGround);
            }
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetEntityMotion(
            const Network::SetEntityMotionS2CPacket& packet) {
        if (g_clientFallingBlocks && g_clientFallingBlocks->Owns(packet.entityId)) {
            g_clientFallingBlocks->SetMotion(packet.entityId, packet.velocity);
        } else if (g_clientMobManager) {
            g_clientMobManager->SetMotion(packet.entityId, packet.velocity);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleSetEntityData(const Network::SetEntityDataS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->SetData(packet.entityId, packet.health, packet.flags,
                                        packet.variantData, packet.hurtTime, packet.deathTime,
                                        packet.swellDir, packet.swell,
                                        packet.pose, packet.animState,
                                        packet.blockStateRaw);
            g_clientMobManager->SetEntityScale(packet.entityId, packet.scale);
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

    void ClientPacketHandler::handleBossEvent(const Network::BossEventS2CPacket& packet) {
        // MC ClientPacketListener.handleBossUpdate → the Gui's events map;
        // one bar here (see BossBarState.hpp).
        switch (packet.op) {
            case Network::BossEventS2CPacket::Op::Add:
                g_bossBarState.visible  = true;
                g_bossBarState.progress = packet.progress;
                g_bossBarState.color    = static_cast<uint8_t>(packet.color);
                g_bossBarState.notches  = packet.notches;
                g_bossBarState.name     = packet.name;
                break;
            case Network::BossEventS2CPacket::Op::Remove:
                g_bossBarState.visible = false;
                break;
            case Network::BossEventS2CPacket::Op::UpdateProgress:
                g_bossBarState.progress = packet.progress;
                break;
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleEndCrystalBeam(
            const Network::EndCrystalBeamS2CPacket& packet) {
        if (g_clientMobManager) {
            g_clientMobManager->SetEndCrystalBeam(packet.entityId, packet.hasTarget,
                                                  packet.target);
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

        // A boss bar from the departed server must not survive into the next
        // session (MC clears the Gui's events map with the level).
        g_bossBarState.visible = false;

        // Clean up every level (the one the player stood in and any seen
        // through a portal). The session teardown destroys them; this just
        // empties them for the disconnect screen.
        ClientLevels::ClearAll();
#if ENABLE_PORTAL_GUN
        // Drop any portals carried over from this server. The next server's
        // SyncToClient will repopulate from authoritative state.
        GetClientPortalManager().Clear();
#endif
    }

    // UNREACHABLE as of the I/O-thread keep-alive rework. ClientConnection no
    // longer decodes KeepAliveS2C into a typed packet; it answers on the I/O
    // thread from a raw registry handler, matching MC's
    // ClientCommonPacketListenerImpl.handleKeepAlive:145. Kept as a loud
    // tripwire rather than deleted: if someone re-adds the decode case, the
    // reply would go out twice and the second one would be an unsolicited echo,
    // which the server now treats as a disconnect.
    void ClientPacketHandler::handleKeepAlive(uint64_t id) {
        Log::Warning("[ClientPacketHandler] handleKeepAlive reached on the main thread "
                     "(id %llu) — KeepAliveS2C should be answered on the I/O thread. "
                     "Did a typed decode case come back?",
                     static_cast<unsigned long long>(id));
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
        m_player->physics.scale  = packet.scale;

        // MC ClientPacketListener.handlePlayerAbilities:1884 assigns
        // `abilities.flying = packet.isFlying()` unconditionally, and this used
        // to skip it so the local double-tap toggle would not fight a stale
        // server value. That divergence is what made flight not survive a
        // rejoin: the server restores the saved flag from playerdata, sends it
        // in the join abilities packet, and the client threw it away — so you
        // came back falling.
        //
        // Applying it is safe because the server only sends this packet at
        // join, on a game-mode change, and as the corrective reply to a client
        // claiming flight it may not have. None of those race the toggle.
        if (packet.mayFly()) {
            m_player->physics.isFlying = packet.flying();
        } else {
            // Flight permission revoked (creative → survival): force-land.
            m_player->physics.isFlying = false;
        }

        // Non-vanilla: noclip is a debug state the client owns outright, so the
        // only thing the server can do with it is hand back what was saved.
        // Guarded on a real change because SetNoclip logs and zeroes velocity,
        // and this packet also arrives on every game-mode change.
        if (m_player->physics.noclip != packet.noclip()) {
            m_player->SetNoclip(packet.noclip());
        }

        // Everything above came FROM the server, so the controller's dirty
        // check must not treat it as a local toggle and send it back.
        m_player->abilitiesSyncedFromServer = true;

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
    // DIMENSION CHANGE
    // ========================================================================

    // Rough analogue of MC ClientPacketListener.handleRespawn, which is what
    // vanilla runs on a dimension change: everything about the level is
    // discarded and rebuilt from the packets that follow.
    void ClientPacketHandler::handleExplode(const Network::ExplodeS2CPacket& packet) {
        // MC ClientPacketListener.handleExplosion: sound, centre particle,
        // debris, then the local player's push.
        // Distance-gated BEFORE the debris loop: SpawnExplosionVisualEffects
        // runs up to 512 RNG + block-lookup iterations per blast, and the
        // particle system then culls everything beyond 32 blocks anyway
        // (MobParticleSystem::kParticleCutoffSq). A mass detonation is
        // thousands of these packets a tick; paying the loop for blasts whose
        // particles are discarded on arrival was the client's dominant
        // cascade cost. Same 32-block radius as the cull, so nothing that
        // would have been drawn is skipped.
        constexpr double kVisualCutoffSq = 32.0 * 32.0;
        const glm::dvec3 toBlast = m_player
            ? packet.center - glm::dvec3(m_player->physics.position) : glm::dvec3(0.0);
        const bool nearEnough = !m_player || glm::dot(toBlast, toBlast) <= kVisualCutoffSq;
        if (Client::g_clientMobManager && nearEnough) {
            SpawnExplosionVisualEffects(Client::g_clientMobManager->Level(),
                                        packet.center.x, packet.center.y, packet.center.z,
                                        packet.radius, packet.small, packet.blockCount);
        }

        // UNIT CONVERSION, and it is easy to miss: the server computes
        // knockback in MC's blocks-per-TICK, while PlayerPhysics is
        // blocks-per-SECOND (see the note at the top of ItemEntity.hpp about
        // the two conventions). Applying the raw value would be a twentieth of
        // the intended shove.
        if (m_player && glm::length(packet.playerKnockback) > 1.0e-6f) {
            constexpr float kTicksPerSecond = 20.0f;
            m_player->physics.velocity += packet.playerKnockback * kTicksPerSecond;
        }
    }

    void ClientPacketHandler::handleChangeDimension(
            const Network::ChangeDimensionS2CPacket& packet) {
        Log::Info("[ClientPacketHandler] Changing dimension to %d (skyLight=%d, ceiling=%d)",
                  static_cast<int>(packet.dimensionId),
                  packet.HasSkyLight() ? 1 : 0, packet.HasCeiling() ? 1 : 0);

        // The player's level changes; the world objects do not get wiped —
        // each dimension is its own ClientLevel now (ClientLevel.hpp). The
        // level being left is destroyed unless the server says to keep it
        // (a seamless crossing, where it stays visible through the portal),
        // which is where the old "throw everything away" went.
        const Game::DimensionId dimension = Game::DimensionFromRaw(packet.dimensionId);
        ClientLevels::SetActive(dimension, packet.KeepPrevious());
        ClientLevels::SetPacketDimension(dimension);

        // The boss bar belongs to the dimension being left (the fight's
        // player scan re-adds it on re-entry before the first progress tick).
        g_bossBarState.visible = false;

        // Remote players are global (one list, each tagged with a
        // dimension); their positions are re-sent within a tick.

        // The sky is a property of the dimension, not of the player's
        // settings — the End its starfield, the Nether no sky at all. Applied
        // before the first chunk arrives so the horizon is never briefly the
        // wrong world's.
        ::Render::g_skyRenderer.SetDimension(packet.dimensionId);

        // Go back to waiting for a level. Without this the loading screen has
        // already been dismissed for the old dimension and the player spends
        // the arrival standing in an empty void watching chunks pop in.
        //
        // Not on a seamless crossing (keepPrevious): the far side was
        // streamed in before the player stepped through, and a loading
        // screen would be exactly the hitch immersive portals exist to
        // remove.
        if (!packet.KeepPrevious()) Client::g_levelLoadTracker.StartClientLoad();

        m_stats.packetsProcessed++;
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
        desiredRate = std::clamp(desiredRate, 0.01f, 256.0f);   // vanilla clamps at 64; our apply is ~10x cheaper than Java's, the 7 ms budget stays the governor

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

    void ClientPacketHandler::handleSetHeldSlot(const Network::SetHeldSlotS2CPacket& packet) {
        // MC ClientPacketListener.handleSetHeldSlot: the server picked the
        // slot (pick block); the hotbar follows.
        if (m_player && packet.slot < 9) m_player->inventory.SetSelectedSlot(packet.slot);
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

    void ClientPacketHandler::handleSetExperience(const Network::SetExperienceS2CPacket& packet) {
        // Mirrors ClientPacketListener.handleSetExperience — the authoritative
        // XP pair onto the local player; the HUD reads it each frame beside
        // health and food.
        if (!m_player) return;
        m_player->xpProgress = packet.progress;
        m_player->xpLevel    = static_cast<int>(packet.level);
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

#if ENABLE_IMMERSIVE_PORTALS
    namespace {
        // The cells whose faces lie on a dimension's axis-aligned portal
        // surfaces, handed to the mesher (Mesher::PortalFace), and the
        // sections those cells are in marked for a rebuild — the ones the
        // old list covered as well, so a removed portal gets its culling
        // back.
        void RefreshPortalFaces(Game::DimensionId dim) {
            std::vector<::Render::Mesher::PortalFace> before = ::Render::Mesher::PortalFacesFor(dim);
            std::vector<::Render::Mesher::PortalFace> after;
            if (ClientLevel* level = ClientLevels::Get(dim)) {
                level->Portals().ForEach([&](const Game::Immersive::Portal& p) {
                    if (!p.Has(Game::Immersive::PortalFlag::Visible)) return;
                    const glm::dvec3 n = p.Normal();
                    int axis = 0;
                    if (std::abs(n.y) > std::abs(n[axis])) axis = 1;
                    if (std::abs(n.z) > std::abs(n[axis])) axis = 2;
                    if (std::abs(n[axis]) < 0.999) return;               // not axis-aligned
                    const double planeCoord = p.origin[axis];
                    const double snapped = std::round(planeCoord);
                    if (std::abs(planeCoord - snapped) > 0.05) return;   // mid-block: touches no face
                    const int sign = n[axis] > 0.0 ? 1 : -1;
                    // The surface's extent in the plane.
                    glm::dvec3 mn, mx;
                    p.BoundingBox(mn, mx, 0.0);
                    ::Render::Mesher::PortalFace f;
                    f.dimension = dim;
                    for (int a = 0; a < 3; ++a) {
                        if (a == axis) {
                            const int cell = sign > 0 ? static_cast<int>(snapped) : static_cast<int>(snapped) - 1;
                            f.min[a] = f.max[a] = cell;
                        } else {
                            f.min[a] = static_cast<int>(std::floor(mn[a] + 1e-6));
                            f.max[a] = static_cast<int>(std::ceil(mx[a] - 1e-6)) - 1;
                            if (f.max[a] < f.min[a]) f.max[a] = f.min[a];
                        }
                    }
                    f.dir = glm::ivec3(0);
                    f.dir[axis] = -sign;   // from the cell into the surface
                    after.push_back(f);
                });
            }
            ::Render::Mesher::SetPortalFaces(dim, after);

            // Walk the LOADED chunks against each face's box, never the box
            // itself: a global surface (a wrap border, a stack seam) is up
            // to 200,000 blocks across, and walking its box was twelve
            // thousand chunks squared. The section range is clamped to the
            // column — a seam at y = 320 or y = -64 reaches one section
            // past either end, and MarkSectionDirty indexes by it unchecked
            // (that write past the array was the SIGBUS on receiving the
            // first stack seam).
            auto remesh = [&](const std::vector<::Render::Mesher::PortalFace>& faces) {
                if (faces.empty()) return;
                ClientLevels::ForEach([&](ClientLevel& level) {
                    if (level.Dimension() != dim || !level.Chunks()) return;
                    std::vector<std::pair<Game::Math::ChunkPos, ClientChunk*>> loaded;
                    level.Chunks()->SnapshotLoadedChunks(loaded);
                    for (const auto& f : faces) {
                        const glm::ivec3 lo = f.min - glm::ivec3(1), hi = f.max + glm::ivec3(1);
                        const int cx0 = lo.x >> 4, cx1 = hi.x >> 4;
                        const int cz0 = lo.z >> 4, cz1 = hi.z >> 4;
                        const int sy0 = std::max((lo.y - Config::MinY) >> 4, 0);
                        const int sy1 = std::min((hi.y - Config::MinY) >> 4, Game::Math::SECTIONS_PER_CHUNK - 1);
                        if (sy1 < sy0) continue;
                        for (const auto& [pos, chunk] : loaded) {
                            if (pos.x < cx0 || pos.x > cx1 || pos.z < cz0 || pos.z > cz1) continue;
                            for (int sy = sy0; sy <= sy1; ++sy) level.Chunks()->MarkSectionDirty(pos, sy);
                        }
                    }
                });
            };
            remesh(before);
            remesh(after);
        }
    }

    void ClientPacketHandler::handleImmersivePortalSync(
            const Network::ImmersivePortalSyncS2CPacket& packet) {
        if (GetClientImmersivePortals().OnSync(packet.portal)) {
            RefreshPortalFaces(packet.portal.dimension);
        }
        m_stats.packetsProcessed++;
    }

    void ClientPacketHandler::handleImmersivePortalRemove(
            const Network::ImmersivePortalRemoveS2CPacket& packet) {
        const Game::DimensionId dim = ClientLevels::BoundDimension();
        GetClientImmersivePortals().OnRemove(packet.portalId);
        RefreshPortalFaces(dim);
        m_stats.packetsProcessed++;
    }
#endif

    void ClientPacketHandler::handleAoRegions(const Network::AoRegionsS2CPacket& packet) {
        m_stats.packetsProcessed++;
        const Game::DimensionId dim = Game::DimensionFromRaw(packet.dimensionId);
        // The union of old and new boxes is what needs remeshing: a removed
        // box gets its shading back, an added one loses it.
        std::vector<::Render::Mesher::AoExclusion> before = ::Render::Mesher::AoExclusionsFor(dim);
        std::vector<::Render::Mesher::AoExclusion> after;
        after.reserve(packet.boxes.size());
        for (const auto& b : packet.boxes) after.push_back({dim, b.min, b.max});
        ::Render::Mesher::SetAoExclusions(dim, after);

        auto remesh = [&](const std::vector<::Render::Mesher::AoExclusion>& boxes) {
            ClientLevels::ForEach([&](ClientLevel& level) {
                if (level.Dimension() != dim || !level.Chunks()) return;
                for (const auto& box : boxes) {
                    // A block of margin: the faces at the box's edge share
                    // sections (and AO samples) with the blocks beside it.
                    const glm::ivec3 lo = box.min - glm::ivec3(1), hi = box.max + glm::ivec3(1);
                    for (int cx = lo.x >> 4; cx <= (hi.x >> 4); ++cx)
                        for (int cz = lo.z >> 4; cz <= (hi.z >> 4); ++cz)
                            for (int sy = (lo.y - Config::MinY) >> 4; sy <= ((hi.y - Config::MinY) >> 4); ++sy)
                                level.Chunks()->MarkSectionDirty(Game::Math::ChunkPos{cx, cz}, sy);
                }
            });
        };
        remesh(before);
        remesh(after);
    }

    void ClientPacketHandler::onDimensionScopeS2C(const Network::DimensionScopeS2CPacket& packet) {
        ClientLevels::SetPacketDimension(packet.Dimension());
        m_stats.packetsProcessed++;
    }

} // namespace Client