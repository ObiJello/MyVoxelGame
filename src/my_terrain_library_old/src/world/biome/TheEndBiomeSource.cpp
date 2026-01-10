#include "world/biome/TheEndBiomeSource.h"
#include "random/LegacyRandomSource.h"
#include <cmath>

// Reference: net/minecraft/world/level/biome/TheEndBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

TheEndBiomeSource::TheEndBiomeSource(int64_t seed)
    : m_seed(seed) {

    // Create simplex noise for island generation
    // Reference: TheEndBiomeSource.java line 28
    // this.islandNoise = SimplexNoise.createSeeded(new WorldgenRandom(new LegacyRandomSource(seed)));
    LegacyRandomSource legacyRandom(seed);
    m_islandNoise = std::make_unique<synth::SimplexNoise>(legacyRandom);

    initBiomes();
}

void TheEndBiomeSource::initBiomes() {
    // Add all End biomes
    m_possibleBiomes.insert(BiomeKeys::THE_END);
    m_possibleBiomes.insert(BiomeKeys::END_HIGHLANDS);
    m_possibleBiomes.insert(BiomeKeys::END_MIDLANDS);
    m_possibleBiomes.insert(BiomeKeys::END_BARRENS);
    m_possibleBiomes.insert(BiomeKeys::SMALL_END_ISLANDS);
}

BiomeKey TheEndBiomeSource::getNoiseBiome(
    int32_t quartX, int32_t quartY, int32_t quartZ,
    const Climate::Sampler& /* sampler */
) {
    // Reference: TheEndBiomeSource.java getNoiseBiome() lines 30-48

    // Convert quart coordinates to block coordinates for distance check
    int32_t blockX = quartX << 2;  // x * 4
    int32_t blockZ = quartZ << 2;  // z * 4

    // Check if we're in the main island area
    // Reference: line 32-33
    // if ((long)chunkX * (long)chunkX + (long)chunkZ * (long)chunkZ <= 4096L)
    //     return THE_END

    // Convert to chunk coordinates for distance check
    int32_t chunkX = blockX >> 4;  // floor divide by 16
    int32_t chunkZ = blockZ >> 4;

    int64_t distSq = static_cast<int64_t>(chunkX) * static_cast<int64_t>(chunkX)
                   + static_cast<int64_t>(chunkZ) * static_cast<int64_t>(chunkZ);

    if (distSq <= 4096L) {
        // We're in the main island
        return BiomeKeys::THE_END;
    }

    // Get height value at this position
    // Reference: line 36
    // float heightValue = this.getHeightValue(islandNoise, chunkX * 2 + 1, chunkZ * 2 + 1);
    float heightValue = getHeightValue(chunkX * 2 + 1, chunkZ * 2 + 1);

    // Select biome based on height value
    // Reference: lines 37-47
    if (heightValue > 40.0f) {
        // High areas: END_HIGHLANDS
        return BiomeKeys::END_HIGHLANDS;
    } else if (heightValue >= 0.0f) {
        // Medium areas: END_MIDLANDS
        return BiomeKeys::END_MIDLANDS;
    } else if (heightValue < -20.0f) {
        // Small scattered islands
        return BiomeKeys::SMALL_END_ISLANDS;
    } else {
        // Low areas near islands: END_BARRENS
        return BiomeKeys::END_BARRENS;
    }
}

float TheEndBiomeSource::getHeightValue(int32_t x, int32_t z) const {
    return getHeightValue(*m_islandNoise, x, z);
}

float TheEndBiomeSource::getHeightValue(const synth::SimplexNoise& noise, int32_t x, int32_t z) {
    // Reference: TheEndBiomeSource.java getHeightValue() lines 50-71
    //
    // This algorithm creates the characteristic End island pattern:
    // - Large main island at center (handled by caller)
    // - Ring of smaller islands at distance 1024+
    // - Scattered small islands everywhere else

    // Scale down coordinates for noise sampling
    // Reference: line 51-52
    int32_t scaledX = x / 2;
    int32_t scaledZ = z / 2;
    int32_t offsetX = x % 2;
    int32_t offsetZ = z % 2;

    // Calculate base height from simplex noise
    // Reference: line 54-56
    // float f = 100.0F - Mth.sqrt((float)(x * x + z * z)) * 8.0F;
    float f = 100.0f - std::sqrt(static_cast<float>(x * x + z * z)) * 8.0f;
    f = std::max(-100.0f, std::min(80.0f, f));

    // Iterate over nearby positions to create smooth islands
    // Reference: lines 58-70
    for (int32_t dx = -12; dx <= 12; ++dx) {
        for (int32_t dz = -12; dz <= 12; ++dz) {
            int64_t sampleX = static_cast<int64_t>(scaledX + dx);
            int64_t sampleZ = static_cast<int64_t>(scaledZ + dz);

            // Check if we're far enough from center (distance > 64 chunks = 1024 blocks)
            if (sampleX * sampleX + sampleZ * sampleZ > 4096L) {
                // Sample simplex noise at this position
                // Reference: line 64
                // float noiseValue = (SimplexNoise.getValue(noise, (double)sampleX, (double)sampleZ) - 0.5F) * 2.0F;
                float noiseValue = static_cast<float>(
                    (noise.getValue(static_cast<double>(sampleX), static_cast<double>(sampleZ)) - 0.5) * 2.0
                );

                // If noise indicates an island, calculate its contribution
                if (noiseValue < -0.9f) {
                    // This creates a small peak
                    // Reference: lines 65-68
                    float heightContrib = (std::abs(noiseValue) - 0.9f) * 3.3333333f;

                    // Calculate distance to this sample point
                    float distX = static_cast<float>(dx * 2 + offsetX);
                    float distZ = static_cast<float>(dz * 2 + offsetZ);
                    float dist = 100.0f - std::sqrt(distX * distX + distZ * distZ) * heightContrib;

                    // Clamp and take maximum
                    dist = std::max(-100.0f, std::min(80.0f, dist));
                    f = std::max(f, dist);
                }
            }
        }
    }

    return f;
}

} // namespace biome
} // namespace world
} // namespace minecraft
