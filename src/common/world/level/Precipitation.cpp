// File: src/common/world/level/Precipitation.cpp
//
// MC ServerLevel.tickPrecipitation + Biome.shouldFreeze / shouldSnow. See
// Precipitation.hpp.
#include "common/world/level/Precipitation.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/SnowLayerBlock.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/lighting/ChunkLight.hpp"   // Lighting::LightLayer

#include <algorithm>
#include <vector>

namespace Game::Precipitation {

    namespace {

        // MC LevelReader.isWaterAt: getFluidState(pos).is(FluidTags.WATER) —
        // any water, source or flowing, free or waterlogged.
        bool IsWaterAt(const World& world, int x, int y, int z) {
            return FluidStateOf(world.GetBlockState(x, y, z)).Is(FluidType::Water);
        }

        // MC Block.pushEntitiesUp(state, newState, level, pos): the collision
        // shape newState ADDS over state (joinUnoptimized ONLY_SECOND), and
        // every entity overlapping it teleported up by 1 + collide(Y, box
        // raised by 1, that shape, -1) — which lands its feet on the new top
        // when it was inside, and leaves it where it was otherwise. For a snow
        // layer both shapes are full-footprint boxes, so the added part is the
        // slab between the old and the new collision top.
        //
        // Reaches the level's Game::Entity mobs. Players are not entities of
        // the server level here; their movement is the client's, whose own
        // collision resolve lifts them out of the thicker pile.
        void PushEntitiesUp(World& world, BlockState state, BlockState newState, const glm::ivec3& pos) {
            const BlockRegistry::BlockShape* oldBox = SnowLayer::CollisionBox(state);
            const BlockRegistry::BlockShape* newBox = SnowLayer::CollisionBox(newState);
            if (!newBox) return;
            const float oldTop = oldBox ? oldBox->max.y : 0.0f;
            const float newTop = newBox->max.y;
            if (newTop <= oldTop) return;   // offsetShape.isEmpty()

            EntityLevel* entities = world.Entities();
            if (!entities) return;
            const glm::vec3 lo(static_cast<float>(pos.x), static_cast<float>(pos.y) + oldTop,
                               static_cast<float>(pos.z));
            const glm::vec3 hi(static_cast<float>(pos.x) + 1.0f, static_cast<float>(pos.y) + newTop,
                               static_cast<float>(pos.z) + 1.0f);
            std::vector<Entity*> colliding;
            entities->GetEntitiesInBox(AABB::FromMinMax(lo, hi), nullptr, colliding);
            const double top = static_cast<double>(pos.y) + static_cast<double>(newTop);
            for (Entity* entity : colliding) {
                if (!entity || entity->IsRemoved()) continue;
                // 1 + max(-1, top - (minY + 1)) == max(0, top - minY): up to
                // the new surface, never down.
                const double lift = std::max(0.0, top - entity->position.y);
                if (lift > 0.0) entity->position.y += lift;
            }
        }

    } // namespace

    bool ShouldFreeze(const World& world, BiomeId biome, const glm::ivec3& pos, int seaLevel) {
        if (BiomeRegistry::WarmEnoughToRain(biome, pos.x, pos.y, pos.z, seaLevel)) return false;
        if (!world.IsValidPosition(pos.x, pos.y, pos.z)) return false;
        if (world.GetBrightness(Lighting::LightLayer::Block, pos.x, pos.y, pos.z) >= 10) return false;

        // `fluidState.is(Fluids.WATER) && blockState.getBlock() instanceof
        // LiquidBlock`: Fluids.WATER is the SOURCE fluid (flowing water is
        // FLOWING_WATER), and the block must be the water block itself — a
        // waterlogged stair never freezes.
        const BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);
        if (!state.Is(BlockID::Water) || !FluidStateOf(state).IsSourceOf(FluidType::Water)) return false;

