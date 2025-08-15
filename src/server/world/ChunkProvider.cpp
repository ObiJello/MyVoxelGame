// File: src/server/world/ChunkProvider.cpp
#include "ChunkProvider.hpp"
#include "common/core/Log.hpp"
#include "storage/MinecraftChunkLoaderImpl.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>

namespace Game {

    // === CONSTRUCTION ===

    ChunkProvider::ChunkProvider(const ChunkProviderConfig& config)
        : m_config(config) {

        if (!m_config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, using defaults");
            m_config = ChunkProviderConfig{};
        }

        Log::Debug("ChunkProvider created");
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
        Log::Info("=== SIMPLIFIED CHUNKPROVIDER INITIALIZATION START ===");

        if (m_initialized) {
            Log::Warning("ChunkProvider already initialized");
            return true;
        }

        try {
            // Create core components
            Log::Info("Creating ChunkCache...");
            ChunkCacheConfig cacheConfig;
            // Calculate cache size based on render distance
            int renderDistance = Platform::g_gameSettings.GetRenderDistance();
            // Formula: (diameter)² + 20% margin for smooth operation
            size_t requiredChunks = (2 * renderDistance + 1) * (2 * renderDistance + 1);
            cacheConfig.maxSize = static_cast<size_t>(requiredChunks * 1.2); // 20% margin
            m_chunkCache = std::make_unique<ChunkCache>(cacheConfig);
            Log::Info("ChunkCache created with size %zu (for render distance %d)", cacheConfig.maxSize, renderDistance);

            // Create chunk generator
            Log::Info("Creating ProceduralChunkGenerator...");
            m_chunkGenerator = std::make_unique<ProceduralChunkGenerator>(m_config.generationConfig);
            Log::Info("ProceduralChunkGenerator created");

            // Create Minecraft loader if world path specified
            if (!m_config.minecraftWorldPath.empty()) {
                Log::Info("Creating MinecraftChunkLoaderImpl for: %s", m_config.minecraftWorldPath.c_str());
                MinecraftLoaderConfig loaderConfig;
                loaderConfig.worldPath = m_config.minecraftWorldPath;
                loaderConfig.enableFallbackGeneration = m_config.enableFallbackGeneration;
                m_chunkLoader = std::make_unique<MinecraftChunkLoaderImpl>(loaderConfig);
                Log::Info("MinecraftChunkLoaderImpl created");
            }

            // Create chunk saver
            Log::Info("Creating AsyncChunkSaver...");
            SaveWorkerConfig saverConfig;
            saverConfig.workerThreads = 1;
            saverConfig.enableAnvilFormat = !m_config.minecraftWorldPath.empty();
            saverConfig.minecraftWorldPath = m_config.minecraftWorldPath;
            m_chunkSaver = std::make_unique<AsyncChunkSaver>(saverConfig);
            Log::Info("AsyncChunkSaver created");

            // Create dirty tracker
            Log::Info("Creating DirtyTracker...");
            m_dirtyTracker = std::make_unique<DirtyTracker>(m_config.dirtyConfig);
            Log::Info("DirtyTracker created");

            // Initialize components
            if (m_chunkLoader && !m_chunkLoader->Initialize()) {
                Log::Warning("Failed to initialize chunk loader, will use generation only");
                m_chunkLoader.reset();
            }

            if (!m_chunkGenerator->Initialize()) {
                Log::Error("Failed to initialize chunk generator - this is critical!");
                return false;
            }

            if (!m_chunkSaver->Initialize()) {
                Log::Error("Failed to initialize chunk saver");
                return false;
            }

            if (!m_dirtyTracker->Initialize()) {
                Log::Error("Failed to initialize dirty tracker");
                return false;
            }

            // Set up component dependencies
            SetupComponentDependencies();
            ConfigureComponents();

            // Reset statistics
            ResetProviderStats();

            m_initialized = true;

            Log::Info("✓ ChunkProvider initialized successfully");
            Log::Info("=== SIMPLIFIED CHUNKPROVIDER INITIALIZATION COMPLETE ===");

            return true;

        } catch (const std::exception& e) {
            Log::Error("ChunkProvider initialization failed with exception: %s", e.what());
            Shutdown();
            return false;
        }
    }

    void ChunkProvider::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down ChunkProvider...");

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

