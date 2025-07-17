// File: src/engine/world/ChunkProvider.hpp - FIXED VERSION
#pragma once

#include "Chunk.hpp"
#include "../../game/WorldMath.hpp"
#include "../../game/WorldCoordinates.hpp"
#include "../block/Blocks.hpp"
#include "../../core/Config.hpp"

// New composition dependencies
#include "cache/ChunkCache.hpp"
#include "generation/ProceduralChunkGenerator.hpp"
#include "saving/AsyncChunkSaver.hpp"
#include "tracking/DirtyTracker.hpp"
#include "interfaces/INeighborProvider.hpp"
#include "interfaces/IChunkLoader.hpp"

#include <memory>
#include <string>
#include <vector>
#include <future>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>

namespace Game {

    // Forward declarations
    class IChunkSaver;
    class IChunkGenerator;
    class AsyncChunkSaver;
    class MinecraftChunkLoaderImpl;
    struct ChunkSaveResult;

    // Configuration for ChunkProvider composition
    struct ChunkProviderConfig {
        // Cache settings
        size_t maxLoadedChunks = 1024;
        bool enableLRUEviction = true;
        
        // Loading settings
        std::string minecraftWorldPath;
        bool enableFallbackGeneration = true;
        
        // Saving settings
        bool enableAsyncSaving = true;
        bool enableAutoSave = true;
        float autoSaveIntervalSeconds = 30.0f;
        
        // Generation settings
        GenerationConfig generationConfig;
        
        // Dirty tracking settings
        DirtyTrackerConfig dirtyConfig;
        
        // Performance settings
        int maxChunksPerFrame = 4;
        float maxLoadTimePerFrame = 10.0f; // milliseconds
        
        bool IsValid() const {
            return maxLoadedChunks > 0 && 
                   maxChunksPerFrame > 0 && 
                   maxLoadTimePerFrame > 0.0f &&
                   generationConfig.IsValid();
        }
    };

    // Legacy DirtySection struct for backward compatibility
    using DirtySection = Game::DirtySection;

    // Statistics for the composed chunk provider
    struct ChunkProviderStats {
        size_t chunksLoaded = 0;
        size_t chunksGenerated = 0;
        size_t chunksSaved = 0;
        size_t chunksEvicted = 0;
        size_t dirtySections = 0;
        
        float averageLoadTime = 0.0f;
        float averageGenerationTime = 0.0f;
        float averageSaveTime = 0.0f;
        
        size_t cacheHitRate = 0;
        size_t memoryUsage = 0;
        
        void Reset() {
            chunksLoaded = chunksGenerated = chunksSaved = chunksEvicted = 0;
            dirtySections = 0;
            averageLoadTime = averageGenerationTime = averageSaveTime = 0.0f;
            cacheHitRate = memoryUsage = 0;
        }
    };

    // Load request for async chunk loading
    struct ChunkLoadRequest {
        Math::ChunkPos position;
        bool highPriority;
        std::shared_ptr<std::promise<std::shared_ptr<Chunk>>> promise;
        
        ChunkLoadRequest(Math::ChunkPos pos, bool priority = false)
            : position(pos), highPriority(priority)
            , promise(std::make_shared<std::promise<std::shared_ptr<Chunk>>>()) {}
    };

