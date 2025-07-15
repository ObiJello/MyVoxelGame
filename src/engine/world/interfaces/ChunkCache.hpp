// File: src/engine/world/cache/ChunkCache.hpp
#pragma once

#include "../Chunk.hpp"
#include "../../../game/WorldMath.hpp"
#include "../../../core/Log.hpp"
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <chrono>
#include <functional>

namespace Game {

    // Forward declarations
    class IChunkSaver;

    // Hash function for ChunkPos (reused from ChunkProvider)
    struct ChunkPosHash {
        std::size_t operator()(const Math::ChunkPos& pos) const {
            return std::hash<int32_t>{}(pos.x) ^ (std::hash<int32_t>{}(pos.z) << 1);
        }
    };

    // Entry in the chunk cache
    struct ChunkCacheEntry {
        std::shared_ptr<Chunk> chunk;
        std::chrono::steady_clock::time_point lastAccess;
        std::chrono::steady_clock::time_point loadTime;
        bool isDirty = false;
        size_t accessCount = 0;

        ChunkCacheEntry() = default;

        ChunkCacheEntry(std::shared_ptr<Chunk> chunkPtr)
            : chunk(chunkPtr)
            , lastAccess(std::chrono::steady_clock::now())
            , loadTime(std::chrono::steady_clock::now())
        {}

        void MarkAccessed() {
            lastAccess = std::chrono::steady_clock::now();
            accessCount++;
        }

        void MarkDirty() {
            isDirty = true;
        }

        // Get age in seconds
        float GetAgeSeconds() const {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - loadTime);
            return duration.count() / 1000.0f;
        }

