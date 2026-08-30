/**
 * C++ Features Parity Test
 *
 * Generates chunks with phases 3-7 (BIOMES, NOISE, SURFACE, CARVERS, FEATURES)
 * For comparison with the Java MinecraftAsyncChunkTest --phases 3-7
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
#include <cstring>

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
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "core/BlockPos.h"

// Test parameters - must match Java test
static const int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

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

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [options]\n"
              << "Options:\n"
              << "  --phases <3-6|3-7>   Phase range (default: 3-7)\n"
              << "                       3-6: BIOMES, NOISE, SURFACE, CARVERS\n"
              << "                       3-7: BIOMES, NOISE, SURFACE, CARVERS, FEATURES\n"
              << "  --radius <N>         Radius in chunks (default: 50 = 101x101 grid)\n"
              << "  --center <X> <Z>     Center chunk coordinates (default: 0 0)\n"
              << "  --output <path>      Output file path\n"
              << "  --help               Show this help\n";
}

int main(int argc, char* argv[]) {
    // Initialize block registry
    minecraft::world::level::block::Blocks::bootstrap();

    // Default parameters
    std::string phases = "3-7";
    int radius = 50;
    int centerX = 0;
    int centerZ = 0;
    std::string outputPath = "";

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--phases") == 0 && i + 1 < argc) {
            phases = argv[++i];
        } else if (strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            radius = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--center") == 0 && i + 2 < argc) {
            centerX = std::stoi(argv[++i]);
            centerZ = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Configure phases
    bool runFeatures = false;
    std::string phasesDescription;
    if (phases == "3-6") {
        runFeatures = false;
        phasesDescription = "3-6 (BIOMES, NOISE, SURFACE, CARVERS)";
    } else if (phases == "3-7") {
        runFeatures = true;
        phasesDescription = "3-7 (BIOMES, NOISE, SURFACE, CARVERS, FEATURES)";
    } else {
        std::cerr << "Invalid phases: " << phases << ". Use 3-6 or 3-7.\n";
        return 1;
    }

    // Set default output path based on phases
    if (outputPath.empty()) {
        outputPath = "tests/output/cpp_phases_" + phases + "_features.txt";
    }

    int gridSize = radius * 2 + 1;
    int totalChunks = gridSize * gridSize;

    std::cout << "=== C++ Features Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Phases: " << phasesDescription << std::endl;
    std::cout << "Center: (" << centerX << ", " << centerZ << ")" << std::endl;
    std::cout << "Radius: " << radius << std::endl;
    std::cout << "Grid: " << gridSize << "x" << gridSize << " chunks" << std::endl;
    std::cout << "Total chunks: " << totalChunks << std::endl;
    std::cout << "Output: " << outputPath << std::endl;
    std::cout << std::endl;

    try {
        // Get block types
        std::cout << "Getting block types..." << std::endl;
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

        if (runFeatures) {
            std::cout << "Bootstrapping BiomeFeatureRegistry..." << std::endl;
            minecraft::data::worldgen::BiomeFeatureRegistry::bootstrap();
        }

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

        // Create runner config for phases 3-7 (no structures)
        std::cout << "Creating ChunkGenerationRunner..." << std::endl;
        minecraft::levelgen::ChunkGenerationRunner::Config config;
        config.runStructures = false;    // No structures (would be phases 1-2)
        config.runBiomes = true;         // Phase 3: BIOMES
        config.runNoise = true;          // Phase 4: NOISE
        config.runSurface = true;        // Phase 5: SURFACE
        config.runCarvers = true;        // Phase 6: CARVERS
        config.runFeatures = runFeatures; // Phase 7: FEATURES (ores, trees, etc.)
        config.runLighting = false;      // Skip lighting
        config.runSpawn = false;         // Skip spawn

        minecraft::levelgen::ChunkGenerationRunner runner(generator, randomState, SEED, config);

        // Set chunk creation params for multi-chunk features (trees)
        runner.setChunkCreationParams(MIN_Y, HEIGHT, airBlock, stoneBlock, registry);

        std::cout << "Setup complete. Starting chunk generation..." << std::endl;
        std::cout << std::endl;

        // Open output file
        std::ofstream writer(outputPath);
        if (!writer.is_open()) {
            std::cerr << "Failed to open output file: " << outputPath << std::endl;
            return 1;
        }

        writer << "# C++ Features Parity Test Output\n";
        writer << "# Seed: " << SEED << "\n";
        writer << "# Center: (" << centerX << ", " << centerZ << ")\n";
        writer << "# Radius: " << radius << "\n";
        writer << "# Phases: " << phasesDescription << "\n";
        writer << "# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...\n";
        writer << "\n";

        int completed = 0;
        auto startTime = std::chrono::steady_clock::now();

        for (int cz = centerZ - radius; cz <= centerZ + radius; cz++) {
            for (int cx = centerX - radius; cx <= centerX + radius; cx++) {
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

                // Add block counts (sorted by count descending)
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
        std::cerr << "Error during chunk generation: " << e.what() << std::endl;
        return 1;
    }
}
