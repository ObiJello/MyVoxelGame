// File: src/engine/world/World.cpp
#include "World.hpp"
#include "ChunkProvider.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include <algorithm>

namespace Game {

    World::World() {
        Log::Info("World system initialized");
    }

    BlockID World::GetBlock(int worldX, int worldY, int worldZ) const {
        // Validate coordinates
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        // Convert to chunk coordinates
        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        // Get chunk
        auto chunk = GetChunk(chunkX, chunkZ);
        if (!chunk) {
            return BlockID::Air; // Chunk not loaded
        }

        // Get block from chunk
        return chunk->GetBlock(localX, worldY, localZ);
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId) {
        // Validate coordinates
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }

        // Convert to chunk coordinates
        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        // Get or load chunk
        auto chunk = GetOrLoadChunk(chunkX, chunkZ);
        if (!chunk) {
            return false; // Failed to load chunk
        }

        // Set block in chunk
        std::unique_lock<std::shared_mutex> lock(worldMutex);
        chunk->SetBlock(localX, worldY, localZ, blockId);

        return true;
    }

    bool World::IsValidPosition(int x, int y, int z) {
        return y >= Config::MinY && y <= Config::MaxY;
    }

    std::shared_ptr<Chunk> World::GetChunk(int chunkX, int chunkZ) const {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
                       static_cast<uint32_t>(chunkZ);

        std::shared_lock<std::shared_mutex> lock(s_registryMutex);
        auto it = s_chunkRegistry.find(key);

        if (it != s_chunkRegistry.end() && it->second->isGenerated.load()) {
            return it->second->chunk;
        }

        return nullptr;
    }

    bool World::IsChunkLoaded(int chunkX, int chunkZ) const {
        return GetChunk(chunkX, chunkZ) != nullptr;
    }

    std::shared_ptr<Chunk> World::GetOrLoadChunk(int chunkX, int chunkZ) const {
        // Try to get existing chunk first
        auto chunk = GetChunk(chunkX, chunkZ);
        if (chunk) {
            return chunk;
        }

        // Request chunk loading if not available
        ChunkProvider::RequestChunk({chunkX, chunkZ});

        // For now, return nullptr - in a real implementation you might
        // want to wait for the chunk to load or return a placeholder
        return nullptr;
    }

    void World::UpdateLoadedChunks(int playerChunkX, int playerChunkZ, int viewDistance) {
        // Request chunks around player position
        for (int dx = -viewDistance; dx <= viewDistance; ++dx) {
            for (int dz = -viewDistance; dz <= viewDistance; ++dz) {
                int chunkX = playerChunkX + dx;
                int chunkZ = playerChunkZ + dz;

                // Check if chunk is within circular view distance
                float distance = std::sqrt(dx * dx + dz * dz);
                if (distance <= viewDistance) {
                    if (!IsChunkLoaded(chunkX, chunkZ)) {
                        ChunkProvider::RequestChunk({chunkX, chunkZ});
                    }
                }
            }
        }
    }

    Math::ChunkPos World::WorldToChunkPos(int worldX, int worldZ) {
        return {WorldToChunkCoord(worldX), WorldToChunkCoord(worldZ)};
    }

    void World::WorldToLocal(int worldX, int worldY, int worldZ,
                            int& chunkX, int& chunkZ,
                            int& localX, int& localY, int& localZ) {
        chunkX = WorldToChunkCoord(worldX);
        chunkZ = WorldToChunkCoord(worldZ);

        localX = WorldToLocalCoord(worldX);
        localY = worldY; // Y coordinate is already world-relative
        localZ = WorldToLocalCoord(worldZ);
    }

    int World::WorldToChunkCoord(int worldCoord) {
        if (worldCoord >= 0) {
            return worldCoord / Game::Math::CHUNK_SIZE_X;
        } else {
            return (worldCoord + 1) / Game::Math::CHUNK_SIZE_X - 1;
        }
    }

    int World::WorldToLocalCoord(int worldCoord) {
        int local = worldCoord % Game::Math::CHUNK_SIZE_X;
        if (local < 0) {
            local += Game::Math::CHUNK_SIZE_X;
        }
        return local;
    }

} // namespace Game