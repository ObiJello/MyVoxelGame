#include "levelgen/RandomState.h"

#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/material/MaterialSystem.h"

// Reference: levelgen.RandomState (26.3).

namespace minecraft {
namespace levelgen {

RandomState::RandomState(std::shared_ptr<const NoiseGeneratorSettings> settings, int64_t seed)
    : m_settings(std::move(settings)),
      m_seed(seed),
      m_density(std::make_unique<density::RandomState>(seed, m_settings->useLegacyRandomSource(),
                                                       density::WorldgenRegistries::get())),
      m_random(m_density->random()) {
    m_uncachedSampler = createClimateSampler(density::SamplerContext::emptyUncached());
    // new MaterialSystem(this, defaultBlock, seaLevel, router.chunkSurfaceLevel(), this.random)
    m_materialSystem = std::make_unique<material::MaterialSystem>(
        *m_density, m_settings->defaultBlock(), m_settings->seaLevel(),
        m_settings->noiseRouter().chunkSurfaceLevel, m_density->random());
}

RandomState::~RandomState() = default;

// NoiseRouter.createClimateSampler(randomState.samplersWithContext(context)).
world::biome::Climate::Sampler RandomState::createClimateSampler(density::SamplerContext& context) {
    const density::DensitySamplerSet samplers = m_density->samplersWithContext(context);
    const density::NoiseRouter& router = m_settings->noiseRouter();
    return world::biome::Climate::Sampler(samplers.get(router.temperature), samplers.get(router.vegetation),
                                          samplers.get(router.continents), samplers.get(router.erosion),
                                          samplers.get(router.depth), samplers.get(router.ridges));
}

random::AnyPositionalRandomFactory* RandomState::getOrCreateRandomFactory(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_factoryMutex);
    auto it = m_factories.find(name);
    if (it != m_factories.end()) return it->second.get();
    auto factory = std::make_unique<random::AnyPositionalRandomFactory>(m_density->getOrCreateRandomFactory(name));
    random::AnyPositionalRandomFactory* raw = factory.get();
    m_factories.emplace(name, std::move(factory));
    return raw;
}

} // namespace levelgen
} // namespace minecraft
