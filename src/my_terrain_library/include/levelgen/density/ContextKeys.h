#pragma once

#include "levelgen/density/DensitySampler.h"
#include "levelgen/density/Interval.h"
#include "levelgen/density/SamplerContext.h"

// The SamplerContext user fields the 26.3 engine defines, keyed by identity
// like Java's ContextKey.vanilla(...):
//   Beardifier.CONTEXT_KEY   "beardifier"      the structure beardifier (a sampler)
//   Blender.ALPHA_KEY        "blender/alpha"   blending alpha sampler
//   Blender.OFFSET_KEY       "blender/offset"  blending offset sampler
//   Blender.CONTEXT_KEY      "blender"         the Blender itself

namespace minecraft {
namespace levelgen {
namespace density {

class Blender;

inline const ContextKey<DensitySampler>& beardifierKey() {
    static const ContextKey<DensitySampler> key("beardifier");
    return key;
}
inline const ContextKey<DensitySampler>& blendAlphaKey() {
    static const ContextKey<DensitySampler> key("blender/alpha");
    return key;
}
inline const ContextKey<DensitySampler>& blendOffsetKey() {
    static const ContextKey<DensitySampler> key("blender/offset");
    return key;
}
inline const ContextKey<Blender>& blenderKey() {
    static const ContextKey<Blender> key("blender");
    return key;
}

// Beardifier.RANGE.
inline Interval beardifierRange() { return Interval::infinite(); }

} // namespace density
} // namespace levelgen
} // namespace minecraft
