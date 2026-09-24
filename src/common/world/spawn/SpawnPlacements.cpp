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
#include "common/entity/mobs/AetherMobs.hpp"   // AetherIds (aether grass)
#include "common/entity/mobs/TwilightCreatures.hpp"
#include "common/entity/mobs/TwilightHostiles.hpp"
#include "common/entity/mobs/TwilightMobs.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <array>
#include <cmath>

namespace Game {

    // ── Shared position tests ───────────────────────────────────────────────

    bool IsCollisionShapeFullBlock(const IBlockAccess& blocks, int x, int y, int z) {
        // One read: the state carries its block. The spawner runs this on
        // every category's start position in every spawn chunk.
        const BlockState state = blocks.GetBlockState(x, y, z);
        if (!BlockRegistry::HasCollision(state.Block())) return false;

        const auto& shape = BlockRegistry::GetBlockShape(state);
        return shape.min.x <= 0.0f && shape.min.y <= 0.0f && shape.min.z <= 0.0f &&
               shape.max.x >= 1.0f && shape.max.y >= 1.0f && shape.max.z >= 1.0f;
    }

    // MC BlockBehaviour.isFaceSturdy(UP), which is what the default
    // isValidSpawn tests on the block below. VoxelShape.getFaceShape(UP)
    // projects the shape's TOP face, so a bottom slab counts — mobs really do
    // spawn on slabs in vanilla — while a fence post or a torch does not.
    static bool IsTopFaceSturdy(const IBlockAccess& blocks, int x, int y, int z) {
        const BlockState state = blocks.GetBlockState(x, y, z);
        if (!BlockRegistry::HasCollision(state.Block())) return false;

        const auto& shape = BlockRegistry::GetBlockShape(state);
        return shape.min.x <= 0.0f && shape.max.x >= 1.0f &&
               shape.min.z <= 0.0f && shape.max.z >= 1.0f;
    }

    // MC Blocks.java `.isValidSpawn(Blocks::never)` — the blocks nothing may
    // spawn on top of even though their top face is sturdy: bedrock, barrier,
    // glass, the stained glasses, tinted glass, moving_piston, every trapdoor
    // (wood, iron, the copper family), the copper grates, chorus plant and
    // scaffolding. Resolved from registry slugs once, like IsSignalSource, so
    // the wood and copper families track BlockDefs.inc.
    //
    // Not covered: the leaves' `Blocks::ocelotOrParrot` (a per-type rule —
    // only ocelots and parrots may spawn on leaves), which needs the entity
    // type this test does not take.
    static bool NeverValidSpawn(BlockID block) {
        static const std::array<bool, static_cast<size_t>(BlockID::Count)> table = [] {
            std::array<bool, static_cast<size_t>(BlockID::Count)> t{};
            auto ends_with = [](std::string_view s, std::string_view suffix) {
                return s.size() >= suffix.size() &&
                       s.substr(s.size() - suffix.size()) == suffix;
            };
            for (size_t i = 0; i < t.size(); ++i) {
                const std::string& slug =
                    BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                t[i] = slug == "bedrock" || slug == "barrier" || slug == "glass" ||
                       slug == "tinted_glass" || ends_with(slug, "_stained_glass") ||
                       slug == "moving_piston" || ends_with(slug, "trapdoor") ||
                       ends_with(slug, "copper_grate") || slug == "chorus_plant" ||
                       slug == "scaffolding";
            }
            return t;
        }();
        return table[static_cast<size_t>(block)];
    }

