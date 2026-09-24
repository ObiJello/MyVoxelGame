#include "levelgen/density/generator/NoiseFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/synth/NormalNoise.h"

// Reference: generator.NoiseFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

DensitySamplerPtr NoiseFunction::compileSampler(CompileContext& context) const {
    synth::NoisePtr noise = context.createNoiseSampler(m_noise);
    if (functionsEqual(m_shiftX, zero()) && functionsEqual(m_shiftY, zero()) && functionsEqual(m_shiftZ, zero())) {
        return std::make_shared<Sampler>(noise, m_xzScale, m_yScale);
    }
    DensitySamplerPtr shiftX = m_shiftX->compileSampler(context);
    DensitySamplerPtr shiftZ = m_shiftZ->compileSampler(context);
    if (functionsEqual(m_shiftY, zero())) {
        return std::make_shared<ShiftedXzSampler>(shiftX, shiftZ, noise, m_xzScale, m_yScale);
    }
    DensitySamplerPtr shiftY = m_shiftY->compileSampler(context);
    return std::make_shared<ShiftedXyzSampler>(shiftX, shiftY, shiftZ, noise, m_xzScale, m_yScale);
}

DensityFunctionPtr NoiseFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr shiftX = rule.rewrite(m_shiftX);
    DensityFunctionPtr shiftY = rule.rewrite(m_shiftY);
    DensityFunctionPtr shiftZ = rule.rewrite(m_shiftZ);
    return shiftX == m_shiftX && shiftY == m_shiftY && shiftZ == m_shiftZ
               ? self()
               : std::make_shared<NoiseFunction>(m_noise, m_xzScale, m_yScale, shiftX, shiftY, shiftZ);
}

Interval NoiseFunction::range() const {
    return m_noise.value->range();
}

int NoiseFunction::domainAxes() const {
    int axes = 7;
    if (m_yScale == 0.0) {
        axes &= -3;
    }
    if (m_xzScale == 0.0) {
        axes &= -6;
    }
    return axes | m_shiftX->domainAxes() | m_shiftY->domainAxes() | m_shiftZ->domainAxes();
}

bool NoiseFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const NoiseFunction*>(&other);
    return o != nullptr && m_noise == o->m_noise && doubleEq(m_xzScale, o->m_xzScale) &&
           doubleEq(m_yScale, o->m_yScale) && functionsEqual(m_shiftX, o->m_shiftX) &&
           functionsEqual(m_shiftY, o->m_shiftY) && functionsEqual(m_shiftZ, o->m_shiftZ);
}

size_t NoiseFunction::hash() const {
    size_t h = hashCombine(0x25, m_noise.hash());
    h = hashCombine(h, hashDouble(m_xzScale));
    h = hashCombine(h, hashDouble(m_yScale));
    h = hashCombine(h, m_shiftX->hash());
    h = hashCombine(h, m_shiftY->hash());
    return hashCombine(h, m_shiftZ->hash());
}

// ---- Sampler -------------------------------------------------------------------

void NoiseFunction::Sampler::sampleVolume(SamplerContext&, DensityBuffer& outputBuffer,
                                          const DensityVolume& volume) const {
    outputBuffer.fill(0.0f);
    m_noise->addToVolume(outputBuffer, volume, m_xzScale, m_yScale, 1.0f);
}

float NoiseFunction::Sampler::sampleValue(SamplerContext&, int blockX, int blockY, int blockZ) const {
    return m_noise->get(static_cast<double>(blockX) * m_xzScale, static_cast<double>(blockY) * m_yScale,
                        static_cast<double>(blockZ) * m_xzScale);
}

// ---- ShiftedXzSampler ------------------------------------------------------------

void NoiseFunction::ShiftedXzSampler::sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                                                   const DensityVolume& volume) const {
    m_shiftX->sampleVolume(context, outputBuffer, volume);
    ScopedBuffer shiftZBuffer = context.acquireBuffer(volume);
    m_shiftZ->sampleVolume(context, *shiftZBuffer, volume);
    int index = 0;
    for (int z = 0; z < volume.sizeZ; ++z) {
        const double baseNoiseZ = static_cast<double>(volume.blockZ(z)) * m_xzScale;
        for (int x = 0; x < volume.sizeX; ++x) {
            const double baseNoiseX = static_cast<double>(volume.blockX(x)) * m_xzScale;
            for (int y = 0; y < volume.sizeY; ++y) {
                const double noiseX = baseNoiseX + static_cast<double>(outputBuffer.get(index));
                const double noiseY = static_cast<double>(volume.blockY(y)) * m_yScale;
                const double noiseZ = baseNoiseZ + static_cast<double>(shiftZBuffer->get(index));
                outputBuffer.set(index, m_noise->get(noiseX, noiseY, noiseZ));
                ++index;
            }
        }
    }
}

float NoiseFunction::ShiftedXzSampler::sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const {
    const double x = static_cast<double>(blockX) * m_xzScale +
                     static_cast<double>(m_shiftX->sampleValue(context, blockX, blockY, blockZ));
    const double y = static_cast<double>(blockY) * m_yScale;
    const double z = static_cast<double>(blockZ) * m_xzScale +
                     static_cast<double>(m_shiftZ->sampleValue(context, blockX, blockY, blockZ));
    return m_noise->get(x, y, z);
}

// ---- ShiftedXyzSampler -----------------------------------------------------------

void NoiseFunction::ShiftedXyzSampler::sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                                                    const DensityVolume& volume) const {
    m_shiftX->sampleVolume(context, outputBuffer, volume);
    ScopedBuffer shiftYBuffer = context.acquireBuffer(volume);
    m_shiftY->sampleVolume(context, *shiftYBuffer, volume);
    ScopedBuffer shiftZBuffer = context.acquireBuffer(volume);
    m_shiftZ->sampleVolume(context, *shiftZBuffer, volume);
    int index = 0;
    for (int z = 0; z < volume.sizeZ; ++z) {
        const double baseNoiseZ = static_cast<double>(volume.blockZ(z)) * m_xzScale;
        for (int x = 0; x < volume.sizeX; ++x) {
            const double baseNoiseX = static_cast<double>(volume.blockX(x)) * m_xzScale;
            for (int y = 0; y < volume.sizeY; ++y) {
                const double noiseX = baseNoiseX + static_cast<double>(outputBuffer.get(index));
                const double noiseY = static_cast<double>(volume.blockY(y)) * m_yScale +
                                      static_cast<double>(shiftYBuffer->get(index));
                const double noiseZ = baseNoiseZ + static_cast<double>(shiftZBuffer->get(index));
                outputBuffer.set(index, m_noise->get(noiseX, noiseY, noiseZ));
                ++index;
            }
        }
    }
}

float NoiseFunction::ShiftedXyzSampler::sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const {
    const double x = static_cast<double>(blockX) * m_xzScale +
                     static_cast<double>(m_shiftX->sampleValue(context, blockX, blockY, blockZ));
    const double y = static_cast<double>(blockY) * m_yScale +
                     static_cast<double>(m_shiftY->sampleValue(context, blockX, blockY, blockZ));
    const double z = static_cast<double>(blockZ) * m_xzScale +
                     static_cast<double>(m_shiftZ->sampleValue(context, blockX, blockY, blockZ));
    return m_noise->get(x, y, z);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
