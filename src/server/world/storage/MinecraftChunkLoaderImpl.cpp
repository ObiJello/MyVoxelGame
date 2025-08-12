// File:src/server/world/storage/MinecraftChunkLoaderImpl.cpp
#include "MinecraftChunkLoaderImpl.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/gen/IChunkGenerator.hpp"
#include "RegionFileCache.hpp"
#include "RegionDumper.hpp"
#include "common/core/JobSystem.hpp"
#include "platform/GameDirectory.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace Game {

    // === LOAD TIMER IMPLEMENTATION ===

    MinecraftChunkLoaderImpl::LoadTimer::LoadTimer()
        : m_start(std::chrono::high_resolution_clock::now()) {}

    float MinecraftChunkLoaderImpl::LoadTimer::GetElapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_start);
        return duration.count() / 1000.0f;
    }

    // === REGION FILE LOCK IMPLEMENTATION ===

    RegionFileLock::RegionFileLock(MinecraftChunkLoaderImpl* loader, int regionX, int regionZ)
        : m_loader(loader), m_regionFile(nullptr), m_valid(false) {

        if (!m_loader) {
            return;
        }

        try {
            std::string regionPath = m_loader->GetRegionFilePath(regionX, regionZ);
            if (m_loader->RegionFileExists(regionX, regionZ)) {
                // FIX: Use singleton RegionFileCache
                auto regionFilePtr = ::World::RegionFileCache::Instance().GetRegionFile(regionPath);
                m_regionFile = regionFilePtr.get();
                m_valid = (m_regionFile != nullptr);
            }
        } catch (const std::exception& e) {
            m_loader->LogError("RegionFileLock", e.what());
            m_valid = false;
        }
    }

    RegionFileLock::~RegionFileLock() {
        // RAII cleanup - region cache handles file lifecycle
    }

    // === MINECRAFT CHUNK LOADER IMPLEMENTATION ===

    MinecraftChunkLoaderImpl::MinecraftChunkLoaderImpl(const MinecraftLoaderConfig& config)
        : m_config(config) {

        // FIX: RegionFileCache is a singleton, use Instance() instead of make_unique
        // m_regionCache will store a reference to the singleton

        Log::Debug("MinecraftChunkLoaderImpl created for world: %s", config.worldPath.c_str());
    }

    MinecraftChunkLoaderImpl::~MinecraftChunkLoaderImpl() {
        Shutdown();
    }

    // === CORE LOADING INTERFACE ===

    ChunkLoadResult MinecraftChunkLoaderImpl::LoadChunk(Math::ChunkPos position) {
        return LoadChunkWithPriority(position, m_defaultPriority);
    }

    std::future<ChunkLoadResult> MinecraftChunkLoaderImpl::LoadChunkAsync(Math::ChunkPos position) {
        if (!m_initialized) {
            std::promise<ChunkLoadResult> promise;
            promise.set_value(ChunkLoadResult::Failure("Loader not initialized"));
            return promise.get_future();
        }

        auto promise = std::make_shared<std::promise<ChunkLoadResult>>();
        auto future = promise->get_future();

        // Submit to job system
        JobSystem::g_ThreadPool.Enqueue([this, position, promise]() {
            ChunkLoadResult result = LoadChunk(position);
            promise->set_value(std::move(result));
        });

        return future;
    }

    bool MinecraftChunkLoaderImpl::ChunkExists(Math::ChunkPos position) const {
        if (!m_initialized) {
            return false;
        }

        int regionX, regionZ, localX, localZ;
        ChunkToRegion(position, regionX, regionZ, localX, localZ);

        if (!RegionFileExists(regionX, regionZ)) {
            return false;
        }

        try {
            RegionFileLock lock(const_cast<MinecraftChunkLoaderImpl*>(this), regionX, regionZ);
            if (!lock.IsValid()) {
                return false;
            }

            // FIX: Use correct method name and check if region file has chunk data
            auto location = lock.GetRegionFile()->GetLocation(localX, localZ);
            return !location.isEmpty();
        } catch (const std::exception& e) {
            LogError("ChunkExists", e.what());
            return false;
        }
    }

    float MinecraftChunkLoaderImpl::EstimateLoadTime(Math::ChunkPos position) const {
        if (!m_initialized) {
            return 1000.0f; // High estimate for uninitialized loader
        }

        // Check if in cache
        std::vector<uint8_t> cachedData;
        if (m_config.enableCaching && GetCachedChunkData(position, cachedData)) {
            return 0.1f; // Very fast cache lookup
        }

        // Check if region file is loaded
        int regionX, regionZ, localX, localZ;
        ChunkToRegion(position, regionX, regionZ, localX, localZ);

        if (IsRegionFileLoaded(regionX, regionZ)) {
            return 5.0f; // Moderate time for disk read from loaded region
        }

        return 50.0f; // Slower time if region file needs to be loaded
    }

    // === BATCH OPERATIONS ===

    std::vector<ChunkLoadResult> MinecraftChunkLoaderImpl::LoadChunks(const std::vector<Math::ChunkPos>& positions) {
        std::vector<ChunkLoadResult> results;
        results.reserve(positions.size());

        // FIX: Create custom hash function for std::pair<int, int>
        struct PairHash {
            std::size_t operator()(const std::pair<int, int>& p) const {
                return std::hash<int>{}(p.first) ^ (std::hash<int>{}(p.second) << 1);
            }
        };

        // Group by region files for efficient batch loading
        std::unordered_map<std::pair<int, int>, std::vector<Math::ChunkPos>, PairHash> regionGroups;

        for (const auto& pos : positions) {
            int regionX, regionZ, localX, localZ;
            ChunkToRegion(pos, regionX, regionZ, localX, localZ);
            regionGroups[{regionX, regionZ}].push_back(pos);
        }

        // Load each region group
        for (const auto& [regionCoords, chunks] : regionGroups) {
            for (const auto& chunkPos : chunks) {
                results.push_back(LoadChunk(chunkPos));
            }
        }

        return results;
    }

    std::vector<bool> MinecraftChunkLoaderImpl::ChunksExist(const std::vector<Math::ChunkPos>& positions) const {
        std::vector<bool> results;
        results.reserve(positions.size());

        for (const auto& pos : positions) {
            results.push_back(ChunkExists(pos));
        }

        return results;
    }

    // === CONFIGURATION ===

    void MinecraftChunkLoaderImpl::SetSource(const std::string& sourcePath) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);

        if (m_initialized) {
            Log::Warning("Cannot change source path while loader is initialized");
            return;
        }
        m_config.worldPath = sourcePath;
        Log::Info("MinecraftChunkLoader source set to: %s", sourcePath.c_str());
    }

    std::string MinecraftChunkLoaderImpl::GetSource() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.worldPath;
    }

    void MinecraftChunkLoaderImpl::SetCachingEnabled(bool enabled) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.enableCaching = enabled;

        if (!enabled) {
            ClearCache();
        }
    }

    bool MinecraftChunkLoaderImpl::IsCachingEnabled() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.enableCaching;
    }

    // === LIFECYCLE ===

    bool MinecraftChunkLoaderImpl::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing MinecraftChunkLoaderImpl...");

        std::lock_guard<std::shared_mutex> lock(m_configMutex);

        if (!ValidateWorldPath(m_config.worldPath)) {
            SetLastError("Invalid world path: " + m_config.worldPath);
            return false;
        }

        // FIX: RegionFileCache is a singleton, don't create new instance
        // Just verify it's accessible
        try {
            auto& cache = ::World::RegionFileCache::Instance();
            // Cache is now accessible
        } catch (const std::exception& e) {
            SetLastError("Failed to access RegionFileCache: " + std::string(e.what()));
            return false;
        }

        try {
            // Set region cache base path
            std::filesystem::path worldPath(m_config.worldPath);
            std::filesystem::path regionPath = worldPath / "region";

            if (!std::filesystem::exists(regionPath)) {
                SetLastError("Region directory not found: " + regionPath.string());
                return false;
            }

            // Reset statistics
            {
                std::lock_guard<std::mutex> statsLock(m_statsMutex);
                m_stats.Reset();
            }

            ClearErrors();
            m_initialized = true;

            Log::Info("MinecraftChunkLoaderImpl initialized successfully");
            return true;

        } catch (const std::exception& e) {
            SetLastError("Initialization failed: " + std::string(e.what()));
            Log::Error("MinecraftChunkLoaderImpl initialization failed: %s", e.what());
            return false;
        }

    }

    void MinecraftChunkLoaderImpl::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down MinecraftChunkLoaderImpl...");

        // Clear caches
        ClearCache();

        // FIX: Don't shutdown singleton RegionFileCache, just clear it
        ::World::RegionFileCache::Instance().Clear();

        // Log final statistics
        LogDiagnostics("Shutdown");

        m_initialized = false;
        Log::Info("MinecraftChunkLoaderImpl shutdown complete");
    }

    bool MinecraftChunkLoaderImpl::IsReady() const {
        return m_initialized;
    }

    // === STATISTICS ===

    MinecraftChunkLoaderImpl::LoaderStats MinecraftChunkLoaderImpl::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    void MinecraftChunkLoaderImpl::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();
    }

    // === PRIORITY AND HINTS ===

    void MinecraftChunkLoaderImpl::SetDefaultPriority(LoadPriority priority) {
        m_defaultPriority = priority;
    }

    MinecraftChunkLoaderImpl::LoadPriority MinecraftChunkLoaderImpl::GetDefaultPriority() const {
        return m_defaultPriority;
    }

    ChunkLoadResult MinecraftChunkLoaderImpl::LoadChunkWithPriority(Math::ChunkPos position, LoadPriority priority) {
        if (!m_initialized) {
            return ChunkLoadResult::Failure("Loader not initialized");
        }

        LoadTimer timer;
        ChunkLoadResult result = LoadChunkInternal(position, priority);
        result.loadTimeMs = timer.GetElapsedMs();

        UpdateStats(result);
        return result;
    }

    // === VALIDATION ===

    bool MinecraftChunkLoaderImpl::ValidateChunk(const Chunk& chunk) const {
        // Use base validation plus Minecraft-specific checks
        if (!IChunkLoader::ValidateChunk(chunk)) {
            return false;
        }

        // Check that chunk has reasonable structure
        if (chunk.GetSectionCount() == 0) {
            return false;
        }

        // Check for bedrock layer at bottom (Minecraft-specific)
        bool hasBedrockLayer = false;
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                BlockID block = chunk.GetBlock(x, Math::WorldCoordinates::MIN_WORLD_Y, z);
                if (block == BlockID::Bedrock) {
                    hasBedrockLayer = true;
                    break;
                }
            }
            if (hasBedrockLayer) break;
        }

        return hasBedrockLayer;
    }

    // === ERROR HANDLING ===

    std::string MinecraftChunkLoaderImpl::GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    void MinecraftChunkLoaderImpl::ClearErrors() {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError.clear();
    }

    // === MINECRAFT-SPECIFIC FEATURES ===

    void MinecraftChunkLoaderImpl::SetConfig(const MinecraftLoaderConfig& config) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);

        if (m_initialized && config.worldPath != m_config.worldPath) {
            Log::Warning("Cannot change world path while loader is initialized");
            return;
        }

        m_config = config;
        Log::Debug("MinecraftChunkLoader config updated");
    }

    MinecraftLoaderConfig MinecraftChunkLoaderImpl::GetConfig() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config;
    }

    void MinecraftChunkLoaderImpl::SetFallbackGenerator(std::shared_ptr<IChunkGenerator> generator) {
        m_fallbackGenerator = generator;
    }

    std::shared_ptr<IChunkGenerator> MinecraftChunkLoaderImpl::GetFallbackGenerator() const {
        return m_fallbackGenerator;
    }

    bool MinecraftChunkLoaderImpl::IsRegionFileLoaded(int regionX, int regionZ) const {
        try {
            std::string regionPath = GetRegionFilePath(regionX, regionZ);
            // FIX: Use singleton RegionFileCache
            auto regionFile = ::World::RegionFileCache::Instance().GetRegionFile(regionPath);
            return regionFile != nullptr;
        } catch (const std::exception& e) {
            return false;
        }
    }

    void MinecraftChunkLoaderImpl::PreloadRegionFile(int regionX, int regionZ) {
        if (!m_initialized) {
            return;
        }

        try {
            std::string regionPath = GetRegionFilePath(regionX, regionZ);
            if (RegionFileExists(regionX, regionZ)) {
                // FIX: Use singleton RegionFileCache
                ::World::RegionFileCache::Instance().GetRegionFile(regionPath);
                Log::Debug("Preloaded region file (%d, %d)", regionX, regionZ);
            }
        } catch (const std::exception& e) {
            LogError("PreloadRegionFile", e.what());
        }
    }

    void MinecraftChunkLoaderImpl::UnloadRegionFile(int regionX, int regionZ) {
        // FIX: Clear the entire singleton cache since we can't unload individual files
        ::World::RegionFileCache::Instance().Clear();
        Log::Debug("Cleared region file cache (includes region (%d, %d))", regionX, regionZ);
    }

    size_t MinecraftChunkLoaderImpl::GetLoadedRegionCount() const {
        return ::World::RegionFileCache::Instance().GetCacheSize();
    }

    size_t MinecraftChunkLoaderImpl::GetCacheSize() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
        return m_chunkDataCache.size();
    }

    void MinecraftChunkLoaderImpl::ClearCache() {
        std::lock_guard<std::shared_mutex> lock(m_cacheMutex);
        m_chunkDataCache.clear();
        Log::Debug("Cleared chunk data cache");
    }

    void MinecraftChunkLoaderImpl::EvictOldCacheEntries(float maxAgeSeconds) {
        std::lock_guard<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_chunkDataCache.begin();
        while (it != m_chunkDataCache.end()) {
            if (it->second.GetAgeSeconds() > maxAgeSeconds) {
                it = m_chunkDataCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    float MinecraftChunkLoaderImpl::GetAverageLoadTime() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats.averageLoadTimeMs;
    }

    size_t MinecraftChunkLoaderImpl::GetRegionFileCacheHits() const {
        // FIX: RegionFileCache doesn't have GetCacheHits method, return 0 for now
        return 0;
    }

    size_t MinecraftChunkLoaderImpl::GetRegionFileCacheMisses() const {
        // FIX: RegionFileCache doesn't have GetCacheMisses method, return 0 for now
        return 0;
    }

    float MinecraftChunkLoaderImpl::GetRegionFileCacheHitRate() const {
        size_t hits = GetRegionFileCacheHits();
        size_t misses = GetRegionFileCacheMisses();
        size_t total = hits + misses;

        return total > 0 ? static_cast<float>(hits) / static_cast<float>(total) : 0.0f;
    }

    MinecraftChunkLoaderImpl::DiagnosticInfo MinecraftChunkLoaderImpl::GetDiagnosticInfo() const {
        DiagnosticInfo info;

        info.regionFilesLoaded = GetLoadedRegionCount();
        info.chunksInCache = GetCacheSize();
        info.averageLoadTimeMs = GetAverageLoadTime();
        info.memoryUsageBytes = CalculateMemoryUsage();

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            info.totalChunksLoaded = m_stats.chunksLoaded;
            // Calculate fallback generated from cache misses that used generator
        }

        return info;
    }

    void MinecraftChunkLoaderImpl::LogDiagnostics(const std::string& prefix) const {
        DiagnosticInfo info = GetDiagnosticInfo();

        Log::Info("%s: Regions=%zu, Cache=%zu, Loaded=%zu, AvgTime=%.2fms, Memory=%zuKB",
                 prefix.c_str(),
                 info.regionFilesLoaded,
                 info.chunksInCache,
                 info.totalChunksLoaded,
                 info.averageLoadTimeMs,
                 info.memoryUsageBytes / 1024);
    }

    // === PROTECTED METHODS ===

    void MinecraftChunkLoaderImpl::UpdateStats(const ChunkLoadResult& result) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        m_stats.chunksLoaded++;
        m_stats.totalLoadTimeMs += result.loadTimeMs;

        if (result.wasFromCache) {
            m_stats.cacheHits++;
        } else {
            m_stats.cacheMisses++;
        }

        if (m_stats.chunksLoaded > 0) {
            m_stats.averageLoadTimeMs = m_stats.totalLoadTimeMs / m_stats.chunksLoaded;
        }

        m_stats.bytesLoaded += result.chunk ? sizeof(Chunk) : 0; // Rough estimate
    }

    // === CORE LOADING IMPLEMENTATION ===

    ChunkLoadResult MinecraftChunkLoaderImpl::LoadChunkInternal(Math::ChunkPos position, LoadPriority priority) {
        if (!ValidateChunkPosition(position)) {
            return ChunkLoadResult::Failure("Invalid chunk position: " + FormatChunkPos(position));
        }

        try {
            // Try loading from Minecraft region files first
            ChunkLoadResult result = LoadFromRegionFile(position);

            if (result.success) {
                result.wasFromDisk = true;
                return result;
            }

            // Fall back to generator if enabled and available
            if (m_config.enableFallbackGeneration && m_fallbackGenerator) {
                result = LoadFromGenerator(position);
                if (result.success) {
                    result.wasGenerated = true;
                    return result;
                }
            }

            return ChunkLoadResult::Failure("Chunk not found and no fallback generator available");

        } catch (const std::exception& e) {
            return ChunkLoadResult::Failure("Load failed: " + std::string(e.what()));
        }
    }

    ChunkLoadResult MinecraftChunkLoaderImpl::LoadFromRegionFile(Math::ChunkPos position) {
        // Check cache first
        if (m_config.enableCaching) {
            std::vector<uint8_t> cachedData;
            if (GetCachedChunkData(position, cachedData)) {
                // TODO: Deserialize cached data to chunk
                // For now, continue to file loading
            }
        }

        int regionX, regionZ, localX, localZ;
        ChunkToRegion(position, regionX, regionZ, localX, localZ);

        if (!RegionFileExists(regionX, regionZ)) {
            return ChunkLoadResult::Failure("Region file does not exist");
        }

        try {
            RegionFileLock lock(this, regionX, regionZ);
            if (!lock.IsValid()) {
                return ChunkLoadResult::Failure("Failed to lock region file");
            }

            // Load NBT data
            ::World::NBTTagPtr nbtData = LoadChunkNBT(position);
            if (!nbtData) {
                return ChunkLoadResult::Failure("Failed to load chunk NBT data");
            }

            // Validate NBT data
            if (!ValidateChunkNBT(nbtData)) {
                return ChunkLoadResult::Failure("Invalid chunk NBT data");
            }

            // Convert NBT to chunk
            std::shared_ptr<Chunk> chunk = NBTToChunk(nbtData, position);
            if (!chunk) {
                return ChunkLoadResult::Failure("Failed to convert NBT to chunk");
            }

            // Validate final chunk
            if (!ValidateChunk(*chunk)) {
                return ChunkLoadResult::Failure("Chunk validation failed");
            }

            ChunkLoadResult result = ChunkLoadResult::Success(chunk);
            result.wasFromDisk = true;
            return result;

        } catch (const std::exception& e) {
            return ChunkLoadResult::Failure("Region file error: " + std::string(e.what()));
        }
    }

    ChunkLoadResult MinecraftChunkLoaderImpl::LoadFromGenerator(Math::ChunkPos position) {
        if (!m_fallbackGenerator) {
            return ChunkLoadResult::Failure("No fallback generator available");
        }

        try {
            auto genResult = m_fallbackGenerator->GenerateChunk(position);

            if (!genResult.success) {
                return ChunkLoadResult::Failure("Generator failed: " + genResult.errorMessage);
            }

            ChunkLoadResult result = ChunkLoadResult::Success(genResult.chunk);
            result.wasGenerated = true;
            return result;

        } catch (const std::exception& e) {
            return ChunkLoadResult::Failure("Generator error: " + std::string(e.what()));
        }
    }

    // === REGION FILE UTILITIES ===

    void MinecraftChunkLoaderImpl::ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ,
                                                int& localX, int& localZ) const {
        regionX = chunkPos.x >> 5; // Divide by 32
        regionZ = chunkPos.z >> 5;
        localX = chunkPos.x & 31;  // Modulo 32
        localZ = chunkPos.z & 31;
    }

    std::string MinecraftChunkLoaderImpl::GetRegionFilePath(int regionX, int regionZ) const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);

        std::filesystem::path worldPath(m_config.worldPath);
        std::filesystem::path regionPath = worldPath / "region";

        std::string fileName = "r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca";
        return (regionPath / fileName).string();
    }

    bool MinecraftChunkLoaderImpl::RegionFileExists(int regionX, int regionZ) const {
        std::string regionPath = GetRegionFilePath(regionX, regionZ);
        return std::filesystem::exists(regionPath);
    }

    // === NBT DATA PROCESSING ===

    ::World::NBTTagPtr MinecraftChunkLoaderImpl::LoadChunkNBT(Math::ChunkPos position) {
        int regionX, regionZ, localX, localZ;
        ChunkToRegion(position, regionX, regionZ, localX, localZ);

        try {
            RegionFileLock lock(this, regionX, regionZ);
            if (!lock.IsValid()) {
                return nullptr;
            }

            // FIX: Use RegionDumper to read chunk data since RegionFile doesn't have GetChunkData
            ::World::ChunkData chunkData = ::World::RegionDumper::ReadChunkData(*lock.GetRegionFile(), localX, localZ);
            if (!chunkData.isValid || chunkData.uncompressedData.empty()) {
                return nullptr;
            }

            // Cache the raw data if caching is enabled
            if (m_config.enableCaching) {
                CacheChunkData(position, chunkData.compressedData);
            }

            // FIX: Parse the uncompressed NBT data
            ::World::NBTParser parser;
            return parser.Parse(chunkData.uncompressedData);

        } catch (const std::exception& e) {
            LogError("LoadChunkNBT", e.what());
            return nullptr;
        }
    }

    std::shared_ptr<Chunk> MinecraftChunkLoaderImpl::NBTToChunk(const ::World::NBTTagPtr& nbtData, Math::ChunkPos position) {
        if (!nbtData) {
            return nullptr;
        }

        try {
            auto chunk = std::make_shared<Chunk>();
            chunk->pos = position;

            // Use SectionDataUnpacker to convert NBT to chunk sections
            // Use static method from SectionDataUnpacker
            if (!SectionDataUnpacker::UnpackChunkSections(nbtData, *chunk)) {
                LogError("NBTToChunk", "Failed to unpack chunk sections");
                return nullptr;
            }

            return chunk;

        } catch (const std::exception& e) {
            LogError("NBTToChunk", e.what());
            return nullptr;
        }
    }

   bool MinecraftChunkLoaderImpl::ValidateChunkNBT(const ::World::NBTTagPtr& nbtData) const {
        if (!nbtData) {
            return false;
        }

        try {
            // FIX: Use RegionDumper validation method
            return ::World::RegionDumper::ValidateChunkNBT(nbtData);

        } catch (const std::exception& e) {
            LogError("ValidateChunkNBT", e.what());
            return false;
        }
    }

    // === CACHING IMPLEMENTATION ===

    bool MinecraftChunkLoaderImpl::GetCachedChunkData(Math::ChunkPos position, std::vector<uint8_t>& data) const {
        if (!m_config.enableCaching) {
            return false;
        }

        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        auto it = m_chunkDataCache.find(position);
        if (it != m_chunkDataCache.end()) {
            data = it->second.data;
            const_cast<ChunkDataCacheEntry&>(it->second).accessCount++;
            return true;
        }

        return false;
    }

    void MinecraftChunkLoaderImpl::CacheChunkData(Math::ChunkPos position, const std::vector<uint8_t>& data) {
        if (!m_config.enableCaching || data.empty()) {
            return;
        }

        std::lock_guard<std::shared_mutex> lock(m_cacheMutex);

        // Check cache size limit
        if (m_chunkDataCache.size() >= m_config.maxCacheSize) {
            // Remove oldest entry
            auto oldestIt = m_chunkDataCache.begin();
            auto oldestTime = oldestIt->second.loadTime;

            for (auto it = m_chunkDataCache.begin(); it != m_chunkDataCache.end(); ++it) {
                if (it->second.loadTime < oldestTime) {
                    oldestTime = it->second.loadTime;
                    oldestIt = it;
                }
            }

            m_chunkDataCache.erase(oldestIt);
        }

        m_chunkDataCache.emplace(position, ChunkDataCacheEntry(data));
    }

    void MinecraftChunkLoaderImpl::RemoveFromCache(Math::ChunkPos position) {
        std::lock_guard<std::shared_mutex> lock(m_cacheMutex);
        m_chunkDataCache.erase(position);
    }

    void MinecraftChunkLoaderImpl::PerformCacheMaintenance() {
        if (!m_config.enableCaching) {
            return;
        }

        // Evict old entries
        EvictOldCacheEntries(m_config.cacheMaxAgeSeconds);
    }

    // === VALIDATION ===

    bool MinecraftChunkLoaderImpl::ValidateWorldPath(const std::string& path) const {
        if (path.empty()) {
            return false;
        }

        try {
            std::filesystem::path worldPath(path);

            if (!std::filesystem::exists(worldPath)) {
                return false;
            }

            if (!std::filesystem::is_directory(worldPath)) {
                return false;
            }

            // Check for region directory
            std::filesystem::path regionPath = worldPath / "region";
            if (!std::filesystem::exists(regionPath) || !std::filesystem::is_directory(regionPath)) {
                return false;
            }

            return true;

        } catch (const std::exception& e) {
            LogError("ValidateWorldPath", e.what());
            return false;
        }
    }

    bool MinecraftChunkLoaderImpl::ValidateChunkPosition(Math::ChunkPos position) const {
        // Check for reasonable chunk coordinates (prevent extreme values)
        const int MAX_CHUNK_COORD = 1000000;

        return std::abs(position.x) < MAX_CHUNK_COORD &&
               std::abs(position.z) < MAX_CHUNK_COORD;
    }

    bool MinecraftChunkLoaderImpl::ValidateChunkData(const Chunk& chunk) const {
        // Additional validation beyond the base class
        if (chunk.IsEmpty()) {
            return false;
        }

        // Check chunk position matches
        // (would need access to expected position)

        return true;
    }

    // === ERROR HANDLING ===

    void MinecraftChunkLoaderImpl::SetLastError(const std::string& error) const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = error;
    }

    void MinecraftChunkLoaderImpl::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("MinecraftChunkLoader %s: %s", operation.c_str(), error.c_str());
        SetLastError(operation + ": " + error);
    }

    // === STATISTICS ===

    void MinecraftChunkLoaderImpl::UpdateLoadStats(const ChunkLoadResult& result, bool fromCache, bool fromGenerator) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (fromCache) {
            m_stats.cacheHits++;
        } else {
            m_stats.cacheMisses++;
        }

        // Update other stats as needed
    }

    void MinecraftChunkLoaderImpl::UpdateCacheStats(bool hit) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (hit) {
            m_stats.cacheHits++;
        } else {
            m_stats.cacheMisses++;
        }
    }

    // === UTILITY METHODS ===

    size_t MinecraftChunkLoaderImpl::CalculateMemoryUsage() const {
        std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

        size_t totalSize = 0;

        for (const auto& [pos, entry] : m_chunkDataCache) {
            totalSize += entry.data.size();
            totalSize += sizeof(ChunkDataCacheEntry);
        }

        return totalSize;
    }

    std::chrono::steady_clock::time_point MinecraftChunkLoaderImpl::GetCurrentTime() const {
        return std::chrono::steady_clock::now();
    }

    std::string MinecraftChunkLoaderImpl::FormatChunkPos(Math::ChunkPos position) const {
        return "(" + std::to_string(position.x) + ", " + std::to_string(position.z) + ")";
    }

    // === MINECRAFT COORDINATE HELPER ===

    Math::ChunkPos MinecraftCoordinateHelper::WorldToChunkPos(int worldX, int worldZ) {
        return Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
    }

    glm::ivec3 MinecraftCoordinateHelper::ChunkToWorldPos(Math::ChunkPos chunkPos, int localX, int localY, int localZ) {
        int worldX = chunkPos.x * 16 + localX;
        int worldZ = chunkPos.z * 16 + localZ;
        return glm::ivec3(worldX, localY, worldZ);
    }

    bool MinecraftCoordinateHelper::IsValidMinecraftY(int worldY) {
        return Math::WorldCoordinates::IsValidWorldY(worldY);
    }

    int MinecraftCoordinateHelper::ClampMinecraftY(int worldY) {
        return Math::WorldCoordinates::ClampWorldY(worldY);
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<IChunkLoader> CreateMinecraftChunkLoader(const MinecraftLoaderConfig& config) {
        return std::make_unique<MinecraftChunkLoaderImpl>(config);
    }

} // namespace Game