    bool IsValidSpawnBlock(const IBlockAccess& blocks, int x, int y, int z) {
        // MC BlockBehaviour.isValidSpawn: the block's own rule when it has one
        // (the `never` set above), else isFaceSturdy(UP) && getLightEmission()
        // < 14. No emission data here yet (glowstone, magma, lava — though
        // magma is separately dangerous).
        if (NeverValidSpawn(blocks.GetBlock(x, y, z))) return false;
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
                       slug == "blue_redstone_torch" || slug == "blue_redstone_wall_torch" ||
                       slug == "display_block" ||
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
            case EntityTypeId::SilentWarden:   // a warden (HushMobs.hpp)
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
                                int x, int y, int z, bool checkFluid) {
        // MC NaturalSpawner.isValidEmptySpawnBlock, in MC's order.
        if (IsCollisionShapeFullBlock(blocks, x, y, z)) return false;
        const BlockID block = blocks.GetBlock(x, y, z);
        if (IsSignalSource(block)) return false;
        if (checkFluid && blocks.IsBlockFluid(x, y, z)) return false;
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
        // TF MazeSlime: TFEntities `.spawnDimensionsScale(4.0F)`. The sulfur
        // cube (26.3 EntityTypes.SULFUR_CUBE) registers 2.0.
        if (type == EntityTypeId::Slime || type == EntityTypeId::MagmaCube ||
            type == EntityTypeId::MazeSlime) {
            return 4.0f;
        }
        return type == EntityTypeId::SulfurCube ? 2.0f : 1.0f;
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

            // The Hush (HushMobs.hpp): the wraith is placed like the vex it
            // rides (NO_RESTRICTIONS — its rule below still wants a floor
            // under the spawn cell, so it starts a block off the ground and
            // takes off); the boss is never placed naturally at all.
            case EntityTypeId::EchoWraith:
            case EntityTypeId::SilentWarden:
                return SpawnPlacementType::NoRestrictions;
            // The deep-Hush creatures (HushCreatures.hpp): the flyers are
            // placed like the vex (the moth takes off from the cell it was
            // given; the leviathan is lifted into the sky by FinalizeSpawn);
            // the Choir Mother is only ever summoned at her altar.
            case EntityTypeId::LumenMoth:
            case EntityTypeId::HushLeviathan:
            case EntityTypeId::ChoirMother:
            // Aurelith's Unsung rises from the Heart's dais (AurelithCities)
            // or hatches from its egg; never placed naturally.
            case EntityTypeId::TheUnsung:
                return SpawnPlacementType::NoRestrictions;
            // The golem and the mimic walk: ON_GROUND.
            case EntityTypeId::CrystalGolem:
            case EntityTypeId::EchoMimic:
                return SpawnPlacementType::OnGround;

            // The Aether's zephyr (AetherMobs.hpp) takes a FLYING rule: the
            // mod registers it ON_GROUND, which on floating islands seats a
            // cloud on the grass; placed NO_RESTRICTIONS it appears in the
            // open air around the islands, and its rule below drops the
            // floor test to match.
            case EntityTypeId::Zephyr:
                return SpawnPlacementType::NoRestrictions;

            // Twilight Forest and Aether creatures (TwilightMobs.hpp,
            // AetherMobs.hpp) — every other one is ON_GROUND in its mod
            // (TFEntities.registerWithEgg's default; AetherEntityTypes.
            // registerSpawnPlacements).
            case EntityTypeId::Deer:
            case EntityTypeId::Boar:
            case EntityTypeId::BighornSheep:
            case EntityTypeId::TinyBird:
            case EntityTypeId::Kobold:
            case EntityTypeId::Redcap:
            case EntityTypeId::Phyg:
            case EntityTypeId::FlyingCow:
            case EntityTypeId::Sheepuff:
            case EntityTypeId::Cockatrice:
            // Pass two — TF biome-spawner creatures (TwilightMobs.hpp /
            // TwilightCreatures.hpp), all TFEntities.registerWithEgg's
            // ON_GROUND.
            case EntityTypeId::Squirrel:
            case EntityTypeId::Raven:
            case EntityTypeId::DwarfRabbit:
            case EntityTypeId::Penguin:
            case EntityTypeId::KingSpider:
            case EntityTypeId::HostileWolf:
            case EntityTypeId::MistWolf:
            case EntityTypeId::WinterWolf:
            case EntityTypeId::MosquitoSwarm:
            case EntityTypeId::SkeletonDruid:
            case EntityTypeId::Yeti:
            // Pass two — TF landmark hostiles (TwilightHostiles.hpp).
            case EntityTypeId::HedgeSpider:
            case EntityTypeId::SwarmSpider:
            case EntityTypeId::Wraith:
            case EntityTypeId::FireBeetle:
            case EntityTypeId::SlimeBeetle:
            case EntityTypeId::PinchBeetle:
            case EntityTypeId::HelmetCrab:
            case EntityTypeId::Troll:
            case EntityTypeId::TowerwoodBorer:
            case EntityTypeId::MazeSlime:
            case EntityTypeId::Minotaur:
            case EntityTypeId::RedcapSapper:
            case EntityTypeId::BlockAndChainGoblin:
            case EntityTypeId::UpperGoblinKnight:
            case EntityTypeId::LowerGoblinKnight:
            // Pass two — the Aether (AetherEntityTypes.registerSpawnPlacements:
            // ON_GROUND for every spawner creature).
            case EntityTypeId::Moa:
            case EntityTypeId::Aerbunny:
            case EntityTypeId::Aerwhale:
            case EntityTypeId::BlueSwet:
            case EntityTypeId::GoldenSwet:
            case EntityTypeId::Whirlwind:
            case EntityTypeId::EvilWhirlwind:
            case EntityTypeId::AechorPlant:
                return SpawnPlacementType::OnGround;

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
            case EntityTypeId::Hushling:   // The Hush's creature, endermite-shaped
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
            // MC PatrollingMonster.checkPatrollingMonsterSpawnRules: block
            // light > 8 rejects (a lit village keeps patrols out).
            if (ctx.level.GetBlockBrightness(pos.x, pos.y, pos.z) > 8) return false;
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

        // Aether AetherAnimal.checkAetherAnimalSpawnRules: aether grass
        // below (#aether:aether_animals_spawnable_on) and raw brightness > 8.
        // Resolved by slug, so it answers false until the block exists.
        bool CheckAetherAnimalSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            const BlockID grass = AetherIds::AetherGrassBlock();
            return grass != BlockID::Air &&
                   ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z) == grass &&
                   Animal::IsBrightEnoughToSpawn(ctx.level, pos);
        }

        // Aether Cockatrice.checkCockatriceSpawnRules: the monster rule
        // (Mob.checkMobSpawnRules + isDarkEnoughToSpawn + not peaceful) and a
        // 1-in-3 roll for natural spawns. The spawnable-blacklist tag is
        // empty-by-default data the Aether pass has not brought over.
        bool CheckCockatriceSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return Monster::CheckMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng) &&
                   (ctx.reason != SpawnReason::Natural || ctx.rng.NextInt(3) == 0);
        }

        // Aether Zephyr.checkZephyrSpawnRules as a FLYING rule: the whole
        // 5x5 hitbox footprint sees the sky (EntityUtil.wholeHitboxCanSeeSky
        // radius 2), not peaceful, and a 1-in-11 roll for natural spawns.
        // Mob.checkMobSpawnRules' floor test is dropped with the ON_GROUND
        // placement (see GetSpawnPlacementType).
        bool CheckZephyrSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    if (!ctx.level.CanSeeSky(pos.x + dx, pos.y, pos.z + dz)) return false;
                }
            }
            return ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                   (ctx.reason != SpawnReason::Natural || ctx.rng.NextInt(11) == 0);
        }

        // The Aether's raw brightness test (its `level.getRawBrightness(pos,
        // 0) > n` rules) — this engine's max local raw brightness.
        bool AetherLightAbove(const SpawnRuleContext& ctx, const glm::ivec3& pos, int n) {
            return ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z) > n;
        }

        bool OnAetherGrass(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            const BlockID grass = AetherIds::AetherGrassBlock();
            return grass != BlockID::Air &&
                   ctx.blocks.GetBlock(pos.x, pos.y - 1, pos.z) == grass;
        }

        // Aether Aerwhale.checkAerwhaleSpawnRules: the mob rule, no fluid at
        // the cell, bright, open sky over the 3x3 around it, and a 1-in-40
        // roll for natural spawns.
        bool CheckAerwhaleSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!CheckMobSpawnRules(ctx, pos)) return false;
            if (ctx.blocks.IsBlockFluid(pos.x, pos.y, pos.z)) return false;
            if (!AetherLightAbove(ctx, pos, 8)) return false;
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (!ctx.level.CanSeeSky(pos.x + dx, pos.y, pos.z + dz)) return false;
                }
            }
            return ctx.reason != SpawnReason::Natural || ctx.rng.NextInt(40) == 0;
        }

        // Aether Swet.checkSwetSpawnRules.
        bool CheckSwetSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return OnAetherGrass(ctx, pos) && AetherLightAbove(ctx, pos, 8) &&
                   ctx.level.GetDifficulty() != Difficulty::Peaceful;
        }

        // Aether AbstractWhirlwind.checkWhirlwindSpawnRules.
        bool CheckWhirlwindSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return CheckMobSpawnRules(ctx, pos) && AetherLightAbove(ctx, pos, 12) &&
                   ctx.level.GetDifficulty() != Difficulty::Peaceful;
        }

        // Aether AechorPlant.checkAechorPlantSpawnRules (its flower-deterrent
        // test is not ported).
        bool CheckAechorPlantSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            return OnAetherGrass(ctx, pos) && AetherLightAbove(ctx, pos, 8) &&
                   ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                   (ctx.reason != SpawnReason::Natural || ctx.rng.NextInt(10) == 0);
        }

        // ── The deep-Hush creatures (HushCreatures.hpp) ─────────────────
        //
        // The Hush is always night and has no block light, so none of these
        // reads light the way a vanilla monster rule does.

        // The lumen moth: surface air (at most six blocks over the surface
        // column), open sky, no fluid. Swarms come from the biome's group
        // size (3-6) and GetMaxSpawnClusterSize.
        bool CheckLumenMothSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (ctx.blocks.IsBlockFluid(pos.x, pos.y, pos.z)) return false;
            if (!IsValidEmptySpawnBlock(EntityTypeId::LumenMoth, ctx.blocks,
                                        pos.x, pos.y, pos.z)) {
                return false;
            }
            if (ctx.surfaceHeight && *ctx.surfaceHeight) {
                const int surface = (*ctx.surfaceHeight)(pos.x, pos.z);
                if (pos.y < surface || pos.y > surface + 6) return false;
            }
            return ctx.level.CanSeeSky(pos.x, pos.y, pos.z);
        }

        // The crystal golem guards crystal: a resonant crystal or cluster
        // within 8 blocks (a 17 x 9 x 17 scan — creature spawns are rare, so
        // the cost is too), a floor, and 1 in 3 natural attempts.
        bool CheckCrystalGolemSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!CheckMobSpawnRules(ctx, pos)) return false;
            if (ctx.reason == SpawnReason::Natural && ctx.rng.NextInt(3) != 0) return false;
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -8; dx <= 8; ++dx) {
                    for (int dz = -8; dz <= 8; ++dz) {
                        const BlockID b = ctx.blocks.GetBlock(pos.x + dx, pos.y + dy, pos.z + dz);
                        if (b == BlockID::ResonantCrystal || b == BlockID::ResonantCluster) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // The leviathan: open sky over the cell and 1 in 8 natural attempts;
        // the one-per-area exclusion and the clear sky it is lifted into are
        // HushLeviathan::CheckSpawnObstruction's (they need the entity).
        bool CheckHushLeviathanSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (ctx.blocks.IsBlockFluid(pos.x, pos.y, pos.z)) return false;
            if (!ctx.level.CanSeeSky(pos.x, pos.y, pos.z)) return false;
            return ctx.reason != SpawnReason::Natural || ctx.rng.NextInt(8) == 0;
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

            // The Hush (HushMobs.hpp). It is always night there and the
            // open sky reads 15 raw sky light, which Monster.isDarkEnough
            // ToSpawn rejects about half the time — so the wraith takes
            // MC's checkAnyLightMonsterSpawnRules (peaceful + floor only,
            // the blaze/breeze rule) and the hushling Mob.checkMobSpawnRules
            // (floor only, any light: a creature). The boss never spawns
            // naturally; it is placed by the echo core's break path.
            case EntityTypeId::EchoWraith:
                return Monster::CheckAnyLightMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);
            case EntityTypeId::Hushling:
                return CheckMobSpawnRules(ctx, pos);
            case EntityTypeId::SilentWarden:
                return false;
            case EntityTypeId::LumenMoth:
                return CheckLumenMothSpawnRules(ctx, pos);
            case EntityTypeId::CrystalGolem:
                return CheckCrystalGolemSpawnRules(ctx, pos);
            case EntityTypeId::HushLeviathan:
                return CheckHushLeviathanSpawnRules(ctx, pos);
            // The mimic haunts the dark underground (Hollow Deep, Crystal
            // Caverns), where the vanilla monster rule's darkness test holds.
            case EntityTypeId::EchoMimic:
                return Monster::CheckMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);
            // Summoned at a Choir Hall altar (ChoirPuzzle), never spawned.
            case EntityTypeId::ChoirMother:
                return false;
            // Rises when an Aurelith is woken (AurelithCities), never spawned.
            case EntityTypeId::TheUnsung:
                return false;

            // Twilight Forest (TFEntities: Animal::checkAnimalSpawnRules for
            // the animals, Monster::checkMonsterSpawnRules for the kobold and
            // the redcap). The tiny bird takes MC's parrot rule instead — a
            // FLYING rule: parrots_spawnable_on (leaves, grass, logs, air)
            // plus light, so it can appear perched in the canopy where TF's
            // grass-only animal rule never puts it.
            case EntityTypeId::Deer:
            case EntityTypeId::Boar:
            case EntityTypeId::BighornSheep:
                return Animal::CheckAnimalSpawnRules(level, ctx.reason, pos);
            case EntityTypeId::TinyBird:
                return CheckSpawnableOnAndBright(ctx, pos, &SpawnTags::ParrotsSpawnableOn);
            case EntityTypeId::Kobold:
            case EntityTypeId::Redcap:
                return Monster::CheckMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);
            // Pass two — each TFEntities row's predicate.
            case EntityTypeId::Squirrel:
            case EntityTypeId::Raven:
            case EntityTypeId::DwarfRabbit:
                return Animal::CheckAnimalSpawnRules(level, ctx.reason, pos);
            case EntityTypeId::Penguin:
                return Penguin::CheckPenguinSpawnRules(level, pos);
            case EntityTypeId::KingSpider:
            case EntityTypeId::MistWolf:
            case EntityTypeId::MosquitoSwarm:
                return Monster::CheckMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);
            case EntityTypeId::HostileWolf:
                return HostileWolf::CheckWolfSpawnRules(ctx, pos);
            case EntityTypeId::WinterWolf:
                return WinterWolf::CheckWinterWolfSpawnRules(ctx, pos);
            case EntityTypeId::SkeletonDruid:
                return SkeletonDruid::CheckDruidSpawnRules(ctx, pos);
            case EntityTypeId::Yeti:
                return Yeti::CheckYetiSpawnRules(ctx, pos);
            // TF landmark hostiles (TFEntities predicates).
            case EntityTypeId::FireBeetle:
            case EntityTypeId::SlimeBeetle:
            case EntityTypeId::PinchBeetle:
            case EntityTypeId::HelmetCrab:
            case EntityTypeId::Troll:
            case EntityTypeId::TowerwoodBorer:
            case EntityTypeId::Minotaur:
            case EntityTypeId::RedcapSapper:
            case EntityTypeId::BlockAndChainGoblin:
            case EntityTypeId::UpperGoblinKnight:
            case EntityTypeId::LowerGoblinKnight:
                return Monster::CheckMonsterSpawnRules(level, ctx.reason, pos, ctx.rng);
            // HedgeSpider.canSpawn — no floor test.
            case EntityTypeId::HedgeSpider:
                return level.GetDifficulty() != Difficulty::Peaceful &&
                       HedgeSpider::IsValidLightLevel(level, pos, ctx.rng);
            // SwarmSpider.getCanSpawnHere — the hedge spider's rule plus
            // Mob.checkMobSpawnRules.
            case EntityTypeId::SwarmSpider:
                return level.GetDifficulty() != Difficulty::Peaceful &&
                       HedgeSpider::IsValidLightLevel(level, pos, ctx.rng) &&
                       CheckMobSpawnRules(ctx, pos);
            case EntityTypeId::Wraith:
                return Wraith::CheckSpawnRules(level, pos, ctx.rng) && CheckMobSpawnRules(ctx, pos);
            case EntityTypeId::MazeSlime:
                return MazeSlime::CheckSpawnRules(level, pos, ctx.rng) && CheckMobSpawnRules(ctx, pos);

            // The Aether (AetherEntityTypes.registerSpawnPlacements).
            case EntityTypeId::Phyg:
            case EntityTypeId::FlyingCow:
            case EntityTypeId::Sheepuff:
                return CheckAetherAnimalSpawnRules(ctx, pos);
            case EntityTypeId::Cockatrice:
                return CheckCockatriceSpawnRules(ctx, pos);
            case EntityTypeId::Zephyr:
                return CheckZephyrSpawnRules(ctx, pos);
            case EntityTypeId::Moa:
            case EntityTypeId::Aerbunny:
                return CheckAetherAnimalSpawnRules(ctx, pos);
            case EntityTypeId::Aerwhale:
                return CheckAerwhaleSpawnRules(ctx, pos);
            case EntityTypeId::BlueSwet:
            case EntityTypeId::GoldenSwet:
                return CheckSwetSpawnRules(ctx, pos);
            case EntityTypeId::Whirlwind:
            case EntityTypeId::EvilWhirlwind:
                return CheckWhirlwindSpawnRules(ctx, pos);
            case EntityTypeId::AechorPlant:
                return CheckAechorPlantSpawnRules(ctx, pos);
            // The Aether's dungeon mobs have no spawn placement: they are
            // placed by their dungeons (and spawn eggs / commands, which do
            // not ask this).
            case EntityTypeId::Mimic:
            case EntityTypeId::Sentry:
            case EntityTypeId::Valkyrie:
            case EntityTypeId::FireMinion:
                return false;
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
