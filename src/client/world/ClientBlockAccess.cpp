// File: src/client/world/ClientBlockAccess.cpp
#include "ClientBlockAccess.hpp"
#include "common/world/biome/Biomes.hpp"
#include "ClientChunkManager.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/core/Config.hpp"

namespace Client {

    ClientBlockAccess* g_clientBlockAccess = nullptr;

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

    uint16_t ClientBlockAccess::GetBiome(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !g_clientChunkManager) {
            return Game::kFallbackBiomeId;
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!chunk || !chunk->chunkData) {
            return Game::kFallbackBiomeId;
        }

        const int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        const int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->GetBiome(localX, worldY, localZ);
    }

    uint8_t ClientBlockAccess::GetBlockState(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !g_clientChunkManager) {
            return 0;
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
            return 0;
        }

        int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->GetBlockState(localX, worldY, localZ);
    }

    bool ClientBlockAccess::SetBlock(int worldX, int worldY, int worldZ,
                                     Game::BlockID blockId, uint32_t updateFlags) {
        return SetBlock(worldX, worldY, worldZ, blockId, updateFlags, 0);
    }

    bool ClientBlockAccess::SetBlock(int worldX, int worldY, int worldZ,
                                     Game::BlockID blockId, uint32_t /*updateFlags*/,
                                     uint8_t stateIndex) {
        // Only ever writes inside an explicit prediction window — see
        // BeginPrediction. Outside one this is a hard no-op: the client is not
        // authoritative and an unreconciled local write would desync until the
        // next chunk reload.
        if (!m_predicting || !g_clientChunkManager) return false;
        if (!IsValidPosition(worldX, worldY, worldZ)) return false;

        // updateFlags is intentionally ignored: neighbour dirtying and the
        // remesh are ClientChunkManager's job, and the client has no
        // neighbour-notification or lighting pipeline to drive with them.
        g_clientChunkManager->PredictBlockChange({worldX, worldY, worldZ}, blockId, m_sequence,
                                                stateIndex);
        return true;
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
        // Everything that isn't air or a fluid. This deliberately does NOT
        // consult Block::opaque: that flag is derived from the render layer
        // (BlockRegistry.cpp, `opaque = layer == RenderLayer::Opaque`), so it
        // reports false for every Cutout/Translucent block — leaves, glass,
        // doors, fences, chests. Answering "solid" with a rendering property
        // made a networked client fall through all of them while the host,
        // running the server World, did not. Matches Mesher, SnapshotBlockAccess
        // and Raycast::IsBlockSolid, which all use this same rule.
        //
        // Note that collision no longer reads this at all — Physics consults
        // BlockRegistry::HasCollision directly, as MC does.
        Game::BlockID block = GetBlock(worldX, worldY, worldZ);
        return block != Game::BlockID::Air &&
               block != Game::BlockID::Water &&
               block != Game::BlockID::Lava;
    }

    bool ClientBlockAccess::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        Game::BlockID block = GetBlock(worldX, worldY, worldZ);
        return block == Game::BlockID::Water || block == Game::BlockID::Lava;
    }

    bool ClientBlockAccess::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= Config::MinY && worldY <= Config::MaxY;
    }

} // namespace Client
