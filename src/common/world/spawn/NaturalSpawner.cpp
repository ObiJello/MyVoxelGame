// File: src/common/world/spawn/NaturalSpawner.cpp
#include "common/world/spawn/NaturalSpawner.hpp"
#include "common/world/spawn/GeneratedMobSpawns.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // MC BlockBehaviour.isCollisionShapeFullBlock. NOT the same as "has
        // collision": a slab, a stair or a fence all collide but none is a full
        // cube, and MC lets a mob's body occupy every one of them.
        bool IsCollisionShapeFullBlock(const IBlockAccess& blocks, int x, int y, int z) {
            const BlockID id = blocks.GetBlock(x, y, z);
            if (!BlockRegistry::HasCollision(id)) return false;

            const auto& shape =
                BlockRegistry::GetBlockShape(id, blocks.GetBlockState(x, y, z));
            return shape.min.x <= 0.0f && shape.min.y <= 0.0f && shape.min.z <= 0.0f &&
                   shape.max.x >= 1.0f && shape.max.y >= 1.0f && shape.max.z >= 1.0f;
        }

        // MC BlockBehaviour.isFaceSturdy(UP), which is what the default
        // isValidSpawn tests on the block below. VoxelShape.getFaceShape(UP)
        // projects the shape's TOP face, so a bottom slab counts — mobs really
        // do spawn on slabs in vanilla — while a fence post or a torch does not.
        bool IsTopFaceSturdy(const IBlockAccess& blocks, int x, int y, int z) {
            const BlockID id = blocks.GetBlock(x, y, z);
            if (!BlockRegistry::HasCollision(id)) return false;

            const auto& shape =
                BlockRegistry::GetBlockShape(id, blocks.GetBlockState(x, y, z));
            return shape.min.x <= 0.0f && shape.max.x >= 1.0f &&
                   shape.min.z <= 0.0f && shape.max.z >= 1.0f;
        }

        // MC NaturalSpawner.isValidEmptySpawnBlock — the position itself must
        // be free. A FULL cube or a fluid is out.
        //
        // MC also rejects redstone sources and BlockTags.PREVENT_MOB_SPAWNING_
        // INSIDE (rails); neither concept exists here yet, so both are absent
        // rather than approximated.
        bool IsValidEmptySpawnBlock(const IBlockAccess& blocks, int x, int y, int z) {
            if (IsCollisionShapeFullBlock(blocks, x, y, z)) return false;
            if (blocks.IsBlockFluid(x, y, z)) return false;
            return true;
        }

        // MC SpawnPlacementTypes.ON_GROUND: a sturdy top face below, and two
        // free blocks at the position. The two-block clearance is what keeps
        // mobs out of 1-high gaps they could never stand in.
        bool IsSpawnPositionOk(const IBlockAccess& blocks, int x, int y, int z) {
            if (!IsTopFaceSturdy(blocks, x, y - 1, z)) return false;
            if (!IsValidEmptySpawnBlock(blocks, x, y, z)) return false;
            if (!IsValidEmptySpawnBlock(blocks, x, y + 1, z)) return false;
            return true;
        }

        bool CheckSpawnRules(EntityTypeId type, EntityLevel& level, const glm::ivec3& pos) {
            switch (GetEntityTypeInfo(type).category) {
                case MobCategory::Monster:  return Monster::CheckMonsterSpawnRules(level, pos);
                case MobCategory::Creature: return Animal::CheckAnimalSpawnRules(level, pos);
                default: return false;
            }
        }

        // MC Level.getNearestPlayer(x, y, z, -1.0, false) followed by
        // distanceToSqr. Returns false when there is no player at all, which is
        // MC's `nearestPlayer != null` guard — no players means no spawning
        // anywhere, not spawning everywhere.
        bool NearestPlayerDistanceSq(const SpawnContext& ctx,
                                     double x, double y, double z, double& out) {
            if (!ctx.playerPositions || ctx.playerPositions->empty()) return false;

            out = std::numeric_limits<double>::max();
            for (const glm::dvec3& p : *ctx.playerPositions) {
                const double dx = p.x - x, dy = p.y - y, dz = p.z - z;
                out = std::min(out, dx * dx + dy * dy + dz * dz);
            }
            return true;
        }

        // MC isRightDistanceToPlayerAndSpawnPoint — all three clauses.
        bool IsRightDistanceToPlayerAndSpawnPoint(const SpawnContext& ctx,
                                                  int x, int y, int z,
                                                  int originChunkX, int originChunkZ,
                                                  double nearestSq) {
            // The 24-block player exclusion. See rule 2 in the header.
            if (nearestSq <= static_cast<double>(kMinSpawnDistance) * kMinSpawnDistance) {
                return false;
            }

            // ...and the same radius around the world respawn point, so a fresh
            // spawn area stays clear even when nobody is standing in it.
            // Note this one is STRICT (`closerToCenterThan`), unlike the <= above.
            if (ctx.hasWorldSpawn) {
                const double dx = ctx.worldSpawn.x - (x + 0.5);
                const double dy = ctx.worldSpawn.y - y;
                const double dz = ctx.worldSpawn.z - (z + 0.5);
                if (dx * dx + dy * dy + dz * dz <
                    static_cast<double>(kMinSpawnDistance) * kMinSpawnDistance) {
                    return false;
                }
            }

            // The pack walk can leave the chunk it started in. MC lets it,
            // but only into a chunk that is itself entity-ticking.
            const int cx = x >> 4;
            const int cz = z >> 4;
            if (cx == originChunkX && cz == originChunkZ) return true;
            return !ctx.chunkSpawnable || ctx.chunkSpawnable(cx, cz);
        }

        // MC isValidSpawnPostitionForType. The ORDER of these four tests is
        // load-bearing: CheckSpawnRules draws from the level's random, so
        // moving a cheaper test after it shifts the whole spawn RNG stream and
        // the world stops matching vanilla for the same seed.
        bool IsValidSpawnPositionForType(const SpawnContext& ctx, const IBlockAccess& blocks,
                                         EntityTypeId type, MobCategory category,
                                         int x, int y, int z, double nearestSq) {
            const int despawn = GetMobCategoryInfo(category).despawnDistance;
            if (nearestSq > static_cast<double>(despawn) * despawn) return false;

            if (!IsSpawnPositionOk(blocks, x, y, z)) return false;
            if (!CheckSpawnRules(type, *ctx.level, glm::ivec3(x, y, z))) return false;

            // MC's last gate: the mob's own bounding box must fit. This is
            // separate from the two-free-blocks check above, which only looks
            // at the column — a 1.4-wide spider needs more than that.
            if (ctx.spawnBoxFree && !ctx.spawnBoxFree(type, x + 0.5, y, z + 0.5)) {
                return false;
            }
            return true;
        }

        // Weighted pick over one category's entries for a biome.
        const MobSpawnEntry* PickSpawnEntry(const BiomeSpawnList& list, MobCategory category,
                                            JavaRandom& rng) {
            int totalWeight = 0;
            for (int i = 0; i < list.count; ++i) {
                if (list.entries[i].category == category) totalWeight += list.entries[i].weight;
            }
            if (totalWeight <= 0) return nullptr;

            int roll = rng.NextInt(totalWeight);
            for (int i = 0; i < list.count; ++i) {
                const MobSpawnEntry& e = list.entries[i];
                if (e.category != category) continue;
                roll -= e.weight;
                if (roll < 0) return &e;
            }
            return nullptr;
        }

    } // namespace

    bool CanSpawnForCategory(const SpawnContext& ctx, MobCategory category) {
        if (!ctx.categoryCounts) return false;

        const MobCategoryInfo& info = GetMobCategoryInfo(category);
        if (info.maxInstancesPerChunk <= 0) return false;

        // Rule 1 in the header. Integer division matches MC exactly, which
        // matters at small chunk counts: with 100 spawnable chunks a category
        // capped at 70 gets 70*100/289 = 24, not 24.2.
        const int cap = info.maxInstancesPerChunk * ctx.spawnableChunkCount / kMagicNumber;
        return ctx.categoryCounts[static_cast<size_t>(category)] < cap;
    }

    void SpawnForChunk(const SpawnContext& ctx, const Chunk& chunk,
                       int chunkX, int chunkZ,
                       int64_t gameTime, JavaRandom& rng,
                       std::vector<std::unique_ptr<Mob>>& out) {
        PROFILE_ZONE_N("NaturalSpawner");

        if (!ctx.level || !ctx.createMob || !ctx.biomeAt) return;
        const IBlockAccess* blocks = ctx.level->Blocks();
        if (!blocks) return;

        // Passive spawning is far rarer than hostile — see kCreatureSpawnInterval.
        const bool spawnPassive = (gameTime % kCreatureSpawnInterval) == 0;

        const MobCategory categories[] = { MobCategory::Monster, MobCategory::Creature };

        for (MobCategory category : categories) {
            if (category == MobCategory::Creature && !spawnPassive) continue;
            if (!CanSpawnForCategory(ctx, category)) continue;
            // MC checks the per-player local cap per chunk, in addition to the
            // global one.
            if (ctx.canSpawnLocal && !ctx.canSpawnLocal(category, chunkX, chunkZ)) continue;

            // ── MC getRandomPosWithin ──────────────────────────────────────
            const int startX = (chunkX << 4) + rng.NextInt(16);
            const int startZ = (chunkZ << 4) + rng.NextInt(16);

            // WORLD_SURFACE + 1 is the first free block above the terrain.
            // Read straight off the chunk we were handed, as MC
            // getRandomPosWithin does (`chunk.getHeight(WORLD_SURFACE, x, z)`),
            // rather than going back through the level for a chunk already in
            // hand — the level path has to re-resolve it, and answers MIN_Y for
            // anything not resident.
            const int topY = chunk.GetSurfaceHeight(startX & 15, startZ & 15,
                                                    HeightmapType::WorldSurface) + 1;

            // MC picks Y uniformly over the WHOLE column, not just the surface,
            // which is what puts monsters in caves as well as on the surface.
            // Mth.randomBetweenInclusive is INCLUSIVE at both ends, so topY
            // itself is reachable — hence the +1 on the range.
            const int startY = kMinBuildHeight +
                               rng.NextInt(std::max(1, topY - kMinBuildHeight + 1));

            // MC spawnCategoryForChunk: `pos.getY() >= level.getMinY() + 1`.
            // The test is on the CHOSEN y, not on the surface height.
            if (startY < kMinBuildHeight + 1) continue;

            // MC tests isRedstoneConductor on the start block, which for a
            // vanilla block is `isCollisionShapeFullBlock` plus the block's own
            // opt-out. The shape half is the part that matters here.
            if (IsCollisionShapeFullBlock(*blocks, startX, startY, startZ)) continue;

            // ── The pack loop (rule 3) ────────────────────────────────────
            // MC's `clusterSize` counts across ALL THREE attempts and returns
            // outright at 4, so one chunk never places more than a single
            // cluster's worth however lucky the attempts are.
            int clusterSize = 0;

            for (int attempt = 0; attempt < kPackAttemptsPerChunk && clusterSize < kMaxSpawnClusterSize;
                 ++attempt) {
                int x = startX;
                int z = startZ;

                const MobSpawnEntry* chosen = nullptr;
                int packSize = static_cast<int>(std::ceil(rng.NextFloat() * 4.0f));

                for (int i = 0; i < packSize; ++i) {
                    // Walk the placement point a little each time. Two
                    // independent rolls, not one signed roll: MC's distribution
                    // is triangular, which clusters placements near the centre.
                    x += rng.NextInt(kPackSpread) - rng.NextInt(kPackSpread);
                    z += rng.NextInt(kPackSpread) - rng.NextInt(kPackSpread);

                    double nearestSq = 0.0;
                    if (!NearestPlayerDistanceSq(ctx, x + 0.5, startY, z + 0.5, nearestSq)) {
                        continue;
                    }
                    if (!IsRightDistanceToPlayerAndSpawnPoint(ctx, x, startY, z,
                                                              chunkX, chunkZ, nearestSq)) {
                        continue;
                    }

                    if (!chosen) {
                        const std::string_view biome = ctx.biomeAt(x, startY, z);
                        const BiomeSpawnList* list = FindBiomeSpawnList(biome);
                        if (!list) break;

                        chosen = PickSpawnEntry(*list, category, rng);
                        if (!chosen) break;

                        // The pack size comes from the CHOSEN entry, replacing
                        // the provisional roll above — a sheep entry says 4..4,
                        // so sheep always arrive as four.
                        packSize = chosen->minCount +
                                   rng.NextInt(1 + chosen->maxCount - chosen->minCount);
                    }

                    if (!IsValidSpawnPositionForType(ctx, *blocks, chosen->type, category,
                                                     x, startY, z, nearestSq)) {
                        continue;
                    }

                    std::unique_ptr<Mob> mob = ctx.createMob(chosen->type);
                    if (!mob) return;   // MC returns from the whole call here

                    // MC snapTo happens BEFORE the final validity test, and it
                    // draws the yaw from the same random — so moving it after
                    // would desync every subsequent roll.
                    mob->position = glm::dvec3(x + 0.5, startY, z + 0.5);
                    mob->yRot = rng.NextFloat() * 360.0f;
                    mob->xRot = 0.0f;
                    mob->yHeadRot = mob->yRot;
                    mob->yBodyRot = mob->yRot;

                    // MC calls finalizeSpawn here, between snapTo and the
                    // final validity test — after the position, because a
                    // subclass may read the biome it landed in.
                    mob->FinalizeSpawn();

                    // MC isValidPositionForMob: the despawn-distance test runs
                    // a SECOND time, now against the mob's own override — which
                    // is how an animal placed 200 blocks out survives where a
                    // zombie in the same spot would be rejected.
                    const int despawn = GetMobCategoryInfo(category).despawnDistance;
                    if (nearestSq > static_cast<double>(despawn) * despawn &&
                        mob->RemoveWhenFarAway(nearestSq)) {
                        continue;
                    }

                    out.push_back(std::move(mob));
                    ++clusterSize;

                    if (clusterSize >= kMaxSpawnClusterSize) return;
                }
            }
        }
    }

} // namespace Game
