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
        NETHER
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
     * Get the biome at a given quart position
     * Reference: MultiNoiseBiomeSource.java getNoiseBiome() lines 27-30
     *
     * CRITICAL: This is the main biome selection method.
     * 1. Sample climate parameters from the sampler
     * 2. Use RTree to find best matching biome
     */
    BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                           const Climate::Sampler& sampler) override;

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
};

} // namespace biome
} // namespace world
} // namespace minecraft
