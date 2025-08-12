// File: src/client/world/PendingDiffsManager.cpp
#include "PendingDiffsManager.hpp"
#include "common/core/Log.hpp"
#include <algorithm>

namespace Client {

    PendingDiffsManager::PendingDiffsManager() = default;
    PendingDiffsManager::~PendingDiffsManager() = default;

    // ========================================================================
    // ADDING DIFFS
    // ========================================================================

    void PendingDiffsManager::AddBlockChange(Game::Math::ChunkPos chunkPos, 
                                            const Network::BlockChangeS2CPacket& packet) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Check if we need to make room
        if (m_pendingDiffs.size() >= MAX_CHUNKS_WITH_DIFFS && 
            m_pendingDiffs.find(chunkPos) == m_pendingDiffs.end()) {
            DropOldestChunkIfNeeded();
        }
        
        // Get or create chunk diffs
        auto& chunkDiffs = m_pendingDiffs[chunkPos];
        
        // Track insertion order for LRU
        auto orderIt = std::find(m_chunkInsertionOrder.begin(), m_chunkInsertionOrder.end(), chunkPos);
        if (orderIt == m_chunkInsertionOrder.end()) {
            m_chunkInsertionOrder.push_back(chunkPos);
        }
        
        // Check per-chunk limit
        if (chunkDiffs.GetTotalDiffCount() >= MAX_DIFFS_PER_CHUNK) {
            m_stats.droppedDiffs++;
            Log::Warning("[PendingDiffs] Dropping block change for chunk (%d,%d) - per-chunk limit reached",
                        chunkPos.x, chunkPos.z);
            return;
        }
        
        // Create block change
        BlockPos blockPos = WorldToBlockPos(packet.worldX, packet.worldY, packet.worldZ);
        
        // Check if we're replacing an existing change (coalescing)
        auto existingIt = chunkDiffs.blockChanges.find(blockPos);
        if (existingIt != chunkDiffs.blockChanges.end()) {
            m_stats.coalescedChanges++;
            Log::Debug("[PendingDiffs] Coalescing block change at (%d,%d,%d)", 
                      blockPos.x, blockPos.y, blockPos.z);
        }
        
        // Add/update the block change (latest change wins)
        chunkDiffs.blockChanges[blockPos] = BlockChange{
            blockPos,
            packet.newBlockId,
            m_currentGeneration.load(),
            std::chrono::steady_clock::now(),
            packet.playSound,
            packet.updateNeighbors
        };
        
        m_stats.totalBlockChanges = chunkDiffs.blockChanges.size();
        
