// File: src/engine/world/World.cpp (FIXED - Delegates to ChunkProvider)
#include "World.hpp"
#include "ChunkProvider.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include "block/BlockRegistry.hpp"

namespace Game {

    World::World() {
        Log::Info("World simulation system initialized");
    }

    // === CORE SIMULATION INTERFACE ===

    BlockID World::GetBlock(int worldX, int worldY, int worldZ) const {
        // Validate coordinates first
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        // Convert to chunk coordinates
        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        // Get chunk directly (no loading - simulation layer doesn't load chunks)
        auto chunk = GetChunkDirect(chunkX, chunkZ);
        if (!chunk) {
            // Chunk not loaded - this is normal and expected
            return BlockID::Air;
        }

        // Get block from chunk
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        blockAccessCount++;
        return chunk->GetBlock(localX, worldY, localZ);
    }

    bool World::SetBlock(int worldX, int worldY, int worldZ, BlockID blockId) {
        // Validate coordinates
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            Log::Debug("SetBlock failed: invalid position (%d, %d, %d)", worldX, worldY, worldZ);
            return false;
        }

        // Convert to chunk coordinates
        int chunkX, chunkZ, localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunkX, chunkZ, localX, localY, localZ);

        // Get chunk directly (no loading - if chunk isn't loaded, we can't set blocks)
        auto chunk = GetChunkDirect(chunkX, chunkZ);
        if (!chunk) {
            Log::Debug("SetBlock failed: chunk (%d, %d) not loaded", chunkX, chunkZ);
            return false;
        }

        // Set block in chunk
        std::unique_lock<std::shared_mutex> lock(worldMutex);
        chunk->SetBlock(localX, worldY, localZ, blockId);

        Log::Debug("SetBlock success: (%d, %d, %d) = %d", worldX, worldY, worldZ, static_cast<int>(blockId));
        return true;
    }

    void World::SetBlocks(const std::vector<std::tuple<int, int, int, BlockID>>& blocks) {
        std::unique_lock<std::shared_mutex> lock(worldMutex);

        for (const auto& [x, y, z, blockId] : blocks) {
            // Note: We unlock/relock for each block to avoid holding the lock too long
            lock.unlock();
            SetBlock(x, y, z, blockId);
            lock.lock();
        }
    }

    std::vector<BlockID> World::GetBlocks(const std::vector<std::tuple<int, int, int>>& positions) const {
        std::vector<BlockID> results;
        results.reserve(positions.size());

        std::shared_lock<std::shared_mutex> lock(worldMutex);

        for (const auto& [x, y, z] : positions) {
            lock.unlock();
            results.push_back(GetBlock(x, y, z));
            lock.lock();
        }

        return results;
    }

    // === COORDINATE UTILITIES ===

    bool World::IsValidPosition(int x, int y, int z) {
        return y >= Config::MinY && y <= Config::MaxY;
        // Note: X and Z coordinates are theoretically unlimited in Minecraft
    }

    bool World::IsValidChunkPosition(int chunkX, int chunkZ) {
        // In practice, chunk coordinates are limited by the world border
        // For now, we'll allow any chunk coordinates
        return true;
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

    glm::ivec3 World::WorldToLocalCoords(int worldX, int worldY, int worldZ) {
        return glm::ivec3(
            WorldToLocalCoord(worldX),
            worldY,
            WorldToLocalCoord(worldZ)
        );
    }

    // === SIMULATION QUERIES ===

    bool World::IsChunkLoaded(int chunkX, int chunkZ) const {
        return GetChunkDirect(chunkX, chunkZ) != nullptr;
    }

    bool World::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }

        Math::ChunkPos chunkPos = WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos.x, chunkPos.z);
    }

    size_t World::GetLoadedChunkCount() const {
        // Delegate to ChunkProvider for this information
        return ChunkProvider::GetLoadedChunkCount();
    }

    // === SIMULATION FEATURES ===

    void World::Tick(float deltaTime) {
        // Future: Implement world simulation ticking
        // - Entity updates
        // - Redstone simulation
        // - Fluid flow
        // - Random tick blocks (grass spread, crop growth, etc.)
        // - Weather updates

        worldTime++;
        timeOfDay = (worldTime % 24000) / 24000.0f;

        // Update performance counters
        std::unique_lock<std::shared_mutex> lock(worldMutex);
        if (worldTime % 1200 == 0) { // Every minute
            Log::Debug("World simulation stats: %zu block accesses, %zu chunk accesses",
                      blockAccessCount, chunkAccessCount);
            blockAccessCount = 0;
            chunkAccessCount = 0;
        }
    }

    int World::GetLightLevel(int worldX, int worldY, int worldZ) const {
        // Future: Implement lighting system
        // For now, return full brightness during day, darker at night
        if (timeOfDay >= 0.25f && timeOfDay <= 0.75f) {
            return 15; // Day
        } else {
            return 4;  // Night
        }
    }

    void World::UpdateLighting(int worldX, int worldY, int worldZ) {
        // Future: Implement lighting updates
        // Would propagate light changes through neighbors
    }

    bool World::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        BlockID blockId = GetBlock(worldX, worldY, worldZ);
        if (blockId == BlockID::Air) {
            return false;
        }

        const Block& block = BlockRegistry::Get(blockId);
        return block.opaque;
    }

    bool World::IsBlockTransparent(int worldX, int worldY, int worldZ) const {
        return !IsBlockSolid(worldX, worldY, worldZ);
    }

    bool World::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        BlockID blockId = GetBlock(worldX, worldY, worldZ);
        return blockId == BlockID::Water || blockId == BlockID::Lava;
    }

    // === INTERNAL CHUNK ACCESS ===

    std::shared_ptr<Chunk> World::GetChunkDirect(int chunkX, int chunkZ) const {
        // **KEY CHANGE**: Completely delegate to ChunkProvider - no direct registry access
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        chunkAccessCount++;

        // Let ChunkProvider handle all chunk registry access
        // This is a read-only operation that doesn't trigger loading
        return ChunkProvider::GetChunkIfLoaded({chunkX, chunkZ});
    }

    // === COORDINATE HELPERS ===

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