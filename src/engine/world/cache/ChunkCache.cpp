// File: src/engine/world/cache/ChunkCache.cpp
#include "ChunkCache.hpp"
#include "../interfaces/IChunkSaver.hpp"
#include <algorithm>
#include <cfloat>

namespace Game {

    ChunkCache::ChunkCache(const ChunkCacheConfig& config)
        : m_config(config)
    {
        if (!m_config.IsValid()) {
            Log::Warning("Invalid ChunkCache configuration, using defaults");
            m_config = ChunkCacheConfig{};
        }

        m_stats.maxSize = m_config.maxSize;

        Log::Debug("ChunkCache created with max size %zu", m_config.maxSize);
    }

    ChunkCache::~ChunkCache() {
        Log::Debug("ChunkCache destructor: saving %zu dirty chunks", GetDirtyChunks().size());

        // Save all dirty chunks before destruction
        SaveAllDirty();

        // Log final statistics
        LogStats("ChunkCache Final");

        Clear();
    }

    ChunkCache::ChunkCache(ChunkCache&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ChunkCache& ChunkCache::operator=(ChunkCache&& other) noexcept {
        if (this != &other) {
            // Clean up current state
            SaveAllDirty();
            Clear();

            // Move from other
            MoveFrom(std::move(other));
        }
        return *this;
    }

    // === CORE CACHE OPERATIONS ===

    std::shared_ptr<Chunk> ChunkCache::Get(Math::ChunkPos position) {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            // Cache hit
            it->second.MarkAccessed();
            UpdateStatsAfterHit();

            return it->second.chunk;
        }

        // Cache miss
        UpdateStatsAfterMiss();
        return nullptr;
    }

    void ChunkCache::Put(Math::ChunkPos position, std::shared_ptr<Chunk> chunk) {
        if (!chunk) {
            Log::Warning("Attempted to put null chunk in cache at (%d, %d)", position.x, position.z);
            return;
        }

        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

        // Check if already exists
        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            // **FIX**: Don't log this as a warning since it's normal behavior
            // Just return early - the existing chunk should remain
            Log::Debug("Chunk (%d, %d) already exists in cache, keeping existing", position.x, position.z);
            return; // Don't overwrite existing chunk
        }

        // Add new entry
        chunk->pos = position;  // Ensure chunk knows its position
        m_cache.emplace(position, ChunkCacheEntry(chunk));
        m_stats.currentSize = m_cache.size();

        Log::Debug("Added new chunk to cache at (%d, %d), cache size: %zu",
                  position.x, position.z, m_cache.size());

