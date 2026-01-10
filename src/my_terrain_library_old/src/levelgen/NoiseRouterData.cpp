#include "levelgen/NoiseRouterData.h"
#include "levelgen/DensityFunctions.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/TerrainProvider.h"
#include "levelgen/NoiseSettings.h"
#include "math/Mth.h"
#include <cmath>
#include <algorithm>

namespace minecraft {
namespace levelgen {

using namespace minecraft::density;
using namespace minecraft::density::DensityFunctions;
using minecraft::Mth;

// ============================================================================
// Constants from Java (NoiseRouterData.java lines 16-31)
// ============================================================================

// Already defined in header, but local for clarity
// IMPORTANT: Java uses (double)-0.50375F which preserves float precision loss
// We must match this exactly for parity
static const double GLOBAL_OFFSET_D = static_cast<double>(-0.50375f);
// Java uses (double)0.08F for ORE_THICKNESS
static const double ORE_THICKNESS_D = static_cast<double>(0.08f);
static constexpr double VEININESS_FREQUENCY_D = 1.5;
static constexpr double SURFACE_DENSITY_THRESHOLD_D = 1.5625;
static constexpr double CHEESE_NOISE_TARGET_D = -0.703125;
static constexpr double NOISE_ZERO_D = 0.390625;
static constexpr double BASE_DENSITY_MULTIPLIER_D = 4.0;

// Ore vein Y ranges (from OreVeinifier.java)
static constexpr int ORE_VEIN_MIN_Y = -60;  // Iron min
static constexpr int ORE_VEIN_MAX_Y = 50;   // Copper max

// ============================================================================
// Public Static Methods
// ============================================================================

float NoiseRouterData::peaksAndValleys(float weirdness) {
    // Java: NoiseRouterData.java line 134-136
    // Formula: -(abs(abs(weirdness) - 0.6666667) - 0.33333334) * 3.0
    return -(std::abs(std::abs(weirdness) - 0.6666667f) - 0.33333334f) * 3.0f;
}

// ============================================================================
// DensityFunction version of peaksAndValleys
// Java: NoiseRouterData.java line 130-132
// ============================================================================

DensityFunction* NoiseRouterData::peaksAndValleys(DensityFunction* weirdness) {
    // DensityFunctions.mul(
    //   DensityFunctions.add(
    //     DensityFunctions.add(weirdness.abs(), DensityFunctions.constant(-0.6666666666666666)).abs(),
    //     DensityFunctions.constant(-0.3333333333333333)
    //   ),
    //   DensityFunctions.constant(-3.0)
    // )
    return mul(
        add(
            abs(
                add(
                    abs(weirdness),
                    constant(-0.6666666666666666)
                )
            ),
            constant(-0.3333333333333333)
        ),
        constant(-3.0)
    );
}

// ============================================================================
// Main Overworld Router - COMPLETE IMPLEMENTATION
// Java: NoiseRouterData.java lines 217-244
// ============================================================================

NoiseRouter* NoiseRouterData::overworld(bool largeBiomes, bool amplified) {
    // Reference: NoiseRouterData.java overworld() lines 217-244
    // and registerTerrainNoises() lines 104-116

    using minecraft::levelgen::DensityFunctionRegistry;
    DensityFunctionRegistry& dfReg = DensityFunctionRegistry::instance();

    // Get world seed from registry (set by RandomState)
    int64_t worldSeed = dfReg.getWorldSeed();

    // ========================================================================
    // Step 1: Create Aquifer Noises (lines 218-221)
    // ========================================================================

    // barrier noise: scale 0.5
    DensityFunction::NoiseHolder* barrierNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("aquifer_barrier", worldSeed);
    DensityFunction* barrierNoise = noise(barrierNoiseHolder, 1.0, 0.5);

    // floodedness noise: yScale 0.67
    DensityFunction::NoiseHolder* floodNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("aquifer_fluid_level_floodedness", worldSeed);
    DensityFunction* fluidLevelFloodednessNoise = noise(floodNoiseHolder, 1.0, 0.67);

    // spread noise: yScale 0.7142857142857143 (5/7)
    DensityFunction::NoiseHolder* spreadNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("aquifer_fluid_level_spread", worldSeed);
    DensityFunction* fluidLevelSpreadNoise = noise(spreadNoiseHolder, 1.0, 0.7142857142857143);

    // lava noise: default scales
    DensityFunction::NoiseHolder* lavaNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("aquifer_lava", worldSeed);
    DensityFunction* lavaNoise = noise(lavaNoiseHolder, 1.0, 1.0);

    // ========================================================================
    // Step 2: Create Shift Functions (lines 80-81 in bootstrap, used in 222-223)
    // shiftX = flatCache(cache2d(shiftA(offset)))
    // shiftZ = flatCache(cache2d(shiftB(offset)))
    // ========================================================================

    DensityFunction::NoiseHolder* offsetNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("offset", worldSeed);
    DensityFunction* shiftX = flatCache(cache2d(shiftA(offsetNoiseHolder)));
    DensityFunction* shiftZ = flatCache(cache2d(shiftB(offsetNoiseHolder)));

    // ========================================================================
    // Step 3: Create Climate Noises (lines 224-225)
    // temperature and vegetation using shifted noise
    // ========================================================================

    const char* tempNoiseName = largeBiomes ? "temperature_large" : "temperature";
    const char* vegNoiseName = largeBiomes ? "vegetation_large" : "vegetation";

    DensityFunction::NoiseHolder* temperatureNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder(tempNoiseName, worldSeed);
    DensityFunction* temperature = shiftedNoise2d(shiftX, shiftZ, 0.25, temperatureNoiseHolder);

    DensityFunction::NoiseHolder* vegetationNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder(vegNoiseName, worldSeed);
    DensityFunction* vegetation = shiftedNoise2d(shiftX, shiftZ, 0.25, vegetationNoiseHolder);

    // ========================================================================
    // Step 4: Create Continents, Erosion, Ridges (lines 85-88 in bootstrap)
    // These are the 2D climate parameters
    // ========================================================================

    const char* contNoiseName = largeBiomes ? "continentalness_large" : "continentalness";
    const char* erosNoiseName = largeBiomes ? "erosion_large" : "erosion";

    DensityFunction::NoiseHolder* continentalnessNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder(contNoiseName, worldSeed);
    DensityFunction* continents = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, continentalnessNoiseHolder));

