#pragma once

#include "levelgen/DensityFunction.h"

namespace minecraft {

/**
 * Blender - Handles blending between old and new terrain chunks
 *
 * This is a stub implementation that returns an "empty" blender.
 * For now, we're focusing on new terrain generation without blending.
 *
 * Full implementation would handle:
 * - Blending density values at chunk boundaries
 * - Blending biomes at boundaries
 * - Handling old vs new chunk formats
 *
 * Reference: net/minecraft/world/level/levelgen/blending/Blender.java
 */
class Blender {
public:
    virtual ~Blender() = default;

    // Returns an empty blender that doesn't modify any values
    static Blender* empty();

    // Check if this is an empty blender
    virtual bool isEmpty() const { return true; }

    /**
     * Blend density value at a given position.
     * For empty blender: just returns noiseValue unchanged
     * Reference: Blender.java lines 41-43 (empty blender)
     */
    virtual double blendDensity(const density::DensityFunction::FunctionContext& context, double noiseValue) const {
        return noiseValue;  // Empty blender doesn't modify density
    }

    /**
     * Get blending alpha and offset at a given position.
     * Reference: Blender.java line 37-38 (empty blender), line 375 (BlendingOutput record)
     *
     * Java BlendingOutput record:
     * public static record BlendingOutput(double alpha, double blendingOffset)
     *
     * For empty blender: alpha = 1.0, blendingOffset = 0.0
     *
     * @param blockX - Block X coordinate
     * @param blockZ - Block Z coordinate
     * @param outAlpha - Output alpha value (1.0 = full new terrain, 0.0 = full old terrain)
     * @param outOffset - Output blending offset
     */
    virtual void blendOffsetAndFactor(int blockX, int blockZ, double& outAlpha, double& outOffset) const {
        // Empty blender returns default values
        outAlpha = 1.0;
        outOffset = 0.0;
    }

private:
    // Singleton empty blender instance
    static Blender* s_emptyBlender;
};

} // namespace minecraft
