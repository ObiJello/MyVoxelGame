/**
 * C++ Bulk Chunk Generation Parity Test
 *
 * Generates a 100x100 grid of chunks and outputs summary data for each.
 * For comparison with the Java MinecraftBulkTest.
 */

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <memory>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <cstdlib>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/LevelChunkSection.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "core/BlockPos.h"

// Test parameters - must match Java test
static int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

// Grid parameters - 100x100 chunks centered around origin
static const int GRID_SIZE = 100;
static const int START_X = -GRID_SIZE / 2;  // -50
static const int START_Z = -GRID_SIZE / 2;  // -50

struct ChunkSummary {
    int64_t airBlocks;
    int64_t nonAirBlocks;
    std::map<std::string, int64_t> blockCounts;
};

std::vector<std::pair<std::string, int64_t>> getSortedBlockCounts(
    const std::map<std::string, int64_t>& blockCounts) {
    std::vector<std::pair<std::string, int64_t>> sorted(blockCounts.begin(), blockCounts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    return sorted;
}

ChunkSummary calculateChunkSummary(minecraft::world::IChunk* chunk) {
    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    int64_t airCount = 0;
    int64_t nonAirCount = 0;
    std::map<std::string, int64_t> blockCounts;

    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                std::string blockName = state ? state->getIdentifier() : "minecraft:air";

                blockCounts[blockName]++;

                if (blockName == "minecraft:air") {
                    airCount++;
                } else {
                    nonAirCount++;
                }
            }
        }
    }

    return {airCount, nonAirCount, blockCounts};
}