    DensityFunction::NoiseHolder* erosionNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder(erosNoiseName, worldSeed);
    DensityFunction* erosion = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, erosionNoiseHolder));

    DensityFunction::NoiseHolder* ridgeNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ridge", worldSeed);
    DensityFunction* ridges = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, ridgeNoiseHolder));

    // ridges_folded = peaksAndValleys(ridges) - line 88
    DensityFunction* ridgesFolded = peaksAndValleys(ridges);

    // ========================================================================
    // Step 5: Build Terrain Splines (from registerTerrainNoises lines 104-116)
    // This creates offset, factor, jaggedness, depth, slopedCheese
    // ========================================================================

    // Create spline coordinates
    // Java uses Holder<DensityFunction> for these, we use raw pointers
    TerrainProvider::Coordinate* continentsCoord = new TerrainProvider::Coordinate(continents);
    TerrainProvider::Coordinate* erosionCoord = new TerrainProvider::Coordinate(erosion);
    TerrainProvider::Coordinate* weirdnessCoord = new TerrainProvider::Coordinate(ridges);  // weirdness = ridges
    TerrainProvider::Coordinate* ridgesFoldedCoord = new TerrainProvider::Coordinate(ridgesFolded);

    // Line 109: offset = splineWithBlending(GLOBAL_OFFSET + spline(overworldOffset), blendOffset())
    TerrainProvider::SplineType* offsetSpline = TerrainProvider::overworldOffset(
        continentsCoord,
        erosionCoord,
        ridgesFoldedCoord,
        amplified
    );
    DensityFunction* offsetSplineFunc = spline(offsetSpline);
    DensityFunction* offsetWithGlobal = add(constant(GLOBAL_OFFSET_D), offsetSplineFunc);
    DensityFunction* offset = splineWithBlending(offsetWithGlobal, blendOffset());

    // Line 110: factor = splineWithBlending(spline(overworldFactor), constant(10))
    TerrainProvider::SplineType* factorSpline = TerrainProvider::overworldFactor(
        continentsCoord,
        erosionCoord,
        weirdnessCoord,
        ridgesFoldedCoord,
        amplified
    );
    DensityFunction* factorSplineFunc = spline(factorSpline);
    DensityFunction* blendingFactor = constant(10.0);
    DensityFunction* factor = splineWithBlending(factorSplineFunc, blendingFactor);

    // Line 111: depth = offsetToDepth(offset)
    DensityFunction* depth = offsetToDepth(offset);

    // Line 112-113: jaggedness = splineWithBlending(spline(overworldJaggedness), zero()) * jaggedNoise.halfNegative()
    TerrainProvider::SplineType* jaggednessSpline = TerrainProvider::overworldJaggedness(
        continentsCoord,
        erosionCoord,
        weirdnessCoord,
        ridgesFoldedCoord,
        amplified
    );
    DensityFunction* jaggednessSplineFunc = spline(jaggednessSpline);
    DensityFunction* blendingJaggedness = zero();
    DensityFunction* unscaledJaggedness = splineWithBlending(jaggednessSplineFunc, blendingJaggedness);

    // jaggedNoise = noise(JAGGED, 1500.0, 0.0) - line 89
    DensityFunction::NoiseHolder* jaggedNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("jagged", worldSeed);
    DensityFunction* jaggedNoise = noise(jaggedNoiseHolder, 1500.0, 0.0);

    // jaggedness = unscaledJaggedness * jaggedNoise.halfNegative()
    DensityFunction* jaggedness = mul(unscaledJaggedness, halfNegative(jaggedNoise));

    // Line 114: initialDensity = noiseGradientDensity(factor, depth + jaggedness)
    DensityFunction* depthWithJaggedness = add(depth, jaggedness);
    DensityFunction* initialDensity = noiseGradientDensity(factor, depthWithJaggedness);

    // Line 115: slopedCheese = initialDensity + base3dNoise
    // base3dNoise = BlendedNoise.createUnseeded(0.25, 0.125, 80.0, 160.0, 8.0) - line 82
    DensityFunction* base3dNoise = dfReg.getOrThrow("overworld/base_3d_noise");
    DensityFunction* slopedCheese = add(initialDensity, base3dNoise);

    // ========================================================================
    // Step 6: Compute Preliminary Surface Level (line 229)
    // ========================================================================

    DensityFunction* prelimSurfaceLevel = preliminarySurfaceLevel(offset, factor, amplified);

    // ========================================================================
    // Step 7: Build Cave System (lines 231-233)
    // ========================================================================

    // Get entrances function
    DensityFunction* entrancesFunc = entrances(worldSeed);

    // Line 231: surfaceWithEntrances = min(slopedCheese, 5.0 * entrances)
    DensityFunction* surfaceWithEntrances = min(
        slopedCheese,
        mul(constant(5.0), entrancesFunc)
    );

    // Line 232: caves = rangeChoice(slopedCheese, -1000000, 1.5625, surfaceWithEntrances, underground(...))
    DensityFunction* undergroundFunc = underground(worldSeed, slopedCheese);
    DensityFunction* caves = rangeChoice(
        slopedCheese,
        -1000000.0,
        SURFACE_DENSITY_THRESHOLD_D,  // 1.5625
        surfaceWithEntrances,
        undergroundFunc
    );

    // Line 233: fullNoise = min(postProcess(slideOverworld(caves)), noodle)
    DensityFunction* noodleFunc = noodle(worldSeed);
    DensityFunction* slidCaves = slideOverworld(amplified, caves);
    DensityFunction* processedCaves = postProcess(slidCaves);
    DensityFunction* fullNoise = min(processedCaves, noodleFunc);

    // ========================================================================
    // Step 8: Build Ore Vein Functions (lines 234-242)
    // ========================================================================

    // y function for Y-limited interpolation
    // y = yClampedGradient(belowBottom, aboveTop, belowBottom, aboveTop) where below/above = MIN_Y*2, MAX_Y*2
    int belowBottom = -64 * 2;  // DimensionType.MIN_Y * 2
    int aboveTop = 320 * 2;      // DimensionType.MAX_Y * 2
    DensityFunction* y = yClampedGradient(belowBottom, aboveTop, static_cast<double>(belowBottom), static_cast<double>(aboveTop));

    // Ore vein Y limits
    int veinMinY = ORE_VEIN_MIN_Y;  // -60 (iron min)
    int veinMaxY = ORE_VEIN_MAX_Y;  // 50 (copper max)

    // Line 237: veinToggle = yLimitedInterpolatable(y, noise(ORE_VEININESS, 1.5, 1.5), veinMinY, veinMaxY, 0)
    DensityFunction::NoiseHolder* oreVeininessNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ore_veininess", worldSeed);
    DensityFunction* oreVeininessNoise = noise(oreVeininessNoiseHolder, 1.5, 1.5);
    DensityFunction* veinToggle = yLimitedInterpolatable(y, oreVeininessNoise, veinMinY, veinMaxY, 0);

    // Lines 239-240: veinA and veinB
    DensityFunction::NoiseHolder* oreVeinANoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ore_vein_a", worldSeed);
    DensityFunction* oreVeinANoise = noise(oreVeinANoiseHolder, 4.0, 4.0);
    DensityFunction* veinA = abs(yLimitedInterpolatable(y, oreVeinANoise, veinMinY, veinMaxY, 0));

    DensityFunction::NoiseHolder* oreVeinBNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ore_vein_b", worldSeed);
    DensityFunction* oreVeinBNoise = noise(oreVeinBNoiseHolder, 4.0, 4.0);
    DensityFunction* veinB = abs(yLimitedInterpolatable(y, oreVeinBNoise, veinMinY, veinMaxY, 0));

    // Line 241: veinRidged = -0.08 + max(veinA, veinB)
    // Java uses (double)-0.08F
    DensityFunction* veinRidged = add(constant(static_cast<double>(-0.08f)), max(veinA, veinB));

    // Line 242: veinGap = noise(ORE_GAP)
    DensityFunction::NoiseHolder* oreGapNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ore_gap", worldSeed);
    DensityFunction* veinGap = noise(oreGapNoiseHolder, 1.0, 1.0);

    // ========================================================================
    // Step 9: Build and Return Complete NoiseRouter (line 243)
    // ========================================================================

    return new NoiseRouter(
        barrierNoise,                    // barrierNoise
        fluidLevelFloodednessNoise,      // fluidLevelFloodednessNoise
        fluidLevelSpreadNoise,           // fluidLevelSpreadNoise
        lavaNoise,                       // lavaNoise
        temperature,                     // temperature
        vegetation,                      // vegetation
        continents,                      // continents
        erosion,                         // erosion
        depth,                           // depth
        ridges,                          // ridges
        prelimSurfaceLevel,              // preliminarySurfaceLevel
        fullNoise,                       // finalDensity
        veinToggle,                      // veinToggle
        veinRidged,                      // veinRidged
        veinGap                          // veinGap
    );
}

