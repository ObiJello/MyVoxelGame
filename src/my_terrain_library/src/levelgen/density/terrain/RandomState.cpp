#include "levelgen/density/terrain/RandomState.h"
#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/synth/NormalNoise.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"

#include <algorithm>
#include <stdexcept>

// Reference: levelgen.RandomState (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// WorldgenRandom.Algorithm.newInstance(seed).forkPositional().
random::AnyPositionalRandomFactory positionalFor(int64_t seed, bool legacy) {
    if (legacy) {
        random::AnyRandomSource source{::minecraft::LegacyRandomSource(seed)};
        return source.forkPositional();
    }
    random::AnyRandomSource source{::minecraft::XoroshiroRandomSource(seed)};
    return source.forkPositional();
}

} // namespace

// RandomState's anonymous DensityFunction.CompileContext.
class RandomState::Context final : public CompileContext {
public:
    explicit Context(RandomState& state) : m_state(state) {}

    synth::NoisePtr createNoiseSampler(const NoiseHolder& parameters) override {
        if (parameters.is("minecraft:nether/temperature")) {
            random::AnyRandomSource legacy{::minecraft::LegacyRandomSource(m_state.m_seed + 0)};
            return parameters.value->createForLegacyNetherBiome(legacy);
        }
        if (parameters.is("minecraft:nether/vegetation")) {
            random::AnyRandomSource legacy{::minecraft::LegacyRandomSource(m_state.m_seed + 1)};
            return parameters.value->createForLegacyNetherBiome(legacy);
        }
        if (!parameters.isReference()) {
            // Java: parameters.unwrapKey().orElseThrow().
            throw std::runtime_error("RandomState: an inline noise has no key to seed it from");
        }
        return m_state.getOrCreateNoise(parameters.key);
    }

    random::AnyRandomSource createRandom(const std::string& seedId) override {
        if (m_state.m_useLegacyRandom && seedId == "minecraft:terrain") {   // BlendedNoise.NOISE_SEED
            return random::AnyRandomSource{::minecraft::LegacyRandomSource(m_state.m_seed + 0)};
        }
        return m_state.m_random.fromHashOf(seedId);
    }

    random::AnyRandomSource createEndIslandRandom() override {
        return random::AnyRandomSource{::minecraft::LegacyRandomSource(m_state.m_seed)};
    }

private:
    RandomState& m_state;
};

RandomState::RandomState(int64_t seed, bool useLegacyRandom, WorldgenRegistries& registries)
    : m_seed(seed),
      m_useLegacyRandom(useLegacyRandom),
      m_random(positionalFor(seed, useLegacyRandom)),
      m_registries(registries),
      m_compileContext(std::make_unique<Context>(*this)),
      m_compiler(*m_compileContext) {}

RandomState::~RandomState() = default;

synth::NoisePtr RandomState::getOrCreateNoise(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_noiseMutex);
    auto it = m_noiseInstances.find(key);
    if (it != m_noiseInstances.end()) return it->second;
    // Noises.instantiate: noises.getOrThrow(key).value().create(random.fromHashOf(key)).
    std::shared_ptr<const synth::NormalNoise> definition = m_registries.noiseDefinition(key);
    synth::NoisePtr noise = synth::instantiate(*definition, key, m_random);
    m_noiseInstances.emplace(key, noise);
    return noise;
}

random::AnyPositionalRandomFactory RandomState::getOrCreateRandomFactory(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_noiseMutex);
    auto it = m_positionalRandoms.find(name);
    if (it != m_positionalRandoms.end()) return it->second;
    random::AnyRandomSource source = m_random.fromHashOf(name);
    random::AnyPositionalRandomFactory factory = source.forkPositional();
    m_positionalRandoms.emplace(name, factory);
    return factory;
}

std::unique_ptr<DensityBufferPool> RandomState::acquireDensityBufferPool() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (!m_pools.empty()) {
        std::unique_ptr<DensityBufferPool> pool = std::move(m_pools.back());
        m_pools.pop_back();
        return pool;
    }
    return std::make_unique<DensityBufferPool>(MAX_BUFFER_AGE_TICKS);
}

void RandomState::releaseDensityBufferPool(std::unique_ptr<DensityBufferPool> pool) {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (m_pools.size() < MAX_BUFFER_POOLS) {
        m_pools.push_back(std::move(pool));
    }
}

void RandomState::garbageCollect() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_pools.erase(std::remove_if(m_pools.begin(), m_pools.end(),
                                 [](const std::unique_ptr<DensityBufferPool>& pool) {
                                     pool->garbageCollect();
                                     return pool->isEmpty();
                                 }),
                  m_pools.end());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
