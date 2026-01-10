#pragma once

#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctions.h"
#include "util/CubicSpline.h"

namespace minecraft {
namespace levelgen {

using util::CubicSpline;
using util::BoundedFloatFunction;

/**
 * TerrainProvider - Builds terrain shaping splines for overworld generation
 *
 * Reference: net/minecraft/data/worldgen/TerrainProvider.java
 *
 * This class creates the complex nested splines that control terrain height,
 * shape, and variation. The main splines are:
 * - offset: Base terrain height offset
 * - factor: Controls terrain amplitude/steepness
 * - jaggedness: Controls terrain surface roughness
 */
class TerrainProvider {
public:
    // ========================================================================
    // Type aliases for spline system (for backward compatibility)
    // ========================================================================
    using Point = density::DensityFunctions::Spline::Point;
    using Coordinate = density::DensityFunctions::Spline::Coordinate;
    using SplineType = density::DensityFunctions::Spline::SplineType;

    // ========================================================================
    // Constants from TerrainProvider.java (lines 9-12)
    // ========================================================================
    static constexpr float DEEP_OCEAN_CONTINENTALNESS = -0.51f;
    static constexpr float OCEAN_CONTINENTALNESS = -0.4f;
    static constexpr float PLAINS_CONTINENTALNESS = 0.1f;
    static constexpr float BEACH_CONTINENTALNESS = -0.15f;

    // ========================================================================
    // Transformers from TerrainProvider.java (lines 13-16, 176-180)
    // ========================================================================
    static BoundedFloatFunction<float>* NO_TRANSFORM();
    static BoundedFloatFunction<float>* AMPLIFIED_OFFSET();
    static BoundedFloatFunction<float>* AMPLIFIED_FACTOR();
    static BoundedFloatFunction<float>* AMPLIFIED_JAGGEDNESS();

    // ========================================================================
    // Helper function from NoiseRouterData.java
    // ========================================================================
    static float peaksAndValleys(float weirdness);

    // ========================================================================
    // Public API - Convenience functions using Spline types
    // These are the most commonly used entry points
    // ========================================================================

    /**
     * Build the overworld offset spline (convenience version)
     * Reference: TerrainProvider.java overworldOffset() lines 18-25
     */
    static SplineType* overworldOffset(
        Coordinate* continents,
        Coordinate* erosion,
        Coordinate* ridges,
        bool amplified);

    /**
     * Build the overworld factor spline (convenience version)
     * Reference: TerrainProvider.java overworldFactor() lines 27-30
     */
    static SplineType* overworldFactor(
        Coordinate* continents,
        Coordinate* erosion,
        Coordinate* weirdness,
        Coordinate* ridges,
        bool amplified);

    /**
     * Build the overworld jaggedness spline (convenience version)
     * Reference: TerrainProvider.java overworldJaggedness() lines 32-36
     */
    static SplineType* overworldJaggedness(
        Coordinate* continents,
        Coordinate* erosion,
        Coordinate* weirdness,
        Coordinate* ridges,
        bool amplified);

    // ========================================================================
    // Public API - Main spline generators (templated versions)
    // Reference: TerrainProvider.java lines 18-36
    // ========================================================================

    /**
     * Build the overworld offset spline (template version)
     * Reference: TerrainProvider.java overworldOffset() lines 18-25
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* overworldOffsetT(
        I* continents,
        I* erosion,
        I* ridges,
        bool amplified);

    /**
     * Build the overworld factor spline (template version)
     * Reference: TerrainProvider.java overworldFactor() lines 27-30
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* overworldFactorT(
        I* continents,
        I* erosion,
        I* weirdness,
        I* ridges,
        bool amplified);

    /**
     * Build the overworld jaggedness spline (template version)
     * Reference: TerrainProvider.java overworldJaggedness() lines 32-36
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* overworldJaggednessT(
        I* continents,
        I* erosion,
        I* weirdness,
        I* ridges,
        bool amplified);

    /**
     * Build erosion-based offset spline (template version)
     * Reference: TerrainProvider.java buildErosionOffsetSpline() lines 148-168
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* buildErosionOffsetSplineT(
        I* erosion,
        I* ridges,
        float lowValley,
        float hill,
        float tallHill,
        float mountainFactor,
        float plain,
        float swamp,
        bool includeExtremeHills,
        bool saddle,
        BoundedFloatFunction<float>* offsetTransformer);

private:
    // ========================================================================
    // Private helper functions (templated)
    // ========================================================================

    /**
     * Build erosion jaggedness spline
     * Reference: TerrainProvider.java buildErosionJaggednessSpline() lines 38-43
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* buildErosionJaggednessSpline(
        I* erosion,
        I* weirdness,
        I* ridges,
        float jaggednessFactorAtPeakRidgeAndErosionIndex0,
        float jaggednessFactorAtPeakRidgeAndErosionIndex1,
        float jaggednessFactorAtHighRidgeAndErosionIndex0,
        float jaggednessFactorAtHighRidgeAndErosionIndex1,
        BoundedFloatFunction<float>* jaggednessTransformer);

    /**
     * Build ridge jaggedness spline
     * Reference: TerrainProvider.java buildRidgeJaggednessSpline() lines 45-64
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* buildRidgeJaggednessSpline(
        I* weirdness,
        I* ridges,
        float jaggednessFactorAtPeakRidge,
        float jaggednessFactorAtHighRidge,
        BoundedFloatFunction<float>* jaggednessTransformer);

    /**
     * Build weirdness jaggedness spline
     * Reference: TerrainProvider.java buildWeirdnessJaggednessSpline() lines 66-70
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* buildWeirdnessJaggednessSpline(
        I* weirdness,
        float jaggednessFactor,
        BoundedFloatFunction<float>* jaggednessTransformer);

    /**
     * Get erosion factor spline
     * Reference: TerrainProvider.java getErosionFactor() lines 72-86
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* getErosionFactor(
        I* erosion,
        I* weirdness,
        I* ridges,
        float baseValue,
        bool shatteredTerrain,
        BoundedFloatFunction<float>* factorTransformer);

    /**
     * Calculate slope between two points
     * Reference: TerrainProvider.java calculateSlope() lines 88-90
     */
    static float calculateSlope(float y1, float y2, float x1, float x2);

    /**
     * Build mountain ridge spline with points
     * Reference: TerrainProvider.java buildMountainRidgeSplineWithPoints() lines 92-128
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* buildMountainRidgeSplineWithPoints(
        I* ridges,
        float modulation,
        bool saddle,
        BoundedFloatFunction<float>* offsetTransformer);

    /**
     * Calculate mountain continentalness
     * Reference: TerrainProvider.java mountainContinentalness() lines 130-138
     */
    static float mountainContinentalness(float ridge, float modulation, float allowRiversBelow);

    /**
     * Calculate mountain ridge zero continentalness point
     * Reference: TerrainProvider.java calculateMountainRidgeZeroContinentalnessPoint() lines 140-146
     */
    static float calculateMountainRidgeZeroContinentalnessPoint(float modulation);

    /**
     * Build ridge spline
     * Reference: TerrainProvider.java ridgeSpline() lines 170-174
     */
    template<typename C, typename I>
    static CubicSpline<C, I>* ridgeSpline(
        I* ridges,
        float valley,
        float low,
        float mid,
        float high,
        float peaks,
        float minValleySteepness,
        BoundedFloatFunction<float>* offsetTransformer);
};

} // namespace levelgen
} // namespace minecraft

// Include template implementations
#include "levelgen/TerrainProvider.inl"