// ============================================================================
// Helper Methods
// ============================================================================

// Java: NoiseRouterData.java line 118-119
DensityFunction* NoiseRouterData::offsetToDepth(DensityFunction* offset) {
    // depth = yClampedGradient(-64, 320, 1.5, -1.5) + offset
    return add(
        yClampedGradient(-64, 320, 1.5, -1.5),
        offset
    );
}

// Java: NoiseRouterData.java line 298-300
DensityFunction* NoiseRouterData::noiseGradientDensity(
    DensityFunction* factor,
    DensityFunction* depthWithJaggedness
) {
    // gradientUnscaled = depthWithJaggedness * factor
    // return 4.0 * quarterNegative(gradientUnscaled)
    DensityFunction* gradientUnscaled = mul(depthWithJaggedness, factor);
    return mul(
        constant(BASE_DENSITY_MULTIPLIER_D),  // 4.0
        quarterNegative(gradientUnscaled)
    );
}

// Java: NoiseRouterData.java line 206-208
DensityFunction* NoiseRouterData::postProcess(DensityFunction* slide) {
    // blended = blendDensity(slide)
    // return (interpolated(blended) * 0.64).squeeze()
    DensityFunction* blended = blendDensity(slide);
    DensityFunction* interpolatedBlended = interpolated(blended);
    DensityFunction* scaled = mul(interpolatedBlended, constant(0.64));
    return squeeze(scaled);
}

