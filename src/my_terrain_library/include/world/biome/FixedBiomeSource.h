#pragma once

#include "world/biome/BiomeSource.h"

// Reference: net/minecraft/world/level/biome/FixedBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

/**
 * FixedBiomeSource - Returns a single biome everywhere.
 * Used by Single Biome worlds and superflat (FlatLevelSource).
 * Reference: FixedBiomeSource.java
 */
class FixedBiomeSource : public BiomeSource {
public:
    explicit FixedBiomeSource(const std::string& biomeKey) : m_biome(biomeKey) {
        m_possibleBiomes.insert(m_biome);
    }

    // Reference: FixedBiomeSource.getNoiseBiome() - sampler ignored.
    BiomeKey getNoiseBiome(int32_t /*quartX*/, int32_t /*quartY*/, int32_t /*quartZ*/,
                           const Climate::Sampler& /*sampler*/) override {
        return m_biome;
    }

    const BiomeKey& biome() const { return m_biome; }

private:
    BiomeKey m_biome;
};

} // namespace biome
} // namespace world
} // namespace minecraft
