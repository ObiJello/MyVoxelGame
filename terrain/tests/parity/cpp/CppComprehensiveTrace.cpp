/**
 * C++ Comprehensive Trace Test
 *
 * Outputs ALL values in the block placement chain to compare with Java
 */

#include <iostream>
#include <iomanip>
#include <vector>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Aquifer.h"
#include "levelgen/Beardifier.h"
#include "levelgen/Blender.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "math/Mth.h"

using namespace minecraft::levelgen;
using namespace minecraft::density;
using minecraft::Mth;

static const int64_t SEED = 12345L;
static const int CHUNK_X = 0;
static const int CHUNK_Z = 0;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

struct TestPos { int x, y, z; };
static const std::vector<TestPos> TEST_POSITIONS = {
    {1, -55, 11},   // C++ says lava, Java says deepslate
    {0, -15, 7},    // C++ says air, Java says deepslate
    {0, -64, 0},    // Control - bedrock
    {8, 64, 8},     // Control - air
};

int main() {
    std::cout << "=== C++ Comprehensive Trace ===" << std::endl;
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

        NoiseRegistry::bootstrap();
        DensityFunctionRegistry::bootstrap(SEED);
        SurfaceRuleData::initialize();

        NoiseRouter* router = NoiseRouterData::overworld(false, false);
        NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
        
        

        NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
            noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {},
            63, false, true, true, false
        );

        RandomState* randomState = RandomState::create(settings, SEED);

        FluidPicker* fluidPicker = new OverworldFluidPicker(
            63, -54, ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), ::minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        minecraft::world::ChunkPos chunkPos(CHUNK_X, CHUNK_Z);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
        );

        Beardifier* beardifier = Beardifier::EMPTY();
        minecraft::Blender* blender = minecraft::Blender::empty();

        NoiseChunk* noiseChunk = NoiseChunk::forChunk(
            chunk, *randomState, beardifier, *settings, fluidPicker, blender
        );

        NoiseRouter* wrappedRouter = randomState->router();

        int cellWidth = noiseSettings.getCellWidth();
        int cellHeight = noiseSettings.getCellHeight();
        int cellNoiseMinY = noiseSettings.minY() / cellHeight;

        std::cout << "Cell dimensions: width=" << cellWidth << ", height=" << cellHeight << std::endl;
        std::cout << std::endl;

        std::cout << std::fixed << std::setprecision(10);

        for (const auto& pos : TEST_POSITIONS) {
            int localX = pos.x;
            int localY = pos.y;
            int localZ = pos.z;
            int worldX = chunkPos.getMinBlockX() + localX;
            int worldZ = chunkPos.getMinBlockZ() + localZ;
            int worldY = localY;

            std::cout << "========================================" << std::endl;
            std::cout << "Position: (" << localX << ", " << localY << ", " << localZ << ")" << std::endl;
            std::cout << "World: (" << worldX << ", " << worldY << ", " << worldZ << ")" << std::endl;
            std::cout << std::endl;

            // Cell indices
            int cellXIndex = localX / cellWidth;
            int cellYIndex = (worldY - noiseSettings.minY()) / cellHeight;
            int cellZIndex = localZ / cellWidth;

            int inCellX = localX % cellWidth;
            int inCellY = (worldY - noiseSettings.minY()) % cellHeight;
            int inCellZ = localZ % cellWidth;

            double factorX = (double)inCellX / (double)cellWidth;
            double factorY = (double)inCellY / (double)cellHeight;
            double factorZ = (double)inCellZ / (double)cellWidth;

            std::cout << "1. CELL INFO:" << std::endl;
            std::cout << "   Cell indices: (" << cellXIndex << ", " << cellYIndex << ", " << cellZIndex << ")" << std::endl;
            std::cout << "   In-cell pos: (" << inCellX << ", " << inCellY << ", " << inCellZ << ")" << std::endl;
            std::cout << "   Factors: (" << factorX << ", " << factorY << ", " << factorZ << ")" << std::endl;
            std::cout << std::endl;

            // Cell corners
            int cellCornerX = chunkPos.getMinBlockX() + cellXIndex * cellWidth;
            int cellCornerY = noiseSettings.minY() + cellYIndex * cellHeight;
            int cellCornerZ = chunkPos.getMinBlockZ() + cellZIndex * cellWidth;

            std::cout << "2. CELL CORNER DENSITIES (finalDensity):" << std::endl;
            int offsets[8][3] = {
                {0, 0, 0}, {cellWidth, 0, 0}, {0, cellHeight, 0}, {cellWidth, cellHeight, 0},
                {0, 0, cellWidth}, {cellWidth, 0, cellWidth}, {0, cellHeight, cellWidth}, {cellWidth, cellHeight, cellWidth}
            };
            const char* names[8] = {"000", "100", "010", "110", "001", "101", "011", "111"};
            double corners[8];

            for (int i = 0; i < 8; i++) {
                int cx = cellCornerX + offsets[i][0];
                int cy = cellCornerY + offsets[i][1];
                int cz = cellCornerZ + offsets[i][2];
                DensityFunction::SinglePointContext ctx(cx, cy, cz);
                corners[i] = wrappedRouter->finalDensity()->compute(ctx);
                std::cout << "   noise" << names[i] << " (" << cx << "," << cy << "," << cz << "): " << corners[i] << std::endl;
            }
            std::cout << std::endl;

            // Manual interpolation
            std::cout << "3. INTERPOLATION:" << std::endl;
            double valueXZ00 = Mth::lerp(factorY, corners[0], corners[2]);
            double valueXZ10 = Mth::lerp(factorY, corners[1], corners[3]);
            double valueXZ01 = Mth::lerp(factorY, corners[4], corners[6]);
            double valueXZ11 = Mth::lerp(factorY, corners[5], corners[7]);
            std::cout << "   valueXZ00 (lerp Y): " << valueXZ00 << std::endl;
            std::cout << "   valueXZ10 (lerp Y): " << valueXZ10 << std::endl;
            std::cout << "   valueXZ01 (lerp Y): " << valueXZ01 << std::endl;
            std::cout << "   valueXZ11 (lerp Y): " << valueXZ11 << std::endl;

            double valueZ0 = Mth::lerp(factorX, valueXZ00, valueXZ10);
            double valueZ1 = Mth::lerp(factorX, valueXZ01, valueXZ11);
            std::cout << "   valueZ0 (lerp X): " << valueZ0 << std::endl;
            std::cout << "   valueZ1 (lerp X): " << valueZ1 << std::endl;

            double interpolated = Mth::lerp(factorZ, valueZ0, valueZ1);
            std::cout << "   INTERPOLATED (lerp Z): " << interpolated << std::endl;
            std::cout << std::endl;

            // Point sampled
            std::cout << "4. POINT-SAMPLED DENSITY:" << std::endl;
            DensityFunction::SinglePointContext pointCtx(worldX, worldY, worldZ);
            double pointDensity = wrappedRouter->finalDensity()->compute(pointCtx);
            std::cout << "   finalDensity: " << pointDensity << std::endl;
            std::cout << std::endl;

            // Aquifer test
            std::cout << "5. AQUIFER.computeSubstance:" << std::endl;
            std::cout << "   Input density: " << pointDensity << std::endl;

            ::BlockState* result = noiseChunk->aquifer()->computeSubstance(pointCtx, pointDensity);
            std::cout << "   Result: " << (result ? result->getIdentifier() : "null (SOLID)") << std::endl;

            ::BlockState* resultInterp = noiseChunk->aquifer()->computeSubstance(pointCtx, interpolated);
            std::cout << "   Result (with interpolated): " << (resultInterp ? resultInterp->getIdentifier() : "null (SOLID)") << std::endl;
            std::cout << std::endl;

            std::cout << "6. ACTUAL BLOCK (getInterpolatedState during fill):" << std::endl;
            std::cout << "   (Run actual chunk generation to see)" << std::endl;
            std::cout << std::endl;
        }

        std::cout << "=== Trace Complete ===" << std::endl;

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