        const bool surroundedByWater = IsWaterAt(world, pos.x - 1, pos.y, pos.z) &&
                                       IsWaterAt(world, pos.x + 1, pos.y, pos.z) &&
                                       IsWaterAt(world, pos.x, pos.y, pos.z - 1) &&
                                       IsWaterAt(world, pos.x, pos.y, pos.z + 1);
        return !surroundedByWater;
    }

    bool ShouldSnow(const World& world, BiomeId biome, const glm::ivec3& pos, int seaLevel) {
        if (BiomeRegistry::PrecipitationAt(biome, pos.x, pos.y, pos.z, seaLevel) !=
            BiomeRegistry::Precipitation::Snow) {
            return false;
        }
        if (!world.IsValidPosition(pos.x, pos.y, pos.z)) return false;
        if (world.GetBrightness(Lighting::LightLayer::Block, pos.x, pos.y, pos.z) >= 10) return false;
        const BlockID here = world.GetBlock(pos.x, pos.y, pos.z);
        if (here != BlockID::Air && here != BlockID::SnowLayer) return false;
        // Blocks.SNOW.defaultBlockState().canSurvive(level, pos).
        return SnowLayer::CanSurvive(world, pos);
    }

    void TickPrecipitation(World& world, const glm::ivec3& pos) {
        const int seaLevel = DimensionSeaLevel(world.GetDimension());

        // getHeightmapPos(MOTION_BLOCKING, pos): the first cell above the
        // column's top motion-blocking block (or fluid).
        const glm::ivec3 topPos(pos.x, world.GetSurfaceHeight(pos.x, pos.z, HeightmapType::MotionBlocking) + 1,
                                pos.z);
        const glm::ivec3 belowPos(topPos.x, topPos.y - 1, topPos.z);
        const BiomeId biome = world.GetBiome(topPos.x, topPos.y, topPos.z);

        if (ShouldFreeze(world, biome, belowPos, seaLevel)) {
            // setBlockAndUpdate: flag 3.
            world.SetBlock(belowPos.x, belowPos.y, belowPos.z,
                           BlockStates::Default(BlockID::Ice), World::UpdateFlags::All);
        }

        if (!world.IsRaining()) return;

        const int maxHeight = Rules::GetInt(Rules::Id::MaxSnowAccumulationHeight);
        if (maxHeight > 0 && ShouldSnow(world, biome, topPos, seaLevel)) {
            const BlockState state = world.GetBlockState(topPos.x, topPos.y, topPos.z);
            if (state.Is(BlockID::SnowLayer)) {
                const int currentLayers = SnowLayer::Layers(state);
                if (currentLayers < std::min(maxHeight, SnowLayer::kMaxHeight)) {
                    // state.setValue(LAYERS, currentLayers + 1).
                    const BlockState newState = SnowLayer::GrownState(state);
                    PushEntitiesUp(world, state, newState, topPos);
                    world.SetBlock(topPos.x, topPos.y, topPos.z, newState, World::UpdateFlags::All);
                }
            } else {
                world.SetBlock(topPos.x, topPos.y, topPos.z,
                               BlockStates::Default(BlockID::SnowLayer), World::UpdateFlags::All);
            }
        }

        // MC then hands the precipitation to the block below:
        //   Biome.Precipitation precipitation = biome.getPrecipitationAt(belowPos, seaLevel);
        //   if (precipitation != NONE) belowState.getBlock()
        //       .handlePrecipitation(belowState, level, belowPos, precipitation);
        const BiomeRegistry::Precipitation precipitation =
            BiomeRegistry::PrecipitationAt(biome, belowPos.x, belowPos.y, belowPos.z, seaLevel);
        if (precipitation != BiomeRegistry::Precipitation::None) {
            HandlePrecipitation(world, world.GetBlockState(belowPos.x, belowPos.y, belowPos.z), belowPos,
                                precipitation);
        }
    }

    void HandlePrecipitation(World& world, BlockState state, const glm::ivec3& pos,
                             BiomeRegistry::Precipitation precipitation) {
        // Block.handlePrecipitation is empty but for the cauldrons.
        // CauldronBlock.shouldHandlePrecipitation: rain fills 1 time in 20
        // (RAIN_FILL_CHANCE 0.05), snow 1 in 10 (POWDER_SNOW_FILL_CHANCE 0.1)
        // — the roll on the level's random.
        const auto shouldHandle = [&]() {
            JavaRandom* random = world.Random();
            if (!random) return false;
            if (precipitation == BiomeRegistry::Precipitation::Rain) return random->NextFloat() < 0.05f;
            if (precipitation == BiomeRegistry::Precipitation::Snow) return random->NextFloat() < 0.1f;
            return false;
        };

        if (state.Is(BlockID::Cauldron)) {
            // CauldronBlock.handlePrecipitation: an empty cauldron becomes a
            // level-1 water or powder snow cauldron.
            if (!shouldHandle()) return;
            const BlockID filled = precipitation == BiomeRegistry::Precipitation::Rain ? BlockID::WaterCauldron
                                                                                        : BlockID::PowderSnowCauldron;
            const BlockState newState = BlockStates::Default(filled).SetIndex(PropertyId::LEVEL_CAULDRON, 0);
            world.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
            // level.gameEvent(null, BLOCK_CHANGE, pos).
            world.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(static_cast<Entity*>(nullptr)));
            return;
        }

        if (state.Is(BlockID::WaterCauldron) || state.Is(BlockID::PowderSnowCauldron)) {
            // LayeredCauldronBlock.handlePrecipitation: its own kind of
            // precipitation (water: RAIN, powder snow: SNOW) raises a
            // non-full cauldron a level.
            const BiomeRegistry::Precipitation own = state.Is(BlockID::WaterCauldron)
                ? BiomeRegistry::Precipitation::Rain : BiomeRegistry::Precipitation::Snow;
            if (!shouldHandle()) return;
            const int levelIndex = state.GetIndex(PropertyId::LEVEL_CAULDRON);   // level 1..3 = 0..2
            if (levelIndex == 2 || precipitation != own) return;
            // state.cycle(LEVEL).
            const BlockState newState = state.SetIndex(PropertyId::LEVEL_CAULDRON, (levelIndex + 1) % 3);
            world.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
            world.GameEvent(GameEventId::BlockChange, pos, GameEventContext::Of(newState));
        }
    }

} // namespace Game::Precipitation
