#include "levelgen/density/CoreFunctions.h"

#include <stdexcept>

// Reference (26.3): generator.ConstantFunction, op.CacheFunction,
// op.SliceFunction, DensityFunctions.HolderHolder.

namespace minecraft {
namespace levelgen {
namespace density {

// ---- ConstantFunction --------------------------------------------------------

DensitySamplerPtr ConstantFunction::compileSampler(CompileContext&) const {
    return std::make_shared<Sampler>(m_value);
}

bool ConstantFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const ConstantFunction*>(&other);
    return o != nullptr && floatEq(m_value, o->m_value);
}

DensityFunctionPtr zero() {
    static const DensityFunctionPtr kZero = std::make_shared<ConstantFunction>(0.0f);
    return kZero;
}

DensityFunctionPtr constant(float value) {
    return std::make_shared<ConstantFunction>(value);
}

const ConstantFunction* asConstant(const DensityFunctionPtr& function) {
    return dynamic_cast<const ConstantFunction*>(function.get());
}

// ---- CacheFunction -----------------------------------------------------------

DensitySamplerPtr CacheFunction::compileSampler(CompileContext&) const {
    throw std::logic_error("Cannot compile cache before it has been deduplicated");
}

DensityFunctionPtr CacheFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<CacheFunction>(input);
}

bool CacheFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const CacheFunction*>(&other);
    return o != nullptr && functionsEqual(m_input, o->m_input);
}

// ---- ReferenceFunction (HolderHolder) -----------------------------------------

DensityFunctionPtr ReferenceFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr newFunction = rule.rewrite(m_target);
    return newFunction == m_target ? self() : newFunction;
}

bool ReferenceFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const ReferenceFunction*>(&other);
    return o != nullptr && m_key == o->m_key;
}

// ---- SliceFunction -----------------------------------------------------------

namespace {

class XzSampler final : public DensitySampler {
public:
    XzSampler(DensitySamplerPtr input, int x, int z) : m_input(std::move(input)), m_x(x), m_z(z) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if (volume.sizeX == 1 && volume.sizeZ == 1 && volume.minBlockX == m_x && volume.minBlockZ == m_z) {
            m_input->sampleVolume(context, outputBuffer, volume);
            return;
        }
        const DensityVolume inputVolume(1, volume.sizeY, 1, m_x, volume.minBlockY, m_z,
                                        volume.stepBlockX, volume.stepBlockY, volume.stepBlockZ);
        ScopedBuffer inputBuffer = context.acquireBuffer(inputVolume);
        m_input->sampleVolume(context, *inputBuffer, inputVolume);
        for (int y = 0; y < volume.sizeY; ++y) {
            const float input = inputBuffer->get(inputVolume.indexUnchecked(0, y, 0));
            int index = volume.indexUnchecked(0, y, 0);
            for (int z = 0; z < volume.sizeZ; ++z) {
                for (int x = 0; x < volume.sizeX; ++x) {
                    outputBuffer.set(index, input);
                    index += volume.sizeY;
                }
            }
        }
    }
    float sampleValue(SamplerContext& context, int, int blockY, int) const override {
        return m_input->sampleValue(context, m_x, blockY, m_z);
    }
private:
    DensitySamplerPtr m_input;
    int m_x;
    int m_z;
};

class XSampler final : public DensitySampler {
public:
    XSampler(DensitySamplerPtr input, int x) : m_input(std::move(input)), m_x(x) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if (volume.sizeX == 1 && volume.minBlockX == m_x) {
            m_input->sampleVolume(context, outputBuffer, volume);
            return;
        }
        const DensityVolume inputVolume(1, volume.sizeY, volume.sizeZ, m_x, volume.minBlockY, volume.minBlockZ,
                                        volume.stepBlockX, volume.stepBlockY, volume.stepBlockZ);
        ScopedBuffer inputBuffer = context.acquireBuffer(inputVolume);
        m_input->sampleVolume(context, *inputBuffer, inputVolume);
        int index = 0;
        for (int z = 0; z < volume.sizeZ; ++z) {
            for (int x = 0; x < volume.sizeX; ++x) {
                for (int y = 0; y < volume.sizeY; ++y) {
                    outputBuffer.set(index, inputBuffer->get(inputVolume.indexUnchecked(0, y, z)));
                    ++index;
                }
            }
        }
    }
    float sampleValue(SamplerContext& context, int, int blockY, int blockZ) const override {
        return m_input->sampleValue(context, m_x, blockY, blockZ);
    }
