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

        if (!m_chunks) {
            return Game::BlockID::Air;
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = m_chunks->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
            return Game::BlockID::Air;
        }

        int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->GetBlock(localX, worldY, localZ);
    }

    uint16_t ClientBlockAccess::GetBiome(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !m_chunks) {
            return Game::kFallbackBiomeId;
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = m_chunks->GetChunk(chunkPos);
        if (!chunk || !chunk->chunkData) {
            return Game::kFallbackBiomeId;
        }

        const int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        const int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->GetBiome(localX, worldY, localZ);
    }

    Game::BlockState ClientBlockAccess::GetBlockState(int worldX, int worldY, int worldZ) const {
        if (!IsValidPosition(worldX, worldY, worldZ) || !m_chunks) {
            return Game::BlockState{};
        }

        Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        ClientChunk* chunk = m_chunks->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
            return Game::BlockState{};
        }

        int localX = worldX - (chunkPos.x * Game::Math::CHUNK_SIZE_X);
        int localZ = worldZ - (chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        return chunk->chunkData->StateAt(localX, worldY, localZ);
    }

    bool ClientBlockAccess::IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                                           bool absentIsAir) const {
        if (min.x > max.x || min.y > max.y || min.z > max.z) return true;
        if (!m_chunks) return absentIsAir;
        int s0, s1, unusedY;
        Game::Math::WorldCoordinates::WorldYToSectionCoords(min.y, s0, unusedY);
        Game::Math::WorldCoordinates::WorldYToSectionCoords(max.y, s1, unusedY);
        s0 = std::max(s0, 0);
        s1 = std::min(s1, Game::Math::SECTIONS_PER_CHUNK - 1);
        if (s0 > s1) return true;
        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                ClientChunk* chunk = m_chunks->GetChunk(Game::Math::ChunkPos{cx, cz});
                if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
                    if (absentIsAir) continue;
                    return false;
                }
                for (int si = s0; si <= s1; ++si) {
                    const Game::ChunkSection* section = chunk->chunkData->GetSection(si);
                    if (!section || !section->IsAllAir()) return false;
                }
            }
        }
        return true;
    }

    uint64_t ClientBlockAccess::RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const {
        if (!m_chunks) return 0;
        uint64_t sum = 0;
        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                ClientChunk* chunk = m_chunks->GetChunk(Game::Math::ChunkPos{cx, cz});
                if (chunk && chunk->chunkData)
                    sum += chunk->chunkData->blockWriteCounter.load(std::memory_order_acquire);
            }
        }
        return sum;
    }

    void ClientBlockAccess::GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                                Game::BlockState* out) const {
        if (min.x > max.x || min.y > max.y || min.z > max.z) return;
        const int ny = max.y - min.y + 1, nz = max.z - min.z + 1;
        const size_t total = static_cast<size_t>(max.x - min.x + 1) * ny * nz;
        for (size_t i = 0; i < total; ++i) out[i] = Game::BlockState{};
        if (!m_chunks) return;
        const auto at = [&](int x, int y, int z) -> Game::BlockState& {
            return out[(static_cast<size_t>(x - min.x) * ny + (y - min.y)) * nz + (z - min.z)];
        };
        const int y0 = std::max(min.y, Game::Math::WorldCoordinates::MIN_WORLD_Y);
        const int y1 = std::min(max.y, Game::Math::WorldCoordinates::MIN_WORLD_Y +
                                       Game::Math::SECTIONS_PER_CHUNK * Game::Math::SECTION_HEIGHT - 1);
        if (y0 > y1) return;
        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                ClientChunk* chunk = m_chunks->GetChunk(Game::Math::ChunkPos{cx, cz});
                if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) continue;
                const Game::Chunk* data = chunk->chunkData.get();
                const int bx0 = std::max(min.x, cx << 4), bx1 = std::min(max.x, (cx << 4) + 15);
                const int bz0 = std::max(min.z, cz << 4), bz1 = std::min(max.z, (cz << 4) + 15);
                for (int wy = y0; wy <= y1; ) {
                    int sectionIndex, sectionY;
                    Game::Math::WorldCoordinates::WorldYToSectionCoords(wy, sectionIndex, sectionY);
                    if (sectionIndex < 0 || sectionIndex >= Game::Math::SECTIONS_PER_CHUNK) break;
                    const int syTop = std::min(15, sectionY + (y1 - wy));
                    const Game::ChunkSection* sec = data->GetSection(sectionIndex);
                    if (sec && !sec->IsAllAir()) {
                        for (int sy = sectionY; sy <= syTop; ++sy)
                            for (int wz = bz0; wz <= bz1; ++wz)
                                for (int wx = bx0; wx <= bx1; ++wx)
                                    at(wx, wy + (sy - sectionY), wz) = sec->StateAt(wx & 15, sy, wz & 15);
                    }
                    wy += (syTop - sectionY) + 1;
                }
            }
        }
    }

    bool ClientBlockAccess::SetBlock(int worldX, int worldY, int worldZ,
                                     Game::BlockID blockId, uint32_t updateFlags) {
        // The block's DEFAULT state, not index 0 — see World::SetBlock.
        return SetBlock(worldX, worldY, worldZ, blockId, updateFlags,
                        Game::DefaultStateIndexOf(blockId));
    }

    bool ClientBlockAccess::SetBlock(int worldX, int worldY, int worldZ,
                                     Game::BlockID blockId, uint32_t /*updateFlags*/,
                                     Game::BlockStateIndex stateIndex) {
        // Only ever writes inside an explicit prediction window — see
        // BeginPrediction. Outside one this is a hard no-op: the client is not
        // authoritative and an unreconciled local write would desync until the
        // next chunk reload.
        if (!m_predicting || !m_chunks) return false;
        if (!IsValidPosition(worldX, worldY, worldZ)) return false;

        // updateFlags is intentionally ignored: neighbour dirtying and the
        // remesh are ClientChunkManager's job, and the client has no
        // neighbour-notification or lighting pipeline to drive with them.
        m_chunks->PredictBlockChange({worldX, worldY, worldZ}, blockId, m_sequence,
                                                stateIndex);
        return true;
    }

    bool ClientBlockAccess::IsChunkLoaded(int chunkX, int chunkZ) const {
        if (!m_chunks) {
            return false;
        }
        return m_chunks->IsChunkLoaded({chunkX, chunkZ});
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
        // MC `!state.getFluidState().isEmpty()`, which is a strictly wider test
        // than "the block here is water": a waterlogged fence, a kelp stalk and
        // a coral fan all hold water. You swim in all three in vanilla, because
        // Entity.updateFluidHeightAndDoFluidPushing reads the FLUID state of
        // each cell its box overlaps, never the block id.
        if (ContainsWater(worldX, worldY, worldZ)) return true;
        return GetBlock(worldX, worldY, worldZ) == Game::BlockID::Lava;
    }

    bool ClientBlockAccess::IsValidPosition(int worldX, int worldY, int worldZ) const {
        return worldY >= Config::MinY && worldY <= Config::MaxY;
    }

} // namespace Client
