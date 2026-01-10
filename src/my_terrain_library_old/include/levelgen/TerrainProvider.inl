#pragma once

// This file is included at the end of TerrainProvider.h
// It contains template implementations for TerrainProvider class methods

#include "math/Mth.h"
#include <algorithm>
#include <cmath>

namespace minecraft {
namespace levelgen {

// ============================================================================
// Static transformer functions
// Reference: TerrainProvider.java lines 176-180
// ============================================================================

inline BoundedFloatFunction<float>* TerrainProvider::NO_TRANSFORM() {
    return BoundedFloatFunction<float>::identity();
}

inline BoundedFloatFunction<float>* TerrainProvider::AMPLIFIED_OFFSET() {
    static auto* func = BoundedFloatFunction<float>::createUnlimited([](float offset) {
        return offset < 0.0f ? offset : offset * 2.0f;
    });
    return func;
}

inline BoundedFloatFunction<float>* TerrainProvider::AMPLIFIED_FACTOR() {
    static auto* func = BoundedFloatFunction<float>::createUnlimited([](float factor) {
        return 1.25f - 6.25f / (factor + 5.0f);
    });
    return func;
}

inline BoundedFloatFunction<float>* TerrainProvider::AMPLIFIED_JAGGEDNESS() {
    static auto* func = BoundedFloatFunction<float>::createUnlimited([](float jaggedness) {
        return jaggedness * 2.0f;
    });
    return func;
}

// ============================================================================
// Helper function from NoiseRouterData.java line 134-136
// ============================================================================

inline float TerrainProvider::peaksAndValleys(float weirdness) {
    return -(std::abs(std::abs(weirdness) - 0.6666667f) - 0.33333334f) * 3.0f;
}

// ============================================================================
// Simple helper functions
// ============================================================================

inline float TerrainProvider::calculateSlope(float y1, float y2, float x1, float x2) {
    // Java line 88-90
    return (y2 - y1) / (x2 - x1);
}

inline float TerrainProvider::mountainContinentalness(
    float ridge,
    float modulation,
    float allowRiversBelow)
{
    // Java lines 130-138
    float ridgeOffset = 1.17f;
    float ridgeAmplitude = 0.46082947f;
    float ridgeSlope = 1.0f - (1.0f - modulation) * 0.5f;
    float ridgeIntersect = 0.5f * (1.0f - modulation);
    float adjustedRidgeHeight = (ridge + ridgeOffset) * ridgeAmplitude;
    float continentalness = adjustedRidgeHeight * ridgeSlope - ridgeIntersect;
    return ridge < allowRiversBelow ? std::max(continentalness, -0.2222f) : std::max(continentalness, 0.0f);
}

inline float TerrainProvider::calculateMountainRidgeZeroContinentalnessPoint(float modulation) {
    // Java lines 140-146
    float ridgeOffset = 1.17f;
    float ridgeAmplitude = 0.46082947f;
    float ridgeSlope = 1.0f - (1.0f - modulation) * 0.5f;
    float ridgeIntersect = 0.5f * (1.0f - modulation);
    return ridgeIntersect / (ridgeAmplitude * ridgeSlope) - ridgeOffset;
}

// ============================================================================
// Template implementations - ridgeSpline
// Reference: TerrainProvider.java ridgeSpline() lines 170-174
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::ridgeSpline(
    I* ridges,
    float valley,
    float low,
    float mid,
    float high,
    float peaks,
    float minValleySteepness,
    BoundedFloatFunction<float>* offsetTransformer)
{
    float d1 = std::max(0.5f * (low - valley), minValleySteepness);
    float d2 = 5.0f * (mid - low);

    return CubicSpline<C, I>::builder(ridges, offsetTransformer)
        .addPoint(-1.0f, valley, d1)
        .addPoint(-0.4f, low, std::min(d1, d2))
        .addPoint(0.0f, mid, d2)
        .addPoint(0.4f, high, 2.0f * (high - mid))
        .addPoint(1.0f, peaks, 0.7f * (peaks - high))
        .build();
}

// ============================================================================
// Template implementations - buildMountainRidgeSplineWithPoints
// Reference: TerrainProvider.java buildMountainRidgeSplineWithPoints() lines 92-128
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::buildMountainRidgeSplineWithPoints(
    I* ridges,
    float modulation,
    bool saddle,
    BoundedFloatFunction<float>* offsetTransformer)
{
    auto builder = CubicSpline<C, I>::builder(ridges, offsetTransformer);

    float allowRiversBelow = -0.7f;
    float minPoint = -1.0f;
    float minPointContinentalness = mountainContinentalness(-1.0f, modulation, allowRiversBelow);
    float maxPoint = 1.0f;
    float maxPointContinentalness = mountainContinentalness(1.0f, modulation, allowRiversBelow);
    float ridgeZeroPoint = calculateMountainRidgeZeroContinentalnessPoint(modulation);
    float afterRiverPoint = -0.65f;

    if (-0.65f < ridgeZeroPoint && ridgeZeroPoint < 1.0f) {
        float afterRiverThresholdContinentalness = mountainContinentalness(-0.65f, modulation, -0.7f);
        float beforeRiverPoint = -0.75f;
        float beforeRiverThresholdContinentalness = mountainContinentalness(-0.75f, modulation, -0.7f);
        float minPointDerivative = calculateSlope(minPointContinentalness, beforeRiverThresholdContinentalness, -1.0f, -0.75f);

        builder.addPoint(-1.0f, minPointContinentalness, minPointDerivative);
        builder.addPoint(-0.75f, beforeRiverThresholdContinentalness);
        builder.addPoint(-0.65f, afterRiverThresholdContinentalness);

        float ridgeZeroPointContinentalness = mountainContinentalness(ridgeZeroPoint, modulation, -0.7f);
        float maxPointDerivative = calculateSlope(ridgeZeroPointContinentalness, maxPointContinentalness, ridgeZeroPoint, 1.0f);

        builder.addPoint(ridgeZeroPoint - 0.01f, ridgeZeroPointContinentalness);
        builder.addPoint(ridgeZeroPoint, ridgeZeroPointContinentalness, maxPointDerivative);
        builder.addPoint(1.0f, maxPointContinentalness, maxPointDerivative);
    } else {
        float simpleDerivative = calculateSlope(minPointContinentalness, maxPointContinentalness, -1.0f, 1.0f);

        if (saddle) {
            builder.addPoint(-1.0f, std::max(0.2f, minPointContinentalness));
            builder.addPoint(0.0f, Mth::lerp(0.5f, minPointContinentalness, maxPointContinentalness), simpleDerivative);
        } else {
            builder.addPoint(-1.0f, minPointContinentalness, simpleDerivative);
        }

        builder.addPoint(1.0f, maxPointContinentalness, simpleDerivative);
    }

    return builder.build();
}

// ============================================================================
// Template implementations - buildWeirdnessJaggednessSpline
// Reference: TerrainProvider.java buildWeirdnessJaggednessSpline() lines 66-70
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::buildWeirdnessJaggednessSpline(
    I* weirdness,
    float jaggednessFactor,
    BoundedFloatFunction<float>* jaggednessTransformer)
{
    float maxJaggednessAtNegativeWeirdness = 0.63f * jaggednessFactor;
    float maxJaggednessAtPositiveWeirdness = 0.3f * jaggednessFactor;

    return CubicSpline<C, I>::builder(weirdness, jaggednessTransformer)
        .addPoint(-0.01f, maxJaggednessAtNegativeWeirdness)
        .addPoint(0.01f, maxJaggednessAtPositiveWeirdness)
        .build();
}

// ============================================================================
// Template implementations - buildRidgeJaggednessSpline
// Reference: TerrainProvider.java buildRidgeJaggednessSpline() lines 45-64
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::buildRidgeJaggednessSpline(
    I* weirdness,
    I* ridges,
    float jaggednessFactorAtPeakRidge,
    float jaggednessFactorAtHighRidge,
    BoundedFloatFunction<float>* jaggednessTransformer)
{
    float highSliceStart = peaksAndValleys(0.4f);
    float highSliceEnd = peaksAndValleys(0.56666666f);
    float highSliceMiddle = (highSliceStart + highSliceEnd) / 2.0f;

    auto ridgeSplineBuilder = CubicSpline<C, I>::builder(ridges, jaggednessTransformer);
    ridgeSplineBuilder.addPoint(highSliceStart, 0.0f);

    if (jaggednessFactorAtHighRidge > 0.0f) {
        ridgeSplineBuilder.addPoint(highSliceMiddle, buildWeirdnessJaggednessSpline<C, I>(weirdness, jaggednessFactorAtHighRidge, jaggednessTransformer));
    } else {
        ridgeSplineBuilder.addPoint(highSliceMiddle, 0.0f);
    }

    if (jaggednessFactorAtPeakRidge > 0.0f) {
        ridgeSplineBuilder.addPoint(1.0f, buildWeirdnessJaggednessSpline<C, I>(weirdness, jaggednessFactorAtPeakRidge, jaggednessTransformer));
    } else {
        ridgeSplineBuilder.addPoint(1.0f, 0.0f);
    }

    return ridgeSplineBuilder.build();
}

// ============================================================================
// Template implementations - buildErosionJaggednessSpline
// Reference: TerrainProvider.java buildErosionJaggednessSpline() lines 38-43
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::buildErosionJaggednessSpline(
    I* erosion,
    I* weirdness,
    I* ridges,
    float jaggednessFactorAtPeakRidgeAndErosionIndex0,
    float jaggednessFactorAtPeakRidgeAndErosionIndex1,
    float jaggednessFactorAtHighRidgeAndErosionIndex0,
    float jaggednessFactorAtHighRidgeAndErosionIndex1,
    BoundedFloatFunction<float>* jaggednessTransformer)
{
    float erosionIndex1Middle = -0.5775f;
    CubicSpline<C, I>* ridgeJaggednessSplineAtErosion0 = buildRidgeJaggednessSpline<C, I>(
        weirdness, ridges,
        jaggednessFactorAtPeakRidgeAndErosionIndex0,
        jaggednessFactorAtHighRidgeAndErosionIndex0,
        jaggednessTransformer
    );
    CubicSpline<C, I>* ridgeJaggednessSplineAtErosion1 = buildRidgeJaggednessSpline<C, I>(
        weirdness, ridges,
        jaggednessFactorAtPeakRidgeAndErosionIndex1,
        jaggednessFactorAtHighRidgeAndErosionIndex1,
        jaggednessTransformer
    );

    return CubicSpline<C, I>::builder(erosion, jaggednessTransformer)
        .addPoint(-1.0f, ridgeJaggednessSplineAtErosion0)
        .addPoint(-0.78f, ridgeJaggednessSplineAtErosion1)
        .addPoint(-0.5775f, ridgeJaggednessSplineAtErosion1)
        .addPoint(-0.375f, 0.0f)
        .build();
}

// ============================================================================
// Template implementations - getErosionFactor
// Reference: TerrainProvider.java getErosionFactor() lines 72-86
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::getErosionFactor(
    I* erosion,
    I* weirdness,
    I* ridges,
    float baseValue,
    bool shatteredTerrain,
    BoundedFloatFunction<float>* factorTransformer)
{
    CubicSpline<C, I>* baseSpline = CubicSpline<C, I>::builder(weirdness, factorTransformer)
        .addPoint(-0.2f, 6.3f)
        .addPoint(0.2f, baseValue)
        .build();

    auto erosionPoints = CubicSpline<C, I>::builder(erosion, factorTransformer)
        .addPoint(-0.6f, baseSpline)
        .addPoint(-0.5f, CubicSpline<C, I>::builder(weirdness, factorTransformer)
            .addPoint(-0.05f, 6.3f)
            .addPoint(0.05f, 2.67f)
            .build())
        .addPoint(-0.35f, baseSpline)
        .addPoint(-0.25f, baseSpline)
        .addPoint(-0.1f, CubicSpline<C, I>::builder(weirdness, factorTransformer)
            .addPoint(-0.05f, 2.67f)
            .addPoint(0.05f, 6.3f)
            .build())
        .addPoint(0.03f, baseSpline);

    if (shatteredTerrain) {
        CubicSpline<C, I>* weirdnessShattered = CubicSpline<C, I>::builder(weirdness, factorTransformer)
            .addPoint(0.0f, baseValue)
            .addPoint(0.1f, 0.625f)
            .build();
        CubicSpline<C, I>* ridgesShattered = CubicSpline<C, I>::builder(ridges, factorTransformer)
            .addPoint(-0.9f, baseValue)
            .addPoint(-0.69f, weirdnessShattered)
            .build();
        erosionPoints.addPoint(0.35f, baseValue)
            .addPoint(0.45f, ridgesShattered)
            .addPoint(0.55f, ridgesShattered)
            .addPoint(0.62f, baseValue);
    } else {
        CubicSpline<C, I>* extremeHillsTerrainFromMidSliceAndUp = CubicSpline<C, I>::builder(ridges, factorTransformer)
            .addPoint(-0.7f, baseSpline)
            .addPoint(-0.15f, 1.37f)
            .build();
        CubicSpline<C, I>* extra3dNoiseOnPeaksOnly = CubicSpline<C, I>::builder(ridges, factorTransformer)
            .addPoint(0.45f, baseSpline)
            .addPoint(0.7f, 1.56f)
            .build();
        erosionPoints.addPoint(0.05f, extra3dNoiseOnPeaksOnly)
            .addPoint(0.4f, extra3dNoiseOnPeaksOnly)
            .addPoint(0.45f, extremeHillsTerrainFromMidSliceAndUp)
            .addPoint(0.55f, extremeHillsTerrainFromMidSliceAndUp)
            .addPoint(0.58f, baseValue);
    }

    return erosionPoints.build();
}

// ============================================================================
// Template implementations - buildErosionOffsetSplineT
// Reference: TerrainProvider.java buildErosionOffsetSpline() lines 148-168
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::buildErosionOffsetSplineT(
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
    BoundedFloatFunction<float>* offsetTransformer)
{
    float lowPeaks = 0.6f;
    float valleyPlateau = 0.5f;
    float plateau = 0.5f;
    (void)lowPeaks;
    (void)valleyPlateau;
    (void)plateau;

    CubicSpline<C, I>* veryLowErosionMountains = buildMountainRidgeSplineWithPoints<C, I>(
        ridges, Mth::lerp(mountainFactor, 0.6f, 1.5f), saddle, offsetTransformer);
    CubicSpline<C, I>* lowErosionMountains = buildMountainRidgeSplineWithPoints<C, I>(
        ridges, Mth::lerp(mountainFactor, 0.6f, 1.0f), saddle, offsetTransformer);
    CubicSpline<C, I>* mountains = buildMountainRidgeSplineWithPoints<C, I>(
        ridges, mountainFactor, saddle, offsetTransformer);
    CubicSpline<C, I>* widePlateau = ridgeSpline<C, I>(
        ridges, lowValley - 0.15f, 0.5f * mountainFactor,
        Mth::lerp(0.5f, 0.5f, 0.5f) * mountainFactor,
        0.5f * mountainFactor, 0.6f * mountainFactor, 0.5f, offsetTransformer);
    CubicSpline<C, I>* narrowPlateau = ridgeSpline<C, I>(
        ridges, lowValley, plain * mountainFactor, hill * mountainFactor,
        0.5f * mountainFactor, 0.6f * mountainFactor, 0.5f, offsetTransformer);
    CubicSpline<C, I>* plains = ridgeSpline<C, I>(
        ridges, lowValley, plain, plain, hill, tallHill, 0.5f, offsetTransformer);
    CubicSpline<C, I>* plainsFarInland = ridgeSpline<C, I>(
        ridges, lowValley, plain, plain, hill, tallHill, 0.5f, offsetTransformer);
    CubicSpline<C, I>* extremeHills = CubicSpline<C, I>::builder(ridges, offsetTransformer)
        .addPoint(-1.0f, lowValley)
        .addPoint(-0.4f, plains)
        .addPoint(0.0f, tallHill + 0.07f)
        .build();
    CubicSpline<C, I>* swamps = ridgeSpline<C, I>(
        ridges, -0.02f, swamp, swamp, hill, tallHill, 0.0f, offsetTransformer);

    auto builder = CubicSpline<C, I>::builder(erosion, offsetTransformer)
        .addPoint(-0.85f, veryLowErosionMountains)
        .addPoint(-0.7f, lowErosionMountains)
        .addPoint(-0.4f, mountains)
        .addPoint(-0.35f, widePlateau)
        .addPoint(-0.1f, narrowPlateau)
        .addPoint(0.2f, plains);

    if (includeExtremeHills) {
        builder.addPoint(0.4f, plainsFarInland)
            .addPoint(0.45f, extremeHills)
            .addPoint(0.55f, extremeHills)
            .addPoint(0.58f, plainsFarInland);
    }

    builder.addPoint(0.7f, swamps);
    return builder.build();
}

// ============================================================================
// Public API - overworldOffsetT (template version)
// Reference: TerrainProvider.java overworldOffset() lines 18-25
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::overworldOffsetT(
    I* continents,
    I* erosion,
    I* ridges,
    bool amplified)
{
    BoundedFloatFunction<float>* offsetTransformer = amplified ? AMPLIFIED_OFFSET() : NO_TRANSFORM();

    CubicSpline<C, I>* beachSpline = buildErosionOffsetSplineT<C, I>(
        erosion, ridges, -0.15f, 0.0f, 0.0f, 0.1f, 0.0f, -0.03f, false, false, offsetTransformer);
    CubicSpline<C, I>* lowSpline = buildErosionOffsetSplineT<C, I>(
        erosion, ridges, -0.1f, 0.03f, 0.1f, 0.1f, 0.01f, -0.03f, false, false, offsetTransformer);
    CubicSpline<C, I>* midSpline = buildErosionOffsetSplineT<C, I>(
        erosion, ridges, -0.1f, 0.03f, 0.1f, 0.7f, 0.01f, -0.03f, true, true, offsetTransformer);
    CubicSpline<C, I>* highSpline = buildErosionOffsetSplineT<C, I>(
        erosion, ridges, -0.05f, 0.03f, 0.1f, 1.0f, 0.01f, 0.01f, true, true, offsetTransformer);

    return CubicSpline<C, I>::builder(continents, offsetTransformer)
        .addPoint(-1.1f, 0.044f)
        .addPoint(-1.02f, -0.2222f)
        .addPoint(-0.51f, -0.2222f)   // DEEP_OCEAN_CONTINENTALNESS
        .addPoint(-0.44f, -0.12f)
        .addPoint(-0.18f, -0.12f)
        .addPoint(-0.16f, beachSpline)
        .addPoint(-0.15f, beachSpline) // BEACH_CONTINENTALNESS
        .addPoint(-0.1f, lowSpline)
        .addPoint(0.25f, midSpline)
        .addPoint(1.0f, highSpline)
        .build();
}

// ============================================================================
// Public API - overworldFactorT (template version)
// Reference: TerrainProvider.java overworldFactor() lines 27-30
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::overworldFactorT(
    I* continents,
    I* erosion,
    I* weirdness,
    I* ridges,
    bool amplified)
{
    BoundedFloatFunction<float>* factorTransformer = amplified ? AMPLIFIED_FACTOR() : NO_TRANSFORM();

    return CubicSpline<C, I>::builder(continents, NO_TRANSFORM())
        .addPoint(-0.19f, 3.95f)
        .addPoint(-0.15f, getErosionFactor<C, I>(erosion, weirdness, ridges, 6.25f, true, NO_TRANSFORM())) // BEACH_CONTINENTALNESS
        .addPoint(-0.1f, getErosionFactor<C, I>(erosion, weirdness, ridges, 5.47f, true, factorTransformer))
        .addPoint(0.03f, getErosionFactor<C, I>(erosion, weirdness, ridges, 5.08f, true, factorTransformer))
        .addPoint(0.06f, getErosionFactor<C, I>(erosion, weirdness, ridges, 4.69f, false, factorTransformer))
        .build();
}

// ============================================================================
// Public API - overworldJaggednessT (template version)
// Reference: TerrainProvider.java overworldJaggedness() lines 32-36
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* TerrainProvider::overworldJaggednessT(
    I* continents,
    I* erosion,
    I* weirdness,
    I* ridges,
    bool amplified)
{
    BoundedFloatFunction<float>* jaggednessTransformer = amplified ? AMPLIFIED_JAGGEDNESS() : NO_TRANSFORM();
    float farInlandMiddle = 0.65f;
    (void)farInlandMiddle;  // Java has this but doesn't use the variable name in the call

    return CubicSpline<C, I>::builder(continents, jaggednessTransformer)
        .addPoint(-0.11f, 0.0f)
        .addPoint(0.03f, buildErosionJaggednessSpline<C, I>(erosion, weirdness, ridges, 1.0f, 0.5f, 0.0f, 0.0f, jaggednessTransformer))
        .addPoint(0.65f, buildErosionJaggednessSpline<C, I>(erosion, weirdness, ridges, 1.0f, 1.0f, 1.0f, 0.0f, jaggednessTransformer))
        .build();
}

// ============================================================================
// Convenience functions (non-template versions using Point/Coordinate types)
// ============================================================================

inline TerrainProvider::SplineType* TerrainProvider::overworldOffset(
    Coordinate* continents,
    Coordinate* erosion,
    Coordinate* ridges,
    bool amplified)
{
    return overworldOffsetT<Point, Coordinate>(continents, erosion, ridges, amplified);
}

inline TerrainProvider::SplineType* TerrainProvider::overworldFactor(
    Coordinate* continents,
    Coordinate* erosion,
    Coordinate* weirdness,
    Coordinate* ridges,
    bool amplified)
{
    return overworldFactorT<Point, Coordinate>(continents, erosion, weirdness, ridges, amplified);
}

inline TerrainProvider::SplineType* TerrainProvider::overworldJaggedness(
    Coordinate* continents,
    Coordinate* erosion,
    Coordinate* weirdness,
    Coordinate* ridges,
    bool amplified)
{
    return overworldJaggednessT<Point, Coordinate>(continents, erosion, weirdness, ridges, amplified);
}

} // namespace levelgen
} // namespace minecraft
