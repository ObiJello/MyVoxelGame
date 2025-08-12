// File: src/server/world/status/ChunkStatusManager.cpp
#include "ChunkStatusManager.hpp"
#include "common/core/Log.hpp"
#include <algorithm>

namespace Server {

    ChunkStatusManager::ChunkStatusManager() {
        m_chunkInfo.reserve(4096);  // Pre-allocate for typical world
        m_networkCache.reserve(1024);  // Pre-allocate cache
    }

    ChunkStatusManager::~ChunkStatusManager() {
        Clear();
    }

    // === STATUS MANAGEMENT ===

    void ChunkStatusManager::SetChunkStatus(Game::Math::ChunkPos chunk, ChunkStatus status) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.status = status;
        UpdateTimestamp(info);
        
        Log::Debug("ChunkStatusManager: Chunk (%d, %d) status changed to %s",
                  chunk.x, chunk.z, ChunkStatusToString(status));
    }

    ChunkStatus ChunkStatusManager::GetChunkStatus(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.status;
        }
        
        return ChunkStatus::EMPTY;
    }

    bool ChunkStatusManager::IsChunkReady(Game::Math::ChunkPos chunk) const {
        return GetChunkStatus(chunk) == ChunkStatus::FULL;
    }

    bool ChunkStatusManager::IsLightReady(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.status >= ChunkStatus::LIGHT_READY &&
                   it->second.lightData.isComplete;
        }
        
        return false;
    }

    void ChunkStatusManager::MarkLightComplete(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.lightData.isComplete = true;
        
        if (info.status < ChunkStatus::LIGHT_READY) {
            info.status = ChunkStatus::LIGHT_READY;
        }
        
        UpdateTimestamp(info);
    }

    void ChunkStatusManager::MarkChunkReady(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.status = ChunkStatus::FULL;
        UpdateTimestamp(info);
        
        Log::Debug("ChunkStatusManager: Chunk (%d, %d) marked as FULL/ready", chunk.x, chunk.z);
    }

    // === BATCH OPERATIONS ===

    std::vector<Game::Math::ChunkPos> ChunkStatusManager::GetChunksWithStatus(ChunkStatus status) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        std::vector<Game::Math::ChunkPos> result;
        
        for (const auto& [chunk, info] : m_chunkInfo) {
            if (info.status == status) {
                result.push_back(chunk);
            }
        }
        
        return result;
    }

    std::vector<Game::Math::ChunkPos> ChunkStatusManager::GetReadyChunks() const {
        return GetChunksWithStatus(ChunkStatus::FULL);
    }

    std::vector<Game::Math::ChunkPos> ChunkStatusManager::GetChunksInProgress() const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        std::vector<Game::Math::ChunkPos> result;
        
        for (const auto& [chunk, info] : m_chunkInfo) {
            if (IsInProgressStatus(info.status)) {
                result.push_back(chunk);
            }
        }
        
        return result;
    }

    // === NETWORK DATA CACHING ===

    void ChunkStatusManager::CacheNetworkData(Game::Math::ChunkPos chunk, std::unique_ptr<ChunkNetData> data) {
        if (!data) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        // Convert unique_ptr to shared_ptr for shared access
        auto sharedData = std::shared_ptr<ChunkNetData>(data.release());
        m_networkCache[chunk] = sharedData;
        
        Log::Debug("ChunkStatusManager: Cached network data for chunk (%d, %d), size: %zu bytes",
                  chunk.x, chunk.z, sharedData->compressedSize);
    }

    std::shared_ptr<ChunkNetData> ChunkStatusManager::GetNetworkData(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        auto it = m_networkCache.find(chunk);
        if (it != m_networkCache.end()) {
            m_cacheHits++;
            it->second->refCount++;
            return it->second;
        }
        
        m_cacheMisses++;
        return nullptr;
    }

    void ChunkStatusManager::InvalidateNetworkData(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        m_networkCache.erase(chunk);
        
        Log::Debug("ChunkStatusManager: Invalidated network cache for chunk (%d, %d)", 
                  chunk.x, chunk.z);
    }

    bool ChunkStatusManager::HasNetworkData(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        return m_networkCache.count(chunk) > 0;
    }

    size_t ChunkStatusManager::GetCacheSize() const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        return m_networkCache.size();
    }

    void ChunkStatusManager::PruneCache(std::chrono::seconds maxAge) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = m_networkCache.begin(); it != m_networkCache.end();) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second->createdTime);
            
            // Remove if too old and not in use
            if (age >= maxAge && it->second->refCount == 0) {
                it = m_networkCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    // === SECTION TRACKING ===

    void ChunkStatusManager::SetSectionMask(Game::Math::ChunkPos chunk, uint32_t mask) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.sectionMask = mask;
        UpdateTimestamp(info);
    }

    uint32_t ChunkStatusManager::GetSectionMask(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.sectionMask;
        }
        
        return 0;
    }

    void ChunkStatusManager::SetHeightmapsAvailable(Game::Math::ChunkPos chunk, bool available) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.hasHeightmaps = available;
        UpdateTimestamp(info);
    }

    bool ChunkStatusManager::HasHeightmaps(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.hasHeightmaps;
        }
        
        return false;
    }

    void ChunkStatusManager::SetBiomesAvailable(Game::Math::ChunkPos chunk, bool available) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.hasBiomes = available;
        UpdateTimestamp(info);
    }

    bool ChunkStatusManager::HasBiomes(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.hasBiomes;
        }
        
        return false;
    }

    void ChunkStatusManager::SetHasBlockEntities(Game::Math::ChunkPos chunk, bool hasEntities) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.hasBlockEntities = hasEntities;
        UpdateTimestamp(info);
    }

    bool ChunkStatusManager::HasBlockEntities(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.hasBlockEntities;
        }
        
        return false;
    }

    // === LIGHT TRACKING ===

    void ChunkStatusManager::SetLightData(Game::Math::ChunkPos chunk, const LightData& data) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto& info = m_chunkInfo[chunk];
        info.lightData = data;
        UpdateTimestamp(info);
    }

    ChunkStatusManager::LightData ChunkStatusManager::GetLightData(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end()) {
            return it->second.lightData;
        }
        
        return LightData{};
    }

    // === GENERATION TRACKING ===

    void ChunkStatusManager::MarkGenerationStarted(Game::Math::ChunkPos chunk) {
        SetChunkStatus(chunk, ChunkStatus::GENERATING);
    }

    void ChunkStatusManager::MarkGenerationComplete(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end() && it->second.status == ChunkStatus::GENERATING) {
            it->second.status = ChunkStatus::FEATURES;
            UpdateTimestamp(it->second);
        }
    }

    bool ChunkStatusManager::IsGenerating(Game::Math::ChunkPos chunk) const {
        auto status = GetChunkStatus(chunk);
        return status == ChunkStatus::GENERATING || 
               status == ChunkStatus::STRUCTURE_GEN ||
               status == ChunkStatus::FEATURES;
    }

    void ChunkStatusManager::MarkLoadingStarted(Game::Math::ChunkPos chunk) {
        SetChunkStatus(chunk, ChunkStatus::LOADING);
    }

    void ChunkStatusManager::MarkLoadingComplete(Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        
        auto it = m_chunkInfo.find(chunk);
        if (it != m_chunkInfo.end() && it->second.status == ChunkStatus::LOADING) {
            it->second.status = ChunkStatus::LIGHT_GEN;
            UpdateTimestamp(it->second);
        }
    }

    bool ChunkStatusManager::IsLoading(Game::Math::ChunkPos chunk) const {
        return GetChunkStatus(chunk) == ChunkStatus::LOADING;
    }

    // === STATISTICS ===

    ChunkStatusManager::Stats ChunkStatusManager::GetStats() const {
        Stats stats;
        
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            
            stats.totalChunks = m_chunkInfo.size();
            
            for (const auto& [chunk, info] : m_chunkInfo) {
                if (info.status == ChunkStatus::FULL) {
                    stats.readyChunks++;
                } else if (info.status == ChunkStatus::LOADING) {
                    stats.loadingChunks++;
                } else if (IsGenerating(chunk)) {
                    stats.generatingChunks++;
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            stats.cachedChunks = m_networkCache.size();
            stats.cacheMemoryUsage = CalculateCacheMemoryUsage();
        }
        
        stats.cacheHits = m_cacheHits.load();
        stats.cacheMisses = m_cacheMisses.load();
        
        return stats;
    }

    void ChunkStatusManager::ResetStats() {
        m_cacheHits = 0;
        m_cacheMisses = 0;
    }

    // === CLEANUP ===

    void ChunkStatusManager::RemoveChunk(Game::Math::ChunkPos chunk) {
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_chunkInfo.erase(chunk);
        }
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_networkCache.erase(chunk);
        }
    }

    void ChunkStatusManager::Clear() {
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_chunkInfo.clear();
        }
        
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_networkCache.clear();
        }
        
        ResetStats();
    }

    // === INTERNAL HELPERS ===

    void ChunkStatusManager::UpdateTimestamp(ChunkInfo& info) {
        info.lastUpdate = std::chrono::steady_clock::now();
    }

    size_t ChunkStatusManager::CalculateCacheMemoryUsage() const {
        size_t usage = 0;
        
        for (const auto& [chunk, data] : m_networkCache) {
            if (data) {
                usage += sizeof(ChunkNetData);
                usage += data->compressedSize;
            }
        }
        
        return usage;
    }

    bool ChunkStatusManager::IsReadyStatus(ChunkStatus status) {
        return status == ChunkStatus::FULL;
    }

    bool ChunkStatusManager::IsInProgressStatus(ChunkStatus status) {
        return status == ChunkStatus::LOADING ||
               status == ChunkStatus::GENERATING ||
               status == ChunkStatus::STRUCTURE_GEN ||
               status == ChunkStatus::FEATURES ||
               status == ChunkStatus::LIGHT_GEN;
    }

    // === UTILITY FUNCTIONS ===

    const char* ChunkStatusToString(ChunkStatus status) {
        switch (status) {
            case ChunkStatus::EMPTY: return "EMPTY";
            case ChunkStatus::LOADING: return "LOADING";
            case ChunkStatus::GENERATING: return "GENERATING";
            case ChunkStatus::STRUCTURE_GEN: return "STRUCTURE_GEN";
            case ChunkStatus::FEATURES: return "FEATURES";
            case ChunkStatus::LIGHT_GEN: return "LIGHT_GEN";
            case ChunkStatus::LIGHT_READY: return "LIGHT_READY";
            case ChunkStatus::FULL: return "FULL";
            case ChunkStatus::INVALID: return "INVALID";
            default: return "UNKNOWN";
        }
    }

    std::unique_ptr<ChunkNetData> BuildChunkNetData(
        Game::Math::ChunkPos position,
        const Game::Chunk* chunk,
        int compressionLevel
    ) {
        if (!chunk) {
            return nullptr;
        }
        
        auto data = std::make_unique<ChunkNetData>();
        data->position = position;
        data->createdTime = std::chrono::steady_clock::now();
        
        // TODO: Implement actual chunk serialization and compression
        // This would involve:
        // 1. Building section data with palettes
        // 2. Encoding heightmaps
        // 3. Encoding biomes
        // 4. Encoding block entities
        // 5. Compressing with zlib
        
        // For now, return placeholder data
        data->compressedSize = 65536;  // Placeholder
        data->uncompressedSize = 131072;  // Placeholder
        data->compressedData = std::make_unique<uint8_t[]>(data->compressedSize);
        data->sectionMask = 0xFFFFFF;  // All 24 sections
        data->hasHeightmaps = true;
        data->hasBiomes = true;
        data->hasBlockEntities = false;
        
        return data;
    }

} // namespace Server