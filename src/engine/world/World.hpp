// File: src/engine/world/World.hpp
#pragma once

#include "../block/Blocks.hpp"
#include "../../game/WorldMath.hpp"
#include "ChunkProvider.hpp"
#include "Chunk.hpp"
#include <memory>
#include <shared_mutex>

namespace Game {

    class World {
    public:
        World();
        ~World() = default;

        // Block access methods
        BlockID GetBlock(int worldX, int worldY, int worldZ) const;
        bool SetBlock(int worldX, int worldY, int worldZ, BlockID blockId);

        // Validation
        static bool IsValidPosition(int x, int y, int z);

        // Chunk management
        void UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance);

        // Get chunk at chunk coordinates
        std::shared_ptr<Chunk> GetChunk(int chunkX, int chunkZ) const;

        // Check if chunk is loaded
        bool IsChunkLoaded(int chunkX, int chunkZ) const;

        // World coordinate conversion
        static Math::ChunkPos WorldToChunkPos(int worldX, int worldZ);
        static void WorldToLocal(int worldX, int worldY, int worldZ,
                                int& chunkX, int& chunkZ,
                                int& localX, int& localY, int& localZ);

    private:
        mutable std::shared_mutex worldMutex;

        // Helper methods
        std::shared_ptr<Chunk> GetOrLoadChunk(int chunkX, int chunkZ) const;

        // Chunk coordinate helpers
        static int WorldToChunkCoord(int worldCoord);
        static int WorldToLocalCoord(int worldCoord);
    };

} // namespace Game