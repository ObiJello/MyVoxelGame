// File: src/common/world/spawn/SpawnPlacements.cpp
//
// Transcribed from MC SpawnPlacements.java's static registration block and the
// per-mob check*SpawnRules statics it references. Every predicate keeps MC's
// exact test ORDER, because several of them roll the shared random — reordering
// a cheap test past a roll desyncs the whole spawn RNG stream.
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/spawn/GeneratedSpawnTags.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/Animal.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <array>
#include <cmath>

namespace Game {

    // ── Shared position tests ───────────────────────────────────────────────

    bool IsCollisionShapeFullBlock(const IBlockAccess& blocks, int x, int y, int z) {
        const BlockID id = blocks.GetBlock(x, y, z);
        if (!BlockRegistry::HasCollision(id)) return false;

        const auto& shape =
            BlockRegistry::GetBlockShape(blocks.GetBlockState(x, y, z));
        return shape.min.x <= 0.0f && shape.min.y <= 0.0f && shape.min.z <= 0.0f &&
               shape.max.x >= 1.0f && shape.max.y >= 1.0f && shape.max.z >= 1.0f;
    }

    // MC BlockBehaviour.isFaceSturdy(UP), which is what the default
    // isValidSpawn tests on the block below. VoxelShape.getFaceShape(UP)
    // projects the shape's TOP face, so a bottom slab counts — mobs really do
    // spawn on slabs in vanilla — while a fence post or a torch does not.
    static bool IsTopFaceSturdy(const IBlockAccess& blocks, int x, int y, int z) {
        const BlockID id = blocks.GetBlock(x, y, z);
        if (!BlockRegistry::HasCollision(id)) return false;

        const auto& shape =
            BlockRegistry::GetBlockShape(blocks.GetBlockState(x, y, z));
        return shape.min.x <= 0.0f && shape.max.x >= 1.0f &&
               shape.min.z <= 0.0f && shape.max.z >= 1.0f;
    }

    bool IsValidSpawnBlock(const IBlockAccess& blocks, int x, int y, int z) {
        // MC also rejects getLightEmission() >= 14 (glowstone, magma, lava —
        // though magma is separately dangerous). No emission data here yet.
        return IsTopFaceSturdy(blocks, x, y, z);
    }

    bool IsSignalSource(BlockID block) {
        // MC BlockBehaviour.isSignalSource — the classes overriding it:
        // buttons, pressure plates, lever, redstone torch/wire/block, detector
        // rail, daylight detector, observer, target, repeater, comparator,
        // tripwire hook, trapped chest, lectern, jukebox, sculk sensors,
        // lightning rod. Resolved from registry slugs once so the wood-type
        // button/plate families never fall out of sync with BlockDefs.inc.
        static const std::array<bool, static_cast<size_t>(BlockID::Count)> table = [] {
            std::array<bool, static_cast<size_t>(BlockID::Count)> t{};
            auto ends_with = [](std::string_view s, std::string_view suffix) {
                return s.size() >= suffix.size() &&
                       s.substr(s.size() - suffix.size()) == suffix;
            };
            for (size_t i = 0; i < t.size(); ++i) {
                const std::string& slug =
                    BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                t[i] = ends_with(slug, "_button") || ends_with(slug, "_pressure_plate") ||
                       slug == "lever" || slug == "redstone_torch" ||
                       slug == "redstone_wall_torch" || slug == "redstone_wire" ||
                       slug == "redstone_block" || slug == "detector_rail" ||
                       slug == "daylight_detector" || slug == "observer" ||
                       slug == "target" || slug == "repeater" || slug == "comparator" ||
                       slug == "tripwire_hook" || slug == "trapped_chest" ||
                       slug == "lectern" || slug == "jukebox" ||
                       slug == "sculk_sensor" || slug == "calibrated_sculk_sensor" ||
                       slug == "lightning_rod";
            }
            return t;
        }();
        return table[static_cast<size_t>(block)];
    }

