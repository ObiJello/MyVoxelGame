#pragma once

#include "levelgen/density/terrain/TerrainSettings.h"

#include <memory>

// The engine's own dimensions' noise settings on the 26.3 density engine,
// built in code (their mods' datapacks are written for pre-26.3 Minecraft,
// whose density-function JSON format 26.3 no longer reads).
//
// Two rules carry the pre-26.3 semantics over:
//   - Before 26.3, NoiseChunk added the beardifier to every router's final
//     density itself (add(finalDensity, beardifier), cache_all_in_cell); 26.3
//     routers name it explicitly. The mod routers get it appended.
//   - flat_cache / cache_2d / cache_once / cache_all_in_cell are all 26.3's
//     plain cache; interpolated takes the noise settings' cell size
//     (size_horizontal * 4, size_vertical * 4).

namespace minecraft {
namespace world {
namespace biome {
class TwilightBiomeLayout;
}
} // namespace world

namespace levelgen {

class ModTerrainSettings {
public:
    // The Aether: aether:skylands (AetherNoiseBuilders / skylands.json).
    static std::shared_ptr<const density::TerrainSettings> aether();
    // The Twilight Forest: twilightforest:twilight_noise_gen.
    static std::shared_ptr<const density::TerrainSettings> twilight(
        std::shared_ptr<const world::biome::TwilightBiomeLayout> layout);
    // The Hush: the Overworld's router and aquifers under hushstone, sea
    // level 50, its own material rules, no spawn target.
    static std::shared_ptr<const density::TerrainSettings> hush();
};

} // namespace levelgen
} // namespace minecraft
