// File: src/server/world/status/ChunkStatusManager.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/network/PacketTypes.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>

namespace Game {
    class Chunk;
}

namespace Server {

    // Chunk generation/loading status
    enum class ChunkStatus {
        EMPTY,           // Not loaded
        LOADING,         // Being loaded from disk
        GENERATING,      // Being generated
        STRUCTURE_GEN,   // Structure generation
        FEATURES,        // Feature generation
        LIGHT_GEN,       // Light calculation
        LIGHT_READY,     // Light complete
        FULL,           // Ready to send to client
        INVALID         // Error state
    };

    // Cached network-ready chunk data
    struct ChunkNetData {
        Game::Math::ChunkPos position;
        std::unique_ptr<uint8_t[]> compressedData;
        size_t compressedSize = 0;
        size_t uncompressedSize = 0;
        uint32_t sectionMask = 0;  // Bitmask of non-empty sections
        bool hasHeightmaps = false;
        bool hasBiomes = false;
        bool hasBlockEntities = false;
        std::chrono::steady_clock::time_point createdTime;
        std::atomic<int> refCount{0};  // For shared access
        
        ChunkNetData() = default;
        ChunkNetData(const ChunkNetData&) = delete;
        ChunkNetData& operator=(const ChunkNetData&) = delete;
        
        // Custom move constructor to handle atomic
        ChunkNetData(ChunkNetData&& other) noexcept
            : position(other.position)
            , compressedData(std::move(other.compressedData))
            , compressedSize(other.compressedSize)
            , uncompressedSize(other.uncompressedSize)
            , sectionMask(other.sectionMask)
            , hasHeightmaps(other.hasHeightmaps)
            , hasBiomes(other.hasBiomes)
            , hasBlockEntities(other.hasBlockEntities)
            , createdTime(other.createdTime)
            , refCount(other.refCount.load())
        {}
        
        // Custom move assignment to handle atomic
        ChunkNetData& operator=(ChunkNetData&& other) noexcept {
            if (this != &other) {
                position = other.position;
                compressedData = std::move(other.compressedData);
                compressedSize = other.compressedSize;
                uncompressedSize = other.uncompressedSize;
                sectionMask = other.sectionMask;
                hasHeightmaps = other.hasHeightmaps;
                hasBiomes = other.hasBiomes;
                hasBlockEntities = other.hasBlockEntities;
                createdTime = other.createdTime;
                refCount.store(other.refCount.load());
            }
            return *this;
        }
    };

    // Manages chunk status and cached network data
    class ChunkStatusManager {
    public:
        ChunkStatusManager();
        ~ChunkStatusManager();

        // === STATUS MANAGEMENT ===

        // Set chunk status
        void SetChunkStatus(Game::Math::ChunkPos chunk, ChunkStatus status);
        