    static bool IsFireImmuneType(EntityTypeId type) {
        // EntityType.java `.fireImmune()` rows, restricted to types this
        // engine registers.
        switch (type) {
            case EntityTypeId::Blaze:
            case EntityTypeId::EnderDragon:
            case EntityTypeId::Ghast:
            case EntityTypeId::MagmaCube:
            case EntityTypeId::Shulker:
            case EntityTypeId::Strider:
            case EntityTypeId::Vex:
            case EntityTypeId::Warden:
            case EntityTypeId::Wither:
            case EntityTypeId::WitherSkeleton:
            case EntityTypeId::Zoglin:
            case EntityTypeId::ZombifiedPiglin:
                return true;
            default:
                return false;
        }
    }

    // MC EntityType.isBlockDangerous: burning blocks unless fire-immune, plus
    // wither rose / sweet berry bush / cactus / powder snow minus the type's
    // `.immuneTo(...)` entries.
    static bool IsBlockDangerousFor(EntityTypeId type, BlockID block) {
        // EntityType.Builder.immuneTo rows.
        if (block == BlockID::SweetBerryBush && type == EntityTypeId::Fox) return false;
        if (block == BlockID::PowderSnow &&
            (type == EntityTypeId::PolarBear || type == EntityTypeId::SnowGolem ||
             type == EntityTypeId::Stray)) {
            return false;
        }
        if (block == BlockID::WitherRose &&
            (type == EntityTypeId::Wither || type == EntityTypeId::WitherSkeleton)) {
            return false;
        }

        // NodeEvaluator.isBurningBlock: fire tag, lava, magma, lit campfire,
        // lava cauldron. Campfire lit-ness is a blockstate; treating every
        // campfire as burning errs on the vanilla-safe side for spawning.
        const bool burning = block == BlockID::Fire || block == BlockID::SoulFire ||
                             block == BlockID::Lava || block == BlockID::MagmaBlock ||
                             block == BlockID::Campfire || block == BlockID::SoulCampfire ||
                             block == BlockID::LavaCauldron;
        if (burning && !IsFireImmuneType(type)) return true;

        return block == BlockID::WitherRose || block == BlockID::SweetBerryBush ||
               block == BlockID::Cactus || block == BlockID::PowderSnow;
    }

    bool IsValidEmptySpawnBlock(EntityTypeId type, const IBlockAccess& blocks,
                                int x, int y, int z) {
        // MC NaturalSpawner.isValidEmptySpawnBlock, in MC's order.
        if (IsCollisionShapeFullBlock(blocks, x, y, z)) return false;
        const BlockID block = blocks.GetBlock(x, y, z);
        if (IsSignalSource(block)) return false;
        if (blocks.IsBlockFluid(x, y, z)) return false;
        if (SpawnTags::PreventMobSpawningInside(block)) return false;
        return !IsBlockDangerousFor(type, block);
    }

    bool CanSpawnFarFromPlayer(EntityTypeId type) {
        const MobCategory category = GetEntityTypeInfo(type).category;
        return category == MobCategory::Creature || category == MobCategory::Misc ||
               type == EntityTypeId::Pillager || type == EntityTypeId::Shulker;
    }

    float GetSpawnDimensionsScale(EntityTypeId type) {
        // EntityType.Builder.spawnDimensionsScale — slime and magma cube spawn
        // at up to size 4, so their spawn box is 4x the base dimensions.
        return (type == EntityTypeId::Slime || type == EntityTypeId::MagmaCube)
            ? 4.0f : 1.0f;
    }

    // ── Placement types (MC SpawnPlacementTypes) ────────────────────────────

