// File: src/server/world/watch/ChunkWatchIndex.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <memory>

namespace Server {

    // Forward declaration
    class PlayerSession;

    // Manages bidirectional mapping between players and chunks they're watching
    class ChunkWatchIndex {
    public:
        // Small set optimized for typical watcher counts (1-8 players)
        using PlayerSet = std::unordered_set<uint32_t>;
        using ChunkSet = std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>;

        ChunkWatchIndex();
        ~ChunkWatchIndex();

        // === WATCHER MANAGEMENT ===

        // Add a player as a watcher of a chunk
        void AddWatcher(uint32_t playerId, Game::Math::ChunkPos chunk);
        
        // Remove a player from watching a chunk
        void RemoveWatcher(uint32_t playerId, Game::Math::ChunkPos chunk);
        
        // Bulk add watchers for a player
        void AddWatchers(uint32_t playerId, const std::vector<Game::Math::ChunkPos>& chunks);
        
        // Bulk remove watchers for a player
        void RemoveWatchers(uint32_t playerId, const std::vector<Game::Math::ChunkPos>& chunks);

        // Remove all watched chunks for a player (on disconnect)
        void RemoveAllWatchersForPlayer(uint32_t playerId);

        // === QUERIES ===

        // Get all players watching a specific chunk
        PlayerSet GetWatchers(Game::Math::ChunkPos chunk) const;
        
        // Get all chunks watched by a specific player
        ChunkSet GetWatchedChunks(uint32_t playerId) const;
        
        // Check if a player is watching a chunk
        bool IsWatching(uint32_t playerId, Game::Math::ChunkPos chunk) const;
        
        // Check if a chunk has any watchers
        bool HasWatchers(Game::Math::ChunkPos chunk) const;
        
        // Get number of watchers for a chunk
        size_t GetWatcherCount(Game::Math::ChunkPos chunk) const;
        
        // Get number of chunks watched by a player
        size_t GetWatchedChunkCount(uint32_t playerId) const;

        // === WATCH SET OPERATIONS ===

        // Compute watch set delta for a player moving
        struct WatchDelta {
            std::vector<Game::Math::ChunkPos> toAdd;    // Chunks entering watch
            std::vector<Game::Math::ChunkPos> toRemove; // Chunks leaving watch
        };
        
        WatchDelta ComputeWatchDelta(
            uint32_t playerId,
            const ChunkSet& newWatchSet
        ) const;

        // Apply a watch delta (returns chunks that changed)
        struct WatchChangeResult {
            std::vector<Game::Math::ChunkPos> added;    // Successfully added
            std::vector<Game::Math::ChunkPos> removed;  // Successfully removed
            size_t totalWatched;                        // Total chunks now watched
        };
        
        WatchChangeResult ApplyWatchDelta(
            uint32_t playerId,
            const WatchDelta& delta
        );

        // === BULK OPERATIONS ===

        // Get all chunks with at least one watcher
        std::vector<Game::Math::ChunkPos> GetAllWatchedChunks() const;
        
        // Get all players with at least one watched chunk
        std::vector<uint32_t> GetAllWatchingPlayers() const;
        
        // Get chunks watched by multiple players (for optimization)
        std::vector<Game::Math::ChunkPos> GetSharedChunks(size_t minWatchers = 2) const;

        // === SECTION-LEVEL WATCHING ===
        
        // Get all players watching a specific section
        // A player watches a section if they watch the chunk containing it
        PlayerSet GetSectionWatchers(const Game::Math::SectionPos& sp) const;
        
        // Check if a section has any watchers
        bool HasSectionWatchers(const Game::Math::SectionPos& sp) const;
        
        // Get all players watching any section in a chunk
        PlayerSet GetChunkWatchers(Game::Math::ChunkPos chunk) const {
            return GetWatchers(chunk);  // Alias for clarity
        }
        
        // === DIMENSION SUPPORT ===

        // Clear all watchers for a dimension (for dimension changes)
        void ClearDimension(int dimensionId);
        
        // Move player's watch set to a different dimension
        void MovePlayerToDimension(uint32_t playerId, int fromDim, int toDim);

        // === STATISTICS ===

        struct Stats {
            size_t totalWatchers;      // Total player-chunk mappings
            size_t uniquePlayers;      // Players watching at least one chunk
            size_t uniqueChunks;       // Chunks with at least one watcher
            size_t avgChunksPerPlayer; // Average chunks per player
            size_t avgWatchersPerChunk;// Average watchers per chunk
            size_t maxChunksPerPlayer; // Maximum chunks watched by one player
            size_t maxWatchersPerChunk;// Maximum watchers for one chunk
        };
        
        Stats GetStats() const;
        
        // === MAINTENANCE ===

        // Validate internal consistency
        bool ValidateConsistency() const;
        
        // Clear all watchers
        void Clear();
        
        // Get memory usage estimate
        size_t GetMemoryUsage() const;

    private:
        // Per-dimension watch index
        struct DimensionWatchIndex {
            // Chunk -> Players watching it
            std::unordered_map<Game::Math::ChunkPos, PlayerSet, Game::Math::ChunkPosHash> watchersByChunk;
            
            // Player -> Chunks they're watching
            std::unordered_map<uint32_t, ChunkSet> watchedChunksByPlayer;
            
            void Clear() {
                watchersByChunk.clear();
                watchedChunksByPlayer.clear();
            }
        };

        // Watch indices per dimension
        std::unordered_map<int, DimensionWatchIndex> m_dimensions;
        
        // Default dimension (overworld)
        static constexpr int DEFAULT_DIMENSION = 0;
        
        // Thread safety
        mutable std::mutex m_mutex;
        
        // === INTERNAL HELPERS ===

        // Get or create dimension index
        DimensionWatchIndex& GetOrCreateDimension(int dimensionId);
        
        // Get dimension index (const)
        const DimensionWatchIndex* GetDimension(int dimensionId) const;
        
        // Remove empty entries
        void CleanupEmptyEntries(DimensionWatchIndex& dim);
        
        
    public:
        // Compute Chebyshev distance between chunks (public for utility functions)
        static int ChunkDistance(Game::Math::ChunkPos a, Game::Math::ChunkPos b);
    };

    // === UTILITY FUNCTIONS ===

    // Compute chunks within view distance using Chebyshev metric
    std::vector<Game::Math::ChunkPos> ComputeWatchSet(
        Game::Math::ChunkPos center,
        int viewDistance
    );

    // Sort chunks by distance from center (nearest first)
    void SortChunksByDistance(
        std::vector<Game::Math::ChunkPos>& chunks,
        Game::Math::ChunkPos center
    );

    // Group chunks into distance rings
    std::vector<std::vector<Game::Math::ChunkPos>> GroupChunksByRings(
        const std::vector<Game::Math::ChunkPos>& chunks,
        Game::Math::ChunkPos center,
        int maxDistance
    );

} // namespace Server