// File: src/client/world/ClientBlockAccess.cpp
#include "ClientBlockAccess.hpp"
#include "ClientChunkManager.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/core/Config.hpp"

namespace Client {

    Game::BlockID ClientBlockAccess::GetBlock(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return Game::BlockID::Air;
        }

        if (!g_clientChunkManager) {
            return Game::BlockID::Air;
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
            return Game::BlockID::Air;
        }

        int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->GetBlock(localX, worldY, localZ);
    }

    bool ClientBlockAccess::IsChunkLoaded(int chunkX, int chunkZ) const {
        if (!g_clientChunkManager) {
            return false;
        }
        return g_clientChunkManager->IsChunkLoaded({chunkX, chunkZ});
    }

    bool ClientBlockAccess::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ)) {
            return false;
        }
        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos.x, chunkPos.z);
    }

    bool ClientBlockAccess::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        Game::BlockID block = GetBlock(worldX, worldY, worldZ);
        if (block == Game::BlockID::Air) {
            return false;
        }
        const Game::Block& blockInfo = Game::BlockRegistry::Get(block);
        return blockInfo.opaque;
    }

    bool ClientBlockAccess::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        Game::BlockID block = GetBlock(worldX, worldY, worldZ);
        return block == Game::BlockID::Water;
    }

    bool ClientBlockAccess::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= Config::MinY && worldY <= Config::MaxY;
    }

} // namespace Client
