#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/DensityFunctions.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/TerrainProvider.h"
#include "synth/BlendedNoise.h"
#include "synth/NormalNoise.h"
#include "random/XoroshiroRandomSource.h"
#include "random/RandomSupport.h"
#include <cmath>
#include <iostream>

// For accessing Spline::Point and Spline::Coordinate
using Spline = minecraft::density::DensityFunctions::Spline;
using SplinePoint = Spline::Point;
using SplineCoordinate = Spline::Coordinate;

namespace minecraft {
namespace levelgen {

using namespace minecraft::density;
using namespace minecraft::density::DensityFunctions;

// GLOBAL_OFFSET constant - matches Java's NoiseRouterData line 29
// Java: private static final float GLOBAL_OFFSET = -0.50375F;
// Must use (double)(-0.50375f) to match Java's float-to-double conversion
static const double GLOBAL_OFFSET_D = static_cast<double>(-0.50375f);

DensityFunctionRegistry::~DensityFunctionRegistry() {
    // Functions are owned by the registry
    // NOTE: Cleanup temporarily disabled due to complex ownership with splines
    // TODO: Implement proper memory management with smart pointers
    /*
    for (auto& pair : m_functions) {
        delete pair.second;
    }
    */
}

DensityFunctionRegistry& DensityFunctionRegistry::instance() {
    static DensityFunctionRegistry registry;
    return registry;
}

void DensityFunctionRegistry::registerFunction(const std::string& name, DensityFunction* function) {
    m_functions[name] = function;
}

DensityFunction* DensityFunctionRegistry::getOrThrow(const std::string& name) const {
    auto it = m_functions.find(name);
    if (it == m_functions.end()) {
        throw std::runtime_error("Density function not found: " + name);
    }
    return it->second;
}

bool DensityFunctionRegistry::has(const std::string& name) const {
    return m_functions.find(name) != m_functions.end();
}

void DensityFunctionRegistry::clear() {
    for (auto& pair : m_functions) {
        delete pair.second;
    }
    m_functions.clear();
}

// Static variables to cache the positional random factory seeds
// These are initialized once per bootstrap and reused for all noises
static uint64_t g_positionalSeedLo = 0;
static uint64_t g_positionalSeedHi = 0;

// Static cache for NoiseHolder instances (matches Java's RandomState.noiseIntances)
// Each noise name maps to exactly ONE NormalNoise instance
static std::unordered_map<std::string, DensityFunction::NoiseHolder*> g_noiseCache;

// Helper: Create NormalNoise and wrap in DensityFunction::NoiseHolder
// This matches Java's RandomState.getOrCreateNoise() with caching
DensityFunction::NoiseHolder* DensityFunctionRegistry::createNoiseHolder(const std::string& noiseName, int64_t worldSeed) {
    // Check cache first (matches Java's computeIfAbsent)
    auto it = g_noiseCache.find(noiseName);
    if (it != g_noiseCache.end()) {
        return it->second;
    }

    NoiseRegistry& noiseReg = NoiseRegistry::instance();
    const NoiseRegistry::NoiseParameters& params = noiseReg.getOrThrow(noiseName);

    // Minecraft's noise seeding process:
    // 1. Create XoroshiroRandomSource from world seed (done ONCE per bootstrap)
    // 2. Fork it positionally to get PositionalRandomFactory (done ONCE per bootstrap)
    // 3. Use fromHashOf("minecraft:noise_name") to get noise-specific random
    // Reference: RandomState.java getOrCreateNoise() -> Noises.instantiate()

    // Use cached positional seeds (initialized in bootstrap())
    // These are the same for ALL noises from the same world seed
    uint64_t seedLo = g_positionalSeedLo;
    uint64_t seedHi = g_positionalSeedHi;


    // Step 3: Use fromHashOf with full resource identifier "minecraft:noise_name"
    std::string resourceId = "minecraft:" + noiseName;
    Seed128bit hashSeed = RandomSupport::seedFromHashOf(resourceId);
    Seed128bit xoredSeed = hashSeed.xor_(seedLo, seedHi);

    // Create the noise-specific random source
    XoroshiroRandomSource noiseRandom(xoredSeed.seedLo, xoredSeed.seedHi);

    // Create the noise
    NormalNoise* noise = new NormalNoise(NormalNoise::create(noiseRandom, params.firstOctave, params.amplitudes));

    // Wrap in holder
    DensityFunction::NoiseHolder* holder = new DensityFunction::NoiseHolder(noise);

    // Cache it (matches Java's noiseIntances.put())
    g_noiseCache[noiseName] = holder;

    return holder;
}

/**
 * registerTerrainNoises - Helper to register terrain spline-based functions
 * Reference: NoiseRouterData.java lines 104-116
 *
 * @param registry The density function registry
 * @param prefix Prefix for registered functions ("overworld", "overworld_large_biomes", "overworld_amplified")
 * @param continents Continentalness density function
 * @param erosion Erosion density function
 * @param weirdness Weirdness density function (ridges_folded)
 * @param ridges Ridge density function
 * @param jaggedNoise Jagged noise density function
 * @param base3dNoise Base 3D noise (BlendedNoise) for this dimension
 * @param amplified Whether to use amplified terrain transformations
 */
static void registerTerrainNoises(
    DensityFunctionRegistry& registry,
    const std::string& prefix,
    DensityFunction* continents,
    DensityFunction* erosion,
    DensityFunction* weirdness,
    DensityFunction* ridges,
    DensityFunction* jaggedNoise,
    DensityFunction* base3dNoise,
    bool amplified)
{
    // using ::levelgen::TerrainProvider;

    // Wrap density functions in Spline::Coordinate
    // Create separate instances for each spline to avoid sharing pointers
    // NOTE: These are leaked but that's acceptable for singleton registry

    // For offsetSpline
    SplineCoordinate* continentsCoordOffset = new SplineCoordinate(continents);
    SplineCoordinate* erosionCoordOffset = new SplineCoordinate(erosion);
    SplineCoordinate* ridgesCoordOffset = new SplineCoordinate(ridges);

    // For factorSpline
    SplineCoordinate* continentsCoordFactor = new SplineCoordinate(continents);
    SplineCoordinate* erosionCoordFactor = new SplineCoordinate(erosion);
    SplineCoordinate* weirnessCoordFactor = new SplineCoordinate(weirdness);
    SplineCoordinate* ridgesCoordFactor = new SplineCoordinate(ridges);

    // For jaggednessSpline
    SplineCoordinate* continentsCoordJagg = new SplineCoordinate(continents);
    SplineCoordinate* erosionCoordJagg = new SplineCoordinate(erosion);
    SplineCoordinate* weirnessCoordJagg = new SplineCoordinate(weirdness);
    SplineCoordinate* ridgesCoordJagg = new SplineCoordinate(ridges);

    // Create the terrain splines using TerrainProvider
    // Reference: NoiseRouterData.java lines 112-114
    auto* offsetSpline = TerrainProvider::overworldOffset(
        continentsCoordOffset, erosionCoordOffset, ridgesCoordOffset, amplified);
    auto* factorSpline = TerrainProvider::overworldFactor(
        continentsCoordFactor, erosionCoordFactor, weirnessCoordFactor, ridgesCoordFactor, amplified);
    auto* jaggednessSpline = TerrainProvider::overworldJaggedness(
        continentsCoordJagg, erosionCoordJagg, weirnessCoordJagg, ridgesCoordJagg, amplified);

    // Wrap splines as DensityFunctions
    DensityFunction* offsetSplineFunc = spline(offsetSpline);
    DensityFunction* factor = spline(factorSpline);
    DensityFunction* jaggedness = spline(jaggednessSpline);

    // Build offset with GLOBAL_OFFSET and blending
    // Reference: NoiseRouterData.java line 109
    // Java: offset = splineWithBlending(GLOBAL_OFFSET + spline(overworldOffset), blendOffset())
    DensityFunction* offsetWithGlobal = add(constant(GLOBAL_OFFSET_D), offsetSplineFunc);
    DensityFunction* offset = DensityFunctionRegistry::splineWithBlending(offsetWithGlobal, blendOffset());

    // Register the functions
    registry.registerFunction(prefix + "/offset", offset);
    registry.registerFunction(prefix + "/factor", factor);
    registry.registerFunction(prefix + "/jaggedness", jaggedness);

    // Calculate depth from offset (Java line 115)
    DensityFunction* depth = DensityFunctionRegistry::offsetToDepth(offset);
    registry.registerFunction(prefix + "/depth", depth);

    // Calculate sloped_cheese (Java line 116)
    // Java: DensityFunction initialDensity = noiseGradientDensity(factor, DensityFunctions.add(depth, jaggedness));
    //       context.register(slopedCheeseName, DensityFunctions.add(initialDensity, getFunction(functions, BASE_3D_NOISE_OVERWORLD)));
    DensityFunction* depthWithJaggedness = add(depth, mul(jaggedness, jaggedNoise));
    DensityFunction* initialDensity = DensityFunctionRegistry::noiseGradientDensity(factor, depthWithJaggedness);
    DensityFunction* slopedCheese = add(initialDensity, base3dNoise);
    registry.registerFunction(prefix + "/sloped_cheese", slopedCheese);
}

/**
 * Bootstrap the density function registry
 * Reference: NoiseRouterData.java lines 73-102
 *
 * @param worldSeed The world seed for noise generation
 */
void DensityFunctionRegistry::bootstrap(int64_t worldSeed) {
    DensityFunctionRegistry& registry = instance();
    NoiseRegistry& noiseReg = NoiseRegistry::instance();

    // Store world seed for later use (e.g., NoiseRouterData::overworld())
    registry.m_worldSeed = worldSeed;

    // Clear any existing functions
    registry.clear();

    // Clear noise cache (matches Java's RandomState constructor creating new noiseIntances map)
    g_noiseCache.clear();

    // Initialize positional random factory seeds (used by ALL noises)
    // This matches Java's RandomState.random.forkPositional()
    XoroshiroRandomSource baseRandom(worldSeed);
    g_positionalSeedLo = baseRandom.nextLong();
    g_positionalSeedHi = baseRandom.nextLong();

    // Positional seeds initialized for all noises

    // Line 76: ZERO = DensityFunctions.zero()
    registry.registerFunction("zero", zero());

    // Line 79: Y = DensityFunctions.yClampedGradient(belowBottom, aboveTop, belowBottom, aboveTop)
    // belowBottom = DimensionType.MIN_Y * 2 = -64 * 2 = -128
    // aboveTop = DimensionType.MAX_Y * 2 = 320 * 2 = 640
    int belowBottom = -64 * 2;  // -128
    int aboveTop = 320 * 2;     // 640
    registry.registerFunction("y", yClampedGradient(belowBottom, aboveTop, static_cast<double>(belowBottom), static_cast<double>(aboveTop)));

    // Line 80-81: SHIFT_X and SHIFT_Z
    // SHIFT_X = flatCache(cache2d(shiftA(noises.getOrThrow(Noises.SHIFT))))
    DensityFunction::NoiseHolder* shiftNoise = createNoiseHolder("offset", worldSeed);  // "offset" is SHIFT
    DensityFunction* shiftAFunc = shiftA(shiftNoise);
    DensityFunction* shiftX = flatCache(cache2d(shiftAFunc));
    registry.registerFunction("shift_x", shiftX);

    DensityFunction* shiftBFunc = shiftB(shiftNoise);
    DensityFunction* shiftZ = flatCache(cache2d(shiftBFunc));
    registry.registerFunction("shift_z", shiftZ);

    // Line 82-84: BASE_3D_NOISE for different dimensions
    // Reference: NoiseRouterData.java lines 82-84
    //
    // IMPORTANT: In Java, BlendedNoise.createUnseeded() creates with seed 0L during bootstrap,
    // but RandomState.create() runs a visitor that re-seeds BlendedNoise with:
    //   random.fromHashOf("minecraft:terrain")
    // where random = XoroshiroRandomSource(worldSeed).forkPositional()
    //
    // We apply the same seeding here directly to get correct terrain noise.

    // Create the positional random factory from world seed (like RandomState does)
    Seed128bit terrainSeedPair = RandomSupport::upgradeSeedTo128bit(worldSeed);
    XoroshiroRandomSource terrainBaseRandom(terrainSeedPair.seedLo, terrainSeedPair.seedHi);
    XoroshiroPositionalRandomFactory terrainFactory = terrainBaseRandom.forkPositional();

    // Get terrain random: random.fromHashOf("minecraft:terrain")
    XoroshiroRandomSource terrainRandom = terrainFactory.fromHashOf("minecraft:terrain");

    // Overworld BlendedNoise: xzScale=0.25, yScale=0.125, xzFactor=80.0, yFactor=160.0, smearScaleMultiplier=8.0
    BlendedNoise* overworldBase3dNoise = new BlendedNoise(terrainRandom, 0.25, 0.125, 80.0, 160.0, 8.0);
    registry.registerFunction("overworld/base_3d_noise", overworldBase3dNoise);

    // Nether and End also use terrain random (same visitor applies to all BlendedNoise)
    // Re-create terrain random for each (consuming random state like Java does)
    XoroshiroRandomSource terrainRandomNether = terrainFactory.fromHashOf("minecraft:terrain");
    BlendedNoise* netherBase3dNoise = new BlendedNoise(terrainRandomNether, 0.25, 0.375, 80.0, 60.0, 8.0);
    registry.registerFunction("nether/base_3d_noise", netherBase3dNoise);

    XoroshiroRandomSource terrainRandomEnd = terrainFactory.fromHashOf("minecraft:terrain");
    BlendedNoise* endBase3dNoise = new BlendedNoise(terrainRandomEnd, 0.25, 0.25, 80.0, 160.0, 4.0);
    registry.registerFunction("end/base_3d_noise", endBase3dNoise);

    // Line 85: CONTINENTS = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, continentalness noise))
    DensityFunction::NoiseHolder* continentalnessNoise = createNoiseHolder("continentalness", worldSeed);
    DensityFunction* continents = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, continentalnessNoise));
    registry.registerFunction("overworld/continents", continents);

