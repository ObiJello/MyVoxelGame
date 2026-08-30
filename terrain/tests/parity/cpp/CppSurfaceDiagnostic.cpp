/**
 * Comprehensive Surface Diagnostic Test
 * Tests ALL values at specific mismatch coordinates to identify parity issues
 *
 * Test positions (from chunk 45,26 comparison):
 * Position 1: (720, 48, 430) - C++ packed_ice, Java water
 * Position 2: (728, 48, 426) - C++ water, Java packed_ice
 * Position 3: (732, 49, 430) - C++ sandstone, Java stone
 */

#include <iostream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <functional>
#include <memory>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"
#include "synth/NormalNoise.h"
#include "random/XoroshiroRandomSource.h"
#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Climate.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "core/BlockPos.h"
#include "core/QuartPos.h"
#include "math/Mth.h"

using namespace minecraft;
using namespace minecraft::levelgen;
using namespace minecraft::world;
using namespace minecraft::world::biome;

static const int64_t SEED = 12345L;
static const int CHUNK_X = 45;
static const int CHUNK_Z = 26;
static const int32_t SEA_LEVEL = 63;

// Test positions
struct TestPosition {
    int32_t x, y, z;
    const char* expectedCpp;
    const char* expectedJava;
};

static TestPosition testPositions[] = {
    {720, 48, 430, "packed_ice", "water"},
    {728, 48, 426, "water", "packed_ice"},
    {732, 49, 430, "sandstone", "stone"},
};

void printSeparator(const char* title) {
    std::cout << "\n========================================" << std::endl;
    std::cout << title << std::endl;
    std::cout << "========================================" << std::endl;
}

void printSubSection(const char* title) {
    std::cout << "\n--- " << title << " ---" << std::endl;
}

