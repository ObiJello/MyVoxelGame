#include "levelgen/material/MaterialRuleContext.h"

#include "levelgen/density/JavaMath.h"
#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/synth/Noise.h"
#include "levelgen/density/terrain/RandomState.h"
#include "levelgen/material/MaterialSystem.h"

#include <stdexcept>
#include <utility>

// Reference: levelgen.material.MaterialRuleContext (26.3).

namespace minecraft {
namespace levelgen {
namespace material {

// ---- DensityGetter ------------------------------------------------------------

DensityGetter::DensityGetter(const MaterialRuleContext& context, density::BoundSampler sampler,
                             std::unique_ptr<density::DensityBuffer> prefilled)
    : m_context(&context), m_sampler(sampler), m_buffer(std::move(prefilled)) {}

float DensityGetter::get() const {
    const MaterialRuleContext& context = *m_context;
    if (m_buffer) {
        const int index = context.expectedVolume().indexOfBlock(context.blockX(), context.blockY(), context.blockZ());
        return index == density::DensityVolume::NO_BLOCK
                   ? m_sampler.sampleValue(context.blockX(), context.blockY(), context.blockZ())
                   : m_buffer->get(index);
    }
    return m_sampler.sampleValue(context.blockX(), context.blockY(), context.blockZ());
}

// ---- Noise samplers (createNoiseSampler2d / createNoiseSampler3d) ------------

class MaterialRuleContext::NoiseSampler2d final : public DoubleSupplier {
public:
    NoiseSampler2d(const MaterialRuleContext& context, density::synth::NoisePtr noise)
        : m_context(context), m_noise(std::move(noise)), m_lastUpdateXZ(context.m_lastUpdateXZ - 1) {}

    double getAsDouble() override {
        if (m_lastUpdateXZ != m_context.m_lastUpdateXZ) {
            m_lastNoise = static_cast<double>(
                m_noise->get(static_cast<double>(m_context.m_blockX), 0.0, static_cast<double>(m_context.m_blockZ)));
            m_lastUpdateXZ = m_context.m_lastUpdateXZ;
        }
        return m_lastNoise;
    }

private:
    const MaterialRuleContext& m_context;
    density::synth::NoisePtr m_noise;
    int64_t m_lastUpdateXZ;
    double m_lastNoise = 0.0;
};

class MaterialRuleContext::NoiseSampler3d final : public DoubleSupplier {
public:
    NoiseSampler3d(const MaterialRuleContext& context, density::synth::NoisePtr noise)
        : m_context(context), m_noise(std::move(noise)), m_lastUpdateY(context.m_lastUpdateY - 1) {}

