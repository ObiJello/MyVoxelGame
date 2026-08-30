/**
 * Comprehensive diagnostic test for biome and carver parity
 * Traces values at specific positions to compare with Java
 */

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <vector>
#include <memory>

// Core includes
#include "core/BlockPos.h"
#include "core/SectionPos.h"

// Random includes
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"

// Biome includes
#include "world/biome/Climate.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biomes.h"

// Levelgen includes
#include "levelgen/RandomState.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FluidPicker.h"

// Carver includes
#include "levelgen/carver/CaveWorldCarver.h"
#include "levelgen/carver/CanyonWorldCarver.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/CarvingMask.h"

// World includes
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"

using namespace minecraft;
using namespace minecraft::core;
using namespace minecraft::world;
using namespace minecraft::world::biome;
using namespace minecraft::levelgen;
using namespace minecraft::levelgen::carver;

// Test parameters
static const int64_t SEED = 12345L;
static const int CHUNK_X = 0;
static const int CHUNK_Z = 0;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

// Test positions - same as known divergence points
struct TestPosition {
    int x, y, z;
    const char* description;
};

std::vector<TestPosition> testPositions = {
    {0, 64, 0, "Surface level center"},
    {0, -55, 0, "Deep cave area (lava level nearby)"},
    {1, -55, 11, "Known lava divergence position"},
    {0, -15, 7, "Known air divergence position 1"},
    {0, -15, 8, "Known air divergence position 2"},
    {8, 32, 8, "Mid-level center"},
    {0, 0, 0, "Y=0 (sea level reference)"},
    {15, 100, 15, "High altitude corner"},
};

void printSeparator(const char* title) {
    std::cout << "\n========================================" << std::endl;
    std::cout << title << std::endl;
    std::cout << "========================================" << std::endl;
}

void testLegacyRandomSource() {
    printSeparator("1. LegacyRandomSource Tests");

    // Test with specific seeds used in carving
    std::vector<int64_t> seeds = {12345L, 0L, 1L, -1L, 3456L, 2345L};

    for (int64_t seed : seeds) {
        LegacyRandomSource rng(seed);

        std::cout << "\nSeed: " << seed << std::endl;
        std::cout << "  nextLong()[0]: " << rng.nextLong() << std::endl;

        // Reset and get sequence
        LegacyRandomSource rng2(seed);
        std::cout << "  nextInt(16)[0]: " << rng2.nextInt(16) << std::endl;
        std::cout << "  nextInt(16)[1]: " << rng2.nextInt(16) << std::endl;
        std::cout << "  nextFloat()[0]: " << std::fixed << std::setprecision(10) << rng2.nextFloat() << std::endl;
        std::cout << "  nextFloat()[1]: " << rng2.nextFloat() << std::endl;
        std::cout << "  nextDouble()[0]: " << rng2.nextDouble() << std::endl;
    }

    // Test setLargeFeatureSeed
    std::cout << "\nsetLargeFeatureSeed tests:" << std::endl;
    LegacyRandomSource rng3(0);
    rng3.setLargeFeatureSeed(12345L, 0, 0);
    std::cout << "  After setLargeFeatureSeed(12345, 0, 0):" << std::endl;
    std::cout << "    nextFloat(): " << std::fixed << std::setprecision(10) << rng3.nextFloat() << std::endl;

    LegacyRandomSource rng4(0);
    rng4.setLargeFeatureSeed(12345L, -8, -6);
    std::cout << "  After setLargeFeatureSeed(12345, -8, -6):" << std::endl;
    std::cout << "    nextFloat(): " << std::fixed << std::setprecision(10) << rng4.nextFloat() << std::endl;
}

void testXoroshiroRandomSource() {
    printSeparator("2. XoroshiroRandomSource Tests");

    std::vector<int64_t> seeds = {12345L, 0L, 1L, -1L};

    for (int64_t seed : seeds) {
        XoroshiroRandomSource rng(seed);

        std::cout << "\nSeed: " << seed << std::endl;
        std::cout << "  nextLong()[0]: " << rng.nextLong() << std::endl;

        XoroshiroRandomSource rng2(seed);
        std::cout << "  nextInt(16)[0]: " << rng2.nextInt(16) << std::endl;
        std::cout << "  nextInt(16)[1]: " << rng2.nextInt(16) << std::endl;
        std::cout << "  nextFloat()[0]: " << std::fixed << std::setprecision(10) << rng2.nextFloat() << std::endl;
        std::cout << "  nextFloat()[1]: " << rng2.nextFloat() << std::endl;
        std::cout << "  nextDouble()[0]: " << rng2.nextDouble() << std::endl;
    }
}

