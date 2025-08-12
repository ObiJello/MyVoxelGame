// File: src/server/world/watch/ChunkWatchIndex.cpp
#include "ChunkWatchIndex.hpp"
#include <algorithm>
#include <cmath>

namespace Server {

    ChunkWatchIndex::ChunkWatchIndex() {
        // Pre-create default dimension
        m_dimensions[DEFAULT_DIMENSION] = DimensionWatchIndex{};
    }

    ChunkWatchIndex::~ChunkWatchIndex() {
        Clear();
    }

    // === WATCHER MANAGEMENT ===

    void ChunkWatchIndex::AddWatcher(uint32_t playerId, Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto& dim = GetOrCreateDimension(DEFAULT_DIMENSION);
        
        // Add player to chunk's watcher set
        dim.watchersByChunk[chunk].insert(playerId);
        
        // Add chunk to player's watched set
        dim.watchedChunksByPlayer[playerId].insert(chunk);
    }

    void ChunkWatchIndex::RemoveWatcher(uint32_t playerId, Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto dimIt = m_dimensions.find(DEFAULT_DIMENSION);
        if (dimIt == m_dimensions.end()) return;
        auto& dim = dimIt->second;
        
        // Remove player from chunk's watcher set
        auto chunkIt = dim.watchersByChunk.find(chunk);
        if (chunkIt != dim.watchersByChunk.end()) {
            chunkIt->second.erase(playerId);
            if (chunkIt->second.empty()) {
                dim.watchersByChunk.erase(chunkIt);
            }
        }
        
        // Remove chunk from player's watched set
        auto playerIt = dim.watchedChunksByPlayer.find(playerId);
        if (playerIt != dim.watchedChunksByPlayer.end()) {
            playerIt->second.erase(chunk);
            if (playerIt->second.empty()) {
                dim.watchedChunksByPlayer.erase(playerIt);
            }
        }
    }

    void ChunkWatchIndex::AddWatchers(uint32_t playerId, const std::vector<Game::Math::ChunkPos>& chunks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto& dim = GetOrCreateDimension(DEFAULT_DIMENSION);
        
        for (const auto& chunk : chunks) {
            dim.watchersByChunk[chunk].insert(playerId);
            dim.watchedChunksByPlayer[playerId].insert(chunk);
        }
    }

    void ChunkWatchIndex::RemoveWatchers(uint32_t playerId, const std::vector<Game::Math::ChunkPos>& chunks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto dimIt = m_dimensions.find(DEFAULT_DIMENSION);
        if (dimIt == m_dimensions.end()) return;
        auto& dim = dimIt->second;
        
        for (const auto& chunk : chunks) {
            // Remove player from chunk's watcher set
            auto chunkIt = dim.watchersByChunk.find(chunk);
            if (chunkIt != dim.watchersByChunk.end()) {
                chunkIt->second.erase(playerId);
                if (chunkIt->second.empty()) {
                    dim.watchersByChunk.erase(chunkIt);
                }
            }
        }
        
        // Remove chunks from player's watched set
        auto playerIt = dim.watchedChunksByPlayer.find(playerId);
        if (playerIt != dim.watchedChunksByPlayer.end()) {
            for (const auto& chunk : chunks) {
                playerIt->second.erase(chunk);
            }
            if (playerIt->second.empty()) {
                dim.watchedChunksByPlayer.erase(playerIt);
            }
        }
    }

    void ChunkWatchIndex::RemoveAllWatchersForPlayer(uint32_t playerId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Process all dimensions
        for (auto& [dimId, dim] : m_dimensions) {
            // Find all chunks watched by this player
            auto playerIt = dim.watchedChunksByPlayer.find(playerId);
            if (playerIt == dim.watchedChunksByPlayer.end()) {
                continue;
            }
            
            // Remove player from each chunk's watcher set
            for (const auto& chunk : playerIt->second) {
                auto chunkIt = dim.watchersByChunk.find(chunk);
                if (chunkIt != dim.watchersByChunk.end()) {
                    chunkIt->second.erase(playerId);
                    if (chunkIt->second.empty()) {
                        dim.watchersByChunk.erase(chunkIt);
                    }
                }
            }
            
            // Remove player's watched chunks
            dim.watchedChunksByPlayer.erase(playerIt);
        }
    }

    // === QUERIES ===

    ChunkWatchIndex::PlayerSet ChunkWatchIndex::GetWatchers(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return {};
        
        auto it = dim->watchersByChunk.find(chunk);
        if (it != dim->watchersByChunk.end()) {
            return it->second;
        }
        
        return {};
    }

