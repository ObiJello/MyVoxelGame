/**
 * Underground Trace Test
 *
 * Traces the underground cave function in detail to find the divergence.
 */

#include <iostream>
#include <iomanip>

#include "levelgen/DensityFunctions.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/TerrainProvider.h"
#include "levelgen/Blender.h"

using namespace minecraft::levelgen;
using namespace minecraft::density;
using namespace minecraft::density::DensityFunctions;

static const int64_t SEED = 12345L;

class TestContext : public DensityFunction::FunctionContext {
private:
    int m_blockX, m_blockY, m_blockZ;
public:
    TestContext(int x, int y, int z) : m_blockX(x), m_blockY(y), m_blockZ(z) {}
    int blockX() const override { return m_blockX; }
    int blockY() const override { return m_blockY; }
    int blockZ() const override { return m_blockZ; }
    minecraft::Blender* getBlender() const override { return nullptr; }
};

void trace(const std::string& name, DensityFunction* func, const TestContext& ctx) {
    double value = func->compute(ctx);
    std::cout << std::fixed << std::setprecision(15);
    std::cout << name << ": " << value << std::endl;
}

int main(int argc, char* argv[]) {
    int testX = (argc > 1) ? std::stoi(argv[1]) : 32;
    int testY = (argc > 2) ? std::stoi(argv[2]) : -56;
    int testZ = (argc > 3) ? std::stoi(argv[3]) : 96;

    std::cout << "=== Underground Trace Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Position: (" << testX << ", " << testY << ", " << testZ << ")" << std::endl;
    std::cout << std::endl;

    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);
    DensityFunctionRegistry& dfReg = DensityFunctionRegistry::instance();

    TestContext ctx(testX, testY, testZ);
    std::cout << std::fixed << std::setprecision(15);

    // Build slopedCheese (same as before - we know this matches)
    DensityFunction::NoiseHolder* offsetNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("offset", SEED);
    DensityFunction* shiftX = flatCache(cache2d(shiftA(offsetNoiseHolder)));
    DensityFunction* shiftZ = flatCache(cache2d(shiftB(offsetNoiseHolder)));

    DensityFunction::NoiseHolder* contNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("continentalness", SEED);
    DensityFunction* continents = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, contNoiseHolder));

    DensityFunction::NoiseHolder* erosionNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("erosion", SEED);
    DensityFunction* erosion = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, erosionNoiseHolder));

    DensityFunction::NoiseHolder* ridgeNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("ridge", SEED);
    DensityFunction* ridges = flatCache(shiftedNoise2d(shiftX, shiftZ, 0.25, ridgeNoiseHolder));

    DensityFunction* ridgesFolded = NoiseRouterData::peaksAndValleys(ridges);

    TerrainProvider::Coordinate* continentsCoord = new TerrainProvider::Coordinate(continents);
    TerrainProvider::Coordinate* erosionCoord = new TerrainProvider::Coordinate(erosion);
    TerrainProvider::Coordinate* weirdnessCoord = new TerrainProvider::Coordinate(ridges);
    TerrainProvider::Coordinate* ridgesFoldedCoord = new TerrainProvider::Coordinate(ridgesFolded);

    double GLOBAL_OFFSET = static_cast<double>(-0.50375f);
    TerrainProvider::SplineType* offsetSpline = TerrainProvider::overworldOffset(
        continentsCoord, erosionCoord, ridgesFoldedCoord, false);
    DensityFunction* offset = NoiseRouterData::splineWithBlending(
        add(constant(GLOBAL_OFFSET), spline(offsetSpline)), blendOffset());

    TerrainProvider::SplineType* factorSpline = TerrainProvider::overworldFactor(
        continentsCoord, erosionCoord, weirdnessCoord, ridgesFoldedCoord, false);
    DensityFunction* factor = NoiseRouterData::splineWithBlending(spline(factorSpline), constant(10.0));

    DensityFunction* depth = NoiseRouterData::offsetToDepth(offset);

    TerrainProvider::SplineType* jaggednessSpline = TerrainProvider::overworldJaggedness(
        continentsCoord, erosionCoord, weirdnessCoord, ridgesFoldedCoord, false);
    DensityFunction::NoiseHolder* jaggedNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("jagged", SEED);
    DensityFunction* jaggedness = mul(
        NoiseRouterData::splineWithBlending(spline(jaggednessSpline), zero()),
        halfNegative(noise(jaggedNoiseHolder, 1500.0, 0.0)));

    DensityFunction* depthWithJaggedness = add(depth, jaggedness);
    DensityFunction* initialDensity = NoiseRouterData::noiseGradientDensity(factor, depthWithJaggedness);
    DensityFunction* base3dNoise = dfReg.getOrThrow("overworld/base_3d_noise");
    DensityFunction* slopedCheese = add(initialDensity, base3dNoise);

    std::cout << "=== SLOPED CHEESE (should match Java: 12.926561946006322) ===" << std::endl;
    trace("slopedCheese", slopedCheese, ctx);

    // ========================================================================
    // Now trace UNDERGROUND function in detail
    // Reference: NoiseRouterData.underground() lines 192-204
    // ========================================================================
    std::cout << std::endl << "=== UNDERGROUND COMPONENTS ===" << std::endl;

    // spaghetti2D
    DensityFunction* spaghetti2DFunc = NoiseRouterData::spaghetti2D(SEED);
    trace("spaghetti2D", spaghetti2DFunc, ctx);

    // spaghettiRoughness
    DensityFunction* spaghettiRoughnessFunc = NoiseRouterData::spaghettiRoughnessFunction(SEED);
    trace("spaghettiRoughness", spaghettiRoughnessFunc, ctx);

    // spaghetti2D + spaghettiRoughness
    DensityFunction* spaghetti2DWithRoughness = add(spaghetti2DFunc, spaghettiRoughnessFunc);
    trace("spaghetti2D + spaghettiRoughness", spaghetti2DWithRoughness, ctx);

    // layerNoiseSource = noise(CAVE_LAYER, 8.0)
    DensityFunction::NoiseHolder* layerNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("cave_layer", SEED);
    DensityFunction* layerNoiseSource = noise(layerNoiseHolder, 8.0, 1.0);
    trace("layerNoiseSource (cave_layer)", layerNoiseSource, ctx);

    // layerizedCavernsFunction = 4.0 * square(layerNoiseSource)
    DensityFunction* layerizedCaverns = mul(constant(4.0), square(layerNoiseSource));
    trace("layerizedCaverns", layerizedCaverns, ctx);

    // cheese = noise(CAVE_CHEESE, 0.6666666666666666)
    DensityFunction::NoiseHolder* cheeseNoiseHolder =
        DensityFunctionRegistry::createNoiseHolder("cave_cheese", SEED);
    DensityFunction* cheese = noise(cheeseNoiseHolder, 0.6666666666666666, 1.0);
    trace("cheese (cave_cheese)", cheese, ctx);

    // solidifedCheeseWithTopSlide = clamp(0.27 + cheese, -1, 1) + clamp(1.5 + -0.64 * slopedCheese, 0, 0.5)
    DensityFunction* cheeseWithOffset = add(constant(0.27), cheese);
    trace("0.27 + cheese", cheeseWithOffset, ctx);

    DensityFunction* clampedCheese = clamp(cheeseWithOffset, -1.0, 1.0);
    trace("clamp(0.27 + cheese, -1, 1)", clampedCheese, ctx);

    DensityFunction* topSlidePart = add(constant(1.5), mul(constant(-0.64), slopedCheese));
    trace("1.5 + -0.64 * slopedCheese", topSlidePart, ctx);

    DensityFunction* clampedTopSlide = clamp(topSlidePart, 0.0, 0.5);
    trace("clamp(topSlide, 0, 0.5)", clampedTopSlide, ctx);

    DensityFunction* solidifiedCheese = add(clampedCheese, clampedTopSlide);
    trace("solidifiedCheese", solidifiedCheese, ctx);

    // baseCaveDensity = layerizedCavernsFunction + solidifedCheeseWithTopSlide
    DensityFunction* baseCaveDensity = add(layerizedCaverns, solidifiedCheese);
    trace("baseCaveDensity", baseCaveDensity, ctx);

    // entrances
    DensityFunction* entrancesFunc = NoiseRouterData::entrances(SEED);
    trace("entrances", entrancesFunc, ctx);

    // min(baseCaveDensity, entrances)
    DensityFunction* minBaseCaveEntrances = min(baseCaveDensity, entrancesFunc);
    trace("min(baseCaveDensity, entrances)", minBaseCaveEntrances, ctx);

    // min(min(baseCaveDensity, entrances), spaghetti2D + spaghettiRoughness)
    DensityFunction* undergroundSubtractions = min(minBaseCaveEntrances, spaghetti2DWithRoughness);
    trace("undergroundSubtractions (min of all caves)", undergroundSubtractions, ctx);

    // pillars
    DensityFunction* pillarsFunc = NoiseRouterData::pillars(SEED);
    trace("pillars", pillarsFunc, ctx);

    // pillarsWithCutoff = rangeChoice(pillars, -1000000, 0.03, constant(-1000000), pillars)
    DensityFunction* pillarsWithCutoff = rangeChoice(pillarsFunc, -1000000.0, 0.03, constant(-1000000.0), pillarsFunc);
    trace("pillarsWithCutoff", pillarsWithCutoff, ctx);

    // underground = max(undergroundSubtractions, pillarsWithCutoff)
    DensityFunction* underground = max(undergroundSubtractions, pillarsWithCutoff);
    trace("underground = max(subtractions, pillars)", underground, ctx);

    // For comparison, use the helper function
    std::cout << std::endl << "=== COMPARISON ===" << std::endl;
    DensityFunction* undergroundFromHelper = NoiseRouterData::underground(SEED, slopedCheese);
    trace("underground (from helper function)", undergroundFromHelper, ctx);

    // Also test at (0, -56, 0)
    std::cout << std::endl << "=== AT (0, -56, 0) ===" << std::endl;
    TestContext ctx00(0, testY, 0);
    trace("underground at (0,y,0)", undergroundFromHelper, ctx00);

    std::cout << std::endl << "Done." << std::endl;
    return 0;
}