void testClimateParameters() {
    printSeparator("3. Climate Parameter Tests");

    // Test Climate::Parameter creation and quantization
    Climate::Parameter p1 = Climate::Parameter::point(0.5f);
    Climate::Parameter p2 = Climate::Parameter::span(-0.5f, 0.5f);

    std::cout << "Parameter::point(0.5f):" << std::endl;
    std::cout << "  min: " << p1.min() << ", max: " << p1.max() << std::endl;

    std::cout << "Parameter::span(-0.5f, 0.5f):" << std::endl;
    std::cout << "  min: " << p2.min() << ", max: " << p2.max() << std::endl;

    // Test quantization
    std::cout << "\nQuantization tests:" << std::endl;
    std::vector<float> testValues = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.123456f};
    for (float val : testValues) {
        int64_t quantized = Climate::quantizeCoord(val);
        float unquantized = Climate::unquantizeCoord(quantized);
        std::cout << "  " << std::fixed << std::setprecision(6) << val
                  << " -> " << quantized << " -> " << unquantized << std::endl;
    }
}

void testOverworldBiomeBuilder() {
    printSeparator("4. OverworldBiomeBuilder Tests");

    OverworldBiomeBuilder builder;

    // Collect all biome entries
    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> biomes;
    builder.addBiomes([&biomes](const std::pair<Climate::ParameterPoint, BiomeKey>& entry) {
        biomes.push_back(entry);
    });

    std::cout << "Total biome entries: " << biomes.size() << std::endl;

    // Print first 10 entries with their parameters
    std::cout << "\nFirst 10 biome entries:" << std::endl;
    for (size_t i = 0; i < std::min(biomes.size(), size_t(10)); ++i) {
        const auto& entry = biomes[i];
        std::cout << "  [" << i << "] " << entry.second << std::endl;
        std::cout << "      temp: [" << entry.first.temperature.min() << ", " << entry.first.temperature.max() << "]" << std::endl;
        std::cout << "      humid: [" << entry.first.humidity.min() << ", " << entry.first.humidity.max() << "]" << std::endl;
        std::cout << "      cont: [" << entry.first.continentalness.min() << ", " << entry.first.continentalness.max() << "]" << std::endl;
    }

    // Note: pickMiddleBiome is private in C++, so we skip that test here
    // Java uses reflection to access it
    std::cout << "\nBiome picking tests: (skipped - pickMiddleBiome is private)" << std::endl;
    std::cout << "  Java uses reflection to access private methods for testing" << std::endl;
}

void testBiomeLookup(RandomState* randomState) {
    printSeparator("5. Biome Lookup at Test Positions");

    // Create MultiNoiseBiomeSource
    auto biomeSource = MultiNoiseBiomeSource::createOverworld();

    Climate::Sampler* sampler = randomState->sampler();

    for (const auto& pos : testPositions) {
        // Convert to quart coordinates
        int quartX = pos.x >> 2;
        int quartY = pos.y >> 2;
        int quartZ = pos.z >> 2;

        std::cout << "\nPosition (" << pos.x << ", " << pos.y << ", " << pos.z << ") - " << pos.description << std::endl;
        std::cout << "  Quart coords: (" << quartX << ", " << quartY << ", " << quartZ << ")" << std::endl;

        // Sample climate
        Climate::TargetPoint target = sampler->sample(quartX, quartY, quartZ);

        std::cout << "  Climate target:" << std::endl;
        std::cout << "    temperature: " << target.temperature << std::endl;
        std::cout << "    humidity: " << target.humidity << std::endl;
        std::cout << "    continentalness: " << target.continentalness << std::endl;
        std::cout << "    erosion: " << target.erosion << std::endl;
        std::cout << "    depth: " << target.depth << std::endl;
        std::cout << "    weirdness: " << target.weirdness << std::endl;

        // Get biome
        BiomeKey biome = biomeSource->getNoiseBiome(quartX, quartY, quartZ, *sampler);
        std::cout << "  Selected biome: " << biome << std::endl;
    }
}

