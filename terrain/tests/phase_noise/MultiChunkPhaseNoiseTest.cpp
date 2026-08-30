/**
 * Multi-Chunk Phase Noise Test
 *
 * Tests noise phase generation across multiple chunk positions including:
 * - Positive coordinates
 * - Negative coordinates
 * - Origin and near-origin
 * - Large coordinates
 *
 * Output format matches Java for comparison.
 */

#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <cstdint>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Beardifier.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/LevelChunkSection.h"

using namespace minecraft::levelgen;
using namespace minecraft::world;

// Test parameters
static const int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

// Chunk positions to test - including negatives and edge cases
static const std::vector<std::pair<int, int>> TEST_CHUNKS = {
    // Origin and near-origin
    {0, 0},
    {1, 0},
    {0, 1},
    {1, 1},

    // Negative coordinates
    {-1, 0},
    {0, -1},
    {-1, -1},
    {-2, -2},
    {-5, -5},
    {-10, -10},

    // Mixed positive/negative
    {-1, 1},
    {1, -1},
    {-3, 5},
    {5, -3},

    // Larger coordinates
    {10, 10},
    {-10, 10},
    {10, -10},
    {-10, -10},
    {100, 100},
    {-100, -100},
    {-100, 100},
    {100, -100},
};

std::string getBlockName(::BlockState* block) {
    if (block == nullptr) return "minecraft:air";
    if (block == ::minecraft::world::level::block::Blocks::AIR->defaultBlockState()) return "minecraft:air";
    if (block == ::minecraft::world::level::block::Blocks::WATER->defaultBlockState()) return "minecraft:water";
    if (block == ::minecraft::world::level::block::Blocks::LAVA->defaultBlockState()) return "minecraft:lava";
    if (block == ::minecraft::world::level::block::Blocks::STONE->defaultBlockState()) return "minecraft:stone";
    if (block == ::minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState()) return "minecraft:deepslate";
    if (block == ::minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState()) return "minecraft:bedrock";
    if (block == ::minecraft::world::level::block::Blocks::GRASS_BLOCK->defaultBlockState()) return "minecraft:grass_block";
    if (block == ::minecraft::world::level::block::Blocks::DIRT->defaultBlockState()) return "minecraft:dirt";
    if (block == ::minecraft::world::level::block::Blocks::SAND->defaultBlockState()) return "minecraft:sand";
    if (block == ::minecraft::world::level::block::Blocks::GRAVEL->defaultBlockState()) return "minecraft:gravel";
    return "minecraft:unknown";
}

struct ChunkStats {
    int chunkX, chunkZ;
    int airCount;
    int stoneCount;
    int waterCount;
    int lavaCount;
    int otherCount;
    uint64_t contentHash;
};

ChunkStats generateAndAnalyzeChunk(
    NoiseBasedChunkGenerator* generator,
    RandomState* randomState,
    int chunkX, int chunkZ
) {
    ChunkStats stats = {chunkX, chunkZ, 0, 0, 0, 0, 0, 0};

    // Create ProtoChunk
    ChunkPos chunkPos(chunkX, chunkZ);
    BlockRegistry* blockRegistry = new BlockRegistry();
    ProtoChunk* chunk = new ProtoChunk(
        chunkPos,
        MIN_Y,
        HEIGHT,
        ::minecraft::world::level::block::Blocks::AIR->defaultBlockState(),
        ::minecraft::world::level::block::Blocks::AIR->defaultBlockState(),
        blockRegistry
    );

    // Configure runner for NOISE PHASE ONLY
    ChunkGenerationRunner::Config config;
    config.runStructures = false;
    config.runBiomes = false;
    config.runNoise = true;
    config.runSurface = false;
    config.runCarvers = false;
    config.runFeatures = false;
    config.runLighting = false;
    config.runSpawn = false;

    // Run noise phase
    ChunkGenerationRunner runner(generator, randomState, SEED, config);
    runner.generateChunk(chunk);

    // Count blocks
    int startX = chunkPos.getMinBlockX();
    int startZ = chunkPos.getMinBlockZ();

    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                try {
                    ::BlockState* block = chunk->getBlockState(startX + x, y, startZ + z);
                    std::string blockName = getBlockName(block);

                    if (blockName == "minecraft:air") stats.airCount++;
                    else if (blockName == "minecraft:stone") stats.stoneCount++;
                    else if (blockName == "minecraft:water") stats.waterCount++;
                    else if (blockName == "minecraft:lava") stats.lavaCount++;
                    else stats.otherCount++;
                } catch (...) {
                    // Uninitialized section - treat as air
                    stats.airCount++;
                }
            }
        }
    }

    // Compute content hash (same formula as Java)
    stats.contentHash = ((uint64_t)stats.airCount * 31) ^
                        ((uint64_t)stats.stoneCount * 37) ^
                        ((uint64_t)stats.waterCount * 41) ^
                        ((uint64_t)stats.lavaCount * 43) ^
                        ((uint64_t)chunkX * 53) ^
                        ((uint64_t)chunkZ * 59);

    // Cleanup chunk only - generator and randomState are reused
    delete chunk;
    delete blockRegistry;

    return stats;
}