    SpawnPlacementType GetSpawnPlacementType(EntityTypeId type) {
        switch (type) {
            // IN_WATER rows.
            case EntityTypeId::Axolotl:
            case EntityTypeId::Cod:
            case EntityTypeId::Dolphin:
            case EntityTypeId::Drowned:
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Salmon:
            case EntityTypeId::Squid:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::GlowSquid:
            case EntityTypeId::Nautilus:
                return SpawnPlacementType::InWater;

            // IN_LAVA row.
            case EntityTypeId::Strider:
                return SpawnPlacementType::InLava;

            // NO_RESTRICTIONS rows (registered with a predicate but no
            // placement restriction).
            case EntityTypeId::Evoker:
            case EntityTypeId::Fox:
            case EntityTypeId::Illusioner:
            case EntityTypeId::Panda:
            case EntityTypeId::Phantom:
            case EntityTypeId::Shulker:
            case EntityTypeId::TraderLlama:
            case EntityTypeId::Vex:
            case EntityTypeId::Vindicator:
            case EntityTypeId::Warden:
                return SpawnPlacementType::NoRestrictions;

            // ON_GROUND rows.
            case EntityTypeId::Armadillo:
            case EntityTypeId::Bat:
            case EntityTypeId::Blaze:
            case EntityTypeId::Bogged:
            case EntityTypeId::Breeze:
            case EntityTypeId::Camel:
            case EntityTypeId::CamelHusk:
            case EntityTypeId::CaveSpider:
            case EntityTypeId::Chicken:
            case EntityTypeId::Cow:
            case EntityTypeId::Creeper:
            case EntityTypeId::Donkey:
            case EntityTypeId::Enderman:
            case EntityTypeId::Endermite:
            case EntityTypeId::EnderDragon:
            case EntityTypeId::Frog:
            case EntityTypeId::Ghast:
            case EntityTypeId::HappyGhast:
            case EntityTypeId::Giant:
            case EntityTypeId::Goat:
            case EntityTypeId::Horse:
            case EntityTypeId::Husk:
            case EntityTypeId::IronGolem:
            case EntityTypeId::Llama:
            case EntityTypeId::MagmaCube:
            case EntityTypeId::Mooshroom:
            case EntityTypeId::Mule:
            case EntityTypeId::Ocelot:
            case EntityTypeId::Parrot:
            case EntityTypeId::Pig:
            case EntityTypeId::Hoglin:
            case EntityTypeId::Piglin:
            case EntityTypeId::Pillager:
            case EntityTypeId::PolarBear:
            case EntityTypeId::Rabbit:
            case EntityTypeId::Ravager:
            case EntityTypeId::Sheep:
            case EntityTypeId::Silverfish:
            case EntityTypeId::Skeleton:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::Slime:
            case EntityTypeId::SnowGolem:
            case EntityTypeId::Spider:
            case EntityTypeId::Stray:
            case EntityTypeId::Parched:
            case EntityTypeId::Turtle:
            case EntityTypeId::Villager:
            case EntityTypeId::WanderingTrader:
            case EntityTypeId::Witch:
            case EntityTypeId::Wither:
            case EntityTypeId::WitherSkeleton:
            case EntityTypeId::Wolf:
            case EntityTypeId::Zoglin:
            case EntityTypeId::Creaking:
            case EntityTypeId::Zombie:
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::ZombifiedPiglin:
            case EntityTypeId::ZombieVillager:
            case EntityTypeId::Cat:
                return SpawnPlacementType::OnGround;

            // Unregistered in MC (allay, bee, sniffer, tadpole, copper golem,
            // zombie nautilus, ...) — NO_RESTRICTIONS is MC's fallthrough.
            default:
                return SpawnPlacementType::NoRestrictions;
        }
    }

    bool IsSpawnPositionOk(EntityTypeId type, const IBlockAccess& blocks,
                           int x, int y, int z) {
        switch (GetSpawnPlacementType(type)) {
            case SpawnPlacementType::NoRestrictions:
                return true;

            case SpawnPlacementType::InWater:
                // Water at the position, and the block ABOVE must not be a
                // redstone conductor (full opaque cube) — squid need headroom.
                return blocks.ContainsWater(x, y, z) &&
                       !IsCollisionShapeFullBlock(blocks, x, y + 1, z);

            case SpawnPlacementType::InLava:
                return blocks.GetBlock(x, y, z) == BlockID::Lava;

            case SpawnPlacementType::OnGround: {
                if (!IsValidSpawnBlock(blocks, x, y - 1, z)) return false;
                return IsValidEmptySpawnBlock(type, blocks, x, y, z) &&
                       IsValidEmptySpawnBlock(type, blocks, x, y + 1, z);
            }
        }
        return false;
    }

    // ── Per-type spawn rules ────────────────────────────────────────────────