private:
    DensitySamplerPtr m_input;
    int m_x;
};

class YSampler final : public DensitySampler {
public:
    YSampler(DensitySamplerPtr input, int y) : m_input(std::move(input)), m_y(y) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if (volume.sizeY == 1 && volume.minBlockY == m_y) {
            m_input->sampleVolume(context, outputBuffer, volume);
            return;
        }
        const DensityVolume inputVolume(volume.sizeX, 1, volume.sizeZ, volume.minBlockX, m_y, volume.minBlockZ,
                                        volume.stepBlockX, volume.stepBlockY, volume.stepBlockZ);
        ScopedBuffer inputBuffer = context.acquireBuffer(inputVolume);
        m_input->sampleVolume(context, *inputBuffer, inputVolume);
        for (int z = 0; z < volume.sizeZ; ++z) {
            for (int x = 0; x < volume.sizeX; ++x) {
                const float input = inputBuffer->get(inputVolume.indexUnchecked(x, 0, z));
                outputBuffer.setRange(volume.indexUnchecked(x, 0, z), volume.sizeY, input);
            }
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int, int blockZ) const override {
        return m_input->sampleValue(context, blockX, m_y, blockZ);
    }
private:
    DensitySamplerPtr m_input;
    int m_y;
};

class ZSampler final : public DensitySampler {
public:
    ZSampler(DensitySamplerPtr input, int z) : m_input(std::move(input)), m_z(z) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if (volume.sizeZ == 1 && volume.minBlockZ == m_z) {
            m_input->sampleVolume(context, outputBuffer, volume);
            return;
        }
        const DensityVolume inputVolume(volume.sizeX, volume.sizeY, 1, volume.minBlockX, volume.minBlockY, m_z,
                                        volume.stepBlockX, volume.stepBlockY, volume.stepBlockZ);
        ScopedBuffer inputBuffer = context.acquireBuffer(inputVolume);
        m_input->sampleVolume(context, *inputBuffer, inputVolume);
        int index = 0;
        for (int z = 0; z < volume.sizeZ; ++z) {
            for (int x = 0; x < volume.sizeX; ++x) {
                for (int y = 0; y < volume.sizeY; ++y) {
                    outputBuffer.set(index, inputBuffer->get(inputVolume.indexUnchecked(x, y, 0)));
                    ++index;
                }
            }
        }
    }
    float sampleValue(SamplerContext& context, int blockX, int blockY, int) const override {
        return m_input->sampleValue(context, blockX, blockY, m_z);
    }
private:
    DensitySamplerPtr m_input;
    int m_z;
};

} // namespace

DensitySamplerPtr SliceFunction::ySampler(DensitySamplerPtr input, int y) {
    return std::make_shared<YSampler>(std::move(input), y);
}

DensitySamplerPtr SliceFunction::compileSampler(CompileContext& context) const {
    if (const auto* inner = dynamic_cast<const SliceFunction*>(m_input.get())) {
        const Axis innerAxis = inner->axis();
        if ((m_axis == Axis::X && innerAxis == Axis::Z) || (m_axis == Axis::Z && innerAxis == Axis::X)) {
            const int x = m_axis == Axis::X ? m_coordinate : inner->coordinate();
            const int z = m_axis == Axis::X ? inner->coordinate() : m_coordinate;
            return std::make_shared<XzSampler>(inner->input()->compileSampler(context), x, z);
        }
    }
    DensitySamplerPtr input = m_input->compileSampler(context);
    switch (m_axis) {
        case Axis::X: return std::make_shared<XSampler>(input, m_coordinate);
        case Axis::Y: return std::make_shared<YSampler>(input, m_coordinate);
        case Axis::Z: return std::make_shared<ZSampler>(input, m_coordinate);
    }
    throw std::logic_error("SliceFunction: bad axis");
}

DensityFunctionPtr SliceFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr input = rule.rewrite(m_input);
    return input == m_input ? self() : std::make_shared<SliceFunction>(m_axis, m_coordinate, input);
}

bool SliceFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const SliceFunction*>(&other);
    return o != nullptr && m_axis == o->m_axis && m_coordinate == o->m_coordinate &&
           functionsEqual(m_input, o->m_input);
}

size_t SliceFunction::hash() const {
    size_t h = hashCombine(0x14, static_cast<size_t>(m_axis));
    h = hashCombine(h, std::hash<int>()(m_coordinate));
    return hashCombine(h, m_input->hash());
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
