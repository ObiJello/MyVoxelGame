// File: src/engine/world/ChunkProvider.hpp - SIMPLIFIED VERSION
#pragma once

#include "Chunk.hpp"
#include "../../game/WorldMath.hpp"
#include "../../game/WorldCoordinates.hpp"
#include "../block/Blocks.hpp"
#include "../../core/Config.hpp"

// Dependencies
#include "cache/ChunkCache.hpp"
#include "generation/ProceduralChunkGenerator.hpp"
#include "saving/AsyncChunkSaver.hpp"
#include "tracking/DirtyTracker.hpp"
#include "interfaces/INeighborProvider.hpp"
#include "interfaces/IChunkLoader.hpp"

#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace Game {

    // Forward declarations
    class IChunkSaver;
    class IChunkGenerator;
    class MinecraftChunkLoaderImpl;

    // Simple configuration
    struct ChunkProviderConfig {
        // Cache settings
        size_t maxLoadedChunks = 1024;

        // Loading settings
        std::string minecraftWorldPath;
        bool enableFallbackGeneration = true;

        // Generation settings
        GenerationConfig generationConfig;

        // Dirty tracking settings
        DirtyTrackerConfig dirtyConfig;

        bool IsValid() const {
            return maxLoadedChunks > 0 && generationConfig.IsValid();
        }
    };

    // Simple statistics
    struct ChunkProviderStats {
        size_t chunksLoaded = 0;
        size_t chunksGenerated = 0;
        size_t chunksSaved = 0;
        size_t chunksEvicted = 0;
        size_t dirtySections = 0;
        size_t memoryUsage = 0;

        void Reset() {
            chunksLoaded = chunksGenerated = chunksSaved = chunksEvicted = 0;
            dirtySections = memoryUsage = 0;
        }
    };

    // Simplified ChunkProvider using composition pattern
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

        void Update(float deltaTime);

        // === CORE CHUNK OPERATIONS ===

        // Get chunk (loads if necessary, returns null if not available)
        std::shared_ptr<Chunk> GetChunk(Math::ChunkPos position);

        // Check if chunk is loaded
        bool IsChunkLoaded(Math::ChunkPos position) const;

        // Unload chunk and save if dirty
        bool UnloadChunk(Math::ChunkPos position);

        // === BLOCK ACCESS ===

        // Get/set blocks using world coordinates
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        void SetBlock(int worldX, int worldY, int worldZ, BlockID block);

        // === INEIGHBORPROVIDER IMPLEMENTATION ===

        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockTransparent(int worldX, int worldY, int worldZ) const override;

        NeighborStats GetStats() const override;
        void ResetStats() override;

        // === DIRTY TRACKING ===

        void MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex);
        void MarkChunkDirty(Math::ChunkPos chunkPos);
        void MarkBlockDirty(int worldX, int worldY, int worldZ);

        std::vector<DirtySection> GetDirtySections(size_t maxCount = SIZE_MAX);
        std::vector<DirtySection> GetAndClearDirtySections();
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        bool IsChunkDirty(Math::ChunkPos chunkPos) const;
        bool IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const;
        size_t GetDirtyCount() const;

        // === SAVING ===

        void SaveChunk(Math::ChunkPos position);
        void SaveAllDirtyChunks();

        // === CONFIGURATION ===

        void SetConfig(const ChunkProviderConfig& config);
        ChunkProviderConfig GetConfig() const;

        void SetWorldPath(const std::string& path);
        std::string GetWorldPath() const;

        void SetMaxLoadedChunks(size_t maxChunks);
        size_t GetMaxLoadedChunks() const;

        void SetGenerationSeed(int32_t seed);
        int32_t GetGenerationSeed() const;

        // === STATISTICS ===

        ChunkProviderStats GetProviderStats() const;
        void ResetProviderStats();

        ChunkCache::CacheStats GetCacheStats() const;
        IChunkLoader::LoaderStats GetLoaderStats() const;
        ProceduralChunkGenerator::GeneratorStats GetGeneratorStats() const;
        DirtyTrackerStats GetDirtyTrackerStats() const;

        // === DIAGNOSTICS ===

        size_t GetMemoryUsage() const;
        size_t GetLoadedChunkCount() const;

        void LogPerformanceStats() const;
        bool ValidateState() const;

    private:
        // Configuration
        ChunkProviderConfig m_config;
        mutable std::mutex m_configMutex;

        // Core components
        std::unique_ptr<ChunkCache> m_chunkCache;
        std::unique_ptr<MinecraftChunkLoaderImpl> m_chunkLoader;
        std::unique_ptr<ProceduralChunkGenerator> m_chunkGenerator;
        std::unique_ptr<IChunkSaver> m_chunkSaver;
        std::unique_ptr<DirtyTracker> m_dirtyTracker;

        // State
        std::atomic<bool> m_initialized{false};

        // Statistics
        mutable std::mutex m_statsMutex;
        ChunkProviderStats m_stats;

        // === INTERNAL WORKFLOWS ===

        std::shared_ptr<Chunk> LoadChunkInternal(Math::ChunkPos position);
        std::shared_ptr<Chunk> TryLoadFromCache(Math::ChunkPos position);
        std::shared_ptr<Chunk> TryLoadFromDisk(Math::ChunkPos position);
        std::shared_ptr<Chunk> TryGenerateChunk(Math::ChunkPos position);
        std::shared_ptr<Chunk> CompleteChunkLoad(std::shared_ptr<Chunk> chunk);

        // === COORDINATION ===

        void SetupComponentDependencies();
        void ConfigureComponents();

        void OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty);

        // === VALIDATION ===

        bool ValidateChunk(const std::shared_ptr<Chunk>& chunk) const;
        bool ValidateChunkPosition(Math::ChunkPos position) const;
        bool ValidateWorldPosition(int worldX, int worldY, int worldZ) const;

        // === HELPERS ===

        void WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                               int& localX, int& localY, int& localZ) const;

        bool IsValidBlockID(BlockID block) const;
        bool GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const;

        void LogError(const std::string& operation, const std::string& error) const;

        // === MOVE SEMANTICS ===

        void MoveFrom(ChunkProvider&& other) noexcept;
    };

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});

    ChunkProviderConfig CreateDefaultConfig();

} // namespace Game