void testCarverSeeding() {
    printSeparator("6. Carver Seeding Tests");

    int64_t worldSeed = 12345L;
    ChunkPos chunkPos(0, 0);

    std::cout << "World seed: " << worldSeed << std::endl;
    std::cout << "Chunk: (" << chunkPos.x() << ", " << chunkPos.z() << ")" << std::endl;

    // Test carving range
    int32_t carverRange = 8; // Default for caves
    std::cout << "\nCarving chunks (range=" << carverRange << "):" << std::endl;

    for (int32_t cx = chunkPos.x() - carverRange; cx <= chunkPos.x() + carverRange; ++cx) {
        for (int32_t cz = chunkPos.z() - carverRange; cz <= chunkPos.z() + carverRange; ++cz) {
            // Calculate carver seed for this source chunk
            LegacyRandomSource seedRng(worldSeed);
            int64_t xScale = seedRng.nextLong();
            int64_t zScale = seedRng.nextLong();
            int64_t carverSeed = static_cast<int64_t>(cx) * xScale ^ static_cast<int64_t>(cz) * zScale ^ worldSeed;

            // Check isStartChunk
            LegacyRandomSource carverRng(carverSeed);
            float probability = 0.00666666666f; // Cave probability
            bool isStart = carverRng.nextFloat() <= probability;

            if (isStart) {
                std::cout << "  Source chunk (" << cx << ", " << cz << "): carverSeed=" << carverSeed
                          << ", isStart=true" << std::endl;
            }
        }
    }

    // Detailed trace for specific source chunks
    std::cout << "\nDetailed carver trace for source chunk (-8, -6):" << std::endl;
    int32_t sourceX = -8, sourceZ = -6;

    LegacyRandomSource seedRng(worldSeed);
    int64_t xScale = seedRng.nextLong();
    int64_t zScale = seedRng.nextLong();

    std::cout << "  xScale: " << xScale << std::endl;
    std::cout << "  zScale: " << zScale << std::endl;

    int64_t carverSeed = static_cast<int64_t>(sourceX) * xScale ^ static_cast<int64_t>(sourceZ) * zScale ^ worldSeed;
    std::cout << "  carverSeed: " << carverSeed << std::endl;

    LegacyRandomSource carverRng(carverSeed);
    std::cout << "  nextFloat() for isStartChunk: " << std::fixed << std::setprecision(10) << carverRng.nextFloat() << std::endl;
}

void testCaveCarverGeneration() {
    printSeparator("7. Cave Carver Generation Test");

    int64_t worldSeed = 12345L;
    ChunkPos sourceChunk(-8, -6);  // Known carving source

    std::cout << "Testing cave carver for source chunk (" << sourceChunk.x() << ", " << sourceChunk.z() << ")" << std::endl;

    // Calculate carver seed
    LegacyRandomSource seedRng(worldSeed);
    int64_t xScale = seedRng.nextLong();
    int64_t zScale = seedRng.nextLong();
    int64_t carverSeed = static_cast<int64_t>(sourceChunk.x()) * xScale ^
                         static_cast<int64_t>(sourceChunk.z()) * zScale ^ worldSeed;

    std::cout << "  carverSeed: " << carverSeed << std::endl;

    // Create random for carving
    LegacyRandomSource random(carverSeed);

    // Consume isStartChunk check
    float startCheck = random.nextFloat();
    std::cout << "  isStartChunk check float: " << std::fixed << std::setprecision(10) << startCheck << std::endl;

    // Cave carver parameters
    int32_t maxDistance = SectionPos::sectionToBlockCoord(8 * 2 - 1);  // range=8
    std::cout << "  maxDistance: " << maxDistance << std::endl;

    // Cave count
    int32_t inner1 = random.nextInt(15) + 1;  // getCaveBound() = 15
    int32_t inner2 = random.nextInt(inner1) + 1;
    int32_t caveCount = random.nextInt(inner2);

    std::cout << "  caveCount computation: nextInt(15)+1=" << inner1
              << ", nextInt(" << inner1 << ")+1=" << inner2
              << ", nextInt(" << inner2 << ")=" << caveCount << std::endl;

    // For each cave, trace starting values
    LegacyRandomSource random2(carverSeed);
    random2.nextFloat(); // Skip isStartChunk
    random2.nextInt(random2.nextInt(random2.nextInt(15) + 1) + 1); // Skip caveCount

    for (int32_t cave = 0; cave < std::min(caveCount, 3); ++cave) {
        std::cout << "\n  Cave " << cave << ":" << std::endl;

        int32_t xOffset = random2.nextInt(16);
        double x = static_cast<double>(sourceChunk.getBlockX(xOffset));
        std::cout << "    x offset: " << xOffset << ", x: " << x << std::endl;

        // y would come from configuration sample
        int32_t zOffset = random2.nextInt(16);
        double z = static_cast<double>(sourceChunk.getBlockZ(zOffset));
        std::cout << "    z offset: " << zOffset << ", z: " << z << std::endl;
    }
}

