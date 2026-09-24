#include "levelgen/density/terrain/NoiseChunk.h"

#include "levelgen/density/ContextKeys.h"
#include "levelgen/density/terrain/RandomState.h"
#include "levelgen/density/terrain/TerrainSettings.h"

// Reference: levelgen.NoiseChunk (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

NoiseChunk::NoiseChunk(RandomState& randomState, std::shared_ptr<const DensitySampler> beardifier,
                       const TerrainSettings& settings, Aquifer::FluidPicker globalFluidPicker,
                       const DensityVolume& volume)
    : m_randomState(randomState),
      m_volume(volume),
      m_beardifier(std::move(beardifier)),
      m_bufferPool(randomState.acquireDensityBufferPool()) {
    ContextMap samplerUserFields;
    samplerUserFields.set(beardifierKey(), m_beardifier.get());
    m_context = std::make_unique<SamplerContext>(SamplerContext::builder()
                                                     .setUserFields(std::move(samplerUserFields))
                                                     .useBufferArena(*m_bufferPool)
                                                     .enableCaches()
                                                     .build());
    m_cachingSamplers = std::make_unique<DensitySamplerSet>(randomState.samplersWithContext(*m_context));
    if (settings.aquifers) {
        m_aquifer = settings.aquifers->create(*m_cachingSamplers, randomState.getOrCreateRandomFactory("minecraft:aquifer"),
                                              m_volume, std::move(globalFluidPicker));
    } else {
        m_aquifer = Aquifer::createDisabled(std::move(globalFluidPicker));
    }
}

NoiseChunk::~NoiseChunk() {
    // The samplers and the context borrow the pool: drop them first.
    m_aquifer.reset();
    m_cachingSamplers.reset();
    m_context.reset();
    m_randomState.releaseDensityBufferPool(std::move(m_bufferPool));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