int main(int argc, char* argv[]) {
    std::string outputPath = (argc > 1) ? argv[1] : "cpp_multi_chunk_output.txt";

    std::cout << "=== C++ Multi-Chunk Phase Noise Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Testing " << TEST_CHUNKS.size() << " chunks" << std::endl;
    std::cout << "Y range: " << MIN_Y << " to " << MAX_Y << std::endl;
    std::cout << std::endl;

    // Bootstrap registries
    std::cout << "Bootstrapping registries..." << std::endl;
    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);

    // Build router
    NoiseRouter* router = NoiseRouterData::overworld(false, false);

    // Create noise settings
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
        true,
        true,
        false
    );

    // Create RandomState ONCE and reuse for all chunks (like Java)
    std::cout << "Creating RandomState..." << std::endl;
    RandomState* randomState = RandomState::create(settings, SEED);

    // Create fluid picker
    FluidPicker* fluidPicker = new OverworldFluidPicker(63, -54,
        ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), ::minecraft::world::level::block::Blocks::LAVA->defaultBlockState());

    // Create chunk generator ONCE and reuse for all chunks (like Java)
    std::cout << "Creating NoiseBasedChunkGenerator..." << std::endl;
    Beardifier* beardifier = new Beardifier();
    NoiseBasedChunkGenerator* generator = new NoiseBasedChunkGenerator(
        settings,
        nullptr,
        nullptr,
        ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(),
        ::minecraft::world::level::block::Blocks::AIR->defaultBlockState(),
        fluidPicker,
        beardifier
    );

    std::cout << "Setup complete. Generating chunks..." << std::endl;
    std::cout << std::endl;

    // Open output file
    std::ofstream out(outputPath);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputPath << std::endl;
        return 1;
    }

    out << "# C++ Multi-Chunk Phase Noise Output" << std::endl;
    out << "# Seed: " << SEED << std::endl;
    out << "# Phase: NOISE ONLY" << std::endl;
    out << "# Format: chunkX,chunkZ,air,stone,water,lava,other,hash" << std::endl;
    out << std::endl;

    // Generate and analyze each chunk
    std::vector<ChunkStats> allStats;

    for (const auto& pos : TEST_CHUNKS) {
        int chunkX = pos.first;
        int chunkZ = pos.second;

        std::cout << "Generating chunk (" << chunkX << ", " << chunkZ << ")..." << std::flush;

        ChunkStats stats = generateAndAnalyzeChunk(generator, randomState, chunkX, chunkZ);
        allStats.push_back(stats);

        std::cout << " done. Air: " << stats.airCount
                  << ", Stone: " << stats.stoneCount
                  << ", Water: " << stats.waterCount
                  << ", Hash: " << std::hex << stats.contentHash << std::dec
                  << std::endl;

        // Write to file
        out << stats.chunkX << "," << stats.chunkZ << ","
            << stats.airCount << "," << stats.stoneCount << ","
            << stats.waterCount << "," << stats.lavaCount << ","
            << stats.otherCount << ","
            << std::hex << stats.contentHash << std::dec << std::endl;
    }

    out << std::endl;
    out << "# Summary:" << std::endl;
    out << "# Total chunks tested: " << allStats.size() << std::endl;

    // Print summary
    std::cout << std::endl;
    std::cout << "=== Summary ===" << std::endl;
    std::cout << std::setw(10) << "ChunkX"
              << std::setw(10) << "ChunkZ"
              << std::setw(10) << "Air"
              << std::setw(10) << "Stone"
              << std::setw(10) << "Water"
              << std::setw(10) << "Lava"
              << std::setw(18) << "Hash" << std::endl;
    std::cout << std::string(78, '-') << std::endl;

    for (const auto& stats : allStats) {
        std::cout << std::setw(10) << stats.chunkX
                  << std::setw(10) << stats.chunkZ
                  << std::setw(10) << stats.airCount
                  << std::setw(10) << stats.stoneCount
                  << std::setw(10) << stats.waterCount
                  << std::setw(10) << stats.lavaCount
                  << std::setw(18) << std::hex << stats.contentHash << std::dec
                  << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Output written to: " << outputPath << std::endl;
    std::cout << "Compare with Java MultiChunkPhaseNoiseTest output for parity check." << std::endl;

    // Cleanup
    delete generator;
    delete beardifier;
    delete fluidPicker;
    delete randomState;
    delete settings;
    delete router;

    return 0;
}
