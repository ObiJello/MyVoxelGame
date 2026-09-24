// File: src/client/world/ClientBlockAccess.cpp
#include "ClientBlockAccess.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/biome/BiomeZoom.hpp"
#include "ClientBiomeZoom.hpp"
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
        // MC ClientLevel.getBiome: the fuzzy zoom (BiomeZoom.hpp) with the
        // server's hashed seed picks the quart, whose noise biome is read
        // from the chunk holding it — a neighbour when the zoom steps over a
        // border. A chunk not loaded answers plains, MC ClientLevel
        // getUncachedNoiseBiome. Y needs no range check: Chunk::GetBiome
        // clamps the quart into the column as ChunkAccess.getNoiseBiome does.
        if (!m_chunks) {
            return Game::kFallbackBiomeId;
        }
        const glm::ivec3 quart =
            Game::BiomeZoom::NoiseQuartAt(BiomeZoomSeed(), worldX, worldY, worldZ);
        ClientChunk* chunk = m_chunks->GetChunk(Game::Math::ChunkPos{ quart.x >> 2, quart.z >> 2 });
        if (!chunk || !chunk->chunkData) {
            return Game::kFallbackBiomeId;
        }
        return chunk->chunkData->GetBiome((quart.x & 3) << 2, quart.y * 4, (quart.z & 3) << 2);
    }

    int ClientBlockAccess::GetBrightness(Game::Lighting::LightLayer layer, int worldX, int worldY, int worldZ) const {
        // The light the server sent with the chunk (ChunkDataS2C, then
        // LightUpdateS2C) — MC ClientLevel.getBrightness. A chunk not loaded
        // reads as MC answers for a missing column: sky 15, block 0.
        if (!m_chunks) return layer == Game::Lighting::LightLayer::Sky ? 15 : 0;
        const Game::Math::ChunkPos chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        const ClientChunk* chunk = m_chunks->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded() || !chunk->chunkData) {
            return layer == Game::Lighting::LightLayer::Sky ? 15 : 0;
        }
        return chunk->chunkData->light.Get(layer, worldX & 15, worldY, worldZ & 15);
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
        PROFILE_ZONE_N("Client.IsRegionAllAir");
        PROFILE_ZONE_VALUE(static_cast<int64_t>((max.x >> 4) - (min.x >> 4) + 1) *
                           static_cast<int64_t>((max.z >> 4) - (min.z >> 4) + 1));
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
        PROFILE_ZONE_N("Client.BlockStatesInBox");
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
        if (!m_chunks) return false;
        if (!IsValidPosition(worldX, worldY, worldZ)) return false;
        if (!m_predicting) {
            // A local write (block event, client block-entity tick): straight
            // into the chunk, no prediction bookkeeping — the server's own
            // updates for these cells are what reconcile them.
            if (!m_localWrites) return false;
            m_chunks->SetBlockLocal({worldX, worldY, worldZ}, blockId, stateIndex, false);
            return true;
        }

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
               block != Game::BlockID::Lava &&
               block != Game::BlockID::ResonantWater;   // Aurelith's river (always-water)
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

#include "client/entity/Player.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/sound/ClientSounds.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"

namespace Client {

    namespace {
        Game::Chunk* ChunkDataAt(ClientChunkManager* chunks, const glm::ivec3& pos, int& lx, int& lz) {
            if (!chunks) return nullptr;
            const Game::Math::ChunkPos cp = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            ClientChunk* chunk = chunks->GetChunk(cp);
            if (!chunk || !chunk->chunkData) return nullptr;
            lx = pos.x - cp.x * Game::Math::CHUNK_SIZE_X;
            lz = pos.z - cp.z * Game::Math::CHUNK_SIZE_Z;
            return chunk->chunkData.get();
        }
    }

    Game::BlockEntity* ClientBlockAccess::GetBlockEntity(const glm::ivec3& pos) {
        int lx, lz;
        Game::Chunk* chunk = ChunkDataAt(m_chunks, pos, lx, lz);
        return chunk ? chunk->GetBlockEntity(lx, pos.y, lz) : nullptr;
    }

    void ClientBlockAccess::SetBlockEntity(const glm::ivec3& pos, std::unique_ptr<Game::BlockEntity> entity) {
        int lx, lz;
        Game::Chunk* chunk = ChunkDataAt(m_chunks, pos, lx, lz);
        if (!chunk || !entity) return;
        entity->SetLevel(this);
        const bool ticks = entity->NeedsTicking();
        chunk->SetBlockEntity(lx, pos.y, lz, std::move(entity));
        if (ticks && m_chunks) m_chunks->RegisterTickingBlockEntity(pos);
    }