int main() {
    std::cout << std::fixed << std::setprecision(15);

    printSeparator("COMPREHENSIVE SURFACE DIAGNOSTIC");
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << "Base coordinates: (" << (CHUNK_X * 16) << ", " << (CHUNK_Z * 16) << ")" << std::endl;

    // Initialize registries
    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);
    SurfaceRuleData::initialize();

    // Create RandomState
    NoiseRouter* router = NoiseRouterData::overworld(false, false);
    NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    RandomState* randomState = RandomState::create(settings, SEED);

    // Get the positional random factory
    XoroshiroRandomSource baseRandom(SEED);
    XoroshiroPositionalRandomFactory noiseRandom = baseRandom.forkPositional();

    // Get all relevant noises
    NormalNoise* icebergSurfaceNoise = randomState->getOrCreateNoise("iceberg_surface");
    NormalNoise* icebergPillarNoise = randomState->getOrCreateNoise("iceberg_pillar");
    NormalNoise* icebergPillarRoofNoise = randomState->getOrCreateNoise("iceberg_pillar_roof");
    NormalNoise* surfaceNoise = randomState->getOrCreateNoise("surface");
    NormalNoise* surfaceSecondaryNoise = randomState->getOrCreateNoise("surface_secondary");

    // Create biome source for biome lookups
    MultiNoiseBiomeSource biomeSource(MultiNoiseBiomeSource::Preset::OVERWORLD);
    Climate::Sampler* sampler = randomState->sampler();

    // Test each position
    for (const auto& pos : testPositions) {
        printSeparator("TESTING POSITION");
        std::cout << "Block: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
        std::cout << "Local: (" << (pos.x & 15) << ", " << pos.y << ", " << (pos.z & 15) << ")" << std::endl;
        std::cout << "Expected C++: " << pos.expectedCpp << std::endl;
        std::cout << "Expected Java: " << pos.expectedJava << std::endl;

        int32_t blockX = pos.x;
        int32_t blockZ = pos.z;
        int32_t blockY = pos.y;

        // ============================================
        // SECTION 1: BIOME LOOKUP
        // ============================================
        printSubSection("1. BIOME LOOKUP");

        int32_t quartX = core::QuartPos::fromBlock(blockX);
        int32_t quartY = core::QuartPos::fromBlock(blockY);
        int32_t quartZ = core::QuartPos::fromBlock(blockZ);

        std::cout << "Quart coords: (" << quartX << ", " << quartY << ", " << quartZ << ")" << std::endl;

        BiomeKey biomeKey = biomeSource.getNoiseBiome(quartX, quartY, quartZ, *sampler);
        std::cout << "Biome: " << biomeKey << std::endl;

        bool isFrozenOcean = (biomeKey == BiomeKeys::FROZEN_OCEAN ||
                              biomeKey == BiomeKeys::DEEP_FROZEN_OCEAN);
        std::cout << "Is frozen ocean: " << (isFrozenOcean ? "true" : "false") << std::endl;

        // Also check biome at Y=0
        BiomeKey biomeKeyY0 = biomeSource.getNoiseBiome(quartX, core::QuartPos::fromBlock(0), quartZ, *sampler);
        std::cout << "Biome at Y=0: " << biomeKeyY0 << std::endl;

        // ============================================
        // SECTION 2: ICEBERG NOISE VALUES
        // ============================================
        printSubSection("2. ICEBERG NOISE VALUES");

        double icebergSurfaceVal = icebergSurfaceNoise->getValue(
            static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
        std::cout << "icebergSurfaceNoise.getValue(" << blockX << ", 0, " << blockZ << ") = "
                  << icebergSurfaceVal << std::endl;

        double pillarScale = 1.28;
        double icebergPillarVal = icebergPillarNoise->getValue(
            static_cast<double>(blockX) * pillarScale, 0.0,
            static_cast<double>(blockZ) * pillarScale);
        std::cout << "icebergPillarNoise.getValue(" << (blockX * pillarScale) << ", 0, "
                  << (blockZ * pillarScale) << ") = " << icebergPillarVal << std::endl;

        double roofScale = 1.17;
        double icebergPillarRoofVal = icebergPillarRoofNoise->getValue(
            static_cast<double>(blockX) * roofScale, 0.0,
            static_cast<double>(blockZ) * roofScale);
        std::cout << "icebergPillarRoofNoise.getValue(" << (blockX * roofScale) << ", 0, "
                  << (blockZ * roofScale) << ") = " << icebergPillarRoofVal << std::endl;

        // ============================================
        // SECTION 3: ICEBERG CALCULATIONS
        // ============================================
        printSubSection("3. ICEBERG INTERMEDIATE CALCULATIONS");

        double absSurface = std::abs(icebergSurfaceVal * 8.25);
        double pillarContrib = icebergPillarVal * 15.0;
        double iceberg = std::min(absSurface, pillarContrib);

        std::cout << "abs(surface * 8.25) = " << absSurface << std::endl;
        std::cout << "pillar * 15.0 = " << pillarContrib << std::endl;
        std::cout << "iceberg = min(" << absSurface << ", " << pillarContrib << ") = " << iceberg << std::endl;
        std::cout << "iceberg > 1.8: " << (iceberg > 1.8 ? "true (proceed)" : "false (skip extension)") << std::endl;

        if (iceberg > 1.8) {
            double roofAmplitude = 1.5;
            double icebergRoof = std::abs(icebergPillarRoofVal * roofAmplitude);
            std::cout << "icebergRoof = abs(" << icebergPillarRoofVal << " * 1.5) = " << icebergRoof << std::endl;

            double icebergSquared = iceberg * iceberg * 1.2;
            double roofContrib = std::ceil(icebergRoof * 40.0) + 14.0;
            double top = std::min(icebergSquared, roofContrib);

            std::cout << "iceberg^2 * 1.2 = " << icebergSquared << std::endl;
            std::cout << "ceil(icebergRoof * 40) + 14 = " << roofContrib << std::endl;
            std::cout << "top (before melt check) = " << top << std::endl;

            // Temperature check - get the biome and check temperature
            const Biome* biome = Biomes::get(biomeKey);
            if (biome) {
                core::BlockPos tempPos(blockX, SEA_LEVEL, blockZ);
                float temperature = biome->getTemperature(tempPos, SEA_LEVEL);
                std::cout << "Temperature at seaLevel: " << temperature << std::endl;
                std::cout << "shouldMelt (temp > 0.1f): " << (temperature > 0.1f ? "true" : "false") << std::endl;
                if (temperature > 0.1f) {
                    top -= 2.0;
                    std::cout << "top (after melt) = " << top << std::endl;
                }
            }

            double extensionBottom, extensionTop;
            if (top > 2.0) {
                extensionBottom = static_cast<double>(SEA_LEVEL) - top - 7.0;
                extensionTop = top + static_cast<double>(SEA_LEVEL);
            } else {
                extensionTop = 0.0;
                extensionBottom = 0.0;
            }

            std::cout << "extensionTop = " << extensionTop << std::endl;
            std::cout << "extensionBottom = " << extensionBottom << std::endl;
        }

        // ============================================
        // SECTION 4: RANDOM VALUES
        // ============================================
        printSubSection("4. RANDOM VALUES (noiseRandom.at)");

        // Test positional seed calculation
        int64_t positionalSeed = Mth::getSeed(blockX, 0, blockZ);
        std::cout << "Mth::getSeed(" << blockX << ", 0, " << blockZ << ") = " << positionalSeed << std::endl;

        XoroshiroRandomSource random = noiseRandom.at(blockX, 0, blockZ);

        // These are the values used in frozenOceanExtension
        int32_t maxSnowDepth = 2 + random.nextInt(4);
        int32_t minSnowHeight = SEA_LEVEL + 18 + random.nextInt(10);

        std::cout << "maxSnowDepth = 2 + nextInt(4) = " << maxSnowDepth << std::endl;
        std::cout << "minSnowHeight = " << SEA_LEVEL << " + 18 + nextInt(10) = " << minSnowHeight << std::endl;

        // Simulate the nextDouble() calls in the loop
        std::cout << "\nNext 5 nextDouble() values (used in placement loop):" << std::endl;
        for (int i = 0; i < 5; i++) {
            double val = random.nextDouble();
            std::cout << "  nextDouble() #" << i << " = " << val
                      << " (> 0.01: " << (val > 0.01 ? "true" : "false")
                      << ", > 0.15: " << (val > 0.15 ? "true" : "false") << ")" << std::endl;
        }

        // ============================================
        // SECTION 5: SURFACE DEPTH
        // ============================================
        printSubSection("5. SURFACE DEPTH");

        double surfaceNoiseVal = surfaceNoise->getValue(
            static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
        std::cout << "surfaceNoise.getValue(" << blockX << ", 0, " << blockZ << ") = "
                  << surfaceNoiseVal << std::endl;

        // Need fresh random for surface depth calculation
        XoroshiroRandomSource surfaceRandom = noiseRandom.at(blockX, 0, blockZ);
        double randomComponent = surfaceRandom.nextDouble() * 0.25;
        int32_t surfaceDepth = static_cast<int32_t>(surfaceNoiseVal * 2.75 + 3.0 + randomComponent);

        std::cout << "Random component (nextDouble() * 0.25) = " << randomComponent << std::endl;
        std::cout << "surfaceDepth = (int)(" << surfaceNoiseVal << " * 2.75 + 3.0 + "
                  << randomComponent << ") = " << surfaceDepth << std::endl;

        // ============================================
        // SECTION 6: SURFACE SECONDARY
        // ============================================
        printSubSection("6. SURFACE SECONDARY NOISE");

        double surfaceSecondaryVal = surfaceSecondaryNoise->getValue(
            static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
        std::cout << "surfaceSecondaryNoise.getValue(" << blockX << ", 0, " << blockZ << ") = "
                  << surfaceSecondaryVal << std::endl;

        // ============================================
        // SECTION 7: CLIMATE VALUES
        // ============================================
        printSubSection("7. CLIMATE TARGET POINT");

        // Sampler takes quart coordinates
        Climate::TargetPoint target = sampler->sample(quartX, quartY, quartZ);
        std::cout << "Climate at quart (" << quartX << ", " << quartY << ", " << quartZ << "):" << std::endl;
        std::cout << "  temperature: " << target.temperature << " (unquantized: " << Climate::unquantizeCoord(target.temperature) << ")" << std::endl;
        std::cout << "  humidity: " << target.humidity << " (unquantized: " << Climate::unquantizeCoord(target.humidity) << ")" << std::endl;
        std::cout << "  continentalness: " << target.continentalness << " (unquantized: " << Climate::unquantizeCoord(target.continentalness) << ")" << std::endl;
        std::cout << "  erosion: " << target.erosion << " (unquantized: " << Climate::unquantizeCoord(target.erosion) << ")" << std::endl;
        std::cout << "  depth: " << target.depth << " (unquantized: " << Climate::unquantizeCoord(target.depth) << ")" << std::endl;
        std::cout << "  weirdness: " << target.weirdness << " (unquantized: " << Climate::unquantizeCoord(target.weirdness) << ")" << std::endl;

        // ============================================
        // SECTION 8: BIOME LOOKUP AT VARIOUS HEIGHTS
        // ============================================
        printSubSection("8. BIOME AT VARIOUS HEIGHTS");

        for (int testY : {0, 48, 63, 80}) {
            int32_t qY = core::QuartPos::fromBlock(testY);
            BiomeKey b = biomeSource.getNoiseBiome(quartX, qY, quartZ, *sampler);
            bool frozen = (b == BiomeKeys::FROZEN_OCEAN || b == BiomeKeys::DEEP_FROZEN_OCEAN);
            std::cout << "Y=" << testY << " (quartY=" << qY << "): "
                      << b
                      << " (frozen: " << (frozen ? "true" : "false") << ")" << std::endl;
        }
    }

    // ============================================
    // SECTION 9: FULL COLUMN TRACE AT POSITION 1
    // ============================================
    printSeparator("FULL COLUMN TRACE AT (720, 430)");

    int32_t traceX = 720;
    int32_t traceZ = 430;

    // Get biome for this column
    int32_t quartX = core::QuartPos::fromBlock(traceX);
    int32_t quartZ = core::QuartPos::fromBlock(traceZ);

    // Calculate iceberg value for this column
    double icebergSurfaceVal = icebergSurfaceNoise->getValue(
        static_cast<double>(traceX), 0.0, static_cast<double>(traceZ));
    double icebergPillarVal = icebergPillarNoise->getValue(
        static_cast<double>(traceX) * 1.28, 0.0, static_cast<double>(traceZ) * 1.28);
    double iceberg = std::min(std::abs(icebergSurfaceVal * 8.25), icebergPillarVal * 15.0);

    std::cout << "Iceberg value: " << iceberg << std::endl;

    if (iceberg > 1.8) {
        double icebergRoofVal = icebergPillarRoofNoise->getValue(
            static_cast<double>(traceX) * 1.17, 0.0, static_cast<double>(traceZ) * 1.17);
        double icebergRoof = std::abs(icebergRoofVal * 1.5);
        double top = std::min(iceberg * iceberg * 1.2, std::ceil(icebergRoof * 40.0) + 14.0);

        // Get biome and check temperature
        BiomeKey biomeKey = biomeSource.getNoiseBiome(quartX, core::QuartPos::fromBlock(48), quartZ, *sampler);
        const Biome* biome = Biomes::get(biomeKey);
        if (biome) {
            core::BlockPos tempPos(traceX, SEA_LEVEL, traceZ);
            float temperature = biome->getTemperature(tempPos, SEA_LEVEL);
            if (temperature > 0.1f) {
                top -= 2.0;
            }
        }

        double extensionBottom = (top > 2.0) ? (SEA_LEVEL - top - 7.0) : 0.0;
        double extensionTop = (top > 2.0) ? (top + SEA_LEVEL) : 0.0;

        std::cout << "extensionTop: " << extensionTop << std::endl;
        std::cout << "extensionBottom: " << extensionBottom << std::endl;

        // Get random for this column
        XoroshiroRandomSource random = noiseRandom.at(traceX, 0, traceZ);
        int32_t maxSnowDepth = 2 + random.nextInt(4);
        int32_t minSnowHeight = SEA_LEVEL + 18 + random.nextInt(10);

        std::cout << "maxSnowDepth: " << maxSnowDepth << std::endl;
        std::cout << "minSnowHeight: " << minSnowHeight << std::endl;

        std::cout << "\nY-level placement decisions (Y=45 to Y=55):" << std::endl;

        // Simulate placement loop
        for (int32_t y = 55; y >= 45; --y) {
            // In actual code, we'd check the block state here
            // For diagnostic, simulate both conditions

            // Air condition: y < extensionTop && random.nextDouble() > 0.01
            double airRandom = random.nextDouble();
            bool airCondition = y < static_cast<int32_t>(extensionTop) && airRandom > 0.01;

            // Water condition: y > extensionBottom && y < seaLevel && extensionBottom != 0 && random.nextDouble() > 0.15
            double waterRandom = random.nextDouble();
            bool waterCondition = y > static_cast<int32_t>(extensionBottom) &&
                                  y < SEA_LEVEL &&
                                  extensionBottom != 0.0 &&
                                  waterRandom > 0.15;

            std::cout << "Y=" << y << ": airRandom=" << airRandom
                      << " (>0.01: " << (airRandom > 0.01 ? "T" : "F") << ")"
                      << ", waterRandom=" << waterRandom
                      << " (>0.15: " << (waterRandom > 0.15 ? "T" : "F") << ")"
                      << ", y<extTop: " << (y < static_cast<int32_t>(extensionTop) ? "T" : "F")
                      << ", y>extBot: " << (y > static_cast<int32_t>(extensionBottom) ? "T" : "F")
                      << ", y<sea: " << (y < SEA_LEVEL ? "T" : "F")
                      << std::endl;
        }
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "DIAGNOSTIC COMPLETE" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
