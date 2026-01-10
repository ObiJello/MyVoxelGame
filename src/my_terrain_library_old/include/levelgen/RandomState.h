#pragma once

#include "world/biome/Climate.h"
#include "levelgen/NoiseRouter.h"
#include "random/PositionalRandomFactory.h"
#include "synth/NormalNoise.h"
#include <unordered_map>
#include <string>
#include <cstdint>

// Forward declarations
namespace minecraft {
    class XoroshiroPositionalRandomFactory;
    namespace levelgen {
        class NoiseGeneratorSettings;
        class SurfaceSystem;
    }
}

// Reference: net/minecraft/world/level/levelgen/RandomState.java

namespace minecraft {
namespace levelgen {

/**
 * RandomState - Manages random number generation and noise routing for world generation
 *
 * In Minecraft, this class:
 * - Creates and manages noise instances
 * - Provides the NoiseRouter for terrain generation
 * - Manages aquifer and ore RNGs
 * - Wires up all density functions
 *
 * Reference: RandomState.java lines 17-149
 */
class RandomState {
private:
    // Reference: RandomState.java line 18
    minecraft::random::PositionalRandomFactory* m_random;

    // Reference: RandomState.java line 20
    NoiseRouter* m_router;

    // Reference: RandomState.java line 21
    minecraft::world::biome::Climate::Sampler* m_sampler;

    // Reference: RandomState.java line 22
    SurfaceSystem* m_surfaceSystem;

    // Reference: RandomState.java line 23
    XoroshiroPositionalRandomFactory* m_aquiferRandom;

    // Reference: RandomState.java line 24
    XoroshiroPositionalRandomFactory* m_oreRandom;

    // Reference: RandomState.java line 25
    // Map of noise name -> NormalNoise instance
    std::unordered_map<std::string, NormalNoise*> m_noiseInstances;

    // Reference: RandomState.java line 26
    // Map of identifier -> PositionalRandomFactory
    std::unordered_map<std::string, random::PositionalRandomFactory*> m_positionalRandoms;

public:
    /**
     * Create RandomState from settings and seed
     * Reference: RandomState.java lines 32-34
     */
    static RandomState* create(NoiseGeneratorSettings* settings, int64_t seed);

    /**
     * Constructor
     * Reference: RandomState.java lines 36-120
     */
    RandomState(NoiseGeneratorSettings* settings, int64_t seed);

    /**
     * Destructor - clean up all allocated resources
     */
    ~RandomState();

    /**
     * Get or create a noise instance by name
     * Reference: RandomState.java lines 122-124
     */
    NormalNoise* getOrCreateNoise(const std::string& noiseName);

    /**
     * Get or create a positional random factory by identifier
     * Reference: RandomState.java lines 126-128
     */
    minecraft::random::PositionalRandomFactory* getOrCreateRandomFactory(const std::string& identifier);

    // Accessors (Reference: RandomState.java lines 130-148)
    NoiseRouter* router() const { return m_router; }
    minecraft::world::biome::Climate::Sampler* sampler() const { return m_sampler; }
    SurfaceSystem* surfaceSystem() const { return m_surfaceSystem; }
    XoroshiroPositionalRandomFactory* aquiferRandom() const { return m_aquiferRandom; }
    XoroshiroPositionalRandomFactory* oreRandom() const { return m_oreRandom; }
    minecraft::random::PositionalRandomFactory* random() const { return m_random; }

};

} // namespace levelgen
} // namespace minecraft
