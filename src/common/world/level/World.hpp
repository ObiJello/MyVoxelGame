// File: src/common/world/level/World.hpp
#pragma once

#include "../chunk/IBlockAccess.hpp"
#include "server/world/ChunkProvider.hpp"
#include "../block/Blocks.hpp"
#include "../math/WorldMath.hpp"
#include "server/world/tracking/DirtyTracker.hpp"
#include <memory>
#include <atomic>
#include <glm/glm.hpp>

namespace Game {

    class World : public IBlockAccess {
    public:
        // Update flags for SetBlock operations (bitfield)
        enum UpdateFlags : uint32_t {
            None              = 0,
            NotifyNeighbors   = 1 << 0,  // Notify neighboring blocks of change
            UpdateShapes      = 1 << 1,  // Update block shapes (for fences, walls, etc.) - TODO
            RecomputeLight    = 1 << 2,  // Recalculate lighting - TODO
            UpdateHeightmap   = 1 << 3,  // Update chunk heightmap - TODO
            MarkDirty         = 1 << 4,  // Mark section dirty for mesh rebuild
            NoDrops           = 1 << 6,  // Don't drop items when breaking - TODO
            
            // Common flag combinations
            All = NotifyNeighbors | UpdateShapes | RecomputeLight | UpdateHeightmap | MarkDirty,
            AllNoDrops = All | NoDrops
        };
        
        World();
        ~World();

        // Core world operations
        void Initialize();
        bool InitializeChunkProvider();
        void Shutdown();

        // IBlockAccess implementation
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;

        // World modification
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId);
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId, uint32_t updateFlags);

        // Mesh system integration
        void MarkSectionDirty(int worldX, int worldY, int worldZ);
        bool HasDirtySections() const;

        // Get dirty sections for mesh rebuilding
        std::vector<DirtySection> GetDirtySections();
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Get loaded chunk count for debugging
        size_t GetLoadedChunkCount() const;

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

        // Signal the world to stop all long-running operations (called from shutdown)
        void RequestStop() {
            m_stopRequested.store(true);
            // Abort blocking getChunk() loops in the terrain library
            if (m_chunkProvider && m_chunkProvider->GetGenerator()) {
                m_chunkProvider->GetGenerator()->RequestAbort();
            }
        }
        bool IsStopRequested() const { return m_stopRequested.load(); }

        // ========================================================================
        // SERVER WORLD LOOP
        // ========================================================================

        // Main world tick function called from server thread
        // Handles simulation only (block ticks, entities, etc.)
        // Chunk loading is driven by the session system, NOT by World.
        void WorldLoop(float deltaTime);

        // Block update processing
        void ProcessBlockUpdates();

        // Random block ticks (like crop growth, ice melting, etc.)
        void PerformRandomBlockTick();

        // Process scheduled block events
        void ProcessBlockEvents();

        // Update tile entities
        void TileEntityTick();

        // Update entities
        void EntityTick();

        // Update world time and weather
        void WorldTimeWeatherTick();

    private:
        std::unique_ptr<ChunkProvider> m_chunkProvider;
        std::string m_minecraftWorldPath;

        // Helper functions
        void OnBlockChanged(int worldX, int worldY, int worldZ);
        void NotifyNeighborBlocks(int worldX, int worldY, int worldZ);
        Math::ChunkPos WorldToChunkPos(int worldX, int worldZ) const;
        void MarkNeighboringSectionsIfNeeded(int worldX, int worldY, int worldZ);

        // Statistics
        mutable size_t m_blockAccessCount = 0;

        // Stop flag for early termination of long-running loops
        std::atomic<bool> m_stopRequested{false};
    };

} // namespace Game