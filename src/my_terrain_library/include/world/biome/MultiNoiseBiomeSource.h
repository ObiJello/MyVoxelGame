#pragma once

#include "world/biome/BiomeSource.h"
#include "world/biome/Climate.h"
#include "world/biome/Biomes.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include <vector>
#include <utility>
#include <memory>
#include <string>

// Reference: net/minecraft/world/level/biome/MultiNoiseBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

/**
 * MultiNoiseBiomeSource - Biome source using multi-noise climate parameters
 * Reference: net/minecraft/world/level/biome/MultiNoiseBiomeSource.java
 *
 * This is the main biome source used for:
 * - Overworld (with OverworldBiomeBuilder parameters)
 * - Nether (with NetherBiomeBuilder parameters)
 *
 * It uses 6 climate parameters (temperature, humidity, continentalness,
 * erosion, depth, weirdness) to select biomes via an R-tree search.
 */
class MultiNoiseBiomeSource : public BiomeSource {
public:
    // Preset identifiers matching Java
    enum class Preset {
        OVERWORLD,
        NETHER,
        HUSH,    // engine-only: The Hush (DimensionId::Hush)
        AETHER   // The Aether (DimensionId::Aether), dimension/the_aether.json
    };

private:
    // The parameter list with RTree index for fast biome lookup
    std::unique_ptr<Climate::ParameterList<BiomeKey>> m_parameters;

    // Cached builder for spawn target
    std::unique_ptr<OverworldBiomeBuilder> m_overworldBuilder;

    // Which preset this source was created from (if any)
    Preset m_preset;

public:
    /**
     * Create a MultiNoiseBiomeSource from a list of biome parameters
     * Reference: MultiNoiseBiomeSource.java lines 22-25
     */
    explicit MultiNoiseBiomeSource(
        const std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>& parameters
    );

    /**
     * Create a MultiNoiseBiomeSource from a preset
     * Reference: MultiNoiseBiomeSource.java Preset class lines 43-91
     *
     * @param preset The preset type (OVERWORLD or NETHER)
     */
    explicit MultiNoiseBiomeSource(Preset preset);

    /**
     * Create the default Overworld biome source
     * Reference: MultiNoiseBiomeSource.Preset.OVERWORLD
     */
    static std::unique_ptr<MultiNoiseBiomeSource> createOverworld();

    /**
     * Create the default Nether biome source
     * Reference: MultiNoiseBiomeSource.Preset.NETHER
     */
    static std::unique_ptr<MultiNoiseBiomeSource> createNether();

    /**
     * Create The Hush biome source (engine-only dimension; no vanilla
     * counterpart). A partition of climate ranges over the Overworld router,
     * see buildHushParameters(), sampled through sampleHushClimate().
     */
    static std::unique_ptr<MultiNoiseBiomeSource> createHush();

    /**
     * The Hush's climate target at a quart position: the sampler's six
     * values, except that temperature is read at 3x and humidity at 1.5x the
     * block x/z (kHushTemperatureScale / kHushHumidityScale, as num/den
     * integers so the scaled coordinate is exact). The Overworld router's
     * temperature and vegetation noises feed ONLY biome choice (final_density
     * never reads them), so this shrinks the temperature- and humidity-driven
     * Hush biomes without touching terrain; continentalness, erosion, depth
     * and weirdness — which shape terrain — stay at the real position.
     * Measurements in buildHushParameters().
     */
    static Climate::TargetPoint sampleHushClimate(const Climate::Sampler& sampler,
                                                  int32_t quartX, int32_t quartY, int32_t quartZ);
    static constexpr int32_t kHushTemperatureScaleNum = 3, kHushTemperatureScaleDen = 1;
    static constexpr int32_t kHushHumidityScaleNum = 3, kHushHumidityScaleDen = 2;

    /**
     * Create The Aether biome source: the multi_noise source of
     * data/aether/dimension/the_aether.json, see buildAetherParameters().
     */
    static std::unique_ptr<MultiNoiseBiomeSource> createAether();

    /**
     * Get the biome at a given quart position
     * Reference: MultiNoiseBiomeSource.java getNoiseBiome() lines 27-30
     *
     * CRITICAL: This is the main biome selection method.
     * 1. Sample climate parameters from the sampler
     * 2. Use RTree to find best matching biome
     */
    BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                           const Climate::Sampler& sampler) override;

    // Reference: 26.3 MultiNoiseBiomeSource.createResolverForChunk - the six
    // climate volumes sampled up front over the quart box, then looked up per
    // cell. The Hush samples temperature and humidity at its own scale, so it
    // stays on the per-cell path.
    BiomeResolver createResolverForChunk(const Climate::Sampler& sampler, int32_t minQuartX, int32_t minQuartY,
                                         int32_t minQuartZ, int32_t quartSizeX, int32_t quartSizeY,
                                         int32_t quartSizeZ) override;

    /**
     * Get spawn target parameters for world spawn selection
     * Reference: MultiNoiseBiomeSource.java getSpawnTarget() line 36
     */
    std::vector<Climate::ParameterPoint> getSpawnTarget() const override;

    /**
     * Add debug info about biome selection
     * Reference: MultiNoiseBiomeSource.java addDebugInfo() lines 32-34
     */
    void addDebugInfo(std::vector<std::string>& info,
                     int32_t x, int32_t y, int32_t z,
                     const Climate::Sampler& sampler) const override;

    /**
     * Get the raw parameter list
     */
    const Climate::ParameterList<BiomeKey>& parameters() const {
        return *m_parameters;
    }

private:
    /**
     * Build overworld biome parameters
     */
    static std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> buildOverworldParameters();

    /**
     * Build nether biome parameters
     * Reference: NetherBiomeBuilder
     */
    static std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> buildNetherParameters();

    /**
     * Build The Hush biome parameters (engine-only). Entry order MUST equal
     * BiomeFeatureRegistry::getHushBiomeKeys() — it is the possibleBiomes()
     * order that seeds every Hush feature.
     */
    static std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> buildHushParameters();

    /**
     * Build The Aether biome parameters — the 14 points of the_aether.json in
     * file order. Every point's first appearance fixes possibleBiomes(), which
     * MUST equal BiomeFeatureRegistry::getAetherBiomeKeys() (it seeds every
     * Aether feature).
     */
    static std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> buildAetherParameters();
};

} // namespace biome
} // namespace world
} // namespace minecraft
