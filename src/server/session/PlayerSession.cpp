// File: src/server/session/PlayerSession.cpp
#include "PlayerSession.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../network/NetworkServer.hpp"
#include "../network/SendScheduler.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../world/watch/ChunkWatchIndex.hpp"
#include "PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "../IntegratedServer.hpp"
#include "common/inventory/AbstractContainerMenu.hpp"
#include "common/inventory/CraftingMenu.hpp"
#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN
#include "../portal/PortalRegistry.hpp"
#endif
#include "common/entity/Item.hpp"
#include "common/entity/Inventory.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace Server {

    PlayerSession::PlayerSession(uint32_t playerId, uint32_t connectionId)
        : m_playerId(playerId)
        , m_connectionId(connectionId)
    {
        m_lastTickTime = std::chrono::steady_clock::now();
        m_lastKeepAliveRx = m_lastTickTime;
        m_lastKeepAliveTx = m_lastTickTime;

        // Initialize stats to prevent immediate timeout
        m_stats.lastKeepAlive = m_lastTickTime;
    }

    PlayerSession::~PlayerSession() {
        Cleanup();
    }

    // === LIFECYCLE ===

    void PlayerSession::Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos) {
        m_config = config;
        m_simulationDistance = config.simulationDistance;
        m_viewDistance = std::min(config.viewDistance, m_simulationDistance);
        
        // Calculate initial chunk position
        m_currentChunk = Game::Math::ChunkPos(
            static_cast<int>(std::floor(spawnPos.x / 16.0f)),
            static_cast<int>(std::floor(spawnPos.z / 16.0f))
        );
        m_anchorChunk = m_currentChunk;
        m_lastKnownChunk = m_currentChunk;
        
        // Clear any existing state first
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();

        // Set state to joining
        m_state = State::JOINING;

        // Let the first Tick() → UpdateWatchSet() compute the initial watch set.
        // This ensures deltas flow through the normal path (ProcessSessionTick → ChunkWatchIndex).
        m_needsWatchUpdate = true;
        
        Log::Info("PlayerSession: Initialized session for player %u in dimension %d",
                 m_playerId, dimensionId);
    }
    
    void PlayerSession::AttachPlayer(ServerPlayer* player) {
        m_player = player;
        if (m_player) {
            // Sync chunk position from player
            m_currentChunk = m_player->getChunkPosition();
            m_anchorChunk = m_currentChunk;
            Log::Info("PlayerSession: Attached player %u '%s' to session",
                     m_player->getPlayerId(), m_player->getName().c_str());
        }
    }
    
    void PlayerSession::DetachPlayer() {
        if (m_player) {
            Log::Info("PlayerSession: Detached player %u from session", m_player->getPlayerId());
            m_player = nullptr;
        }
    }

    void PlayerSession::Tick(int64_t serverTick) {
        PROFILE_ZONE_N("SessionTick");
        auto tickStart = std::chrono::steady_clock::now();
        
        // Skip if not in playing state
        if (m_state != State::PLAYING && m_state != State::JOINING) {
            return;
        }
        
        // Reset per-tick budgets
        m_bytesOutThisTick = 0;
        m_chunksOutThisTick = 0;
        m_diffsOutThisTick = 0;
        
        // Update watch set if needed
        if (m_needsWatchUpdate) {
            UpdateWatchSet();
            m_needsWatchUpdate = false;
        }

        // Transition to playing state after initial join
        if (m_state == State::JOINING && (!m_pendingChunkLoads.empty() || !m_pendingChunksToSend.empty())) {
            m_state = State::PLAYING;
        }

        // Tick the player entity (movement physics, mining, item-use
        // countdown). Lives HERE — per-session — so REMOTE players tick too;
        // the old host-only call in IntegratedServer::ServerTick was removed
        // (it only ever ticked m_serverPlayer, so a LAN client's eat timer
        // never advanced).
        if (m_player) {
            ASSERT_SERVER_THREAD();
            Game::World* world = nullptr;
            if (auto* server = Server::g_integratedServer.get()) {
                world = server->GetWorld();
            }
            m_player->tick(world, static_cast<int>(serverTick));

            // Stats sync — mirrors ServerPlayer.tick's dirty-check on
            // lastSentHealth / lastSentFood / lastSaturationLevel: send
            // SetHealthS2C only when the triple changed (first PLAYING tick
            // always sends because the cached values start impossible).
            if (m_connection) {
                const float health     = m_player->getHealth();
                const int   food       = m_player->getFoodData().getFoodLevel();
                const float saturation = m_player->getFoodData().getSaturationLevel();
                if (health != m_lastSentHealth || food != m_lastSentFood
                    || saturation != m_lastSentSaturation) {
                    Network::SetHealthS2CPacket out;
                    out.health     = health;
                    out.food       = static_cast<uint32_t>(food);
                    out.saturation = saturation;
                    auto data = Network::Serialization::Serialize(out);
                    m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SetHealthS2C), data);
                    m_lastSentHealth     = health;
                    m_lastSentFood       = food;
                    m_lastSentSaturation = saturation;
                }
            }

            // Per-tick container diff — MC ServerPlayer.doTick's
            // containerMenu.broadcastChanges(). This SUPERSEDES the old
            // hand-rolled dirty-slot broadcast: routing every server-side
            // inventory mutation through the same diff is what keeps
            // m_remoteSlots an accurate model of the client. The old loop sent
            // slots directly and left the model stale, which then made the
            // next click re-send those slots needlessly.
            //
            // dirtySlots is still drained (other code marks it), but it no
            // longer drives sends — the diff finds every real change on its
            // own, including ones nothing bothered to mark.
            m_player->dirtySlots().clear();
            BroadcastContainerChanges();
        }
        
        // Store tick number
        m_lastServerTick = serverTick;
        
        // Update statistics
        auto tickEnd = std::chrono::steady_clock::now();
        float tickTime = std::chrono::duration<float, std::milli>(tickEnd - tickStart).count();
        
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.lastTickTime = tickTime;
            m_stats.averageTickTime = m_stats.averageTickTime * 0.95f + tickTime * 0.05f;
            m_stats.bytesOutThisTick = m_bytesOutThisTick;
        }
        
        m_lastTickTime = tickEnd;
    }

    void PlayerSession::Cleanup() {
        m_state = State::DISCONNECTING;
        
        // Clear all data structures
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        Log::Info("PlayerSession: Cleaned up session for player %u", m_playerId);
    }

    // === PLAYER STATE ===

    void PlayerSession::UpdatePosition(const glm::vec3& position, const glm::vec2& rotation) {
        // Delegate to ServerPlayer if attached
        if (m_player) {
            m_player->setPosition(glm::dvec3(position));
            m_player->setRotation(rotation.x, rotation.y);
            
            // Calculate new chunk position from player
            Game::Math::ChunkPos newChunk = m_player->getChunkPosition();
            
            // Update chunk position if changed
            if (newChunk != m_currentChunk) {
                UpdateChunkPosition(newChunk);
            }
        }
    }

    void PlayerSession::UpdateChunkPosition(Game::Math::ChunkPos newChunk) {
        if (newChunk == m_currentChunk) {
            return;
        }
        
        m_lastKnownChunk = m_currentChunk;
        m_currentChunk = newChunk;
        m_anchorChunk = newChunk;
        m_needsWatchUpdate = true;
        
        Log::Info("SESSION CHUNK MOVE: player %u from (%d,%d) to (%d,%d)",
                  m_playerId, m_lastKnownChunk.x, m_lastKnownChunk.z, newChunk.x, newChunk.z);
    }

    void PlayerSession::ChangeDimension(int newDimensionId, const glm::vec3& targetPos) {
        if (!m_player) return;
        
        if (m_player->getDimensionId() == newDimensionId) {
            return;
        }
        
        Log::Info("PlayerSession: Player %u changing dimension from %d to %d", 
                 m_playerId, m_player->getDimensionId(), newDimensionId);
        
        m_isChangingDimension = true;
        
        // Send unload for all watched chunks
        for (const auto& chunk : m_watchSet) {
            SendChunkUnload(chunk);
        }
        
        // Clear all watch sets
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        // Update player dimension and position
        m_player->setDimensionId(newDimensionId);
        m_player->teleport(glm::dvec3(targetPos));
        
        m_currentChunk = m_player->getChunkPosition();
        m_anchorChunk = m_currentChunk;
        
        // Recompute watch set for new dimension
        m_needsWatchUpdate = true;
        m_isChangingDimension = false;
    }

    void PlayerSession::Respawn(const glm::vec3& spawnPos) {
        Log::Info("PlayerSession: Player %u respawning at (%.1f, %.1f, %.1f)",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z);
        
        m_isRespawning = true;
        m_state = State::RESPAWNING;
        
        // Respawn player entity
        if (m_player) {
            m_player->respawn(spawnPos);
            m_currentChunk = m_player->getChunkPosition();
            m_anchorChunk = m_currentChunk;
        }
        
        // Recompute watch set
        m_needsWatchUpdate = true;
        
        m_state = State::PLAYING;
        m_isRespawning = false;
    }

    // === VIEW CONFIGURATION ===

    void PlayerSession::SetViewDistance(int distance) {
        if (distance == m_viewDistance) {
            return;
        }
        
        // Clamp to valid range and simulation distance
        m_viewDistance = std::clamp(distance, 2, std::min(32, m_simulationDistance));
        m_needsWatchUpdate = true;
        
        Log::Info("PlayerSession: Player %u view distance changed to %d", 
                 m_playerId, m_viewDistance);
    }

    void PlayerSession::SetSimulationDistance(int distance) {
        if (distance == m_simulationDistance) {
            return;
        }
        
        m_simulationDistance = std::clamp(distance, 2, 32);
        
        // Ensure view distance doesn't exceed simulation distance
        if (m_viewDistance > m_simulationDistance) {
            m_viewDistance = m_simulationDistance;
        }
        
        m_needsWatchUpdate = true;
        
        Log::Info("PlayerSession: Player %u simulation distance changed to %d", 
                 m_playerId, m_simulationDistance);
    }

    // === WATCH SET MANAGEMENT ===

    void PlayerSession::UpdateWatchSet() {
        PROFILE_ZONE;
        // Compute new watch set
        auto newWatch = ComputeWatchSet(m_anchorChunk, m_viewDistance);

        // Compute deltas
        std::vector<Game::Math::ChunkPos> toAdd, toRemove;
        ComputeWatchDeltas(newWatch, toAdd, toRemove);

        if (!toRemove.empty() || !toAdd.empty()) {
            Log::Info("UpdateWatchSet: anchor=(%d,%d) viewDist=%d watchSet=%zu newWatch=%zu toAdd=%zu toRemove=%zu",
                     m_anchorChunk.x, m_anchorChunk.z, m_viewDistance,
                     m_watchSet.size(), newWatch.size(), toAdd.size(), toRemove.size());
        }

        // Apply removals first
        for (const auto& chunk : toRemove) {
            DropChunk(chunk);
            m_watchSet.erase(chunk);
        }

        // Queue additions — add to pending loads (IntegratedServer will move to ready-to-send when loaded)
        for (const auto& chunk : toAdd) {
            m_pendingChunkLoads.insert(chunk);
            m_watchSet.insert(chunk);
        }

        // Store deltas for ChunkWatchIndex synchronization (consumed by PlayerSessionManager)
        m_pendingWatchAdds = toAdd;
        m_pendingWatchRemoves = toRemove;

        // Update stats
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksInWatch = m_watchSet.size();
            m_stats.chunksPending = m_pendingChunkLoads.size() + m_pendingChunksToSend.size();
        }
    }

    bool PlayerSession::IsWatching(Game::Math::ChunkPos chunk) const {
        return m_watchSet.count(chunk) > 0;
    }

    bool PlayerSession::HasSentChunk(Game::Math::ChunkPos chunk) const {
        return m_sentChunks.count(chunk) > 0;
    }

    // === CHUNK SENDER (Minecraft's PlayerChunkSender) ===

    void PlayerSession::MarkChunkReadyToSend(Game::Math::ChunkPos pos) {
        m_pendingChunkLoads.erase(pos);    // No longer waiting for load

        // Don't queue for sending if the chunk was removed from the watch set
        // while we were waiting for it to load (race condition)
        if (m_watchSet.find(pos) == m_watchSet.end()) {
            Log::Debug("MarkChunkReadyToSend: chunk (%d, %d) no longer in watch set, skipping",
                      pos.x, pos.z);
            return;
        }

        m_pendingChunksToSend.insert(pos); // Ready to send to client
    }

    void PlayerSession::DropChunk(Game::Math::ChunkPos pos) {
        m_pendingChunkLoads.erase(pos);
        if (!m_pendingChunksToSend.erase(pos)) {
            // Wasn't pending to send — if already sent, send unload to client
            if (m_sentChunks.erase(pos)) {
                SendChunkUnload(pos);
            }
        }
    }

    void PlayerSession::SendNextChunks(Game::World* world) {
        if (!world || !m_connection) return;
        if (m_pendingChunksToSend.empty()) return;
        PROFILE_ZONE_N("SendNextChunks");

        // Back-pressure: don't send if too many unacknowledged batches
        if (m_unackedBatches >= m_maxUnackedBatches) return;

        // Accumulate fractional budget
        float maxBatchSize = std::max(1.0f, m_desiredChunksPerTick);
        m_batchQuota = std::min(m_batchQuota + m_desiredChunksPerTick, maxBatchSize);

        if (m_batchQuota < 1.0f) return;

        int maxBatch = static_cast<int>(m_batchQuota);

        // Collect loaded chunks, sorted by distance from anchor
        struct ChunkDist {
            Game::Math::ChunkPos pos;
            std::shared_ptr<Game::Chunk> chunk;
            int distSq;
        };
        std::vector<ChunkDist> candidates;
        candidates.reserve(m_pendingChunksToSend.size());

        // Also collect chunks to remove from pending if they left the watch set
        std::vector<Game::Math::ChunkPos> staleChunks;

        for (const auto& pos : m_pendingChunksToSend) {
            // Skip chunks that are no longer in the watch set (race condition)
            if (m_watchSet.find(pos) == m_watchSet.end()) {
                staleChunks.push_back(pos);
                continue;
            }

            auto chunk = world->GetChunk(pos.x, pos.z);
            if (!chunk) continue;  // Not loaded yet — skip, will be picked up later

            int dx = pos.x - m_anchorChunk.x;
            int dz = pos.z - m_anchorChunk.z;
            candidates.push_back({pos, chunk, dx * dx + dz * dz});
        }

        // Remove stale chunks from pending set
        for (const auto& pos : staleChunks) {
            m_pendingChunksToSend.erase(pos);
            Log::Debug("SendNextChunks: removed stale chunk (%d, %d) from pending (no longer watched)",
                      pos.x, pos.z);
        }

        if (candidates.empty()) return;

        size_t toSend = std::min(static_cast<size_t>(maxBatch), candidates.size());
        if (candidates.size() > toSend) {
            std::partial_sort(candidates.begin(), candidates.begin() + toSend, candidates.end(),
                              [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        } else {
            std::sort(candidates.begin(), candidates.end(),
                      [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        }

        // Send ChunkBatchStartS2C
        {
            auto data = Network::Serialization::Serialize(Network::ChunkBatchStartS2CPacket{});
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchStartS2C), data);
        }

        // Send each chunk
        size_t sentCount = 0;
        for (size_t i = 0; i < toSend; ++i) {
            const auto& cd = candidates[i];
            PROFILE_ZONE_N("SerializeChunk");

            // Build ChunkDataS2CPacket
            Network::ChunkDataS2CPacket packet;
            packet.chunkX = cd.pos.x;
            packet.chunkZ = cd.pos.z;
            packet.groundUpContinuous = true;
            packet.primaryBitmask = 0;

            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                const auto* section = cd.chunk->GetSection(sectionY);
                if (!section) continue;

                uint16_t nonAirCount = 0;
                for (size_t j = 0; j < section->blocks.size(); ++j) {
                    if (section->blocks[j] != static_cast<uint16_t>(Game::BlockID::Air)) {
                        nonAirCount++;
                    }
                }
                if (nonAirCount == 0) continue;

                packet.primaryBitmask |= (1 << sectionY);

                Network::ChunkDataS2CPacket::SectionData sectionData;
                sectionData.blockCount = nonAirCount;
                sectionData.bitsPerEntry = 16; // Direct block IDs

                const size_t blocksPerSection = 16 * 16 * 16;
                const size_t blocksPerLong = 64 / 16; // 4 blocks per uint64_t
                sectionData.dataArray.resize((blocksPerSection + blocksPerLong - 1) / blocksPerLong, 0);

                for (size_t j = 0; j < blocksPerSection; ++j) {
                    uint16_t blockId = section->blocks[j];
                    size_t longIndex = j / blocksPerLong;
                    size_t bitOffset = (j % blocksPerLong) * 16;
                    sectionData.dataArray[longIndex] |= (static_cast<uint64_t>(blockId) << bitOffset);
                }

                // Block states ride as their own plane, and only when the
                // section has any — see the note on SectionData::states.
                if (section->HasStates()) {
                    sectionData.states = section->states;
                }

                packet.sections.push_back(std::move(sectionData));
            }

            // Noise biomes ride alongside the blocks — the client needs them to
            // tint grass, foliage and water, and it has no generator of its own
            // to derive them from.
            packet.biomes = cd.chunk->biomes;

            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkDataS2C), data);

            // Move from pending to sent
            m_pendingChunksToSend.erase(cd.pos);
            m_sentChunks.insert(cd.pos);
            sentCount++;

            Log::Debug("Sent chunk (%d, %d) to player %u", cd.pos.x, cd.pos.z, m_playerId);
        }

        // Send ChunkBatchFinishedS2C
        {
            Network::ChunkBatchFinishedS2CPacket finishPacket(static_cast<int32_t>(sentCount));
            auto data = Network::Serialization::Serialize(finishPacket);
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchFinishedS2C), data);
        }

        m_batchQuota -= static_cast<float>(sentCount);
        m_unackedBatches++;

        Log::Debug("Sent chunk batch: %zu chunks (quota=%.1f, unacked=%d, rate=%.1f) to player %u",
                  sentCount, m_batchQuota, m_unackedBatches, m_desiredChunksPerTick, m_playerId);
    }

    void PlayerSession::OnChunkBatchAck(float desiredRate) {
        m_unackedBatches--;
        m_desiredChunksPerTick = std::isnan(desiredRate) ? 0.01f : std::clamp(desiredRate, 0.01f, 64.0f);
        if (m_unackedBatches == 0) m_batchQuota = 1.0f;
        m_maxUnackedBatches = 10;

        Log::Debug("Chunk batch ack: desiredRate=%.2f, unacked=%d, maxUnacked=%d (player %u)",
                  m_desiredChunksPerTick, m_unackedBatches, m_maxUnackedBatches, m_playerId);
    }

    void PlayerSession::SendChunkUnload(Game::Math::ChunkPos chunk) {
        // Send UnloadChunkS2CPacket directly through the connection
        // (SendScheduler's sendCallback is not implemented, so bypass it)
        if (m_connection) {
            Network::UnloadChunkS2CPacket packet(chunk.x, chunk.z);
            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::UnloadChunkS2C), data);
        }

        // Remove from sets
        m_watchSet.erase(chunk);
        m_sentChunks.erase(chunk);
        m_pendingChunksToSend.erase(chunk);

        // Clear any pending diffs for this chunk
        m_pendingDiffs.erase(chunk);
        
        Log::Debug("UNLOAD SENT: chunk (%d, %d) to player %u",
                  chunk.x, chunk.z, m_playerId);
    }

    // === BLOCK UPDATES ===

    void PlayerSession::QueueBlockChange(int worldX, int worldY, int worldZ, Game::BlockID newBlock) {
        // Calculate chunk and local coordinates
        Game::Math::ChunkPos chunk(
            worldX >> 4,  // divide by 16
            worldZ >> 4
        );
        
        // Check if chunk is watched
        if (!IsWatching(chunk)) {
            return;
        }
        
        // NOTE: Block change accumulation is now handled centrally by SectionChangeAccumulator
        // This method is kept for compatibility but doesn't queue for per-player processing
        // The change will be accumulated in World::SetBlock and broadcast by ChunkDeltaBroadcaster
    }

    void PlayerSession::QueueSectionChanges(Game::Math::ChunkPos chunk, int section,
                                           const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes) {
        if (!IsWatching(chunk)) {
            return;
        }
        
        for (const auto& change : changes) {
            CoalesceBlockChange(chunk, section, change.localX, change.localY, change.localZ,
                                change.blockId, change.blockState);
        }
        
        if (HasSentChunk(chunk)) {
            m_diffQueue.push({chunk, section});
        }
    }

    // NOTE: ProcessDiffs is deprecated - block changes are now handled by ChunkDeltaBroadcaster
    // This method is kept for compatibility but does nothing
    void PlayerSession::ProcessDiffs(SendScheduler* scheduler) {
        // Block change broadcasting is now centralized in ChunkDeltaBroadcaster::flush()
        return;
        size_t maxDiffBytes = m_config.maxDiffBytesPerTick;
        size_t diffBytesThisTick = 0;
        
        while (!m_diffQueue.empty() && diffBytesThisTick < maxDiffBytes) {
            auto [chunk, section] = m_diffQueue.front();
            m_diffQueue.pop();
            
            // Get pending diffs for this chunk section
            auto chunkIt = m_pendingDiffs.find(chunk);
            if (chunkIt == m_pendingDiffs.end()) {
                continue;
            }
            
            auto sectionIt = chunkIt->second.find(section);
            if (sectionIt == chunkIt->second.end()) {
                continue;
            }
            
            auto& diffs = sectionIt->second;
            if (diffs.changes.empty()) {
                continue;
            }
            
            // Build and send packet based on number of changes
            if (diffs.changes.size() == 1) {
                // Single block change - use simple packet
                auto& [packedPos, blockId] = *diffs.changes.begin();
                int localX = (packedPos >> 8) & 0xF;
                int localY = (packedPos >> 4) & 0xF;
                int localZ = packedPos & 0xF;
                
                // Convert to world coordinates
                int worldX = chunk.x * 16 + localX;
                int worldY = section * 16 + localY - 64;  // Adjust for world height
                int worldZ = chunk.z * 16 + localZ;
                
                Network::BlockChangeS2CPacket packet(worldX, worldY, worldZ,
                                                     blockId.id, blockId.state);
                SendSingleBlockChange(packet);
                
                // Estimate packet size
                size_t estimatedPacketSize = 20; // Single block change is small
                diffBytesThisTick += estimatedPacketSize;
            } else {
                // Multiple changes in same section - use section update packet
                Network::ClientboundSectionBlocksUpdateS2CPacket packet(chunk, section);
                
                for (const auto& [packedPos, blockId] : diffs.changes) {
                    uint8_t localX = (packedPos >> 8) & 0xF;
                    uint8_t localY = (packedPos >> 4) & 0xF;
                    uint8_t localZ = packedPos & 0xF;
                    packet.AddChange(localX, localY, localZ,
                                     static_cast<uint16_t>(blockId.id), blockId.state);
                }
                
                SendSectionBlocksUpdate(packet);
                
                // Estimate packet size (each block change is roughly 4 bytes as VarInt)
                size_t estimatedPacketSize = diffs.changes.size() * 4 + 16; // +16 for packet header
                diffBytesThisTick += estimatedPacketSize;
            }
            
            // Clear processed diffs
            chunkIt->second.erase(sectionIt);
            if (chunkIt->second.empty()) {
                m_pendingDiffs.erase(chunkIt);
            }
            
            m_diffsOutThisTick++;
        }
    }

    // === PACKET HANDLING ===

    void PlayerSession::HandlePlayerMove(const Network::PlayerMoveC2SPacket& packet) {
        // Drop stale client-predicted moves that were in flight when we issued a
        // teleport (the client may have sent 1–2 MovePlayer packets at the old
        // position before processing our ClientboundPlayerPosition). Applying them
        // would revert the server-side position to the pre-teleport spot and other
        // clients would see this player flicker / stay behind. Matches MC's
        // ServerGamePacketListenerImpl.handleMovePlayer awaitingPositionFromClient
        // null-check.
        if (m_connection && m_connection->IsAwaitingTeleportAck()) {
            return;
        }

        // Dead players don't move — MC freezes the body until the client
        // sends PERFORM_RESPAWN (ServerGamePacketListenerImpl.handleMovePlayer
        // returns early when player.isImmobile()/dead).
        if (m_player && m_player->isDead()) {
            return;
        }

        // Movement statistics BEFORE the position write (needs the old Y /
        // horizontal delta) — fall-distance accumulation + exhaustion
        // sources, mirroring ServerPlayer.checkMovementStatistics and
        // LivingEntity.checkFallDamage.
        if (m_player) {
            UpdateMovementStats(packet);
        }

        UpdatePosition(packet.position, packet.rotation);
        if (m_player) {
            m_player->setSneaking(packet.isCrouching);
        }
    }

    void PlayerSession::UpdateMovementStats(const Network::PlayerMoveC2SPacket& packet) {
        ServerPlayer& p = *m_player;
        const glm::dvec3 oldPos = p.getPosition();
        const glm::dvec3 newPos = glm::dvec3(packet.position);

        // First move after join/teleport snaps can produce huge deltas —
        // teleport() already resets fall distance; distance-based exhaustion
        // is naturally capped by the anti-cheat in setPosition, good enough.
        const double dy = newPos.y - oldPos.y;
        const double dx = newPos.x - oldPos.x;
        const double dz = newPos.z - oldPos.z;
        const double horizontal = std::sqrt(dx * dx + dz * dz);

        // Feet-in-water check (server-side; block at the foot position).
        bool inWater = false;
        if (auto* server = g_integratedServer.get()) {
            if (Game::World* world = server->GetWorld()) {
                const glm::ivec3 feet(static_cast<int>(std::floor(newPos.x)),
                                      static_cast<int>(std::floor(newPos.y)),
                                      static_cast<int>(std::floor(newPos.z)));
                inWater = world->GetBlock(feet.x, feet.y, feet.z) == Game::BlockID::Water;
            }
        }

        // ── Fall damage — client-reported landing distance. The CLIENT's
        //    physics tracks the fall (PlayerPhysics::fallDistance) because
        //    only it knows exact ground contact: reconstructing falls from
        //    20 Hz position snapshots missed bunny-hop landings (land + jump
        //    inside one tick never shows an onGround packet) and stacked hop
        //    descents into phantom damage. Formula per MC
        //    LivingEntity.calculateFallDamage: floor(fd + 1e-6 - 3.0)
        //    (SAFE_FALL_DISTANCE = 3, FALL_DAMAGE_MULTIPLIER = 1).
        //    Sanity clamp: terminal-velocity falls in a 384-block world
        //    can't meaningfully exceed ~512 blocks.
        (void)dy;
        if (packet.fallDistance > 0.0f && !p.isFlying() && !inWater) {
            const float fd = std::min(packet.fallDistance, 512.0f);
            const int dmg = static_cast<int>(std::floor(fd + 1.0e-6f - 3.0f));
            if (dmg > 0) {
                p.damage(static_cast<float>(dmg), DamageSource::FALL);
            }
        }

        // ── Exhaustion sources — FoodConstants: sprint 0.1/m, swim 0.01/m,
        //    jump 0.05 (sprint-jump 0.2). Walking costs nothing in modern MC.
        //    Survival/adventure only (Player.causeFoodExhaustion no-ops when
        //    invulnerable, i.e. creative/spectator).
        const GameMode mode = p.getGameMode();
        if (mode == GameMode::SURVIVAL || mode == GameMode::ADVENTURE) {
            // Ignore absurd per-packet deltas (teleport races) — MC's stat
            // path rounds per-tick distances, which are small; 10 m per move
            // packet (~200 m/s) is a safe outlier cutoff.
            if (horizontal > 0.0 && horizontal < 10.0) {
                if (inWater) {
                    p.getFoodData().addExhaustion(static_cast<float>(0.01 * horizontal));
                } else if (packet.isSprinting && packet.onGround) {
                    p.getFoodData().addExhaustion(static_cast<float>(0.1 * horizontal));
                }
            }
            if (packet.jumpedThisTick) {
                p.getFoodData().addExhaustion(packet.isSprinting ? 0.2f : 0.05f);
            }
        }
    }

    void PlayerSession::HandleBlockAction(const Network::BlockActionC2SPacket& packet) {
        // MC acks EVERY ServerboundPlayerActionPacket regardless of outcome
        // (ServerGamePacketListenerImpl.handlePlayerAction line 1332) — the ack
        // is "I processed this", not "I agreed with it". Rejections still get
        // corrected, because a refused break leaves the block where it was and
        // the client's prediction rolls back to it on retirement.
        //
        // This runs before the early returns below on purpose: a reach-check
        // rejection MUST still ack, or the client's predicted break would be
        // stranded forever, permanently swallowing every future server update
        // at that position.
        AckBlockChangesUpTo(packet.sequenceNumber);

        if (!m_player) return;

        switch (packet.action) {
            // MC's START_DESTROY / ABORT_DESTROY are purely informational
            // (the server doesn't track per-player mining progress for
            // single-player; the client is authoritative on timing).
            // Treat them as no-ops for now — could be wired into anti-cheat /
            // per-player "currently mining" state later.
            case Network::BlockActionType::START_DESTROY:
            case Network::BlockActionType::ABORT_DESTROY:
                break;
            // Both BREAK (legacy) and STOP_DESTROY (new) finalize the dig.
            case Network::BlockActionType::STOP_DESTROY:
            case Network::BlockActionType::BREAK: {
                glm::ivec3 pos(packet.worldX, packet.worldY, packet.worldZ);

                // Validate reach
                glm::vec3 blockCenter = glm::vec3(pos) + glm::vec3(0.5f);
                if (!m_player->canReach(blockCenter)) {
                    Log::Warning("HandleBlockAction: Player %u cannot reach (%d,%d,%d)",
                                m_playerId, pos.x, pos.y, pos.z);
                    return;
                }

                // Get world and break the block (set to air)
                IntegratedServer* server = g_integratedServer.get();
                if (!server || !server->GetWorld()) return;
                Game::World* world = server->GetWorld();

                // Trust the packet's blockId for inventory purposes. In integrated-server
                // mode the client and server share one World, so by the time we get here
                // the client's local SetBlock(Air) prediction has already cleared the
                // world block — world->GetBlock(pos) would return Air. The packet's
                // blockId carries what the player actually broke. Fall back to the world
                // value when the packet doesn't supply one (e.g. older clients).
                Game::BlockID oldBlock = (packet.blockId != Game::BlockID::Air)
                                       ? packet.blockId
                                       : world->GetBlock(pos.x, pos.y, pos.z);
                if (oldBlock == Game::BlockID::Air) return;
                // Bedrock is unbreakable in survival/adventure, but creative
                // destroys it outright (MC's ServerPlayerGameMode never
                // consults destroyTime on the creative path).
                const bool creativeBreak =
                    (m_player->getGameMode() == Server::GameMode::CREATIVE);
                if (oldBlock == Game::BlockID::Bedrock && !creativeBreak) return;

                // SetBlock may already be a no-op (the world is already Air in integrated
                // mode), but call it anyway so dedicated multiplayer still clears the
                // server's world.
                world->SetBlock(pos.x, pos.y, pos.z, Game::BlockID::Air);
#if ENABLE_PORTAL_GUN
                // Remove any portal mounted on this block. Block-break
                // bypasses IntegratedServer::ApplyBlockChange so the
                // notification has to happen here too.
                Game::Portal::ServerRegistry().OnBlockChanged(pos);
#endif
                Log::Debug("HandleBlockAction: Player %u broke block at (%d,%d,%d)",
                          m_playerId, pos.x, pos.y, pos.z);

                // Mining exhaustion — MC Player.causeFoodExhaustion on block
                // destroy, EXHAUSTION_MINE = 0.005F (FoodConstants.java:23).
                // Survival only (creative never accrues exhaustion).
                if (m_player->getGameMode() == Server::GameMode::SURVIVAL) {
                    m_player->getFoodData().addExhaustion(0.005f);
                }

                // Add the broken block to the player's inventory and broadcast slot
                // deltas. Without this the server-side inventory stays empty even
                // though the client predicts the pickup, causing inventory clicks to
                // be no-ops (server sees empty slots) and shift-click-clear to leave
                // ghost items on screen (no SetSlot deltas for already-empty slots).
                //
                // Creative is exempt: MC's ServerPlayerGameMode.destroyBlock
                // bails out immediately after removing the block when
                // isCreative(), so no drop is ever produced.
                if (m_connection && !creativeBreak) {
                    // Mutate only. BroadcastContainerChanges (this tick, from
                    // Tick()) diffs the container against m_remoteSlots and
                    // sends the deltas with a correct stateId. Sending them
                    // by hand here left m_remoteSlots stale AND stamped
                    // stateId=0 onto the client, which disabled the click
                    // staleness guard until the next full sync.
                    m_player->getInventory().AddBlocks(oldBlock, 1);
                }
                break;
            }
            case Network::BlockActionType::PLACE:
                // Handled by HandleUseItemOn
                break;
            case Network::BlockActionType::INTERACT:
                // TODO: Implement block interaction (chests, doors, etc.)
                break;
        }
    }
    
    void PlayerSession::HandleHeldItemChange(const Network::HeldItemChangeC2SPacket& packet) {
        if (!m_player) return;
        int slot = packet.slot;
        if (slot >= 0 && slot < 9) {
            // Only update the selected-slot index. The packet's `blockId` field is
            // legacy from when the client was inventory-authoritative — applying it
            // via setHotbarBlock(64) would clobber the real (server-authoritative)
            // stack with a 64-count of whatever block the client thinks is here.
            m_player->selectHotbarSlot(slot);
            Log::Debug("[PlayerSession] Player %u: selected slot %d", m_playerId, slot);
        }
    }

    namespace {
        // MC compares with ItemStack.matches (item + count + components).
        // Game::ItemStacksMatch is exactly that; it used to be open-coded here
        // by serializing both stacks and diffing the bytes, which allocated two
        // PacketBuffers per call — 46 slots × every player × every tick.
        bool StacksIdentical(const Game::ItemStack& a, const Game::ItemStack& b) {
            return Game::ItemStacksMatch(a, b);
        }
    } // namespace

    void PlayerSession::InvalidateRemoteSlot(int index) {
        if (index < 0 || index >= static_cast<int>(m_remoteSlots.size())) return;
        // count = -1 is unreachable for a real stack (empty slots are count 0),
        // so ItemStacksMatch is guaranteed to report a difference and the diff
        // will resend this slot.
        m_remoteSlots[index] = Game::ItemStack{};
        m_remoteSlots[index].count = -1;
    }

    void PlayerSession::InvalidateRemoteInventorySlot(int inventoryIndex) {
        if (!m_player) return;
        InvalidateRemoteSlot(m_player->container().MenuIndexForInventorySlot(inventoryIndex));
    }

    void PlayerSession::BroadcastContainerChanges() {
        if (!m_player || !m_connection) return;

        // Walk the OPEN MENU's slots, not the inventory's. For the player's own
        // menu the two are the same list; for a crafting table the menu also
        // covers the table's grid and output, which the inventory knows nothing
        // about. Slot::GetItem resolves each index to the right container.
        auto& menu = m_player->container();
        const int slotCount = menu.SlotCount();
        if (static_cast<int>(m_remoteSlots.size()) != slotCount) {
            // The menu changed under us without a full sync. Re-seed and send
            // everything rather than diff against a mismatched model.
            SendInventoryFull();
            return;
        }

        for (int i = 0; i < slotCount; ++i) {
            const Game::ItemStack& actual = menu.GetSlot(i).GetItem();
            if (StacksIdentical(actual, m_remoteSlots[i])) continue;

            m_remoteSlots[i] = actual;
            BumpContainerState();
            Network::InventorySetSlotS2CPacket out;
            out.slotIndex = static_cast<int16_t>(i);
            out.stack     = actual;
            out.stateId   = m_containerStateId;
            auto data = Network::Serialization::Serialize(out);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::InventorySetSlotS2C), data);
        }

        const Game::ItemStack& carried = m_player->getCarried();
        if (!StacksIdentical(carried, m_remoteCarried)) {
            m_remoteCarried = carried;
            BumpContainerState();
            Network::InventorySetCarriedS2CPacket out;
            out.stack   = carried;
            out.stateId = m_containerStateId;
            auto data = Network::Serialization::Serialize(out);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::InventorySetCarriedS2C), data);
        }
    }

    // Flip to 1 to trace every container click through the authoritative
    // handler. Logs the cursor and clicked slot on BOTH sides of the call, so
    // a divergence between what the client is showing and what the server
    // believes shows up as the first line whose "before" state is not what the
    // previous line's "after" state left behind.
