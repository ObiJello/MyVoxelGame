#pragma once

#include "NoiseRouter.h"
#include "DensityFunction.h"
#include <string>

namespace minecraft {
namespace levelgen {

/**
 * NoiseRouterData - Builds complete NoiseRouter by wiring density functions together
 *
 * Reference: net/minecraft/world/level/levelgen/NoiseRouterData.java
 *
 * This is THE wiring class that connects all density functions to create
 * the final terrain generation pipeline. It defines how continents, erosion,
 * ridges, depth, caves, and aquifers all combine into final block placement.
 */
class NoiseRouterData {
public:
    // Constants
    static constexpr float GLOBAL_OFFSET = -0.50375f;
    static constexpr float ORE_THICKNESS = 0.08f;
    static constexpr double VEININESS_FREQUENCY = 1.5;
    static constexpr double NOODLE_SPACING_AND_STRAIGHTNESS = 1.5;
    static constexpr double SURFACE_DENSITY_THRESHOLD = 1.5625;
    static constexpr double CHEESE_NOISE_TARGET = -0.703125;
    static constexpr double NOISE_ZERO = 0.390625;
    static constexpr int ISLAND_CHUNK_DISTANCE = 64;
    static constexpr long ISLAND_CHUNK_DISTANCE_SQR = 4096L;

    // Density Y anchor points
    static constexpr int DENSITY_Y_ANCHOR_BOTTOM = -64;
    static constexpr int DENSITY_Y_ANCHOR_TOP = 320;
    static constexpr double DENSITY_Y_BOTTOM = 1.5;
    static constexpr double DENSITY_Y_TOP = -1.5;
    static constexpr int OVERWORLD_BOTTOM_SLIDE_HEIGHT = 24;
    static constexpr double BASE_DENSITY_MULTIPLIER = 4.0;

    /**
     * peaksAndValleys - Transforms weirdness to create peaks/valleys
     *
     * Formula: -(abs(abs(weirdness) - 0.6666667) - 0.33333334) * 3.0
     *
     * This creates a valley at weirdness=0, peaks at ±0.66667, and back to flat at ±1.0
     *
     * From NoiseRouterData.java line 134
     */
    static float peaksAndValleys(float weirdness);

    /**
     * Build complete overworld NoiseRouter
     *
     * This is the main method that wires everything together!
     *
     * @param largeBiomes If true, use large biome noise parameters
     * @param amplified If true, use amplified terrain (extreme hills)
     * @return Complete NoiseRouter with all 15 density functions wired
     *
     * Note: Simplified version for now - full version requires registry system
     */
    static NoiseRouter* overworld(bool largeBiomes, bool amplified);

    /**
     * Build nether NoiseRouter (stubbed for now)
     */
    static NoiseRouter* nether();

    /**
     * Build end NoiseRouter (stubbed for now)
     */
    static NoiseRouter* end();

    /**
     * Build empty NoiseRouter (all zeros)
     */
    static NoiseRouter* none();

    // ========================================================================
    // Cave Generation Functions (matching Java exactly) - PUBLIC for registry
    // ========================================================================

    /**
     * spaghettiRoughnessFunction - Roughness modifier for spaghetti caves
     * Java: NoiseRouterData.spaghettiRoughnessFunction() line 138-142
     */
    static DensityFunction* spaghettiRoughnessFunction(int64_t worldSeed);

    /**
     * entrances - Cave entrance generation (big spaghetti caves)
     * Java: NoiseRouterData.entrances() line 144-154
     */
    static DensityFunction* entrances(int64_t worldSeed);

    /**
     * noodle - Noodle cave generation
     * Java: NoiseRouterData.noodle() line 156-168
     */
    static DensityFunction* noodle(int64_t worldSeed);

    /**
     * pillars - Pillar/column generation
     * Java: NoiseRouterData.pillars() line 170-178
     */
    static DensityFunction* pillars(int64_t worldSeed);

    /**
     * spaghetti2D - 2D spaghetti cave generation
     * Java: NoiseRouterData.spaghetti2D() line 180-190
     */
    static DensityFunction* spaghetti2D(int64_t worldSeed);

    /**
     * spaghetti2DThicknessModulator - Thickness modifier for 2D spaghetti
     * Helper function needed by spaghetti2D
     */
    static DensityFunction* spaghetti2DThicknessModulator(int64_t worldSeed);

    /**
     * underground - Complete underground cave system
     * Java: NoiseRouterData.underground() line 192-204
     *
     * Combines all cave types (spaghetti, noodle, entrances, pillars)
     * with cave cheese and layered caverns
     */
    static DensityFunction* underground(int64_t worldSeed, DensityFunction* slopedCheese);

    /**
     * preliminarySurfaceLevel - Preliminary surface level estimate
     * Java: NoiseRouterData.preliminarySurfaceLevel() line 302-310
     *
     * Used for aquifer placement before final terrain is known
     */
    static DensityFunction* preliminarySurfaceLevel(DensityFunction* offset, DensityFunction* factor, bool amplified);

    /**
     * caves - Cave dimension NoiseRouter
     * Java: NoiseRouterData.caves() line 271-273
     */
    static NoiseRouter* caves();

