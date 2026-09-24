#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/JavaMath.h"
#include "levelgen/density/SamplerContext.h"

#include <string>

// Reference: op.BinaryFunction (26.3) - add, sub, mul, div, min, max.
// compileSampler picks a specialised sampler: a constant operand folds into a
// Const* sampler, min/max of inputs whose ranges never overlap collapse to one
// side, and min/max skip the right input when the left already decides. Each
// specialisation rounds its own way (x - c becomes x + (-c), x / c becomes
// x * (1 / c)), so all of them are kept.

namespace minecraft {
namespace levelgen {
namespace density {

class BinaryFunction final : public DensityFunction {
public:
    enum class Type {
        ADD,
        SUB,
        MUL,
        DIV,
        MIN,
        MAX,
    };
    static const char* typeName(Type type);

    BinaryFunction(Type type, DensityFunctionPtr left, DensityFunctionPtr right)
        : m_type(type), m_left(std::move(left)), m_right(std::move(right)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override { return m_left->domainAxes() | m_right->domainAxes(); }
    std::string typeId() const override { return std::string("minecraft:") + typeName(m_type); }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    Type type() const { return m_type; }
    const DensityFunctionPtr& left() const { return m_left; }
    const DensityFunctionPtr& right() const { return m_right; }

    // ---- samplers ----------------------------------------------------------------

    class ConstAddSampler final : public DensitySampler {
    public:
        ConstAddSampler(DensitySamplerPtr left, float right) : m_left(std::move(left)), m_right(right) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.addTo(i, m_right);
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left->sampleValue(context, blockX, blockY, blockZ) + m_right;
        }
        const DensitySamplerPtr& left() const { return m_left; }
        float right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        float m_right;
    };

    class AddSampler final : public DensitySampler {
    public:
        AddSampler(DensitySamplerPtr left, DensitySamplerPtr right) : m_left(std::move(left)), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.addTo(i, rightBuffer->get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left->sampleValue(context, blockX, blockY, blockZ) + m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
    };

    class ConstSubSampler final : public DensitySampler {
    public:
        ConstSubSampler(float left, DensitySamplerPtr right) : m_left(left), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_right->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, m_left - outputBuffer.get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left - m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        float left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        float m_left;
        DensitySamplerPtr m_right;
    };

    class SubSampler final : public DensitySampler {
    public:
        SubSampler(DensitySamplerPtr left, DensitySamplerPtr right) : m_left(std::move(left)), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.addTo(i, -rightBuffer->get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left->sampleValue(context, blockX, blockY, blockZ) - m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
    };

    class ConstMulSampler final : public DensitySampler {
    public:
        ConstMulSampler(DensitySamplerPtr left, float right) : m_left(std::move(left)), m_right(right) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, outputBuffer.get(i) * m_right);
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left->sampleValue(context, blockX, blockY, blockZ) * m_right;
        }
        const DensitySamplerPtr& left() const { return m_left; }
        float right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        float m_right;
    };

    class MulSampler final : public DensitySampler {
    public:
        MulSampler(DensitySamplerPtr left, DensitySamplerPtr right) : m_left(std::move(left)), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, outputBuffer.get(i) * rightBuffer->get(i));
            }
        }
        // The point path skips the right input when the left is zero (the
        // volume path does not: 0 * inf/NaN differs).
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            const float left = m_left->sampleValue(context, blockX, blockY, blockZ);
            return left == 0.0f ? 0.0f : left * m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
    };

    class ConstDivSampler final : public DensitySampler {
    public:
        ConstDivSampler(float left, DensitySamplerPtr right) : m_left(left), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_right->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, m_left / outputBuffer.get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return m_left / m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        float left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        float m_left;
        DensitySamplerPtr m_right;
    };

    class DivSampler final : public DensitySampler {
    public:
        DivSampler(DensitySamplerPtr left, DensitySamplerPtr right) : m_left(std::move(left)), m_right(std::move(right)) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                outputBuffer.set(i, outputBuffer.get(i) / rightBuffer->get(i));
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            const float left = m_left->sampleValue(context, blockX, blockY, blockZ);
            return left == 0.0f ? 0.0f : left / m_right->sampleValue(context, blockX, blockY, blockZ);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
    };

    class ConstMinSampler final : public DensitySampler {
    public:
        ConstMinSampler(DensitySamplerPtr left, float right) : m_left(std::move(left)), m_right(right) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                if (m_right < outputBuffer.get(i)) {
                    outputBuffer.set(i, m_right);
                }
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::fmin(m_left->sampleValue(context, blockX, blockY, blockZ), m_right);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        float right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        float m_right;
    };

    class MinSampler final : public DensitySampler {
    public:
        MinSampler(DensitySamplerPtr left, DensitySamplerPtr right, float rightMinValue)
            : m_left(std::move(left)), m_right(std::move(right)), m_rightMinValue(rightMinValue) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                const float rightValue = rightBuffer->get(i);
                if (rightValue < outputBuffer.get(i)) {
                    outputBuffer.set(i, rightValue);
                }
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            const float left = m_left->sampleValue(context, blockX, blockY, blockZ);
            return left <= m_rightMinValue ? left
                                           : jmath::fmin(left, m_right->sampleValue(context, blockX, blockY, blockZ));
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
        float rightMinValue() const { return m_rightMinValue; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
        float m_rightMinValue;
    };

    class ConstMaxSampler final : public DensitySampler {
    public:
        ConstMaxSampler(DensitySamplerPtr left, float right) : m_left(std::move(left)), m_right(right) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                if (m_right > outputBuffer.get(i)) {
                    outputBuffer.set(i, m_right);
                }
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            return jmath::fmax(m_left->sampleValue(context, blockX, blockY, blockZ), m_right);
        }
        const DensitySamplerPtr& left() const { return m_left; }
        float right() const { return m_right; }
    private:
        DensitySamplerPtr m_left;
        float m_right;
    };

    class MaxSampler final : public DensitySampler {
    public:
        MaxSampler(DensitySamplerPtr left, DensitySamplerPtr right, float rightMaxValue)
            : m_left(std::move(left)), m_right(std::move(right)), m_rightMaxValue(rightMaxValue) {}
        void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
            m_left->sampleVolume(context, outputBuffer, volume);
            ScopedBuffer rightBuffer = context.acquireBuffer(volume);
            m_right->sampleVolume(context, *rightBuffer, volume);
            for (int i = 0; i < outputBuffer.size(); ++i) {
                const float rightValue = rightBuffer->get(i);
                if (rightValue > outputBuffer.get(i)) {
                    outputBuffer.set(i, rightValue);
                }
            }
        }
        float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
            const float left = m_left->sampleValue(context, blockX, blockY, blockZ);
            return left >= m_rightMaxValue ? left
                                           : jmath::fmax(left, m_right->sampleValue(context, blockX, blockY, blockZ));
        }
        const DensitySamplerPtr& left() const { return m_left; }
        const DensitySamplerPtr& right() const { return m_right; }
        float rightMaxValue() const { return m_rightMaxValue; }
    private:
        DensitySamplerPtr m_left;
        DensitySamplerPtr m_right;
        float m_rightMaxValue;
    };

private:
    void warnNonIntersecting() const;

    Type m_type;
    DensityFunctionPtr m_left;
    DensityFunctionPtr m_right;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
