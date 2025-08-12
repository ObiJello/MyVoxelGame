// File: src/server/session/PlayerSession.cpp
#include "PlayerSession.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../world/watch/ChunkWatchIndex.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <cmath>

namespace Server {

    PlayerSession::PlayerSession(uint32_t playerId, uint32_t connectionId)
        : m_playerId(playerId)
        , m_connectionId(connectionId)
        , m_dimensionId(0)
        , m_pendingAdd(32)  // 32 distance rings
    {
        m_lastTickTime = std::chrono::steady_clock::now();
        m_lastKeepAliveRx = m_lastTickTime;
        m_lastKeepAliveTx = m_lastTickTime;
    }

    PlayerSession::~PlayerSession() {
        Cleanup();
    }

    // === LIFECYCLE ===

    void PlayerSession::Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos) {
        m_config = config;
        m_dimensionId = dimensionId;
        m_position = spawnPos;
        m_simulationDistance = config.simulationDistance;
        m_viewDistance = std::min(config.viewDistance, m_simulationDistance);
        
        // Calculate initial chunk position
        m_currentChunk = Game::Math::ChunkPos(
            static_cast<int>(std::floor(spawnPos.x / 16.0f)),
            static_cast<int>(std::floor(spawnPos.z / 16.0f))
        );
        m_anchorChunk = m_currentChunk;
        m_lastKnownChunk = m_currentChunk;
        
        // Compute initial watch set
        auto initialWatch = ComputeWatchSet(m_anchorChunk, m_viewDistance);
        
        // Queue all initial chunks for sending
        for (const auto& chunk : initialWatch) {
            int priority = CalculateChunkPriority(chunk);
            m_pendingAdd.Push(chunk, priority);
        }
        
        // Clear any existing state
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        // Set state to joining
        m_state = State::JOINING;
        m_needsWatchUpdate = true;
        
        Log::Info("PlayerSession: Initialized player %u at (%.1f, %.1f, %.1f) in dimension %d",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z, dimensionId);
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
        if (m_state == State::JOINING && !m_pendingAdd.Empty()) {
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
        m_position = position;
        m_rotation = rotation;
        
        // Calculate new chunk position
        Game::Math::ChunkPos newChunk(
            static_cast<int>(std::floor(position.x / 16.0f)),
            static_cast<int>(std::floor(position.z / 16.0f))
        );
        
        // Update chunk position if changed
        if (newChunk != m_currentChunk) {
            UpdateChunkPosition(newChunk);
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
        
        Log::Debug("PlayerSession: Player %u moved to chunk (%d, %d)", 
                  m_playerId, newChunk.x, newChunk.z);
    }

    void PlayerSession::ChangeDimension(int newDimensionId, const glm::vec3& targetPos) {
        if (m_dimensionId == newDimensionId) {
            return;
        }
        
        Log::Info("PlayerSession: Player %u changing dimension from %d to %d", 
                 m_playerId, m_dimensionId, newDimensionId);
        
        m_isChangingDimension = true;
        
        // Send unload for all watched chunks
        for (const auto& chunk : m_watchSet) {
            SendChunkUnload(chunk);
        }
        
        // Clear all watch sets
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        // Update dimension and position
        m_dimensionId = newDimensionId;
        m_position = targetPos;
        m_currentChunk = Game::Math::ChunkPos(
            static_cast<int>(std::floor(targetPos.x / 16.0f)),
            static_cast<int>(std::floor(targetPos.z / 16.0f))
        );
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
        
        // Update position
        m_position = spawnPos;
        m_currentChunk = Game::Math::ChunkPos(
            static_cast<int>(std::floor(spawnPos.x / 16.0f)),
            static_cast<int>(std::floor(spawnPos.z / 16.0f))
        );
        m_anchorChunk = m_currentChunk;
        
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
        
        // Apply removals first
        for (const auto& chunk : toRemove) {
            m_pendingRemove.push_back(chunk);
            m_watchSet.erase(chunk);
        }
        
        // Queue additions with priority
        for (const auto& chunk : toAdd) {
            int priority = CalculateChunkPriority(chunk);
            m_pendingAdd.Push(chunk, priority);
            m_watchSet.insert(chunk);
        }
        
        // Update stats
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksInWatch = m_watchSet.size();
            m_stats.chunksPending = m_pendingAdd.Size();
        }
    }

    bool PlayerSession::IsWatching(Game::Math::ChunkPos chunk) const {
        return m_watchSet.count(chunk) > 0;
    }

    bool PlayerSession::HasSentChunk(Game::Math::ChunkPos chunk) const {
        return m_sentChunks.count(chunk) > 0;
    }

    // === CHUNK STREAMING ===

    void PlayerSession::QueueChunkSend(Game::Math::ChunkPos chunk, int priority) {
        if (HasSentChunk(chunk) || m_inflightChunks.count(chunk) > 0) {
            return; // Already sent or in flight
        }
        
        m_pendingAdd.Push(chunk, priority);
    }

    void PlayerSession::ProcessChunkSends(ChunkStatusManager* statusMgr, SendScheduler* scheduler) {
        // Process removals first (up to N per tick)
        size_t maxRemovals = 16;  // Increased from 10 for faster unloading
        while (!m_pendingRemove.empty() && maxRemovals > 0) {
            auto chunk = m_pendingRemove.back();
            m_pendingRemove.pop_back();
            
            SendChunkUnload(chunk);
            maxRemovals--;
        }
        
        // Process additions within budget
        while (m_chunksOutThisTick < static_cast<size_t>(m_config.maxChunksPerTick) &&
               m_bytesOutThisTick < static_cast<size_t>(m_config.maxBytesPerTick) &&
               !m_pendingAdd.Empty()) {
            
            Game::Math::ChunkPos chunk;
            if (!m_pendingAdd.Pop(chunk)) {
                break;
            }
            
            // Skip if already sent or in flight
            if (HasSentChunk(chunk) || m_inflightChunks.count(chunk) > 0) {
                continue;
            }
            
            // TODO: Check chunk status with ChunkStatusManager
            // For now, assume chunk is ready
            
            // TODO: Build and send chunk data packet
            // This would involve getting chunk data, building packet, and sending via scheduler
            
            // Mark as in flight
            m_inflightChunks.insert(chunk);
            m_chunksOutThisTick++;
            
            // TODO: Track bytes sent
            m_bytesOutThisTick += 65536; // Estimate for now
        }
    }

    void PlayerSession::SendChunkUnload(Game::Math::ChunkPos chunk) {
        // TODO: Send UnloadChunkS2CPacket via SendScheduler
        
        // Remove from sets
        m_watchSet.erase(chunk);
        m_sentChunks.erase(chunk);
        m_inflightChunks.erase(chunk);
        
        // Clear any pending diffs for this chunk
        m_pendingDiffs.erase(chunk);
        
        Log::Debug("PlayerSession: Sent chunk unload for (%d, %d) to player %u", 
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
        
        // Calculate section and local position
        int sectionY = (worldY + 64) >> 4;  // Assuming minY = -64
        uint8_t localX = worldX & 0xF;
        uint8_t localY = worldY & 0xF;
        uint8_t localZ = worldZ & 0xF;
        
        // Coalesce the change
        CoalesceBlockChange(chunk, sectionY, localX, localY, localZ, newBlock);
        
        // Add to diff queue if chunk has been sent
        if (HasSentChunk(chunk)) {
            m_diffQueue.push({chunk, sectionY});
        }
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

    void PlayerSession::ProcessDiffs(SendScheduler* scheduler) {
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
            
            // TODO: Build and send MultiBlockChangeS2CPacket via scheduler
            // Estimate diff packet size (each block change is roughly 12 bytes)
            size_t estimatedPacketSize = diffs.changes.size() * 12 + 16; // +16 for packet header
            diffBytesThisTick += estimatedPacketSize;
            
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
        
        // TODO: Validate movement (anti-cheat)
        // TODO: Update last move sequence number
    }

    void PlayerSession::HandleBlockAction(const Network::BlockActionC2SPacket& packet) {
        // TODO: Validate block action
        // TODO: Apply block change to world
        // TODO: Queue diff for other watching players
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

    void PlayerSession::OnChunkSendComplete(Game::Math::ChunkPos chunk) {
        // Move from inflight to sent
        m_inflightChunks.erase(chunk);
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
            m_stats.chunksInFlight = m_inflightChunks.size();
        }
    }

    void PlayerSession::OnChunkUnloadComplete(Game::Math::ChunkPos chunk) {
        // Ensure chunk is removed from all sets
        m_watchSet.erase(chunk);
        m_sentChunks.erase(chunk);
        m_inflightChunks.erase(chunk);
    }

    // === STATISTICS ===

    PlayerSession::Stats PlayerSession::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    void PlayerSession::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    // === INTERNAL METHODS ===

    std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>
    PlayerSession::ComputeWatchSet(Game::Math::ChunkPos anchor, int viewDistance) const {
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> watchSet;
        
        // Use Chebyshev distance (square pattern)
        for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
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

    int PlayerSession::CalculateChunkPriority(Game::Math::ChunkPos chunk) const {
        // Calculate Chebyshev distance from anchor
        int dx = std::abs(chunk.x - m_anchorChunk.x);
        int dz = std::abs(chunk.z - m_anchorChunk.z);
        return std::max(dx, dz);
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
        m_inflightChunks.clear();
    }

    void PlayerSession::ClearQueues() {
        m_pendingAdd.Clear();
        m_pendingRemove.clear();
        
        // Clear diff queue
        while (!m_diffQueue.empty()) {
            m_diffQueue.pop();
        }
    }

    void PlayerSession::ClearDiffs() {
        m_pendingDiffs.clear();
    }

} // namespace Server