        m_initialized = false;
        Log::Info("ChunkProvider shutdown complete");
    }

    // === CORE CHUNK OPERATIONS ===

    std::shared_ptr<Chunk> ChunkProvider::GetChunk(Math::ChunkPos position) {
        if (!m_initialized) {
            Log::Error("ChunkProvider::GetChunk: Not initialized");
            return nullptr;
        }

        if (!ValidateChunkPosition(position)) {
            Log::Error("ChunkProvider::GetChunk: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        // Always check cache first
        std::shared_ptr<Chunk> chunk = TryLoadFromCache(position);
        if (chunk) {
            return chunk;
        }

        // Load from disk or generate
        chunk = LoadChunkInternal(position);

        if (chunk) {
            chunk = CompleteChunkLoad(chunk);
            //Log::Info("Successfully loaded/generated chunk (%d, %d)", position.x, position.z);
        } else {
            Log::Warning("Failed to load/generate chunk (%d, %d)", position.x, position.z);
        }

        return chunk;
    }

    bool ChunkProvider::IsChunkLoaded(Math::ChunkPos position) const {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        return m_chunkCache->Contains(position);
    }

    bool ChunkProvider::UnloadChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        return m_chunkCache->Remove(position);
    }

    // === BLOCK ACCESS ===

    BlockID ChunkProvider::GetBlock(int worldX, int worldY, int worldZ) const {
        if (!ValidateWorldPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        auto chunk = m_chunkCache ? m_chunkCache->Get(chunkPos) : nullptr;
        if (!chunk) {
            return BlockID::Air;
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

        auto chunk = GetChunk(chunkPos);
        if (!chunk) {
            Log::Warning("Cannot set block at (%d, %d, %d) - chunk not available", worldX, worldY, worldZ);
            return;
        }

        chunk->SetBlock(localX, localY, localZ, block);

        // Mark chunk as dirty for saving
        if (m_chunkCache) {
            m_chunkCache->MarkDirty(chunkPos);
        }

        // Mark affected sections dirty for mesh rebuilding
        MarkBlockDirty(worldX, worldY, worldZ);
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
        NeighborStats stats;
        auto providerStats = GetProviderStats();
        stats.totalQueries = providerStats.chunksLoaded * 100;
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

        auto affectedSections = GetAffectedSections(worldX, worldY, worldZ, true);

        for (const auto& section : affectedSections) {
            m_dirtyTracker->MarkSectionDirty(section.chunkPos, section.sectionIndex);
        }
    }

    std::vector<DirtySection> ChunkProvider::GetDirtySections() {
        if (!m_initialized || !m_dirtyTracker) {
            return {};
        }

        return m_dirtyTracker->GetDirtySections();
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

        auto chunk = m_chunkCache->Get(position);
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

    // === CONFIGURATION ===

    void ChunkProvider::SetConfig(const ChunkProviderConfig& config) {
        if (!config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, ignoring");
            return;
        }

        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;

        if (m_initialized) {
            ConfigureComponents();
        }
    }

    ChunkProviderConfig ChunkProvider::GetConfig() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }

    void ChunkProvider::SetWorldPath(const std::string& path) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
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

        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.minecraftWorldPath;
    }

    void ChunkProvider::SetMaxLoadedChunks(size_t maxChunks) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        // Cache size is now managed directly by ChunkCache
        if (m_chunkCache) {
            ChunkCacheConfig cacheConfig;
            cacheConfig.maxSize = std::max(size_t(16), maxChunks);
            // Note: Would need to recreate cache to change size
        }
    }

    size_t ChunkProvider::GetMaxLoadedChunks() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        // Return actual cache size
        if (m_chunkCache) {
            return m_chunkCache->GetStats().maxSize;
        }
        return 2048; // Default
    }

    void ChunkProvider::SetGenerationSeed(int32_t seed) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
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

        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.generationConfig.seed;
    }

    // === STATISTICS ===

    ChunkProviderStats ChunkProvider::GetProviderStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ChunkProviderStats stats = m_stats;

        // Update real-time values
        if (m_chunkCache) {
            auto cacheStats = m_chunkCache->GetStats();
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

        if (m_chunkCache) m_chunkCache->ResetStats();
        if (m_chunkLoader) m_chunkLoader->ResetStats();
        if (m_chunkGenerator) m_chunkGenerator->ResetStats();
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
        Log::Info("  Memory: %zu KB, Dirty Sections: %zu",
                 stats.memoryUsage / 1024, stats.dirtySections);
    }

    bool ChunkProvider::ValidateState() const {
        if (!m_initialized) {
            return false;
        }

        if (m_dirtyTracker && !m_dirtyTracker->ValidateState()) {
            Log::Error("DirtyTracker validation failed");
            return false;
        }

        return true;
    }

    // === INTERNAL WORKFLOWS ===

    std::shared_ptr<Chunk> ChunkProvider::LoadChunkInternal(Math::ChunkPos position) {
        if (!ValidateChunkPosition(position)) {
            Log::Error("LoadChunkInternal: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        //Log::Debug("LoadChunkInternal: Loading chunk (%d, %d)", position.x, position.z);

        // Try loading from disk first
        std::shared_ptr<Chunk> chunk = TryLoadFromDisk(position);
        if (chunk) {
            Log::Debug("Loaded chunk (%d, %d) from disk", position.x, position.z);
            return chunk;
        }

        // Generate new chunk as fallback
        //Log::Debug("Generating chunk (%d, %d)", position.x, position.z);
        chunk = TryGenerateChunk(position);
        if (chunk) {
            //Log::Debug("Generated chunk (%d, %d)", position.x, position.z);
            return chunk;
        } else {
            Log::Error("Failed to generate chunk (%d, %d)", position.x, position.z);
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
            //Log::Debug("Generating chunk (%d, %d) using generator", position.x, position.z);
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

            result.chunk->pos = position;

            //Log::Info("Successfully generated chunk (%d, %d)", position.x, position.z);
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

        // Check if chunk is already in cache before adding
        if (m_chunkCache->Contains(chunk->pos)) {
            Log::Debug("Chunk (%d, %d) already in cache during CompleteChunkLoad, returning existing",
                      chunk->pos.x, chunk->pos.z);
            return m_chunkCache->Get(chunk->pos);
        }

        // Add to cache
        m_chunkCache->Put(chunk->pos, chunk);
        //Log::Debug("Added chunk (%d, %d) to cache", chunk->pos.x, chunk->pos.z);

        return chunk;
    }

    // === COORDINATION ===

    void ChunkProvider::SetupComponentDependencies() {
        if (!m_initialized) {
            return;
        }

        // Set up chunk cache dependencies
        if (m_chunkCache && m_chunkSaver) {
            std::shared_ptr<IChunkSaver> saverPtr(m_chunkSaver.get(), [](IChunkSaver*){});
            m_chunkCache->SetChunkSaver(saverPtr);
        }

        // Set up loader fallback generation
        if (m_chunkLoader && m_chunkGenerator) {
            std::shared_ptr<IChunkGenerator> generatorPtr(m_chunkGenerator.get(), [](IChunkGenerator*){});
            m_chunkLoader->SetFallbackGenerator(generatorPtr);
        }

        // Set up callbacks
        if (m_chunkCache) {
            m_chunkCache->SetEvictionCallback(
                [this](Math::ChunkPos pos, std::shared_ptr<Chunk> chunk, bool wasDirty) {
                    OnChunkEvicted(pos, chunk, wasDirty);
                });
        }
    }

    void ChunkProvider::ConfigureComponents() {
        std::lock_guard<std::mutex> lock(m_configMutex);

        // Configure loader
        if (m_chunkLoader) {
            m_chunkLoader->SetSource(m_config.minecraftWorldPath);
        }

        // Configure generator
        if (m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }

        // Configure dirty tracker
        if (m_dirtyTracker) {
            m_dirtyTracker->SetConfig(m_config.dirtyConfig);
        }
    }

    void ChunkProvider::OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty) {
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksEvicted++;
        }

        Log::Debug("Chunk (%d, %d) evicted from cache%s", position.x, position.z, wasDirty ? " (was dirty)" : "");
    }

    // === VALIDATION ===

    bool ChunkProvider::ValidateChunk(const std::shared_ptr<Chunk>& chunk) const {
        if (!chunk) {
            return false;
        }

        if (chunk->IsEmpty()) {
            return false;
        }

        return ValidateChunkPosition(chunk->pos);
    }

    bool ChunkProvider::ValidateChunkPosition(Math::ChunkPos position) const {
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
               std::abs(worldX) < 30000000 && std::abs(worldZ) < 30000000;
    }

    // === HELPERS ===

    void ChunkProvider::WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                                          int& localX, int& localY, int& localZ) const {
        chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        localX = worldX - (chunkPos.x * Math::CHUNK_SIZE_X);
        localY = worldY;
        localZ = worldZ - (chunkPos.z * Math::CHUNK_SIZE_Z);

        if (localX < 0) localX += Math::CHUNK_SIZE_X;
        if (localZ < 0) localZ += Math::CHUNK_SIZE_Z;
    }

    bool ChunkProvider::IsValidBlockID(BlockID block) const {
        return static_cast<int>(block) >= 0 && static_cast<int>(block) < 1000;
    }

    bool ChunkProvider::GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const {
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

    void ChunkProvider::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("ChunkProvider %s: %s", operation.c_str(), error.c_str());
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
        m_initialized = other.m_initialized.load();

        // Clear other's state
        other.m_chunkCache.reset();
        other.m_chunkLoader.reset();
        other.m_chunkGenerator.reset();
        other.m_chunkSaver.reset();
        other.m_dirtyTracker.reset();
        other.m_stats.Reset();
        other.m_initialized = false;
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config) {
        return std::make_unique<ChunkProvider>(config);
    }

    ChunkProviderConfig CreateDefaultConfig() {
        ChunkProviderConfig config;
        // ChunkCache now defaults to 2048

        // Set up generation config
        config.generationConfig.seed = 12345;
        config.generationConfig.worldType = "default";
        config.generationConfig.generateOres = true;
        config.generationConfig.generateCaves = true;
        config.generationConfig.generateStructures = true;
        config.generationConfig.generateVegetation = true;

        // Set up dirty tracking config
        config.dirtyConfig.enableNeighborInvalidation = true;

        return config;
    }

} // namespace Game