        // Get chunk status
        ChunkStatus GetChunkStatus(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk is ready to send
        bool IsChunkReady(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk has valid light
        bool IsLightReady(Game::Math::ChunkPos chunk) const;
        
        // Mark chunk as having complete light
        void MarkLightComplete(Game::Math::ChunkPos chunk);
        
        // Mark chunk as fully ready
        void MarkChunkReady(Game::Math::ChunkPos chunk);

        // === BATCH OPERATIONS ===

        // Get all chunks with specific status
        std::vector<Game::Math::ChunkPos> GetChunksWithStatus(ChunkStatus status) const;
        
        // Get all ready chunks
        std::vector<Game::Math::ChunkPos> GetReadyChunks() const;
        
        // Get chunks in progress (loading/generating)
        std::vector<Game::Math::ChunkPos> GetChunksInProgress() const;

        // === NETWORK DATA CACHING ===

        // Store pre-built network data for a chunk
        void CacheNetworkData(Game::Math::ChunkPos chunk, std::unique_ptr<ChunkNetData> data);
        
        // Get cached network data (increases ref count)
        std::shared_ptr<ChunkNetData> GetNetworkData(Game::Math::ChunkPos chunk);
        
        // Invalidate cached network data
        void InvalidateNetworkData(Game::Math::ChunkPos chunk);
        
        // Check if network data is cached
        bool HasNetworkData(Game::Math::ChunkPos chunk) const;
        
        // Get cache size
        size_t GetCacheSize() const;
        
        // Clear old cached data
        void PruneCache(std::chrono::seconds maxAge);

        // === SECTION TRACKING ===

        // Track which sections have data
        void SetSectionMask(Game::Math::ChunkPos chunk, uint32_t mask);
        uint32_t GetSectionMask(Game::Math::ChunkPos chunk) const;
        
        // Track heightmap availability
        void SetHeightmapsAvailable(Game::Math::ChunkPos chunk, bool available);
        bool HasHeightmaps(Game::Math::ChunkPos chunk) const;
        
        // Track biome availability
        void SetBiomesAvailable(Game::Math::ChunkPos chunk, bool available);
        bool HasBiomes(Game::Math::ChunkPos chunk) const;
        
        // Track block entities
        void SetHasBlockEntities(Game::Math::ChunkPos chunk, bool hasEntities);
        bool HasBlockEntities(Game::Math::ChunkPos chunk) const;

        // === LIGHT TRACKING ===

        // Track light data per section
        struct LightData {
            uint32_t skyLightMask = 0;     // Sections with sky light
            uint32_t blockLightMask = 0;   // Sections with block light
            uint32_t emptySkyLightMask = 0;  // Sections with no sky light
            uint32_t emptyBlockLightMask = 0; // Sections with no block light
            bool isComplete = false;
        };
        
        void SetLightData(Game::Math::ChunkPos chunk, const LightData& data);
        LightData GetLightData(Game::Math::ChunkPos chunk) const;
        
        // === GENERATION TRACKING ===

        // Track generation tasks
        void MarkGenerationStarted(Game::Math::ChunkPos chunk);
        void MarkGenerationComplete(Game::Math::ChunkPos chunk);
        bool IsGenerating(Game::Math::ChunkPos chunk) const;
        
        // Track loading tasks
        void MarkLoadingStarted(Game::Math::ChunkPos chunk);
        void MarkLoadingComplete(Game::Math::ChunkPos chunk);
        bool IsLoading(Game::Math::ChunkPos chunk) const;

        // === STATISTICS ===

        struct Stats {
            size_t totalChunks = 0;
            size_t readyChunks = 0;
            size_t loadingChunks = 0;
            size_t generatingChunks = 0;
            size_t cachedChunks = 0;
            size_t cacheMemoryUsage = 0;
            size_t cacheHits = 0;
            size_t cacheMisses = 0;
        };
        
        Stats GetStats() const;
        void ResetStats();

        // === CLEANUP ===

        // Remove chunk status and data
        void RemoveChunk(Game::Math::ChunkPos chunk);
        
        // Clear all data
        void Clear();

    private:
        // Chunk status information
        struct ChunkInfo {
            ChunkStatus status = ChunkStatus::EMPTY;
            uint32_t sectionMask = 0;
            bool hasHeightmaps = false;
            bool hasBiomes = false;
            bool hasBlockEntities = false;
            LightData lightData;
            std::chrono::steady_clock::time_point lastUpdate;
        };

        // Status tracking
        std::unordered_map<Game::Math::ChunkPos, ChunkInfo, Game::Math::ChunkPosHash> m_chunkInfo;
        mutable std::mutex m_statusMutex;
        
        // Network data cache
        std::unordered_map<Game::Math::ChunkPos, std::shared_ptr<ChunkNetData>, 
                          Game::Math::ChunkPosHash> m_networkCache;
        mutable std::mutex m_cacheMutex;
        
        // Statistics
        mutable std::atomic<size_t> m_cacheHits{0};
        mutable std::atomic<size_t> m_cacheMisses{0};
        
        // === INTERNAL HELPERS ===
        
        // Update chunk info timestamp
        void UpdateTimestamp(ChunkInfo& info);
        
        // Calculate cache memory usage
        size_t CalculateCacheMemoryUsage() const;
        
        // Check if status represents readiness
        static bool IsReadyStatus(ChunkStatus status);
        
        // Check if status represents in-progress work
        static bool IsInProgressStatus(ChunkStatus status);
    };

    // === UTILITY FUNCTIONS ===

    // Convert status to string for logging
    const char* ChunkStatusToString(ChunkStatus status);
    
    // Build network data from chunk
    std::unique_ptr<ChunkNetData> BuildChunkNetData(
        Game::Math::ChunkPos position,
        const class Chunk* chunk,
        int compressionLevel = 6
    );

} // namespace Server