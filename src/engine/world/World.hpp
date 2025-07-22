// File: src/engine/world/World.hpp
#pragma once

#include "IBlockAccess.hpp"
#include "ChunkProvider.hpp"
#include "../block/Blocks.hpp"
#include "../../game/WorldMath.hpp"
#include "tracking/DirtyTracker.hpp"
#include <memory>
#include <glm/glm.hpp>

namespace Game {

    class World : public IBlockAccess {
    public:
        World();
        ~World();

        // Core world operations
        void Initialize();
        void Update(float deltaTime);
        void Shutdown();

        // Refresh settings from game settings
        void RefreshSettings();

        // IBlockAccess implementation
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;

        // World modification
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId);

        // Simplified chunk management - loads ALL chunks in square pattern
        void UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance = 0);

        // Mesh system integration
        void MarkSectionDirty(int worldX, int worldY, int worldZ);
        bool HasDirtySections() const;

        // Get dirty sections for mesh rebuilding
        std::vector<DirtySection> GetDirtySections(size_t maxCount = SIZE_MAX);
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Get loaded chunk count for debugging
        size_t GetLoadedChunkCount() const;

        // Get current render distance from settings
        int GetRenderDistance() const { return m_renderDistance; }

        // World bounds (from Config)
        static constexpr int MIN_Y = -64;
        static constexpr int MAX_Y = 319;
        static constexpr int WORLD_HEIGHT = MAX_Y - MIN_Y + 1;

        // Minecraft world support
        void SetMinecraftWorldPath(const std::string& worldPath);
        const std::string& GetMinecraftWorldPath() const;
        bool HasMinecraftWorld() const;

        // Provide chunk access for mesh system
        std::shared_ptr<Chunk> GetChunk(int chunkX, int chunkZ) const;

        // Convenience method for mesh manager
        const Chunk* GetChunkForMeshing(int chunkX, int chunkZ) const;

        // Performance and debugging
        void LogPerformanceStats();
        void SaveAllChunks();
        size_t GetMemoryUsage() const;
        ChunkProviderStats GetChunkProviderStats() const;

        // World generation control
        void SetGenerationSeed(int32_t seed);
        int32_t GetGenerationSeed() const;

        // Direct access to chunk provider for advanced use cases
        ChunkProvider* GetChunkProvider() const { return m_chunkProvider.get(); }

    private:
        std::unique_ptr<ChunkProvider> m_chunkProvider;
        std::string m_minecraftWorldPath;

        // Settings-based configuration
        int m_renderDistance = 10; // Will be loaded from settings

        // Helper functions
        void OnBlockChanged(int worldX, int worldY, int worldZ);
        Math::ChunkPos WorldToChunkPos(int worldX, int worldZ) const;
        void MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ);

        // Square-based chunk unloading helper
        void UnloadDistantChunks(int centerX, int centerZ, int keepDistance);

        // Load settings from game settings
        void LoadWorldSettings();

        // Statistics
        mutable size_t m_blockAccessCount = 0;
    };

} // namespace Game