    // Line 86: EROSION = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, erosion noise))
    DensityFunction::NoiseHolder* erosionNoise = createNoiseHolder("erosion", worldSeed);
    DensityFunction* erosion = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, erosionNoise));
    registry.registerFunction("overworld/erosion", erosion);

    // Line 87: RIDGES = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, ridge noise))
    DensityFunction::NoiseHolder* ridgeNoise = createNoiseHolder("ridge", worldSeed);
    DensityFunction* ridge = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, ridgeNoise));
    registry.registerFunction("overworld/ridges", ridge);

    // Line 88: RIDGES_FOLDED = peaksAndValleys(ridge)
    // peaksAndValleys formula: -(abs(abs(weirdness) - 0.6666667) - 0.33333334) * 3.0
    // = mul(add(add(abs(weirdness), constant(-0.666...)).abs(), constant(-0.333...)), constant(-3.0))
    DensityFunction* ridgesFolded = mul(
        add(
            abs(add(abs(ridge), constant(-0.6666666666666666))),
            constant(-0.3333333333333333)
        ),
        constant(-3.0)
    );
    registry.registerFunction("overworld/ridges_folded", ridgesFolded);

    // Line 89: jaggedNoise = noise(JAGGED, 1500.0, 0.0)
    DensityFunction::NoiseHolder* jaggedNoiseHolder = createNoiseHolder("jagged", worldSeed);
    DensityFunction* jaggedNoise = noise(jaggedNoiseHolder, 1500.0, 0.0);

    // Line 90-94: Register terrain spline functions for normal overworld
    // Uses TerrainProvider splines with Spline::Point and Spline::Coordinate wrappers
    registerTerrainNoises(registry, "overworld", continents, erosion, ridgesFolded, ridge, jaggedNoise, overworldBase3dNoise, false);

    // Large biomes versions - uses different noise parameters
    DensityFunction::NoiseHolder* continentalnessLargeNoise = createNoiseHolder("continentalness_large", worldSeed);
    DensityFunction* continentsLarge = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, continentalnessLargeNoise));
    registry.registerFunction("overworld_large_biomes/continents", continentsLarge);

    DensityFunction::NoiseHolder* erosionLargeNoise = createNoiseHolder("erosion_large", worldSeed);
    DensityFunction* erosionLarge = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, erosionLargeNoise));
    registry.registerFunction("overworld_large_biomes/erosion", erosionLarge);

    // Register terrain splines for large biomes (uses large noise parameters, same base_3d_noise)
    registerTerrainNoises(registry, "overworld_large_biomes", continentsLarge, erosionLarge, ridgesFolded, ridge, jaggedNoise, overworldBase3dNoise, false);

    // Amplified versions - uses same noise but with amplified transformations
    registerTerrainNoises(registry, "overworld_amplified", continents, erosion, ridgesFolded, ridge, jaggedNoise, overworldBase3dNoise, true);

    // Register cave-related functions (Java lines 138-204)
    // These are now fully implemented matching Java exactly
    // Pass world seed to create proper noise holders
    registry.registerFunction("spaghetti_roughness_function", NoiseRouterData::spaghettiRoughnessFunction(worldSeed));
    registry.registerFunction("spaghetti_2d_thickness_modulator", NoiseRouterData::spaghetti2DThicknessModulator(worldSeed));
    registry.registerFunction("spaghetti_2d", NoiseRouterData::spaghetti2D(worldSeed));
    registry.registerFunction("entrances", NoiseRouterData::entrances(worldSeed));
    registry.registerFunction("noodle", NoiseRouterData::noodle(worldSeed));
    registry.registerFunction("pillars", NoiseRouterData::pillars(worldSeed));

    // Line 95: SLOPED_CHEESE_END
    // Java: add(endIslands(0L), base3dNoiseEnd)
    // endIslands uses seed 0L for the inner SimplexNoise (unseeded)
    DensityFunction* endIslandsFunc = endIslands(0L);
    DensityFunction* base3dNoiseEnd = registry.getOrThrow("end/base_3d_noise");
    registry.registerFunction("end/sloped_cheese", add(endIslandsFunc, base3dNoiseEnd));
}

// Helper: splineWithBlending
// Reference: NoiseRouterData.java lines 293-296
DensityFunction* DensityFunctionRegistry::splineWithBlending(DensityFunction* splineFunc, DensityFunction* blendingTarget) {
    DensityFunction* blendedSpline = lerp(blendAlpha(), blendingTarget, splineFunc);
    return flatCache(cache2d(blendedSpline));
}

// Helper: offsetToDepth
// Reference: NoiseRouterData.java lines 118-120
DensityFunction* DensityFunctionRegistry::offsetToDepth(DensityFunction* offset) {
    return add(yClampedGradient(-64, 320, 1.5, -1.5), offset);
}

// Helper: noiseGradientDensity
// Reference: NoiseRouterData.java lines 298-300
DensityFunction* DensityFunctionRegistry::noiseGradientDensity(DensityFunction* factor, DensityFunction* depthWithJaggedness) {
    DensityFunction* gradientUnscaled = mul(depthWithJaggedness, factor);
    return mul(constant(4.0), quarterNegative(gradientUnscaled));
}

} // namespace levelgen
} // namespace minecraft