// Java: NoiseRouterData.java line 293-296
DensityFunction* NoiseRouterData::splineWithBlending(DensityFunction* spline, DensityFunction* blendingTarget) {
    // blendedSpline = lerp(blendAlpha(), blendingTarget, spline)
    // return flatCache(cache2d(blendedSpline))
    DensityFunction* blendedSpline = lerp(blendAlpha(), blendingTarget, spline);
    return flatCache(cache2d(blendedSpline));
}

// Java: NoiseRouterData.java line 211-214
DensityFunction* NoiseRouterData::remap(
    DensityFunction* input,
    double fromMin,
    double fromMax,
    double toMin,
    double toMax
) {
    // Linear remapping: output = input * factor + offset
    // factor = (toMax - toMin) / (fromMax - fromMin)
    // offset = toMin - fromMin * factor
    double factor = (toMax - toMin) / (fromMax - fromMin);
    double offset = toMin - fromMin * factor;

    return add(
        mul(input, constant(factor)),
        constant(offset)
    );
}

// Java: NoiseRouterData.java line 312-314
DensityFunction* NoiseRouterData::yLimitedInterpolatable(
    DensityFunction* y,
    DensityFunction* whenInRange,
    int minYInclusive,
    int maxYInclusive,
    int whenOutOfRange
) {
    // interpolated(rangeChoice(y, minY, maxY+1, whenInRange, constant(whenOutOfRange)))
    return interpolated(
        rangeChoice(
            y,
            static_cast<double>(minYInclusive),
            static_cast<double>(maxYInclusive + 1),
            whenInRange,
            constant(static_cast<double>(whenOutOfRange))
        )
    );
}

// Java: NoiseRouterData.java line 316-322
DensityFunction* NoiseRouterData::slide(
    DensityFunction* caves,
    int minY,
    int height,
    int topStartY,
    int topEndY,
    double topTarget,
    int bottomStartY,
    int bottomEndY,
    double bottomTarget
) {
    // topFactor = yClampedGradient(minY + height - topStartY, minY + height - topEndY, 1.0, 0.0)
    DensityFunction* topFactor = yClampedGradient(
        minY + height - topStartY,
        minY + height - topEndY,
        1.0,
        0.0
    );

    // noiseValue = lerp(topFactor, topTarget, caves)
    DensityFunction* noiseValue = lerp(topFactor, topTarget, caves);

    // bottomFactor = yClampedGradient(minY + bottomStartY, minY + bottomEndY, 0.0, 1.0)
    DensityFunction* bottomFactor = yClampedGradient(
        minY + bottomStartY,
        minY + bottomEndY,
        0.0,
        1.0
    );

    // noiseValue = lerp(bottomFactor, bottomTarget, noiseValue)
    noiseValue = lerp(bottomFactor, bottomTarget, noiseValue);

    return noiseValue;
}

// Java: NoiseRouterData.java line 255-257
DensityFunction* NoiseRouterData::slideOverworld(bool isAmplified, DensityFunction* caves) {
    // slide(caves, -64, 384, isAmplified ? 16 : 80, isAmplified ? 0 : 64, -0.078125, 0, 24, isAmplified ? 0.4 : 0.1171875)
    return slide(
        caves,
        -64,            // minY
        384,            // height
        isAmplified ? 16 : 80,      // topStartY
        isAmplified ? 0 : 64,       // topEndY
        -0.078125,                  // topTarget
        0,              // bottomStartY
        24,             // bottomEndY
        isAmplified ? 0.4 : 0.1171875  // bottomTarget
    );
}

