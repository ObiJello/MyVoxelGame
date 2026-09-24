#pragma once

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/SamplerContext.h"
#include "random/AnyPositionalRandomFactory.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Reference: levelgen.RandomState (26.3) - everything seeded for one world
// and one noise-settings preset: the positional random, the noise instances
// (one per registry noise, created on first use), the density function
// compiler, and a pool of density buffer pools lent to NoiseChunks.

namespace minecraft {
namespace levelgen {
namespace density {

class WorldgenRegistries;

class RandomState {
public:
    RandomState(int64_t seed, bool useLegacyRandom, WorldgenRegistries& registries);
    ~RandomState();

    RandomState(const RandomState&) = delete;
    RandomState& operator=(const RandomState&) = delete;

    DensitySamplerSet samplersWithContext(SamplerContext& context) { return DensitySamplerSet(m_compiler, context); }
    const DensitySampler& getSampler(const DensityFunctionPtr& function) { return m_compiler.getSampler(function); }
    float sampleBlockValueUncached(const DensityFunctionPtr& function, int x, int y, int z) {
        return getSampler(function).sampleValue(SamplerContext::emptyUncached(), x, y, z);
    }

    synth::NoisePtr getOrCreateNoise(const std::string& key);
    random::AnyPositionalRandomFactory getOrCreateRandomFactory(const std::string& name);

    // Buffer pools: one per NoiseChunk in flight, reused afterwards.
    std::unique_ptr<DensityBufferPool> acquireDensityBufferPool();
    void releaseDensityBufferPool(std::unique_ptr<DensityBufferPool> pool);
    void garbageCollect();

    int64_t seed() const { return m_seed; }
    const random::AnyPositionalRandomFactory& random() const { return m_random; }
    bool useLegacyRandom() const { return m_useLegacyRandom; }

private:
    class Context;

    static constexpr size_t MAX_BUFFER_POOLS = 16;
    static constexpr int MAX_BUFFER_AGE_TICKS = 20;

    int64_t m_seed;
    bool m_useLegacyRandom;
    random::AnyPositionalRandomFactory m_random;
    WorldgenRegistries& m_registries;

    std::mutex m_noiseMutex;
    std::unordered_map<std::string, synth::NoisePtr> m_noiseInstances;
    std::unordered_map<std::string, random::AnyPositionalRandomFactory> m_positionalRandoms;

    std::unique_ptr<Context> m_compileContext;
    DensityFunctionCompiler m_compiler;

    std::mutex m_poolMutex;
    std::vector<std::unique_ptr<DensityBufferPool>> m_pools;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
