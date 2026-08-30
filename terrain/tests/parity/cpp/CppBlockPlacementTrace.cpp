/**
 * C++ Block Placement Trace Test
 *
 * Traces ALL values that affect final block placement at specific positions:
 * 1. Point-sampled density (direct from router)
 * 2. Interpolated density (from CacheAllInCell during filling)
 * 3. Aquifer computeSubstance inputs and decision
 * 4. Final block result
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
#include "math/Mth.h"

using namespace minecraft::levelgen;
using namespace minecraft::density;
using minecraft::Mth;

// Test parameters
static const int64_t SEED = 12345L;
static const int CHUNK_X = 0;
static const int CHUNK_Z = 0;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

// Positions to trace
struct TracePos {
    int x, y, z;
    const char* cppBlock;
    const char* javaBlock;
};

static const std::vector<TracePos> TRACE_POSITIONS = {
    {1, -55, 11, "lava", "deepslate"},
    {0, -15, 7, "air", "deepslate"},
    {0, -15, 8, "air", "deepslate"},
    {0, -64, 0, "bedrock", "bedrock"},  // Control - should match
    {8, 64, 8, "air", "air"},           // Control - should match
};

int main(int argc, char* argv[]) {
    std::cout << "=== C++ Block Placement Trace ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << std::endl;

    try {
        // Setup
        ::BlockState* airBlock = ::minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        ::BlockState* stoneBlock = ::minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
        registry->registerBlock(airBlock);
        registry->registerBlock(stoneBlock);
        registry->registerBlock(::minecraft::world::level::block::Blocks::WATER->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());

        // Bootstrap
        NoiseRegistry::bootstrap();
        DensityFunctionRegistry::bootstrap(SEED);
        SurfaceRuleData::initialize();

        // Build noise router
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

        // Create NoiseChunk
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

        // Get wrapped router
        NoiseRouter* wrappedRouter = randomState->router();

        std::cout << std::fixed << std::setprecision(10);

        // Cell dimensions
        int cellWidth = noiseSettings.getCellWidth();   // 4
        int cellHeight = noiseSettings.getCellHeight(); // 8
        int cellCountXZ = 16 / cellWidth;               // 4
        int cellCountY = noiseSettings.height() / cellHeight;
        int cellNoiseMinY = noiseSettings.minY() / cellHeight;

        std::cout << "Cell dimensions: width=" << cellWidth << ", height=" << cellHeight << std::endl;
        std::cout << "Cell counts: XZ=" << cellCountXZ << ", Y=" << cellCountY << std::endl;
        std::cout << std::endl;

        // Initialize interpolation
        noiseChunk->initializeForFirstCellX();

        for (const auto& pos : TRACE_POSITIONS) {
            int worldX = chunkPos.getMinBlockX() + pos.x;
            int worldZ = chunkPos.getMinBlockZ() + pos.z;
            int worldY = pos.y;

            std::cout << "========================================" << std::endl;
            std::cout << "Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
            std::cout << "World: (" << worldX << ", " << worldY << ", " << worldZ << ")" << std::endl;
            std::cout << "Expected: C++=" << pos.cppBlock << ", Java=" << pos.javaBlock << std::endl;
            std::cout << std::endl;

            // Calculate cell indices
            int cellXIndex = pos.x / cellWidth;
            int cellYIndex = (worldY - noiseSettings.minY()) / cellHeight;
            int cellZIndex = pos.z / cellWidth;

            // Position within cell
            int inCellX = pos.x % cellWidth;
            int inCellY = (worldY - noiseSettings.minY()) % cellHeight;
            int inCellZ = pos.z % cellWidth;

            std::cout << "Cell indices: (" << cellXIndex << ", " << cellYIndex << ", " << cellZIndex << ")" << std::endl;
            std::cout << "In-cell pos: (" << inCellX << ", " << inCellY << ", " << inCellZ << ")" << std::endl;

            // Interpolation factors
            double factorX = (double)inCellX / (double)cellWidth;
            double factorY = (double)inCellY / (double)cellHeight;
            double factorZ = (double)inCellZ / (double)cellWidth;
            std::cout << "Factors: (" << factorX << ", " << factorY << ", " << factorZ << ")" << std::endl;
            std::cout << std::endl;

            // 1. Point-sampled density (direct from router)
            DensityFunction::SinglePointContext pointCtx(worldX, worldY, worldZ);
            double pointSampledDensity = wrappedRouter->finalDensity()->compute(pointCtx);
            std::cout << "1. Point-sampled finalDensity: " << pointSampledDensity << std::endl;

            // 2. Sample cell corner densities for manual interpolation check
            int cellCornerX = chunkPos.getMinBlockX() + cellXIndex * cellWidth;
            int cellCornerY = noiseSettings.minY() + cellYIndex * cellHeight;
            int cellCornerZ = chunkPos.getMinBlockZ() + cellZIndex * cellWidth;

            std::cout << std::endl;
            std::cout << "2. Cell corner densities (for interpolation):" << std::endl;

            double corners[8];
            int offsets[8][3] = {
                {0, 0, 0}, {cellWidth, 0, 0}, {0, cellHeight, 0}, {cellWidth, cellHeight, 0},
                {0, 0, cellWidth}, {cellWidth, 0, cellWidth}, {0, cellHeight, cellWidth}, {cellWidth, cellHeight, cellWidth}
            };
            const char* cornerNames[8] = {"000", "100", "010", "110", "001", "101", "011", "111"};

            for (int i = 0; i < 8; i++) {
                int cx = cellCornerX + offsets[i][0];
                int cy = cellCornerY + offsets[i][1];
                int cz = cellCornerZ + offsets[i][2];
                DensityFunction::SinglePointContext ctx(cx, cy, cz);
                corners[i] = wrappedRouter->finalDensity()->compute(ctx);
                std::cout << "   Corner " << cornerNames[i] << " (" << cx << "," << cy << "," << cz << "): "
                          << corners[i] << (corners[i] > 0 ? " (AIR)" : " (SOLID)") << std::endl;
            }

            // 3. Manual trilinear interpolation
            double lerp_y_000_010 = Mth::lerp(factorY, corners[0], corners[2]);
            double lerp_y_100_110 = Mth::lerp(factorY, corners[1], corners[3]);
            double lerp_y_001_011 = Mth::lerp(factorY, corners[4], corners[6]);
            double lerp_y_101_111 = Mth::lerp(factorY, corners[5], corners[7]);

            double lerp_x_00 = Mth::lerp(factorX, lerp_y_000_010, lerp_y_100_110);
            double lerp_x_01 = Mth::lerp(factorX, lerp_y_001_011, lerp_y_101_111);

            double interpolatedDensity = Mth::lerp(factorZ, lerp_x_00, lerp_x_01);

            std::cout << std::endl;
            std::cout << "3. Manual trilinear interpolation:" << std::endl;
            std::cout << "   lerp(Y): " << lerp_y_000_010 << ", " << lerp_y_100_110 << ", "
                      << lerp_y_001_011 << ", " << lerp_y_101_111 << std::endl;
            std::cout << "   lerp(X): " << lerp_x_00 << ", " << lerp_x_01 << std::endl;
            std::cout << "   Interpolated density: " << interpolatedDensity
                      << (interpolatedDensity > 0 ? " (AIR)" : " (SOLID)") << std::endl;

            // 4. Aquifer-related values
            std::cout << std::endl;
            std::cout << "4. Aquifer-related noise values:" << std::endl;
            std::cout << "   barrierNoise: " << wrappedRouter->barrierNoise()->compute(pointCtx) << std::endl;
            std::cout << "   fluidLevelFloodedness: " << wrappedRouter->fluidLevelFloodednessNoise()->compute(pointCtx) << std::endl;
            std::cout << "   fluidLevelSpread: " << wrappedRouter->fluidLevelSpreadNoise()->compute(pointCtx) << std::endl;
            std::cout << "   lavaNoise: " << wrappedRouter->lavaNoise()->compute(pointCtx) << std::endl;

            // 5. Fluid picker result
            std::cout << std::endl;
            std::cout << "5. FluidPicker result at Y=" << worldY << ":" << std::endl;
            FluidStatus fluidStatus = fluidPicker->computeFluid(worldX, worldY, worldZ);
            std::cout << "   Fluid level: " << fluidStatus.fluidLevel << std::endl;
            std::cout << "   Fluid type: " << (fluidStatus.fluidType ? fluidStatus.fluidType->getIdentifier() : "null") << std::endl;

            // 6. What aquifer.computeSubstance would return
            std::cout << std::endl;
            std::cout << "6. Aquifer decision:" << std::endl;
            std::cout << "   Input density (point): " << pointSampledDensity << std::endl;
            std::cout << "   Input density (interpolated): " << interpolatedDensity << std::endl;

            // Test with both densities
            ::BlockState* resultPoint = noiseChunk->aquifer()->computeSubstance(pointCtx, pointSampledDensity);
            ::BlockState* resultInterp = noiseChunk->aquifer()->computeSubstance(pointCtx, interpolatedDensity);

            std::cout << "   Result with point density: " << (resultPoint ? resultPoint->getIdentifier() : "null (solid)") << std::endl;
            std::cout << "   Result with interpolated density: " << (resultInterp ? resultInterp->getIdentifier() : "null (solid)") << std::endl;

            // 7. Key decision factors
            std::cout << std::endl;
            std::cout << "7. Key factors:" << std::endl;
            std::cout << "   preliminarySurfaceLevel: " << noiseChunk->preliminarySurfaceLevel(worldX, worldZ) << std::endl;
            std::cout << "   Y below surface? " << (worldY < noiseChunk->preliminarySurfaceLevel(worldX, worldZ) ? "YES" : "NO") << std::endl;
            std::cout << "   density > 0? " << (pointSampledDensity > 0 ? "YES (air)" : "NO (solid)") << std::endl;
            std::cout << std::endl;
        }

        std::cout << "=== Trace Complete ===" << std::endl;

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