// Java: NoiseRouterData.java line 302-310
DensityFunction* NoiseRouterData::preliminarySurfaceLevel(DensityFunction* offset, DensityFunction* factor, bool amplified) {
    // cachedFactor = cache2d(factor)
    DensityFunction* cachedFactor = cache2d(factor);

    // cachedOffset = cache2d(offset)
    DensityFunction* cachedOffset = cache2d(offset);

    // upperBound = remap(0.2734375 * factor.invert() + -1.0 * offset, 1.5, -1.5, -64.0, 320.0)
    DensityFunction* upperBound = remap(
        add(
            mul(constant(0.2734375), invert(cachedFactor)),
            mul(constant(-1.0), cachedOffset)
        ),
        1.5, -1.5, -64.0, 320.0
    );

    // upperBound = clamp(upperBound, -40.0, 320.0)
    upperBound = clamp(upperBound, -40.0, 320.0);

    // density = slideOverworld(amplified, (noiseGradientDensity(factor, offsetToDepth(offset)) + constant(-0.703125)).clamp(-64, 64)) + constant(-0.390625)
    DensityFunction* density = add(
        slideOverworld(
            amplified,
            clamp(
                add(
                    noiseGradientDensity(cachedFactor, offsetToDepth(cachedOffset)),
                    constant(CHEESE_NOISE_TARGET_D)  // -0.703125
                ),
                -64.0, 64.0
            )
        ),
        constant(-NOISE_ZERO_D)  // -0.390625
    );

    // return findTopSurface(density, upperBound, -64, cellHeight=8)
    return findTopSurface(density, upperBound, -64, 8);
}

// ============================================================================
// Cave Generation Functions
// ============================================================================

// Java: NoiseRouterData.spaghettiRoughnessFunction() line 138-142
DensityFunction* NoiseRouterData::spaghettiRoughnessFunction(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // spaghettiRoughnessNoise = noise(SPAGHETTI_ROUGHNESS)
    DensityFunction* spaghettiRoughnessNoise = noise(
        DensityFunctionRegistry::createNoiseHolder("spaghetti_roughness", worldSeed),
        1.0, 1.0
    );

    // spaghettiRoughnessModulator = mappedNoise(SPAGHETTI_ROUGHNESS_MODULATOR, 0.0, -0.1)
    DensityFunction* spaghettiRoughnessModulator = mappedNoise(
        DensityFunctionRegistry::createNoiseHolder("spaghetti_roughness_modulator", worldSeed),
        0.0, -0.1
    );

    // return cacheOnce(mul(modulator, add(abs(noise), constant(-0.4))))
    return cacheOnce(
        mul(
            spaghettiRoughnessModulator,
            add(abs(spaghettiRoughnessNoise), constant(-0.4))
        )
    );
}

// Java: context.register(SPAGHETTI_2D_THICKNESS_MODULATOR, ...) line 97
DensityFunction* NoiseRouterData::spaghetti2DThicknessModulator(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // cacheOnce(mappedNoise(SPAGHETTI_2D_THICKNESS, 2.0, 1.0, -0.6, -1.3))
    return cacheOnce(
        mappedNoise(
            DensityFunctionRegistry::createNoiseHolder("spaghetti_2d_thickness", worldSeed),
            2.0, 1.0, -0.6, -1.3
        )
    );
}

// Java: NoiseRouterData.entrances() line 144-154
DensityFunction* NoiseRouterData::entrances(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // spaghetti3DRarityModulator = cacheOnce(noise(SPAGHETTI_3D_RARITY, 2.0, 1.0))
    DensityFunction* spaghetti3DRarityModulator = cacheOnce(
        noise(DensityFunctionRegistry::createNoiseHolder("spaghetti_3d_rarity", worldSeed), 2.0, 1.0)
    );

    // spaghetti3DThicknessModulator = mappedNoise(SPAGHETTI_3D_THICKNESS, -0.065, -0.088)
    DensityFunction* spaghetti3DThicknessModulator = mappedNoise(
        DensityFunctionRegistry::createNoiseHolder("spaghetti_3d_thickness", worldSeed),
        -0.065, -0.088
    );

    // spaghetti3DCave1 = weirdScaledSampler(rarity, SPAGHETTI_3D_1, TYPE1)
    DensityFunction* spaghetti3DCave1 = weirdScaledSampler(
        spaghetti3DRarityModulator,
        DensityFunctionRegistry::createNoiseHolder("spaghetti_3d_1", worldSeed),
        WeirdScaledSampler::RarityValueMapper::TYPE1
    );

    // spaghetti3DCave2 = weirdScaledSampler(rarity, SPAGHETTI_3D_2, TYPE1)
    DensityFunction* spaghetti3DCave2 = weirdScaledSampler(
        spaghetti3DRarityModulator,
        DensityFunctionRegistry::createNoiseHolder("spaghetti_3d_2", worldSeed),
        WeirdScaledSampler::RarityValueMapper::TYPE1
    );

    // spaghetti3DFunction = (max(cave1, cave2) + thicknessModulator).clamp(-1, 1)
    DensityFunction* spaghetti3DFunction = clamp(
        add(max(spaghetti3DCave1, spaghetti3DCave2), spaghetti3DThicknessModulator),
        -1.0, 1.0
    );

    // Get spaghetti roughness function
    DensityFunction* spaghettiRoughness = spaghettiRoughnessFunction(worldSeed);

    // bigEntranceNoiseSource = noise(CAVE_ENTRANCE, 0.75, 0.5)
    DensityFunction* bigEntranceNoiseSource = noise(
        DensityFunctionRegistry::createNoiseHolder("cave_entrance", worldSeed),
        0.75, 0.5
    );

    // bigEntrancesFunction = bigEntranceNoiseSource + 0.37 + yClampedGradient(-10, 30, 0.3, 0.0)
    DensityFunction* bigEntrancesFunction = add(
        add(bigEntranceNoiseSource, constant(0.37)),
        yClampedGradient(-10, 30, 0.3, 0.0)
    );

    // return cacheOnce(min(bigEntrancesFunction, add(spaghettiRoughness, spaghetti3DFunction)))
    return cacheOnce(
        min(
            bigEntrancesFunction,
            add(spaghettiRoughness, spaghetti3DFunction)
        )
    );
}

