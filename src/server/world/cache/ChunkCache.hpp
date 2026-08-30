// File: src/server/world/cache/ChunkCache.hpp
#pragma once

#include <atomic>

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

    // Bumped by every mutation that can change what Get() answers — Put,
    // Remove, Clear and eviction. A caller memoizing Get()'s result validates
    // against this instead of re-entering the cache, which is the whole point:
    // Get takes a process-wide mutex, does two hash lookups, an LRU list
    // erase+push_front (one free, one alloc) and returns a shared_ptr by value.
    // See ChunkProvider::GetCachedChunk.
    uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }
    // The same counter split by WHAT changed, for a memo that can afford to be
    // smarter than "forget everything": a Put can only turn a "not resident"
    // answer stale (a chunk appeared), a Remove/eviction/Clear can only turn a
    // "resident" answer stale (a chunk vanished). While chunks stream in
    // around a mass event, Put fires hundreds of times a second and the
    // whole-memo reset it caused sent every worker back through the mutex —
    // three quarters of the falling-block physics time was __psynch_mutexwait.
    uint64_t PutGeneration() const    { return m_putGeneration.load(std::memory_order_acquire); }
    uint64_t RemoveGeneration() const { return m_removeGeneration.load(std::memory_order_acquire); }

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

        // Get all loaded chunk positions (for iteration without full debug state)
        std::vector<Math::ChunkPos> GetLoadedChunkPositions() const;

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
        std::atomic<uint64_t> m_generation{0};
        std::atomic<uint64_t> m_putGeneration{0};
        std::atomic<uint64_t> m_removeGeneration{0};
// The size at which Get resumes maintaining LRU order. Below it nothing
        // can be evicted, so recency decides nothing and the splice is skipped.
        // 1/8 of headroom so a burst of Puts cannot cross from "order does not
        // matter" to "evicting" inside one tick without Get having kept order
        // for a while first.
        size_t EvictionWatermark() const {
            return m_config.maxSize - m_config.maxSize / 8;
        }

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
        // One chunk on its way out, carried from under the cache lock to
        // after it is released.
        struct PendingEviction {
            Math::ChunkPos         pos;
            std::shared_ptr<Chunk> chunk;
            bool                   wasDirty = false;
        };

        // Both run WITH m_cacheMutex held and do no I/O: they only move the
        // entry out of the map and record what has to happen next.
        void EvictLRU(std::vector<PendingEviction>& out);

        // Runs with the lock RELEASED. Saving a chunk now means serialising
        // and compressing it, and doing that while holding the lock every
        // block read needs was the single worst contention point in the cache
        // — with maxSize 5120 it evicted on essentially every Put.
        void FlushEvictions(std::vector<PendingEviction>& pending);

        // Move implementation
        void MoveFrom(ChunkCache&& other) noexcept;
    };

} // namespace Game