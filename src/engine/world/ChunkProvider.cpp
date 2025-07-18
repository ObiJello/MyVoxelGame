// File: src/engine/world/ChunkProvider.cpp - FIXED VERSION
#include "ChunkProvider.hpp"
#include "../../core/Log.hpp"
#include "loading/MinecraftChunkLoaderImpl.hpp"
#include "../../core/JobSystem.hpp"
#include "../block/BlockRegistry.hpp"
#include <algorithm>
#include <chrono>

namespace Game {

    // === CONSTRUCTION ===

    ChunkProvider::ChunkProvider(const ChunkProviderConfig& config)
        : m_config(config)
        , m_lastAutoSave(std::chrono::steady_clock::now()) {

        if (!m_config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, using defaults");
            m_config = ChunkProviderConfig{};
        }

        Log::Debug("ChunkProvider created with max chunks: %zu", m_config.maxLoadedChunks);
    }

    ChunkProvider::~ChunkProvider() {
        Shutdown();
    }

    ChunkProvider::ChunkProvider(ChunkProvider&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ChunkProvider& ChunkProvider::operator=(ChunkProvider&& other) noexcept {
        if (this != &other) {
            Shutdown();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    // === LIFECYCLE ===

    bool ChunkProvider::Initialize() {
        Log::Info("=== CHUNKPROVIDER INITIALIZATION START ===");

        if (m_initialized) {
            Log::Warning("ChunkProvider already initialized");
            return true;
        }

        Log::Info("Initializing ChunkProvider with composition architecture...");

        try {
            // Validate config first
            if (!m_config.IsValid()) {
                Log::Error("Invalid ChunkProvider configuration");
                return false;
            }
            Log::Info("Configuration validated");

            // Create core components
            Log::Info("Creating ChunkCache...");
            ChunkCacheConfig cacheConfig;
            cacheConfig.maxSize = m_config.maxLoadedChunks;
            cacheConfig.enableLRUEviction = m_config.enableLRUEviction;
            m_chunkCache = std::make_unique<ChunkCache>(cacheConfig);
            if (!m_chunkCache) {
                Log::Error("Failed to create ChunkCache");
                return false;
            }
            Log::Info("ChunkCache created");

            // Create chunk generator (ALWAYS create this as fallback)
            Log::Info("Creating ProceduralChunkGenerator...");
            m_chunkGenerator = std::make_unique<ProceduralChunkGenerator>(m_config.generationConfig);
            if (!m_chunkGenerator) {
                Log::Error("Failed to create ProceduralChunkGenerator");
                return false;
            }
            Log::Info("ProceduralChunkGenerator created");

            // Create Minecraft loader only if world path specified
            if (!m_config.minecraftWorldPath.empty()) {
                Log::Info("Creating MinecraftChunkLoaderImpl for: %s", m_config.minecraftWorldPath.c_str());
                MinecraftLoaderConfig loaderConfig;
                loaderConfig.worldPath = m_config.minecraftWorldPath;
                loaderConfig.enableFallbackGeneration = m_config.enableFallbackGeneration;
                m_chunkLoader = std::make_unique<MinecraftChunkLoaderImpl>(loaderConfig);
                if (!m_chunkLoader) {
                    Log::Warning("Failed to create MinecraftChunkLoaderImpl");
                } else {
                    Log::Info("MinecraftChunkLoaderImpl created");
                }
            } else {
                Log::Info("No Minecraft world path, skipping loader creation");
                m_chunkLoader = nullptr;
            }

            // Create chunk saver
            Log::Info("Creating AsyncChunkSaver...");
            SaveWorkerConfig saverConfig;
            saverConfig.workerThreads = m_config.enableAsyncSaving ? 2 : 1;
            saverConfig.enableAnvilFormat = !m_config.minecraftWorldPath.empty();
            saverConfig.minecraftWorldPath = m_config.minecraftWorldPath;
            m_chunkSaver = std::make_unique<AsyncChunkSaver>(saverConfig);
            if (!m_chunkSaver) {
                Log::Error("Failed to create AsyncChunkSaver");
                return false;
            }
            Log::Info("AsyncChunkSaver created");

            // Create dirty tracker
            Log::Info("Creating DirtyTracker...");
            m_dirtyTracker = std::make_unique<DirtyTracker>(m_config.dirtyConfig);
            if (!m_dirtyTracker) {
                Log::Error("Failed to create DirtyTracker");
                return false;
            }
            Log::Info("DirtyTracker created");

            // Initialize components
            Log::Info("Initializing components...");

            // Initialize loader (optional)
            if (m_chunkLoader) {
                Log::Info("Initializing MinecraftChunkLoaderImpl...");
                if (!m_chunkLoader->Initialize()) {
                    Log::Warning("Failed to initialize chunk loader, will use generation only");
                    m_chunkLoader.reset();
                } else {
                    Log::Info("MinecraftChunkLoaderImpl initialized");
                }
            }

            // Initialize generator (critical)
            Log::Info("Initializing ProceduralChunkGenerator...");
            if (!m_chunkGenerator->Initialize()) {
                Log::Error("Failed to initialize chunk generator - this is critical!");
                return false;
            }
            Log::Info("ProceduralChunkGenerator initialized");

            // Initialize saver
            Log::Info("Initializing AsyncChunkSaver...");
            if (!m_chunkSaver->Initialize()) {
                Log::Error("Failed to initialize chunk saver");
                return false;
            }
            Log::Info("AsyncChunkSaver initialized");

            // Initialize dirty tracker
            Log::Info("Initializing DirtyTracker...");
            if (!m_dirtyTracker->Initialize()) {
                Log::Error("Failed to initialize dirty tracker");
                return false;
            }
            Log::Info("DirtyTracker initialized");

            // Set up component dependencies
            Log::Info("Setting up component dependencies...");
            SetupComponentDependencies();
            Log::Info("Component dependencies set up");

            Log::Info("Configuring components...");
            ConfigureComponents();
            Log::Info("Components configured");

            // Reset statistics
            ResetProviderStats();

            // Mark as initialized
            m_initialized = true;

            Log::Info("✓ ChunkProvider initialized successfully");
            Log::Info("  - Generator: %s", m_chunkGenerator ? "Available" : "None");
            Log::Info("  - Loader: %s", m_chunkLoader ? "Available" : "None");
            Log::Info("  - Cache size: %zu", m_config.maxLoadedChunks);
            Log::Info("  - Initialized flag: %s", m_initialized ? "true" : "false");
            Log::Info("=== CHUNKPROVIDER INITIALIZATION COMPLETE ===");

            return true;

        } catch (const std::exception& e) {
            Log::Error("ChunkProvider initialization failed with exception: %s", e.what());
            Shutdown();
            return false;
        } catch (...) {
            Log::Error("ChunkProvider initialization failed with unknown exception");
            Shutdown();
            return false;
        }
}

    void ChunkProvider::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down ChunkProvider...");

        m_shutdownRequested = true;

        // Save all dirty chunks before shutdown
        if (m_chunkSaver && m_chunkCache) {
            Log::Info("Saving all dirty chunks before shutdown...");
            m_chunkCache->SaveAllDirty();
        }

        // Shutdown components in reverse order
        if (m_dirtyTracker) {
            m_dirtyTracker->Shutdown();
            m_dirtyTracker.reset();
        }

        if (m_chunkSaver) {
            m_chunkSaver->Shutdown();
            m_chunkSaver.reset();
        }

        if (m_chunkGenerator) {
            m_chunkGenerator->Shutdown();
            m_chunkGenerator.reset();
        }

        if (m_chunkLoader) {
            m_chunkLoader->Shutdown();
            m_chunkLoader.reset();
        }

        if (m_chunkCache) {
            m_chunkCache.reset();
        }

        // Clear pending requests
        {
            std::lock_guard<std::mutex> lock(m_loadRequestMutex);
            while (!m_loadRequests.empty()) {
                m_loadRequests.pop();
            }
            m_pendingLoads.clear();
        }

        LogComponentStats();

        m_initialized = false;
        Log::Info("ChunkProvider shutdown complete");
    }

    void ChunkProvider::Update(float deltaTime) {
        if (!m_initialized || m_shutdownRequested) {
            return;
        }

        PerformMaintenance(deltaTime);
    }

    // === CORE CHUNK OPERATIONS ===

    std::shared_ptr<Chunk> ChunkProvider::GetChunk(Math::ChunkPos position) {
        return GetChunkWithPriority(position, false);
    }

    std::shared_ptr<Chunk> ChunkProvider::GetChunkWithPriority(Math::ChunkPos position, bool highPriority) {
        if (!m_initialized) {
            Log::Error("ChunkProvider::GetChunk: Not initialized");
            return nullptr;
        }

        if (!ValidateChunkPosition(position)) {
            Log::Error("ChunkProvider::GetChunk: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        // CRITICAL: Always check cache first and return if found
        std::shared_ptr<Chunk> chunk = TryLoadFromCache(position);
        if (chunk) {
            Log::Debug("Chunk (%d, %d) retrieved from cache (not regenerated)", position.x, position.z);
            return chunk;
        }

        Log::Info("Chunk (%d, %d) not in cache, attempting to load/generate", position.x, position.z);

        // Performance throttling (only if not high priority)
        if (!highPriority && ShouldThrottleLoading()) {
            Log::Debug("Chunk loading throttled for (%d, %d)", position.x, position.z);
            return nullptr;
        }

        // Load from disk or generate (only if NOT already in cache)
        chunk = LoadChunkInternal(position, true);

        if (chunk) {
            // Complete chunk loading and add to cache
            chunk = CompleteChunkLoad(chunk);
            Log::Info("Successfully loaded/generated chunk (%d, %d)", position.x, position.z);
        } else {
            Log::Warning("Failed to load/generate chunk (%d, %d)", position.x, position.z);
        }

        return chunk;
    }

    std::future<std::shared_ptr<Chunk>> ChunkProvider::LoadChunkAsync(Math::ChunkPos position) {
        auto promise = std::make_shared<std::promise<std::shared_ptr<Chunk>>>();
        auto future = promise->get_future();

        if (!m_initialized) {
            promise->set_value(nullptr);
            return future;
        }

        // Check cache first
        auto chunk = TryLoadFromCache(position);
        if (chunk) {
            promise->set_value(chunk);
            return future;
        }

        // Queue for async loading
        {
            std::lock_guard<std::mutex> lock(m_loadRequestMutex);

            // Check if already pending
            if (m_pendingLoads.find(position) != m_pendingLoads.end()) {
                promise->set_value(nullptr); // Already pending
                return future;
            }

            ChunkLoadRequest request(position);
            request.promise = promise;
            m_loadRequests.push(std::move(request));
            m_pendingLoads.insert(position);
        }

        return future;
    }

    bool ChunkProvider::IsChunkLoaded(Math::ChunkPos position) const {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        // Check cache first
        bool inCache = m_chunkCache->Contains(position);

        if (inCache) {
            Log::Debug("Chunk (%d, %d) found in cache", position.x, position.z);
        } else {
            Log::Debug("Chunk (%d, %d) NOT in cache", position.x, position.z);
        }

        return inCache;
    }

    void ChunkProvider::PreloadChunk(Math::ChunkPos position) {
        if (!IsChunkLoaded(position)) {
            LoadChunkAsync(position);
        }
    }

    void ChunkProvider::PreloadArea(Math::ChunkPos center, int radius) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                Math::ChunkPos pos{center.x + dx, center.z + dz};
                PreloadChunk(pos);
            }
        }
    }

    bool ChunkProvider::UnloadChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        return m_chunkCache->Remove(position);
    }

