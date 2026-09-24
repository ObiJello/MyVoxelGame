#pragma once

// Reference: levelgen.blending.Blender (26.3) - the part of the Blender the
// density functions call through SamplerContext (blenderKey(), Java's
// Blender.CONTEXT_KEY). Implemented by the terrain generator's blender; the
// density engine only sees this interface.

namespace minecraft {
namespace levelgen {
namespace density {

class Blender {
public:
    virtual ~Blender() = default;

    // Blender.isEmpty(): true for Blender.empty() (no old chunks around).
    virtual bool isEmpty() const = 0;
    // Blender.blendDensity(blockX, blockY, blockZ, noiseValue).
    virtual float blendDensity(int blockX, int blockY, int blockZ, float noiseValue) const = 0;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