        // Hard limit eviction
        if (m_cache.size() > m_config.maxSize) {
            lock.unlock();
            size_t toEvict = m_cache.size() - m_config.maxSize;
            EvictLRU(toEvict);
        }
    }

    bool ChunkCache::Remove(Math::ChunkPos position) {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it == m_cache.end()) {
            return false;
        }

        ChunkCacheEntry entry = std::move(it->second);
        m_cache.erase(it);
        m_stats.currentSize = m_cache.size();

        lock.unlock();

        // Save if dirty
        if (entry.isDirty) {
            EvictChunk(position, entry);
        }

        Log::Debug("Removed chunk from cache at (%d, %d)", position.x, position.z);
        return true;
    }

    bool ChunkCache::Contains(Math::ChunkPos position) const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
        return m_cache.find(position) != m_cache.end();
    }

    std::shared_ptr<Chunk> ChunkCache::Peek(Math::ChunkPos position) const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            return it->second.chunk;
        }

        return nullptr;
    }

    // === DIRTY TRACKING ===

    void ChunkCache::MarkDirty(Math::ChunkPos position) {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            bool wasDirty = it->second.isDirty;
            it->second.MarkDirty();

            if (!wasDirty) {
                m_stats.dirtyChunks++;
                Log::Debug("Marked chunk (%d, %d) as dirty", position.x, position.z);
            }
        }
    }

    bool ChunkCache::IsDirty(Math::ChunkPos position) const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        return it != m_cache.end() && it->second.isDirty;
    }

    std::vector<Math::ChunkPos> ChunkCache::GetDirtyChunks() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        std::vector<Math::ChunkPos> dirtyChunks;
        for (const auto& [pos, entry] : m_cache) {
            if (entry.isDirty) {
                dirtyChunks.push_back(pos);
            }
        }

        return dirtyChunks;
    }

    size_t ChunkCache::SaveAllDirty() {
        if (!m_chunkSaver) {
            Log::Warning("No chunk saver configured, cannot save dirty chunks");
            return 0;
        }

        std::vector<std::shared_ptr<const Chunk>> chunksToSave;
        std::vector<Math::ChunkPos> dirtyPositions;

        // Collect dirty chunks
        {
            std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
            for (const auto& [pos, entry] : m_cache) {
                if (entry.isDirty && entry.chunk) {
                    chunksToSave.push_back(entry.chunk);
                    dirtyPositions.push_back(pos);
                }
            }
        }

        if (chunksToSave.empty()) {
            return 0;
        }

        Log::Debug("Saving %zu dirty chunks", chunksToSave.size());

        // Save chunks
        auto results = m_chunkSaver->SaveChunks(chunksToSave);

        size_t savedCount = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i].success) {
                ClearDirtyFlag(dirtyPositions[i]);
                savedCount++;
                UpdateStatsAfterSave();
            } else {
                Log::Warning("Failed to save chunk (%d, %d): %s",
                           dirtyPositions[i].x, dirtyPositions[i].z,
                           results[i].errorMessage.c_str());
            }
        }

        Log::Debug("Successfully saved %zu out of %zu dirty chunks", savedCount, chunksToSave.size());
        return savedCount;
    }

    void ChunkCache::ClearDirtyFlag(Math::ChunkPos position) {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end() && it->second.isDirty) {
            it->second.isDirty = false;
            m_stats.dirtyChunks--;
        }
    }

    // === CACHE MANAGEMENT ===

    size_t ChunkCache::EvictLRU(size_t maxToEvict) {
        if (maxToEvict == 0) {
            return 0;
        }

        std::vector<Math::ChunkPos> candidates = SelectEvictionCandidates(maxToEvict);

        if (candidates.empty()) {
            return 0;
        }

        Log::Debug("Evicting %zu chunks using LRU policy", candidates.size());

        size_t evictedCount = 0;
        for (Math::ChunkPos pos : candidates) {
            if (Remove(pos)) {
                evictedCount++;
                UpdateStatsAfterEviction();
            }
        }

        return evictedCount;
    }

    size_t ChunkCache::EvictByAge(float maxAgeSeconds) {
        std::vector<Math::ChunkPos> toEvict;

        {
            std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
            for (const auto& [pos, entry] : m_cache) {
                if (entry.GetAgeSeconds() > maxAgeSeconds) {
                    toEvict.push_back(pos);
                }
            }
        }

        return RemoveMultiple(toEvict);
    }

    size_t ChunkCache::EvictByUnusedTime(float maxUnusedSeconds) {
        std::vector<Math::ChunkPos> toEvict;

        {
            std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
            for (const auto& [pos, entry] : m_cache) {
                if (entry.GetTimeSinceAccessSeconds() > maxUnusedSeconds) {
                    toEvict.push_back(pos);
                }
            }
        }

        return RemoveMultiple(toEvict);
    }

    void ChunkCache::Clear() {
        Log::Debug("Clearing chunk cache");

        SaveAllDirty();

        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
        m_cache.clear();
        m_stats.currentSize = 0;
        m_stats.dirtyChunks = 0;
    }

    void ChunkCache::ShrinkToSize(size_t targetSize) {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
        size_t currentSize = m_cache.size();
        lock.unlock();

        if (currentSize <= targetSize) {
            return;
        }

        size_t toEvict = currentSize - targetSize;
        EvictLRU(toEvict);
    }

    // === CONFIGURATION ===

    void ChunkCache::SetConfig(const ChunkCacheConfig& config) {
        if (!config.IsValid()) {
            Log::Warning("Invalid ChunkCache configuration, ignoring");
            return;
        }

        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
        m_config = config;
        m_stats.maxSize = config.maxSize;

        // Shrink if necessary
        if (m_cache.size() > config.maxSize) {
            lock.unlock();
            ShrinkToSize(config.maxSize);
        }
    }

    ChunkCacheConfig ChunkCache::GetConfig() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
        return m_config;
    }

    void ChunkCache::SetChunkSaver(std::shared_ptr<IChunkSaver> saver) {
        m_chunkSaver = saver;
        Log::Debug("ChunkCache: Set chunk saver");
    }

    void ChunkCache::SetEvictionCallback(EvictionCallback callback) {
        m_evictionCallback = callback;
    }

    // === STATISTICS ===

    ChunkCache::CacheStats ChunkCache::GetStats() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        CacheStats stats = m_stats;
        stats.currentSize = m_cache.size();

        // Calculate age statistics
        if (!m_cache.empty()) {
            float totalAge = 0.0f;
            float minAge = FLT_MAX;
            float maxAge = 0.0f;

            for (const auto& [pos, entry] : m_cache) {
                float age = entry.GetAgeSeconds();
                totalAge += age;
                minAge = std::min(minAge, age);
                maxAge = std::max(maxAge, age);
            }

            stats.averageAgeSeconds = totalAge / m_cache.size();
            stats.newestChunkAgeSeconds = minAge;
            stats.oldestChunkAgeSeconds = maxAge;
        }

        return stats;
    }

    void ChunkCache::ResetStats() {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
        m_stats.Reset();
        m_stats.maxSize = m_config.maxSize;
        m_stats.currentSize = m_cache.size();
    }

    // === MAINTENANCE ===

    void ChunkCache::PerformMaintenance() {
        // Age-based eviction
        if (m_config.maxAgeSeconds > 0.0f) {
            EvictByAge(m_config.maxAgeSeconds);
        }

        // Unused time eviction
        if (m_config.maxUnusedTimeSeconds > 0.0f) {
            EvictByUnusedTime(m_config.maxUnusedTimeSeconds);
        }

        // Preemptive eviction check
        CheckPreemptiveEviction();

        // Update statistics
        UpdateAverageAge();
    }

    bool ChunkCache::ValidateIntegrity() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        size_t dirtyCount = 0;
        for (const auto& [pos, entry] : m_cache) {
            // Check chunk pointer
            if (!entry.chunk) {
                Log::Error("ChunkCache integrity error: null chunk at (%d, %d)", pos.x, pos.z);
                return false;
            }

            // Check position consistency
            if (entry.chunk->pos.x != pos.x || entry.chunk->pos.z != pos.z) {
                Log::Error("ChunkCache integrity error: position mismatch at (%d, %d)", pos.x, pos.z);
                return false;
            }

            if (entry.isDirty) {
                dirtyCount++;
            }
        }

        if (dirtyCount != m_stats.dirtyChunks) {
            Log::Error("ChunkCache integrity error: dirty count mismatch (%zu vs %zu)",
                      dirtyCount, m_stats.dirtyChunks);
            return false;
        }

        return true;
    }

    size_t ChunkCache::GetMemoryUsageBytes() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        size_t totalSize = sizeof(ChunkCache);
        totalSize += m_cache.bucket_count() * sizeof(void*); // Hash table overhead

        for (const auto& [pos, entry] : m_cache) {
            totalSize += sizeof(Math::ChunkPos) + sizeof(ChunkCacheEntry);
            if (entry.chunk) {
                // Estimate chunk memory usage
                totalSize += sizeof(Chunk);
                totalSize += entry.chunk->GetNonAirBlockCount() * sizeof(BlockID); // Rough estimate
            }
        }

        return totalSize;
    }

    // === BULK OPERATIONS ===

    std::vector<std::shared_ptr<Chunk>> ChunkCache::GetMultiple(const std::vector<Math::ChunkPos>& positions) {
        std::vector<std::shared_ptr<Chunk>> results;
        results.reserve(positions.size());

        for (Math::ChunkPos pos : positions) {
            results.push_back(Get(pos));
        }

        return results;
    }

    void ChunkCache::PutMultiple(const std::vector<std::pair<Math::ChunkPos, std::shared_ptr<Chunk>>>& chunks) {
        for (const auto& [pos, chunk] : chunks) {
            Put(pos, chunk);
        }
    }

    size_t ChunkCache::RemoveMultiple(const std::vector<Math::ChunkPos>& positions) {
        size_t removedCount = 0;
        for (Math::ChunkPos pos : positions) {
            if (Remove(pos)) {
                removedCount++;
            }
        }
        return removedCount;
    }

    // === DEBUGGING ===

    ChunkCache::CacheState ChunkCache::GetDebugState() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        CacheState state;

        for (const auto& [pos, entry] : m_cache) {
            state.allChunks.push_back(pos);

            if (entry.isDirty) {
                state.dirtyChunks.push_back(pos);
            }

            state.chunksWithAge.emplace_back(pos, entry.GetAgeSeconds());
            state.chunksWithAccessCount.emplace_back(pos, entry.accessCount);
        }

        return state;
    }

    void ChunkCache::LogStats(const std::string& prefix) const {
        CacheStats stats = GetStats();

        Log::Info("%s Stats: Size=%zu/%zu (%.1f%%), Hits=%zu, Misses=%zu (%.1f%% hit rate), "
                 "Evictions=%zu, Saves=%zu, Dirty=%zu",
                 prefix.c_str(),
                 stats.currentSize, stats.maxSize, stats.GetUtilization() * 100.0f,
                 stats.totalHits, stats.totalMisses, stats.GetHitRate() * 100.0f,
                 stats.totalEvictions, stats.totalSaves, stats.dirtyChunks);

        if (stats.currentSize > 0) {
            Log::Debug("%s Age Stats: Avg=%.1fs, Oldest=%.1fs, Newest=%.1fs",
                      prefix.c_str(),
                      stats.averageAgeSeconds, stats.oldestChunkAgeSeconds, stats.newestChunkAgeSeconds);
        }
    }

    // === PRIVATE HELPERS ===

    void ChunkCache::UpdateStatsAfterHit() {
        m_stats.totalHits++;
    }

    void ChunkCache::UpdateStatsAfterMiss() {
        m_stats.totalMisses++;
    }

    void ChunkCache::UpdateStatsAfterEviction() {
        m_stats.totalEvictions++;
    }

    void ChunkCache::UpdateStatsAfterSave() {
        m_stats.totalSaves++;
    }

    std::vector<Math::ChunkPos> ChunkCache::SelectEvictionCandidates(size_t maxCandidates) const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        if (m_cache.empty()) {
            return {};
        }

        // Collect all entries with their scores
        std::vector<std::pair<Math::ChunkPos, float>> candidates;
        candidates.reserve(m_cache.size());

        for (const auto& [pos, entry] : m_cache) {
            if (ShouldEvictChunk(entry)) {
                // Calculate eviction score (higher = more likely to evict)
                float score = entry.GetTimeSinceAccessSeconds();

                // Boost score for older chunks
                score += entry.GetAgeSeconds() * 0.1f;

                // Reduce score for frequently accessed chunks
                score -= static_cast<float>(entry.accessCount) * 0.01f;

                candidates.emplace_back(pos, score);
            }
        }

        // Sort by score (highest first)
        std::sort(candidates.begin(), candidates.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

        // Extract positions
        std::vector<Math::ChunkPos> result;
        size_t count = std::min(maxCandidates, candidates.size());
        result.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            result.push_back(candidates[i].first);
        }

        return result;
    }

    bool ChunkCache::ShouldEvictChunk(const ChunkCacheEntry& entry) const {
        // Don't evict recently accessed chunks
        if (entry.GetTimeSinceAccessSeconds() < 1.0f) {
            return false;
        }

        // Always consider old chunks for eviction
        if (entry.GetAgeSeconds() > m_config.maxAgeSeconds) {
            return true;
        }

        // Consider unused chunks for eviction
        if (entry.GetTimeSinceAccessSeconds() > m_config.maxUnusedTimeSeconds) {
            return true;
        }

        return true; // Default: can be evicted
    }

    void ChunkCache::EvictChunk(Math::ChunkPos position, ChunkCacheEntry& entry) {
        bool wasDirty = entry.isDirty;

        // Save if dirty and we have a saver
        if (wasDirty && m_chunkSaver && entry.chunk) {
            auto future = m_chunkSaver->SaveChunkAsync(*entry.chunk);
            // Note: In a full implementation, you might want to track this future
        }

        // Call eviction callback
        if (m_evictionCallback) {
            m_evictionCallback(position, entry.chunk, wasDirty);
        }

        Log::Debug("Evicted chunk (%d, %d) %s",
                  position.x, position.z, wasDirty ? "(was dirty)" : "");
    }

    void ChunkCache::CheckPreemptiveEviction() {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
        size_t currentSize = m_cache.size();
        size_t threshold = static_cast<size_t>(m_config.maxSize * m_config.preemptiveEvictionThreshold);
        lock.unlock();

        if (currentSize >= threshold) {
            size_t toEvict = std::max(static_cast<size_t>(1), m_config.evictionBatchSize);
            EvictLRU(toEvict);
        }
    }

    void ChunkCache::UpdateAverageAge() const {
        // This is called from maintenance, age stats are calculated in GetStats()
        // No separate implementation needed as it's computed on-demand
    }

    void ChunkCache::MoveFrom(ChunkCache&& other) noexcept {
        m_cache = std::move(other.m_cache);
        m_config = other.m_config;
        m_chunkSaver = std::move(other.m_chunkSaver);
        m_evictionCallback = std::move(other.m_evictionCallback);
        m_stats = other.m_stats;

        // Clear other's state
        other.m_cache.clear();
        other.m_chunkSaver.reset();
        other.m_evictionCallback = nullptr;
        other.m_stats.Reset();
    }

} // namespace Game