    void ChunkProvider::UnloadArea(Math::ChunkPos center, int radius) {
        if (!m_initialized) {
            return;
        }

        // Unload chunks outside the radius
        auto loadedChunks = m_chunkCache->GetDebugState().allChunks;

        for (const auto& pos : loadedChunks) {
            int dx = pos.x - center.x;
            int dz = pos.z - center.z;
            int distanceSquared = dx * dx + dz * dz;

            if (distanceSquared > radius * radius) {
                UnloadChunk(pos);
            }
        }
    }

    // === BLOCK ACCESS ===

    BlockID ChunkProvider::GetBlock(int worldX, int worldY, int worldZ) const {
        if (!ValidateWorldPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        auto chunk = m_chunkCache ? m_chunkCache->Peek(chunkPos) : nullptr;
        if (!chunk) {
            return BlockID::Air; // Chunk not loaded
        }

        return chunk->GetBlock(localX, localY, localZ);
    }

    void ChunkProvider::SetBlock(int worldX, int worldY, int worldZ, BlockID block) {
        if (!ValidateWorldPosition(worldX, worldY, worldZ) || !IsValidBlockID(block)) {
            return;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        auto chunk = GetChunk(chunkPos); // Load if necessary
        if (!chunk) {
            Log::Warning("Cannot set block at (%d, %d, %d) - chunk not available", worldX, worldY, worldZ);
            return;
        }

        // Set the block
        chunk->SetBlock(localX, localY, localZ, block);

        // Mark chunk as dirty for saving
        if (m_chunkCache) {
            m_chunkCache->MarkDirty(chunkPos);
        }

        // Mark affected sections dirty for mesh rebuilding
        MarkBlockDirty(worldX, worldY, worldZ);
    }

    void ChunkProvider::SetBlocks(const std::vector<std::tuple<int, int, int, BlockID>>& blocks) {
        if (blocks.empty()) {
            return;
        }

        // Group blocks by chunk for efficient processing
        std::unordered_map<Math::ChunkPos, std::vector<std::tuple<int, int, int, BlockID>>, Math::ChunkPosHash> chunkBlocks;

        for (const auto& [worldX, worldY, worldZ, block] : blocks) {
            if (ValidateWorldPosition(worldX, worldY, worldZ) && IsValidBlockID(block)) {
                Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
                chunkBlocks[chunkPos].emplace_back(worldX, worldY, worldZ, block);
            }
        }

        // Process each chunk
        for (const auto& [chunkPos, chunkBlockList] : chunkBlocks) {
            auto chunk = GetChunk(chunkPos);
            if (!chunk) {
                continue;
            }

            // Set all blocks in this chunk
            for (const auto& [worldX, worldY, worldZ, block] : chunkBlockList) {
                int localX, localY, localZ;
                Math::ChunkPos tempPos;
                WorldToLocalCoords(worldX, worldY, worldZ, tempPos, localX, localY, localZ);

                chunk->SetBlock(localX, localY, localZ, block);
                MarkBlockDirty(worldX, worldY, worldZ);
            }

            // Mark chunk dirty for saving
            if (m_chunkCache) {
                m_chunkCache->MarkDirty(chunkPos);
            }
        }
    }

    std::vector<BlockID> ChunkProvider::GetBlocks(const std::vector<std::tuple<int, int, int>>& positions) const {
        std::vector<BlockID> results;
        results.reserve(positions.size());

        for (const auto& [worldX, worldY, worldZ] : positions) {
            results.push_back(GetBlock(worldX, worldY, worldZ));
        }

        return results;
    }

    // === INEIGHBORPROVIDER IMPLEMENTATION ===

    bool ChunkProvider::IsChunkLoaded(int chunkX, int chunkZ) const {
        return IsChunkLoaded(Math::ChunkPos{chunkX, chunkZ});
    }

    bool ChunkProvider::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos);
    }

