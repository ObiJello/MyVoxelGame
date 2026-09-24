// File: src/server/world/cache/ChunkCache.cpp
#include "ChunkCache.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/DeferredDispose.hpp"
#include <vector>

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
        // The map's destruction — every resident chunk with its sections
        // and block entities — was 25-30 ms of the leave-to-title gap. It
        // is pure deallocation, so the containers go to the background
        // disposer whole; nothing else references a chunk the cache alone
        // still holds (the provider drained and closed the saver first).
        std::unordered_map<Math::ChunkPos, ChunkCacheEntry, Math::ChunkPosHash> cache;
        std::list<Math::ChunkPos> order;
        std::unordered_map<Math::ChunkPos, std::list<Math::ChunkPos>::iterator, Math::ChunkPosHash> iterators;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            cache.swap(m_cache);
            order.swap(m_accessOrder);
            iterators.swap(m_accessIterators);
            m_stats.currentSize = 0;
            m_stats.dirtyChunks = 0;
        }
        Core::DeferredDispose::Run([cache = std::move(cache), order = std::move(order),
                                    iterators = std::move(iterators)]() mutable {
            iterators.clear();
            order.clear();
            cache.clear();
        });
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
        if (it == m_cache.end()) return nullptr;

        // The LRU splice — a hash lookup, a list erase, a push_front and a hash
        // store — only decides WHO GETS EVICTED, and nothing is evicted while
        // the cache is below capacity. Skipping it under the watermark leaves
        // exactly the same chunks resident and cuts the critical section down to
        // the find, which is what matters now that eight threads reach this
        // (MobManager's parallel TNT tick).
        //
        // NOT a shared_mutex. Tried that; it was measurably WORSE — 9,532
        // mutex-wait samples became 12,978. libc++ implements std::shared_mutex
        // on top of a std::mutex plus condition variables, so lock_shared still
        // serialises on an internal mutex and adds bookkeeping on top of it. A
        // shorter critical section beats a "cheaper" lock that is not cheap.
        if (m_cache.size() >= EvictionWatermark()) UpdateAccess(position);
        return it->second.chunk;
    }

    void ChunkCache::GrowMaxSize(size_t maxSize) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (maxSize <= m_config.maxSize) return;
        Log::Info("ChunkCache capacity %zu -> %zu chunks", m_config.maxSize, maxSize);
        m_config.maxSize = maxSize;
        m_stats.maxSize = maxSize;
    }

    size_t ChunkCache::MaxSize() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        return m_config.maxSize;
    }

    void ChunkCache::Put(Math::ChunkPos position, std::shared_ptr<Chunk> chunk) {
        if (!chunk) {
            Log::Warning("Attempted to put null chunk in cache at (%d, %d)", position.x, position.z);
            return;
        }

        std::vector<PendingEviction> evicted;
        {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        // Check if already exists
        auto it = m_cache.find(position);
        if (it != m_cache.end()) {
            Log::Debug("Chunk (%d, %d) already exists in cache, keeping existing", position.x, position.z);
            return;     // nothing evicted yet, so nothing to flush
        }

        // Add new entry — this changes what Get() answers, so any memo of it
        // must be invalidated. See ChunkCache::Generation.
        m_generation.fetch_add(1, std::memory_order_release);
        m_putGeneration.fetch_add(1, std::memory_order_release);

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
            EvictLRU(evicted);
        }
        }   // m_cacheMutex released

        // Serialising and compressing a chunk is far too much work to do while
        // holding the lock every block read needs.
        FlushEvictions(evicted);
    }

    bool ChunkCache::Remove(Math::ChunkPos position) {
        PROFILE_ZONE_N("ChunkCache.Remove");
        std::vector<PendingEviction> evicted;
        {
            PROFILE_ZONE_N("Remove.Locked");
            std::lock_guard<std::mutex> lock(m_cacheMutex);

            auto it = m_cache.find(position);
            if (it == m_cache.end()) {
                return false;
            }

            ChunkCacheEntry entry = std::move(it->second);
            m_cache.erase(it);
            m_generation.fetch_add(1, std::memory_order_release);
            m_removeGeneration.fetch_add(1, std::memory_order_release);

            // Remove from access order
            auto accessIt = m_accessIterators.find(position);
            if (accessIt != m_accessIterators.end()) {
                m_accessOrder.erase(accessIt->second);
                m_accessIterators.erase(accessIt);
            }

            m_stats.currentSize = m_cache.size();

            if (entry.isDirty) {
                evicted.push_back(PendingEviction{position, entry.chunk, true});
            }
        }   // m_cacheMutex released

        {
            PROFILE_ZONE_N("Remove.Flush");
            FlushEvictions(evicted);
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
                m_eagerSaveQueue.push_back(position);
                // A server that never has spare time never drains this; keep
                // it bounded by rebuilding it from what is actually dirty.
                if (m_eagerSaveQueue.size() > 4 * m_cache.size() + 1024) {
                    m_eagerSaveQueue.clear();
                    for (const auto& [pos, entry] : m_cache) {
                        if (entry.isDirty) m_eagerSaveQueue.push_back(pos);
                    }
                }
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

    void ChunkCache::SaveAllDirty(bool wait) {
        if (!m_chunkSaver) {
            // Expected on read-only worlds, where ChunkProvider deliberately
            // never builds a saver — not a warning, and this runs on every
            // eviction and in the destructor, so warning here would spam.
            Log::Debug("No chunk saver configured, skipping dirty-chunk save");
            return;
        }

        std::vector<std::shared_ptr<Chunk>> chunksToSave;
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
            if (!wait) PollInflightSaves();
            return;
        }

        Log::Debug("Saving %zu dirty chunks", chunksToSave.size());

        if (!wait) {
            // MC autosave: snapshot each chunk and hand it to the IO thread;
            // never wait for the disk. This used to block until every write
            // had landed, with the compression on this thread too — 13 s
            // for ~15,000 fresh chunks on the pause-save (2026-09-23).
            PollInflightSaves();
            size_t queued = 0;
            for (size_t i = 0; i < chunksToSave.size(); ++i) {
                if (QueueBackgroundSave(dirtyPositions[i], chunksToSave[i])) ++queued;
            }
            Log::Debug("Queued %zu of %zu dirty chunks for background save", queued, chunksToSave.size());
            return;
        }

        if (m_onSaved) {
            for (size_t i = 0; i < chunksToSave.size(); ++i) m_onSaved(dirtyPositions[i], *chunksToSave[i]);
        }

        // Save chunks — flush: no write cooldown (MC saveAllChunks(true)).
        // The cooldown answers "skipped" as success-with-0-bytes, and that
        // used to clear the dirty flag, so a chunk written by an autosave and
        // edited within the next 10 s lost the edit on quit.
        std::vector<std::future<ChunkSaveResult>> futures;
        futures.reserve(chunksToSave.size());
        for (const auto& chunk : chunksToSave) futures.push_back(m_chunkSaver->SaveChunkNowAsync(*chunk));

        size_t savedCount = 0;
        for (size_t i = 0; i < futures.size(); ++i) {
            const ChunkSaveResult result = futures[i].get();
            if (result.success) {
                ClearDirtyFlag(dirtyPositions[i]);
                savedCount++;
            } else {
                Log::Warning("Failed to save chunk (%d, %d): %s",
                           dirtyPositions[i].x, dirtyPositions[i].z,
                           result.errorMessage.c_str());
            }
        }

        Log::Debug("Successfully saved %zu out of %zu dirty chunks", savedCount, chunksToSave.size());
    }

    bool ChunkCache::QueueBackgroundSave(Math::ChunkPos pos, const std::shared_ptr<Chunk>& chunk) {
        if (!chunk) return false;
        ClearDirtyFlag(pos);
        if (m_onSaved) m_onSaved(pos, *chunk);
        std::future<ChunkSaveResult> future = m_chunkSaver->SaveChunkAsync(*chunk);
        if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            // Answered on the spot: the write cooldown skipped it (success,
            // 0 bytes) or serialising failed. Either way nothing will be
            // written — the chunk stays dirty for a later save.
            const ChunkSaveResult result = future.get();
            if (!result.success || result.bytesWritten == 0) {
                MarkDirty(pos);
                if (!result.success) {
                    Log::Warning("Failed to save chunk (%d, %d): %s", pos.x, pos.z,
                                 result.errorMessage.c_str());
                }
                return false;
            }
            return true;
        }
        std::lock_guard<std::mutex> lock(m_inflightMutex);
        m_inflightSaves.emplace_back(pos, std::move(future));
        return true;
    }

    void ChunkCache::PollInflightSaves() {
        std::vector<Math::ChunkPos> failed;
        {
            std::lock_guard<std::mutex> lock(m_inflightMutex);
            size_t keep = 0;
            for (size_t i = 0; i < m_inflightSaves.size(); ++i) {
                auto& entry = m_inflightSaves[i];
                if (entry.second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    if (keep != i) m_inflightSaves[keep] = std::move(entry);
                    ++keep;
                    continue;
                }
                const ChunkSaveResult result = entry.second.get();
                if (!result.success) {
                    Log::Warning("Failed to save chunk (%d, %d): %s", entry.first.x, entry.first.z,
                                 result.errorMessage.c_str());
                    failed.push_back(entry.first);
                }
            }
            m_inflightSaves.resize(keep);
        }
        for (const auto& pos : failed) MarkDirty(pos);   // retried by a later save
    }

    size_t ChunkCache::SaveSomeDirty(size_t maxChunks, std::chrono::steady_clock::time_point deadline) {
        if (!m_chunkSaver || maxChunks == 0) return 0;
        PollInflightSaves();
        static constexpr size_t kMaxInflightWrites = 128;   // MC activeChunkWrites < 128
        size_t queued = 0;
        size_t budget;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            budget = m_eagerSaveQueue.size();   // a chunk re-queued below waits for the next call
        }
        while (queued < maxChunks && budget-- > 0 && std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(m_inflightMutex);
                if (m_inflightSaves.size() >= kMaxInflightWrites) break;
            }
            Math::ChunkPos pos{0, 0};
            std::shared_ptr<Chunk> chunk;
            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                if (m_eagerSaveQueue.empty()) break;
                pos = m_eagerSaveQueue.front();
                m_eagerSaveQueue.pop_front();
                const auto it = m_cache.find(pos);
                if (it == m_cache.end() || !it->second.isDirty || !it->second.chunk) continue;   // stale entry
                chunk = it->second.chunk;
            }
            if (QueueBackgroundSave(pos, chunk)) ++queued;
        }
        return queued;
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
        m_eagerSaveQueue.clear();
        m_cache.clear();
        m_accessOrder.clear();
        m_accessIterators.clear();
        m_generation.fetch_add(1, std::memory_order_release);
        m_removeGeneration.fetch_add(1, std::memory_order_release);
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

    std::vector<Math::ChunkPos> ChunkCache::GetLoadedChunkPositions() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        std::vector<Math::ChunkPos> positions;
        positions.reserve(m_cache.size());

        for (const auto& [pos, entry] : m_cache) {
            positions.push_back(pos);
        }

        return positions;
    }

    std::vector<std::pair<Math::ChunkPos, std::shared_ptr<Chunk>>>
    ChunkCache::GetChunksWithBlockEntities() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        std::vector<std::pair<Math::ChunkPos, std::shared_ptr<Chunk>>> out;
        for (const auto& [pos, entry] : m_cache) {
            if (entry.chunk && !entry.chunk->GetAllBlockEntities().empty()) {
                out.emplace_back(pos, entry.chunk);
            }
        }
        return out;
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

    void ChunkCache::EvictLRU(std::vector<PendingEviction>& out) {
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
            m_generation.fetch_add(1, std::memory_order_release);
            m_removeGeneration.fetch_add(1, std::memory_order_release);
            m_stats.currentSize = m_cache.size();
            m_stats.totalEvictions++;

            // Bookkeeping only. The save and the callback happen once the
            // caller has released m_cacheMutex.
            out.push_back(PendingEviction{lruPos, entry.chunk, entry.isDirty});
        }
    }

    void ChunkCache::FlushEvictions(std::vector<PendingEviction>& pending) {
        for (auto& e : pending) {
            // Order preserved from the original EvictChunk: save first, then
            // notify. ChunkProvider::OnChunkEvicted is wired to that callback
            // and assumes the chunk has already been handed to the saver.
            if (e.wasDirty && m_chunkSaver && e.chunk) {
                // Deferred: the chunk has left the cache, so the saver may
                // serialise it on its own thread (see IChunkSaver).
                PROFILE_ZONE_N("Flush.SaveEvicted");
                (void)m_chunkSaver->SaveEvictedAsync(e.chunk);
            }
            if (m_evictionCallback) {
                PROFILE_ZONE_N("Flush.Callback");
                m_evictionCallback(e.pos, e.chunk, e.wasDirty);
            }
            Log::Debug("Evicted chunk (%d, %d) %s",
                       e.pos.x, e.pos.z, e.wasDirty ? "(was dirty)" : "");
        }
        { PROFILE_ZONE_N("Flush.Clear"); pending.clear(); }
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