// Java: NoiseRouterData.noodle() line 156-168
DensityFunction* NoiseRouterData::noodle(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // y function
    int belowBottom = -64 * 2;
    int aboveTop = 320 * 2;
    DensityFunction* y = yClampedGradient(belowBottom, aboveTop, static_cast<double>(belowBottom), static_cast<double>(aboveTop));

    int noodleMinY = -60;
    int noodleMaxY = 320;

    // noodleToggle = yLimitedInterpolatable(y, noise(NOODLE, 1.0, 1.0), -60, 320, -1)
    DensityFunction* noodleToggle = yLimitedInterpolatable(
        y,
        noise(DensityFunctionRegistry::createNoiseHolder("noodle", worldSeed), 1.0, 1.0),
        noodleMinY, noodleMaxY, -1
    );

    // noodleThickness = yLimitedInterpolatable(y, mappedNoise(NOODLE_THICKNESS, 1.0, 1.0, -0.05, -0.1), -60, 320, 0)
    DensityFunction* noodleThickness = yLimitedInterpolatable(
        y,
        mappedNoise(DensityFunctionRegistry::createNoiseHolder("noodle_thickness", worldSeed), 1.0, 1.0, -0.05, -0.1),
        noodleMinY, noodleMaxY, 0
    );

    // noodle ridge frequency = 2.6666666666666665
    double noodleRidgeFrequency = 2.6666666666666665;

    // noodleRidgeA = yLimitedInterpolatable(y, noise(NOODLE_RIDGE_A, freq, freq), -60, 320, 0)
    DensityFunction* noodleRidgeA = yLimitedInterpolatable(
        y,
        noise(DensityFunctionRegistry::createNoiseHolder("noodle_ridge_a", worldSeed), noodleRidgeFrequency, noodleRidgeFrequency),
        noodleMinY, noodleMaxY, 0
    );

    // noodleRidgeB = yLimitedInterpolatable(y, noise(NOODLE_RIDGE_B, freq, freq), -60, 320, 0)
    DensityFunction* noodleRidgeB = yLimitedInterpolatable(
        y,
        noise(DensityFunctionRegistry::createNoiseHolder("noodle_ridge_b", worldSeed), noodleRidgeFrequency, noodleRidgeFrequency),
        noodleMinY, noodleMaxY, 0
    );

    // noodleRidged = 1.5 * max(abs(noodleRidgeA), abs(noodleRidgeB))
    DensityFunction* noodleRidged = mul(
        constant(1.5),
        max(abs(noodleRidgeA), abs(noodleRidgeB))
    );

    // return rangeChoice(noodleToggle, -1000000, 0, constant(64), add(noodleThickness, noodleRidged))
    return rangeChoice(
        noodleToggle,
        -1000000.0,
        0.0,
        constant(64.0),
        add(noodleThickness, noodleRidged)
    );
}

// Java: NoiseRouterData.pillars() line 170-178
DensityFunction* NoiseRouterData::pillars(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    double xzFrequency = 25.0;
    double yFrequency = 0.3;

    // pillarNoiseSource = noise(PILLAR, 25.0, 0.3)
    DensityFunction* pillarNoiseSource = noise(
        DensityFunctionRegistry::createNoiseHolder("pillar", worldSeed),
        xzFrequency, yFrequency
    );

    // pillarRarenessModulator = mappedNoise(PILLAR_RARENESS, 0.0, -2.0)
    DensityFunction* pillarRarenessModulator = mappedNoise(
        DensityFunctionRegistry::createNoiseHolder("pillar_rareness", worldSeed),
        0.0, -2.0
    );

    // pillarThicknessModulator = mappedNoise(PILLAR_THICKNESS, 0.0, 1.1)
    DensityFunction* pillarThicknessModulator = mappedNoise(
        DensityFunctionRegistry::createNoiseHolder("pillar_thickness", worldSeed),
        0.0, 1.1
    );

    // pillarsWithRareness = pillarNoiseSource * 2.0 + pillarRarenessModulator
    DensityFunction* pillarsWithRareness = add(
        mul(pillarNoiseSource, constant(2.0)),
        pillarRarenessModulator
    );

    // return cacheOnce(pillarsWithRareness * cube(pillarThicknessModulator))
    return cacheOnce(
        mul(pillarsWithRareness, cube(pillarThicknessModulator))
    );
}