int main(int argc, char* argv[]) {
    std::string outputPath = argc > 1 ? argv[1] : "tests/output/cpp_bulk_100x100.txt";
    if (argc > 2) {
        SEED = std::strtoll(argv[2], nullptr, 10);
    }

    std::cout << "=== C++ Bulk Chunk Generation Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Grid: " << GRID_SIZE << "x" << GRID_SIZE << " chunks" << std::endl;
    std::cout << "Range: (" << START_X << "," << START_Z << ") to ("
              << (START_X + GRID_SIZE - 1) << "," << (START_Z + GRID_SIZE - 1) << ")" << std::endl;
    std::cout << "Total chunks: " << (GRID_SIZE * GRID_SIZE) << std::endl;
    std::cout << std::endl;

    try {
        // Get block types
        std::cout << "Getting block types..." << std::endl;
        minecraft::world::level::block::Blocks::bootstrap();
        minecraft::BlockState* airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        minecraft::BlockState* stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        // Create block registry
        std::cout << "Creating block registry..." << std::endl;
        minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
        registry->registerBlock(airBlock);
        registry->registerBlock(stoneBlock);
        registry->registerBlock(minecraft::world::level::block::Blocks::WATER->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::GRASS_BLOCK->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::DIRT->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::SAND->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::GRAVEL->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::TUFF->defaultBlockState());

        // Bootstrap registries
        std::cout << "Bootstrapping NoiseRegistry..." << std::endl;
        minecraft::levelgen::NoiseRegistry::bootstrap();

        std::cout << "Bootstrapping DensityFunctionRegistry for seed: " << SEED << std::endl;
        minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);

        std::cout << "Initializing SurfaceRuleData..." << std::endl;
        minecraft::levelgen::SurfaceRuleData::initialize();

        // Build noise router
        std::cout << "Building overworld NoiseRouter..." << std::endl;
        minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);

        // Create settings
        std::cout << "Creating NoiseGeneratorSettings..." << std::endl;
        minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
        
        

        minecraft::levelgen::NoiseGeneratorSettings* settings = new minecraft::levelgen::NoiseGeneratorSettings(
            noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
        );

        // Create RandomState
        std::cout << "Creating RandomState for seed: " << SEED << std::endl;
        minecraft::levelgen::RandomState* randomState = minecraft::levelgen::RandomState::create(settings, SEED);

        // Create surface rules
        std::cout << "Creating overworld surface rules..." << std::endl;
        minecraft::levelgen::RuleSource* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

        // Create FluidPicker
        std::cout << "Creating OverworldFluidPicker..." << std::endl;
        minecraft::levelgen::FluidPicker* fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
            63, -54, minecraft::world::level::block::Blocks::WATER->defaultBlockState(), minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        // Create biome source
        std::cout << "Creating MultiNoiseBiomeSource..." << std::endl;
        std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> biomeSource =
            minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

        // Create chunk generator
        std::cout << "Creating NoiseBasedChunkGenerator..." << std::endl;
        minecraft::levelgen::NoiseBasedChunkGenerator* generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
            settings, randomState->surfaceSystem(), surfaceRules, stoneBlock, airBlock, fluidPicker, nullptr
        );

        // Set biome source on generator
        generator->setBiomeSource(biomeSource.get());

        // Create runner config
        std::cout << "Creating ChunkGenerationRunner..." << std::endl;
        minecraft::levelgen::ChunkGenerationRunner::Config config =
            minecraft::levelgen::ChunkGenerationRunner::Config::terrainWithCaves();
        config.runCarvers = true;
        config.runSurface = true;

        minecraft::levelgen::ChunkGenerationRunner runner(generator, randomState, SEED, config);

        std::cout << "Setup complete. Starting chunk generation..." << std::endl;
        std::cout << std::endl;

        // Open output file
        std::ofstream writer(outputPath);
        if (!writer.is_open()) {
            std::cerr << "Failed to open output file: " << outputPath << std::endl;
            return 1;
        }

        writer << "# C++ Bulk Chunk Test Output\n";
        writer << "# Seed: " << SEED << "\n";
        writer << "# Grid: " << GRID_SIZE << "x" << GRID_SIZE << "\n";
        writer << "# Phases: BIOMES, NOISE, SURFACE, CARVERS\n";
        writer << "# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...\n";
        writer << "\n";

        int totalChunks = GRID_SIZE * GRID_SIZE;
        int completed = 0;
        auto startTime = std::chrono::steady_clock::now();

        for (int cz = START_Z; cz < START_Z + GRID_SIZE; cz++) {
            for (int cx = START_X; cx < START_X + GRID_SIZE; cx++) {
                // Create chunk
                minecraft::world::ChunkPos chunkPos(cx, cz);
                minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
                    chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
                );

                // Generate the chunk
                runner.generateChunk(chunk);

                // Calculate summary
                ChunkSummary summary = calculateChunkSummary(chunk);

                // Write result: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...
                writer << cx << "," << cz << ","
                       << summary.airBlocks << ","
                       << summary.nonAirBlocks;

                // Add block counts (sorted by count descending, matching async test output)
                auto sorted = getSortedBlockCounts(summary.blockCounts);
                for (const auto& entry : sorted) {
                    writer << "," << entry.first << ":" << entry.second;
                }
                writer << "\n";

                // Cleanup
                delete chunk;

                completed++;
                if (completed % 100 == 0) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - startTime).count();
                    double rate = completed / elapsed;
                    double remaining = (totalChunks - completed) / rate;
                    std::cout << "Progress: " << completed << "/" << totalChunks
                              << " chunks (" << std::fixed << std::setprecision(1)
                              << (100.0 * completed / totalChunks) << "%) - "
                              << std::setprecision(1) << rate << " chunks/sec - ETA: "
                              << std::setprecision(0) << remaining << " sec" << std::endl;
                }
            }
        }

        writer.close();

        auto endTime = std::chrono::steady_clock::now();
        double totalTime = std::chrono::duration<double>(endTime - startTime).count();

        std::cout << std::endl;
        std::cout << "Generation complete!" << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(1) << totalTime << " seconds" << std::endl;
        std::cout << "Average: " << std::setprecision(2) << (totalTime * 1000.0 / totalChunks) << " ms per chunk" << std::endl;
        std::cout << "Output written to: " << outputPath << std::endl;

        // Cleanup
        delete generator;
        delete randomState;
        delete settings;
        delete registry;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error during bulk chunk generation: " << e.what() << std::endl;
        return 1;
    }
}