#define INVENTORY_CLICK_TRACE 0

    void PlayerSession::HandleInventoryClick(const Network::InventoryClickC2SPacket& packet) {
        if (!m_player || !m_connection) return;

#if INVENTORY_CLICK_TRACE
        const auto& traceInv = m_player->getInventory();
        const auto  beforeCarried = m_player->getCarried();
        const auto  beforeSlot = (packet.slotIndex >= 0 && packet.slotIndex < Game::Inventory::TOTAL_SIZE)
                               ? traceInv.GetSlot(packet.slotIndex) : Game::ItemStack{};
        Log::Info("[ClickTrace] IN  action=%u slot=%d btn=%u | cursor=%u x%d | slot=%u x%d",
                  (unsigned)packet.action, (int)packet.slotIndex, (unsigned)packet.button,
                  (unsigned)beforeCarried.itemId, beforeCarried.count,
                  (unsigned)beforeSlot.itemId, beforeSlot.count);
#endif

        // MC handleContainerClick's very first check: does this click even
        // target the menu that is open? The server bumps containerId on close,
        // so a click aimed at a menu that has since been replaced is dropped
        // rather than applied to whatever took its place. id 0 means the client
        // has not learned an id yet (pre-first-snapshot) and is accepted.
        if (packet.containerId != 0
            && packet.containerId != m_player->container().containerId) {
            Log::Debug("[PlayerSession %u] Click for container %u (open: %u) — dropped",
                       m_playerId, packet.containerId,
                       m_player->container().containerId);
            return;
        }

        auto result = m_player->container().DoClick(packet);

#if INVENTORY_CLICK_TRACE
        {
            const auto afterCarried = m_player->getCarried();
            const auto afterSlot = (packet.slotIndex >= 0 && packet.slotIndex < Game::Inventory::TOTAL_SIZE)
                                 ? traceInv.GetSlot(packet.slotIndex) : Game::ItemStack{};
            Log::Info("[ClickTrace] OUT cursor=%u x%d | slot=%u x%d | changed=%zu carriedChanged=%d",
                      (unsigned)afterCarried.itemId, afterCarried.count,
                      (unsigned)afterSlot.itemId, afterSlot.count,
                      result.changedSlots.size(), result.carriedChanged ? 1 : 0);
        }
#endif

        // Adopt the client's PREDICTED outcome as our model of what it now
        // believes (MC: setRemoteSlotNoCopy / setRemoteCarried for every entry
        // in the packet's changedSlots). This is the piece that makes the diff
        // below able to catch a slot the client wrote but the server did not —
        // the client tells us it wrote it, so the model disagrees with the
        // truth and we correct it. Without this, such a slot is invisible to
        // any delta scheme and survives as a ghost item.
        if (packet.hasPrediction) {
            for (const auto& [slot, stack] : packet.predictedSlots) {
                if (slot < m_remoteSlots.size()) m_remoteSlots[slot] = stack;
            }
            m_remoteCarried = packet.predictedCarried;
        }

        // Predicted against a revision we have already moved past → the
        // client's whole picture is suspect, so replace it wholesale rather
        // than patching (MC does the same on a state mismatch).
        if (packet.stateId != 0 && packet.stateId != m_containerStateId) {
            Log::Debug("[PlayerSession %u] Click predicted against state %u (server at %u) — full resync",
                       m_playerId, packet.stateId, m_containerStateId);
            SendInventoryFull();
            return;
        }

        // Normal path: send only what the client actually has wrong. A correct
        // prediction sends nothing at all.
        (void)result;
        BroadcastContainerChanges();

        // TODO: spawn dropped item entity if !result.droppedItem.IsEmpty()
    }

    void PlayerSession::HandleInventoryClose(const Network::InventoryCloseC2SPacket&) {
        if (!m_player || !m_connection) return;
#if INVENTORY_CLICK_TRACE
        {
            const auto c = m_player->getCarried();
            Log::Info("[ClickTrace] CLOSE cursor=%u x%d (goes back into inventory)",
                      (unsigned)c.itemId, c.count);
        }
#endif
        // MC drops the cursor item as a world entity when the inventory closes,
        // but since we have no item-entity system yet, dropping = the item just
        // disappears. Better UX: try to put the carried stack back into the
        // player's inventory (matching what the user expects when pressing E
        // without intentionally dropping). Only items that don't fit get
        // silently dropped (acceptable since the inventory is large).
        // MC doCloseContainer → menu.removed(player): a menu with its own
        // storage hands it back before it disappears, and containerMenu drops
        // to inventoryMenu so the id the client was clicking against stops
        // being current — any click still in flight for the closed menu is then
        // rejected by the containerId guard in HandleInventoryClick.
        //
        // closeContainerMenu does both for a block container. With only the
        // player's own menu open there is nothing to swap, so its 2x2 grid is
        // emptied and the id bumped here instead — otherwise items parked in
        // the crafting square would sit there invisibly until next time.
        if (m_player->hasOpenContainerMenu()) {
            m_player->closeContainerMenu();
        } else {
            Game::ContainerClickResult removal;
            m_player->container().Removed(removal);
            m_player->container().containerId++;
        }

        int leftover = 0;
        auto& carried = m_player->getCarried();
        if (!carried.IsEmpty()) {
            // AddStack, not AddItems: the cursor may hold an enchanted book or
            // any other stack with per-stack components, and (id, count) would
            // drop them.
            leftover = m_player->getInventory().AddStack(carried);
            // Whatever didn't fit is silently dropped (no item-entity system).
            carried.Clear();
        }

        // Always full-sync on close, even when nothing moved: containerId rides
        // ONLY on InventoryFullS2C, and a client that never learns the new id
        // would have every subsequent click rejected by the guard above. This
        // is also what MC does when the open menu changes (sendAllDataToRemote).
        // No hand-rolled per-slot sends here — the snapshot covers the returned
        // cursor and every slot it landed in, and refreshes m_remoteSlots.
        SendInventoryFull();

        if (leftover > 0) {
            Log::Debug("[HandleInventoryClose] Dropped %d items (no entity system to spawn them)",
                       leftover);
        }
    }

    void PlayerSession::HandlePlayerAbilities(const Network::PlayerAbilitiesC2SPacket& packet) {
        if (!m_player || !m_connection) return;
        // MC ServerGamePacketListenerImpl.handlePlayerAbilities: only the
        // FLYING bit is client-writable, and only while mayFly. A client
        // claiming flight without permission gets a corrective resend.
        if (m_player->canFly()) {
            m_player->setFlying(packet.flying());
        } else if (packet.flying()) {
            Log::Warning("[PlayerSession %u] Client requested flight without mayFly — correcting",
                         m_playerId);
            m_connection->SendPlayerAbilities(*m_player);
        }
    }

    void PlayerSession::SendInventoryFull() {
        if (!m_player || !m_connection) return;
        const auto& inv = m_player->getInventory();
        auto& menu = m_player->container();

        Network::InventoryFullS2CPacket out;
        out.menuType = m_player->openMenuType();
        out.slots.reserve(static_cast<size_t>(menu.SlotCount()));
        for (int i = 0; i < menu.SlotCount(); ++i) {
            out.slots.push_back(menu.GetSlot(i).GetItem());  // components ride along
        }
        out.carried            = m_player->getCarried();
        out.selectedHotbarSlot = static_cast<uint8_t>(inv.GetSelectedSlot());
        BumpContainerState();
        out.stateId            = m_containerStateId;
        // The only packet carrying containerId — this is how the client learns
        // which menu to stamp on its clicks.
        out.containerId        = m_player->container().containerId;
#if INVENTORY_CLICK_TRACE
        // This is the other path that pushes a cursor to the client (join,
        // instant item-use in DispatchUseItem, respawn). If a stale cursor is
        // being resurrected, expect to see it here.
        Log::Info("[ClickTrace] FULLSYNC cursor=%u x%d",
                  (unsigned)out.carried.itemId, out.carried.count);
#endif

        auto data = Network::Serialization::Serialize(out);
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::InventoryFullS2C), data);

        // The client adopts this snapshot verbatim, so our model of its state
        // is now exactly what we just sent — including its SIZE, which is how
        // the model follows a menu swap.
        m_remoteSlots   = out.slots;
        m_remoteCarried = out.carried;
    }

    void PlayerSession::FlushPendingMenuOpen() {
        if (!m_player || !m_connection) return;
        const auto pending = m_player->takePendingMenuOpen();
        if (!pending) return;

        std::unique_ptr<Game::AbstractContainerMenu> menu;
        std::string title;
        switch (pending->type) {
            case Game::MenuType::Crafting:
                menu  = std::make_unique<Game::CraftingMenu>(&m_player->getInventory());
                title = "Crafting";
                break;
            case Game::MenuType::Inventory:
                // Not a thing a block can ask for — the player menu is always
                // open behind whatever else is.
                return;
        }
        if (!menu) return;

        m_player->openContainerMenu(std::move(menu), pending->type);

        // MC ServerPlayer.openMenu: ClientboundOpenScreenPacket first (so the
        // client builds the matching menu), then the contents.
        Network::OpenScreenS2CPacket open;
        open.containerId = m_player->container().containerId;
        open.menuType    = pending->type;
        open.title       = title;
        auto data = Network::Serialization::Serialize(open);
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::OpenScreenS2C), data);

        SendInventoryFull();
        Log::Debug("[PlayerSession %u] Opened menu type %u (container %u)",
                   m_playerId, static_cast<unsigned>(pending->type),
                   m_player->container().containerId);
    }

    void PlayerSession::HandleUseItemOn(const Network::UseItemOnC2SPacket& packet) {
        // === 1. Thread safety & basic validation ===
        ASSERT_SERVER_THREAD();

        // Record the ack up front so EVERY exit path below is covered,
        // including the bail-outs that predate prediction (no world, stale
        // sequence, …). An interaction that is never acked strands the
        // client's prediction for that position forever, and a stranded
        // prediction permanently swallows all future server block updates
        // there — a far worse failure than acking a request we ignored.
        //
        // Recording early is safe because the ack PACKET is not sent here: it
        // is emitted by FlushBlockChangeAck after this tick's block updates
        // (see IntegratedServer's tick), so any correction this handler sends
        // still reaches the client first.
        AckBlockChangesUpTo(packet.sequence);

        if (!m_player) {
            Log::Warning("HandleUseItemOn: No player attached to session");
            return;
        }
        
        // Get world instance
        IntegratedServer* server = g_integratedServer.get();
        if (!server) {
            Log::Warning("HandleUseItemOn: No integrated server");
            return;
        }
        
        Game::World* world = server->GetWorld();
        if (!world) {
            Log::Warning("HandleUseItemOn: No world available");
            return;
        }
        
        // === 2. Fast guards (reject early, no world touch) ===
        
        // Validate sequence number
        if (packet.sequence <= m_lastInteractionSequence) {
            // Stale packet, ignore
            Log::Debug("HandleUseItemOn: Stale sequence %u <= %u", packet.sequence, m_lastInteractionSequence);
            return;
        }
        
        // Check if connection is in PLAY phase
        if (m_state != State::PLAYING) {
            Log::Warning("HandleUseItemOn: Not in PLAYING state");
            AckInteraction(packet.sequence, false);
            return;
        }
        
        glm::ivec3 clicked(packet.blockX, packet.blockY, packet.blockZ);
        
        // Check if chunk is loaded
        if (!world->IsPositionLoaded(clicked.x, clicked.y, clicked.z)) {
            Log::Warning("HandleUseItemOn: Chunk not loaded at (%d,%d,%d)", clicked.x, clicked.y, clicked.z);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // Build height checks
        if (!world->IsValidPosition(clicked.x, clicked.y, clicked.z)) {
            Log::Warning("HandleUseItemOn: Invalid position (%d,%d,%d)", clicked.x, clicked.y, clicked.z);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // === 3. Rebuild the authoritative hit context ===
        
        // Convert packet data to block hit result
        glm::vec3 hitPoint = Game::faceLocalUVToWorld(
            packet.direction,
            packet.cursorX,
            packet.cursorY,
            packet.cursorZ,
            clicked
        );
        
        Game::BlockHitResult hit(clicked, packet.direction, hitPoint, packet.insideBlock);
        Game::UseOnContext context(world, m_player, packet.hand, hit);
        context.playerYaw = m_player->getYaw();
        context.playerPitch = m_player->getPitch();
        context.altInteract = packet.altInteract;
        
        // === 4. Reach validation ===

        // Reconstruct ray from player eye to hit point
        glm::dvec3 playerPos = m_player->getPosition();
        glm::vec3 eyePos = glm::vec3(playerPos.x, playerPos.y + 1.62, playerPos.z); // Eye height
        float distance = glm::length(hitPoint - eyePos);
        float maxReach = m_player->getGameMode() == GameMode::CREATIVE ? 5.0f : 4.5f;

#if ENABLE_PORTAL_GUN
        // The portal gun fires a projectile that travels up to
        // sv_portal_projectile_delay × BLAST_SPEED ≈ 28.5 m before
        // expiring. Client-side collision sweeps it forward each tick
        // and only sends UseItemOnC2S on the impact face, so the impact
        // point is genuinely much farther than melee reach. Exempt the
        // portal gun from the reach cap so long-range shots place.
        {
            const int slot = m_player->getInventory().GetSelectedSlot();
            const Game::ItemStack& s = m_player->getInventory().GetSlot(
                Game::Inventory::HotbarToIndex(slot));
            if (s.itemId == Game::Items::PortalGun) {
                maxReach = 256.0f;  // long-range portal shots
            }
        }
#endif

        if (distance > maxReach) {
            Log::Warning("HandleUseItemOn: Out of reach %.2f > %.2f", distance, maxReach);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // === 5. MC-style dispatch — mirrors ServerPlayerGameMode.useItemOn
        //    (ServerPlayerGameMode.java lines 329-381) ===
        //
        //   feature-gating: if !state.block.isEnabled(level.enabledFeatures())
        //       → return FAIL                                    [TODO, see below]
        //   if gameMode == SPECTATOR:
        //       open menu provider if any (chests etc.)          [TODO, no menus]
        //       → CONSUME / PASS
        //   suppressUsingBlock = isSecondaryUseActive() && haveSomethingInOurHands
        //   if !suppressUsingBlock:
        //       result = block.useItemOn(stack, ...)            // block reacts to held item
        //       if consumesAction:
        //           CriteriaTriggers.ITEM_USED_ON_BLOCK         [TODO, no advancements]
        //           → done
        //       if result == TryEmptyHandInteraction && mainHand:
        //           result = block.useWithoutItem(...)          // block reacts as if empty hand
        //           if consumesAction:
        //               CriteriaTriggers.DEFAULT_BLOCK_USE      [TODO, no advancements]
        //               → done
        //   if !stack.isEmpty() && !player.cooldowns.isOnCooldown(stack):  [TODO, no cooldowns]
        //       if hasInfiniteMaterials (creative):
        //           int count = stack.count
        //           result = stack.useOn(ctx)
        //           stack.count = count                         // creative count preservation
        //       else:
        //           result = stack.useOn(ctx)
        //       if consumesAction:
        //           CriteriaTriggers.ITEM_USED_ON_BLOCK         [TODO, no advancements]
        //           → done
        //   else PASS → fall through to BlockItem placement.
        const Game::BlockID clickedBlkId  = world->GetBlock(clicked.x, clicked.y, clicked.z);
        const Game::Block&  clickedBlock  = Game::BlockRegistry::Get(clickedBlkId);
        const int           selectedSlot  = m_player->getInventory().GetSelectedSlot();
        Game::ItemStack&    heldStack     = m_player->getInventory().MutableSlot(
                                                Game::Inventory::HotbarToIndex(selectedSlot));
        const Game::Item&   heldItem      = Game::ItemRegistry::Get(heldStack.itemId);
        const bool isMainHand             = (packet.hand == 0);
        const bool isCreative             = (m_player->getGameMode() == Server::GameMode::CREATIVE);
        // const bool isSpectator         = (m_player->getGameMode() == Server::GameMode::SPECTATOR);

        // (void)clickedBlkId; — kept for future feature-flag check:
        //   if (!IsBlockFeatureEnabled(clickedBlkId)) { ResyncAndAck(...); return; }

        // TODO(spectator): if (isSpectator) {
        //     auto provider = clickedBlock.menuProvider;
        //     if (provider) { m_player->openMenu(provider); AckInteraction(true); return; }
        //     AckInteraction(false); return;
        // }
        // (Spectators today fall through to the regular dispatch — same as if SURVIVAL —
        // which is harmless because they can't actually mutate the world via SetBlock
        // gating elsewhere. Revisit once we have menu providers.)

        // MC: `suppressUsingBlock = isSecondaryUseActive() && haveSomethingInOurHands`
        //  — sneaking + something in hand → skip block-use, run item.useOn directly.
        const bool sneaking               = m_player->IsSneaking();
        const bool somethingInHands       = !heldStack.IsEmpty();
        const bool suppressBlockUse       = sneaking && somethingInHands;

        if (!suppressBlockUse) {
            if (clickedBlock.useItemOn) {
                Game::UseResult r = clickedBlock.useItemOn(
                    heldStack, world, clicked, m_player, packet.hand, hit);
                if (Game::ConsumesAction(r)) {
                    // TODO(advancements): CriteriaTriggers.ITEM_USED_ON_BLOCK.trigger(player, pos, stackCopy);
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
                if (r == Game::UseResult::TryEmptyHandInteraction && isMainHand
                    && clickedBlock.useWithoutItem) {
                    Game::UseResult r2 = clickedBlock.useWithoutItem(
                        world, clicked, m_player, hit);
                    if (Game::ConsumesAction(r2)) {
                        // TODO(advancements): CriteriaTriggers.DEFAULT_BLOCK_USE.trigger(player, pos);
                        AckInteraction(packet.sequence, true);
                        m_lastInteractionSequence = packet.sequence;
                        return;
                    }
                }
            } else if (clickedBlock.useWithoutItem) {
                // Block declares no item-on-block reaction but does have an
                // empty-hand reaction (the common case: doors, levers, buttons).
                if (heldStack.IsEmpty() || isMainHand) {
                    Game::UseResult r = clickedBlock.useWithoutItem(
                        world, clicked, m_player, hit);
                    if (Game::ConsumesAction(r)) {
                        // TODO(advancements): CriteriaTriggers.DEFAULT_BLOCK_USE.trigger(player, pos);
                        AckInteraction(packet.sequence, true);
                        m_lastInteractionSequence = packet.sequence;
                        return;
                    }
                }
            }
        }

        // Item.useOn — the item acts on the targeted block (FlintAndSteel,
        // Hoe, Bucket, Shovel, BoneMeal, Shears, …).
        // TODO(cooldowns): if (m_player->cooldowns().isOnCooldown(heldStack)) skip this block.
        // (MC.useItemOn line 362: `if (!itemStack.isEmpty() && !player.getCooldowns().isOnCooldown(itemStack))`)
        if (heldItem.useOn && !heldStack.IsEmpty()) {
            // MC's "creative count preservation" trick (ServerPlayerGameMode.java
            // line 365-371): in creative, snapshot count BEFORE useOn, restore it
            // AFTER, so a single block placed/transformed doesn't decrement the
            // infinite stack. We mutate the stack via the &-reference, so the
            // snapshot/restore must wrap the call.
            const int countBefore = heldStack.count;
            Game::UseResult r = heldItem.useOn(context, heldStack);
            if (isCreative) {
                heldStack.count = countBefore;
            }
            if (Game::ConsumesAction(r)) {
                // TODO(advancements): CriteriaTriggers.ITEM_USED_ON_BLOCK.trigger(player, pos, stackCopy);
                AckInteraction(packet.sequence, true);
                m_lastInteractionSequence = packet.sequence;
                return;
            }
            if (r == Game::UseResult::Fail) {
                // Item explicitly rejected — DO NOT fall through to placement.
                AckInteraction(packet.sequence, false);
                m_lastInteractionSequence = packet.sequence;
                return;
            }
        }

        // === 6. BlockItem placement fallback (existing behaviour) ===

        // Get block to place from player's hand
        Game::BlockID blockToPlace = m_player->getHeldBlock();

        // If holding air or no block, can't place
        if (blockToPlace == Game::BlockID::Air) {
            // Use-item fallthrough — MC's CLIENT falls through useItemOn →
            // useItem when the whole block chain didn't consume
            // (Minecraft.startUseItem, Minecraft.java:1656, iterating
            // MAIN_HAND then OFF_HAND). We have no client-side interaction
            // prediction, so the fallthrough runs server-side here instead.
            // ANY non-empty stack dispatches — Item_DefaultUse routes the
            // component chain (CONSUMABLE → EQUIPPABLE swap → BLOCKS_ATTACKS)
            // and returns Pass harmlessly for inert items. The old gate here
            // (`use duration > 0 || item.use`) skipped armor entirely: its
            // equip lives in the Item_DefaultUse fallback, so right-clicking
            // with a helmet while aiming at the ground never equipped it.
            if (!heldStack.IsEmpty()) {
                const Game::UseResult used = DispatchUseItem(packet.hand);
                if (Game::ConsumesAction(used)) {
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
            }
            {
                const Game::ItemStack& offhand = m_player->getItemInHand(1);
                if (!offhand.IsEmpty() && Game::GetUseDuration(offhand) > 0) {
                    DispatchUseItem(1);
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
            }
            AckInteraction(packet.sequence, false);
            m_lastInteractionSequence = packet.sequence;
            return;
        }
        
        // === 6a. Resolve the placement cell (MC BlockPlaceContext) ===
        //
        // Vanilla asks three questions in a fixed order, and the ORDER is what
        // makes segmented ground cover behave:
        //
        //   replaceClicked = clickedState.canBeReplaced(ctx)          // ctor
        //   getClickedPos() = replaceClicked ? clicked : relativePos
        //   canPlace()      = replaceClicked
        //                     || stateAt(getClickedPos()).canBeReplaced(ctx)
        //
        // Only after that does getStateForPlacement look at the block sitting
        // at the resolved position. That is why clicking the GRASS BLOCK under a
        // leaf litter clump still grows the clump: grass isn't replaceable, so
        // the position resolves up into the litter's own cell, and the growth
        // check happens there rather than on whatever the crosshair touched.
        const Game::BlockID clickedBlockId = world->GetBlock(clicked.x, clicked.y, clicked.z);
        const uint8_t clickedBlockState = world->GetBlockState(clicked.x, clicked.y, clicked.z);

        const bool replaceClicked = Game::CanBeReplacedByPlacement(
            clickedBlockId, clickedBlockState, blockToPlace, sneaking);

        glm::ivec3 targetPos = replaceClicked ? clicked : context.getPlacementPos();

        // Validate target position
        if (!world->IsValidPosition(targetPos.x, targetPos.y, targetPos.z)) {
            Log::Warning("HandleUseItemOn: Target position invalid (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }

        const Game::BlockID targetBlockId = world->GetBlock(targetPos.x, targetPos.y, targetPos.z);
        const uint8_t targetBlockState = world->GetBlockState(targetPos.x, targetPos.y, targetPos.z);

        // MC BlockPlaceContext.canPlace. Without it we'd silently overwrite the
        // block already sitting in the resolved cell — clicking a wall whose
        // +X neighbour holds a slab would replace that slab, consuming an
        // inventory item while appearing to do nothing.
        if (!replaceClicked &&
            !Game::CanBeReplacedByPlacement(targetBlockId, targetBlockState, blockToPlace, sneaking)) {
            Log::Debug("HandleUseItemOn: Target cell already occupied at (%d,%d,%d) by block %u",
                       targetPos.x, targetPos.y, targetPos.z, static_cast<unsigned>(targetBlockId));
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }

        // === 6b. Segmented ground cover grows in place ===
        //
        // MC SegmentableBlock.getStateForPlacement, run against the RESOLVED
        // cell as vanilla does:
        //
        //   BlockState state = level.getBlockState(context.getClickedPos());
        //   return state.is(block) ? state.setValue(segment, min(4, n + 1)) : …
        //
        // The facing is deliberately not recomputed — `state.setValue` mutates
        // the state already there, so a clump never re-orients as you add to it.
        bool growInPlace = false;
        {
            const Game::BlockID base = Game::SegmentedFamilyBase(targetBlockId);
            if (base != Game::BlockID::Air &&
                base == Game::SegmentedFamilyBase(blockToPlace)) {
                const Game::BlockID grown = Game::SegmentedGrowth(targetBlockId);
                if (grown != Game::BlockID::Air) {
                    blockToPlace = grown;
                    growInPlace  = true;
                }
            }
        }
        
        // === 6c. Slab orientation (top vs bottom half) ===
        // Mirrors MC's SlabBlock.getStateForPlacement (SlabBlock.java:66-90):
        // the slab is placed in the TOP half when the player clicked on the
        // bottom face of a block (face == DOWN), or when they clicked the
        // side of a block above its vertical midpoint (cursor.y > 0.5). The
        // BOTTOM half is the default — picked when clicking on a TOP face or
        // the lower portion of a side. We promote each "*SlabTop" variant to
        // its own BlockID, so the rule is a simple BlockID swap here.
        {
            const Game::BlockID topVariant =
                Game::BlockRegistry::SlabTopVariant(blockToPlace);
            if (topVariant != Game::BlockID::Air) {
                bool placeAsTop = false;
                switch (packet.direction) {
                    case 0:                                 // -Y (bottom face)
                        placeAsTop = true;
                        break;
                    case 1:                                 // +Y (top face)
                        placeAsTop = false;
                        break;
                    default:                                // side faces
                        placeAsTop = (packet.cursorY > 0.5f);
                        break;
                }
                if (placeAsTop) {
                    blockToPlace = topVariant;
                }
            }
        }

        // === 7. Validate placement ===

        // MC BlockItem.canPlace → `stateForPlacement.canSurvive(level, pos)`.
        // Only the families with a modelled rule are constrained (see
        // CanSurviveOn); everything else still places anywhere, as before.
        //
        // This is what stops leaf litter from stacking on itself. A full
        // 4-segment clump is no longer replaceable by its own item, so the
        // position resolves to the cell ABOVE — and there LeafLitterBlock's
        // canSurvive asks for a sturdy top face below, which a leaf litter's
        // empty collision shape does not provide.
        {
            const Game::BlockID belowId =
                world->GetBlock(targetPos.x, targetPos.y - 1, targetPos.z);
            const uint8_t belowState =
                world->GetBlockState(targetPos.x, targetPos.y - 1, targetPos.z);
            if (!Game::CanSurviveOn(blockToPlace, belowId, belowState)) {
                Log::Debug("HandleUseItemOn: Block cannot survive at (%d,%d,%d)",
                           targetPos.x, targetPos.y, targetPos.z);
                ResyncAndAck(clicked, targetPos, packet.sequence);
                return;
            }
        }

        // === 8. Collision check with entities ===
        
        // Check if any players are in the target block space
        // TODO: Get all players from PlayerSessionManager
        glm::dvec3 playerPosDouble = m_player->getPosition();
        glm::vec3 playerCollisionPos = glm::vec3(playerPosDouble.x, playerPosDouble.y, playerPosDouble.z);
        glm::vec3 blockCenter = glm::vec3(targetPos) + glm::vec3(0.5f, 0.5f, 0.5f);
        
        // Simple AABB check - player is 0.6x1.8x0.6, block is 1x1x1.
        //
        // Skipped entirely for blocks MC declares `.noCollision()`. Vanilla's
        // gate is `level.isUnobstructed(state, pos, CollisionContext.empty())`
        // (BlockItem.java), which tests the block's COLLISION shape — empty for
        // flowers, grass and leaf litter, so those always place. Without this
        // you can't add a segment to the clump you're standing on, or put a
        // flower down at your own feet.
        bool playerCollides = false;
        if (Game::BlockRegistry::HasCollision(blockToPlace) &&
            std::abs(playerCollisionPos.x - blockCenter.x) < 0.8f &&
            std::abs(playerCollisionPos.z - blockCenter.z) < 0.8f &&
            playerCollisionPos.y < targetPos.y + 1.0f &&
            playerCollisionPos.y + 1.8f > targetPos.y) {
            playerCollides = true;
        }
        
        if (playerCollides) {
            Log::Debug("HandleUseItemOn: Player collides with placement at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }
        
        // TODO: Check collision with other entities
        
        // === 9. Mutate the world ===

        // MC Block.getStateForPlacement, applied through one shared table (see
        // BlockPlacement.cpp). Returns 0 — the block's default state — for
        // anything with no orientation, so it applies unconditionally. The
        // client mirrors this exact call when it predicts the placement, so the
        // block never visibly flips when this authoritative update lands.
        // Growing a clump carries its existing facing across (see 6b); every
        // other placement derives orientation from how the player was standing.
        const uint8_t placedState =
            growInPlace ? targetBlockState
                        : Game::ComputePlacementState(blockToPlace, context);

        bool changed = world->SetBlock(
            targetPos.x, targetPos.y, targetPos.z,
            blockToPlace,
            Game::World::UpdateFlags::All,
            placedState
        );
        
        if (!changed) {
            Log::Warning("HandleUseItemOn: SetBlock failed at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }
        
        // === 10. Run block hooks ===

        // BlockEntity item-data merge. World::SetBlock already created the
        // BE (via its lifecycle hook); we just need to apply any relevant
        // components from the held stack onto it — sign text, banner
        // patterns, custom name, dye colour, …. Mirrors MC's
        // BlockItem.updateCustomBlockEntityTag + BlockEntity.applyImplicitComponents
        // (BlockItem.java:118-160).
        if (Game::BlockEntityTypes::HasBlockEntity(blockToPlace)) {
            const auto chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(
                targetPos.x, targetPos.z);
            if (auto chunk = world->GetChunk(chunkPos.x, chunkPos.z)) {
                const int lx = targetPos.x - chunkPos.x * 16;
                const int lz = targetPos.z - chunkPos.z * 16;
                if (auto* be = chunk->GetBlockEntity(lx, targetPos.y, lz)) {
                    be->ApplyItemComponents(heldStack.components);

                    // NOTE: chest facing is NOT set here any more. In vanilla,
                    // orientation is a blockstate property and the block entity
                    // carries none — ChestBlockEntity's whole field set is
                    // items + openersCounter + chestLidController
                    // (ChestBlockEntity.java:33-35), and ChestRenderer reads the
                    // angle off the BLOCK (ChestRenderer.java:67:
                    // blockState.getValue(ChestBlock.FACING).toYRot()). Chest is
                    // in the placement table like every other oriented block, so
                    // its facing rode in with the SetBlock above.

                    // The BE was just CREATED with default state and the
                    // initial BlockEntityDataS2C went out alongside the block
                    // change. ApplyItemComponents may have mutated it — mark
                    // dirty + re-broadcast so clients see the updated contents.
                    be->MarkDirty();
                    if (m_connection) {
                        Network::BlockEntityDataS2CPacket pkt(
                            targetPos.x, targetPos.y, targetPos.z,
                            be->GetType()->TypeId());
                        Network::PacketBuffer scratch;
                        be->Save(scratch);
                        pkt.dataBlob = scratch.GetData();
                        auto data = Network::Serialization::Serialize(pkt);
                        // Broadcast via the integrated server (every watcher
                        // gets the updated facing, not just the placer).
                        if (Server::g_integratedServer && Server::g_integratedServer->GetNetworkServer()) {
                            Server::g_integratedServer->GetNetworkServer()->BroadcastPacket(
                                static_cast<uint8_t>(Network::PacketId::BlockEntityDataS2C),
                                data);
                        }
                    }
                }
            }
        }

        // TODO: Run block hooks when BlockRegistry is fully implemented
        // if (blockToPl) {
        //     // Call onPlace hook
        //     blockToPl->onPlace(world, targetPos, m_player);
        //
        //     // TODO: Call setPlacedBy for orientation
        // }
        
        // TODO: Schedule systems
        // - Redstone neighbor updates
        // - Fluid ticks (water/lava) for flow and waterlogging
        // - Gravity blocks (sand) if implemented
        
        // === 11. Inventory update ===
        // Decrement the selected hotbar slot and broadcast the new count.
        // Without this the server's inventory keeps the original 64-stack while
        // the client's local prediction decrements toward 0; clicking the slot
        // in the inventory then "refills" it to whatever the server still has.
        // Creative keeps infinite stacks — MC's ItemStack.consume no-ops when
        // hasInfiniteMaterials() (the client mirrors this in its prediction).
        if (m_player->getGameMode() != GameMode::CREATIVE) {
            auto& inv = m_player->getInventory();
            int selUnified = Game::Inventory::HOTBAR_BEGIN + inv.GetSelectedSlot();
            auto& slot = inv.MutableSlot(selUnified);
            if (!slot.IsEmpty()) {
                slot.count--;
                if (slot.count <= 0) slot.Clear();
                // Mutate only — this tick's BroadcastContainerChanges sends the
                // delta with a correct stateId and updates m_remoteSlots.
            }
        }
        
        // === 12. Accumulate outbound notifications ===

        // TODO: Play sound effects
        // TODO: Send particle effects
        
        // === 13. Success acknowledgment ===
        
        AckInteraction(packet.sequence, true);
        m_lastInteractionSequence = packet.sequence;
        
        Log::Debug("HandleUseItemOn: Successfully placed %d at (%d,%d,%d)",
                  static_cast<int>(blockToPlace), targetPos.x, targetPos.y, targetPos.z);
    }
    
    void PlayerSession::HandleUseItem(const Network::UseItemC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handleUseItem
        // (ServerGamePacketListenerImpl.java:1329-1354).
        ASSERT_SERVER_THREAD();

        if (!m_player) {
            Log::Warning("HandleUseItem: No player attached to session");
            return;
        }
        if (m_state != State::PLAYING) {
            Log::Warning("HandleUseItem: Not in PLAYING state");
            AckInteraction(packet.sequence, false);
            return;
        }
        // Stale-sequence guard — same shared counter as dig/useOn.
        if (packet.sequence <= m_lastInteractionSequence) {
            Log::Debug("HandleUseItem: Stale sequence %u <= %u",
                       packet.sequence, m_lastInteractionSequence);
            return;
        }

        // :1332 ackBlockChangesUpTo(sequence)
        AckInteraction(packet.sequence, true);
        m_lastInteractionSequence = packet.sequence;

        // :1335 — the stack in the used hand. :1336 resetLastActionTime — no
        // idle-kick system. :1337 isItemEnabled feature-flag check omitted.
        const Game::ItemStack& stack = m_player->getItemInHand(packet.hand);
        if (stack.IsEmpty()) return;

        // :1338-1342 — wrap the client-reported rotation and snap the player
        // to it, so the use action happens with the exact aim the client had.
        auto wrapDegrees = [](float deg) {
            float d = std::fmod(deg + 180.0f, 360.0f);
            if (d < 0.0f) d += 360.0f;
            return d - 180.0f;
        };
        const float yRot = wrapDegrees(packet.yRot);
        const float xRot = wrapDegrees(packet.xRot);
        if (xRot != m_player->getPitch() || yRot != m_player->getYaw()) {
            m_player->setRotation(yRot, xRot);   // absSnapRotationTo
        }

        // :1344 — the game-mode useItem logic.
        DispatchUseItem(packet.hand);

        // :1345-1350 swing-on-SERVER-source — the local client swings
        // predictively; remote-player swing broadcast is a viewmodel-only
        // concern we don't replicate yet.
    }

    Game::UseResult PlayerSession::DispatchUseItem(uint32_t hand) {
        // Mirrors ServerPlayerGameMode.useItem (ServerPlayerGameMode.java:290-327).
        ASSERT_SERVER_THREAD();
        if (!m_player) return Game::UseResult::Pass;

        IntegratedServer* server = g_integratedServer.get();
        Game::World* world = server ? server->GetWorld() : nullptr;

        // :291-292 spectator → PASS. (No spectator interaction support — same
        // fallthrough note as HandleUseItemOn's dispatch.)
        // :293-294 cooldown check omitted — no cooldown system (excluded).

        Game::ItemStack& stack = m_player->getItemInHand(hand);
        const int oldCount = stack.count;                       // :296
        // :297 oldDamage — durability excluded.

        const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
        const Game::UseResult result =
            item.use ? item.use(world, m_player, hand, stack)
                     : Game::Item_DefaultUse(world, m_player, hand, stack);

        // :299-305 — resultStack. Our callbacks mutate the hand stack in
        // place (BlockInteraction.hpp:44-51 documents the equivalence with
        // MC's heldItemTransformedTo), so resultStack IS the hand slot.
        Game::ItemStack& resultStack = m_player->getItemInHand(hand);

        // :307 — nothing observable changed and the item has no use duration
        // → done, no resync needed.
        if (resultStack.count == oldCount
            && Game::GetUseDuration(resultStack) <= 0) {
            return result;
        }
        // :309-310 — an aborted consumable start (FAIL from e.g. "not hungry")
        // must not trigger a resync either.
        if (result == Game::UseResult::Fail
            && Game::GetUseDuration(resultStack) > 0
            && !m_player->isUsingItem()) {
            return result;
        }
        // :316-318 — fully consumed → make sure the slot reads as empty.
        if (resultStack.IsEmpty()) {
            resultStack.Clear();
            m_player->markSlotDirty(m_player->handSlotIndex(hand));
        }
        // :320-322 — if we are NOT in a hold-to-use (instant action: equip
        // swap, instant consume), push the authoritative inventory now
        // (MC: inventoryMenu.sendAllDataToRemote()).
        if (!m_player->isUsingItem()) {
            SendInventoryFull();
        }
        return result;
    }

    void PlayerSession::HandlePlayerAction(const Network::PlayerActionC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handlePlayerAction
        // (ServerGamePacketListenerImpl.java:1191-1248).
        ASSERT_SERVER_THREAD();
        if (!m_player) return;

        switch (packet.action) {
            case Network::PlayerAction::RELEASE_USE_ITEM:
                // :1235-1237 — stop the hold-to-use early (bow fires here in
                // MC via releaseUsing; food simply doesn't finish).
                m_player->releaseUsingItem();
                return;

            case Network::PlayerAction::SWAP_ITEM_WITH_OFFHAND: {
                // :1214-1220 — swap main-hand (selected hotbar) and offhand
                // stacks, then stopUsingItem.
                auto& inv = m_player->getInventory();
                const int mainIdx = m_player->handSlotIndex(0);
                const int offIdx  = m_player->handSlotIndex(1);
                Game::ItemStack tmp = inv.GetSlot(offIdx);
                inv.SetSlotFull(offIdx, inv.GetSlot(mainIdx));
                inv.SetSlotFull(mainIdx, tmp);
                m_player->stopUsingItem();          // :1218
                m_player->markSlotDirty(mainIdx);
                m_player->markSlotDirty(offIdx);
                return;
            }

            case Network::PlayerAction::DROP_ITEM:
            case Network::PlayerAction::DROP_ALL_ITEMS: {
                // :1223-1233 — MC: player.drop(all) spawns an ItemEntity in
                // the world. We have no item entities, so the stack shrinks /
                // clears and the items are gone (documented gap).
                Game::ItemStack& held = m_player->getItemInHand(0);
                if (held.IsEmpty()) return;
                if (packet.action == Network::PlayerAction::DROP_ALL_ITEMS) {
                    held.Clear();
                } else {
                    held.count--;
                    if (held.count <= 0) held.Clear();
                }
                m_player->markSlotDirty(m_player->handSlotIndex(0));
                Log::Debug("[PlayerSession] Dropped item(s) — no item-entity "
                           "system, stack shrunk only");
                return;
            }

            case Network::PlayerAction::START_DESTROY_BLOCK:
            case Network::PlayerAction::ABORT_DESTROY_BLOCK:
            case Network::PlayerAction::STOP_DESTROY_BLOCK:
                // Dig still rides BlockActionC2S (HandleBlockAction) —
                // migrating it onto PlayerAction is a separate cleanup.
                Log::Debug("[PlayerSession] PlayerAction dig stage %u ignored "
                           "(dig uses BlockActionC2S)",
                           static_cast<unsigned>(packet.action));
                return;

            case Network::PlayerAction::STAB:
                // No combat system.
                return;

            case Network::PlayerAction::PERFORM_RESPAWN: {
                // MC ServerGamePacketListenerImpl.handleClientCommand
                // PERFORM_RESPAWN → PlayerList.respawn. Only honored while
                // actually dead.
                if (!m_player->isDead()) return;

                if (auto* server = g_integratedServer.get()) {
                    if (auto* sessions = server->GetSessionManager()) {
                        // Resets health/food/position via ServerPlayer::respawn.
                        sessions->OnPlayerRespawn(m_playerId);
                    }
                }

                if (m_connection) {
                    // Authoritative snap to the spawn point (same teleport-id
                    // channel as /tp, so stale death-position moves get
                    // dropped), then refresh inventory + abilities. The next
                    // session tick's SetHealthS2C (health back to 20) closes
                    // the client's death screen.
                    const glm::dvec3 pos = m_player->getPosition();
                    m_connection->Teleport(pos.x, pos.y, pos.z,
                                           m_player->getYaw(), m_player->getPitch());
                    SendInventoryFull();
                    m_connection->SendPlayerAbilities(*m_player);
                }
                return;
            }
        }
    }

    void PlayerSession::ResyncAndAck(const glm::ivec3& clicked, const glm::ivec3& target, uint32_t sequence) {
        // Send authoritative block states back to client to resync
        if (m_connection) {
            // Get world
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            if (server && server->GetWorld()) {
                Game::World* world = server->GetWorld();

                // Send clicked block, WITH its state index. Dropping the state
                // here is what made a full leaf litter clump spin north every
                // time a right-click on it was rejected: the resync told the
                // client "leaf_litter, default state", overwriting the facing
                // the client had rendered correctly all along. Same applied to
                // every furnace, chest and log that ever got resynced.
                SendBlockUpdate(clicked,
                                world->GetBlock(clicked.x, clicked.y, clicked.z),
                                world->GetBlockState(clicked.x, clicked.y, clicked.z));

                // Send target block if different
                if (clicked != target) {
                    SendBlockUpdate(target,
                                    world->GetBlock(target.x, target.y, target.z),
                                    world->GetBlockState(target.x, target.y, target.z));
                }
            }

            // Also resync the held hotbar slot. The client predictively
            // decrements its inventory the instant the player right-clicks
            // (so the HUD count drops without a network round trip). When
            // the server rejects the placement we need to push the true
            // count back, otherwise the client's count keeps ticking down
            // toward 0 even though nothing was consumed.
            //
            // This is the one path where a plain diff is not enough: the
            // server's slot never changed (that's the point — the placement was
            // rejected), so it still matches m_remoteSlots and
            // BroadcastContainerChanges would send nothing. Invalidate the
            // model entry first to force the resend.
            if (m_player) {
                const int sel = Game::Inventory::HOTBAR_BEGIN
                              + m_player->getInventory().GetSelectedSlot();
                // Player-inventory index → menu index: they differ whenever a
                // block container is on top.
                InvalidateRemoteInventorySlot(sel);
                BroadcastContainerChanges();
            }
        }

        // Send failure acknowledgment
        AckInteraction(sequence, false);
    }

    void PlayerSession::HandleKeepAlive(const Network::KeepAliveC2SPacket& packet) {
        m_lastKeepAliveRx = std::chrono::steady_clock::now();
        
        // Calculate latency
        auto roundTrip = std::chrono::duration<float, std::milli>(
            m_lastKeepAliveRx - m_lastKeepAliveTx).count();
        
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.latency = roundTrip / 2.0f;
            m_stats.lastKeepAlive = m_lastKeepAliveRx;
        }
    }

    // === SEND METHODS ===
    
    void PlayerSession::SendPositionSync() {
        if (!m_player || !m_connection) return;

        Network::PlayerUpdateS2CPacket packet;
        packet.playerId = m_player->getPlayerId();
        packet.position = glm::vec3(m_player->getPosition()); // dvec3 -> vec3
        packet.rotation = m_player->getRotation();
        packet.sequenceNumber = 0; // not used for broadcast

        auto data = Network::Serialization::Serialize(packet);
        m_connection->SendPacket(
            static_cast<uint8_t>(Network::PacketId::PlayerUpdateS2C), data);
    }
    
    void PlayerSession::SendBlockUpdate(const glm::ivec3& pos, Game::BlockID block,
                                        uint8_t stateIndex) {
        if (!m_connection) return;

        Network::BlockChangeS2CPacket packet(pos.x, pos.y, pos.z, block, stateIndex);
        SendSingleBlockChange(packet);
    }
    
    void PlayerSession::SendSingleBlockChange(const Network::BlockChangeS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server)
        if (g_integratedServer) {
            g_integratedServer->SendBlockChangeS2CPacket(packet);
        }
        // TODO: Add network connection support for multiplayer
    }
    
    void PlayerSession::SendSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server)
        if (g_integratedServer) {
            g_integratedServer->SendSectionBlocksUpdateS2CPacket(packet);
        }
        // TODO: Add network connection support for multiplayer
    }
    
    void PlayerSession::SendInventoryUpdate(int slot) {
        // TODO: Implement when inventory system exists
        // if (!m_player || !m_connection) return;
        // Network::SetSlotS2CPacket packet;
        // packet.windowId = 0; // Player inventory
        // packet.slot = slot;
        // packet.item = m_player->getInventory().getSlot(slot);
        // m_connection->SendPacket(packet);
    }
    
    void PlayerSession::AckInteraction(uint32_t sequence, bool success) {
        m_lastInteractionSequence = sequence;

        // The ack itself carries no success flag — MC's
        // ClientboundBlockChangedAckPacket is a bare sequence. "Failure" is
        // communicated by the corrective block updates the reject paths
        // already send (ResyncAndAck), which the client applies when the
        // prediction retires. Keeping the flag in this signature documents
        // intent at the call sites and drives the debug log.
        AckBlockChangesUpTo(sequence);

        Log::Debug("PlayerSession: Acknowledged interaction seq=%u success=%s",
                  sequence, success ? "true" : "false");
    }

    void PlayerSession::AckBlockChangesUpTo(uint32_t sequence) {
        // MC takes the max so a tick that handled several interactions emits
        // one ack covering all of them.
        if (sequence > m_ackBlockChangesUpTo) {
            m_ackBlockChangesUpTo = sequence;
        }
    }

    void PlayerSession::FlushBlockChangeAck() {
        if (m_ackBlockChangesUpTo == 0 || !m_connection) return;

        Network::BlockChangedAckS2CPacket packet(m_ackBlockChangesUpTo);
        auto data = Network::Serialization::Serialize(packet);
        m_connection->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BlockChangedAckS2C), data);
        m_ackBlockChangesUpTo = 0;
    }
    
    void PlayerSession::OnChunkSendComplete(Game::Math::ChunkPos chunk) {
        // Mark as sent
        m_sentChunks.insert(chunk);

        // Process any buffered diffs for this chunk
        auto diffIt = m_pendingDiffs.find(chunk);
        if (diffIt != m_pendingDiffs.end()) {
            for (const auto& [section, diffs] : diffIt->second) {
                if (!diffs.changes.empty()) {
                    m_diffQueue.push({chunk, section});
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksSent++;
        }
    }

    void PlayerSession::OnChunkUnloadComplete(Game::Math::ChunkPos chunk) {
        // Ensure chunk is removed from all sets
        m_watchSet.erase(chunk);
        m_sentChunks.erase(chunk);
        m_pendingChunkLoads.erase(chunk);
        m_pendingChunksToSend.erase(chunk);
    }

    // === STATISTICS ===

    PlayerSession::Stats PlayerSession::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    // === GETTERS ===
    
    glm::vec3 PlayerSession::GetPosition() const {
        if (m_player) {
            return glm::vec3(m_player->getPosition());
        }
        return glm::vec3(0.0f);
    }
    
    glm::vec2 PlayerSession::GetRotation() const {
        if (m_player) {
            return m_player->getRotation();
        }
        return glm::vec2(0.0f);
    }
    
    int PlayerSession::GetDimensionId() const {
        if (m_player) {
            return m_player->getDimensionId();
        }
        return 0;
    }
    
    void PlayerSession::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    // === INTERNAL METHODS ===

    std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>
    PlayerSession::ComputeWatchSet(Game::Math::ChunkPos anchor, int viewDistance) const {
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> watchSet;

        // Add +2 buffer like Minecraft's chunkRadius = viewRange + 3
        // This prevents thrashing at the boundary when moving
        int radius = viewDistance + 2;

        // Use Chebyshev distance (square pattern)
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                watchSet.emplace(anchor.x + dx, anchor.z + dz);
            }
        }
        
        return watchSet;
    }

    void PlayerSession::ComputeWatchDeltas(
        const std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>& newWatch,
        std::vector<Game::Math::ChunkPos>& toAdd,
        std::vector<Game::Math::ChunkPos>& toRemove
    ) const {
        // Find chunks to add (in new but not in current)
        for (const auto& chunk : newWatch) {
            if (m_watchSet.count(chunk) == 0) {
                toAdd.push_back(chunk);
            }
        }
        
        // Find chunks to remove (in current but not in new)
        for (const auto& chunk : m_watchSet) {
            if (newWatch.count(chunk) == 0) {
                toRemove.push_back(chunk);
            }
        }
    }

    void PlayerSession::CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                           uint8_t localX, uint8_t localY, uint8_t localZ,
                                           Game::BlockID blockId, uint8_t stateIndex) {
        auto& sectionDiffs = m_pendingDiffs[chunk][section];
        sectionDiffs.chunkPos = chunk;
        sectionDiffs.sectionIndex = section;
        sectionDiffs.AddChange(localX, localY, localZ, blockId, stateIndex);
    }

    size_t PlayerSession::EstimatePacketSize(const Network::ChunkDataS2CPacket& packet) const {
        return packet.CalculateDataSize() + 32; // Add header overhead
    }

    size_t PlayerSession::EstimatePacketSize(const Network::MultiBlockChangeS2CPacket& packet) const {
        return packet.changes.size() * 8 + 16; // Estimate
    }

    void PlayerSession::ClearWatchSets() {
        m_watchSet.clear();
        m_sentChunks.clear();
        m_pendingChunkLoads.clear();
    }

    void PlayerSession::ClearQueues() {
        m_pendingChunkLoads.clear();
        m_pendingChunksToSend.clear();

        // Clear diff queue
        while (!m_diffQueue.empty()) {
            m_diffQueue.pop();
        }
    }

    void PlayerSession::ClearDiffs() {
        m_pendingDiffs.clear();
    }

} // namespace Server
