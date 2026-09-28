// File: src/common/world/block/FireBlock.cpp
//
// MC FireBlock / BaseFireBlock / SoulFireBlock — see FireBlock.hpp.
#include "common/world/block/FireBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/block/TntBlock.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace Game {

    namespace {

        constexpr int kMaxAge = 15;

        // FireBlock.igniteOdds / burnOdds, by block id — built once from the
        // generated bootStrap table.
        struct Flammability {
            std::array<uint8_t, BlockRegistry::Size> ignite{};
            std::array<uint8_t, BlockRegistry::Size> burn{};
            Flammability() {
                struct Row { std::string_view slug; int ignite; int burn; };
                static constexpr Row kRows[] = {
#define FIRE_FLAMMABLE(slug, igniteOdds, burnOdds) Row{ slug, igniteOdds, burnOdds },
#include "GeneratedFireFlammability.inc"
#undef FIRE_FLAMMABLE
                };
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    const std::string_view slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    // Later rows win, as a later setFlammable put() does.
                    for (const Row& row : kRows) {
                        if (row.slug != slug) continue;
                        ignite[i] = static_cast<uint8_t>(row.ignite);
                        burn[i]   = static_cast<uint8_t>(row.burn);
                    }
                }
            }
        };
        const Flammability& Table() {
            static const Flammability table;
            return table;
        }

        bool IsWaterlogged(BlockState state) {
            return state.HasProperty(PropertyId::WATERLOGGED) && state.GetIndex(PropertyId::WATERLOGGED) == 0;
        }

        glm::ivec3 Step(const glm::ivec3& pos, Direction d) {
            return pos + glm::ivec3(StepX(d), StepY(d), StepZ(d));
        }

        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        constexpr Direction kAllDirections[] = {Direction::Down, Direction::Up, Direction::North,
                                                Direction::South, Direction::West, Direction::East};

        // MC FireBlock.isValidFireLocation: some neighbour can burn.
        bool IsValidFireLocation(const IBlockAccess& level, const glm::ivec3& pos) {
            for (Direction d : kAllDirections) {
                if (FireCanBurn(StateAt(level, Step(pos, d)))) return true;
            }
            return false;
        }

        // MC FireBlock.getIgniteOdds(level, pos): an EMPTY cell takes the
        // strongest ignite odds of its six neighbours.
        int IgniteOddsAt(const IBlockAccess& level, const glm::ivec3& pos) {
            if (level.GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return 0;
            int odds = 0;
            for (Direction d : kAllDirections) odds = std::max(FireIgniteOdds(StateAt(level, Step(pos, d))), odds);
            return odds;
        }

        // The dimension type's infiniburn tag.
        bool IsInfiniburn(DimensionId dimension, BlockID below) {
            const char* tag = dimension == DimensionId::Nether ? "minecraft:infiniburn_nether"
                            : dimension == DimensionId::End    ? "minecraft:infiniburn_end"
                                                               : "minecraft:infiniburn_overworld";
            return DataTags::HasTag(DataTags::Registry::Block, BlockRegistry::Get(below).registrySlug, tag);
        }

        // EnvironmentAttributes.INCREASED_FIRE_BURNOUT — the biomes whose
        // data sets gameplay/increased_fire_burnout (26.3 worldgen/biome).
        bool IncreasedFireBurnout(const IBlockAccess& level, const glm::ivec3& pos) {
            static const std::vector<BiomeId> kBiomes = [] {
                std::vector<BiomeId> ids;
                for (const char* name : {"jungle", "bamboo_jungle", "swamp", "mangrove_swamp", "mushroom_fields",
                                         "snowy_slopes", "jagged_peaks", "frozen_peaks"}) {
                    ids.push_back(BiomeRegistry::FromName(name));
                }
                return ids;
            }();
            const BiomeId biome = static_cast<BiomeId>(level.GetBiome(pos.x, pos.y, pos.z));
            return std::find(kBiomes.begin(), kBiomes.end(), biome) != kBiomes.end();
        }

        // MC FireBlock.isNearRain: rain at the cell or one of its four
        // horizontal neighbours.
        bool IsNearRain(const World& world, const glm::ivec3& p) {
            return world.IsRainingAt(p.x, p.y, p.z) ||
                   world.IsRainingAt(p.x - 1, p.y, p.z) || world.IsRainingAt(p.x + 1, p.y, p.z) ||
                   world.IsRainingAt(p.x, p.y, p.z - 1) || world.IsRainingAt(p.x, p.y, p.z + 1);
        }

        // MC FireBlock.getStateWithAge: the placement state, aged when fire.
        BlockState StateWithAge(const IBlockAccess& level, const glm::ivec3& pos, int age) {
            const BlockState placement = FireStateForPlacement(level, pos);
            return placement.Is(BlockID::Fire) ? placement.SetIndex(PropertyId::AGE_15, age) : placement;
        }

        void RemoveBlock(ILevelWrite& level, const glm::ivec3& pos) {
            if (auto* world = dynamic_cast<World*>(&level)) {
                world->RemoveBlock(pos, false);
            } else {
                level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
            }
        }

        // MC FireBlock.checkBurnOut.
        void CheckBurnOut(ILevelWrite& level, const World* world, const glm::ivec3& pos, int chance,
                          JavaRandom& random, int age) {
            if (!level.IsPositionLoaded(pos.x, pos.y, pos.z)) return;
            const BlockState oldState = StateAt(level, pos);
            const int odds = FireBurnOdds(oldState);
            if (!(random.NextInt(chance) < odds)) return;
            if (random.NextInt(age + 10) < 5 && !(world && world->IsRainingAt(pos.x, pos.y, pos.z))) {
                const int newAge = std::min(age + random.NextInt(5) / 4, kMaxAge);
                level.SetBlock(pos.x, pos.y, pos.z, StateWithAge(level, pos, newAge), World::UpdateFlags::All);
            } else {
                RemoveBlock(level, pos);
            }
            if (oldState.Is(BlockID::Tnt)) TntPrime(level, pos, nullptr);
        }

        // MC FireBlock.tick.
        void FireTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& random) {
            FireScheduleTick(level, pos);
            World* world = dynamic_cast<World*>(&level);
            if (!world) return;
            if (EntityLevel* entities = world->Entities()) {
                if (!FireCanSpreadAround(*entities, pos)) return;
            }

            if (!FireBlockCanSurvive(level, pos, state)) RemoveBlock(level, pos);

            const BlockID below = level.GetBlock(pos.x, pos.y - 1, pos.z);
            const bool infiniBurn = IsInfiniburn(level.GetDimension(), below);
            const int age = std::max(0, state.GetIndex(PropertyId::AGE_15));
            if (!infiniBurn && world->IsRaining() && IsNearRain(*world, pos) &&
                random.NextFloat() < 0.2f + static_cast<float>(age) * 0.03f) {
                RemoveBlock(level, pos);
                return;
            }

            const int newAge = std::min(kMaxAge, age + random.NextInt(3) / 2);
            if (age != newAge) {
                state = state.SetIndex(PropertyId::AGE_15, newAge);
                // flag 260: UPDATE_INVISIBLE | UPDATE_SKIP_BLOCK_ENTITY_SIDE_EFFECTS.
                level.SetBlock(pos.x, pos.y, pos.z, state,
                               World::UpdateFlags::Invisible | World::UpdateFlags::SkipBlockEntitySideEffects);
            }

            if (!infiniBurn) {
                if (!IsValidFireLocation(level, pos)) {
                    const glm::ivec3 belowPos(pos.x, pos.y - 1, pos.z);
                    if (!IsFaceSturdyAt(level, belowPos, Direction::Up) || age > 3) RemoveBlock(level, pos);
                    return;
                }
                if (age == kMaxAge && random.NextInt(4) == 0 && !FireCanBurn(StateAt(level, {pos.x, pos.y - 1, pos.z}))) {
                    RemoveBlock(level, pos);
                    return;
                }
            }

            const bool increasedBurnout = IncreasedFireBurnout(level, pos);
            const int extra = increasedBurnout ? -50 : 0;
            CheckBurnOut(level, world, {pos.x + 1, pos.y, pos.z}, 300 + extra, random, age);
            CheckBurnOut(level, world, {pos.x - 1, pos.y, pos.z}, 300 + extra, random, age);
            CheckBurnOut(level, world, {pos.x, pos.y - 1, pos.z}, 250 + extra, random, age);
            CheckBurnOut(level, world, {pos.x, pos.y + 1, pos.z}, 250 + extra, random, age);
            CheckBurnOut(level, world, {pos.x, pos.y, pos.z - 1}, 300 + extra, random, age);
            CheckBurnOut(level, world, {pos.x, pos.y, pos.z + 1}, 300 + extra, random, age);

            const int difficultyId = static_cast<int>(world->GetDifficulty());
            for (int xx = -1; xx <= 1; ++xx) {
                for (int zz = -1; zz <= 1; ++zz) {
                    for (int yy = -1; yy <= 4; ++yy) {
                        if (xx == 0 && yy == 0 && zz == 0) continue;
                        int rate = 100;
                        if (yy > 1) rate += (yy - 1) * 100;
                        const glm::ivec3 testPos(pos.x + xx, pos.y + yy, pos.z + zz);
                        if (!level.IsPositionLoaded(testPos.x, testPos.y, testPos.z)) continue;
                        const int igniteOdds = IgniteOddsAt(level, testPos);
                        if (igniteOdds <= 0) continue;
                        int odds = (igniteOdds + 40 + difficultyId * 7) / (age + 30);
                        if (increasedBurnout) odds /= 2;
                        if (odds > 0 && random.NextInt(rate) <= odds &&
                            (!world->IsRaining() || !IsNearRain(*world, testPos))) {
                            const int spreadAge = std::min(kMaxAge, age + random.NextInt(5) / 4);
                            level.SetBlock(testPos.x, testPos.y, testPos.z, StateWithAge(level, testPos, spreadAge),
                                           World::UpdateFlags::All);
                        }
                    }
                }
            }
        }

        // MC FireBlock.updateShape: canSurvive ? getStateWithAge(age) : AIR.
        bool FireUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state, Direction,
                             BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (FireBlockCanSurvive(level, pos, state)) {
                outState = StateWithAge(level, pos, std::max(0, state.GetIndex(PropertyId::AGE_15)));
            } else {
                outState = BlockStates::Default(BlockID::Air);
            }
            return outState.RawId() != state.RawId();
        }

        // MC SoulFireBlock.updateShape: canSurvive ? this : AIR.
        bool SoulFireUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state, Direction,
                                 BlockID, BlockState& outState, ScheduledTickAccess*) {
            if (FireBlockCanSurvive(level, pos, state)) return false;
            outState = BlockStates::Default(BlockID::Air);
            return true;
        }

    } // namespace

    int FireIgniteOdds(BlockState state) {
        if (IsWaterlogged(state)) return 0;
        return Table().ignite[static_cast<size_t>(state.Block())];
    }

    int FireBurnOdds(BlockState state) {
        if (IsWaterlogged(state)) return 0;
        return Table().burn[static_cast<size_t>(state.Block())];
    }

    bool FireCanBurn(BlockState state) { return FireIgniteOdds(state) > 0; }

    BlockState FireStateForPlacement(const IBlockAccess& level, const glm::ivec3& pos) {
        const glm::ivec3 below(pos.x, pos.y - 1, pos.z);
        const BlockState belowState = StateAt(level, below);
        // BaseFireBlock.getState: SoulFireBlock.canSurviveOnBlock.
        if (IsSoulFireBaseBlock(belowState.Block())) return BlockStates::Default(BlockID::SoulFire);
        BlockState result = BlockStates::Default(BlockID::Fire);
        if (FireCanBurn(belowState) || IsFaceSturdyAt(level, below, Direction::Up)) return result;
        // Clinging: each side (and the top) that can burn.
        const struct { Direction d; PropertyId p; } kSides[] = {
            {Direction::North, PropertyId::NORTH}, {Direction::East, PropertyId::EAST},
            {Direction::South, PropertyId::SOUTH}, {Direction::West, PropertyId::WEST},
            {Direction::Up, PropertyId::UP},
        };
        for (const auto& side : kSides) {
            if (!result.HasProperty(side.p)) continue;
            // Boolean properties: index 0 == true.
            result = result.SetIndex(side.p, FireCanBurn(StateAt(level, Step(pos, side.d))) ? 0 : 1);
        }
        return result;
    }

    bool FireBlockCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        const glm::ivec3 below(pos.x, pos.y - 1, pos.z);
        if (state.Is(BlockID::SoulFire)) {
            // SoulFireBlock.canSurvive → canSurviveOnBlock(below).
            return IsSoulFireBaseBlock(level.GetBlock(below.x, below.y, below.z));
        }
        return IsFaceSturdyAt(level, below, Direction::Up) || IsValidFireLocation(level, pos);
    }

    bool FireCanSpreadAround(const EntityLevel& level, const glm::ivec3& pos) {
        // MC ServerLevel.canSpreadFireAround: fire_spread_radius_around_player
        // -1 = everywhere; else a non-spectator player within that many
        // blocks of the cell's centre (ChunkMap.anyPlayerCloseEnoughTo).
        const int spreadRadius = Rules::GetInt(Rules::Id::FireSpreadRadiusAroundPlayer);
        if (spreadRadius == -1) return true;
        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        const glm::dvec3 center = glm::dvec3(pos) + glm::dvec3(0.5);
        const double radiusSq = static_cast<double>(spreadRadius) * spreadRadius;
        for (const LivingEntity* player : players) {
            if (!player || player->IsSpectator()) continue;
            const glm::dvec3 d = player->position - center;
            if (glm::dot(d, d) < radiusSq) return true;
        }
        return false;
    }

    void FireScheduleTick(ILevelWrite& level, const glm::ivec3& pos) {
        if (level.IsClientSide()) return;
        ScheduledTickAccess* ticks = level.Ticks();
        JavaRandom* random = level.Random();
        if (!ticks || !random) return;
        // getFireTickDelay: 30 + nextInt(10).
        ticks->ScheduleTick(pos, BlockID::Fire, 30 + random->NextInt(10));
    }

    void RegisterFireBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& fire = blocks[static_cast<size_t>(BlockID::Fire)];
        fire.tick        = &FireTick;
        fire.updateShape = &FireUpdateShape;
        Block& soulFire = blocks[static_cast<size_t>(BlockID::SoulFire)];
        soulFire.updateShape = &SoulFireUpdateShape;
    }

} // namespace Game
