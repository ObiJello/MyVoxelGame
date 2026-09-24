#include "levelgen/density/op/LerpFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/JavaMath.h"

// Reference: op.LerpFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

DensitySamplerPtr LerpFunction::compileSampler(CompileContext& context) const {
    // All three compile first, in Java's order, even when a constant side goes unused.
    DensitySamplerPtr alpha = m_alpha->compileSampler(context);
    DensitySamplerPtr first = m_first->compileSampler(context);
    DensitySamplerPtr second = m_second->compileSampler(context);
    if (const ConstantFunction* constantFirst = asConstant(m_first)) {
        const float firstValue = constantFirst->value();
        return std::make_shared<ConstFirstSampler>(alpha, firstValue, second);
    }
    if (const ConstantFunction* constantSecond = asConstant(m_second)) {
        const float secondValue = constantSecond->value();
        return std::make_shared<ConstSecondSampler>(alpha, first, secondValue);
    }
    return std::make_shared<Sampler>(alpha, first, second);
}

DensityFunctionPtr LerpFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr alpha = rule.rewrite(m_alpha);
    DensityFunctionPtr first = rule.rewrite(m_first);
    DensityFunctionPtr second = rule.rewrite(m_second);
    return alpha == m_alpha && first == m_first && second == m_second
        ? self()
        : std::make_shared<LerpFunction>(alpha, first, second);
}

Interval LerpFunction::range() const {
    return Interval::lerp(m_alpha->range(), m_first->range(), m_second->range());
}

int LerpFunction::domainAxes() const {
    return m_alpha->domainAxes() | m_first->domainAxes() | m_second->domainAxes();
}

bool LerpFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const LerpFunction*>(&other);
    return o != nullptr && functionsEqual(m_alpha, o->m_alpha) && functionsEqual(m_first, o->m_first) &&
           functionsEqual(m_second, o->m_second);
}

size_t LerpFunction::hash() const {
    size_t h = hashCombine(0x31, m_alpha->hash());
    h = hashCombine(h, m_first->hash());
    return hashCombine(h, m_second->hash());
}

// ---- ConstFirstSampler -------------------------------------------------------

void LerpFunction::ConstFirstSampler::sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                                                   const DensityVolume& volume) const {
    m_alpha->sampleVolume(context, outputBuffer, volume);
    ScopedBuffer secondBuffer = context.acquireBuffer(volume);
    m_second->sampleVolume(context, *secondBuffer, volume);
    for (int i = 0; i < outputBuffer.size(); ++i) {
        const float alpha = outputBuffer.get(i);
        const float second = secondBuffer->get(i);
        if (alpha == 0.0f) {
            outputBuffer.set(i, m_first);
        } else if (alpha == 1.0f) {
            outputBuffer.set(i, second);
        } else {
            outputBuffer.set(i, jmath::lerp(alpha, m_first, second));
        }
    }
}

float LerpFunction::ConstFirstSampler::sampleValue(SamplerContext& context, int blockX, int blockY,
                                                   int blockZ) const {
    const float alpha = m_alpha->sampleValue(context, blockX, blockY, blockZ);
    if (alpha == 0.0f) {
        return m_first;
    }
    return alpha == 1.0f ? m_second->sampleValue(context, blockX, blockY, blockZ)
                         : jmath::lerp(alpha, m_first, m_second->sampleValue(context, blockX, blockY, blockZ));
}

// ---- ConstSecondSampler ------------------------------------------------------

void LerpFunction::ConstSecondSampler::sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                                                    const DensityVolume& volume) const {
    m_alpha->sampleVolume(context, outputBuffer, volume);
    ScopedBuffer firstBuffer = context.acquireBuffer(volume);
    m_first->sampleVolume(context, *firstBuffer, volume);
    for (int i = 0; i < outputBuffer.size(); ++i) {
        const float alpha = outputBuffer.get(i);
        const float first = firstBuffer->get(i);
        if (alpha == 0.0f) {
            outputBuffer.set(i, first);
        } else if (alpha == 1.0f) {
            outputBuffer.set(i, m_second);
        } else {
            outputBuffer.set(i, jmath::lerp(alpha, first, m_second));
        }
    }
}

float LerpFunction::ConstSecondSampler::sampleValue(SamplerContext& context, int blockX, int blockY,
                                                    int blockZ) const {
    const float alpha = m_alpha->sampleValue(context, blockX, blockY, blockZ);
    if (alpha == 0.0f) {
        return m_first->sampleValue(context, blockX, blockY, blockZ);
    }
    return alpha == 1.0f ? m_second
                         : jmath::lerp(alpha, m_first->sampleValue(context, blockX, blockY, blockZ), m_second);
}

// ---- Sampler -----------------------------------------------------------------

void LerpFunction::Sampler::sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer,
                                         const DensityVolume& volume) const {
    m_alpha->sampleVolume(context, outputBuffer, volume);
    ScopedBuffer firstBuffer = context.acquireBuffer(volume);
    m_first->sampleVolume(context, *firstBuffer, volume);
    ScopedBuffer secondBuffer = context.acquireBuffer(volume);
    m_second->sampleVolume(context, *secondBuffer, volume);
    for (int i = 0; i < outputBuffer.size(); ++i) {
        const float alpha = outputBuffer.get(i);
        const float first = firstBuffer->get(i);
        const float second = secondBuffer->get(i);
        if (alpha == 0.0f) {
            outputBuffer.set(i, first);
        } else if (alpha == 1.0f) {
            outputBuffer.set(i, second);
        } else {
            outputBuffer.set(i, jmath::lerp(alpha, first, second));
        }
    }
}

float LerpFunction::Sampler::sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const {
    const float alpha = m_alpha->sampleValue(context, blockX, blockY, blockZ);
    if (alpha == 0.0f) {
        return m_first->sampleValue(context, blockX, blockY, blockZ);
    }
    if (alpha == 1.0f) {
        return m_second->sampleValue(context, blockX, blockY, blockZ);
    }
    // Java evaluates the lerp arguments left to right: first, then second.
    const float first = m_first->sampleValue(context, blockX, blockY, blockZ);
    const float second = m_second->sampleValue(context, blockX, blockY, blockZ);
    return jmath::lerp(alpha, first, second);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