void testSurfaceSystem(RandomState* randomState) {
    printSeparator("8. Surface System Tests");

    // Get noise router values at test positions
    std::cout << "Surface noise values at test positions:" << std::endl;

    NoiseRouter* router = randomState->router();

    for (const auto& pos : testPositions) {
        std::cout << "\nPosition (" << pos.x << ", " << pos.y << ", " << pos.z << "):" << std::endl;

        density::DensityFunction::SinglePointContext ctx(pos.x, pos.y, pos.z);

        // Get density values from router
        double finalDensity = router->finalDensity() ?
            router->finalDensity()->compute(ctx) : 0.0;
        double depth = router->depth() ?
            router->depth()->compute(ctx) : 0.0;

        std::cout << "  finalDensity: " << std::fixed << std::setprecision(10) << finalDensity << std::endl;
        std::cout << "  depth: " << depth << std::endl;
    }
}

void testAquiferValues(RandomState* randomState) {
    printSeparator("9. Aquifer Test Values");

    std::cout << "Aquifer-related noise values at test positions:" << std::endl;

    NoiseRouter* router = randomState->router();

    for (const auto& pos : testPositions) {
        std::cout << "\nPosition (" << pos.x << ", " << pos.y << ", " << pos.z << "):" << std::endl;

        density::DensityFunction::SinglePointContext ctx(pos.x, pos.y, pos.z);

        // Aquifer uses these noise functions
        double barrierNoise = router->barrierNoise() ?
            router->barrierNoise()->compute(ctx) : 0.0;
        double fluidLevelFloodedness = router->fluidLevelFloodednessNoise() ?
            router->fluidLevelFloodednessNoise()->compute(ctx) : 0.0;
        double fluidLevelSpread = router->fluidLevelSpreadNoise() ?
            router->fluidLevelSpreadNoise()->compute(ctx) : 0.0;
        double lavaNoise = router->lavaNoise() ?
            router->lavaNoise()->compute(ctx) : 0.0;

        std::cout << "  barrierNoise: " << std::fixed << std::setprecision(10) << barrierNoise << std::endl;
        std::cout << "  fluidLevelFloodedness: " << fluidLevelFloodedness << std::endl;
        std::cout << "  fluidLevelSpread: " << fluidLevelSpread << std::endl;
        std::cout << "  lavaNoise: " << lavaNoise << std::endl;
    }
}

int main() {
    std::cout << "=== C++ Biome/Carver Diagnostic Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;

    try {
        // Run standalone tests first (no setup required)
        testLegacyRandomSource();
        testXoroshiroRandomSource();
        testClimateParameters();
        testOverworldBiomeBuilder();
        testCarverSeeding();
        testCaveCarverGeneration();

        // Now set up the full environment for noise-based tests
        printSeparator("Setting up RandomState environment");

        std::cout << "Bootstrapping NoiseRegistry..." << std::endl;
        NoiseRegistry::bootstrap();

        std::cout << "Bootstrapping DensityFunctionRegistry for seed: " << SEED << std::endl;
        DensityFunctionRegistry::bootstrap(SEED);

        std::cout << "Initializing SurfaceRuleData..." << std::endl;
        SurfaceRuleData::initialize();

        std::cout << "Building overworld NoiseRouter..." << std::endl;
        NoiseRouter* router = NoiseRouterData::overworld(false, false);

        std::cout << "Creating NoiseGeneratorSettings..." << std::endl;
        NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
        
        

        NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
            noiseSettings,
            ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(),
            ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            *router,
            nullptr,  // surfaceRule - use default
            {},       // spawnTarget - empty
            63,       // seaLevel
            false,    // disableMobGeneration
            true,     // aquifersEnabled
            true,     // oreVeinsEnabled
            false     // useLegacyRandomSource (XOROSHIRO for overworld)
        );

        std::cout << "Creating RandomState for seed: " << SEED << std::endl;
        RandomState* randomState = RandomState::create(settings, SEED);

        std::cout << "Environment setup complete." << std::endl;

        // Run noise-based tests
        testBiomeLookup(randomState);
        testSurfaceSystem(randomState);
        testAquiferValues(randomState);

        printSeparator("DIAGNOSTIC COMPLETE");
        std::cout << "All tests completed successfully." << std::endl;

        // Cleanup
        delete randomState;
        delete settings;
        delete router;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