    namespace {

        std::string_view BiomeAt(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!ctx.biomeAt || !*ctx.biomeAt) return {};
            return (*ctx.biomeAt)(pos.x, pos.y, pos.z);
        }

        // MC Mob.checkMobSpawnRules.
        bool CheckMobSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return IsSpawner(ctx.reason) ||
                   IsValidSpawnBlock(ctx.blocks, pos.x, pos.y - 1, pos.z);
        }

        // MC WaterAnimal.checkSurfaceWaterAnimalSpawnRules — also the body of
        // AgeableWaterCreature.checkSurfaceAgeableWaterCreatureSpawnRules.
        bool CheckSurfaceWaterAnimalSpawnRules(const SpawnRuleContext& ctx,
                                               const glm::ivec3& pos) {
            const int seaLevel = ctx.seaLevel;
            return pos.y >= seaLevel - 13 && pos.y <= seaLevel &&
                   ctx.blocks.ContainsWater(pos.x, pos.y - 1, pos.z) &&
                   ctx.blocks.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Water;
        }

        // MC LevelReader.canSeeSkyFromBelowWater — climb out of the water
        // column first, then ask canSeeSky.
        bool CanSeeSkyFromBelowWater(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            int y = pos.y;
            while (ctx.blocks.GetBlock(pos.x, y, pos.z) == BlockID::Water) ++y;
            return ctx.level.CanSeeSky(pos.x, y, pos.z);
        }

        bool CheckDrownedSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            // MC Drowned.checkDrownedSpawnRules, structure preserved.
            if (!ctx.blocks.ContainsWater(pos.x, pos.y - 1, pos.z) && !IsSpawner(ctx.reason)) {
                return false;
            }
            const bool canMonsterSpawn =
                ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                (IgnoresLightRequirements(ctx.reason) ||
                 Monster::IsDarkEnoughToSpawn(ctx.level, pos, ctx.rng)) &&
                (IsSpawner(ctx.reason) || ctx.blocks.ContainsWater(pos.x, pos.y, pos.z));
            if (canMonsterSpawn &&
                (IsSpawner(ctx.reason) || ctx.reason == SpawnReason::Reinforcement)) {
                return true;
            }
            if (SpawnTags::MoreFrequentDrownedSpawns(BiomeAt(ctx, pos))) {
                return ctx.rng.NextInt(15) == 0 && canMonsterSpawn;
            }
            // isDeepEnoughToSpawn: below seaLevel - 5.
            return ctx.rng.NextInt(40) == 0 && pos.y < ctx.seaLevel - 5 && canMonsterSpawn;
        }

        bool CheckGuardianSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return (ctx.rng.NextInt(20) == 0 || !CanSeeSkyFromBelowWater(ctx, pos)) &&
                   ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                   (IsSpawner(ctx.reason) || ctx.blocks.ContainsWater(pos.x, pos.y, pos.z)) &&
                   ctx.blocks.ContainsWater(pos.x, pos.y - 1, pos.z);
        }

        bool CheckTropicalFishSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return ctx.blocks.ContainsWater(pos.x, pos.y - 1, pos.z) &&
                   ctx.blocks.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Water &&
                   (SpawnTags::AllowsTropicalFishSpawnsAtAnyHeight(BiomeAt(ctx, pos)) ||
                    CheckSurfaceWaterAnimalSpawnRules(ctx, pos));
        }

        bool CheckBatSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (ctx.surfaceHeight && *ctx.surfaceHeight &&
                pos.y >= (*ctx.surfaceHeight)(pos.x, pos.z)) {
                return false;
            }
            if (ctx.rng.NextBool()) return false;
            if (ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z) >
                ctx.rng.NextInt(4)) {
                return false;
            }
            if (!SpawnTags::BatsSpawnableOn(ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z))) {
                return false;
            }
            return CheckMobSpawnRules(ctx, pos);
        }

        // MC Endermite.checkEndermiteSpawnRules — also Silverfish's body.
        bool CheckNearbyPlayerFreeMonsterSpawnRules(const SpawnRuleContext& ctx,
                                                    const glm::ivec3& pos) {
            if (!Monster::CheckAnyLightMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng)) {
                return false;
            }
            if (IsSpawner(ctx.reason)) return true;
            // getNearestPlayer(x+0.5, y+0.5, z+0.5, 5.0, true) must be null.
            // The `true` selects only survival (non-creative) players; the
            // engine's GetNearestPlayer does not filter creative, which only
            // matters for endermite/silverfish farms run in creative mode.
            return ctx.level.GetNearestPlayer(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                                              5.0) == nullptr;
        }

        bool CheckGhastSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                   ctx.rng.NextInt(20) == 0 && CheckMobSpawnRules(ctx, pos);
        }

        bool CheckGlowSquidSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return pos.y <= ctx.seaLevel - 33 &&
                   ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 0) == 0 &&
                   ctx.blocks.GetBlock(pos.x, pos.y, pos.z) == BlockID::Water;
        }

        bool CheckNautilusSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            const int seaLevel = ctx.seaLevel;
            return pos.y >= seaLevel - 25 && pos.y <= seaLevel - 5 &&
                   ctx.blocks.ContainsWater(pos.x, pos.y - 1, pos.z) &&
                   ctx.blocks.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::Water;
        }

        bool CheckPatrollingMonsterSpawnRules(const SpawnRuleContext& ctx,
                                              const glm::ivec3& pos) {
            // MC: block light > 8 rejects. Block light is 0 without a light
            // engine, so the first clause never fires yet.
            constexpr int kBlockLight = 0;
            if (kBlockLight > 8) return false;
            return Monster::CheckAnyLightMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng);
        }

        bool CheckPolarBearSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!SpawnTags::PolarBearsSpawnOnAlternateBlocks(BiomeAt(ctx, pos))) {
                return Animal::CheckAnimalSpawnRules(ctx.level, ctx.reason, pos);
            }
            return Animal::IsBrightEnoughToSpawn(ctx.level, pos) &&
                   SpawnTags::PolarBearsSpawnableOnAlternate(
                       ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z));
        }

        bool CheckSkeletonHorseSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!IsSpawner(ctx.reason)) {
                return Animal::CheckAnimalSpawnRules(ctx.level, ctx.reason, pos);
            }
            return IgnoresLightRequirements(ctx.reason) ||
                   Animal::IsBrightEnoughToSpawn(ctx.level, pos);
        }

        // DimensionType.MOON_BRIGHTNESS_PER_PHASE, phase = (dayTime/24000) % 8.
        float MoonBrightness(const EntityLevel& level) {
            static constexpr float kPerPhase[8] = {
                1.0f, 0.75f, 0.5f, 0.25f, 0.0f, 0.25f, 0.5f, 0.75f
            };
            const int64_t day = level.GetDayTime() / 24000;
            return kPerPhase[static_cast<size_t>(day % 8)];
        }

        bool CheckSlimeSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            // MC Slime.checkSlimeSpawnRules. Draw order preserved exactly.
            if (ctx.level.GetDifficulty() == Difficulty::Peaceful) return false;
            if (IsSpawner(ctx.reason)) return CheckMobSpawnRules(ctx, pos);

            if (SpawnTags::AllowsSurfaceSlimeSpawns(BiomeAt(ctx, pos)) &&
                pos.y > 50 && pos.y < 70) {
                // EnvironmentAttributes.SURFACE_SLIME_SPAWN_CHANCE — the moon
                // timeline keys it at moonBrightness * 0.5 per phase.
                const float chance = MoonBrightness(ctx.level) * 0.5f;
                if (ctx.rng.NextFloat() < chance &&
                    ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z) <=
                        ctx.rng.NextInt(8)) {
                    return CheckMobSpawnRules(ctx, pos);
                }
            }

            // Slime chunk: WorldgenRandom.seedSlimeChunk — a throwaway
            // java.util.Random over a chunk-position hash of the world seed.
            // The first two and last terms are INT arithmetic in Java and must
            // wrap; unsigned math reproduces Java's wraparound without UB.
            const int cx = pos.x >> 4;
            const int cz = pos.z >> 4;
            const auto wrapMul = [](int a, int b, int c) {
                return static_cast<int64_t>(static_cast<int32_t>(
                    static_cast<uint32_t>(a) * static_cast<uint32_t>(b) *
                    static_cast<uint32_t>(c)));
            };
            const int64_t seed =
                (ctx.worldSeed +
                 wrapMul(cx, cx, 4987142) +
                 wrapMul(cx, 5947611, 1) +
                 static_cast<int64_t>(static_cast<int32_t>(
                     static_cast<uint32_t>(cz) * static_cast<uint32_t>(cz))) * 4392871LL +
                 wrapMul(cz, 389711, 1)) ^ 987234911LL;
            JavaRandom slimeRng(seed);
            const bool slimeChunk = slimeRng.NextInt(10) == 0;
            if (ctx.rng.NextInt(10) == 0 && slimeChunk && pos.y < 40) {
                return CheckMobSpawnRules(ctx, pos);
            }
            return false;
        }

        bool CheckStraySpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            // Climb out of any powder-snow column before the sky test.
            int skyY = pos.y;
            do {
                ++skyY;
            } while (ctx.blocks.GetBlock(pos.x, skyY, pos.z) == BlockID::PowderSnow);

            return Monster::CheckMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng) &&
                   (IsSpawner(ctx.reason) || ctx.level.CanSeeSky(pos.x, skyY - 1, pos.z));
        }

        bool CheckStriderSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            int y = pos.y;
            while (ctx.blocks.GetBlock(pos.x, y, pos.z) == BlockID::Lava) ++y;
            return ctx.blocks.GetBlock(pos.x, y, pos.z) == BlockID::Air;
        }

        bool CheckTurtleSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            // TurtleEggBlock.onSand — the block BELOW the spawn position.
            return pos.y < ctx.seaLevel + 4 &&
                   SpawnTags::Sand(ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z)) &&
                   Animal::IsBrightEnoughToSpawn(ctx.level, pos);
        }

        // The "block below in tag + bright enough" family.
        bool CheckSpawnableOnAndBright(const SpawnRuleContext& ctx, const glm::ivec3& pos,
                                       bool (*tag)(BlockID)) {
            return tag(ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z)) &&
                   Animal::IsBrightEnoughToSpawn(ctx.level, pos);
        }

    } // namespace

    bool CheckSpawnRules(EntityTypeId type, const SpawnRuleContext& ctx,
                         const glm::ivec3& pos) {
        EntityLevel& level = ctx.level;
        switch (type) {
            // Monster.checkMonsterSpawnRules rows.
            case EntityTypeId::Bogged:
            case EntityTypeId::CaveSpider:
            case EntityTypeId::Creeper:
            case EntityTypeId::Enderman:
            case EntityTypeId::Giant:
            case EntityTypeId::Skeleton:
            case EntityTypeId::Spider:
            case EntityTypeId::Witch:
            case EntityTypeId::Wither:
            case EntityTypeId::WitherSkeleton:
            case EntityTypeId::Creaking:
            case EntityTypeId::Zombie:
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::ZombieVillager:
            case EntityTypeId::Evoker:
            case EntityTypeId::Illusioner:
            case EntityTypeId::Ravager:
            case EntityTypeId::Vex:
            case EntityTypeId::Vindicator:
            case EntityTypeId::Warden:
                return Monster::CheckMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);

            // Monster.checkAnyLightMonsterSpawnRules rows.
            case EntityTypeId::Blaze:
            case EntityTypeId::Breeze:
            case EntityTypeId::Zoglin:
                return Monster::CheckAnyLightMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);

            // Monster.checkSurfaceMonstersSpawnRules rows.
            case EntityTypeId::CamelHusk:
            case EntityTypeId::Husk:
            case EntityTypeId::Parched:
                return Monster::CheckSurfaceMonstersSpawnRules(level, ctx.reason, pos, ctx.rng);

            // Animal.checkAnimalSpawnRules rows.
            case EntityTypeId::Chicken:
            case EntityTypeId::Cow:
            case EntityTypeId::Donkey:
            case EntityTypeId::HappyGhast:
            case EntityTypeId::Horse:
            case EntityTypeId::Llama:
            case EntityTypeId::Mule:
            case EntityTypeId::Pig:
            case EntityTypeId::Sheep:
            case EntityTypeId::Cat:
            case EntityTypeId::Panda:
            case EntityTypeId::TraderLlama:
                return Animal::CheckAnimalSpawnRules(level, ctx.reason, pos);

            // Mob.checkMobSpawnRules rows.
            case EntityTypeId::EnderDragon:
            case EntityTypeId::IronGolem:
            case EntityTypeId::SnowGolem:
            case EntityTypeId::Villager:
            case EntityTypeId::WanderingTrader:
            case EntityTypeId::Phantom:
            case EntityTypeId::Shulker:
                return CheckMobSpawnRules(ctx, pos);

            // Per-type predicates.
            case EntityTypeId::Axolotl:
                return SpawnTags::AxolotlsSpawnableOn(
                    ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z));
            case EntityTypeId::Cod:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Salmon:
            case EntityTypeId::Dolphin:
            case EntityTypeId::Squid:
                return CheckSurfaceWaterAnimalSpawnRules(ctx, pos);
            case EntityTypeId::Drowned:
                return CheckDrownedSpawnRules(ctx, pos);
            case EntityTypeId::Guardian:
            case EntityTypeId::ElderGuardian:
                return CheckGuardianSpawnRules(ctx, pos);
            case EntityTypeId::TropicalFish:
                return CheckTropicalFishSpawnRules(ctx, pos);
            case EntityTypeId::Armadillo:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::ArmadilloSpawnableOn);
            case EntityTypeId::Bat:
                return CheckBatSpawnRules(ctx, pos);
            case EntityTypeId::Camel:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::CamelsSpawnableOn);
            case EntityTypeId::Endermite:
            case EntityTypeId::Silverfish:
                return CheckNearbyPlayerFreeMonsterSpawnRules(ctx, pos);
            case EntityTypeId::Frog:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::FrogsSpawnableOn);
            case EntityTypeId::Ghast:
                return CheckGhastSpawnRules(ctx, pos);
            case EntityTypeId::GlowSquid:
                return CheckGlowSquidSpawnRules(ctx, pos);
            case EntityTypeId::Goat:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::GoatsSpawnableOn);
            case EntityTypeId::MagmaCube:
                return level.GetDifficulty() != Difficulty::Peaceful;
            case EntityTypeId::Mooshroom:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::MooshroomsSpawnableOn);
            case EntityTypeId::Nautilus:
                return CheckNautilusSpawnRules(ctx, pos);
            case EntityTypeId::Ocelot:
                return ctx.rng.NextInt(3) != 0;
            case EntityTypeId::Parrot:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::ParrotsSpawnableOn);
            case EntityTypeId::Hoglin:
            case EntityTypeId::Piglin:
                return ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z) !=
                       BlockID::NetherWartBlock;
            case EntityTypeId::Pillager:
                return CheckPatrollingMonsterSpawnRules(ctx, pos);
            case EntityTypeId::PolarBear:
                return CheckPolarBearSpawnRules(ctx, pos);
            case EntityTypeId::Rabbit:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::RabbitsSpawnableOn);
            case EntityTypeId::SkeletonHorse:
                return CheckSkeletonHorseSpawnRules(ctx, pos);
            case EntityTypeId::Slime:
                return CheckSlimeSpawnRules(ctx, pos);
            case EntityTypeId::Stray:
                return CheckStraySpawnRules(ctx, pos);
            case EntityTypeId::Strider:
                return CheckStriderSpawnRules(ctx, pos);
            case EntityTypeId::Turtle:
                return CheckTurtleSpawnRules(ctx, pos);
            case EntityTypeId::Wolf:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::WolvesSpawnableOn);
            case EntityTypeId::ZombifiedPiglin:
                return level.GetDifficulty() != Difficulty::Peaceful &&
                       ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z) !=
                           BlockID::NetherWartBlock;
            case EntityTypeId::Fox:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::FoxesSpawnableOn);

            // Unregistered in MC's table — checkSpawnRules answers true.
            default:
                return true;
        }
    }

} // namespace Game
