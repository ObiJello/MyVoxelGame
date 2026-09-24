#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <string>

// Reference: op.UnaryFunction (26.3) - abs, square, cube, sqrt,
// half_negative, quarter_negative, reciprocal, negate, squeeze, log, sign.
// Its samplers are public (PowFunction reuses Sqrt/Square/Cube/Reciprocal).

namespace minecraft {
namespace levelgen {
namespace density {

class UnaryFunction final : public DensityFunction {
public:
    enum class Type {
        ABS,
        SQUARE,
        CUBE,
        SQRT,
        HALF_NEGATIVE,
        QUARTER_NEGATIVE,
        RECIPROCAL,
        NEGATE,
        SQUEEZE,
        LOG,
        SIGN,
    };
    static const char* typeName(Type type);

    UnaryFunction(Type type, DensityFunctionPtr input) : m_type(type), m_input(std::move(input)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override { return m_input->domainAxes(); }
    std::string typeId() const override { return std::string("minecraft:") + typeName(m_type); }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    Type type() const { return m_type; }
    const DensityFunctionPtr& input() const { return m_input; }

    // Samplers: the volume path transforms the input buffer in place.
    class UnarySampler : public DensitySampler {
    public:
        explicit UnarySampler(DensitySamplerPtr input) : m_input(std::move(input)) {}
        const DensitySamplerPtr& input() const { return m_input; }
    protected:
        DensitySamplerPtr m_input;
    };

    class AbsSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, std::fabs(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return std::fabs(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

    class SquareSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, jmath::square(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::square(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

    class CubeSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, jmath::cube(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::cube(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

    class SqrtSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, jmath::sqrt(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::sqrt(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

    class LeakyReLUSampler final : public UnarySampler {
    public:
        LeakyReLUSampler(DensitySamplerPtr input, float negativeFactor)
            : UnarySampler(std::move(input)), m_negativeFactor(negativeFactor) {}
        static float apply(float negativeFactor, float input) {
            return input > 0.0f ? input : input * negativeFactor;
        }
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, apply(m_negativeFactor, outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return apply(m_negativeFactor, m_input->sampleValue(context, blockX, blockY, blockZ));
        }
        float negativeFactor() const { return m_negativeFactor; }
    private:
        float m_negativeFactor;
    };

    class ReciprocalSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, 1.0f / outputBuffer.get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return 1.0f / m_input->sampleValue(context, blockX, blockY, blockZ);
        }
    };

    class NegateSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, -outputBuffer.get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return -m_input->sampleValue(context, blockX, blockY, blockZ);
        }
    };

    class SqueezeSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        static float apply(float input) {
            const float clampedInput = jmath::clamp(input, -1.0f, 1.0f);
            return clampedInput / 2.0f - jmath::cube(clampedInput) / 24.0f;
        }
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, apply(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return apply(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

    class LogSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, static_cast<float>(std::log(static_cast<double>(outputBuffer.get(i)))));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return static_cast<float>(std::log(static_cast<double>(m_input->sampleValue(context, blockX, blockY, blockZ))));
        }
    };

    class SignSampler final : public UnarySampler {
    public:
        using UnarySampler::UnarySampler;
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_input->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, jmath::signum(outputBuffer.get(i)));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::signum(m_input->sampleValue(context, blockX, blockY, blockZ));
        }
    };

private:
    Type m_type;
    DensityFunctionPtr m_input;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
