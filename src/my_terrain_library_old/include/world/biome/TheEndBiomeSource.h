#pragma once

#include "world/biome/BiomeSource.h"
#include "world/biome/Climate.h"
#include "world/biome/Biomes.h"
#include "synth/SimplexNoise.h"
#include <memory>

// Reference: net/minecraft/world/level/biome/TheEndBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

/**
 * TheEndBiomeSource - Biome source for The End dimension
 * Reference: net/minecraft/world/level/biome/TheEndBiomeSource.java
 *
 * The End uses a special island-based algorithm rather than climate parameters.
 * It generates:
 * - The main island at the center
 * - Outer islands in a ring pattern
 * - Different biomes based on island characteristics
 */
class TheEndBiomeSource : public BiomeSource {
public:
    // Seed used for island noise generation
    int64_t m_seed;

    // Simplex noise for island generation
    std::unique_ptr<synth::SimplexNoise> m_islandNoise;

public:
    /**
     * Create an End biome source
     * Reference: TheEndBiomeSource.java constructor
     *
     * @param seed The world seed used for island generation
     */
    explicit TheEndBiomeSource(int64_t seed);

    /**
     * Get the biome at a given quart position
     * Reference: TheEndBiomeSource.java getNoiseBiome() lines 30-48
     *
     * The algorithm:
     * 1. Check if we're in the main island (distance < 64 blocks from origin)
     * 2. If in outer islands, use noise to determine island height
     * 3. Select biome based on island characteristics:
     *    - THE_END: Main island and central parts of outer islands
     *    - END_HIGHLANDS: High elevated outer islands
     *    - END_MIDLANDS: Medium elevation areas
     *    - END_BARRENS: Low areas (exposed bedrock)
     *    - SMALL_END_ISLANDS: Very small scattered islands
     */
    BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                           const Climate::Sampler& sampler) override;

    /**
     * Get island noise value at a position
     * Reference: TheEndBiomeSource.java getHeightValue() lines 50-71
     *
     * This is the core End terrain generation algorithm.
     * Used for both biome selection and terrain height.
     *
     * @param x X coordinate (block scale, not quart)
     * @param z Z coordinate (block scale, not quart)
     * @return Noise value for island height (-100 to ~100+)
     */
    float getHeightValue(int32_t x, int32_t z) const;

    /**
     * Static version using shared seed - matches Java signature
     * Reference: TheEndBiomeSource.java getHeightValue(SimplexNoise, int, int) lines 50-71
     */
    static float getHeightValue(const synth::SimplexNoise& noise, int32_t x, int32_t z);

private:
    /**
     * Initialize the biome set
     */
    void initBiomes();
};

} // namespace biome
} // namespace world
} // namespace minecraft
