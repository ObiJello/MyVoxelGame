// File: src/common/world/level/Precipitation.hpp
//
// MC ServerLevel.tickPrecipitation and the Biome rules it asks
// (shouldFreeze, shouldSnow) — the "iceandsnow" half of ServerLevel.tickChunk.
// World::PerformRandomBlockTick rolls MC's 1-in-48 per random-tick sample and
// calls TickPrecipitation on a random column of each ticking chunk.
//
// Two halves with different gates, exactly as vanilla:
//   * freezing — an exposed water source in a cold biome becomes ice. Runs
//     whatever the weather: it is why a lake in a snowy plains ices over.
//   * snowfall — while it is raining (World::IsRaining), snow layers form and
//     pile up to the snowAccumulationHeight game rule (default 1, at most 8),
//     pushing mobs standing in them up; and the block under the column's top
//     gets the precipitation (HandlePrecipitation — the cauldrons fill).
#pragma once

#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    class World;

    namespace Precipitation {

        // MC Biome.shouldFreeze(level, pos, checkNeighbors = true): not warm
        // enough to rain, inside the build height, block light below 10, a
        // water SOURCE in a water block (not a waterlogged one), and — the
        // neighbour check — not surrounded on all four sides by water, so
        // ice grows in from the shore rather than appearing mid-lake.
        bool ShouldFreeze(const World& world, BiomeId biome, const glm::ivec3& pos, int seaLevel);

        // MC Biome.shouldSnow(level, pos): the biome's precipitation there is
        // SNOW, inside the build height, block light below 10, and the cell is
        // air or a snow layer where a snow layer could stand.
        bool ShouldSnow(const World& world, BiomeId biome, const glm::ivec3& pos, int seaLevel);

        // MC ServerLevel.tickPrecipitation(pos): `pos` is any cell of the
        // column (getBlockRandomPos(minX, 0, minZ, 15)); the work happens at
        // the column's MOTION_BLOCKING surface.
        void TickPrecipitation(World& world, const glm::ivec3& pos);

        // MC Block.handlePrecipitation(state, level, pos, precipitation) —
        // what the precipitation does to the block under the column's top:
        // the cauldrons fill (an empty one 1 time in 20 with rain / 1 in 10
        // with snow; a water or powder snow cauldron gains a level from its
        // own kind). Everything else ignores it.
        void HandlePrecipitation(World& world, BlockState state, const glm::ivec3& pos,
                                 BiomeRegistry::Precipitation precipitation);

    } // namespace Precipitation

} // namespace Game
