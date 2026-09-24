#include "levelgen/NoiseGeneratorSettings.h"

#include "levelgen/density/WorldgenRegistries.h"

namespace minecraft {
namespace levelgen {

std::shared_ptr<NoiseGeneratorSettings> NoiseGeneratorSettings::load(const std::string& key) {
    density::WorldgenRegistries& registries = density::WorldgenRegistries::get();
    return std::make_shared<NoiseGeneratorSettings>(density::TerrainSettings::load(key, registries));
}

} // namespace levelgen
} // namespace minecraft
