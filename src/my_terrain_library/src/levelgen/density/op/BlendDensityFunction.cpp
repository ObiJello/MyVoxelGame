#include "levelgen/density/op/BlendDensityFunction.h"

#include "levelgen/density/Blender.h"
#include "levelgen/density/ContextKeys.h"
#include "levelgen/density/SamplerContext.h"

// Reference: op.BlendDensityFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// BlendDensityFunction.Sampler.
class BlendDensitySampler final : public DensitySampler {
public:
    explicit BlendDensitySampler(DensitySamplerPtr input) : m_input(std::move(input)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        m_input->sampleVolume(context, outputBuffer, volume);
        const Blender* blender = context.getField(blenderKey());
        if (blender != nullptr && !blender->isEmpty()) {
            int index = 0;
            for (int z = 0; z < volume.sizeZ; ++z) {
                const int blockZ = volume.blockZ(z);
                for (int x = 0; x < volume.sizeX; ++x) {
                    const int blockX = volume.blockX(x);
                    for (int y = 0; y < volume.sizeY; ++y) {
                        const int blockY = volume.blockY(y);
                        outputBuffer.set(index, blender->blendDensity(blockX, blockY, blockZ, outputBuffer.get(index)));
                        ++index;
                    }
                }
            }
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float input = m_input->sampleValue(context, blockX, blockY, blockZ);
        const Blender* blender = context.getField(blenderKey());
        return blender != nullptr && !blender->isEmpty() ? blender->blendDensity(blockX, blockY, blockZ, input) : input;
    }

private:
    DensitySamplerPtr m_input;
};

} // namespace

DensitySamplerPtr BlendDensityFunction::compileSampler(CompileContext& context) const {
    return std::make_shared<BlendDensitySampler>(m_input->compileSampler(context));
}

DensityFunctionPtr BlendDensityFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<BlendDensityFunction>(input);
}

bool BlendDensityFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const BlendDensityFunction*>(&other);
    return o != nullptr && functionsEqual(m_input, o->m_input);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