    ChunkWatchIndex::ChunkSet ChunkWatchIndex::GetWatchedChunks(uint32_t playerId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return {};
        
        auto it = dim->watchedChunksByPlayer.find(playerId);
        if (it != dim->watchedChunksByPlayer.end()) {
            return it->second;
        }
        
        return {};
    }

    bool ChunkWatchIndex::IsWatching(uint32_t playerId, Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return false;
        
        auto it = dim->watchedChunksByPlayer.find(playerId);
        if (it != dim->watchedChunksByPlayer.end()) {
            return it->second.count(chunk) > 0;
        }
        
        return false;
    }

    bool ChunkWatchIndex::HasWatchers(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return false;
        
        auto it = dim->watchersByChunk.find(chunk);
        return it != dim->watchersByChunk.end() && !it->second.empty();
    }

    size_t ChunkWatchIndex::GetWatcherCount(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return 0;
        
        auto it = dim->watchersByChunk.find(chunk);
        if (it != dim->watchersByChunk.end()) {
            return it->second.size();
        }
        
        return 0;
    }

    size_t ChunkWatchIndex::GetWatchedChunkCount(uint32_t playerId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) return 0;
        
        auto it = dim->watchedChunksByPlayer.find(playerId);
        if (it != dim->watchedChunksByPlayer.end()) {
            return it->second.size();
        }
        