        // Get time since last access in seconds
        float GetTimeSinceAccessSeconds() const {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAccess);
            return duration.count() / 1000.0f;
        }
    };

    // Configuration for chunk cache behavior
    struct ChunkCacheConfig {
        size_t maxSize = 1024;                    // Maximum number of chunks to cache
        float maxAgeSeconds = 300.0f;             // Maximum age before eviction (5 minutes)
        float maxUnusedTimeSeconds = 60.0f;       // Maximum time unused before eviction (1 minute)
        bool enableAutoSave = true;               // Automatically save dirty chunks on eviction
        bool enableLRUEviction = true;            // Use LRU eviction policy
        size_t evictionBatchSize = 10;            // How many chunks to evict at once

        // Performance settings
        bool enableAccessTracking = true;        // Track access patterns for optimization
        bool enablePreemptiveEviction = true;    // Evict before hitting max size
        float preemptiveEvictionThreshold = 0.9f; // Evict when cache is 90% full

        bool IsValid() const {
            return maxSize > 0 && maxAgeSeconds > 0.0f && maxUnusedTimeSeconds > 0.0f &&
                   preemptiveEvictionThreshold > 0.0f && preemptiveEvictionThreshold < 1.0f;
        }
    };

    // RAII chunk cache with LRU eviction and automatic dirty chunk saving
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

        // Get chunk if in cache, otherwise return null (doesn't update access time)
        std::shared_ptr<Chunk> Peek(Math::ChunkPos position) const;

        // === DIRTY TRACKING ===

        // Mark chunk as dirty (needs saving)
        void MarkDirty(Math::ChunkPos position);

        // Check if chunk is dirty
        bool IsDirty(Math::ChunkPos position) const;

        // Get all dirty chunks
        std::vector<Math::ChunkPos> GetDirtyChunks() const;

        // Save all dirty chunks
        size_t SaveAllDirty();

        // Clear dirty flag for chunk (after successful save)
        void ClearDirtyFlag(Math::ChunkPos position);

        // === CACHE MANAGEMENT ===

        // Manually trigger eviction based on LRU policy
        size_t EvictLRU(size_t maxToEvict = SIZE_MAX);

        // Evict chunks older than specified age
        size_t EvictByAge(float maxAgeSeconds);

        // Evict chunks unused for specified time
        size_t EvictByUnusedTime(float maxUnusedSeconds);

        // Clear entire cache (saves all dirty chunks first)
        void Clear();

        // Shrink cache to target size
        void ShrinkToSize(size_t targetSize);

        // === CONFIGURATION ===

        // Update cache configuration
        void SetConfig(const ChunkCacheConfig& config);
        ChunkCacheConfig GetConfig() const;

        // Set chunk saver for automatic dirty chunk saving
        void SetChunkSaver(std::shared_ptr<IChunkSaver> saver);

        // Set callback for when chunks are evicted
        using EvictionCallback = std::function<void(Math::ChunkPos, std::shared_ptr<Chunk>, bool wasDirty)>;
        void SetEvictionCallback(EvictionCallback callback);

        // === STATISTICS ===

        struct CacheStats {
            size_t currentSize = 0;
            size_t maxSize = 0;
            size_t totalHits = 0;
            size_t totalMisses = 0;
            size_t totalEvictions = 0;
            size_t totalSaves = 0;
            size_t dirtyChunks = 0;

            float averageAgeSeconds = 0.0f;
            float oldestChunkAgeSeconds = 0.0f;
            float newestChunkAgeSeconds = 0.0f;

            float GetHitRate() const {
                size_t total = totalHits + totalMisses;
                return total > 0 ? static_cast<float>(totalHits) / static_cast<float>(total) : 0.0f;
            }

            float GetUtilization() const {
                return maxSize > 0 ? static_cast<float>(currentSize) / static_cast<float>(maxSize) : 0.0f;
            }

            void Reset() {
                totalHits = totalMisses = totalEvictions = totalSaves = 0;
                averageAgeSeconds = oldestChunkAgeSeconds = newestChunkAgeSeconds = 0.0f;
            }
        };

        CacheStats GetStats() const;
        void ResetStats();

        // === MAINTENANCE ===

        // Perform maintenance operations (eviction, cleanup)
        void PerformMaintenance();

        // Check cache integrity and consistency
        bool ValidateIntegrity() const;

        // Get memory usage estimate
        size_t GetMemoryUsageBytes() const;

        // === BULK OPERATIONS ===

        // Get multiple chunks at once (more efficient)
        std::vector<std::shared_ptr<Chunk>> GetMultiple(const std::vector<Math::ChunkPos>& positions);

        // Put multiple chunks at once
        void PutMultiple(const std::vector<std::pair<Math::ChunkPos, std::shared_ptr<Chunk>>>& chunks);

        // Remove multiple chunks
        size_t RemoveMultiple(const std::vector<Math::ChunkPos>& positions);

        // === DEBUGGING ===

        // Get detailed cache state for debugging
        struct CacheState {
            std::vector<Math::ChunkPos> allChunks;
            std::vector<Math::ChunkPos> dirtyChunks;
            std::vector<std::pair<Math::ChunkPos, float>> chunksWithAge;
            std::vector<std::pair<Math::ChunkPos, size_t>> chunksWithAccessCount;
        };

        CacheState GetDebugState() const;

        // Log cache statistics
        void LogStats(const std::string& prefix = "ChunkCache") const;

    private:
        // Cache storage
        mutable std::shared_mutex m_cacheMutex;
        std::unordered_map<Math::ChunkPos, ChunkCacheEntry, ChunkPosHash> m_cache;

        // Configuration
        ChunkCacheConfig m_config;

        // Dependencies
        std::shared_ptr<IChunkSaver> m_chunkSaver;
        EvictionCallback m_evictionCallback;

        // Statistics
        mutable CacheStats m_stats;

        // Internal helpers
        void UpdateStatsAfterHit();
        void UpdateStatsAfterMiss();
        void UpdateStatsAfterEviction();
        void UpdateStatsAfterSave();

        // Eviction helpers
        std::vector<Math::ChunkPos> SelectEvictionCandidates(size_t maxCandidates) const;
        bool ShouldEvictChunk(const ChunkCacheEntry& entry) const;
        void EvictChunk(Math::ChunkPos position, ChunkCacheEntry& entry);

        // Maintenance helpers
        void CheckPreemptiveEviction();
        void UpdateAverageAge() const;

        // Move implementation
        void MoveFrom(ChunkCache&& other) noexcept;
    };

} // namespace Game