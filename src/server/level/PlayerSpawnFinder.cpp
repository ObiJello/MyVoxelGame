// File: src/server/level/PlayerSpawnFinder.cpp
#include "PlayerSpawnFinder.hpp"

#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/physics/Physics.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace Server {

    namespace {
        constexpr int MIN_Y = Game::Math::WorldCoordinates::MIN_WORLD_Y;
        constexpr int MAX_Y = Game::Math::WorldCoordinates::MAX_WORLD_Y;

        // MC checks `Block.isFaceFull(state.getCollisionShape(...), Direction.UP)`
        // — "can something stand on the top of this block". We have a boolean
        // collision flag rather than per-block shapes, so a partial block
        // (slab, fence) reads as standable here where MC would reject it.
        //
        // That approximation is safe because it can only ever ADD candidates,
        // and every candidate is then put through NoCollisionNoLiquid with the
        // real player box. A bad guess is rejected there and the search moves
        // on; it can never produce a spawn inside geometry.
        bool CanStandOn(const Game::World& world, int x, int y, int z) {
            if (world.IsBlockFluid(x, y, z)) return false;
            return Game::BlockRegistry::HasCollision(world.GetBlock(x, y, z));
        }
    } // namespace

    std::optional<glm::ivec3> PlayerSpawnFinder::GetOverworldRespawnPos(
            const Game::World& world, int x, int z) {

        // MC starts at the MOTION_BLOCKING heightmap. We have no heightmaps, so
        // find the same value directly: the highest block that either blocks
        // motion or is a fluid. Scanning for it is equivalent to reading the
        // heightmap MC maintains incrementally.
        int topY = MIN_Y - 1;
        for (int y = MAX_Y; y >= MIN_Y; --y) {
            if (world.IsBlockFluid(x, y, z) ||
                Game::BlockRegistry::HasCollision(world.GetBlock(x, y, z))) {
                topY = y;
                break;
            }
        }
        if (topY < MIN_Y) return std::nullopt;   // empty column (void / unloaded)

        // MC then rejects columns whose surface is fluid — an ocean, where
        // WORLD_SURFACE sits above OCEAN_FLOOR with water in between. The scan
        // below covers that case on its own: descending from the top, water is
        // reached before the floor and ends the search.
        for (int y = topY + 1; y >= MIN_Y; --y) {
            // MC: `if (!blockState.getFluidState().isEmpty()) break;`
            if (world.IsBlockFluid(x, y, z)) break;
            // MC: `if (Block.isFaceFull(collisionShape, UP)) return pos.above();`
            if (CanStandOn(world, x, y, z)) {
                return glm::ivec3(x, y + 1, z);
            }
        }
        return std::nullopt;
    }

    std::optional<glm::ivec3> PlayerSpawnFinder::GetSpawnPosInChunk(
            const Game::World& world, Game::Math::ChunkPos chunkPos) {
        const int minX = chunkPos.x * 16;
        const int minZ = chunkPos.z * 16;
        // x-major, then z — MC's loop order. It decides which block a given
        // seed lands on, so it is part of the algorithm rather than a detail.
        for (int x = minX; x <= minX + 15; ++x) {
            for (int z = minZ; z <= minZ + 15; ++z) {
                if (auto pos = GetOverworldRespawnPos(world, x, z)) return pos;
            }
        }
        return std::nullopt;
    }

    bool PlayerSpawnFinder::NoCollisionNoLiquid(const Game::World& world,
                                                const glm::ivec3& blockPos) {
        // MC: PLAYER_DIMENSIONS.makeBoundingBox(pos.getBottomCenter()) — the
        // box is centred on the block in x/z and rises from its floor.
        const float halfWidth = Game::PlayerPhysics::WIDTH * 0.5f;
        const float centreX = static_cast<float>(blockPos.x) + 0.5f;
        const float centreZ = static_cast<float>(blockPos.z) + 0.5f;
        const float minXf = centreX - halfWidth;
        const float maxXf = centreX + halfWidth;
        const float minZf = centreZ - halfWidth;
        const float maxZf = centreZ + halfWidth;
        const float minYf = static_cast<float>(blockPos.y);
        const float maxYf = minYf + Game::PlayerPhysics::HEIGHT_STANDING;

        const int bx0 = static_cast<int>(std::floor(minXf));
        const int bx1 = static_cast<int>(std::floor(maxXf));
        const int bz0 = static_cast<int>(std::floor(minZf));
        const int bz1 = static_cast<int>(std::floor(maxZf));
        const int by0 = static_cast<int>(std::floor(minYf));
        // The box's top edge sits exactly on a block boundary at integer
        // heights; ceil-minus-one keeps that boundary from pulling in the block
        // above, which would reject every otherwise-valid 2-high gap.
        const int by1 = static_cast<int>(std::ceil(maxYf)) - 1;

        for (int y = by0; y <= by1; ++y) {
            if (y < MIN_Y || y > MAX_Y) return false;
            for (int x = bx0; x <= bx1; ++x) {
                for (int z = bz0; z <= bz1; ++z) {
                    // MC's noCollision with checkLiquid=true rejects both.
                    if (world.IsBlockFluid(x, y, z)) return false;
                    if (Game::BlockRegistry::HasCollision(world.GetBlock(x, y, z))) return false;
                }
            }
        }
        return true;
    }

    glm::vec3 PlayerSpawnFinder::FixupSpawnHeight(const Game::World& world,
                                                  const glm::ivec3& spawnPos) {
        // Verbatim port of MC's fixupSpawnHeight: rise out of whatever we are
        // stuck in, fall to the floor, step back up onto it.
        glm::ivec3 pos = spawnPos;
        while (!NoCollisionNoLiquid(world, pos) && pos.y < MAX_Y) ++pos.y;
        --pos.y;
        while (NoCollisionNoLiquid(world, pos) && pos.y > MIN_Y) --pos.y;
        ++pos.y;
        return glm::vec3(static_cast<float>(pos.x) + 0.5f,
                         static_cast<float>(pos.y),
                         static_cast<float>(pos.z) + 0.5f);
    }

    int PlayerSpawnFinder::GetCoprime(int candidateCount) {
        // MC getCoprime: 17 is coprime with every candidate count it can meet
        // above 16, and below that the count-1 step still visits every cell.
        return candidateCount <= 16 ? candidateCount - 1 : 17;
    }

    glm::vec3 PlayerSpawnFinder::FindSpawn(const Game::World& world,
                                           const glm::ivec3& spawnSuggestion,
                                           int respawnRadius,
                                           uint64_t randomSeed) {
        const int radius = std::max(0, respawnRadius);
        const long long side = static_cast<long long>(radius) * 2LL + 1LL;
        const int candidateCount =
            static_cast<int>(std::min<long long>(ABSOLUTE_MAX_ATTEMPTS, side * side));
        if (candidateCount <= 0) return FixupSpawnHeight(world, spawnSuggestion);

        const int coprime = GetCoprime(candidateCount);
        // MC uses RandomSource.create() — a fresh unseeded source, so arrivals
        // scatter. Seeding is exposed for tests and for anywhere that needs the
        // choice to be reproducible.
        std::mt19937_64 rng(randomSeed != 0 ? randomSeed : std::random_device{}());
        const int offset = static_cast<int>(rng() % static_cast<uint64_t>(candidateCount));

        for (int i = 0; i < candidateCount; ++i) {
            const int value  = (offset + coprime * i) % candidateCount;
            const int deltaX = value % (radius * 2 + 1);
            const int deltaZ = value / (radius * 2 + 1);
            const int targetX = spawnSuggestion.x + deltaX - radius;
            const int targetZ = spawnSuggestion.z + deltaZ - radius;

            const auto candidate = GetOverworldRespawnPos(world, targetX, targetZ);
            if (candidate && NoCollisionNoLiquid(world, *candidate)) {
                return glm::vec3(static_cast<float>(candidate->x) + 0.5f,
                                 static_cast<float>(candidate->y),
                                 static_cast<float>(candidate->z) + 0.5f);
            }
        }

        // MC's final candidate is the suggestion itself, height-corrected.
        return FixupSpawnHeight(world, spawnSuggestion);
    }

} // namespace Server
