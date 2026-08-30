/**
 * C++ Carver Trace Test
 *
 * Traces ALL values in the carver chain to compare with Java:
 * 1. setLargeFeatureSeed calculation
 * 2. isStartChunk random values
 * 3. Cave count and parameters
 * 4. Which blocks get carved
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <set>

#include "levelgen/ChunkGenerator.h"
#include "levelgen/ChunkGenerationRunner.h"
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
#include "levelgen/carver/CaveWorldCarver.h"
#include "levelgen/carver/CanyonWorldCarver.h"
#include "levelgen/carver/ConfiguredWorldCarver.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/carver/CarverConfiguration.h"
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
#include "random/LegacyRandomSource.h"
#include "random/RandomSupport.h"
#include "math/Mth.h"

using namespace minecraft::levelgen;
using namespace minecraft::levelgen::carver;
using namespace minecraft::density;
using minecraft::Mth;
using minecraft::LegacyRandomSource;
using minecraft::RandomSupport;
using minecraft::core::BlockPos;

// Test parameters - must match Java
static const int64_t SEED = 12345L;
static const int CHUNK_X = 0;
static const int CHUNK_Z = 0;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

// Positions to trace (local coordinates within chunk)
struct TracePos {
    int x, y, z;
    const char* cppBlock;
    const char* javaBlock;
};

static const std::vector<TracePos> TRACE_POSITIONS = {
    {1, -55, 11, "lava", "deepslate"},
    {0, -15, 7, "air", "deepslate"},
    {0, -15, 8, "air", "deepslate"},
};

// Track which positions get carved
static std::set<std::tuple<int,int,int>> carvedPositions;

int main() {
    std::cout << "=== C++ Carver Trace ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << std::endl;

    // DEBUG: Test LegacyRandomSource BEFORE any bootstrap
    {
        LegacyRandomSource testRng(12345);
        int64_t testXScale = testRng.nextLong();
        int64_t testZScale = testRng.nextLong();
        std::cout << "DEBUG PRE-BOOTSTRAP: seed=12345 xScale=" << testXScale
                  << " zScale=" << testZScale << std::endl;
    }

    try {
        // Setup blocks
        ::BlockState* airBlock = ::minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        ::BlockState* stoneBlock = ::minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
        registry->registerBlock(airBlock);
        registry->registerBlock(stoneBlock);
        registry->registerBlock(::minecraft::world::level::block::Blocks::WATER->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());
        registry->registerBlock(::minecraft::world::level::block::Blocks::CAVE_AIR->defaultBlockState());

        // Bootstrap
        NoiseRegistry::bootstrap();
        DensityFunctionRegistry::bootstrap(SEED);
        SurfaceRuleData::initialize();

        // DEBUG: Test LegacyRandomSource AFTER bootstrap
        {
            LegacyRandomSource testRng(12345);
            int64_t testXScale = testRng.nextLong();
            int64_t testZScale = testRng.nextLong();
            std::cout << "DEBUG POST-BOOTSTRAP: seed=12345 xScale=" << testXScale
                      << " zScale=" << testZScale << std::endl;
        }

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

        // First fill the chunk with noise (so we have terrain to carve)
        std::cout << "=== Filling chunk with noise ===" << std::endl;
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

        // Fill with noise using simplified approach
        int cellWidth = noiseSettings.getCellWidth();
        int cellHeight = noiseSettings.getCellHeight();
        int cellCountY = noiseSettings.height() / cellHeight;
        int cellCountXZ = 16 / cellWidth;

        noiseChunk->initializeForFirstCellX();

        for (int cellX = 0; cellX < cellCountXZ; ++cellX) {
            noiseChunk->advanceCellX(cellX);

            for (int cellZ = 0; cellZ < cellCountXZ; ++cellZ) {
                int sectionIndex = chunk->getSectionsCount() - 1;

                for (int cellY = cellCountY - 1; cellY >= 0; --cellY) {
                    noiseChunk->selectCellYZ(cellY, cellZ);

                    for (int inCellY = cellHeight - 1; inCellY >= 0; --inCellY) {
                        int worldY = (noiseSettings.minY() / cellHeight + cellY) * cellHeight + inCellY;
                        int localY = worldY & 15;
                        int sectionIdx = chunk->getSectionIndex(worldY);

                        if (sectionIndex != sectionIdx) {
                            sectionIndex = sectionIdx;
                        }

                        noiseChunk->updateForY(worldY, inCellY / (double)cellHeight);

                        for (int inCellX = 0; inCellX < cellWidth; ++inCellX) {
                            int localX = cellX * cellWidth + inCellX;
                            noiseChunk->updateForX(localX, inCellX / (double)cellWidth);

                            for (int inCellZ = 0; inCellZ < cellWidth; ++inCellZ) {
                                int localZ = cellZ * cellWidth + inCellZ;
                                noiseChunk->updateForZ(localZ, inCellZ / (double)cellWidth);

                                ::BlockState* state = noiseChunk->getInterpolatedState();
                                if (state == nullptr) {
                                    state = stoneBlock;  // Use stone as default
                                }

                                if (state && !state->isAir()) {
                                    chunk->getSection(sectionIndex).setBlockState(localX, localY, localZ, state, false);
                                }
                            }
                        }
                    }
                }
            }
            noiseChunk->swapSlices();
        }
        noiseChunk->stopInterpolation();

        // Log blocks at trace positions BEFORE carving
        std::cout << std::endl << "=== Blocks BEFORE carving ===" << std::endl;
        for (const auto& pos : TRACE_POSITIONS) {
            BlockPos blockPos(pos.x, pos.y, pos.z);
            ::BlockState* block = chunk->getBlockState(blockPos);
            std::cout << "  (" << pos.x << ", " << pos.y << ", " << pos.z << "): "
                      << (block ? block->getIdentifier() : "null") << std::endl;
        }

        // Now trace carver setup
        std::cout << std::endl << "=== Carver Seed Tracing ===" << std::endl;

        // Set up carver configuration
        static std::set<std::string> replaceable = {
            "minecraft:stone", "minecraft:granite", "minecraft:diorite", "minecraft:andesite",
            "minecraft:dirt", "minecraft:coarse_dirt", "minecraft:podzol", "minecraft:grass_block",
            "minecraft:terracotta", "minecraft:white_terracotta", "minecraft:orange_terracotta",
            "minecraft:red_terracotta", "minecraft:brown_terracotta", "minecraft:yellow_terracotta",
            "minecraft:sand", "minecraft:sandstone", "minecraft:red_sand", "minecraft:red_sandstone",
            "minecraft:gravel", "minecraft:mycelium", "minecraft:snow", "minecraft:packed_ice",
            "minecraft:deepslate", "minecraft:tuff", "minecraft:calcite", "minecraft:smooth_basalt",
            "minecraft:clay", "minecraft:dripstone_block", "minecraft:pointed_dripstone"
        };

        // CAVE carver config
        static UniformHeight caveHeight(VerticalAnchor::aboveBottom(8), VerticalAnchor::absolute(180));
        static UniformFloat caveYScale(0.1f, 0.9f);
        static UniformFloat caveHorizontalMult(0.7f, 1.4f);
        static UniformFloat caveVerticalMult(0.8f, 1.3f);
        static UniformFloat caveFloorLevel(-1.0f, -0.4f);

        static CaveCarverConfiguration caveConfig(
            0.15f, &caveHeight, &caveYScale, VerticalAnchor::aboveBottom(8),
            CarverDebugSettings(), replaceable,
            &caveHorizontalMult, &caveVerticalMult, &caveFloorLevel
        );

        // CAVE_EXTRA_UNDERGROUND config
        static UniformHeight caveExtraHeight(VerticalAnchor::aboveBottom(8), VerticalAnchor::absolute(47));
        static UniformFloat caveExtraYScale(0.1f, 0.9f);
        static UniformFloat caveExtraHorizontalMult(0.7f, 1.4f);
        static UniformFloat caveExtraVerticalMult(0.8f, 1.3f);
        static UniformFloat caveExtraFloorLevel(-1.0f, -0.4f);

        static CaveCarverConfiguration caveExtraConfig(
            0.07f, &caveExtraHeight, &caveExtraYScale, VerticalAnchor::aboveBottom(8),
            CarverDebugSettings(), replaceable,
            &caveExtraHorizontalMult, &caveExtraVerticalMult, &caveExtraFloorLevel
        );

        // CANYON config
        static UniformHeight canyonHeight(VerticalAnchor::absolute(10), VerticalAnchor::absolute(67));
        static ConstantFloat canyonYScale(3.0f);
        static UniformFloat canyonVerticalRotation(-0.125f, 0.125f);
        static UniformFloat canyonDistanceFactor(0.75f, 1.0f);
        static TrapezoidFloat canyonThickness(0.0f, 6.0f, 2.0f);
        static UniformFloat canyonHorizontalRadiusFactor(0.75f, 1.0f);

        static CanyonShapeConfiguration canyonShape(
            &canyonDistanceFactor, &canyonThickness, 3,
            &canyonHorizontalRadiusFactor, 1.0f, 0.0f
        );

        static CanyonCarverConfiguration canyonConfig(
            0.01f, &canyonHeight, &canyonYScale, VerticalAnchor::aboveBottom(8),
            CarverDebugSettings(), replaceable,
            &canyonVerticalRotation, canyonShape
        );

        static CaveWorldCarver caveCarver;
        static CaveWorldCarver caveExtraCarver;
        static CanyonWorldCarver canyonCarver;

        static ConfiguredCaveCarver configuredCaveCarver(&caveCarver, caveConfig);
        static ConfiguredCaveCarver configuredCaveExtraCarver(&caveExtraCarver, caveExtraConfig);
        static ConfiguredCanyonCarver configuredCanyonCarver(&canyonCarver, canyonConfig);

        std::vector<ConfiguredCarverBase*> carvers = {
            &configuredCaveCarver,
            &configuredCaveExtraCarver,
            &configuredCanyonCarver
        };

        // Create carving context
        NoiseChunk* carverNoiseChunk = NoiseChunk::forChunk(
            chunk, *randomState, beardifier, *settings, fluidPicker, blender
        );
        Aquifer* aquifer = carverNoiseChunk->aquifer();

        CarvingContext carvingContext(
            chunk->getMinBuildHeight(),
            chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(),
            carverNoiseChunk,
            randomState,
            nullptr
        );

        // Get carving mask
        CarvingMask& mask = chunk->getOrCreateCarvingMask();

        // Create LegacyRandomSource
        LegacyRandomSource legacyRandom(RandomSupport::generateUniqueSeed().seedLo);

        constexpr int32_t CARVER_RANGE = 8;

        int totalCarvingChunks = 0;

        std::cout << std::endl << "=== Iterating over source chunks ===" << std::endl;

        // DEBUG: Test right before loop
        {
            int64_t testSeed = SEED + 0;  // carverIndex=0
            LegacyRandomSource testRng(testSeed);
            int64_t testXScale = testRng.nextLong();
            int64_t testZScale = testRng.nextLong();
            std::cout << "DEBUG PRE-LOOP: carverSeed=" << testSeed << " xScale=" << testXScale
                      << " zScale=" << testZScale << std::endl;
        }

        int iterCount = 0;
        for (int32_t dx = -CARVER_RANGE; dx <= CARVER_RANGE; ++dx) {
            for (int32_t dz = -CARVER_RANGE; dz <= CARVER_RANGE; ++dz) {
                ::world::ChunkPos sourcePos(chunkPos.x() + dx, chunkPos.z() + dz);

                for (size_t carverIndex = 0; carverIndex < carvers.size(); ++carverIndex) {
                    ConfiguredCarverBase* carver = carvers[carverIndex];

                    // Calculate and trace the seed
                    int64_t carverSeed = SEED + static_cast<int64_t>(carverIndex);

                    // DEBUG: Check when corruption starts
                    if (iterCount < 10) {
                        LegacyRandomSource testCheck(12345L);
                        int64_t testVal = testCheck.nextLong();
                        bool isCorrect = (testVal == 6674089274190705457LL);
                        if (!isCorrect) {
                            std::cout << "CORRUPTION at iter=" << iterCount
                                      << " dx=" << dx << " dz=" << dz << " carverIdx=" << carverIndex
                                      << " got=" << testVal << std::endl;
                        }
                    }
                    iterCount++;

                    // Trace setLargeFeatureSeed calculation
                    // DEBUG: Test without volatile in Debug mode
                    int64_t seedToUse = carverSeed;  // Should be 12345 for carverIndex=0
                    LegacyRandomSource seedCalcRandom(seedToUse);
                    int64_t xScale = seedCalcRandom.nextLong();
                    int64_t zScale = seedCalcRandom.nextLong();

                    // Also trace with literal for comparison
                    if (carverIndex == 0 && iterCount < 15) {
                        LegacyRandomSource literalCheck(12345L);
                        int64_t litX = literalCheck.nextLong();
                        int64_t seedVal = seedToUse;  // Read from volatile
                        if (litX != xScale && seedVal == 12345) {
                            std::cout << "MISMATCH at iter " << iterCount << ": literal=" << litX
                                      << " variable=" << xScale << " seedToUse=" << seedVal << std::endl;
                        }
                    }
                    int64_t finalSeed = static_cast<int64_t>(sourcePos.x()) * xScale ^
                                        static_cast<int64_t>(sourcePos.z()) * zScale ^ carverSeed;

                    legacyRandom.setLargeFeatureSeed(carverSeed, sourcePos.x(), sourcePos.z());

                    // Check isStartChunk and trace the random value
                    float randomValue = legacyRandom.nextFloat();
                    bool isStart = randomValue <= (carverIndex == 2 ? 0.01f : (carverIndex == 1 ? 0.07f : 0.15f));

                    // Reset random for actual carving
                    legacyRandom.setLargeFeatureSeed(carverSeed, sourcePos.x(), sourcePos.z());

                    if (carver->isStartChunk(legacyRandom)) {
                        totalCarvingChunks++;
                        std::cout << "Source chunk (" << sourcePos.x() << ", " << sourcePos.z()
                                  << ") carverIndex=" << carverIndex
                                  << " xScale=" << xScale
                                  << " zScale=" << zScale
                                  << " finalSeed=" << finalSeed
                                  << " randomValue=" << std::fixed << std::setprecision(10) << randomValue
                                  << " isStart=TRUE" << std::endl;

                        // Run the carver
                        carver->carve(
                            carvingContext,
                            chunk,
                            [](const BlockPos& pos) -> void* { return nullptr; },
                            legacyRandom,
                            aquifer,
                            sourcePos,
                            mask
                        );
                    }
                }
            }
        }

        std::cout << std::endl << "Total carving chunks: " << totalCarvingChunks << std::endl;

        // Log blocks at trace positions AFTER carving
        std::cout << std::endl << "=== Blocks AFTER carving ===" << std::endl;
        for (const auto& pos : TRACE_POSITIONS) {
            BlockPos blockPos(pos.x, pos.y, pos.z);
            ::BlockState* block = chunk->getBlockState(blockPos);
            std::cout << "  (" << pos.x << ", " << pos.y << ", " << pos.z << "): "
                      << (block ? block->getIdentifier() : "null")
                      << " (expected C++=" << pos.cppBlock << ", Java=" << pos.javaBlock << ")" << std::endl;
        }

        // Count blocks
        std::cout << std::endl << "=== Block counts ===" << std::endl;
        std::map<std::string, int> blockCounts;
        for (int y = MIN_Y; y < MIN_Y + HEIGHT; ++y) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    BlockPos blockPos(x, y, z);
                    ::BlockState* block = chunk->getBlockState(blockPos);
                    std::string name = block ? block->getIdentifier() : "null";
                    blockCounts[name]++;
                }
            }
        }

        for (const auto& [name, count] : blockCounts) {
            std::cout << "  " << name << ": " << count << std::endl;
        }

        std::cout << std::endl << "=== Carver Trace Complete ===" << std::endl;

        // Cleanup
        delete carverNoiseChunk;
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