    void ClientBlockAccess::RemoveBlockEntity(const glm::ivec3& pos) {
        int lx, lz;
        Game::Chunk* chunk = ChunkDataAt(m_chunks, pos, lx, lz);
        if (chunk) chunk->RemoveBlockEntity(lx, pos.y, lz);
    }

    int64_t ClientBlockAccess::GameTime() const {
        return m_chunks ? m_chunks->ClientGameTime() : 0;
    }

    void ClientBlockAccess::PlaySeededSound(const Game::SoundExcept& except, const glm::dvec3& pos,
                                            std::string_view event, Game::SoundSource source,
                                            float volume, float pitch, int64_t seed) {
        // MC ClientLevel: `if (except == this.minecraft.player)`. The only
        // IUsePlayer on the client is the local player's predictor, so a
        // player `except` IS the local player; an entity `except` never is
        // (the local player is not a client-side Entity).
        if (except.player) Sounds::PlayAt(pos, event, source, volume, pitch, false, seed);
    }

    void ClientBlockAccess::PlayLocalSound(const glm::dvec3& pos, std::string_view event,
                                           Game::SoundSource source, float volume, float pitch,
                                           bool distanceDelay) {
        Sounds::PlayLocal(pos, event, source, volume, pitch, distanceDelay);
    }

    bool ClientBlockAccess::GetLocalPlayerBox(glm::dvec3& outMin, glm::dvec3& outMax) const {
        if (!m_player) return false;
        const auto& ph = m_player->physics;
        const double hw = Game::PlayerPhysics::WIDTH * 0.5;
        const double h  = ph.isSneaking ? Game::PlayerPhysics::HEIGHT_SNEAKING : Game::PlayerPhysics::HEIGHT_STANDING;
        outMin = glm::dvec3(ph.position.x - hw, ph.position.y,     ph.position.z - hw);
        outMax = glm::dvec3(ph.position.x + hw, ph.position.y + h, ph.position.z + hw);
        return true;
    }

    void ClientBlockAccess::MoveLocalPlayerByPiston(const glm::dvec3& delta) {
        if (!m_player) return;
        // MC LocalPlayer.move(MoverType.PISTON, delta) — a direct move, with
        // NOCLIP set for the piston's own direction so the block that is
        // pushing never blocks the push.
        m_player->physics.position += delta;
        m_player->predictedPos     += delta;
    }

    // PlayerPhysics keeps velocity in blocks per SECOND; MC's deltaMovement,
    // which these hooks speak, is blocks per TICK (the same conversion the
    // explosion knockback makes in ClientPacketHandler::handleExplode).
    namespace {
        constexpr double kTicksPerSecond = 20.0;
    }

    bool ClientBlockAccess::GetLocalPlayerMovement(glm::dvec3& outDeltaMovement, bool& outFlying) const {
        if (!m_player || m_player->IsSpectator() || m_player->health <= 0) return false;
        outDeltaMovement = glm::dvec3(m_player->physics.velocity) / kTicksPerSecond;
        outFlying = m_player->physics.isFlying;
        return true;
    }

    void ClientBlockAccess::AddLocalPlayerDeltaMovement(const glm::dvec3& delta) {
        if (!m_player) return;
        m_player->physics.velocity += glm::vec3(delta * kTicksPerSecond);
    }

    void ClientBlockAccess::CheckLocalPlayerFallDistanceAccumulation() {
        if (!m_player) return;
        // MC Entity.checkFallDistanceAccumulation.
        Game::PlayerPhysics& ph = m_player->physics;
        if (static_cast<double>(ph.velocity.y) / kTicksPerSecond > -0.5 && ph.fallDistance > 1.0f) {
            ph.fallDistance = 1.0f;
        }
    }

    void ClientBlockAccess::AddParticle(Game::ParticleKind kind, double x, double y, double z,
                                        double vx, double vy, double vz) {
        if (g_clientMobManager) g_clientMobManager->Level().AddParticle(kind, x, y, z, vx, vy, vz);
    }

    void ClientBlockAccess::BlockEvent(const glm::ivec3& pos, Game::BlockID block, int b0, int b1) {
        // MC Level.blockEvent (client): getBlockState(pos).triggerEvent(...).
        const Game::BlockState state = GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(block)) return;
        const Game::Block& def = Game::BlockRegistry::Get(block);
        if (!def.triggerEvent) return;
        LocalWriteScope scope(*this);
        def.triggerEvent(*this, pos, state, b0, b1);
        // An event can set a block entity moving — a chest's lid starts to
        // open or close — so it joins the ticking list now. It drops off by
        // itself once NeedsTicking goes false (the lid at rest).
        if (Game::BlockEntity* be = GetBlockEntity(pos); be && be->NeedsTicking() && m_chunks) {
            m_chunks->RegisterTickingBlockEntity(pos);
        }
    }

} // namespace Client
