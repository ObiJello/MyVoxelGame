// File: src/engine/world/World.hpp
#pragma once

#include "../block/Blocks.hpp"
#include "../../game/WorldMath.hpp"
#include "Chunk.hpp"
#include "IBlockAccess.hpp"
#include <shared_mutex>

#include "glm/vec3.hpp"

namespace Game {

    // Forward declarations
    class ChunkProvider;

    class World : public IBlockAccess {
    public:
        World();
        ~World() = default;

        // === CORE SIMULATION INTERFACE ===

        // Block access methods (primary world interface)
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId);

        // Batch block operations
        void SetBlocks(const std::vector<std::tuple<int, int, int, BlockID>>& blocks);
        std::vector<BlockID> GetBlocks(const std::vector<std::tuple<int, int, int>>& positions) const;

        // === IBlockAccess IMPLEMENTATION ===

        // Chunk loading state queries
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;

        // Convenience methods for physics and other systems
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;

        // === COORDINATE UTILITIES ===

        // Validation
        static bool IsValidPosition(int x, int y, int z);
        static bool IsValidChunkPosition(int chunkX, int chunkZ);

        // World coordinate conversion
        static Math::ChunkPos WorldToChunkPos(int worldX, int worldZ);
        static void WorldToLocal(int worldX, int worldY, int worldZ,
                                int& chunkX, int& chunkZ,
                                int& localX, int& localY, int& localZ);
        static glm::ivec3 WorldToLocalCoords(int worldX, int worldY, int worldZ);

        // === SIMULATION QUERIES ===

        // Get loaded chunk count for statistics
        size_t GetLoadedChunkCount() const;

        // === FUTURE SIMULATION FEATURES ===

        // Tick simulation (entities, redstone, fluids, etc.)
        void Tick(float deltaTime);

        // Lighting system interface
        int GetLightLevel(int worldX, int worldY, int worldZ) const;
        void UpdateLighting(int worldX, int worldY, int worldZ);

        // Physics queries (already declared in IBlockAccess, but kept for backwards compatibility)
        bool IsBlockTransparent(int worldX, int worldY, int worldZ) const;

    private:
        mutable std::shared_mutex worldMutex;

        // === INTERNAL CHUNK ACCESS ===

        // Get chunk directly (no loading) - thread-safe
        std::shared_ptr<Chunk> GetChunkDirect(int chunkX, int chunkZ) const;

        // Coordinate helpers
        static int WorldToChunkCoord(int worldCoord);
        static int WorldToLocalCoord(int worldCoord);

        // === FUTURE SIMULATION STATE ===

        // World time and weather
        int64_t worldTime = 0;
        float timeOfDay = 0.0f;
        bool isRaining = false;

        // Performance tracking
        mutable size_t blockAccessCount = 0;
        mutable size_t chunkAccessCount = 0;
    };

} // namespace Game