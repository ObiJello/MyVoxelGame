#include "levelgen/density/generator/SimpleDensityFunction.h"

#include "levelgen/density/ContextKeys.h"
#include "levelgen/density/CoreFunctions.h"

#include <stdexcept>

// Reference: generator.SimpleDensityFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

DensityFunctionPtr SimpleDensityFunction::blendAlpha() {
    static const DensityFunctionPtr kInstance = std::make_shared<SimpleDensityFunction>(Kind::BLEND_ALPHA);
    return kInstance;
}

DensityFunctionPtr SimpleDensityFunction::blendOffset() {
    static const DensityFunctionPtr kInstance = std::make_shared<SimpleDensityFunction>(Kind::BLEND_OFFSET);
    return kInstance;
}

DensityFunctionPtr SimpleDensityFunction::beardifier() {
    static const DensityFunctionPtr kInstance = std::make_shared<SimpleDensityFunction>(Kind::BEARDIFIER);
    return kInstance;
}

DensityFunctionPtr SimpleDensityFunction::of(Kind kind) {
    switch (kind) {
        case Kind::BLEND_ALPHA: return blendAlpha();
        case Kind::BLEND_OFFSET: return blendOffset();
        case Kind::BEARDIFIER: return beardifier();
    }
    throw std::logic_error("SimpleDensityFunction: bad kind");
}

DensitySamplerPtr SimpleDensityFunction::compileSampler(CompileContext&) const {
    switch (m_kind) {
        case Kind::BLEND_ALPHA:
            return std::make_shared<ContextBoundSampler>(blendAlphaKey(), std::make_shared<ConstantFunction::Sampler>(1.0f));
        case Kind::BLEND_OFFSET:
            return std::make_shared<ContextBoundSampler>(blendOffsetKey(), std::make_shared<ConstantFunction::Sampler>(0.0f));
        case Kind::BEARDIFIER:
            return std::make_shared<ContextBoundSampler>(beardifierKey(), std::make_shared<ConstantFunction::Sampler>(0.0f));
    }
    throw std::logic_error("SimpleDensityFunction: bad kind");
}

Interval SimpleDensityFunction::range() const {
    switch (m_kind) {
        case Kind::BLEND_ALPHA: return Interval::of(0.0f, 1.0f);
        case Kind::BLEND_OFFSET: return Interval::infinite();
        case Kind::BEARDIFIER: return beardifierRange();
    }
    throw std::logic_error("SimpleDensityFunction: bad kind");
}

int SimpleDensityFunction::domainAxes() const {
    switch (m_kind) {
        case Kind::BLEND_ALPHA:
        case Kind::BLEND_OFFSET:
            return 5;
        case Kind::BEARDIFIER:
            return 7;
    }
    throw std::logic_error("SimpleDensityFunction: bad kind");
}

const char* SimpleDensityFunction::id() const {
    switch (m_kind) {
        case Kind::BLEND_ALPHA: return "blend_alpha";
        case Kind::BLEND_OFFSET: return "blend_offset";
        case Kind::BEARDIFIER: return "beardifier";
    }
    throw std::logic_error("SimpleDensityFunction: bad kind");
}

bool SimpleDensityFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const SimpleDensityFunction*>(&other);
    return o != nullptr && m_kind == o->m_kind;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