        // Enforce global limits
        EnforceLimits();
    }

    void PendingDiffsManager::AddMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Check if we need to make room
        if (m_pendingDiffs.size() >= MAX_CHUNKS_WITH_DIFFS && 
            m_pendingDiffs.find(packet.chunkPos) == m_pendingDiffs.end()) {
            DropOldestChunkIfNeeded();
        }
        
        // Get or create chunk diffs
        auto& chunkDiffs = m_pendingDiffs[packet.chunkPos];
        
        // Track insertion order
        auto orderIt = std::find(m_chunkInsertionOrder.begin(), m_chunkInsertionOrder.end(), packet.chunkPos);
        if (orderIt == m_chunkInsertionOrder.end()) {
            m_chunkInsertionOrder.push_back(packet.chunkPos);
        }
        
        // Add each change
        for (const auto& change : packet.changes) {
            // Check per-chunk limit
            if (chunkDiffs.GetTotalDiffCount() >= MAX_DIFFS_PER_CHUNK) {
                m_stats.droppedDiffs++;
                Log::Warning("[PendingDiffs] Dropping remaining changes for chunk (%d,%d) - limit reached",
                            packet.chunkPos.x, packet.chunkPos.z);
                break;
            }
            
            // Calculate world position
            int worldX = packet.chunkPos.x * 16 + change.localX;
            int worldZ = packet.chunkPos.z * 16 + change.localZ;
            BlockPos blockPos = WorldToBlockPos(worldX, change.localY, worldZ);
            
            // Check for coalescing
            if (chunkDiffs.blockChanges.find(blockPos) != chunkDiffs.blockChanges.end()) {
                m_stats.coalescedChanges++;
            }
            
            // Add/update the change
            chunkDiffs.blockChanges[blockPos] = BlockChange{
                blockPos,
                change.blockId,
                m_currentGeneration.load(),
                std::chrono::steady_clock::now(),
                false,  // Don't play sound for bulk changes
                false   // Don't update neighbors for bulk changes
            };
        }
        
        m_stats.totalBlockChanges = chunkDiffs.blockChanges.size();
        
        Log::Debug("[PendingDiffs] Added %zu block changes for chunk (%d,%d)",
                  packet.changes.size(), packet.chunkPos.x, packet.chunkPos.z);
        
        // Enforce global limits
        EnforceLimits();
    }

    void PendingDiffsManager::AddLightUpdate(Game::Math::ChunkPos chunkPos, const BlockPos& pos,
                                            uint8_t skyLight, uint8_t blockLight) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Check if we need to make room
        if (m_pendingDiffs.size() >= MAX_CHUNKS_WITH_DIFFS && 
            m_pendingDiffs.find(chunkPos) == m_pendingDiffs.end()) {
            DropOldestChunkIfNeeded();
        }
        
        auto& chunkDiffs = m_pendingDiffs[chunkPos];
        
        // Check per-chunk limit
        if (chunkDiffs.GetTotalDiffCount() >= MAX_DIFFS_PER_CHUNK) {
            m_stats.droppedDiffs++;
            return;
        }
        
        chunkDiffs.lightUpdates.push_back(LightUpdate{
            pos,
            skyLight,
            blockLight,
            m_currentGeneration.load(),
            std::chrono::steady_clock::now()
        });
        
        m_stats.totalLightUpdates++;
        EnforceLimits();
    }

    void PendingDiffsManager::AddBlockEntityUpdate(Game::Math::ChunkPos chunkPos, const BlockPos& pos,
                                                  const std::vector<uint8_t>& nbtData) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Check if we need to make room
        if (m_pendingDiffs.size() >= MAX_CHUNKS_WITH_DIFFS && 
            m_pendingDiffs.find(chunkPos) == m_pendingDiffs.end()) {
            DropOldestChunkIfNeeded();
        }
        
        auto& chunkDiffs = m_pendingDiffs[chunkPos];
        
        // Check per-chunk limit
        if (chunkDiffs.GetTotalDiffCount() >= MAX_DIFFS_PER_CHUNK) {
            m_stats.droppedDiffs++;
            return;
        }
        
        chunkDiffs.blockEntityUpdates[pos] = BlockEntityUpdate{
            pos,
            nbtData,
            m_currentGeneration.load(),
            std::chrono::steady_clock::now()
        };
        
        m_stats.totalBlockEntityUpdates++;
        EnforceLimits();
    }

    // ========================================================================
    // APPLYING DIFFS
    // ========================================================================

    size_t PendingDiffsManager::ApplyPendingDiffs(Game::Math::ChunkPos chunkPos, uint32_t chunkGeneration) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_pendingDiffs.find(chunkPos);
        if (it == m_pendingDiffs.end()) {
            return 0;  // No pending diffs for this chunk
        }
        
        ChunkDiffs& diffs = it->second;
        size_t appliedCount = 0;
        
        // Check generation to drop stale diffs
        if (diffs.generation < chunkGeneration) {
            Log::Debug("[PendingDiffs] Applying %zu diffs for chunk (%d,%d)",
                      diffs.GetTotalDiffCount(), chunkPos.x, chunkPos.z);
            
            // Note: The actual application happens in ClientChunkManager
            // This just counts and prepares the diffs
            appliedCount = diffs.GetTotalDiffCount();
            m_stats.appliedDiffs += appliedCount;
        } else {
            Log::Warning("[PendingDiffs] Dropping stale diffs for chunk (%d,%d) - generation mismatch",
                        chunkPos.x, chunkPos.z);
            m_stats.droppedDiffs += diffs.GetTotalDiffCount();
        }
        
        // Remove from insertion order
        auto orderIt = std::find(m_chunkInsertionOrder.begin(), m_chunkInsertionOrder.end(), chunkPos);
        if (orderIt != m_chunkInsertionOrder.end()) {
            m_chunkInsertionOrder.erase(orderIt);
        }
        
        // Remove the diffs after applying
        m_pendingDiffs.erase(it);
        
        return appliedCount;
    }

    const PendingDiffsManager::ChunkDiffs* PendingDiffsManager::GetPendingDiffs(Game::Math::ChunkPos chunkPos) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_pendingDiffs.find(chunkPos);
        if (it != m_pendingDiffs.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // ========================================================================
    // CLEANUP
    // ========================================================================

    void PendingDiffsManager::DropChunkDiffs(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_pendingDiffs.find(chunkPos);
        if (it != m_pendingDiffs.end()) {
            size_t droppedCount = it->second.GetTotalDiffCount();
            m_stats.droppedDiffs += droppedCount;
            
            Log::Debug("[PendingDiffs] Dropping %zu diffs for unloaded chunk (%d,%d)",
                      droppedCount, chunkPos.x, chunkPos.z);
            
            // Remove from insertion order
            auto orderIt = std::find(m_chunkInsertionOrder.begin(), m_chunkInsertionOrder.end(), chunkPos);
            if (orderIt != m_chunkInsertionOrder.end()) {
                m_chunkInsertionOrder.erase(orderIt);
            }
            
            m_pendingDiffs.erase(it);
        }
    }

    void PendingDiffsManager::DropStaleDiffs(std::chrono::seconds maxAge) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto now = std::chrono::steady_clock::now();
        std::vector<Game::Math::ChunkPos> toRemove;
        
        for (const auto& [chunkPos, diffs] : m_pendingDiffs) {
            bool allStale = true;
            
            // Check if all diffs are older than maxAge
            for (const auto& [pos, change] : diffs.blockChanges) {
                if (now - change.timestamp < maxAge) {
                    allStale = false;
                    break;
                }
            }
            
            if (allStale) {
                toRemove.push_back(chunkPos);
            }
        }
        
        // Remove stale chunks
        for (const auto& chunkPos : toRemove) {
            auto it = m_pendingDiffs.find(chunkPos);
            if (it != m_pendingDiffs.end()) {
                m_stats.droppedDiffs += it->second.GetTotalDiffCount();
                
                // Remove from insertion order
                auto orderIt = std::find(m_chunkInsertionOrder.begin(), 
                                        m_chunkInsertionOrder.end(), chunkPos);
                if (orderIt != m_chunkInsertionOrder.end()) {
                    m_chunkInsertionOrder.erase(orderIt);
                }
                
                m_pendingDiffs.erase(it);
            }
        }
        
        if (!toRemove.empty()) {
            Log::Info("[PendingDiffs] Dropped stale diffs for %zu chunks", toRemove.size());
        }
    }

    void PendingDiffsManager::ClearAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        size_t totalDiffs = 0;
        for (const auto& [chunkPos, diffs] : m_pendingDiffs) {
            totalDiffs += diffs.GetTotalDiffCount();
        }
        
        Log::Info("[PendingDiffs] Clearing %zu diffs across %zu chunks",
                 totalDiffs, m_pendingDiffs.size());
        
        m_pendingDiffs.clear();
        m_chunkInsertionOrder.clear();
        m_stats = Stats{};  // Reset stats
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================

    PendingDiffsManager::Stats PendingDiffsManager::GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        Stats stats = m_stats;
        stats.chunksWithDiffs = m_pendingDiffs.size();
        
        // Recalculate totals
        stats.totalBlockChanges = 0;
        stats.totalLightUpdates = 0;
        stats.totalBlockEntityUpdates = 0;
        
        for (const auto& [chunkPos, diffs] : m_pendingDiffs) {
            stats.totalBlockChanges += diffs.blockChanges.size();
            stats.totalLightUpdates += diffs.lightUpdates.size();
            stats.totalBlockEntityUpdates += diffs.blockEntityUpdates.size();
        }
        
        return stats;
    }

    void PendingDiffsManager::ResetStats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats = Stats{};
    }

    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    void PendingDiffsManager::EnforceLimits() {
        // Calculate total diffs
        size_t totalDiffs = 0;
        for (const auto& [chunkPos, diffs] : m_pendingDiffs) {
            totalDiffs += diffs.GetTotalDiffCount();
        }
        
        // Drop oldest chunks if over global limit
        while (totalDiffs > MAX_TOTAL_DIFFS && !m_chunkInsertionOrder.empty()) {
            DropOldestChunkIfNeeded();
            
            // Recalculate
            totalDiffs = 0;
            for (const auto& [chunkPos, diffs] : m_pendingDiffs) {
                totalDiffs += diffs.GetTotalDiffCount();
            }
        }
    }

    void PendingDiffsManager::DropOldestChunkIfNeeded() {
        if (m_chunkInsertionOrder.empty()) {
            return;
        }
        
        // Get oldest chunk
        Game::Math::ChunkPos oldestChunk = m_chunkInsertionOrder.front();
        m_chunkInsertionOrder.erase(m_chunkInsertionOrder.begin());
        
        // Drop its diffs
        auto it = m_pendingDiffs.find(oldestChunk);
        if (it != m_pendingDiffs.end()) {
            size_t droppedCount = it->second.GetTotalDiffCount();
            m_stats.droppedDiffs += droppedCount;
            
            Log::Debug("[PendingDiffs] Evicting %zu diffs for oldest chunk (%d,%d)",
                      droppedCount, oldestChunk.x, oldestChunk.z);
            
            m_pendingDiffs.erase(it);
        }
    }

} // namespace Client