// File: src/server/world/cache/ChunkCache.hpp
#pragma once

#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/Log.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include <list>
#include "../interfaces/IChunkSaver.hpp"

namespace Game {

    // Forward declarations
    class IChunkSaver;

    // Simple cache entry
    struct ChunkCacheEntry {
        std::shared_ptr<Chunk> chunk;
        std::chrono::steady_clock::time_point loadTime;
        bool isDirty = false;

        ChunkCacheEntry() = default;
        ChunkCacheEntry(std::shared_ptr<Chunk> chunkPtr)
            : chunk(chunkPtr)
            , loadTime(std::chrono::steady_clock::now()) {}

        void MarkDirty() {
            isDirty = true;
        }
    };

    // Simple configuration
    struct ChunkCacheConfig {
        size_t maxSize = 2048;  // Increased for better performance with VD 10-12

        bool IsValid() const {
            return maxSize > 0;
        }
    };

    // Simplified chunk cache with basic LRU eviction
    class ChunkCache {
    public:
        explicit ChunkCache(const ChunkCacheConfig& config = ChunkCacheConfig{});
        ~ChunkCache();

        // Non-copyable but movable
        ChunkCache(const ChunkCache&) = delete;
        ChunkCache& operator=(const ChunkCache&) = delete;
        ChunkCache(ChunkCache&& other) noexcept;
        ChunkCache& operator=(ChunkCache&& other) noexcept;

        // === CORE CACHE OPERATIONS ===

        // Get chunk from cache (returns null if not cached)
        std::shared_ptr<Chunk> Get(Math::ChunkPos position);

        // Put chunk in cache (may trigger eviction)
        void Put(Math::ChunkPos position, std::shared_ptr<Chunk> chunk);

        // Remove chunk from cache (saves if dirty)
        bool Remove(Math::ChunkPos position);

        // Check if chunk is in cache
        bool Contains(Math::ChunkPos position) const;

        // === DIRTY TRACKING ===

        // Mark chunk as dirty (needs saving)
        void MarkDirty(Math::ChunkPos position);

        // Check if chunk is dirty
        bool IsDirty(Math::ChunkPos position) const;

        // Get all dirty chunks
        std::vector<Math::ChunkPos> GetDirtyChunks() const;

        // Save all dirty chunks
        void SaveAllDirty();

        // Clear dirty flag for chunk (after successful save)
        void ClearDirtyFlag(Math::ChunkPos position);

        // === CACHE MANAGEMENT ===

        // Clear entire cache (saves all dirty chunks first)
        void Clear();

        // === CONFIGURATION ===

        // Set chunk saver for automatic dirty chunk saving
        void SetChunkSaver(std::shared_ptr<IChunkSaver> saver);

        // Set callback for when chunks are evicted
        using EvictionCallback = std::function<void(Math::ChunkPos, std::shared_ptr<Chunk>, bool wasDirty)>;
        void SetEvictionCallback(EvictionCallback callback);

        // === STATISTICS ===

        struct CacheStats {
            size_t currentSize = 0;
            size_t maxSize = 0;
            size_t totalEvictions = 0;
            size_t dirtyChunks = 0;

            float GetUtilization() const {
                return maxSize > 0 ? static_cast<float>(currentSize) / static_cast<float>(maxSize) : 0.0f;
            }

            void Reset() {
                totalEvictions = 0;
            }
        };

        CacheStats GetStats() const;
        void ResetStats();

        // Get memory usage estimate
        size_t GetMemoryUsageBytes() const;

        // === DEBUGGING ===

        // Get detailed cache state for debugging
        struct CacheState {
            std::vector<Math::ChunkPos> allChunks;
            std::vector<Math::ChunkPos> dirtyChunks;
        };

        CacheState GetDebugState() const;

        // Log cache statistics
        void LogStats(const std::string& prefix = "ChunkCache") const;

    private:
        // Cache storage
        mutable std::mutex m_cacheMutex;
        std::unordered_map<Math::ChunkPos, ChunkCacheEntry, Math::ChunkPosHash> m_cache;

        // Keep track of access order for LRU
        std::list<Math::ChunkPos> m_accessOrder;
        std::unordered_map<Math::ChunkPos, std::list<Math::ChunkPos>::iterator, Math::ChunkPosHash> m_accessIterators;

        // Configuration
        ChunkCacheConfig m_config;

        // Dependencies
        std::shared_ptr<IChunkSaver> m_chunkSaver;
        EvictionCallback m_evictionCallback;

        // Statistics
        mutable CacheStats m_stats;

        // Internal helpers
        void UpdateAccess(Math::ChunkPos position);
        void EvictLRU();
        void EvictChunk(Math::ChunkPos position, ChunkCacheEntry& entry);

        // Move implementation
        void MoveFrom(ChunkCache&& other) noexcept;
    };

} // namespace Game