// Java: NoiseRouterData.spaghetti2D() line 180-190
DensityFunction* NoiseRouterData::spaghetti2D(int64_t worldSeed) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // spaghetti2DRarityModulator = noise(SPAGHETTI_2D_MODULATOR, 2.0, 1.0)
    DensityFunction* spaghetti2DRarityModulator = noise(
        DensityFunctionRegistry::createNoiseHolder("spaghetti_2d_modulator", worldSeed),
        2.0, 1.0
    );

    // spaghetti2DCave = weirdScaledSampler(rarity, SPAGHETTI_2D, TYPE2)
    DensityFunction* spaghetti2DCave = weirdScaledSampler(
        spaghetti2DRarityModulator,
        DensityFunctionRegistry::createNoiseHolder("spaghetti_2d", worldSeed),
        WeirdScaledSampler::RarityValueMapper::TYPE2
    );

    // spaghetti2DElevationModulator = mappedNoise(SPAGHETTI_2D_ELEVATION, 0.0, floor(-64/8), 8.0)
    // floor(-64/8) = -8
    DensityFunction* spaghetti2DElevationModulator = mappedNoise(
        DensityFunctionRegistry::createNoiseHolder("spaghetti_2d_elevation", worldSeed),
        0.0, static_cast<double>(std::floor(-64.0 / 8.0)), 8.0
    );

    // Get thickness modulator
    DensityFunction* spaghetti2DThicknessModulatorFunc = spaghetti2DThicknessModulator(worldSeed);

    // slopedSpaghetti = abs(spaghetti2DElevationModulator + yClampedGradient(-64, 320, 8.0, -40.0))
    DensityFunction* slopedSpaghetti = abs(
        add(
            spaghetti2DElevationModulator,
            yClampedGradient(-64, 320, 8.0, -40.0)
        )
    );

    // layerRidged = cube(slopedSpaghetti + spaghetti2DThicknessModulator)
    DensityFunction* layerRidged = cube(
        add(slopedSpaghetti, spaghetti2DThicknessModulatorFunc)
    );

    // caveNoise = spaghetti2DCave + 0.083 * spaghetti2DThicknessModulator
    DensityFunction* caveNoise = add(
        spaghetti2DCave,
        mul(constant(0.083), spaghetti2DThicknessModulatorFunc)
    );

    // return max(caveNoise, layerRidged).clamp(-1, 1)
    return clamp(max(caveNoise, layerRidged), -1.0, 1.0);
}

// Java: NoiseRouterData.underground() line 192-204
DensityFunction* NoiseRouterData::underground(int64_t worldSeed, DensityFunction* slopedCheese) {
    using minecraft::levelgen::DensityFunctionRegistry;

    // Get spaghetti2D and spaghettiRoughness functions
    DensityFunction* spaghetti2DFunction = spaghetti2D(worldSeed);
    DensityFunction* spaghettiRoughnessFunc = spaghettiRoughnessFunction(worldSeed);

    // layerNoiseSource = noise(CAVE_LAYER, 8.0)
    // Java's single-arg noise(holder, scale) = noise(holder, 1.0, scale) - scale goes to yScale!
    DensityFunction* layerNoiseSource = noise(
        DensityFunctionRegistry::createNoiseHolder("cave_layer", worldSeed),
        1.0, 8.0  // xzScale=1.0, yScale=8.0 (Java single-arg semantics)
    );

    // layerizedCavernsFunction = 4.0 * square(layerNoiseSource)
    DensityFunction* layerizedCavernsFunction = mul(
        constant(4.0),
        square(layerNoiseSource)
    );

    // cheese = noise(CAVE_CHEESE, 0.6666666666666666)
    // Java's single-arg noise(holder, scale) = noise(holder, 1.0, scale) - scale goes to yScale!
    DensityFunction* cheese = noise(
        DensityFunctionRegistry::createNoiseHolder("cave_cheese", worldSeed),
        1.0, 0.6666666666666666  // xzScale=1.0, yScale=0.666... (Java single-arg semantics)
    );

    // solidifedCheeseWithTopSlide = clamp(0.27 + cheese, -1, 1) + clamp(1.5 + -0.64 * slopedCheese, 0, 0.5)
    DensityFunction* solidifedCheeseWithTopSlide = add(
        clamp(add(constant(0.27), cheese), -1.0, 1.0),
        clamp(
            add(
                constant(1.5),
                mul(constant(-0.64), slopedCheese)
            ),
            0.0, 0.5
        )
    );

    // baseCaveDensity = layerizedCavernsFunction + solidifedCheeseWithTopSlide
    DensityFunction* baseCaveDensity = add(layerizedCavernsFunction, solidifedCheeseWithTopSlide);

    // Get entrances function
    DensityFunction* entrancesFunc = entrances(worldSeed);

    // undergroundSubtractions = min(min(baseCaveDensity, entrances), add(spaghetti2D, spaghettiRoughness))
    DensityFunction* undergroundSubtractions = min(
        min(baseCaveDensity, entrancesFunc),
        add(spaghetti2DFunction, spaghettiRoughnessFunc)
    );

    // pillarsWithoutCutoff = pillars function
    DensityFunction* pillarsWithoutCutoff = pillars(worldSeed);

    // pillarsFunc = rangeChoice(pillarsWithoutCutoff, -1000000, 0.03, constant(-1000000), pillarsWithoutCutoff)
    DensityFunction* pillarsFunc = rangeChoice(
        pillarsWithoutCutoff,
        -1000000.0,
        0.03,
        constant(-1000000.0),
        pillarsWithoutCutoff
    );

    // return max(undergroundSubtractions, pillars)
    return max(undergroundSubtractions, pillarsFunc);
}

// ============================================================================
// Other Dimension Routers
// ============================================================================

