// File: src/common/world/biome/BiomeAttributes.hpp
//
// MC 26.x EnvironmentAttributes for the gameplay questions the server asks
// of a place: the dimension type's "attributes" layer, overridden by the
// biome's ("attributes" in data/<ns>/worldgen/biome/<path>.json — vanilla's
// and the mod biomes' own datapack JSON), read once per biome and cached.
// Only constant (non-timeline) boolean attributes are answered here; the
// client's visual attributes live in EnvironmentState.
#pragma once

#include "common/world/biome/Biomes.hpp"
#include "common/world/level/DimensionId.hpp"

#include <optional>
#include <string_view>

namespace Game::BiomeAttributes {

    // The biome's own value for a boolean attribute ("minecraft:gameplay/
    // snow_golem_melts"), when its JSON sets one. Thread-safe.
    std::optional<bool> GetBool(BiomeId biome, std::string_view attribute);

    // MC EnvironmentAttributes.SNOW_GOLEM_MELTS at a position in `dimension`
    // whose biome is `biome`: the biome's value, else the dimension type's
    // (the_nether sets it), else the attribute's default, false.
    bool SnowGolemMelts(DimensionId dimension, BiomeId biome);

} // namespace Game::BiomeAttributes