    /**
     * Refactored ChunkProvider using composition pattern
     * 
     * This class coordinates between:
     * - ChunkCache: In-memory chunk storage with LRU eviction
     * - MinecraftChunkLoaderImpl: Loading chunks from Minecraft region files
     * - ProceduralChunkGenerator: Generating new chunks when not found
     * - AsyncChunkSaver: Saving dirty chunks to disk
     * - DirtyTracker: Tracking sections that need mesh updates
     * 
     * Responsibilities:
     * - Coordinate loading, generation, and saving workflows
     * - Manage chunk lifecycle and dependencies
     * - Provide INeighborProvider interface for meshing
     * - Handle dirty tracking for mesh invalidation
     * - Maintain performance targets and statistics
     */
    class ChunkProvider : public INeighborProvider {
    public:
        explicit ChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});
        ~ChunkProvider();

        // Non-copyable, movable
        ChunkProvider(const ChunkProvider&) = delete;
        ChunkProvider& operator=(const ChunkProvider&) = delete;
        ChunkProvider(ChunkProvider&& other) noexcept;
        ChunkProvider& operator=(ChunkProvider&& other) noexcept;

        // === LIFECYCLE ===
        
        bool Initialize();
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }
        
        // Regular update for maintenance tasks
        void Update(float deltaTime);

        // === CORE CHUNK OPERATIONS ===

        // Get chunk (loads if necessary, returns null if not available)
        std::shared_ptr<Chunk> GetChunk(Math::ChunkPos position);
        
        // Get chunk with specific priority
        std::shared_ptr<Chunk> GetChunkWithPriority(Math::ChunkPos position, bool highPriority = false);

        // Load chunk asynchronously
        std::future<std::shared_ptr<Chunk>> LoadChunkAsync(Math::ChunkPos position);

        // Check if chunk is loaded
        bool IsChunkLoaded(Math::ChunkPos position) const;

        // Preload chunks for better performance
        void PreloadChunk(Math::ChunkPos position);
        void PreloadArea(Math::ChunkPos center, int radius);

        // Unload chunk and save if dirty
        bool UnloadChunk(Math::ChunkPos position);
        void UnloadArea(Math::ChunkPos center, int radius);

        // === BLOCK ACCESS ===

        // Get/set blocks using world coordinates
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        void SetBlock(int worldX, int worldY, int worldZ, BlockID block);

        // Batch block operations
        void SetBlocks(const std::vector<std::tuple<int, int, int, BlockID>>& blocks);
        std::vector<BlockID> GetBlocks(const std::vector<std::tuple<int, int, int>>& positions) const;

        // === INEIGHBORPROVIDER IMPLEMENTATION ===

        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockTransparent(int worldX, int worldY, int worldZ) const override;

        NeighborStats GetStats() const override;
        void ResetStats() override;

        // === DIRTY TRACKING ===

        // Mark sections dirty for mesh updates
        void MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex);
        void MarkChunkDirty(Math::ChunkPos chunkPos);
        void MarkBlockDirty(int worldX, int worldY, int worldZ);
        void MarkRegionDirty(int minX, int minY, int minZ, int maxX, int maxY, int maxZ);

        // Get dirty sections for mesh rebuilding
        std::vector<DirtySection> GetDirtySections(size_t maxCount = SIZE_MAX);
        std::vector<DirtySection> GetAndClearDirtySections();
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Check dirty state
        bool IsChunkDirty(Math::ChunkPos chunkPos) const;
        bool IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const;
        size_t GetDirtyCount() const;

        // === SAVING ===

        // Save operations
        void SaveChunk(Math::ChunkPos position);
        void SaveAllDirtyChunks();
        void SaveArea(Math::ChunkPos center, int radius);

        // Auto-save configuration
        void SetAutoSaveEnabled(bool enabled);
        void SetAutoSaveInterval(float seconds);
        bool IsAutoSaveEnabled() const;

        // === CONFIGURATION ===

        void SetConfig(const ChunkProviderConfig& config);
        ChunkProviderConfig GetConfig() const;

        // Component configuration
        void SetWorldPath(const std::string& path);
        std::string GetWorldPath() const;

        void SetMaxLoadedChunks(size_t maxChunks);
        size_t GetMaxLoadedChunks() const;

        void SetGenerationSeed(int32_t seed);
        int32_t GetGenerationSeed() const;

        // === STATISTICS ===

        ChunkProviderStats GetProviderStats() const;
        void ResetProviderStats();

        // Component statistics
        ChunkCache::CacheStats GetCacheStats() const;
        IChunkLoader::LoaderStats GetLoaderStats() const;
        ProceduralChunkGenerator::GeneratorStats GetGeneratorStats() const;
        IChunkSaver::SaverStats getSaverStats() const;
        DirtyTrackerStats GetDirtyTrackerStats() const;

        // === DIAGNOSTICS ===

        // Memory usage
        size_t GetMemoryUsage() const;
        size_t GetLoadedChunkCount() const;

        // Performance monitoring
        void LogPerformanceStats() const;
        void LogComponentStats() const;

        // Debug validation
        bool ValidateState() const;
        void DumpLoadedChunks() const;

        // === LEGACY COMPATIBILITY ===

        // For compatibility with existing code that expects these methods
        std::shared_ptr<Chunk> LoadChunkFromDisk(Math::ChunkPos position);
        std::shared_ptr<Chunk> GenerateChunk(Math::ChunkPos position);
        void QueueChunkForSaving(std::shared_ptr<const Chunk> chunk);

        // Legacy dirty tracking
        void AddDirtySection(const DirtySection& section);
        std::vector<DirtySection> RemoveDirtySections();

    private:
        // Configuration
        ChunkProviderConfig m_config;
        mutable std::shared_mutex m_configMutex;

        // Core components (composition instead of inheritance)
        std::unique_ptr<ChunkCache> m_chunkCache;
        std::unique_ptr<MinecraftChunkLoaderImpl> m_chunkLoader;
        std::unique_ptr<ProceduralChunkGenerator> m_chunkGenerator;
        std::unique_ptr<IChunkSaver> m_chunkSaver;
        std::unique_ptr<DirtyTracker> m_dirtyTracker;

        // State
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_shutdownRequested{false};

        // Performance tracking
        mutable std::mutex m_statsMutex;
        ChunkProviderStats m_stats;
        std::chrono::steady_clock::time_point m_lastAutoSave;

        // Async loading support
        mutable std::mutex m_loadRequestMutex;
        std::queue<ChunkLoadRequest> m_loadRequests;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_pendingLoads;

        // === INTERNAL WORKFLOWS ===

        // Core loading workflow
        std::shared_ptr<Chunk> LoadChunkInternal(Math::ChunkPos position, bool allowGeneration = true);

        // Try loading from cache first
        std::shared_ptr<Chunk> TryLoadFromCache(Math::ChunkPos position);

        // Try loading from disk
        std::shared_ptr<Chunk> TryLoadFromDisk(Math::ChunkPos position);

        // Generate new chunk
        std::shared_ptr<Chunk> TryGenerateChunk(Math::ChunkPos position);

        // Complete chunk loading (add to cache, etc.)
        std::shared_ptr<Chunk> CompleteChunkLoad(std::shared_ptr<Chunk> chunk);

        // === COORDINATION ===

        // Coordinate between components
        void SetupComponentDependencies();
        void ConfigureComponents();
        void SynchronizeComponents();

        // Event handlers for component interactions
        void OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty);
        void OnChunkSaved(const ChunkSaveResult& result);
        void OnSectionDirty(const DirtySection& section);

        // === MAINTENANCE ===

        // Regular maintenance tasks
        void PerformMaintenance(float deltaTime);
        void UpdateAutoSave(float deltaTime);
        void ProcessLoadRequests();
        void UpdateComponentStats();

        // Performance enforcement
        void EnforcePerformanceLimits();
        bool ShouldThrottleLoading() const;

        // === VALIDATION ===

        bool ValidateChunk(const std::shared_ptr<Chunk>& chunk) const;
        bool ValidateChunkPosition(Math::ChunkPos position) const;
        bool ValidateWorldPosition(int worldX, int worldY, int worldZ) const;

        // === HELPERS ===

        // Coordinate conversion helpers
        void WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                               int& localX, int& localY, int& localZ) const;

        // Block property helpers
        bool IsValidBlockID(BlockID block) const;
        bool GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const;

        // Statistics helpers
        void UpdateLoadStats(float loadTime, bool fromCache, bool fromDisk, bool generated);
        void UpdateSaveStats(float saveTime, bool success);

        // Error handling
        void LogError(const std::string& operation, const std::string& error) const;
        void LogWarning(const std::string& operation, const std::string& warning) const;

        // === MOVE SEMANTICS ===

        void MoveFrom(ChunkProvider&& other) noexcept;
    };

    // === UTILITY FUNCTIONS ===

    // Factory function for creating configured chunk providers
    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});

    // Configuration helpers
    ChunkProviderConfig CreateDefaultConfig();
    ChunkProviderConfig CreatePerformanceConfig();  // Optimized for performance
    ChunkProviderConfig CreateMemoryConfig();       // Optimized for low memory usage

} // namespace Game