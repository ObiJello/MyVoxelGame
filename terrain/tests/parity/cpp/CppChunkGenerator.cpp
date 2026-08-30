/**
 * C++ Chunk Generation Parity Test
 *
 * This test generates a chunk using the C++ implementation
 * for comparison with the Java Minecraft implementation.
 *
 * Test Parameters (must match Java test):
 *   Seed: 12345
 *   Chunk: (0, 0)
 *   Y range: -64 to 320
 */

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <memory>
#include <algorithm>

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

// Test parameters - these must match the Java test
static const int64_t SEED = 12345L;
static int CHUNK_X = 0;  // Can be overridden by command line args
static int CHUNK_Z = 0;  // Can be overridden by command line args
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

void writeChunkData(minecraft::world::IChunk* chunk, const std::string& outputPath) {
    std::ofstream writer(outputPath);
    if (!writer.is_open()) {
        std::cerr << "Failed to open output file: " << outputPath << std::endl;
        return;
    }

    // Write header
    writer << "# C++ Chunk Output\n";
    writer << "# Seed: " << SEED << "\n";
    writer << "# Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")\n";
    writer << "# Format: x,y,z,block_name\n";
    writer << "\n";

    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    int blockCount = 0;
    int airCount = 0;
    std::map<std::string, int> blockCounts;

    // Iterate through all blocks in the chunk
    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                std::string blockName = state ? state->getIdentifier() : "minecraft:air";

                // Write block data
                writer << x << "," << y << "," << z << "," << blockName << "\n";

                // Count blocks
                blockCount++;
                if (blockName == "minecraft:air") {
                    airCount++;
                }
                blockCounts[blockName]++;
            }
        }
    }

    // Write summary at end
    writer << "\n";
    writer << "# Summary:\n";
    writer << "# Total blocks: " << blockCount << "\n";
    writer << "# Air blocks: " << airCount << "\n";
    writer << "# Non-air blocks: " << (blockCount - airCount) << "\n";
    writer << "# Block types:\n";

    // Sort by count (descending)
    std::vector<std::pair<std::string, int>> sorted(blockCounts.begin(), blockCounts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& entry : sorted) {
        writer << "#   " << entry.first << ": " << entry.second << "\n";
    }

    writer.close();

    std::cout << "Total blocks written: " << blockCount << std::endl;
    std::cout << "Air blocks: " << airCount << std::endl;
    std::cout << "Non-air blocks: " << (blockCount - airCount) << std::endl;
}

int main(int argc, char* argv[]) {
    // Parse command line args: parity_test [chunkX] [chunkZ] [outputFile]
    if (argc >= 3) {
        CHUNK_X = std::atoi(argv[1]);
        CHUNK_Z = std::atoi(argv[2]);
    }

    std::cout << "=== C++ Chunk Generation Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;
    std::cout << "Y range: " << MIN_Y << " to " << MAX_Y << std::endl;
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
        // Register other common blocks
        registry->registerBlock(minecraft::world::level::block::Blocks::WATER->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::GRASS_BLOCK->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::DIRT->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::SAND->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::GRAVEL->defaultBlockState());
        registry->registerBlock(minecraft::world::level::block::Blocks::TUFF->defaultBlockState());

        // Bootstrap the noise registries with our seed
        // This must happen BEFORE creating NoiseGeneratorSettings or RandomState
        std::cout << "Bootstrapping NoiseRegistry..." << std::endl;
        minecraft::levelgen::NoiseRegistry::bootstrap();

        std::cout << "Bootstrapping DensityFunctionRegistry for seed: " << SEED << std::endl;
        minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);

        // Initialize surface rule data (must be done before creating surface rules)
        std::cout << "Initializing SurfaceRuleData..." << std::endl;
        minecraft::levelgen::SurfaceRuleData::initialize();

        // Build the overworld noise router
        std::cout << "Building overworld NoiseRouter..." << std::endl;
        minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);

        // Create overworld noise generator settings with proper router
        std::cout << "Creating NoiseGeneratorSettings..." << std::endl;
        minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

        minecraft::levelgen::NoiseGeneratorSettings* settings = new minecraft::levelgen::NoiseGeneratorSettings(
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

        // Create RandomState from settings and seed
        std::cout << "Creating RandomState for seed: " << SEED << std::endl;
        minecraft::levelgen::RandomState* randomState = minecraft::levelgen::RandomState::create(settings, SEED);

        // Create the overworld surface rules
        std::cout << "Creating overworld surface rules..." << std::endl;
        minecraft::levelgen::RuleSource* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

        // Create the overworld fluid picker
        // Java: Uses a lambda (x, y, z) -> y < Math.min(-54, seaLevel) ? lavaFluid : seaFluid
        std::cout << "Creating OverworldFluidPicker..." << std::endl;
        minecraft::levelgen::FluidPicker* fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
            63,   // seaLevel
            -54,  // lavaLevel (below this Y, return lava)
            minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        // Create biome source for the overworld
        std::cout << "Creating MultiNoiseBiomeSource..." << std::endl;
        std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> biomeSource =
            minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

        // Create the chunk generator with the settings
        std::cout << "Creating NoiseBasedChunkGenerator..." << std::endl;
        minecraft::levelgen::NoiseBasedChunkGenerator* generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
            settings,
            randomState->surfaceSystem(),
            surfaceRules,  // Use overworld surface rules
            stoneBlock,
            airBlock,
            fluidPicker,  // Use the overworld fluid picker
            nullptr       // beardifier - empty for no structures
        );

        // Set biome source on generator
        generator->setBiomeSource(biomeSource.get());

        // Create chunk generation runner
        std::cout << "Creating ChunkGenerationRunner..." << std::endl;
        minecraft::levelgen::ChunkGenerationRunner::Config config =
            minecraft::levelgen::ChunkGenerationRunner::Config::terrainWithCaves();
        config.runCarvers = true;   // Enable carvers for parity testing
        config.runSurface = true;   // Include surface blocks
        config.runFeatures = true;  // Enable features (ores, etc.)

        minecraft::levelgen::ChunkGenerationRunner runner(generator, randomState, SEED, config);

        // Create proto chunk
        std::cout << "Creating ProtoChunk at (" << CHUNK_X << ", " << CHUNK_Z << ")..." << std::endl;
        minecraft::world::ChunkPos chunkPos(CHUNK_X, CHUNK_Z);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos,
            MIN_Y,
            HEIGHT,
            airBlock,
            stoneBlock,
            registry
        );

        // Generate the chunk
        std::cout << "Generating chunk..." << std::endl;
        runner.generateChunk(chunk);

        std::cout << "Chunk generation complete!" << std::endl;

        // Output block data to file
        std::string outputPath;
        if (argc >= 4) {
            outputPath = argv[3];
        } else {
            outputPath = "cpp_chunk_" + std::to_string(CHUNK_X) + "_" + std::to_string(CHUNK_Z) + ".txt";
        }
        std::cout << "Writing output to: " << outputPath << std::endl;

        writeChunkData(chunk, outputPath);

        std::cout << "Done!" << std::endl;

        // Cleanup
        delete chunk;
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
