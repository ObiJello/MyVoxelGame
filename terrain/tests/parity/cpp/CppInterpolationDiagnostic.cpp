/**
 * C++ Interpolation Diagnostic Test
 *
 * Traces the actual interpolated density values during chunk generation
 * at specific positions where C++ and Java outputs differ.
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
static const int CHUNK_X = 4;
static const int CHUNK_Z = -2;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

// Target positions to trace
struct TracePosition {
    int x, y, z;
    const char* cppBlock;
    const char* javaBlock;
};

static const std::vector<TracePosition> TRACE_POSITIONS = {
    {10, -49, 9, "deepslate", "tuff"},
    {10, 47, 7, "stone", "granite"},
};

int main(int argc, char* argv[]) {
    std::cout << "=== C++ Interpolation Diagnostic Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << std::endl;

    try {
        minecraft::world::level::block::Blocks::bootstrap();

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

        // Bootstrap registries
        std::cout << "Bootstrapping registries..." << std::endl;
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

        std::cout << "Creating NoiseChunk..." << std::endl;
        Beardifier* beardifier = Beardifier::EMPTY();
        minecraft::Blender* blender = minecraft::Blender::empty();

        auto makeNoiseChunk = [&]() {
            return NoiseChunk::forChunk(
                chunk,
                *randomState,
                beardifier,
                *settings,
                fluidPicker,
                blender
            );
        };

        NoiseChunk* noiseChunk = makeNoiseChunk();

        // Get noise settings for cell dimensions
        int cellWidth = noiseSettings.getCellWidth();   // Usually 4
        int cellHeight = noiseSettings.getCellHeight(); // Usually 8
        int cellCountXZ = 16 / cellWidth;               // Usually 4
        int cellCountY = noiseSettings.height() / cellHeight;
        int cellNoiseMinY = noiseSettings.minY() / cellHeight;

        std::cout << std::endl;
        std::cout << "Cell dimensions:" << std::endl;
        std::cout << "  cellWidth: " << cellWidth << " blocks" << std::endl;
        std::cout << "  cellHeight: " << cellHeight << " blocks" << std::endl;
        std::cout << "  cellCountXZ: " << cellCountXZ << std::endl;
        std::cout << "  cellCountY: " << cellCountY << std::endl;
        std::cout << "  cellNoiseMinY: " << cellNoiseMinY << std::endl;
        std::cout << std::endl;

        std::cout << std::fixed << std::setprecision(10);

        // For each trace position, show which cell it's in and the cell corner values
        for (const auto& pos : TRACE_POSITIONS) {
            int worldX = chunkPos.getMinBlockX() + pos.x;
            int worldZ = chunkPos.getMinBlockZ() + pos.z;
            int worldY = pos.y;

            // Calculate which cell this block is in
            int cellXIndex = pos.x / cellWidth;
            int cellZIndex = pos.z / cellWidth;
            int cellYIndex = (worldY - noiseSettings.minY()) / cellHeight;

            // Calculate position within cell (0 to 1)
            double factorX = (double)(pos.x % cellWidth) / cellWidth;
            double factorY = (double)((worldY - noiseSettings.minY()) % cellHeight) / cellHeight;
            double factorZ = (double)(pos.z % cellWidth) / cellWidth;

            std::cout << "=== Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ") ===" << std::endl;
            std::cout << "World coords: (" << worldX << ", " << worldY << ", " << worldZ << ")" << std::endl;
            std::cout << "Expected: C++=" << pos.cppBlock << ", Java=" << pos.javaBlock << std::endl;
            std::cout << std::endl;

            std::cout << "Cell indices: (" << cellXIndex << ", " << cellYIndex << ", " << cellZIndex << ")" << std::endl;
            std::cout << "Factor within cell: (" << factorX << ", " << factorY << ", " << factorZ << ")" << std::endl;
            std::cout << std::endl;

            // Calculate cell corner world positions
            int cellCornerX = chunkPos.getMinBlockX() + cellXIndex * cellWidth;
            int cellCornerY = noiseSettings.minY() + cellYIndex * cellHeight;
            int cellCornerZ = chunkPos.getMinBlockZ() + cellZIndex * cellWidth;

            std::cout << "Cell corners (world coords):" << std::endl;

            // Sample density at all 8 cell corners using point sampling
            NoiseRouter* wrappedRouter = randomState->router();

            // Corner positions: (X, Y, Z) where each can be +0 or +cellWidth/Height
            int cornerOffsets[8][3] = {
                {0, 0, 0},
                {0, 0, cellWidth},
                {cellWidth, 0, 0},
                {cellWidth, 0, cellWidth},
                {0, cellHeight, 0},
                {0, cellHeight, cellWidth},
                {cellWidth, cellHeight, 0},
                {cellWidth, cellHeight, cellWidth}
            };

            auto printInterpolated = [&](const char* name, DensityFunction* function) {
                double cornerValues[8];
                for (int i = 0; i < 8; i++) {
                    int cx = cellCornerX + cornerOffsets[i][0];
                    int cy = cellCornerY + cornerOffsets[i][1];
                    int cz = cellCornerZ + cornerOffsets[i][2];

                    DensityFunction::SinglePointContext ctx(cx, cy, cz);
                    cornerValues[i] = function->compute(ctx);
                }

                double v000 = cornerValues[0];
                double v001 = cornerValues[1];
                double v100 = cornerValues[2];
                double v101 = cornerValues[3];
                double v010 = cornerValues[4];
                double v011 = cornerValues[5];
                double v110 = cornerValues[6];
                double v111 = cornerValues[7];

                double vXZ00 = Mth::lerp(factorY, v000, v010);
                double vXZ10 = Mth::lerp(factorY, v100, v110);
                double vXZ01 = Mth::lerp(factorY, v001, v011);
                double vXZ11 = Mth::lerp(factorY, v101, v111);
                double vZ0 = Mth::lerp(factorX, vXZ00, vXZ10);
                double vZ1 = Mth::lerp(factorX, vXZ01, vXZ11);
                double interpolated = Mth::lerp(factorZ, vZ0, vZ1);

                DensityFunction::SinglePointContext exactCtx(worldX, worldY, worldZ);
                double pointSampled = function->compute(exactCtx);

                std::cout << name << ":" << std::endl;
                for (int i = 0; i < 8; i++) {
                    int cx = cellCornerX + cornerOffsets[i][0];
                    int cy = cellCornerY + cornerOffsets[i][1];
                    int cz = cellCornerZ + cornerOffsets[i][2];
                    std::cout << "  Corner " << i << " (" << cx << ", " << cy << ", " << cz << "): "
                              << cornerValues[i] << std::endl;
                }
                std::cout << "  Interpolated: " << interpolated << std::endl;
                std::cout << "  Point-sampled: " << pointSampled << std::endl;
                std::cout << std::endl;
            };

            printInterpolated("finalDensity", wrappedRouter->finalDensity());
            printInterpolated("veinToggle", wrappedRouter->veinToggle());
            printInterpolated("veinRidged", wrappedRouter->veinRidged());

            DensityFunction::SinglePointContext exactCtx(worldX, worldY, worldZ);
            std::cout << "veinGap:" << std::endl;
            std::cout << "  Point-sampled: " << wrappedRouter->veinGap()->compute(exactCtx) << std::endl;
            std::cout << std::endl;

            NoiseChunk* liveNoiseChunk = makeNoiseChunk();
            liveNoiseChunk->initializeForFirstCellX();
            for (int currentCellX = 0; currentCellX <= cellXIndex; ++currentCellX) {
                liveNoiseChunk->advanceCellX(currentCellX);
                if (currentCellX != cellXIndex) {
                    liveNoiseChunk->swapSlices();
                }
            }
            liveNoiseChunk->selectCellYZ(cellYIndex, cellZIndex);
            liveNoiseChunk->updateForY(worldY, factorY);
            liveNoiseChunk->updateForX(worldX, factorX);
            liveNoiseChunk->updateForZ(worldZ, factorZ);

            std::cout << "Live NoiseChunk wrapped values:" << std::endl;
            std::cout << "  finalDensity: " << liveNoiseChunk->getWrappedFinalDensityForDebug()->compute(*liveNoiseChunk) << std::endl;
            std::cout << "  veinToggle:   " << liveNoiseChunk->getWrappedVeinToggleForDebug()->compute(*liveNoiseChunk) << std::endl;
            std::cout << "  veinRidged:   " << liveNoiseChunk->getWrappedVeinRidgedForDebug()->compute(*liveNoiseChunk) << std::endl;
            std::cout << "  veinGap:      " << liveNoiseChunk->getWrappedVeinGapForDebug()->compute(*liveNoiseChunk) << std::endl;

            BlockState* liveState = liveNoiseChunk->getInterpolatedState();
            std::cout << "  interpolatedState: "
                      << (liveState ? liveState->getBlock()->getIdentifier() : std::string("air")) << std::endl;
            std::cout << std::endl;

            liveNoiseChunk->stopInterpolation();
            delete liveNoiseChunk;
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
