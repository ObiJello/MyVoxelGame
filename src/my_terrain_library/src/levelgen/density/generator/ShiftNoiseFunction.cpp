#include "levelgen/density/generator/ShiftNoiseFunction.h"

#include "levelgen/density/generator/NoiseFunction.h"
#include "levelgen/density/op/BinaryFunction.h"
#include "levelgen/density/synth/NormalNoise.h"

// Reference: generator.ShiftNoiseFunction (26.3).
//
// Java's rewriteChildren on all three returns a NEW record (never `this`);
// kept as is.

namespace minecraft {
namespace levelgen {
namespace density {

Interval ShiftNoiseFunction::range() const {
    return Interval::mul(m_offsetNoise.value->range(), Interval::ofExact(4.0f));
}

// ---- ShiftB ----------------------------------------------------------------------

namespace {

// The anonymous DensitySampler in ShiftB.compileSampler.
class ShiftBSampler final : public DensitySampler {
public:
    explicit ShiftBSampler(synth::NoisePtr noise) : m_noise(std::move(noise)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        const DensityVolume transposedVolume(volume.sizeZ, volume.sizeX, 1, volume.minBlockZ, volume.minBlockX, 0,
                                             volume.stepBlockZ, volume.stepBlockX, 1);
        ScopedBuffer transposedBuffer = context.acquireBuffer(transposedVolume);
        transposedBuffer->fill(0.0f);
        m_noise->addToVolume(*transposedBuffer, transposedVolume, 0.25, 0.25, 4.0f);
        for (int z = 0; z < volume.sizeZ; ++z) {
            for (int x = 0; x < volume.sizeX; ++x) {
                const float value = transposedBuffer->get(transposedVolume.indexUnchecked(z, x, 0));
                outputBuffer.setRange(volume.indexUnchecked(x, 0, z), volume.sizeY, value);
            }
        }
    }

    float sampleValue(SamplerContext&, int blockX, int, int blockZ) const override {
        return m_noise->get(static_cast<double>(blockZ) * 0.25, static_cast<double>(blockX) * 0.25, 0.0) * 4.0f;
    }

private:
    synth::NoisePtr m_noise;
};

} // namespace

DensitySamplerPtr ShiftNoiseFunction::ShiftB::compileSampler(CompileContext& context) const {
    synth::NoisePtr noise = context.createNoiseSampler(m_offsetNoise);
    return std::make_shared<ShiftBSampler>(std::move(noise));
}

DensityFunctionPtr ShiftNoiseFunction::ShiftB::rewriteChildren(const DfRewriteRule&) const {
    return std::make_shared<ShiftB>(m_offsetNoise);
}

bool ShiftNoiseFunction::ShiftB::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const ShiftB*>(&other);
    return o != nullptr && m_offsetNoise == o->m_offsetNoise;
}

// ---- ShiftA ----------------------------------------------------------------------

DensitySamplerPtr ShiftNoiseFunction::ShiftA::compileSampler(CompileContext& context) const {
    synth::NoisePtr noise = context.createNoiseSampler(m_offsetNoise);
    return std::make_shared<BinaryFunction::ConstMulSampler>(
        std::make_shared<NoiseFunction::Sampler>(noise, 0.25, 0.0), 4.0f);
}

DensityFunctionPtr ShiftNoiseFunction::ShiftA::rewriteChildren(const DfRewriteRule&) const {
    return std::make_shared<ShiftA>(m_offsetNoise);
}

bool ShiftNoiseFunction::ShiftA::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const ShiftA*>(&other);
    return o != nullptr && m_offsetNoise == o->m_offsetNoise;
}

// ---- Shift -----------------------------------------------------------------------

DensitySamplerPtr ShiftNoiseFunction::Shift::compileSampler(CompileContext& context) const {
    synth::NoisePtr noise = context.createNoiseSampler(m_offsetNoise);
    return std::make_shared<BinaryFunction::ConstMulSampler>(
        std::make_shared<NoiseFunction::Sampler>(noise, 0.25, 0.25), 4.0f);
}

DensityFunctionPtr ShiftNoiseFunction::Shift::rewriteChildren(const DfRewriteRule&) const {
    return std::make_shared<Shift>(m_offsetNoise);
}

bool ShiftNoiseFunction::Shift::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const Shift*>(&other);
    return o != nullptr && m_offsetNoise == o->m_offsetNoise;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
