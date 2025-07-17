// File: src/engine/world/loading/MinecraftChunkLoaderImpl.hpp
#pragma once

#include "../interfaces/IChunkLoader.hpp"
#include "../../../game/WorldCoordinates.hpp"
#include "../../../core/Log.hpp"
#include "../../../core/JobSystem.hpp"
#include "../SectionDataUnpacker.hpp"
#include "../NBTParser.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <future>
#include <atomic>
#include <glm/glm.hpp>

#include "world/RegionFile.hpp"
#include "world/RegionFileCache.hpp"


namespace Game {

    // Forward declarations
    class Chunk;
    class IChunkGenerator;
    namespace World { class RegionFileCache; }
    class NBTTag;
    using NBTTagPtr = std::shared_ptr<NBTTag>;

    // Cache entry for loaded chunk data
    struct ChunkDataCacheEntry {
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point loadTime;
        size_t accessCount = 0;

        ChunkDataCacheEntry(std::vector<uint8_t> chunkData)
            : data(std::move(chunkData))
            , loadTime(std::chrono::steady_clock::now()) {}

        float GetAgeSeconds() const {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - loadTime);
            return duration.count() / 1000.0f;
        }
    };

    // Configuration for the Minecraft chunk loader
    struct MinecraftLoaderConfig {
        std::string worldPath;                    // Path to Minecraft world save
        bool enableCaching = true;                // Enable chunk data caching
        size_t maxCacheSize = 256;               // Maximum cached chunk data entries
        float cacheMaxAgeSeconds = 300.0f;       // Maximum age before cache eviction
        bool enableFallbackGeneration = true;    // Generate chunks if not found in files
        bool enableAsyncLoading = true;          // Enable background loading
        int maxAsyncTasks = 4;                   // Maximum concurrent async loads
        float loadTimeoutSeconds = 30.0f;        // Timeout for chunk loading
        bool enableCompression = true;           // Use compressed region data
        bool strictValidation = false;           // Strict chunk validation
    };

    // Minecraft region file chunk loader implementation
    class MinecraftChunkLoaderImpl : public IChunkLoader {
    public:
        explicit MinecraftChunkLoaderImpl(const MinecraftLoaderConfig& config = MinecraftLoaderConfig{});
        ~MinecraftChunkLoaderImpl() override;

        // === CORE LOADING INTERFACE ===
        ChunkLoadResult LoadChunk(Math::ChunkPos position) override;
        std::future<ChunkLoadResult> LoadChunkAsync(Math::ChunkPos position) override;
        bool ChunkExists(Math::ChunkPos position) const override;
        float EstimateLoadTime(Math::ChunkPos position) const override;

        // === BATCH OPERATIONS ===
        std::vector<ChunkLoadResult> LoadChunks(const std::vector<Math::ChunkPos>& positions) override;
        std::vector<bool> ChunksExist(const std::vector<Math::ChunkPos>& positions) const override;

        // === CONFIGURATION ===
        void SetSource(const std::string& sourcePath) override;
        std::string GetSource() const override;
        void SetCachingEnabled(bool enabled) override;
        bool IsCachingEnabled() const override;

        // === LIFECYCLE ===
        bool Initialize() override;
        void Shutdown() override;
        bool IsReady() const override;

        // === STATISTICS ===
        LoaderStats GetStats() const override;
        void ResetStats() override;

        // === PRIORITY AND HINTS ===
        void SetDefaultPriority(LoadPriority priority) override;
        LoadPriority GetDefaultPriority() const override;
        ChunkLoadResult LoadChunkWithPriority(Math::ChunkPos position, LoadPriority priority) override;

        // === VALIDATION ===
        bool ValidateChunk(const Chunk& chunk) const override;

        // === ERROR HANDLING ===
        std::string GetLastError() const override;
        void ClearErrors() override;

        // === MINECRAFT-SPECIFIC FEATURES ===

        // Configuration management
        void SetConfig(const MinecraftLoaderConfig& config);
        MinecraftLoaderConfig GetConfig() const;

        // Set fallback generator for chunks not found in region files
        void SetFallbackGenerator(std::shared_ptr<IChunkGenerator> generator);
        std::shared_ptr<IChunkGenerator> GetFallbackGenerator() const;

        // Region file management
        bool IsRegionFileLoaded(int regionX, int regionZ) const;
        void PreloadRegionFile(int regionX, int regionZ);
        void UnloadRegionFile(int regionX, int regionZ);
        size_t GetLoadedRegionCount() const;

        // Cache management
        size_t GetCacheSize() const;
        void ClearCache();
        void EvictOldCacheEntries(float maxAgeSeconds = 300.0f);

        // Performance monitoring
        float GetAverageLoadTime() const;
        size_t GetRegionFileCacheHits() const;
        size_t GetRegionFileCacheMisses() const;
        float GetRegionFileCacheHitRate() const;

        // Debugging and diagnostics
        struct DiagnosticInfo {
            size_t regionFilesLoaded = 0;
            size_t chunksInCache = 0;
            size_t totalChunksLoaded = 0;
            size_t fallbackGenerated = 0;
            float averageLoadTimeMs = 0.0f;
            size_t memoryUsageBytes = 0;
        };

        DiagnosticInfo GetDiagnosticInfo() const;
        void LogDiagnostics(const std::string& prefix = "MinecraftLoader") const;

    protected:
        void UpdateStats(const ChunkLoadResult& result) override;

    private:
        // Configuration
        MinecraftLoaderConfig m_config;
        mutable std::shared_mutex m_configMutex;

        // Dependencies
        std::shared_ptr<IChunkGenerator> m_fallbackGenerator;

        // Region file management
        std::unique_ptr<::World::RegionFileCache> m_regionCache;

        // Chunk data caching
        mutable std::shared_mutex m_cacheMutex;
        std::unordered_map<Math::ChunkPos, ChunkDataCacheEntry, Math::ChunkPosHash> m_chunkDataCache;

        // Statistics and performance monitoring
        mutable std::mutex m_statsMutex;
        LoaderStats m_stats;
        std::atomic<bool> m_initialized{false};
        LoadPriority m_defaultPriority = LoadPriority::Normal;

        // Error handling
        mutable std::mutex m_errorMutex;
        std::string m_lastError;

        // Async loading support
        std::atomic<bool> m_shutdownRequested{false};

        // === CORE LOADING IMPLEMENTATION ===

        // Main chunk loading logic
        ChunkLoadResult LoadChunkInternal(Math::ChunkPos position, LoadPriority priority);

        // Load from Minecraft region files
        ChunkLoadResult LoadFromRegionFile(Math::ChunkPos position);

        // Load using fallback generator
        ChunkLoadResult LoadFromGenerator(Math::ChunkPos position);

        // === REGION FILE UTILITIES ===

        // Convert chunk coordinates to region coordinates
        void ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ,
                          int& localX, int& localZ) const;

        // Get region file path
        std::string GetRegionFilePath(int regionX, int regionZ) const;

        // Check if region file exists
        bool RegionFileExists(int regionX, int regionZ) const;

        // === NBT DATA PROCESSING ===

        // Load chunk NBT from region file
        NBTTagPtr LoadChunkNBT(Math::ChunkPos position);

        // Convert NBT data to chunk
        std::shared_ptr<Chunk> NBTToChunk(const ::World::NBTTagPtr& nbtData, Math::ChunkPos position);

        // Validate NBT chunk data
        bool ValidateChunkNBT(const ::World::NBTTagPtr& nbtData) const;

        // === CACHING IMPLEMENTATION ===

        // Get cached chunk data
        bool GetCachedChunkData(Math::ChunkPos position, std::vector<uint8_t>& data);

        // Store chunk data in cache
        void CacheChunkData(Math::ChunkPos position, const std::vector<uint8_t>& data);

        // Remove from cache
        void RemoveFromCache(Math::ChunkPos position);

        // Cache maintenance
        void PerformCacheMaintenance();

        // === VALIDATION ===

        // Validate world path
        bool ValidateWorldPath(const std::string& path) const;

        // Validate chunk position
        bool ValidateChunkPosition(Math::ChunkPos position) const;

        // Validate loaded chunk data
        bool ValidateChunkData(const Chunk& chunk) const;

        // === ERROR HANDLING ===

        void SetLastError(const std::string& error) const;
        void LogError(const std::string& operation, const std::string& error) const;

        // === STATISTICS ===

        void UpdateLoadStats(const ChunkLoadResult& result, bool fromCache, bool fromGenerator);
        void UpdateCacheStats(bool hit);

        // === UTILITY METHODS ===

        // Calculate memory usage
        size_t CalculateMemoryUsage() const;

        // Get current timestamp
        std::chrono::steady_clock::time_point GetCurrentTime() const;

        // Format chunk position for logging
        std::string FormatChunkPos(Math::ChunkPos position) const;

        // Performance timing utilities
        class LoadTimer {
        public:
            LoadTimer();
            float GetElapsedMs() const;

        private:
            std::chrono::steady_clock::time_point m_start;
        };
    };

    // === RAII REGION FILE LOCK ===

    // RAII wrapper for region file access
    class RegionFileLock {
    public:
        RegionFileLock(MinecraftChunkLoaderImpl* loader, int regionX, int regionZ);
        ~RegionFileLock();

        bool IsValid() const { return m_valid; }
        ::World::RegionFile* GetRegionFile() const { return m_regionFile; }

    private:
        MinecraftChunkLoaderImpl* m_loader;
        ::World::RegionFile* m_regionFile;
        bool m_valid = false;
    };

    // === UTILITY FUNCTIONS ===

    // Factory function for creating Minecraft chunk loaders
    std::unique_ptr<IChunkLoader> CreateMinecraftChunkLoader(const MinecraftLoaderConfig& config = MinecraftLoaderConfig{});

    // Helper for converting Minecraft world coordinates
    class MinecraftCoordinateHelper {
    public:
        static Math::ChunkPos WorldToChunkPos(int worldX, int worldZ);
        static glm::ivec3 ChunkToWorldPos(Math::ChunkPos chunkPos, int localX, int localY, int localZ);
        static bool IsValidMinecraftY(int worldY);
        static int ClampMinecraftY(int worldY);
    };

} // namespace Game
