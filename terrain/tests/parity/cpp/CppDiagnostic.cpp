/**
 * C++ Diagnostic Test for Parity Investigation
 *
 * Tests specific block positions where C++ and Java outputs differ,
 * outputting intermediate density and aquifer values.
 */

#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Aquifer.h"
#include "levelgen/Beardifier.h"
#include "levelgen/Blender.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/LevelChunkSection.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Climate.h"
#include "core/BlockPos.h"
#include "core/QuartPos.h"

using namespace minecraft::levelgen;
using namespace minecraft::density;

// Test parameters
static const int64_t SEED = 12345L;
static const int CHUNK_X = 4;
static const int CHUNK_Z = -2;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

// Test positions where C++ and Java differ
struct TestPosition {
    int x, y, z;
    const char* cppBlock;
    const char* javaBlock;
};

static const std::vector<TestPosition> TEST_POSITIONS = {
    {10, -49, 9, "deepslate", "tuff"},
    {10, 47, 7, "stone", "granite"},
    {9, -49, 9, "deepslate", "deepslate"},
    {9, 47, 7, "stone", "stone"},
};

int main(int argc, char* argv[]) {
    std::cout << "=== C++ Diagnostic Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << std::endl;

    try {
        minecraft::world::level::block::Blocks::bootstrap();

        // Setup (same as CppChunkGenerator.cpp)
        ::BlockState* airBlock = ::minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        ::BlockState* stoneBlock = ::minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
        registry->registerBlock(airBlock);
        registry->registerBlock(stoneBlock);
        registry->registerBlock(::minecraft::world::level::block::Blocks::WATER->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());

        // Bootstrap registries
        std::cout << "Bootstrapping registries..." << std::endl;
        NoiseRegistry::bootstrap();
        DensityFunctionRegistry::bootstrap(SEED);
        SurfaceRuleData::initialize();

        // Build noise router
        std::cout << "Building NoiseRouter..." << std::endl;
        NoiseRouter* router = NoiseRouterData::overworld(false, false);

        // Create settings
        NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
        
        

        NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
            noiseSettings,
            ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(),
            ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            *router,
            nullptr,
            {},
            63,
            false,
            true,  // aquifersEnabled
            true,  // oreVeinsEnabled
            false
        );

        // Create RandomState
        std::cout << "Creating RandomState..." << std::endl;
        RandomState* randomState = RandomState::create(settings, SEED);

        // Create fluid picker
        FluidPicker* fluidPicker = new OverworldFluidPicker(
            63, -54,
            ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            ::minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        // Create proto chunk
        minecraft::world::ChunkPos chunkPos(CHUNK_X, CHUNK_Z);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
        );

        // Create NoiseChunk (the key component)
        std::cout << "Creating NoiseChunk..." << std::endl;
        Beardifier* beardifier = Beardifier::EMPTY();
        minecraft::Blender* blender = minecraft::Blender::empty();

        NoiseChunk* noiseChunk = NoiseChunk::forChunk(
            chunk,
            *randomState,
            beardifier,
            *settings,
            fluidPicker,
            blender
        );

        // Get the noise router from RandomState (which has wrapped functions)
        NoiseRouter* wrappedRouter = randomState->router();

        std::cout << std::endl;
        std::cout << "=== Testing Density Values at Mismatched Positions ===" << std::endl;
        std::cout << std::fixed << std::setprecision(10);
        std::cout << std::endl;

        // Header
        std::cout << std::left << std::setw(20) << "Position"
                  << std::setw(15) << "C++ Block"
                  << std::setw(15) << "Java Block"
                  << std::setw(20) << "finalDensity"
                  << std::setw(20) << "prelimSurface"
                  << std::endl;
        std::cout << std::string(90, '-') << std::endl;

        for (const auto& pos : TEST_POSITIONS) {
            // Convert local chunk coords to world coords
            int worldX = chunkPos.getMinBlockX() + pos.x;
            int worldZ = chunkPos.getMinBlockZ() + pos.z;
            int worldY = pos.y;

            // Create context for this position using SinglePointContext
            DensityFunction::SinglePointContext ctx(worldX, worldY, worldZ);

            // Sample finalDensity
            double finalDensity = wrappedRouter->finalDensity()->compute(ctx);

            // Get preliminary surface level
            int prelimSurface = noiseChunk->preliminarySurfaceLevel(worldX, worldZ);

            // Output results
            std::cout << std::left
                      << "(" << std::setw(2) << pos.x << "," << std::setw(4) << pos.y << "," << std::setw(2) << pos.z << ")  "
                      << std::setw(15) << pos.cppBlock
                      << std::setw(15) << pos.javaBlock
                      << std::setw(20) << finalDensity
                      << std::setw(20) << prelimSurface
                      << std::endl;
        }

        std::cout << std::endl;
        std::cout << "=== Detailed Density Function Breakdown ===" << std::endl;
        std::cout << std::endl;

        // For key mismatched positions, show all density function values
        std::vector<TestPosition> detailedPositions = {
            {10, -49, 9, "deepslate", "tuff"},
            {10, 47, 7, "stone", "granite"},
        };

        for (const auto& pos : detailedPositions) {
            int worldX = chunkPos.getMinBlockX() + pos.x;
            int worldZ = chunkPos.getMinBlockZ() + pos.z;
            int worldY = pos.y;

            DensityFunction::SinglePointContext ctx(worldX, worldY, worldZ);

            std::cout << "Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
            std::cout << "  World coords: (" << worldX << ", " << worldY << ", " << worldZ << ")" << std::endl;
            std::cout << "  Expected: C++=" << pos.cppBlock << ", Java=" << pos.javaBlock << std::endl;
            std::cout << std::endl;

            // Sample all noise router functions
            std::cout << "  NoiseRouter values:" << std::endl;
            std::cout << "    barrierNoise:              " << wrappedRouter->barrierNoise()->compute(ctx) << std::endl;
            std::cout << "    fluidLevelFloodednessNoise:" << wrappedRouter->fluidLevelFloodednessNoise()->compute(ctx) << std::endl;
            std::cout << "    fluidLevelSpreadNoise:     " << wrappedRouter->fluidLevelSpreadNoise()->compute(ctx) << std::endl;
            std::cout << "    lavaNoise:                 " << wrappedRouter->lavaNoise()->compute(ctx) << std::endl;
            std::cout << "    temperature:               " << wrappedRouter->temperature()->compute(ctx) << std::endl;
            std::cout << "    vegetation:                " << wrappedRouter->vegetation()->compute(ctx) << std::endl;
            std::cout << "    continents:                " << wrappedRouter->continents()->compute(ctx) << std::endl;
            std::cout << "    erosion:                   " << wrappedRouter->erosion()->compute(ctx) << std::endl;
            std::cout << "    depth:                     " << wrappedRouter->depth()->compute(ctx) << std::endl;
            std::cout << "    ridges:                    " << wrappedRouter->ridges()->compute(ctx) << std::endl;
            std::cout << "    prelimSurfaceLevel:        " << wrappedRouter->preliminarySurfaceLevel()->compute(ctx) << std::endl;
            std::cout << "    finalDensity:              " << wrappedRouter->finalDensity()->compute(ctx) << std::endl;
            std::cout << "    veinToggle:                " << wrappedRouter->veinToggle()->compute(ctx) << std::endl;
            std::cout << "    veinRidged:                " << wrappedRouter->veinRidged()->compute(ctx) << std::endl;
            std::cout << "    veinGap:                   " << wrappedRouter->veinGap()->compute(ctx) << std::endl;
            auto oreRandom = randomState->oreRandom()->at(worldX, worldY, worldZ);
            std::cout << "    oreRandom.nextFloat[0]:    " << oreRandom.nextFloat() << std::endl;
            std::cout << "    oreRandom.nextFloat[1]:    " << oreRandom.nextFloat() << std::endl;
            std::cout << "    oreRandom.nextFloat[2]:    " << oreRandom.nextFloat() << std::endl;
            std::cout << std::endl;

            // Preliminary surface level
            std::cout << "  preliminarySurfaceLevel: " << noiseChunk->preliminarySurfaceLevel(worldX, worldZ) << std::endl;
            std::cout << std::endl;
        }

        std::cout << "=== Diagnostic Complete ===" << std::endl;

        // Cleanup
        delete noiseChunk;
        delete chunk;
        delete randomState;
        delete settings;
        delete registry;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
