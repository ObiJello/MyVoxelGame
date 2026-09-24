#pragma once

#include "levelgen/density/DensitySampler.h"
#include "levelgen/density/Interval.h"
#include "levelgen/density/synth/Noise.h"
#include "random/AnyPositionalRandomFactory.h"

#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Reference: levelgen.densityfunction.DensityFunction (26.3).
//
// A density function is an immutable expression tree (Java records). It is
// never evaluated directly: DensityFunctionCompiler rewrites it (inlining
// registry references, deduplicating caches, slicing away unused axes) and
// compiles it into a DensitySampler. Two functions that are EQUAL as records
// share a sampler and, for `cache`, a cache cell — and a shared cache can
// answer a point query from a volume it filled — so equals()/hash() must be
// Java's structural record equality, not pointer identity.

namespace minecraft {
namespace levelgen {
namespace density {

namespace synth {
class NormalNoise;
}

class DensityFunction;
using DensityFunctionPtr = std::shared_ptr<const DensityFunction>;

// DensityFunction.Axes: which coordinates a function depends on.
enum Axes : int {
    NO_AXES = 0,
    AXIS_X = 1,
    AXIS_Y = 2,
    AXIS_Z = 4,
    ALL_AXES = 7,
};

enum class Axis { X, Y, Z };
inline int axesFrom(Axis axis) {
    switch (axis) {
        case Axis::X: return AXIS_X;
        case Axis::Y: return AXIS_Y;
        case Axis::Z: return AXIS_Z;
    }
    return NO_AXES;
}

// Holder<NormalNoise>. A registry holder (a key) compares by identity like
// Java's Holder.Reference; a direct holder compares its NormalNoise.
struct NoiseHolder {
    std::string key;                                   // "minecraft:ridge"; empty for a direct holder
    std::shared_ptr<const synth::NormalNoise> value;   // the definition (never null)

    bool isReference() const { return !key.empty(); }
    bool is(const std::string& otherKey) const { return key == otherKey; }
    bool operator==(const NoiseHolder& other) const;
    size_t hash() const;
};

class CompileContext {
public:
    virtual ~CompileContext() = default;
    virtual synth::NoisePtr createNoiseSampler(const NoiseHolder& parameters) = 0;
    virtual random::AnyRandomSource createRandom(const std::string& seedId) = 0;
    // Deprecated in 26.3 but still what EndIslandFunction uses.
    virtual random::AnyRandomSource createEndIslandRandom() = 0;
};

class DfRewriteRule;

class DensityFunction : public std::enable_shared_from_this<DensityFunction> {
public:
    virtual ~DensityFunction() = default;

    virtual DensitySamplerPtr compileSampler(CompileContext& context) const = 0;
    // Returns this function (shared_from_this) when no child changed.
    virtual DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const = 0;
    virtual Interval range() const = 0;
    virtual int domainAxes() const = 0;
    // The codec's registry id ("minecraft:add"), for diagnostics.
    virtual std::string typeId() const = 0;

    // Java record equals()/hashCode(): same concrete type, equal components
    // (child functions compared with equals, recursively).
    virtual bool equals(const DensityFunction& other) const = 0;
    virtual size_t hash() const = 0;

    DensityFunctionPtr self() const { return shared_from_this(); }
};

// Structural equality/hash for containers keyed by function.
struct DensityFunctionEquals {
    bool operator()(const DensityFunctionPtr& a, const DensityFunctionPtr& b) const {
        return a == b || (a && b && a->equals(*b));
    }
};
struct DensityFunctionHash {
    size_t operator()(const DensityFunctionPtr& f) const { return f ? f->hash() : 0; }
};

inline bool functionsEqual(const DensityFunctionPtr& a, const DensityFunctionPtr& b) {
    return DensityFunctionEquals()(a, b);
}

// Java record equality for float/double components: Float.compare == 0
// (NaN equals NaN, -0.0 differs from +0.0).
inline bool floatEq(float a, float b) {
    if (a != a) return b != b;
    return a == b && std::signbit(a) == std::signbit(b);
}
inline bool doubleEq(double a, double b) {
    if (a != a) return b != b;
    return a == b && std::signbit(a) == std::signbit(b);
}

// hash_combine for record hashes.
inline size_t hashCombine(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}
inline size_t hashFloat(float v) { return std::hash<float>()(v); }
inline size_t hashDouble(double v) { return std::hash<double>()(v); }

} // namespace density
} // namespace levelgen
} // namespace minecraft
