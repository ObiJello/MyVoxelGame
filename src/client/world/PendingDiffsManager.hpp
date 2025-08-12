// File: src/client/world/PendingDiffsManager.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/network/PacketTypes.hpp"
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>

namespace Client {

    // Manages pending block/light changes for chunks that haven't arrived yet
    // Implements bounded storage with coalescing to prevent memory exhaustion
    class PendingDiffsManager {
    public:
        // Configuration constants
        static constexpr size_t MAX_DIFFS_PER_CHUNK = 256;        // Max diffs per chunk
        static constexpr size_t MAX_CHUNKS_WITH_DIFFS = 64;       // Max chunks that can have pending diffs
        static constexpr size_t MAX_TOTAL_DIFFS = 4096;           // Global diff limit
        
        // Block position within world (not chunk-relative)
        struct BlockPos {
            int x, y, z;
            
            bool operator==(const BlockPos& other) const {
                return x == other.x && y == other.y && z == other.z;
            }
        };
        
        struct BlockPosHash {
            std::size_t operator()(const BlockPos& pos) const {
                return std::hash<int>{}(pos.x) ^ 
                       (std::hash<int>{}(pos.y) << 1) ^
                       (std::hash<int>{}(pos.z) << 2);
            }
        };
        
        // A single block change
        struct BlockChange {
            BlockPos pos;
            Game::BlockID blockId;
            uint32_t generation;    // Generation when change was received
            std::chrono::steady_clock::time_point timestamp;
            bool playSound = false;
            bool updateNeighbors = false;
        };
        
        // Light update data
        struct LightUpdate {
            BlockPos pos;
            uint8_t skyLight;
            uint8_t blockLight;
            uint32_t generation;
            std::chrono::steady_clock::time_point timestamp;
        };
        
        // Block entity update
        struct BlockEntityUpdate {
            BlockPos pos;
            std::vector<uint8_t> nbtData;
            uint32_t generation;
            std::chrono::steady_clock::time_point timestamp;
        };
        
        // All pending diffs for a chunk
        struct ChunkDiffs {
            // Coalesced block changes (latest change wins)
            std::unordered_map<BlockPos, BlockChange, BlockPosHash> blockChanges;
            
            // Light updates (may have multiple per block)
            std::vector<LightUpdate> lightUpdates;
            
            // Block entity updates
            std::unordered_map<BlockPos, BlockEntityUpdate, BlockPosHash> blockEntityUpdates;
            
            // Chunk generation when these diffs were collected
            uint32_t generation = 0;
            
            // Stats
            size_t GetTotalDiffCount() const {
                return blockChanges.size() + lightUpdates.size() + blockEntityUpdates.size();
            }
        };
        
    public:
        PendingDiffsManager();
        ~PendingDiffsManager();
        
        // ========================================================================
        // ADDING DIFFS (thread-safe)
        // ========================================================================
        
        // Add a block change (coalesces with existing change for same position)
        void AddBlockChange(Game::Math::ChunkPos chunkPos, const Network::BlockChangeS2CPacket& packet);
        
        // Add multiple block changes
        void AddMultiBlockChange(const Network::MultiBlockChangeS2CPacket& packet);
        
        // Add a light update (future use)
        void AddLightUpdate(Game::Math::ChunkPos chunkPos, const BlockPos& pos, 
                           uint8_t skyLight, uint8_t blockLight);
        
        // Add a block entity update (future use)
        void AddBlockEntityUpdate(Game::Math::ChunkPos chunkPos, const BlockPos& pos,
                                 const std::vector<uint8_t>& nbtData);
        
        // ========================================================================
        // APPLYING DIFFS (main thread only)
        // ========================================================================
        
        // Apply all pending diffs for a chunk (called after groundUp chunk load)
        // Returns the number of diffs applied
        size_t ApplyPendingDiffs(Game::Math::ChunkPos chunkPos, uint32_t chunkGeneration);
        
        // Get pending diffs for inspection (doesn't remove them)
        const ChunkDiffs* GetPendingDiffs(Game::Math::ChunkPos chunkPos) const;
        
        // ========================================================================
        // CLEANUP
        // ========================================================================
        
        // Drop all diffs for a chunk (called on UnloadChunk)
        void DropChunkDiffs(Game::Math::ChunkPos chunkPos);
        
        // Drop stale diffs older than the given duration
        void DropStaleDiffs(std::chrono::seconds maxAge = std::chrono::seconds(60));
        
        // Clear all pending diffs
        void ClearAll();
        
        // ========================================================================
        // STATISTICS
        // ========================================================================
        
        struct Stats {
            size_t chunksWithDiffs = 0;
            size_t totalBlockChanges = 0;
            size_t totalLightUpdates = 0;
            size_t totalBlockEntityUpdates = 0;
            size_t droppedDiffs = 0;           // Diffs dropped due to limits
            size_t coalescedChanges = 0;       // Changes that replaced older ones
            size_t appliedDiffs = 0;           // Successfully applied diffs
        };
        
        Stats GetStats() const;
        void ResetStats();
        
    private:
        // Internal helper to enforce limits
        void EnforceLimits();
        
        // Drop oldest chunk's diffs if we're over the chunk limit
        void DropOldestChunkIfNeeded();
        
        // Convert world coordinates to block position
        static BlockPos WorldToBlockPos(int worldX, int worldY, int worldZ) {
            return {worldX, worldY, worldZ};
        }
        
    private:
        // Thread safety
        mutable std::mutex m_mutex;
        
        // Main storage: chunk position -> pending diffs
        std::unordered_map<Game::Math::ChunkPos, ChunkDiffs, Game::Math::ChunkPosHash> m_pendingDiffs;
        
        // Track insertion order for LRU eviction
        std::vector<Game::Math::ChunkPos> m_chunkInsertionOrder;
        
        // Statistics
        mutable Stats m_stats;
        
        // Current generation counter (incremented for each new chunk groundUp)
        std::atomic<uint32_t> m_currentGeneration{1};
    };

} // namespace Client