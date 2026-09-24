#pragma once

#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/density/terrain/RandomState.h"
#include "random/AnyPositionalRandomFactory.h"
#include "world/biome/Climate.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Reference: levelgen.RandomState (26.3) - everything seeded for one world and
// one NoiseGeneratorSettings. The density side (noise instances, the density
// function compiler, buffer pools) is density::RandomState; this class adds
// what Java's RandomState also owns: the MaterialSystem and the climate
// samplers, plus stable pointers to the positional randoms for the engine's
// callers.

namespace minecraft {
namespace levelgen {

namespace material {
class MaterialSystem;
}

class RandomState {
public:
    RandomState(std::shared_ptr<const NoiseGeneratorSettings> settings, int64_t seed);
    ~RandomState();

    RandomState(const RandomState&) = delete;
    RandomState& operator=(const RandomState&) = delete;

    static RandomState* create(std::shared_ptr<const NoiseGeneratorSettings> settings, int64_t seed) {
        return new RandomState(std::move(settings), seed);
    }

    density::RandomState& density() { return *m_density; }
    const NoiseGeneratorSettings& settings() const { return *m_settings; }

    // RandomState.createClimateSampler(context). The context must outlive
    // the sampler.
    world::biome::Climate::Sampler createClimateSampler(density::SamplerContext& context);
    // A sampler over SamplerContext.EMPTY_UNCACHED (no caches, the global
    // arena): what createUncachedResolver uses; safe on any thread.
    world::biome::Climate::Sampler* sampler() { return &m_uncachedSampler; }

    // RandomState.surfaceSystem() (26.3's MaterialSystem).
    material::MaterialSystem* surfaceSystem() { return m_materialSystem.get(); }

    // WorldgenRandom.Algorithm.newInstance(seed).forkPositional().
    random::AnyPositionalRandomFactory* random() { return &m_random; }
    // getOrCreateRandomFactory(name): random.fromHashOf(name).forkPositional(),
    // one per name, at a stable address.
    random::AnyPositionalRandomFactory* getOrCreateRandomFactory(const std::string& name);

    int64_t seed() const { return m_seed; }

private:
    std::shared_ptr<const NoiseGeneratorSettings> m_settings;
    int64_t m_seed;
    std::unique_ptr<density::RandomState> m_density;
    random::AnyPositionalRandomFactory m_random;
    world::biome::Climate::Sampler m_uncachedSampler;
    std::unique_ptr<material::MaterialSystem> m_materialSystem;

    std::mutex m_factoryMutex;
    std::unordered_map<std::string, std::unique_ptr<random::AnyPositionalRandomFactory>> m_factories;
};

} // namespace levelgen
} // namespace minecraft
