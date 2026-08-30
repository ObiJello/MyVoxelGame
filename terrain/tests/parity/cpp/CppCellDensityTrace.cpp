/**
 * C++ Cell Density Trace
 * Traces cell corner densities and interpolated values for comparison with Java
 * Focus on Y=90-100 at position x=-799, z=-152
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <memory>

#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/LevelChunkSection.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/Aquifer.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "world/biome/MultiNoiseBiomeSource.h"

using namespace minecraft::levelgen;
using namespace minecraft::density;
using namespace minecraft::world::biome;

constexpr int64_t SEED = 12345L;
constexpr int CHUNK_X = -50;
constexpr int CHUNK_Z = -10;
constexpr int MIN_Y = -64;
constexpr int HEIGHT = 384;
constexpr int CELL_WIDTH = 4;
constexpr int CELL_HEIGHT = 8;

double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

double trilinearInterpolate(double* corners, double fx, double fy, double fz) {
    // corners order: 000, 100, 010, 110, 001, 101, 011, 111
    double c00 = lerp(fx, corners[0], corners[1]);
    double c10 = lerp(fx, corners[2], corners[3]);
    double c01 = lerp(fx, corners[4], corners[5]);
    double c11 = lerp(fx, corners[6], corners[7]);

    double c0 = lerp(fy, c00, c10);
    double c1 = lerp(fy, c01, c11);

    return lerp(fz, c0, c1);
}

void getCellCorners(const NoiseRouter* router, double* corners,
                    int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
    int coords[8][3] = {
        {minX, minY, minZ}, {maxX, minY, minZ}, {minX, maxY, minZ}, {maxX, maxY, minZ},
        {minX, minY, maxZ}, {maxX, minY, maxZ}, {minX, maxY, maxZ}, {maxX, maxY, maxZ}
    };
    for (int i = 0; i < 8; i++) {
        DensityFunction::SinglePointContext ctx(coords[i][0], coords[i][1], coords[i][2]);
        corners[i] = router->finalDensity()->compute(ctx);
    }
}

void printCellCorners(const NoiseRouter* router, int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
    int coords[8][3] = {
        {minX, minY, minZ}, {maxX, minY, minZ}, {minX, maxY, minZ}, {maxX, maxY, minZ},
        {minX, minY, maxZ}, {maxX, minY, maxZ}, {minX, maxY, maxZ}, {maxX, maxY, maxZ}
    };
    const char* names[8] = {
        "(minX,minY,minZ)", "(maxX,minY,minZ)", "(minX,maxY,minZ)", "(maxX,maxY,minZ)",
        "(minX,minY,maxZ)", "(maxX,minY,maxZ)", "(minX,maxY,maxZ)", "(maxX,maxY,maxZ)"
    };
    for (int i = 0; i < 8; i++) {
        DensityFunction::SinglePointContext ctx(coords[i][0], coords[i][1], coords[i][2]);
        double density = router->finalDensity()->compute(ctx);
        std::cout << "  " << names[i] << " (" << coords[i][0] << "," << coords[i][1] << "," << coords[i][2]
                  << "): " << std::showpos << std::fixed << std::setprecision(15) << density << std::noshowpos << std::endl;
    }
}

int main() {
    std::cout << "=== C++ Cell Density Trace ===" << std::endl;
    std::cout << "Seed: " << SEED << ", Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << std::endl;

    // Bootstrap registries
    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);
    SurfaceRuleData::initialize();

    // Build overworld noise router
    NoiseRouter* router = NoiseRouterData::overworld(false, false);

    // Create noise settings
    NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    // Create RandomState
    RandomState* randomState = RandomState::create(settings, SEED);
    const NoiseRouter* wrappedRouter = randomState->router();

    world::ChunkPos chunkPos(CHUNK_X, CHUNK_Z);
    int worldX = chunkPos.getMinBlockX() + 1;  // local x=1
    int worldZ = chunkPos.getMinBlockZ() + 8;  // local z=8

    // Find the cell for local x=1, z=8
    int cellX = 1 / CELL_WIDTH;  // = 0
    int cellZ = 8 / CELL_WIDTH;  // = 2

    int cellMinX = chunkPos.getMinBlockX() + cellX * CELL_WIDTH;
    int cellMaxX = cellMinX + CELL_WIDTH;
    int cellMinZ = chunkPos.getMinBlockZ() + cellZ * CELL_WIDTH;
    int cellMaxZ = cellMinZ + CELL_WIDTH;

    std::cout << "Target position: world (" << worldX << ", Y, " << worldZ << ")" << std::endl;
    std::cout << "Cell X: " << cellMinX << " to " << cellMaxX << std::endl;
    std::cout << "Cell Z: " << cellMinZ << " to " << cellMaxZ << std::endl;
    std::cout << std::endl;

    std::cout << "=== Cell containing Y=88-95 (cellMinY=88, cellMaxY=96) ===" << std::endl;
    printCellCorners(wrappedRouter, cellMinX, cellMaxX, 88, 96, cellMinZ, cellMaxZ);

    std::cout << std::endl;
    std::cout << "=== Cell containing Y=96-103 (cellMinY=96, cellMaxY=104) ===" << std::endl;
    printCellCorners(wrappedRouter, cellMinX, cellMaxX, 96, 104, cellMinZ, cellMaxZ);

    std::cout << std::endl;
    std::cout << "=== Interpolated density at target position for Y=90-100 ===" << std::endl;
    std::cout << std::endl;

    double factorX = (double)(worldX - cellMinX) / CELL_WIDTH;  // = 1/4 = 0.25
    double factorZ = (double)(worldZ - cellMinZ) / CELL_WIDTH;  // = 0/4 = 0.0

    std::cout << "factorX = " << factorX << ", factorZ = " << factorZ << std::endl;
    std::cout << std::endl;

    // Get corner densities for both cells
    double corners88[8], corners96[8];
    getCellCorners(wrappedRouter, corners88, cellMinX, cellMaxX, 88, 96, cellMinZ, cellMaxZ);
    getCellCorners(wrappedRouter, corners96, cellMinX, cellMaxX, 96, 104, cellMinZ, cellMaxZ);

    std::cout << std::left << std::setw(5) << "Y" << " "
              << std::setw(12) << "Cell" << " "
              << std::setw(24) << "Point Density" << " "
              << std::setw(24) << "Interpolated" << " "
              << std::setw(10) << "Result" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    for (int y = 100; y >= 90; y--) {
        int cellMinY, cellMaxY;
        double* corners;

        if (y < 96) {
            cellMinY = 88;
            cellMaxY = 96;
            corners = corners88;
        } else {
            cellMinY = 96;
            cellMaxY = 104;
            corners = corners96;
        }

        double factorY = (double)(y - cellMinY) / CELL_HEIGHT;
        double interpolated = trilinearInterpolate(corners, factorX, factorY, factorZ);

        // Point sampled
        DensityFunction::SinglePointContext ctx(worldX, y, worldZ);
        double pointDensity = wrappedRouter->finalDensity()->compute(ctx);

        std::string cellStr = std::to_string(cellMinY) + "-" + std::to_string(cellMaxY);
        std::string result = interpolated > 0 ? "AIR" : "STONE";

        std::cout << std::left << std::setw(5) << y << " "
                  << std::setw(12) << cellStr << " "
                  << std::showpos << std::fixed << std::setprecision(15)
                  << std::setw(24) << pointDensity << " "
                  << std::setw(24) << interpolated << " "
                  << std::noshowpos << std::setw(10) << result << std::endl;
    }

    std::cout << std::endl;
    std::cout << "=== Comparing with actual blocks after NOISE phase ===" << std::endl;
    std::cout << std::endl;

    // Get block types
    minecraft::BlockState* airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
    minecraft::BlockState* stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();

    // Create block registry
    minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
    registry->registerBlock(airBlock);
    registry->registerBlock(stoneBlock);
    registry->registerBlock(minecraft::world::level::block::Blocks::WATER->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::LAVA->defaultBlockState());

    // Create fluid picker
    FluidPicker* fluidPicker = new OverworldFluidPicker(63, -54, minecraft::world::level::block::Blocks::WATER->defaultBlockState(), minecraft::world::level::block::Blocks::LAVA->defaultBlockState());

    // Create biome source
    auto biomeSource = MultiNoiseBiomeSource::createOverworld();

    // Create surface rules
    RuleSource* surfaceRules = SurfaceRuleData::overworld();

    // Create chunk generator
    NoiseBasedChunkGenerator* generator = new NoiseBasedChunkGenerator(
        settings, randomState->surfaceSystem(), surfaceRules,
        stoneBlock, airBlock,
        fluidPicker, nullptr
    );
    generator->setBiomeSource(biomeSource.get());

    // Create chunk generation runner (NOISE phase only)
    ChunkGenerationRunner::Config config;
    config.runNoise = true;
    config.runSurface = false;
    config.runCarvers = false;

    ChunkGenerationRunner runner(generator, randomState, SEED, config);

    // Create and generate chunk
    minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
        chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
    );
    runner.generateChunk(chunk);

    std::cout << std::left << std::setw(5) << "Y" << " " << std::setw(15) << "Actual Block" << std::endl;
    std::cout << std::string(25, '-') << std::endl;

    for (int y = 100; y >= 90; y--) {
        auto* block = chunk->getBlockState(1, y, 8);
        std::string blockName = block ? block->getBlockName() : "null";
        // Simplify name
        if (blockName.find("minecraft:") == 0) {
            blockName = blockName.substr(10);
        }
        std::cout << std::left << std::setw(5) << y << " " << std::setw(15) << blockName << std::endl;
    }

    std::cout << std::endl;
    std::cout << "=== Trace Complete ===" << std::endl;

    // Cleanup
    delete chunk;
    delete generator;
    delete fluidPicker;
    delete registry;
    delete randomState;
    delete settings;
    delete router;

    return 0;
}