    /**
     * floatingIslands - Floating islands dimension NoiseRouter
     * Java: NoiseRouterData.floatingIslands() line 275-277
     */
    static NoiseRouter* floatingIslands();

    /**
     * peaksAndValleys - DensityFunction version
     * Java: NoiseRouterData.peaksAndValleys(DensityFunction) line 130-132
     *
     * Creates a density function that transforms weirdness to peaks and valleys
     */
    static DensityFunction* peaksAndValleys(DensityFunction* weirdness);

    /**
     * QuantizedSpaghettiRarity - Helper class for spaghetti cave rarity
     * Java: NoiseRouterData.QuantizedSpaghettiRarity line 324-346
     */
    class QuantizedSpaghettiRarity {
    public:
        /**
         * getSphaghettiRarity2D - Get 2D spaghetti cave rarity value
         * Java line 325-335
         */
        static double getSphaghettiRarity2D(double rarityFactor);

        /**
         * getSpaghettiRarity3D - Get 3D spaghetti cave rarity value
         * Java line 337-345
         */
        static double getSpaghettiRarity3D(double rarityFactor);
    };

    // ========================================================================
    // Helper Methods (public for testing/debugging)
    // ========================================================================

    /**
     * slide - Applies top and bottom slide to density
     *
     * Gradually transitions density to target values at world top/bottom
     * This prevents floating islands at build height and solid bedrock floor
     *
     * From NoiseRouterData.java line 316
     */
    static DensityFunction* slide(
        DensityFunction* caves,
        int minY,
        int height,
        int topStartY,
        int topEndY,
        double topTarget,
        int bottomStartY,
        int bottomEndY,
        double bottomTarget
    );

    /**
     * slideOverworld - Overworld-specific slide parameters
     *
     * From NoiseRouterData.java line 255
     */
    static DensityFunction* slideOverworld(bool isAmplified, DensityFunction* caves);

    /**
     * remap - Linear remapping from one range to another
     *
     * Maps [fromMin, fromMax] -> [toMin, toMax]
     *
     * From NoiseRouterData.java line 211
     */
    static DensityFunction* remap(
        DensityFunction* input,
        double fromMin,
        double fromMax,
        double toMin,
        double toMax
    );

    /**
     * offsetToDepth - Converts offset to depth
     *
     * depth = yClampedGradient(-64, 320, 1.5, -1.5) + offset
     *
     * From NoiseRouterData.java line 118
     */
    static DensityFunction* offsetToDepth(DensityFunction* offset);

    /**
     * noiseGradientDensity - Scales depth with factor
     *
     * Returns: 4.0 * quarterNegative(factor * depthWithJaggedness)
     *
     * From NoiseRouterData.java line 298
     */
    static DensityFunction* noiseGradientDensity(
        DensityFunction* factor,
        DensityFunction* depthWithJaggedness
    );

    /**
     * postProcess - Final post-processing of density
     *
     * Applies blending, interpolation, and squeeze
     *
     * From NoiseRouterData.java line 206
     */
    static DensityFunction* postProcess(DensityFunction* slide);

    /**
     * yLimitedInterpolatable - Limits function to Y range with interpolation
     *
     * Returns whenInRange between minY and maxY, otherwise whenOutOfRange
     *
     * From NoiseRouterData.java line 312
     */
    static DensityFunction* yLimitedInterpolatable(
        DensityFunction* y,
        DensityFunction* whenInRange,
        int minYInclusive,
        int maxYInclusive,
        int whenOutOfRange
    );

    /**
     * slideNetherLike - Nether-style slide (for nether and caves dimensions)
     * Java: NoiseRouterData.slideNetherLike() line 259-261
     */
    static DensityFunction* slideNetherLike(int minY, int height);

    /**
     * slideEndLike - End-style slide (for end and floating islands)
     * Java: NoiseRouterData.slideEndLike() line 263-265
     */
    static DensityFunction* slideEndLike(DensityFunction* caves, int minY, int height);

    /**
     * slideEnd - Wrapper for slideEndLike with default parameters
     * Java: NoiseRouterData.slideEnd() line 279-281
     */
    static DensityFunction* slideEnd(DensityFunction* caves);

    /**
     * noNewCaves - Creates a NoiseRouter without caves
     * Java: NoiseRouterData.noNewCaves() line 246-253
     */
    static NoiseRouter* noNewCaves(DensityFunction* slide);

    /**
     * splineWithBlending - Wraps a spline with blending support
     * Java: NoiseRouterData.splineWithBlending() line 293-296
     */
    static DensityFunction* splineWithBlending(DensityFunction* spline, DensityFunction* blendingTarget);

private:
    /**
     * getBase3DNoiseNether - Get the base 3D noise for nether
     */
    static DensityFunction* getBase3DNoiseNether();

    /**
     * getBase3DNoiseEnd - Get the base 3D noise for end
     */
    static DensityFunction* getBase3DNoiseEnd();

    /**
     * getSlopedCheeseEnd - Get sloped cheese for end dimension
     */
    static DensityFunction* getSlopedCheeseEnd();
};

} // namespace levelgen
} // namespace minecraft
