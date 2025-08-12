// File: src/server/world/cache/ChunkCache.cpp
#include "ChunkCache.hpp"

namespace Game {

    ChunkCache::ChunkCache(const ChunkCacheConfig& config)
        : m_config(config) {

        if (!m_config.IsValid()) {
            Log::Warning("Invalid ChunkCache configuration, using defaults");
            m_config = ChunkCacheConfig{};
        }

        m_stats.maxSize = m_config.maxSize;
        Log::Debug("ChunkCache created with max size %zu", m_config.maxSize);
    }

    ChunkCache::~ChunkCache() {
        Log::Debug("ChunkCache destructor: saving %zu dirty chunks", GetDirtyChunks().size());
        SaveAllDirty();
        Clear();
    }

    ChunkCache::ChunkCache(ChunkCache&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ChunkCache& ChunkCache::operator=(ChunkCache&& other) noexcept {
        if (this != &other) {
            SaveAllDirty();
            Clear();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    // === CORE CACHE OPERATIONS ===

    std::shared_ptr<Chunk> ChunkCache::Get(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            UpdateAccess(position);
            return it->second.chunk;
        }

        return nullptr;
    }

    void ChunkCache::Put(Math::ChunkPos position, std::shared_ptr<Chunk> chunk) {
        if (!chunk) {
            Log::Warning("Attempted to put null chunk in cache at (%d, %d)", position.x, position.z);
            return;
        }

        std::lock_guard<std::mutex> lock(m_cacheMutex);

        // Check if already exists
        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            Log::Debug("Chunk (%d, %d) already exists in cache, keeping existing", position.x, position.z);
            return;
        }

        // Add new entry
        chunk->pos = position;
        m_cache.emplace(position, ChunkCacheEntry(chunk));

        // Add to access order
        m_accessOrder.push_front(position);
        m_accessIterators[position] = m_accessOrder.begin();

        m_stats.currentSize = m_cache.size();

        /*Log::Debug("Added new chunk to cache at (%d, %d), cache size: %zu",
                  position.x, position.z, m_cache.size());*/

        // Evict if needed
        while (m_cache.size() > m_config.maxSize) {
            EvictLRU();
        }
    }

    bool ChunkCache::Remove(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it == m_cache.end()) {
            return false;
        }

        ChunkCacheEntry entry = std::move(it->second);
        m_cache.erase(it);

        // Remove from access order
        auto accessIt = m_accessIterators.find(position);
        if (accessIt != m_accessIterators.end()) {
            m_accessOrder.erase(accessIt->second);
            m_accessIterators.erase(accessIt);
        }

        m_stats.currentSize = m_cache.size();

        // Save if dirty
        if (entry.isDirty) {
            EvictChunk(position, entry);
        }

        Log::Debug("Removed chunk from cache at (%d, %d)", position.x, position.z);
        return true;
    }

    bool ChunkCache::Contains(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        return m_cache.find(position) != m_cache.end();
    }

    // === DIRTY TRACKING ===

    void ChunkCache::MarkDirty(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

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
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        return it != m_cache.end() && it->second.isDirty;
    }

    std::vector<Math::ChunkPos> ChunkCache::GetDirtyChunks() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        std::vector<Math::ChunkPos> dirtyChunks;
        for (const auto& [pos, entry] : m_cache) {
            if (entry.isDirty) {
                dirtyChunks.push_back(pos);
            }
        }