// Java: NoiseRouterData.slideNetherLike() line 259-261
DensityFunction* NoiseRouterData::slideNetherLike(int minY, int height) {
    // return slide(getFunction(functions, BASE_3D_NOISE_NETHER), minY, height, 24, 0, 0.9375, -8, 24, 2.5)
    return slide(
        getBase3DNoiseNether(),
        minY,
        height,
        24,    // topStartY
        0,     // topEndY
        0.9375,   // topTarget
        -8,    // bottomStartY
        24,    // bottomEndY
        2.5    // bottomTarget
    );
}

// Java: NoiseRouterData.slideEndLike() line 263-265
DensityFunction* NoiseRouterData::slideEndLike(DensityFunction* caves, int minY, int height) {
    // return slide(caves, minY, height, 72, -184, -23.4375, 4, 32, -0.234375)
    return slide(
        caves,
        minY,
        height,
        72,       // topStartY
        -184,     // topEndY
        -23.4375, // topTarget
        4,        // bottomStartY
        32,       // bottomEndY
        -0.234375 // bottomTarget
    );
}

// Java: NoiseRouterData.slideEnd() line 279-281
DensityFunction* NoiseRouterData::slideEnd(DensityFunction* caves) {
    return slideEndLike(caves, 0, 128);
}

// Java: NoiseRouterData.noNewCaves() line 246-253
NoiseRouter* NoiseRouterData::noNewCaves(DensityFunction* slide) {
    DensityFunction* zeroFunc = zero();
    DensityFunction* fullNoise = postProcess(slide);

    return new NoiseRouter(
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,  // barrier, fluidFlood, fluidSpread, lava
        zeroFunc, zeroFunc,                       // temperature, vegetation
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,  // continents, erosion, depth, ridges
        zeroFunc, fullNoise,                      // prelimSurface, finalDensity
        zeroFunc, zeroFunc, zeroFunc             // veinToggle, veinRidged, veinGap
    );
}

// Java: NoiseRouterData.nether() line 267-269
NoiseRouter* NoiseRouterData::nether() {
    return noNewCaves(slideNetherLike(0, 128));
}

// Java: NoiseRouterData.caves() line 271-273
NoiseRouter* NoiseRouterData::caves() {
    return noNewCaves(slideNetherLike(-64, 192));
}

// Java: NoiseRouterData.floatingIslands() line 275-277
NoiseRouter* NoiseRouterData::floatingIslands() {
    return noNewCaves(slideEndLike(getBase3DNoiseEnd(), 0, 256));
}

// Java: NoiseRouterData.end() line 283-287
NoiseRouter* NoiseRouterData::end() {
    DensityFunction* islands = cache2d(endIslands(0L));
    DensityFunction* fullNoise = postProcess(slideEnd(getSlopedCheeseEnd()));
    DensityFunction* zeroFunc = zero();

    return new NoiseRouter(
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,  // barrier, fluidFlood, fluidSpread, lava
        zeroFunc, zeroFunc, zeroFunc,             // temp, vegetation, continents
        islands,                                   // erosion (used for islands in End!)
        zeroFunc, zeroFunc,                       // depth, ridges
        zeroFunc, fullNoise,                       // prelimSurface, finalDensity
        zeroFunc, zeroFunc, zeroFunc              // veinToggle, veinRidged, veinGap
    );
}

// Java: NoiseRouterData.none() line 289-291
NoiseRouter* NoiseRouterData::none() {
    DensityFunction* zeroFunc = zero();
    return new NoiseRouter(
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,
        zeroFunc, zeroFunc, zeroFunc, zeroFunc,
        zeroFunc, zeroFunc, zeroFunc
    );
}

// Helper: Get base 3D noise for nether
// Java: BlendedNoise.createUnseeded(0.25, 0.375, 80.0, 60.0, 8.0) - line 83
DensityFunction* NoiseRouterData::getBase3DNoiseNether() {
    return DensityFunctionRegistry::instance().getOrThrow("nether/base_3d_noise");
}

// Helper: Get base 3D noise for end
// Java: BlendedNoise.createUnseeded(0.25, 0.25, 80.0, 160.0, 4.0) - line 84
DensityFunction* NoiseRouterData::getBase3DNoiseEnd() {
    return DensityFunctionRegistry::instance().getOrThrow("end/base_3d_noise");
}

// Helper: Get sloped cheese for end dimension
// Java: add(endIslands(0L), base3dNoiseEnd) - line 95
DensityFunction* NoiseRouterData::getSlopedCheeseEnd() {
    return add(endIslands(0L), getBase3DNoiseEnd());
}

// ============================================================================
// QuantizedSpaghettiRarity implementation
// Java: NoiseRouterData.QuantizedSpaghettiRarity lines 324-346
// ============================================================================

double NoiseRouterData::QuantizedSpaghettiRarity::getSphaghettiRarity2D(double rarityFactor) {
    if (rarityFactor < -0.75) {
        return 0.5;
    } else if (rarityFactor < -0.5) {
        return 0.75;
    } else if (rarityFactor < 0.5) {
        return 1.0;
    } else {
        return rarityFactor < 0.75 ? 2.0 : 3.0;
    }
}

double NoiseRouterData::QuantizedSpaghettiRarity::getSpaghettiRarity3D(double rarityFactor) {
    if (rarityFactor < -0.5) {
        return 0.75;
    } else if (rarityFactor < 0.0) {
        return 1.0;
    } else {
        return rarityFactor < 0.5 ? 1.5 : 2.0;
    }
}

} // namespace levelgen
} // namespace minecraft