    bool ChunkProvider::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isSolid;
    }

    bool ChunkProvider::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isFluid;
    }

    bool ChunkProvider::IsBlockTransparent(int worldX, int worldY, int worldZ) const {
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isTransparent;
    }

    INeighborProvider::NeighborStats ChunkProvider::GetStats() const {
        // Return basic stats for neighbor provider interface
        NeighborStats stats;
        auto providerStats = GetProviderStats();
        stats.totalQueries = providerStats.chunksLoaded * 100; // Rough estimate
        stats.cacheHits = static_cast<size_t>(providerStats.cacheHitRate);
        return stats;
    }

    void ChunkProvider::ResetStats() {
        ResetProviderStats();
    }

    // === DIRTY TRACKING ===

    void ChunkProvider::MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->MarkSectionDirty(chunkPos, sectionIndex);
    }

    void ChunkProvider::MarkChunkDirty(Math::ChunkPos chunkPos) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->MarkChunkDirty(chunkPos);
    }

    void ChunkProvider::MarkBlockDirty(int worldX, int worldY, int worldZ) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        // Get affected sections (including neighbors if on boundaries)
        auto affectedSections = GetAffectedSections(worldX, worldY, worldZ, true);

        for (const auto& section : affectedSections) {
            m_dirtyTracker->MarkSectionDirty(section.chunkPos, section.sectionIndex);
        }
    }

    void ChunkProvider::MarkRegionDirty(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->MarkRegionDirty(minX, minY, minZ, maxX, maxY, maxZ);
    }

    std::vector<DirtySection> ChunkProvider::GetDirtySections(size_t maxCount) {
        if (!m_initialized || !m_dirtyTracker) {
            return {};
        }

        return m_dirtyTracker->GetDirtySections(maxCount);
    }

    std::vector<DirtySection> ChunkProvider::GetAndClearDirtySections() {
        if (!m_initialized || !m_dirtyTracker) {
            return {};
        }

        return m_dirtyTracker->GetAndClearAllDirtySections();
    }

    void ChunkProvider::ClearDirtySections(const std::vector<DirtySection>& sections) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->ClearDirtySections(sections);
    }

    bool ChunkProvider::IsChunkDirty(Math::ChunkPos chunkPos) const {
        if (!m_initialized || !m_dirtyTracker) {
            return false;
        }

        return m_dirtyTracker->IsChunkDirty(chunkPos);
    }

    bool ChunkProvider::IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const {
        if (!m_initialized || !m_dirtyTracker) {
            return false;
        }

        return m_dirtyTracker->IsSectionDirty(chunkPos, sectionIndex);
    }

    size_t ChunkProvider::GetDirtyCount() const {
        if (!m_initialized || !m_dirtyTracker) {
            return 0;
        }

        return m_dirtyTracker->GetDirtyCount();
    }

    // === SAVING ===

    void ChunkProvider::SaveChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache || !m_chunkSaver) {
            return;
        }

        auto chunk = m_chunkCache->Peek(position);
        if (chunk && m_chunkCache->IsDirty(position)) {
            m_chunkSaver->SaveChunkAsync(*chunk);
            m_chunkCache->ClearDirtyFlag(position);
        }
    }

    void ChunkProvider::SaveAllDirtyChunks() {
        if (!m_initialized || !m_chunkCache) {
            return;
        }

        m_chunkCache->SaveAllDirty();
    }

    void ChunkProvider::SaveArea(Math::ChunkPos center, int radius) {
        if (!m_initialized || !m_chunkCache) {
            return;
        }

        auto dirtyChunks = m_chunkCache->GetDirtyChunks();

        for (const auto& pos : dirtyChunks) {
            int dx = pos.x - center.x;
            int dz = pos.z - center.z;

            if (dx * dx + dz * dz <= radius * radius) {
                SaveChunk(pos);
            }
        }
    }

    void ChunkProvider::SetAutoSaveEnabled(bool enabled) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.enableAutoSave = enabled;
    }

    void ChunkProvider::SetAutoSaveInterval(float seconds) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.autoSaveIntervalSeconds = std::max(1.0f, seconds);
    }

    bool ChunkProvider::IsAutoSaveEnabled() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.enableAutoSave;
    }

    // === CONFIGURATION ===

    void ChunkProvider::SetConfig(const ChunkProviderConfig& config) {
        if (!config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, ignoring");
            return;
        }

        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config = config;

        if (m_initialized) {
            ConfigureComponents();
        }
    }

    ChunkProviderConfig ChunkProvider::GetConfig() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config;
    }

    void ChunkProvider::SetWorldPath(const std::string& path) {
        {
            std::lock_guard<std::shared_mutex> lock(m_configMutex);
            m_config.minecraftWorldPath = path;
        }

        if (m_initialized && m_chunkLoader) {
            m_chunkLoader->SetSource(path);
        }
    }

    std::string ChunkProvider::GetWorldPath() const {
        if (m_chunkLoader) {
            return m_chunkLoader->GetSource();
        }

        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.minecraftWorldPath;
    }

    void ChunkProvider::SetMaxLoadedChunks(size_t maxChunks) {
        {
            std::lock_guard<std::shared_mutex> lock(m_configMutex);
            m_config.maxLoadedChunks = std::max(size_t(16), maxChunks);
        }

        if (m_initialized && m_chunkCache) {
            ChunkCacheConfig cacheConfig = m_chunkCache->GetConfig();
            cacheConfig.maxSize = maxChunks;
            m_chunkCache->SetConfig(cacheConfig);
        }
    }

    size_t ChunkProvider::GetMaxLoadedChunks() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.maxLoadedChunks;
    }

    void ChunkProvider::SetGenerationSeed(int32_t seed) {
        {
            std::lock_guard<std::shared_mutex> lock(m_configMutex);
            m_config.generationConfig.seed = seed;
        }

        if (m_initialized && m_chunkGenerator) {
            m_chunkGenerator->SetSeed(seed);
        }
    }

    int32_t ChunkProvider::GetGenerationSeed() const {
        if (m_chunkGenerator) {
            return m_chunkGenerator->GetSeed();
        }

        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.generationConfig.seed;
    }

    // === STATISTICS ===

    ChunkProviderStats ChunkProvider::GetProviderStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ChunkProviderStats stats = m_stats;

        // Update real-time values
        if (m_chunkCache) {
            auto cacheStats = m_chunkCache->GetStats();
            stats.cacheHitRate = static_cast<size_t>(cacheStats.GetHitRate() * 100);
            stats.memoryUsage = m_chunkCache->GetMemoryUsageBytes();
        }

        if (m_dirtyTracker) {
            stats.dirtySections = m_dirtyTracker->GetDirtyCount();
        }

        return stats;
    }

    void ChunkProvider::ResetProviderStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();

        // Reset component stats
        if (m_chunkCache) m_chunkCache->ResetStats();
        if (m_chunkLoader) m_chunkLoader->ResetStats();
        if (m_chunkGenerator) m_chunkGenerator->ResetStats();
        if (m_chunkSaver) m_chunkSaver->ResetStats();
        if (m_dirtyTracker) m_dirtyTracker->ResetStats();
    }

    ChunkCache::CacheStats ChunkProvider::GetCacheStats() const {
        return m_chunkCache ? m_chunkCache->GetStats() : ChunkCache::CacheStats{};
    }

    IChunkLoader::LoaderStats ChunkProvider::GetLoaderStats() const {
        return m_chunkLoader ? m_chunkLoader->GetStats() : IChunkLoader::LoaderStats{};
    }

    ProceduralChunkGenerator::GeneratorStats ChunkProvider::GetGeneratorStats() const {
        return m_chunkGenerator ? m_chunkGenerator->GetStats() : ProceduralChunkGenerator::GeneratorStats{};
    }

    AsyncChunkSaver::SaverStats ChunkProvider::getSaverStats() const {
        return m_chunkSaver ? m_chunkSaver->GetStats() : AsyncChunkSaver::SaverStats{};
    }

    DirtyTrackerStats ChunkProvider::GetDirtyTrackerStats() const {
        return m_dirtyTracker ? m_dirtyTracker->GetStats() : DirtyTrackerStats{};
    }

    // === DIAGNOSTICS ===

    size_t ChunkProvider::GetMemoryUsage() const {
        size_t total = sizeof(ChunkProvider);

        if (m_chunkCache) total += m_chunkCache->GetMemoryUsageBytes();
        if (m_dirtyTracker) total += m_dirtyTracker->GetMemoryUsage();

        return total;
    }

    size_t ChunkProvider::GetLoadedChunkCount() const {
        return m_chunkCache ? m_chunkCache->GetStats().currentSize : 0;
    }

    void ChunkProvider::LogPerformanceStats() const {
        auto stats = GetProviderStats();

        Log::Info("ChunkProvider Performance:");
        Log::Info("  Loaded: %zu, Generated: %zu, Saved: %zu, Evicted: %zu",
                 stats.chunksLoaded, stats.chunksGenerated, stats.chunksSaved, stats.chunksEvicted);
        Log::Info("  Avg Times - Load: %.2fms, Gen: %.2fms, Save: %.2fms",
                 stats.averageLoadTime, stats.averageGenerationTime, stats.averageSaveTime);
        Log::Info("  Cache Hit Rate: %zu%%, Memory: %zu KB",
                 stats.cacheHitRate, stats.memoryUsage / 1024);
        Log::Info("  Dirty Sections: %zu", stats.dirtySections);
    }

    void ChunkProvider::LogComponentStats() const {
        Log::Info("=== ChunkProvider Component Statistics ===");

        if (m_chunkCache) {
            m_chunkCache->LogStats("ChunkCache");
        }

        if (m_chunkLoader) {
            m_chunkLoader->LogDiagnostics("ChunkLoader");
        }

        if (m_chunkGenerator) {
            auto genStats = m_chunkGenerator->GetStats();
            Log::Info("ChunkGenerator: Generated=%zu, AvgTime=%.2fms, Blocks=%zu",
                     genStats.chunksGenerated, genStats.averageGenerationTimeMs, genStats.totalBlocksGenerated);
        }

        if (m_chunkSaver) {
            auto saveStats = m_chunkSaver->GetStats();
            Log::Info("ChunkSaver: Saved=%zu, Failed=%zu, AvgTime=%.2fms, Bytes=%zu",
                     saveStats.chunksSaved, saveStats.saveFailures, saveStats.averageSaveTimeMs, saveStats.totalBytesWritten);
        }

        if (m_dirtyTracker) {
            m_dirtyTracker->LogDirtySections("DirtyTracker");
        }
    }

    bool ChunkProvider::ValidateState() const {
        if (!m_initialized) {
            return false;
        }

        bool valid = true;

        if (m_chunkCache && !m_chunkCache->ValidateIntegrity()) {
            Log::Error("ChunkCache validation failed");
            valid = false;
        }

        if (m_dirtyTracker && !m_dirtyTracker->ValidateState()) {
            Log::Error("DirtyTracker validation failed");
            valid = false;
        }

        return valid;
    }

    void ChunkProvider::DumpLoadedChunks() const {
        if (!m_chunkCache) {
            Log::Info("No chunk cache available");
            return;
        }

        auto debugState = m_chunkCache->GetDebugState();
        Log::Info("=== Loaded Chunks (%zu total) ===", debugState.allChunks.size());

        for (const auto& pos : debugState.allChunks) {
            bool dirty = std::find(debugState.dirtyChunks.begin(), debugState.dirtyChunks.end(), pos)
                        != debugState.dirtyChunks.end();
            Log::Info("  Chunk (%d, %d)%s", pos.x, pos.z, dirty ? " [DIRTY]" : "");
        }
    }

    // === LEGACY COMPATIBILITY ===

    std::shared_ptr<Chunk> ChunkProvider::LoadChunkFromDisk(Math::ChunkPos position) {
        return TryLoadFromDisk(position);
    }

    std::shared_ptr<Chunk> ChunkProvider::GenerateChunk(Math::ChunkPos position) {
        return TryGenerateChunk(position);
    }

    void ChunkProvider::QueueChunkForSaving(std::shared_ptr<const Chunk> chunk) {
        if (chunk && m_chunkSaver) {
            m_chunkSaver->QueueChunkForSave(chunk);
        }
    }

    void ChunkProvider::AddDirtySection(const DirtySection& section) {
        MarkSectionDirty(section.chunkPos, section.sectionIndex);
    }

    std::vector<DirtySection> ChunkProvider::RemoveDirtySections() {
        return GetAndClearDirtySections();
    }

    // === INTERNAL WORKFLOWS ===

    std::shared_ptr<Chunk> ChunkProvider::LoadChunkInternal(Math::ChunkPos position, bool allowGeneration) {
        if (!ValidateChunkPosition(position)) {
            Log::Error("LoadChunkInternal: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        Log::Debug("LoadChunkInternal: Loading chunk (%d, %d)", position.x, position.z);

        // Try loading from disk first
        std::shared_ptr<Chunk> chunk = TryLoadFromDisk(position);
        if (chunk) {
            Log::Debug("Loaded chunk (%d, %d) from disk", position.x, position.z);
            return chunk;
        }

        if (allowGeneration) {
            // Generate new chunk as fallback
            Log::Debug("Generating chunk (%d, %d)", position.x, position.z);
            chunk = TryGenerateChunk(position);
            if (chunk) {
                Log::Debug("Generated chunk (%d, %d)", position.x, position.z);
                return chunk;
            } else {
                Log::Error("Failed to generate chunk (%d, %d)", position.x, position.z);
            }
        }

        Log::Warning("LoadChunkInternal: All methods failed for chunk (%d, %d)", position.x, position.z);
        return nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::TryLoadFromCache(Math::ChunkPos position) {
        if (!m_chunkCache) {
            return nullptr;
        }

        return m_chunkCache->Get(position);
    }

    std::shared_ptr<Chunk> ChunkProvider::TryLoadFromDisk(Math::ChunkPos position) {
        if (!m_chunkLoader) {
            return nullptr;
        }

        auto result = m_chunkLoader->LoadChunk(position);
        return result.success ? result.chunk : nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::TryGenerateChunk(Math::ChunkPos position) {
        if (!m_chunkGenerator) {
            Log::Error("TryGenerateChunk: No chunk generator available");
            return nullptr;
        }

        try {
            Log::Debug("Generating chunk (%d, %d) using generator", position.x, position.z);
            auto result = m_chunkGenerator->GenerateChunk(position);

            if (!result.success) {
                Log::Error("TryGenerateChunk: Generator failed for chunk (%d, %d): %s",
                          position.x, position.z, result.errorMessage.c_str());
                return nullptr;
            }

            if (!result.chunk) {
                Log::Error("TryGenerateChunk: Generator returned null chunk for (%d, %d)",
                          position.x, position.z);
                return nullptr;
            }

            // Ensure chunk position is set correctly
            result.chunk->pos = position;

            Log::Info("Successfully generated chunk (%d, %d)", position.x, position.z);
            return result.chunk;

        } catch (const std::exception& e) {
            Log::Error("TryGenerateChunk: Exception generating chunk (%d, %d): %s",
                      position.x, position.z, e.what());
            return nullptr;
        }
    }

    std::shared_ptr<Chunk> ChunkProvider::CompleteChunkLoad(std::shared_ptr<Chunk> chunk) {
        if (!chunk || !m_chunkCache) {
            return chunk;
        }

        // Validate chunk
        if (!ValidateChunk(chunk)) {
            Log::Warning("Loaded chunk failed validation: (%d, %d)", chunk->pos.x, chunk->pos.z);
            return nullptr;
        }

        // CRITICAL: Check if chunk is already in cache before adding
        if (m_chunkCache->Contains(chunk->pos)) {
            Log::Warning("Chunk (%d, %d) already in cache, not adding duplicate", chunk->pos.x, chunk->pos.z);
            // Return the existing chunk from cache instead
            return m_chunkCache->Get(chunk->pos);
        }

        // Add to cache
        m_chunkCache->Put(chunk->pos, chunk);
        Log::Debug("Added chunk (%d, %d) to cache", chunk->pos.x, chunk->pos.z);

        return chunk;
    }

    // === COORDINATION ===

    void ChunkProvider::SetupComponentDependencies() {
        if (!m_initialized) {
            return;
        }

        // Set up chunk cache dependencies
        if (m_chunkCache && m_chunkSaver) {
            // Create a non-owning shared_ptr that won't delete the object
            std::shared_ptr<IChunkSaver> saverPtr(m_chunkSaver.get(), [](IChunkSaver*){
                // Empty deleter - the unique_ptr will handle deletion
            });
            m_chunkCache->SetChunkSaver(saverPtr);
        }

        // Set up loader fallback generation
        if (m_chunkLoader && m_chunkGenerator) {
            // Create a non-owning shared_ptr that won't delete the object
            std::shared_ptr<IChunkGenerator> generatorPtr(m_chunkGenerator.get(), [](IChunkGenerator*){
                // Empty deleter - the unique_ptr will handle deletion
            });
            m_chunkLoader->SetFallbackGenerator(generatorPtr);
        }

        // Set up callbacks
        if (m_chunkCache) {
            m_chunkCache->SetEvictionCallback(
                [this](Math::ChunkPos pos, std::shared_ptr<Chunk> chunk, bool wasDirty) {
                    OnChunkEvicted(pos, chunk, wasDirty);
                });
        }

        if (m_dirtyTracker) {
            m_dirtyTracker->SetDirtyCallback(
                [this](const DirtySection& section) {
                    OnSectionDirty(section);
                });
        }
    }

    void ChunkProvider::ConfigureComponents() {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);

        // Configure cache
        if (m_chunkCache) {
            ChunkCacheConfig cacheConfig = m_chunkCache->GetConfig();
            cacheConfig.maxSize = m_config.maxLoadedChunks;
            cacheConfig.enableLRUEviction = m_config.enableLRUEviction;
            m_chunkCache->SetConfig(cacheConfig);
        }

        // Configure loader
        if (m_chunkLoader) {
            m_chunkLoader->SetSource(m_config.minecraftWorldPath);
        }

        // Configure generator
        if (m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }

        // Configure saver
        if (m_chunkSaver) {
            m_chunkSaver->SetAutoSaveEnabled(m_config.enableAutoSave);
            m_chunkSaver->SetAutoSaveInterval(m_config.autoSaveIntervalSeconds);
        }

        // Configure dirty tracker
        if (m_dirtyTracker) {
            m_dirtyTracker->SetConfig(m_config.dirtyConfig);
        }
    }

    void ChunkProvider::SynchronizeComponents() {
        // Synchronize state between components if needed
        // Currently not much synchronization needed due to good separation of concerns
    }

    void ChunkProvider::OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty) {
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksEvicted++;
        }

        Log::Debug("Chunk (%d, %d) evicted from cache%s", position.x, position.z, wasDirty ? " (was dirty)" : "");
    }

    void ChunkProvider::OnChunkSaved(const ChunkSaveResult& result) {
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            if (result.success) {
                m_stats.chunksSaved++;
                m_stats.averageSaveTime = (m_stats.averageSaveTime * (m_stats.chunksSaved - 1) + result.saveTimeMs) / m_stats.chunksSaved;
            }
        }
    }

    void ChunkProvider::OnSectionDirty(const DirtySection& section) {
        // Optional: Log or perform additional processing when sections become dirty
        // Currently just forwarded to interested systems
    }

    // === MAINTENANCE ===

    void ChunkProvider::PerformMaintenance(float deltaTime) {
        UpdateAutoSave(deltaTime);
        ProcessLoadRequests();
        UpdateComponentStats();
        EnforcePerformanceLimits();

        // Update components
        if (m_chunkCache) {
            m_chunkCache->PerformMaintenance();
        }

        if (m_dirtyTracker) {
            m_dirtyTracker->Update(deltaTime);
        }
    }

    void ChunkProvider::UpdateAutoSave(float deltaTime) {
        if (!IsAutoSaveEnabled()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastAutoSave);

        if (elapsed.count() >= m_config.autoSaveIntervalSeconds) {
            SaveAllDirtyChunks();
            m_lastAutoSave = now;
        }
    }

    void ChunkProvider::ProcessLoadRequests() {
        std::lock_guard<std::mutex> lock(m_loadRequestMutex);

        // Process up to maxChunksPerFrame requests
        int processed = 0;
        while (!m_loadRequests.empty() && processed < m_config.maxChunksPerFrame) {
            ChunkLoadRequest request = std::move(m_loadRequests.front());
            m_loadRequests.pop();
            m_pendingLoads.erase(request.position);

            // Load chunk
            auto chunk = LoadChunkInternal(request.position);
            if (chunk) {
                chunk = CompleteChunkLoad(chunk);
            }

            // Set promise result
            if (request.promise) {
                request.promise->set_value(chunk);
            }

            processed++;
        }
    }

    void ChunkProvider::UpdateComponentStats() {
        // Aggregate stats from components
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (m_chunkLoader) {
            auto loaderStats = m_chunkLoader->GetStats();
            m_stats.chunksLoaded = loaderStats.chunksLoaded;
            m_stats.averageLoadTime = loaderStats.averageLoadTimeMs;
        }

        if (m_chunkGenerator) {
            auto genStats = m_chunkGenerator->GetStats();
            m_stats.chunksGenerated = genStats.chunksGenerated;
            m_stats.averageGenerationTime = genStats.averageGenerationTimeMs;
        }

        if (m_chunkSaver) {
            auto saveStats = m_chunkSaver->GetStats();
            m_stats.chunksSaved = saveStats.chunksSaved;
            m_stats.averageSaveTime = saveStats.averageSaveTimeMs;
        }
    }

    void ChunkProvider::EnforcePerformanceLimits() {
        // Currently basic enforcement - could be enhanced
        if (m_chunkCache) {
            auto stats = m_chunkCache->GetStats();
            if (stats.GetUtilization() > 0.95f) {
                // Cache is very full, trigger eviction
                m_chunkCache->EvictLRU(m_config.maxLoadedChunks / 10);
            }
        }
    }

    bool ChunkProvider::ShouldThrottleLoading() const {
        // Simple throttling based on cache usage
        if (!m_chunkCache) {
            return false;
        }

        auto stats = m_chunkCache->GetStats();
        return stats.GetUtilization() > 0.9f; // Throttle when cache is 90% full
    }

    // === VALIDATION ===

    bool ChunkProvider::ValidateChunk(const std::shared_ptr<Chunk>& chunk) const {
        if (!chunk) {
            return false;
        }

        // Basic validation
        if (chunk->IsEmpty()) {
            return false;
        }

        // Validate position is reasonable
        return ValidateChunkPosition(chunk->pos);
    }

    bool ChunkProvider::ValidateChunkPosition(Math::ChunkPos position) const {
        // Prevent extreme coordinates that might cause issues
        const int MAX_CHUNK_COORD = 1000000;
        bool valid = std::abs(position.x) < MAX_CHUNK_COORD && std::abs(position.z) < MAX_CHUNK_COORD;

        if (!valid) {
            Log::Error("Invalid chunk position: (%d, %d) exceeds maximum coordinate limit",
                      position.x, position.z);
        }

        return valid;
    }

    bool ChunkProvider::ValidateWorldPosition(int worldX, int worldY, int worldZ) const {
        return Math::WorldCoordinates::IsValidWorldY(worldY) &&
               std::abs(worldX) < 30000000 && std::abs(worldZ) < 30000000; // Minecraft world limits
    }

    // === HELPERS ===

    void ChunkProvider::WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                                          int& localX, int& localY, int& localZ) const {
        chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        localX = worldX - (chunkPos.x * Math::CHUNK_SIZE_X);
        localY = worldY; // World Y is used directly
        localZ = worldZ - (chunkPos.z * Math::CHUNK_SIZE_Z);
        
        // Handle negative coordinates properly
        if (localX < 0) localX += Math::CHUNK_SIZE_X;
        if (localZ < 0) localZ += Math::CHUNK_SIZE_Z;
    }

    bool ChunkProvider::IsValidBlockID(BlockID block) const {
        // Basic validation - could use BlockRegistry for more sophisticated checking
        return static_cast<int>(block) >= 0 && static_cast<int>(block) < 1000;
    }

    bool ChunkProvider::GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const {
        // Default properties - could integrate with BlockRegistry for more accurate data
        switch (block) {
            case BlockID::Air:
                isSolid = false;
                isFluid = false;
                isTransparent = true;
                break;
            case BlockID::Water:
                isSolid = false;
                isFluid = true;
                isTransparent = true;
                break;
            case BlockID::Glass:
                isSolid = true;
                isFluid = false;
                isTransparent = true;
                break;
            case BlockID::OakLeaves:
            case BlockID::BirchLeaves:
                isSolid = true;
                isFluid = false;
                isTransparent = true;
                break;
            default:
                isSolid = true;
                isFluid = false;
                isTransparent = false;
                break;
        }
        return true;
    }

    void ChunkProvider::UpdateLoadStats(float loadTime, bool fromCache, bool fromDisk, bool generated) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (fromCache) {
            // Cache hit - very fast
            m_stats.cacheHitRate = std::min(size_t(100), m_stats.cacheHitRate + 1);
        } else {
            m_stats.cacheHitRate = std::max(size_t(0), m_stats.cacheHitRate - 1);
        }

        if (fromDisk) {
            m_stats.chunksLoaded++;
            m_stats.averageLoadTime = (m_stats.averageLoadTime * (m_stats.chunksLoaded - 1) + loadTime) / m_stats.chunksLoaded;
        }

        if (generated) {
            m_stats.chunksGenerated++;
            m_stats.averageGenerationTime = (m_stats.averageGenerationTime * (m_stats.chunksGenerated - 1) + loadTime) / m_stats.chunksGenerated;
        }
    }

    void ChunkProvider::UpdateSaveStats(float saveTime, bool success) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (success) {
            m_stats.chunksSaved++;
            m_stats.averageSaveTime = (m_stats.averageSaveTime * (m_stats.chunksSaved - 1) + saveTime) / m_stats.chunksSaved;
        }
    }

    void ChunkProvider::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("ChunkProvider %s: %s", operation.c_str(), error.c_str());
    }

    void ChunkProvider::LogWarning(const std::string& operation, const std::string& warning) const {
        Log::Warning("ChunkProvider %s: %s", operation.c_str(), warning.c_str());
    }

    // === MOVE SEMANTICS ===

    void ChunkProvider::MoveFrom(ChunkProvider&& other) noexcept {
        m_config = std::move(other.m_config);
        m_chunkCache = std::move(other.m_chunkCache);
        m_chunkLoader = std::move(other.m_chunkLoader);
        m_chunkGenerator = std::move(other.m_chunkGenerator);
        m_chunkSaver = std::move(other.m_chunkSaver);
        m_dirtyTracker = std::move(other.m_dirtyTracker);
        m_stats = other.m_stats;
        m_lastAutoSave = other.m_lastAutoSave;
        m_initialized = other.m_initialized.load();
        m_shutdownRequested = other.m_shutdownRequested.load();

        // Clear other's state
        other.m_chunkCache.reset();
        other.m_chunkLoader.reset();
        other.m_chunkGenerator.reset();
        other.m_chunkSaver.reset();
        other.m_dirtyTracker.reset();
        other.m_stats.Reset();
        other.m_initialized = false;
        other.m_shutdownRequested = true;
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config) {
        return std::make_unique<ChunkProvider>(config);
    }

    ChunkProviderConfig CreateDefaultConfig() {
        ChunkProviderConfig config;
        config.maxLoadedChunks = 1024;
        config.enableLRUEviction = true;
        config.enableFallbackGeneration = true;
        config.enableAsyncSaving = true;
        config.enableAutoSave = true;
        config.autoSaveIntervalSeconds = 30.0f;
        config.maxChunksPerFrame = 4;
        config.maxLoadTimePerFrame = 10.0f;

        // Set up generation config
        config.generationConfig.seed = 12345;
        config.generationConfig.worldType = "default";
        config.generationConfig.generateOres = true;
        config.generationConfig.generateCaves = true;
        config.generationConfig.generateStructures = true;
        config.generationConfig.generateVegetation = true;

        // Set up dirty tracking config
        config.dirtyConfig.enableBatching = true;
        config.dirtyConfig.maxBatchSize = 50;
        config.dirtyConfig.batchTimeoutMs = 10.0f;
        config.dirtyConfig.enableNeighborInvalidation = true;

        return config;
    }

    ChunkProviderConfig CreatePerformanceConfig() {
        ChunkProviderConfig config = CreateDefaultConfig();

        // Optimize for performance
        config.maxLoadedChunks = 2048;           // More chunks in memory
        config.maxChunksPerFrame = 8;            // Load more chunks per frame
        config.maxLoadTimePerFrame = 20.0f;      // Allow more time for loading
        config.autoSaveIntervalSeconds = 60.0f;  // Save less frequently

        // Faster dirty tracking
        config.dirtyConfig.enableBatching = true;
        config.dirtyConfig.maxBatchSize = 100;
        config.dirtyConfig.batchTimeoutMs = 5.0f;

        return config;
    }

    ChunkProviderConfig CreateMemoryConfig() {
        ChunkProviderConfig config = CreateDefaultConfig();

        // Optimize for low memory usage
        config.maxLoadedChunks = 256;            // Fewer chunks in memory
        config.maxChunksPerFrame = 2;            // Load fewer chunks per frame
        config.maxLoadTimePerFrame = 5.0f;       // Shorter loading time
        config.autoSaveIntervalSeconds = 15.0f;  // Save more frequently

        // Aggressive eviction
        config.enableLRUEviction = true;

        // Smaller dirty tracking batches
        config.dirtyConfig.maxBatchSize = 20;
        config.dirtyConfig.batchTimeoutMs = 20.0f;

        return config;
    }

} // namespace Game