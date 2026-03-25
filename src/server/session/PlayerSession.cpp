// File: src/server/session/PlayerSession.cpp
#include "PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../network/SendScheduler.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../world/watch/ChunkWatchIndex.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include "../IntegratedServer.hpp"
#include <algorithm>
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

                packet.sections.push_back(std::move(sectionData));
            }

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
            CoalesceBlockChange(chunk, section, change.localX, change.localY, change.localZ, change.blockId);
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
                
                Network::BlockChangeS2CPacket packet(worldX, worldY, worldZ, blockId);
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
                    packet.AddChange(localX, localY, localZ, static_cast<uint16_t>(blockId));
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
        UpdatePosition(packet.position, packet.rotation);
        if (m_player) {
            m_player->setSneaking(packet.isCrouching);
        }
    }

    void PlayerSession::HandleBlockAction(const Network::BlockActionC2SPacket& packet) {
        if (!m_player) return;

        switch (packet.action) {
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

                Game::BlockID oldBlock = world->GetBlock(pos.x, pos.y, pos.z);
                if (oldBlock == Game::BlockID::Air || oldBlock == Game::BlockID::Bedrock) return;

                if (world->SetBlock(pos.x, pos.y, pos.z, Game::BlockID::Air)) {
                    Log::Debug("HandleBlockAction: Player %u broke block at (%d,%d,%d)",
                              m_playerId, pos.x, pos.y, pos.z);
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
            m_player->selectHotbarSlot(slot);
            // Also sync the block type for this slot (client-authoritative for now)
            if (packet.blockId != 0) {
                m_player->setHotbarBlock(slot, static_cast<Game::BlockID>(packet.blockId));
            }
            Log::Debug("[PlayerSession] Player %u: slot %d, block %d", m_playerId, slot, packet.blockId);
        }
    }

    void PlayerSession::HandleUseItemOn(const Network::UseItemOnC2SPacket& packet) {
        // === 1. Thread safety & basic validation ===
        ASSERT_SERVER_THREAD();
        
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
        
        // === 4. Reach validation ===
        
        // Reconstruct ray from player eye to hit point
        glm::dvec3 playerPos = m_player->getPosition();
        glm::vec3 eyePos = glm::vec3(playerPos.x, playerPos.y + 1.62, playerPos.z); // Eye height
        float distance = glm::length(hitPoint - eyePos);
        float maxReach = m_player->getGameMode() == GameMode::CREATIVE ? 5.0f : 4.5f;
        
        if (distance > maxReach) {
            Log::Warning("HandleUseItemOn: Out of reach %.2f > %.2f", distance, maxReach);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // === 5. "Use then place" semantics (Minecraft rule) ===
        
        // TODO: Implement block use logic when BlockRegistry is fully implemented
        // If player is not sneaking, try to use the block first
        // if (!m_player->IsSneaking()) {
        //     Game::BlockID clickedBlockId = world->GetBlock(clicked.x, clicked.y, clicked.z);
        //     Game::Block* clickedBlock = Game::BlockRegistry::getInstance().getBlock(clickedBlockId);
        //     
        //     if (clickedBlock) {
        //         Game::UseResult useResult = clickedBlock->use(world, clicked, m_player, packet.hand, hit);
        //         
        //         if (useResult == Game::UseResult::Success || useResult == Game::UseResult::Consume) {
        //             // Block was used (chest opened, lever flipped, etc.)
        //             // TODO: Play use effects
        //             // TODO: Open container if needed
        //             AckInteraction(packet.sequence, true);
        //             m_lastInteractionSequence = packet.sequence;
        //             return;
        //         }
        //     }
        // }
        
        // === 6. Build the candidate block state (from the held item) ===
        
        // Get block to place from player's hand
        Game::BlockID blockToPlace = m_player->getHeldBlock();
        
        // If holding air or no block, can't place
        if (blockToPlace == Game::BlockID::Air) {
            AckInteraction(packet.sequence, false);
            m_lastInteractionSequence = packet.sequence;
            return;
        }
        
        // Calculate target position (where to place the block)
        glm::ivec3 targetPos = clicked;
        
        // Check if clicked block is replaceable
        Game::BlockID clickedBlockId = world->GetBlock(clicked.x, clicked.y, clicked.z);
        // TODO: Check if block is replaceable when BlockRegistry is fully implemented
        // For now, only air is replaceable
        bool isReplaceable = (clickedBlockId == Game::BlockID::Air);
        
        if (!isReplaceable) {
            // Place at offset position
            targetPos = context.getPlacementPos();
        }
        // If replaceable (snow, tall grass, etc.), place at clicked position
        
        // Validate target position
        if (!world->IsValidPosition(targetPos.x, targetPos.y, targetPos.z)) {
            Log::Warning("HandleUseItemOn: Target position invalid (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }
        
        // === 7. Validate placement ===
        
        // TODO: Check if block can survive at target position when BlockRegistry is fully implemented
        // For now, assume all blocks can be placed anywhere
        // Game::Block* blockToPl = Game::BlockRegistry::getInstance().getBlock(blockToPlace);
        // if (blockToPl && !blockToPl->canSurvive(world, targetPos)) {
        //     Log::Debug("HandleUseItemOn: Block cannot survive at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
        //     ResyncAndAck(clicked, targetPos, packet.sequence);
        //     return;
        // }
        
        // === 8. Collision check with entities ===
        
        // Check if any players are in the target block space
        // TODO: Get all players from PlayerSessionManager
        glm::dvec3 playerPosDouble = m_player->getPosition();
        glm::vec3 playerCollisionPos = glm::vec3(playerPosDouble.x, playerPosDouble.y, playerPosDouble.z);
        glm::vec3 blockCenter = glm::vec3(targetPos) + glm::vec3(0.5f, 0.5f, 0.5f);
        
        // Simple AABB check - player is 0.6x1.8x0.6, block is 1x1x1
        bool playerCollides = false;
        if (std::abs(playerCollisionPos.x - blockCenter.x) < 0.8f &&
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
        
        bool changed = world->SetBlock(
            targetPos.x, targetPos.y, targetPos.z,
            blockToPlace,
            Game::World::UpdateFlags::All
        );
        
        if (!changed) {
            Log::Warning("HandleUseItemOn: SetBlock failed at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }
        
        // === 10. Run block hooks ===
        
        // TODO: Run block hooks when BlockRegistry is fully implemented
        // if (blockToPl) {
        //     // Call onPlace hook
        //     blockToPl->onPlace(world, targetPos, m_player);
        //     
        //     // TODO: Call setPlacedBy for orientation
        //     // TODO: Create Block Entity if required
        //     // TODO: Merge NBT from item (BlockEntityTag)
        // }
        
        // TODO: Schedule systems
        // - Redstone neighbor updates
        // - Fluid ticks (water/lava) for flow and waterlogging
        // - Gravity blocks (sand) if implemented
        
        // === 11. Inventory update ===
        
        if (m_player->getGameMode() != GameMode::CREATIVE) {
            // TODO: Decrement held stack
            // TODO: Send inventory slot echo back to player
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
    
    void PlayerSession::ResyncAndAck(const glm::ivec3& clicked, const glm::ivec3& target, uint32_t sequence) {
        // Send authoritative block states back to client to resync
        if (m_connection) {
            // Get world
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            if (server && server->GetWorld()) {
                Game::World* world = server->GetWorld();
                
                // Send clicked block
                Game::BlockID clickedBlock = world->GetBlock(clicked.x, clicked.y, clicked.z);
                SendBlockUpdate(clicked, clickedBlock);
                
                // Send target block if different
                if (clicked != target) {
                    Game::BlockID targetBlock = world->GetBlock(target.x, target.y, target.z);
                    SendBlockUpdate(target, targetBlock);
                }
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
    
    void PlayerSession::SendBlockUpdate(const glm::ivec3& pos, Game::BlockID block) {
        if (!m_connection) return;
        
        Network::BlockChangeS2CPacket packet(pos.x, pos.y, pos.z, block);
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
        
        // TODO: Send acknowledgment packet to client
        // Network::AckInteractionS2CPacket packet;
        // packet.sequence = sequence;
        // packet.success = success;
        // m_connection->SendPacket(packet);
        
        Log::Debug("PlayerSession: Acknowledged interaction seq=%u success=%s",
                  sequence, success ? "true" : "false");
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
                                           Game::BlockID blockId) {
        auto& sectionDiffs = m_pendingDiffs[chunk][section];
        sectionDiffs.chunkPos = chunk;
        sectionDiffs.sectionIndex = section;
        sectionDiffs.AddChange(localX, localY, localZ, blockId);
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