        return dirtyChunks;
    }

    void ChunkCache::SaveAllDirty() {
        if (!m_chunkSaver) {
            Log::Warning("No chunk saver configured, cannot save dirty chunks");
            return;
        }

        std::vector<std::shared_ptr<const Chunk>> chunksToSave;
        std::vector<Math::ChunkPos> dirtyPositions;

        // Collect dirty chunks
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            for (const auto& [pos, entry] : m_cache) {
                if (entry.isDirty && entry.chunk) {
                    chunksToSave.push_back(entry.chunk);
                    dirtyPositions.push_back(pos);
                }
            }
        }

        if (chunksToSave.empty()) {
            return;
        }

        Log::Debug("Saving %zu dirty chunks", chunksToSave.size());

        // Save chunks
        auto results = m_chunkSaver->SaveChunks(chunksToSave);

        size_t savedCount = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i].success) {
                ClearDirtyFlag(dirtyPositions[i]);
                savedCount++;
            } else {
                Log::Warning("Failed to save chunk (%d, %d): %s",
                           dirtyPositions[i].x, dirtyPositions[i].z,
                           results[i].errorMessage.c_str());
            }
        }

        Log::Debug("Successfully saved %zu out of %zu dirty chunks", savedCount, chunksToSave.size());
    }

    void ChunkCache::ClearDirtyFlag(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        auto it = m_cache.find(position);
        if (it != m_cache.end() && it->second.isDirty) {
            it->second.isDirty = false;
            m_stats.dirtyChunks--;
        }
    }

    // === CACHE MANAGEMENT ===

    void ChunkCache::Clear() {
        Log::Debug("Clearing chunk cache");
        SaveAllDirty();

        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache.clear();
        m_accessOrder.clear();
        m_accessIterators.clear();
        m_stats.currentSize = 0;
        m_stats.dirtyChunks = 0;
    }

    // === CONFIGURATION ===

    void ChunkCache::SetChunkSaver(std::shared_ptr<IChunkSaver> saver) {
        m_chunkSaver = saver;
        Log::Debug("ChunkCache: Set chunk saver");
    }

    void ChunkCache::SetEvictionCallback(EvictionCallback callback) {
        m_evictionCallback = callback;
    }

    // === STATISTICS ===

    ChunkCache::CacheStats ChunkCache::GetStats() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        CacheStats stats = m_stats;
        stats.currentSize = m_cache.size();

        return stats;
    }

    void ChunkCache::ResetStats() {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_stats.Reset();
        m_stats.maxSize = m_config.maxSize;
        m_stats.currentSize = m_cache.size();
    }

    size_t ChunkCache::GetMemoryUsageBytes() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        size_t totalSize = sizeof(ChunkCache);
        totalSize += m_cache.bucket_count() * sizeof(void*);

        for (const auto& [pos, entry] : m_cache) {
            totalSize += sizeof(Math::ChunkPos) + sizeof(ChunkCacheEntry);
            if (entry.chunk) {
                totalSize += sizeof(Chunk);
                totalSize += entry.chunk->GetNonAirBlockCount() * sizeof(BlockID);
            }
        }

        return totalSize;
    }

    // === DEBUGGING ===

    ChunkCache::CacheState ChunkCache::GetDebugState() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        CacheState state;

        for (const auto& [pos, entry] : m_cache) {
            state.allChunks.push_back(pos);

            if (entry.isDirty) {
                state.dirtyChunks.push_back(pos);
            }
        }

        return state;
    }

    void ChunkCache::LogStats(const std::string& prefix) const {
        CacheStats stats = GetStats();

        Log::Info("%s Stats: Size=%zu/%zu (%.1f%%), Evictions=%zu, Dirty=%zu",
                 prefix.c_str(),
                 stats.currentSize, stats.maxSize, stats.GetUtilization() * 100.0f,
                 stats.totalEvictions, stats.dirtyChunks);
    }

    // === PRIVATE HELPERS ===

    void ChunkCache::UpdateAccess(Math::ChunkPos position) {
        auto accessIt = m_accessIterators.find(position);
        if (accessIt != m_accessIterators.end()) {
            // Move to front
            m_accessOrder.erase(accessIt->second);
            m_accessOrder.push_front(position);
            m_accessIterators[position] = m_accessOrder.begin();
        }
    }

    void ChunkCache::EvictLRU() {
        if (m_accessOrder.empty()) {
            return;
        }

        Math::ChunkPos lruPos = m_accessOrder.back();
        m_accessOrder.pop_back();
        m_accessIterators.erase(lruPos);

        auto it = m_cache.find(lruPos);
        if (it != m_cache.end()) {
            ChunkCacheEntry entry = std::move(it->second);
            m_cache.erase(it);
            m_stats.currentSize = m_cache.size();
            m_stats.totalEvictions++;

            EvictChunk(lruPos, entry);
        }
    }

    void ChunkCache::EvictChunk(Math::ChunkPos position, ChunkCacheEntry& entry) {
        bool wasDirty = entry.isDirty;

        // Save if dirty and we have a saver
        if (wasDirty && m_chunkSaver && entry.chunk) {
            auto future = m_chunkSaver->SaveChunkAsync(*entry.chunk);
        }

        // Call eviction callback
        if (m_evictionCallback) {
            m_evictionCallback(position, entry.chunk, wasDirty);
        }

        Log::Debug("Evicted chunk (%d, %d) %s",
                  position.x, position.z, wasDirty ? "(was dirty)" : "");
    }

    void ChunkCache::MoveFrom(ChunkCache&& other) noexcept {
        m_cache = std::move(other.m_cache);
        m_accessOrder = std::move(other.m_accessOrder);
        m_accessIterators = std::move(other.m_accessIterators);
        m_config = other.m_config;
        m_chunkSaver = std::move(other.m_chunkSaver);
        m_evictionCallback = std::move(other.m_evictionCallback);
        m_stats = other.m_stats;

        // Clear other's state
        other.m_cache.clear();
        other.m_accessOrder.clear();
        other.m_accessIterators.clear();
        other.m_chunkSaver.reset();
        other.m_evictionCallback = nullptr;
        other.m_stats.Reset();
    }

} // namespace Game