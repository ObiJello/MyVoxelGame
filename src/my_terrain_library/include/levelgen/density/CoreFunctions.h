#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DfRewriteRule.h"
#include "levelgen/density/SamplerContext.h"

#include <string>

// The density functions the compiler itself depends on (26.3):
//   generator.ConstantFunction           "minecraft:constant"
//   op.CacheFunction                     "minecraft:cache"
//   op.SliceFunction                     "minecraft:slice"
//   DensityFunctions.HolderHolder        a registry reference (no codec)
// plus the samplers behind caching (CachingDensitySampler) and context
// fields (ContextBoundSampler).

namespace minecraft {
namespace levelgen {
namespace density {

// ---- generator.ConstantFunction -------------------------------------------

class ConstantFunction final : public DensityFunction {
public:
    explicit ConstantFunction(float value) : m_value(value) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule&) const override { return self(); }
    Interval range() const override { return Interval::ofExact(m_value); }
    int domainAxes() const override { return NO_AXES; }
    std::string typeId() const override { return "minecraft:constant"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x11, hashFloat(m_value)); }

    float value() const { return m_value; }

    class Sampler final : public DensitySampler {
    public:
        explicit Sampler(float value) : m_value(value) {}
        void sampleVolume(SamplerContext&, DensityBuffer& out, const DensityVolume&) const override {
            out.fill(m_value);
        }
        float sampleValue(SamplerContext&, int, int, int) const override { return m_value; }
        float value() const { return m_value; }
    private:
        float m_value;
    };

private:
    float m_value;
};

// DensityFunctions.zero(): one shared instance, as Java's ZERO.
DensityFunctionPtr zero();
DensityFunctionPtr constant(float value);
// `function instanceof ConstantFunction c` -> c, else nullptr.
const ConstantFunction* asConstant(const DensityFunctionPtr& function);

// ---- op.CacheFunction -------------------------------------------------------

class CacheFunction final : public DensityFunction {
public:
    explicit CacheFunction(DensityFunctionPtr input) : m_input(std::move(input)) {}

    // Java throws: a cache compiles only after DensityFunctionCompiler has
    // replaced it by its prepared (deduplicated) form.
    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_input->range(); }
    int domainAxes() const override { return m_input->domainAxes(); }
    std::string typeId() const override { return "minecraft:cache"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x12, m_input->hash()); }

    const DensityFunctionPtr& input() const { return m_input; }

private:
    DensityFunctionPtr m_input;
};

// ---- DensityFunctions.HolderHolder -------------------------------------------

// A reference to a registry density function ("minecraft:overworld/depth").
// Equal when it names the same registry entry (Holder.Reference identity).
class ReferenceFunction final : public DensityFunction {
public:
    ReferenceFunction(std::string key, DensityFunctionPtr target)
        : m_key(std::move(key)), m_target(std::move(target)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override {
        return m_target->compileSampler(context);
    }
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_target->range(); }
    int domainAxes() const override { return m_target->domainAxes(); }
    std::string typeId() const override { return "reference:" + m_key; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override { return hashCombine(0x13, std::hash<std::string>()(m_key)); }

    const std::string& key() const { return m_key; }
    const DensityFunctionPtr& target() const { return m_target; }

private:
    std::string m_key;
    DensityFunctionPtr m_target;
};

// ---- op.SliceFunction --------------------------------------------------------

class SliceFunction final : public DensityFunction {
public:
    SliceFunction(Axis axis, int coordinate, DensityFunctionPtr input)
        : m_axis(axis), m_coordinate(coordinate), m_input(std::move(input)) {}

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override { return m_input->range(); }
    int domainAxes() const override { return m_input->domainAxes() & ~axesFrom(m_axis); }
    std::string typeId() const override { return "minecraft:slice"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    Axis axis() const { return m_axis; }
    int coordinate() const { return m_coordinate; }
    const DensityFunctionPtr& input() const { return m_input; }

    // SliceFunction.YSampler: samples input at a fixed y (FindTopSurface wraps
    // its sampler in one too).
    static DensitySamplerPtr ySampler(DensitySamplerPtr input, int y);

private:
    Axis m_axis;
    int m_coordinate;
    DensityFunctionPtr m_input;
};

// ---- CachingDensitySampler / ContextBoundSampler ------------------------------

class CachingDensitySampler final : public DensitySampler {
public:
    CachingDensitySampler(int id, DensitySamplerPtr input) : m_id(id), m_input(std::move(input)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& out, const DensityVolume& volume) const override {
        context.sampleVolumeCached(m_id, *m_input, out, volume);
    }
    float sampleValue(SamplerContext& context, int x, int y, int z) const override {
        return context.sampleValueCached(m_id, *m_input, x, y, z);
    }
    int id() const { return m_id; }
    const DensitySamplerPtr& input() const { return m_input; }
private:
    int m_id;
    DensitySamplerPtr m_input;
};

// Samples whatever sampler the context carries under `key` (the beardifier,
// the blender's alpha/offset), else the fallback.
class ContextBoundSampler final : public DensitySampler {
public:
    ContextBoundSampler(const ContextKey<DensitySampler>& key, DensitySamplerPtr fallback)
        : m_key(&key), m_fallback(std::move(fallback)) {}
    void sampleVolume(SamplerContext& context, DensityBuffer& out, const DensityVolume& volume) const override {
        context.getFieldOrDefault(*m_key, m_fallback.get())->sampleVolume(context, out, volume);
    }
    float sampleValue(SamplerContext& context, int x, int y, int z) const override {
        return context.getFieldOrDefault(*m_key, m_fallback.get())->sampleValue(context, x, y, z);
    }
private:
    const ContextKey<DensitySampler>* m_key;
    DensitySamplerPtr m_fallback;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
