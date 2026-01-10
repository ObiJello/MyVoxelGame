#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/biome/Biomes.h"
#include "world/biome/Biome.h"

// Reference: net/minecraft/world/level/biome/MultiNoiseBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

// Constructor from parameter list
MultiNoiseBiomeSource::MultiNoiseBiomeSource(
    const std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>& parameters
) : m_preset(Preset::OVERWORLD) {
    m_parameters = std::make_unique<Climate::ParameterList<BiomeKey>>(parameters);
    collectBiomes(parameters);
}

// Constructor from preset
MultiNoiseBiomeSource::MultiNoiseBiomeSource(Preset preset)
    : m_preset(preset) {

    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> params;

    switch (preset) {
        case Preset::OVERWORLD:
            params = buildOverworldParameters();
            m_overworldBuilder = std::make_unique<OverworldBiomeBuilder>();
            break;
        case Preset::NETHER:
            params = buildNetherParameters();
            break;
    }

    m_parameters = std::make_unique<Climate::ParameterList<BiomeKey>>(params);
    collectBiomes(params);
}

// Factory for overworld
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createOverworld() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::OVERWORLD);
}

// Factory for nether
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createNether() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::NETHER);
}

// Main biome selection method
BiomeKey MultiNoiseBiomeSource::getNoiseBiome(
    int32_t quartX, int32_t quartY, int32_t quartZ,
    const Climate::Sampler& sampler
) {
    // Reference: MultiNoiseBiomeSource.java lines 27-30
    // return this.parameters.findValue(sampler.sample(quartX, quartY, quartZ));
    Climate::TargetPoint target = sampler.sample(quartX, quartY, quartZ);
    return m_parameters->findValue(target);
}

// Spawn target for world spawn position
std::vector<Climate::ParameterPoint> MultiNoiseBiomeSource::getSpawnTarget() const {
    // Reference: MultiNoiseBiomeSource.java line 36
    // return this.preset.map(preset -> preset.biomeSource().getSpawnTarget()).orElse(List.of());

    if (m_overworldBuilder) {
        return m_overworldBuilder->spawnTarget();
    }
    return {};
}

// Debug info
void MultiNoiseBiomeSource::addDebugInfo(
    std::vector<std::string>& info,
    int32_t x, int32_t y, int32_t z,
    const Climate::Sampler& sampler
) const {
    // Reference: MultiNoiseBiomeSource.java lines 32-34
    // Adds climate parameter info to debug display

    int32_t quartX = x >> 2;
    int32_t quartY = y >> 2;
    int32_t quartZ = z >> 2;

    Climate::TargetPoint target = sampler.sample(quartX, quartY, quartZ);

    float temp = Climate::unquantizeCoord(target.temperature);
    float humid = Climate::unquantizeCoord(target.humidity);
    float cont = Climate::unquantizeCoord(target.continentalness);
    float eros = Climate::unquantizeCoord(target.erosion);
    float depth = Climate::unquantizeCoord(target.depth);
    float weird = Climate::unquantizeCoord(target.weirdness);

    info.push_back("Biome Noise Parameters:");
    info.push_back("  Temperature: " + std::to_string(temp));
    info.push_back("  Humidity: " + std::to_string(humid));
    info.push_back("  Continentalness: " + std::to_string(cont));
    info.push_back("  Erosion: " + std::to_string(eros));
    info.push_back("  Depth: " + std::to_string(depth));
    info.push_back("  Weirdness: " + std::to_string(weird));
}

// Build overworld parameters using OverworldBiomeBuilder
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildOverworldParameters() {
    // Reference: MultiNoiseBiomeSource.Preset.OVERWORLD lines 50-54
    // OverworldBiomeBuilder builder = new OverworldBiomeBuilder();
    // builder.addBiomes(pair -> list.add(pair.mapSecond(biomes::getOrThrow)));

    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;

    OverworldBiomeBuilder builder;
    builder.addBiomes([&result](const std::pair<Climate::ParameterPoint, BiomeKey>& pair) {
        result.push_back(pair);
    });

    return result;
}

// Build nether parameters
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildNetherParameters() {
    // Reference: MultiNoiseBiomeSource.Preset.NETHER and NetherBiomeBuilder
    // The nether uses a simpler 2D climate system (temperature and humidity only)

    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;

    // Nether biome parameters - Reference: NetherBiomeBuilder
    // The nether ignores most climate parameters and just uses temperature/humidity

    Climate::Parameter fullRange = Climate::Parameter::span(-1.0f, 1.0f);
    Climate::Parameter zeroPoint = Climate::Parameter::point(0.0f);

    // Nether Wastes - default biome
    result.push_back({
        Climate::parameters(
            Climate::Parameter::span(0.0f, 1.0f),       // temperature (warm)
            Climate::Parameter::span(-1.0f, 1.0f),      // humidity (any)
            fullRange, fullRange, fullRange, fullRange,
            0.0f
        ),
        BiomeKeys::NETHER_WASTES
    });

    // Soul Sand Valley
    result.push_back({
        Climate::parameters(
            Climate::Parameter::span(-0.5f, 0.0f),      // temperature (cold)
            Climate::Parameter::span(-1.0f, -0.5f),     // humidity (dry)
            fullRange, fullRange, fullRange, fullRange,
            0.0f
        ),
        BiomeKeys::SOUL_SAND_VALLEY
    });

    // Crimson Forest
    result.push_back({
        Climate::parameters(
            Climate::Parameter::span(-0.15f, 0.5f),     // temperature
            Climate::Parameter::span(0.0f, 1.0f),       // humidity (humid)
            fullRange, fullRange, fullRange, fullRange,
            0.375f
        ),
        BiomeKeys::CRIMSON_FOREST
    });

    // Warped Forest
    result.push_back({
        Climate::parameters(
            Climate::Parameter::span(-1.0f, 0.0f),      // temperature (cold)
            Climate::Parameter::span(0.0f, 1.0f),       // humidity (humid)
            fullRange, fullRange, fullRange, fullRange,
            0.375f
        ),
        BiomeKeys::WARPED_FOREST
    });

    // Basalt Deltas
    result.push_back({
        Climate::parameters(
            Climate::Parameter::span(-0.5f, 0.5f),      // temperature
            Climate::Parameter::span(-1.0f, -0.35f),    // humidity (dry)
            fullRange, fullRange, fullRange, fullRange,
            0.175f
        ),
        BiomeKeys::BASALT_DELTAS
    });

    return result;
}

} // namespace biome
} // namespace world
} // namespace minecraft