    double getAsDouble() override {
        if (m_lastUpdateY != m_context.m_lastUpdateY) {
            m_lastNoise = static_cast<double>(m_noise->get(static_cast<double>(m_context.m_blockX),
                                                           static_cast<double>(m_context.m_blockY),
                                                           static_cast<double>(m_context.m_blockZ)));
            m_lastUpdateY = m_context.m_lastUpdateY;
        }
        return m_lastNoise;
    }

private:
    const MaterialRuleContext& m_context;
    density::synth::NoisePtr m_noise;
    int64_t m_lastUpdateY;
    double m_lastNoise = 0.0;
};

// ---- MaterialRuleContext -------------------------------------------------------

MaterialRuleContext::MaterialRuleContext(const MaterialSystem& system, density::RandomState& randomState,
                                         const density::DensityVolume& expectedVolume,
                                         const density::DensitySamplerSet& densitySamplers, BiomeGetter biomeGetter,
                                         const GenerationContext& context, const PossibleBiomes* possibleBiomes)
    : m_system(system),
      m_randomState(randomState),
      m_expectedVolume(expectedVolume),
      m_densitySamplers(densitySamplers),
      m_biomeGetter(std::move(biomeGetter)),
      m_context(context),
      m_possibleBiomes(possibleBiomes),
      m_preliminarySurfaceVolume(expectedVolume.sizeX, 1, expectedVolume.sizeZ, expectedVolume.minBlockX, 0,
                                 expectedVolume.minBlockZ),
      m_lastSurfaceDepth2Update(m_lastUpdateXZ - 1),
      m_lastMinSurfaceLevelUpdate(m_lastUpdateXZ - 1) {}

void MaterialRuleContext::updateXZ(int32_t blockX, int32_t blockZ, int32_t surfaceGradientX,
                                   int32_t surfaceGradientZ) {
    ++m_lastUpdateXZ;
    ++m_lastUpdateY;
    m_blockX = blockX;
    m_blockZ = blockZ;
    m_surfaceGradientX = surfaceGradientX;
    m_surfaceGradientZ = surfaceGradientZ;
    m_surfaceDepth = m_system.getSurfaceDepth(blockX, blockZ);
}

void MaterialRuleContext::updateY(int32_t stoneDepthAbove, int32_t stoneDepthBelow, int32_t waterHeight,
                                  int32_t blockY) {
    ++m_lastUpdateY;
    m_biome = nullptr;
    m_biomeResolved = false;
    m_blockY = blockY;
    m_waterHeight = waterHeight;
    m_stoneDepthBelow = stoneDepthBelow;
    m_stoneDepthAbove = stoneDepthAbove;
}

double MaterialRuleContext::getSurfaceSecondary() {
    if (m_lastSurfaceDepth2Update != m_lastUpdateXZ) {
        m_lastSurfaceDepth2Update = m_lastUpdateXZ;
        m_surfaceSecondary = m_system.getSurfaceSecondary(m_blockX, m_blockZ);
    }
    return m_surfaceSecondary;
}

world::biome::BiomeHolder MaterialRuleContext::getBiome() {
    if (!m_biomeResolved) {
        m_biome = m_biomeGetter(m_pos.set(m_blockX, m_blockY, m_blockZ));
        m_biomeResolved = true;
    }
    return m_biome;
}

int32_t MaterialRuleContext::getSeaLevel() const {
    return m_system.getSeaLevel();
}

int32_t MaterialRuleContext::getMinSurfaceLevel() {
    if (m_lastMinSurfaceLevelUpdate != m_lastUpdateXZ) {
        m_lastMinSurfaceLevelUpdate = m_lastUpdateXZ;
        const int index = m_preliminarySurfaceVolume.indexOfBlock(m_blockX, 0, m_blockZ);
        float preliminarySurfaceLevel;
        if (index != density::DensityVolume::NO_BLOCK) {
            if (!m_preliminarySurfaceBuffer) {
                m_preliminarySurfaceBuffer = density::DensityBuffer::createUnpooled(m_preliminarySurfaceVolume.size());
                m_densitySamplers.get(m_system.preliminarySurfaceFunction())
                    .sampleVolume(*m_preliminarySurfaceBuffer, m_preliminarySurfaceVolume);
            }
            preliminarySurfaceLevel = m_preliminarySurfaceBuffer->get(index);
        } else {
            preliminarySurfaceLevel =
                m_densitySamplers.sampleValue(m_system.preliminarySurfaceFunction(), m_blockX, 0, m_blockZ);
        }
        m_minSurfaceLevel = density::jmath::floor(preliminarySurfaceLevel) + m_surfaceDepth -
                            HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE;
    }
    return m_minSurfaceLevel;
}

DoubleSupplier& MaterialRuleContext::getNoiseSampler(const std::string& noiseId, bool is3d) {
    const std::string key = density::WorldgenRegistries::normalizeKey(noiseId);
    auto& samplers = is3d ? m_noiseSamplers3d : m_noiseSamplers2d;
    auto it = samplers.find(key);
    if (it != samplers.end()) return *it->second;
    density::synth::NoisePtr noise = m_randomState.getOrCreateNoise(key);
    std::unique_ptr<DoubleSupplier> sampler;
    if (is3d) {
        sampler = std::make_unique<NoiseSampler3d>(*this, std::move(noise));
    } else {
        sampler = std::make_unique<NoiseSampler2d>(*this, std::move(noise));
    }
    DoubleSupplier& result = *sampler;
    samplers.emplace(key, std::move(sampler));
    return result;
}

random::AnyPositionalRandomFactory MaterialRuleContext::getOrCreateRandomFactory(const std::string& name) {
    return m_randomState.getOrCreateRandomFactory(density::WorldgenRegistries::normalizeKey(name));
}

DensityGetter MaterialRuleContext::getDensitiesInChunk(const density::DensityFunctionPtr& function, bool prefill) {
    const density::BoundSampler sampler = m_densitySamplers.get(function);
    if (prefill) {
        std::unique_ptr<density::DensityBuffer> buffer = density::DensityBuffer::createUnpooled(m_expectedVolume.size());
        sampler.sampleVolume(*buffer, m_expectedVolume);
        return DensityGetter(*this, sampler, std::move(buffer));
    }
    return DensityGetter(*this, sampler, nullptr);
}

BlockState* MaterialRuleContext::getBand(int32_t x, int32_t y, int32_t z) const {
    return m_system.getBand(x, y, z);
}

// ---- LazyYCondition / LazyXZCondition ------------------------------------------

MaterialRuleContext::LazyYCondition::LazyYCondition(MaterialRuleContext& context)
    : context(context), m_lastUpdate(context.m_lastUpdateY - 1) {}

bool MaterialRuleContext::LazyYCondition::test() {
    const int64_t lastContextUpdate = context.m_lastUpdateY;
    if (lastContextUpdate == m_lastUpdate) {
        if (!m_result.has_value()) throw std::logic_error("Update triggered but the result is null");
        return *m_result;
    }
    m_lastUpdate = lastContextUpdate;
    m_result = compute();
    return *m_result;
}

MaterialRuleContext::LazyXZCondition::LazyXZCondition(MaterialRuleContext& context)
    : context(context), m_lastUpdate(context.m_lastUpdateXZ - 1) {}

bool MaterialRuleContext::LazyXZCondition::test() {
    const int64_t lastContextUpdate = context.m_lastUpdateXZ;
    if (lastContextUpdate == m_lastUpdate) {
        if (!m_result.has_value()) throw std::logic_error("Update triggered but the result is null");
        return *m_result;
    }
    m_lastUpdate = lastContextUpdate;
    m_result = compute();
    return *m_result;
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
