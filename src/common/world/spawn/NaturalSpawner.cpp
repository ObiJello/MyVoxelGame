// File: src/common/world/spawn/NaturalSpawner.cpp
//
// Transcribed from MC NaturalSpawner.java. The loop structure and every RNG
// draw are in MC's exact order — spawnCategoryForPosition draws position
// jitter, the entry pick, the pack size, and the yaw from the SAME level
// random, so reordering any test past a draw desyncs the whole spawn stream.
#include "common/world/spawn/NaturalSpawner.hpp"
#include "common/world/spawn/GeneratedMobSpawns.hpp"
#include "common/world/spawn/GeneratedSpawnTags.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/spawn/PotentialCalculator.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/HushBlocks.hpp"
#include "common/world/level/AurelithQuest.hpp"
#include "common/entity/MobCategory.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

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

            // ...and the same radius around the level respawn point, so a fresh
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

        // Weighted pick over one category's entries for a biome — MC
        // WeightedList.getRandom: one nextInt(totalWeight) roll, cumulative
        // subtraction in insertion order.
        const MobSpawnEntry* PickWeighted(const BiomeSpawnList& list, MobCategory category,
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

        // What one placement attempt asks the level about its position, each
        // answer resolved at most once. MC asks again at every step — mobsAt
        // in getRandomSpawnMobAt and again in canSpawnMobAt, the biome's
        // spawn costs in canSpawn and again in afterSpawn — and gets the same
        // answer every time: a biome never changes mid-tick, and nothing
        // between those steps writes the block below (the structure
        // override's input). Scoped to a single attempt, so a later attempt
        // that walks back onto the same spot asks afresh.
        class SpawnPosition {
        public:
            SpawnPosition(const SpawnContext& ctx, MobCategory category, int x, int y, int z)
                : x(x), y(y), z(z), m_ctx(ctx), m_category(category) {}

            const int x, y, z;

            // The biome's slug — the key the biome-tag tests read.
            std::string_view BiomeName() {
                return m_ctx.biomeIdAt ? BiomeRegistry::Get(BiomeIdAt()).name
                                       : m_ctx.biomeAt(x, y, z);
            }

            // The BIOME's list, whatever structure the position is in — the
            // spawn-cost lookups read this one.
            const BiomeSpawnList* BiomeList() {
                if (!m_haveBiomeList) {
                    m_biomeList = m_ctx.biomeIdAt ? SpawnListForBiome(BiomeIdAt())
                                                  : FindBiomeSpawnList(m_ctx.biomeAt(x, y, z));
                    m_haveBiomeList = true;
                }
                return m_biomeList;
            }

            // MC NaturalSpawner.mobsAt for the pass's category:
            // isInNetherFortressBounds ? FORTRESS_ENEMIES : generator.getMobsAt
            // — a structure's spawn_overrides list replaces the biome's inside
            // it (StructureSpawnOverrides), else the biome's.
            const BiomeSpawnList* MobsAt() {
                if (!m_haveMobs) {
                    const BiomeSpawnList* structure =
                        m_ctx.structureSpawnsAt ? m_ctx.structureSpawnsAt(m_category, x, y, z)
                                                : nullptr;
                    m_mobs = structure ? structure : BiomeList();
                    m_haveMobs = true;
                }
                return m_mobs;
            }

        private:
            BiomeId BiomeIdAt() {
                if (!m_haveBiomeId) {
                    m_biomeId = m_ctx.biomeIdAt(x, y, z);
                    m_haveBiomeId = true;
                }
                return m_biomeId;
            }

            const SpawnContext&   m_ctx;
            const MobCategory     m_category;
            BiomeId               m_biomeId = 0;
            const BiomeSpawnList* m_biomeList = nullptr;
            const BiomeSpawnList* m_mobs = nullptr;
            bool                  m_haveBiomeId = false;
            bool                  m_haveBiomeList = false;
            bool                  m_haveMobs = false;
        };

        // MC getRandomSpawnMobAt: the water-ambient 98% thin-out (on the
        // BIOME's tag, before and regardless of any structure), then the
        // weighted pick over mobsAt's list.
        const MobSpawnEntry* GetRandomSpawnMobAt(MobCategory category, SpawnPosition& pos,
                                                 JavaRandom& rng) {
            if (category == MobCategory::WaterAmbient &&
                SpawnTags::ReduceWaterAmbientSpawns(pos.BiomeName()) &&
                rng.NextFloat() < 0.98f) {
                return nullptr;
            }
            const BiomeSpawnList* list = pos.MobsAt();
            return list ? PickWeighted(*list, category, rng) : nullptr;
        }

        // MC canSpawnMobAt: the chosen entry must still be in mobsAt's list
        // at the CURRENT (drifted) position — the pack walk can cross into a
        // biome, or out of a structure, that does not spawn this mob at all.
        bool CanSpawnMobAt(const MobSpawnEntry* entry, SpawnPosition& pos) {
            const BiomeSpawnList* list = pos.MobsAt();
            if (!list) return false;
            // MC compares SpawnerData records by value; the baked tables share
            // storage per biome, so pointer equality also covers the common
            // same-biome case.
            for (int i = 0; i < list->count; ++i) {
                const MobSpawnEntry& e = list->entries[i];
                if (e.category == entry->category && e.type == entry->type &&
                    e.weight == entry->weight && e.minCount == entry->minCount &&
                    e.maxCount == entry->maxCount) {
                    return true;
                }
            }
            return false;
        }

        // MC SpawnState.canSpawn — the spawn-cost (PotentialCalculator) gate.
        bool CanSpawnWithinBudget(const SpawnContext& ctx, EntityTypeId type,
                                  SpawnPosition& pos) {
            if (!ctx.spawnPotential) return true;
            const MobSpawnCost* cost = FindMobSpawnCost(pos.BiomeList(), type);
            if (!cost) return true;
            return ctx.spawnPotential->GetPotentialEnergyChange(
                       glm::ivec3(pos.x, pos.y, pos.z), cost->charge) <= cost->energyBudget;
        }

        // The spawnPotential half of MC SpawnState.afterSpawn.
        void AddSpawnCharge(const SpawnContext& ctx, EntityTypeId type, SpawnPosition& pos) {
            if (!ctx.spawnPotential) return;
            if (const MobSpawnCost* cost = FindMobSpawnCost(pos.BiomeList(), type)) {
                ctx.spawnPotential->AddCharge(glm::ivec3(pos.x, pos.y, pos.z), cost->charge);
            }
        }

        // MC isValidSpawnPostitionForType [sic] — the six tests in MC's order.
        // None of them draws RNG, but CheckSpawnRules right after them does,
        // so keeping the order still matters.
        bool IsValidSpawnPositionForType(const SpawnContext& ctx, const IBlockAccess& blocks,
                                         const MobSpawnEntry& entry, SpawnPosition& pos,
                                         double nearestSq, JavaRandom& rng) {
            const int x = pos.x, y = pos.y, z = pos.z;
            const EntityTypeId type = entry.type;
            if (GetEntityTypeInfo(type).category == MobCategory::Misc) return false;

            // MC: !canSpawnFarFromPlayer && dist > despawnDistance² — using
            // the TYPE's category, which for a creature in a monster list
            // differs from the pass's category.
            const int despawn =
                GetMobCategoryInfo(GetEntityTypeInfo(type).category).despawnDistance;
            if (!CanSpawnFarFromPlayer(type) &&
                nearestSq > static_cast<double>(despawn) * despawn) {
                return false;
            }

            // MC: type.canSummon() && canSpawnMobAt(...). Every mob type here
            // is summonable.
            if (!CanSpawnMobAt(&entry, pos)) return false;

            {
                PROFILE_ZONE_DETAIL("Spawn.PositionOk");
                if (!IsSpawnPositionOk(type, blocks, x, y, z)) return false;
            }

            // The Hush's echo heart (HushBlocks.hpp): no hostile mob spawns
            // naturally within its radius. Keyed on the TYPE's category, like
            // the despawn test above — a monster in any pass's list. Before
            // CheckSpawnRules so a refused position draws no RNG, exactly as
            // if the heart's ward were one more of the position tests.
            if (ctx.level && IsMonsterCategory(GetEntityTypeInfo(type).category) &&
                EchoHeart::SuppressesHostileSpawn(ctx.level->Dimension(), glm::ivec3(x, y, z),
                                                  ctx.level->GetGameTime())) {
                return false;
            }
            // An awakened Aurelith (AurelithQuest.hpp): no hostile mob spawns
            // naturally inside its walls — the Chord held again keeps them
            // out, as the echo heart does its sixteen blocks.
            if (ctx.level && IsMonsterCategory(GetEntityTypeInfo(type).category) &&
                Aurelith::SuppressesHostileSpawn(ctx.level->Dimension(), glm::ivec3(x, y, z))) {
                return false;
            }

            {
                PROFILE_ZONE_DETAIL("Spawn.Rules");
                SpawnRuleContext ruleCtx{ *ctx.level, blocks, rng, SpawnReason::Natural,
                                          &ctx.biomeAt, &ctx.surfaceHeight,
                                          ctx.seaLevel, ctx.worldSeed };
                if (!CheckSpawnRules(type, ruleCtx, glm::ivec3(x, y, z))) return false;
            }

            // MC's last gate: noCollision(type.getSpawnAABB(...)).
            PROFILE_ZONE_DETAIL("Spawn.BoxFree");
            if (ctx.spawnBoxFree && !ctx.spawnBoxFree(type, x + 0.5, y, z + 0.5)) {
                return false;
            }
            return true;
        }

        // MC isValidPositionForMob — the post-instantiation test, run AFTER
        // snapTo and BEFORE finalizeSpawn. Draws no RNG.
        bool IsValidPositionForMob(const SpawnContext& ctx, Mob& mob, double nearestSq) {
            const int despawn =
                GetMobCategoryInfo(mob.TypeInfo().category).despawnDistance;
            if (nearestSq > static_cast<double>(despawn) * despawn &&
                mob.RemoveWhenFarAway(nearestSq)) {
                return false;
            }
            return mob.CheckSpawnRules(*ctx.level, SpawnReason::Natural) &&
                   mob.CheckSpawnObstruction(*ctx.level);
        }

        // MC spawnCategoryForPosition — the pack loop.
        void SpawnCategoryForPosition(const SpawnContext& ctx, const IBlockAccess& blocks,
                                      MobCategory category,
                                      int startX, int startY, int startZ,
                                      int chunkX, int chunkZ, JavaRandom& rng) {
            // MC tests isRedstoneConductor on the start block, which for a
            // vanilla block is `isCollisionShapeFullBlock` plus the block's own
            // opt-out. The shape half is the part that matters here.
            {
                PROFILE_ZONE_DETAIL("Spawn.StartBlock");
                if (IsCollisionShapeFullBlock(blocks, startX, startY, startZ)) return;
            }

            int clusterSize = 0;

            for (int group = 0; group < kPackAttemptsPerChunk; ++group) {
                int x = startX;
                int z = startZ;

                // Both reset PER GROUP in MC — each of the three attempts
                // re-picks its type and starts a fresh group-data token.
                const MobSpawnEntry* currentSpawnData = nullptr;
                std::shared_ptr<SpawnGroupData> groupData;

                int max = static_cast<int>(std::ceil(rng.NextFloat() * 4.0f));
                int groupSize = 0;

                for (int attempt = 0; attempt < max; ++attempt) {
                    // Walk the placement point a little each time. Two
                    // independent rolls, not one signed roll: MC's distribution
                    // is triangular, which clusters placements near the centre.
                    x += rng.NextInt(kPackSpread) - rng.NextInt(kPackSpread);
                    z += rng.NextInt(kPackSpread) - rng.NextInt(kPackSpread);

                    const double xx = x + 0.5;
                    const double zz = z + 0.5;

                    double nearestSq = 0.0;
                    {
                        PROFILE_ZONE_DETAIL("Spawn.Distance");
                        if (!NearestPlayerDistanceSq(ctx, xx, startY, zz, nearestSq)) {
                            continue;   // MC: nearestPlayer == null
                        }
                        if (!IsRightDistanceToPlayerAndSpawnPoint(ctx, x, startY, z,
                                                                  chunkX, chunkZ, nearestSq)) {
                            continue;
                        }
                    }

                    SpawnPosition pos(ctx, category, x, startY, z);

                    if (!currentSpawnData) {
                        {
                            PROFILE_ZONE_DETAIL("Spawn.Pick");
                            currentSpawnData = GetRandomSpawnMobAt(category, pos, rng);
                        }
                        if (!currentSpawnData) break;   // MC: empty pick ends the group

                        // The pack size comes from the CHOSEN entry, replacing
                        // the provisional roll above — a sheep entry says 4..4,
                        // so sheep always arrive as four.
                        max = currentSpawnData->minCount +
                              rng.NextInt(1 + currentSpawnData->maxCount -
                                          currentSpawnData->minCount);
                    }

                    {
                        PROFILE_ZONE_DETAIL("Spawn.Validate");
                        if (!IsValidSpawnPositionForType(ctx, blocks, *currentSpawnData, pos,
                                                         nearestSq, rng)) {
                            continue;
                        }
                        if (!CanSpawnWithinBudget(ctx, currentSpawnData->type, pos)) {
                            continue;
                        }
                    }
                    PROFILE_ZONE_DETAIL("Spawn.Place");

                    std::unique_ptr<Mob> mob = ctx.createMob(currentSpawnData->type);
                    if (!mob) return;   // MC returns from the whole call here

                    // MC snapTo: block centre, random yaw, zero pitch.
                    mob->position = glm::dvec3(xx, startY, zz);
                    mob->yRot = rng.NextFloat() * 360.0f;
                    mob->xRot = 0.0f;
                    mob->yHeadRot = mob->yRot;
                    mob->yBodyRot = mob->yRot;

                    // MC order: isValidPositionForMob FIRST, finalizeSpawn
                    // second. finalizeSpawn draws RNG (a sheep's colour roll),
                    // so a rejected position must not consume those draws.
                    if (!IsValidPositionForMob(ctx, *mob, nearestSq)) {
                        continue;
                    }

                    groupData = mob->FinalizeSpawn(SpawnReason::Natural,
                                                   std::move(groupData));
                    ++clusterSize;
                    ++groupSize;

                    Mob& placed = *mob;
                    if (ctx.addFreshEntity) ctx.addFreshEntity(std::move(mob));

                    // MC SpawnState.afterSpawn: tighten the local cap and add
                    // the spawn-cost charge within the same tick.
                    AddSpawnCharge(ctx, currentSpawnData->type, pos);
                    if (ctx.afterSpawn) {
                        ctx.afterSpawn(category, glm::ivec3(x, startY, z));
                    }

                    // Uses the VIRTUAL — wolves cluster to 8, ghasts to 1.
                    if (clusterSize >= placed.GetMaxSpawnClusterSize()) return;
                    if (placed.IsMaxGroupSizeReached(groupSize)) break;
                }
            }
        }

    } // namespace

    const BiomeSpawnList* SpawnListForBiome(BiomeId biome) {
        // The biome table is generated and fixed at build time, so one pass
        // over it answers every id for the life of the process.
        static const std::vector<const BiomeSpawnList*> kByBiome = [] {
            std::vector<const BiomeSpawnList*> table(BiomeRegistry::Count());
            for (BiomeId id = 0; id < BiomeRegistry::Count(); ++id) {
                table[id] = FindBiomeSpawnList(BiomeRegistry::Get(id).name);
            }
            return table;
        }();
        // An id past the table resolves to the fallback biome, as
        // BiomeRegistry::Get does.
        return biome < kByBiome.size() ? kByBiome[biome]
                                       : FindBiomeSpawnList(BiomeRegistry::Get(biome).name);
    }

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

    std::vector<MobCategory> GetFilteredSpawningCategories(const SpawnContext& ctx,
                                                           bool spawnFriendlies,
                                                           bool spawnEnemies,
                                                           bool spawnPersistent) {
        // MC SPAWNING_CATEGORIES = every category except MISC, in enum order.
        static constexpr MobCategory kSpawningCategories[] = {
            MobCategory::Monster,
            MobCategory::Creature,
            MobCategory::Ambient,
            MobCategory::Axolotls,
            MobCategory::UndergroundWaterCreature,
            MobCategory::WaterCreature,
            MobCategory::WaterAmbient,
            // The Aether's appended categories (MobCategory.hpp) — MC's
            // SPAWNING_CATEGORIES is every value but MISC, so they are in it.
            MobCategory::AetherSurfaceMonster,
            MobCategory::AetherDarknessMonster,
            MobCategory::AetherSkyMonster,
            MobCategory::AetherAerwhale,
        };

        std::vector<MobCategory> out;
        out.reserve(std::size(kSpawningCategories));
        for (MobCategory category : kSpawningCategories) {
            const MobCategoryInfo& info = GetMobCategoryInfo(category);
            if ((spawnFriendlies || !info.isFriendly) &&
                (spawnEnemies || info.isFriendly) &&
                (spawnPersistent || !info.isPersistent) &&
                CanSpawnForCategory(ctx, category)) {
                out.push_back(category);
            }
        }
        return out;
    }

    void SpawnForChunk(const SpawnContext& ctx, const Chunk& chunk,
                       int chunkX, int chunkZ,
                       const std::vector<MobCategory>& categories,
                       JavaRandom& rng) {
        PROFILE_ZONE_N("NaturalSpawner");

        if (!ctx.level || !ctx.createMob || !ctx.biomeAt) return;
        const IBlockAccess* blocks = ctx.level->Blocks();
        if (!blocks) return;

        for (MobCategory category : categories) {
            // Opt-in (EXPLOSION_DETAIL_ZONES): tens of thousands per tick
            // under a few dozen players — see Profiling_Tracy.hpp.
            PROFILE_ZONE_DETAIL("Spawn.Category");
            // MC checks the per-player local cap per chunk, in addition to the
            // per-tick global filter.
            {
                PROFILE_ZONE_DETAIL("Spawn.LocalCap");
                if (ctx.canSpawnLocal && !ctx.canSpawnLocal(category, chunkX, chunkZ)) continue;
            }

            // ── MC spawnCategoryForChunk / getRandomPosWithin ──────────────
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
            const int startY = ctx.minY +
                               rng.NextInt(std::max(1, topY - ctx.minY + 1));

            // MC spawnCategoryForChunk: `pos.getY() >= level.getMinY() + 1`.
            // The test is on the CHOSEN y, not on the surface height.
            if (startY < ctx.minY + 1) continue;

            SpawnCategoryForPosition(ctx, *blocks, category,
                                     startX, startY, startZ, chunkX, chunkZ, rng);
        }
    }

} // namespace Game