        return 0;
    }

    // === WATCH SET OPERATIONS ===

    ChunkWatchIndex::WatchDelta ChunkWatchIndex::ComputeWatchDelta(
        uint32_t playerId,
        const ChunkSet& newWatchSet
    ) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        WatchDelta delta;
        
        auto* dim = GetDimension(DEFAULT_DIMENSION);
        if (!dim) {
            // No existing watches, all new chunks are additions
            delta.toAdd.insert(delta.toAdd.end(), newWatchSet.begin(), newWatchSet.end());
            return delta;
        }
        
        // Get current watched chunks
        ChunkSet currentWatchSet;
        auto playerIt = dim->watchedChunksByPlayer.find(playerId);
        if (playerIt != dim->watchedChunksByPlayer.end()) {
            currentWatchSet = playerIt->second;
        }
        
        // Compute additions (in new but not in current)
        for (const auto& chunk : newWatchSet) {
            if (currentWatchSet.count(chunk) == 0) {
                delta.toAdd.push_back(chunk);
            }
        }
        
        // Compute removals (in current but not in new)
        for (const auto& chunk : currentWatchSet) {
            if (newWatchSet.count(chunk) == 0) {
                delta.toRemove.push_back(chunk);
            }
        }
        
        return delta;
    }

    ChunkWatchIndex::WatchChangeResult ChunkWatchIndex::ApplyWatchDelta(
        uint32_t playerId,
        const WatchDelta& delta
    ) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        WatchChangeResult result;
        auto& dim = GetOrCreateDimension(DEFAULT_DIMENSION);
        
        // Apply removals
        for (const auto& chunk : delta.toRemove) {
            // Remove player from chunk's watcher set
            auto chunkIt = dim.watchersByChunk.find(chunk);
            if (chunkIt != dim.watchersByChunk.end()) {
                chunkIt->second.erase(playerId);
                if (chunkIt->second.empty()) {
                    dim.watchersByChunk.erase(chunkIt);
                }
                result.removed.push_back(chunk);
            }
            
            // Remove chunk from player's watched set
            auto& playerChunks = dim.watchedChunksByPlayer[playerId];
            playerChunks.erase(chunk);
        }
        
        // Apply additions
        for (const auto& chunk : delta.toAdd) {
            dim.watchersByChunk[chunk].insert(playerId);
            dim.watchedChunksByPlayer[playerId].insert(chunk);
            result.added.push_back(chunk);
        }
        
        // Clean up empty player entry if needed
        auto playerIt = dim.watchedChunksByPlayer.find(playerId);
        if (playerIt != dim.watchedChunksByPlayer.end()) {
            if (playerIt->second.empty()) {
                dim.watchedChunksByPlayer.erase(playerIt);
                result.totalWatched = 0;
            } else {
                result.totalWatched = playerIt->second.size();
            }
        } else {
            result.totalWatched = 0;
        }
        
        return result;
    }

    // === BULK OPERATIONS ===

    std::vector<Game::Math::ChunkPos> ChunkWatchIndex::GetAllWatchedChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        std::vector<Game::Math::ChunkPos> result;
        
        for (const auto& [dimId, dim] : m_dimensions) {
            for (const auto& [chunk, watchers] : dim.watchersByChunk) {
                if (!watchers.empty()) {
                    result.push_back(chunk);
                }
            }
        }
        
        return result;
    }

    std::vector<uint32_t> ChunkWatchIndex::GetAllWatchingPlayers() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        std::unordered_set<uint32_t> players;
        
        for (const auto& [dimId, dim] : m_dimensions) {
            for (const auto& [playerId, chunks] : dim.watchedChunksByPlayer) {
                if (!chunks.empty()) {
                    players.insert(playerId);
                }
            }
        }
        
        return std::vector<uint32_t>(players.begin(), players.end());
    }

    std::vector<Game::Math::ChunkPos> ChunkWatchIndex::GetSharedChunks(size_t minWatchers) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        std::vector<Game::Math::ChunkPos> result;
        
        for (const auto& [dimId, dim] : m_dimensions) {
            for (const auto& [chunk, watchers] : dim.watchersByChunk) {
                if (watchers.size() >= minWatchers) {
                    result.push_back(chunk);
                }
            }
        }
        
        return result;
    }

    // === DIMENSION SUPPORT ===

    void ChunkWatchIndex::ClearDimension(int dimensionId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_dimensions.find(dimensionId);
        if (it != m_dimensions.end()) {
            it->second.Clear();
        }
    }

    void ChunkWatchIndex::MovePlayerToDimension(uint32_t playerId, int fromDim, int toDim) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Remove from old dimension
        auto fromIt = m_dimensions.find(fromDim);
        if (fromIt != m_dimensions.end()) {
            // Find player's chunks
            auto playerIt = fromIt->second.watchedChunksByPlayer.find(playerId);
            if (playerIt != fromIt->second.watchedChunksByPlayer.end()) {
                // Remove player from all watched chunks
                for (const auto& chunk : playerIt->second) {
                    auto chunkIt = fromIt->second.watchersByChunk.find(chunk);
                    if (chunkIt != fromIt->second.watchersByChunk.end()) {
                        chunkIt->second.erase(playerId);
                        if (chunkIt->second.empty()) {
                            fromIt->second.watchersByChunk.erase(chunkIt);
                        }
                    }
                }
                // Remove player entry
                fromIt->second.watchedChunksByPlayer.erase(playerIt);
            }
        }
        
        // Player starts fresh in new dimension (no chunks watched initially)
        GetOrCreateDimension(toDim);
    }

    // === STATISTICS ===

    ChunkWatchIndex::Stats ChunkWatchIndex::GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        Stats stats{};
        std::unordered_set<uint32_t> uniquePlayers;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> uniqueChunks;
        
        for (const auto& [dimId, dim] : m_dimensions) {
            // Count watchers
            for (const auto& [chunk, watchers] : dim.watchersByChunk) {
                stats.totalWatchers += watchers.size();
                uniqueChunks.insert(chunk);
                stats.maxWatchersPerChunk = std::max(stats.maxWatchersPerChunk, watchers.size());
            }
            
            // Count player chunks
            for (const auto& [playerId, chunks] : dim.watchedChunksByPlayer) {
                uniquePlayers.insert(playerId);
                stats.maxChunksPerPlayer = std::max(stats.maxChunksPerPlayer, chunks.size());
            }
        }
        
        stats.uniquePlayers = uniquePlayers.size();
        stats.uniqueChunks = uniqueChunks.size();
        
        if (stats.uniquePlayers > 0) {
            stats.avgChunksPerPlayer = stats.totalWatchers / stats.uniquePlayers;
        }
        
        if (stats.uniqueChunks > 0) {
            stats.avgWatchersPerChunk = stats.totalWatchers / stats.uniqueChunks;
        }
        
        return stats;
    }

    // === MAINTENANCE ===

    bool ChunkWatchIndex::ValidateConsistency() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        for (const auto& [dimId, dim] : m_dimensions) {
            // Check that every player->chunk mapping has a corresponding chunk->player mapping
            for (const auto& [playerId, chunks] : dim.watchedChunksByPlayer) {
                for (const auto& chunk : chunks) {
                    auto chunkIt = dim.watchersByChunk.find(chunk);
                    if (chunkIt == dim.watchersByChunk.end()) {
                        return false; // Missing chunk entry
                    }
                    if (chunkIt->second.count(playerId) == 0) {
                        return false; // Player not in chunk's watcher set
                    }
                }
            }
            
            // Check that every chunk->player mapping has a corresponding player->chunk mapping
            for (const auto& [chunk, watchers] : dim.watchersByChunk) {
                for (uint32_t playerId : watchers) {
                    auto playerIt = dim.watchedChunksByPlayer.find(playerId);
                    if (playerIt == dim.watchedChunksByPlayer.end()) {
                        return false; // Missing player entry
                    }
                    if (playerIt->second.count(chunk) == 0) {
                        return false; // Chunk not in player's watched set
                    }
                }
            }
        }
        
        return true;
    }

    void ChunkWatchIndex::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dimensions.clear();
        m_dimensions[DEFAULT_DIMENSION] = DimensionWatchIndex{};
    }

    size_t ChunkWatchIndex::GetMemoryUsage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        size_t usage = sizeof(*this);
        
        for (const auto& [dimId, dim] : m_dimensions) {
            usage += sizeof(dimId) + sizeof(dim);
            
            // Estimate map overhead
            usage += dim.watchersByChunk.size() * (sizeof(Game::Math::ChunkPos) + sizeof(PlayerSet) + 32);
            usage += dim.watchedChunksByPlayer.size() * (sizeof(uint32_t) + sizeof(ChunkSet) + 32);
            
            // Estimate set contents
            for (const auto& [chunk, watchers] : dim.watchersByChunk) {
                usage += watchers.size() * sizeof(uint32_t);
            }
            for (const auto& [playerId, chunks] : dim.watchedChunksByPlayer) {
                usage += chunks.size() * sizeof(Game::Math::ChunkPos);
            }
        }
        
        return usage;
    }

    // === INTERNAL HELPERS ===

    ChunkWatchIndex::DimensionWatchIndex& ChunkWatchIndex::GetOrCreateDimension(int dimensionId) {
        auto it = m_dimensions.find(dimensionId);
        if (it == m_dimensions.end()) {
            m_dimensions[dimensionId] = DimensionWatchIndex{};
            return m_dimensions[dimensionId];
        }
        return it->second;
    }

    const ChunkWatchIndex::DimensionWatchIndex* ChunkWatchIndex::GetDimension(int dimensionId) const {
        auto it = m_dimensions.find(dimensionId);
        if (it != m_dimensions.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void ChunkWatchIndex::CleanupEmptyEntries(DimensionWatchIndex& dim) {
        // Remove empty chunk entries
        for (auto it = dim.watchersByChunk.begin(); it != dim.watchersByChunk.end();) {
            if (it->second.empty()) {
                it = dim.watchersByChunk.erase(it);
            } else {
                ++it;
            }
        }
        
        // Remove empty player entries
        for (auto it = dim.watchedChunksByPlayer.begin(); it != dim.watchedChunksByPlayer.end();) {
            if (it->second.empty()) {
                it = dim.watchedChunksByPlayer.erase(it);
            } else {
                ++it;
            }
        }
    }

    int ChunkWatchIndex::ChunkDistance(Game::Math::ChunkPos a, Game::Math::ChunkPos b) {
        return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
    }

    // === UTILITY FUNCTIONS ===

    std::vector<Game::Math::ChunkPos> ComputeWatchSet(
        Game::Math::ChunkPos center,
        int viewDistance
    ) {
        std::vector<Game::Math::ChunkPos> chunks;
        chunks.reserve((2 * viewDistance + 1) * (2 * viewDistance + 1));
        
        // Use Chebyshev distance (square pattern like Minecraft)
        for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                chunks.emplace_back(center.x + dx, center.z + dz);
            }
        }
        
        return chunks;
    }

    void SortChunksByDistance(
        std::vector<Game::Math::ChunkPos>& chunks,
        Game::Math::ChunkPos center
    ) {
        std::sort(chunks.begin(), chunks.end(),
            [center](const Game::Math::ChunkPos& a, const Game::Math::ChunkPos& b) {
                int distA = ChunkWatchIndex::ChunkDistance(a, center);
                int distB = ChunkWatchIndex::ChunkDistance(b, center);
                if (distA != distB) {
                    return distA < distB;
                }
                // Stable sort by position for equal distances
                if (a.x != b.x) return a.x < b.x;
                return a.z < b.z;
            });
    }

    std::vector<std::vector<Game::Math::ChunkPos>> GroupChunksByRings(
        const std::vector<Game::Math::ChunkPos>& chunks,
        Game::Math::ChunkPos center,
        int maxDistance
    ) {
        std::vector<std::vector<Game::Math::ChunkPos>> rings(maxDistance + 1);
        
        for (const auto& chunk : chunks) {
            int distance = ChunkWatchIndex::ChunkDistance(chunk, center);
            if (distance <= maxDistance) {
                rings[distance].push_back(chunk);
            }
        }
        
        return rings;
    }

} // namespace Server