/**
 * C++ Chunk Parity Test
 *
 * Generates chunks and outputs block data for parity testing with Java Minecraft.
 *
 * Usage:
 *   chunk_parity_test --single <chunkX> <chunkZ> [--output <file>]
 *       Generates a single chunk and outputs every block position with block name.
 *       Output includes summary with block counts sorted by frequency.
 *
 *   chunk_parity_test --radius <radius> [--center <x> <z>] [--output <file>]
 *       Generates chunks in a radius and outputs summary for each chunk.
 *       Each line: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...
 *       Blocks are sorted by count (most to least).
 *
 *   chunk_parity_test --bulk <gridSize> [--output <file>]
 *       Generates a gridSize x gridSize grid of chunks centered at origin.
 *
 * Test Parameters:
 *   Seed: 12345 (default, can be overridden with --seed)
 *   Y range: -64 to 320
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
#include <cstdlib>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/placement/PlacementModifiers.h"
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

// Default test parameters
static int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

struct ChunkSummary {
    int chunkX;
    int chunkZ;
    int64_t airBlocks;
    int64_t nonAirBlocks;
    std::map<std::string, int64_t> blockCounts;
};

struct TestConfig {
    enum Mode { SINGLE, RADIUS, BULK };
    Mode mode = SINGLE;
    int chunkX = 0;
    int chunkZ = 0;
    int radius = 0;
    int gridSize = 10;
    std::string outputPath;
    bool verbose = false;
    bool debugBiomeFilter = false;
};

// Forward declarations
class ChunkGenerator;

ChunkSummary calculateChunkSummary(minecraft::world::IChunk* chunk, int chunkX, int chunkZ) {
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

    return {chunkX, chunkZ, airCount, nonAirCount, blockCounts};
}

std::vector<std::pair<std::string, int64_t>> getSortedBlockCounts(
    const std::map<std::string, int64_t>& blockCounts) {
    std::vector<std::pair<std::string, int64_t>> sorted(blockCounts.begin(), blockCounts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    return sorted;
}

void writeSingleChunkOutput(minecraft::world::IChunk* chunk,
                             const ChunkSummary& summary,
                             std::ostream& out) {
    // Header
    out << "# C++ Single Chunk Parity Test Output\n";
    out << "# Seed: " << SEED << "\n";
    out << "# Chunk: (" << summary.chunkX << ", " << summary.chunkZ << ")\n";
    out << "# Format: x,y,z,block_name\n";
    out << "\n";

    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    // Write every block position
    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                std::string blockName = state ? state->getIdentifier() : "minecraft:air";

                out << x << "," << y << "," << z << "," << blockName << "\n";
            }
        }
    }

    // Summary at bottom
    out << "\n";
    out << "# ===========================================\n";
    out << "# SUMMARY\n";
    out << "# ===========================================\n";
    out << "# Total blocks: " << (summary.airBlocks + summary.nonAirBlocks) << "\n";
    out << "# Air blocks: " << summary.airBlocks << "\n";
    out << "# Non-air blocks: " << summary.nonAirBlocks << "\n";
    out << "#\n";
    out << "# Block counts (sorted by frequency):\n";

    auto sorted = getSortedBlockCounts(summary.blockCounts);
    for (const auto& entry : sorted) {
        out << "#   " << entry.first << ": " << entry.second << "\n";
    }
}

void writeBulkChunkLine(const ChunkSummary& summary, std::ostream& out) {
    // Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...
    out << summary.chunkX << "," << summary.chunkZ << ","
        << summary.airBlocks << "," << summary.nonAirBlocks;

    // Add block counts sorted by count (descending)
    auto sorted = getSortedBlockCounts(summary.blockCounts);
    for (const auto& entry : sorted) {
        out << "," << entry.first << ":" << entry.second;
    }
    out << "\n";
}

void printUsage(const char* programName) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << programName << " --single <chunkX> <chunkZ> [--output <file>] [--seed <seed>]\n";
    std::cerr << "      Generate single chunk with full block output.\n\n";
    std::cerr << "  " << programName << " --radius <radius> [--center <x> <z>] [--output <file>] [--seed <seed>]\n";
    std::cerr << "      Generate chunks in radius around center.\n\n";
    std::cerr << "  " << programName << " --bulk <gridSize> [--output <file>] [--seed <seed>]\n";
    std::cerr << "      Generate gridSize x gridSize grid centered at origin.\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --seed <seed>     Set world seed (default: 12345)\n";
    std::cerr << "  --output <file>   Output file path (default: stdout for single, auto-generated for bulk)\n";
    std::cerr << "  -v, --verbose     Verbose output\n";
}

TestConfig parseArgs(int argc, char* argv[]) {
    TestConfig config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--single") {
            config.mode = TestConfig::SINGLE;
            if (i + 2 < argc) {
                config.chunkX = std::atoi(argv[++i]);
                config.chunkZ = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --single requires chunkX and chunkZ\n";
                exit(1);
            }
        } else if (arg == "--radius") {
            config.mode = TestConfig::RADIUS;
            if (i + 1 < argc) {
                config.radius = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --radius requires a radius value\n";
                exit(1);
            }
        } else if (arg == "--bulk") {
            config.mode = TestConfig::BULK;
            if (i + 1 < argc) {
                config.gridSize = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --bulk requires a grid size\n";
                exit(1);
            }
        } else if (arg == "--center") {
            if (i + 2 < argc) {
                config.chunkX = std::atoi(argv[++i]);
                config.chunkZ = std::atoi(argv[++i]);
            } else {
                std::cerr << "Error: --center requires x and z coordinates\n";
                exit(1);
            }
        } else if (arg == "--output") {
            if (i + 1 < argc) {
                config.outputPath = argv[++i];
            } else {
                std::cerr << "Error: --output requires a file path\n";
                exit(1);
            }
        } else if (arg == "--seed") {
            if (i + 1 < argc) {
                SEED = std::atoll(argv[++i]);
            } else {
                std::cerr << "Error: --seed requires a seed value\n";
                exit(1);
            }
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--debug-biome-filter") {
            config.debugBiomeFilter = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            exit(1);
        }
    }

    // Set default output paths
    if (config.outputPath.empty()) {
        if (config.mode == TestConfig::SINGLE) {
            config.outputPath = "tests/output/cpp_chunk_" +
                std::to_string(config.chunkX) + "_" +
                std::to_string(config.chunkZ) + ".txt";
        } else if (config.mode == TestConfig::RADIUS) {
            config.outputPath = "tests/output/cpp_radius_" +
                std::to_string(config.radius) + "_at_" +
                std::to_string(config.chunkX) + "_" +
                std::to_string(config.chunkZ) + ".txt";
        } else {
            config.outputPath = "tests/output/cpp_bulk_" +
                std::to_string(config.gridSize) + "x" +
                std::to_string(config.gridSize) + ".txt";
        }
    }

    return config;
}

class ChunkGeneratorSetup {
public:
    minecraft::world::BlockRegistry* registry = nullptr;
    minecraft::levelgen::NoiseGeneratorSettings* settings = nullptr;
    minecraft::levelgen::RandomState* randomState = nullptr;
    minecraft::levelgen::RuleSource* surfaceRules = nullptr;
    minecraft::levelgen::FluidPicker* fluidPicker = nullptr;
    std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> biomeSource;
    minecraft::levelgen::NoiseBasedChunkGenerator* generator = nullptr;
    std::unique_ptr<minecraft::levelgen::ChunkGenerationRunner> runner;
    minecraft::BlockState* airBlock = nullptr;
    minecraft::BlockState* stoneBlock = nullptr;

    void initialize(bool verbose, bool debugBiomeFilter = false) {
        // Enable BiomeFilter debug if requested
        if (debugBiomeFilter) {
            minecraft::levelgen::placement::BiomeFilter::setDebugEnabled(true);
            if (verbose) std::cout << "BiomeFilter debug output enabled" << std::endl;
        }
        if (verbose) std::cout << "Getting block types..." << std::endl;
        airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        if (verbose) std::cout << "Creating block registry..." << std::endl;
        registry = new minecraft::world::BlockRegistry();
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

        if (verbose) std::cout << "Bootstrapping NoiseRegistry..." << std::endl;
        minecraft::levelgen::NoiseRegistry::bootstrap();

        if (verbose) std::cout << "Bootstrapping DensityFunctionRegistry for seed: " << SEED << std::endl;
        minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);

        if (verbose) std::cout << "Initializing SurfaceRuleData..." << std::endl;
        minecraft::levelgen::SurfaceRuleData::initialize();

        if (verbose) std::cout << "Building overworld NoiseRouter..." << std::endl;
        minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);

        if (verbose) std::cout << "Creating NoiseGeneratorSettings..." << std::endl;
        minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

        settings = new minecraft::levelgen::NoiseGeneratorSettings(
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

        if (verbose) std::cout << "Creating RandomState for seed: " << SEED << std::endl;
        randomState = minecraft::levelgen::RandomState::create(settings, SEED);

        if (verbose) std::cout << "Creating overworld surface rules..." << std::endl;
        surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

        if (verbose) std::cout << "Creating OverworldFluidPicker..." << std::endl;
        fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
            63, -54, minecraft::world::level::block::Blocks::WATER->defaultBlockState(), minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        if (verbose) std::cout << "Creating MultiNoiseBiomeSource..." << std::endl;
        biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

        if (verbose) std::cout << "Creating NoiseBasedChunkGenerator..." << std::endl;
        generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
            settings,
            randomState->surfaceSystem(),
            surfaceRules,
            stoneBlock,
            airBlock,
            fluidPicker,
            nullptr
        );

        generator->setBiomeSource(biomeSource.get());

        if (verbose) std::cout << "Creating ChunkGenerationRunner..." << std::endl;
        minecraft::levelgen::ChunkGenerationRunner::Config config =
            minecraft::levelgen::ChunkGenerationRunner::Config::terrainWithCaves();
        config.runCarvers = true;
        config.runSurface = true;
        config.runFeatures = true;  // Features phase enabled

        runner = std::make_unique<minecraft::levelgen::ChunkGenerationRunner>(
            generator, randomState, SEED, config
        );

        if (verbose) std::cout << "Setup complete." << std::endl;
    }

    minecraft::world::ProtoChunk* createChunk(int chunkX, int chunkZ) {
        minecraft::world::ChunkPos chunkPos(chunkX, chunkZ);
        return new minecraft::world::ProtoChunk(
            chunkPos,
            MIN_Y,
            HEIGHT,
            airBlock,
            stoneBlock,
            registry
        );
    }

    void generateChunk(minecraft::world::IChunk* chunk) {
        runner->generateChunk(chunk);
    }

    ~ChunkGeneratorSetup() {
        delete generator;
        delete randomState;
        delete settings;
        delete registry;
    }
};

int runSingleChunkTest(const TestConfig& config) {
    std::cout << "=== C++ Single Chunk Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << config.chunkX << ", " << config.chunkZ << ")" << std::endl;
    std::cout << "Y range: " << MIN_Y << " to " << MAX_Y << std::endl;
    std::cout << std::endl;

    try {
        ChunkGeneratorSetup setup;
        setup.initialize(config.verbose, config.debugBiomeFilter);

        std::cout << "Generating chunk (" << config.chunkX << ", " << config.chunkZ << ")..." << std::endl;
        minecraft::world::ProtoChunk* chunk = setup.createChunk(config.chunkX, config.chunkZ);
        setup.generateChunk(chunk);

        std::cout << "Chunk generation complete!" << std::endl;

        // Print BiomeFilter stats if debug enabled
        if (config.debugBiomeFilter) {
            minecraft::levelgen::placement::BiomeFilter::printDebugStats();
        }

        ChunkSummary summary = calculateChunkSummary(chunk, config.chunkX, config.chunkZ);

        std::cout << "Writing output to: " << config.outputPath << std::endl;
        std::ofstream outFile(config.outputPath);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open output file: " << config.outputPath << std::endl;
            return 1;
        }

        writeSingleChunkOutput(chunk, summary, outFile);
        outFile.close();

        std::cout << std::endl;
        std::cout << "Summary:" << std::endl;
        std::cout << "  Total blocks: " << (summary.airBlocks + summary.nonAirBlocks) << std::endl;
        std::cout << "  Air blocks: " << summary.airBlocks << std::endl;
        std::cout << "  Non-air blocks: " << summary.nonAirBlocks << std::endl;

        delete chunk;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int runRadiusTest(const TestConfig& config) {
    int totalChunks = (config.radius * 2 + 1) * (config.radius * 2 + 1);

    std::cout << "=== C++ Radius Chunk Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Center: (" << config.chunkX << ", " << config.chunkZ << ")" << std::endl;
    std::cout << "Radius: " << config.radius << std::endl;
    std::cout << "Total chunks: " << totalChunks << std::endl;
    std::cout << std::endl;

    try {
        ChunkGeneratorSetup setup;
        setup.initialize(config.verbose);

        std::ofstream outFile(config.outputPath);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open output file: " << config.outputPath << std::endl;
            return 1;
        }

        outFile << "# C++ Radius Chunk Parity Test Output\n";
        outFile << "# Seed: " << SEED << "\n";
        outFile << "# Center: (" << config.chunkX << ", " << config.chunkZ << ")\n";
        outFile << "# Radius: " << config.radius << "\n";
        outFile << "# Phases: BIOMES, NOISE, SURFACE, CARVERS\n";
        outFile << "# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...\n";
        outFile << "# Blocks sorted by count (most to least)\n";
        outFile << "\n";

        int completed = 0;
        auto startTime = std::chrono::steady_clock::now();

        // Iterate in same order as Java (X outer, Z inner, from -radius to +radius)
        for (int dx = -config.radius; dx <= config.radius; dx++) {
            for (int dz = -config.radius; dz <= config.radius; dz++) {
                int cx = config.chunkX + dx;
                int cz = config.chunkZ + dz;

                minecraft::world::ProtoChunk* chunk = setup.createChunk(cx, cz);
                setup.generateChunk(chunk);

                ChunkSummary summary = calculateChunkSummary(chunk, cx, cz);
                writeBulkChunkLine(summary, outFile);

                delete chunk;
                completed++;

                if (config.verbose && completed % 10 == 0) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - startTime).count();
                    double rate = completed / elapsed;
                    std::cout << "Progress: " << completed << "/" << totalChunks
                              << " (" << std::fixed << std::setprecision(1)
                              << (100.0 * completed / totalChunks) << "%)"
                              << " - " << std::setprecision(1) << rate << " chunks/sec"
                              << std::endl;
                }
            }
        }

        outFile.close();

        auto endTime = std::chrono::steady_clock::now();
        double totalTime = std::chrono::duration<double>(endTime - startTime).count();

        std::cout << std::endl;
        std::cout << "Generation complete!" << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(2) << totalTime << " seconds" << std::endl;
        std::cout << "Average: " << std::setprecision(2) << (totalTime * 1000.0 / totalChunks) << " ms per chunk" << std::endl;
        std::cout << "Output written to: " << config.outputPath << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int runBulkTest(const TestConfig& config) {
    int startX = -config.gridSize / 2;
    int startZ = -config.gridSize / 2;
    int totalChunks = config.gridSize * config.gridSize;

    std::cout << "=== C++ Bulk Chunk Parity Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Grid: " << config.gridSize << "x" << config.gridSize << std::endl;
    std::cout << "Range: (" << startX << "," << startZ << ") to ("
              << (startX + config.gridSize - 1) << "," << (startZ + config.gridSize - 1) << ")" << std::endl;
    std::cout << "Total chunks: " << totalChunks << std::endl;
    std::cout << std::endl;

    try {
        ChunkGeneratorSetup setup;
        setup.initialize(config.verbose);

        std::ofstream outFile(config.outputPath);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open output file: " << config.outputPath << std::endl;
            return 1;
        }

        outFile << "# C++ Bulk Chunk Parity Test Output\n";
        outFile << "# Seed: " << SEED << "\n";
        outFile << "# Grid: " << config.gridSize << "x" << config.gridSize << "\n";
        outFile << "# Phases: BIOMES, NOISE, SURFACE, CARVERS\n";
        outFile << "# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,block2:count2,...\n";
        outFile << "# Blocks sorted by count (most to least)\n";
        outFile << "\n";

        int completed = 0;
        auto startTime = std::chrono::steady_clock::now();

        // Iterate Z outer, X inner (same as CppBulkTest.cpp)
        for (int cz = startZ; cz < startZ + config.gridSize; cz++) {
            for (int cx = startX; cx < startX + config.gridSize; cx++) {
                minecraft::world::ProtoChunk* chunk = setup.createChunk(cx, cz);
                setup.generateChunk(chunk);

                ChunkSummary summary = calculateChunkSummary(chunk, cx, cz);
                writeBulkChunkLine(summary, outFile);

                delete chunk;
                completed++;

                if (completed % 100 == 0) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - startTime).count();
                    double rate = completed / elapsed;
                    double remaining = (totalChunks - completed) / rate;
                    std::cout << "Progress: " << completed << "/" << totalChunks
                              << " (" << std::fixed << std::setprecision(1)
                              << (100.0 * completed / totalChunks) << "%)"
                              << " - " << std::setprecision(1) << rate << " chunks/sec"
                              << " - ETA: " << std::setprecision(0) << remaining << " sec"
                              << std::endl;
                }
            }
        }

        outFile.close();

        auto endTime = std::chrono::steady_clock::now();
        double totalTime = std::chrono::duration<double>(endTime - startTime).count();

        std::cout << std::endl;
        std::cout << "Generation complete!" << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(1) << totalTime << " seconds" << std::endl;
        std::cout << "Average: " << std::setprecision(2) << (totalTime * 1000.0 / totalChunks) << " ms per chunk" << std::endl;
        std::cout << "Output written to: " << config.outputPath << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int main(int argc, char* argv[]) {
    // Initialize block registry
    minecraft::world::level::block::Blocks::bootstrap();

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    TestConfig config = parseArgs(argc, argv);

    switch (config.mode) {
        case TestConfig::SINGLE:
            return runSingleChunkTest(config);
        case TestConfig::RADIUS:
            return runRadiusTest(config);
        case TestConfig::BULK:
            return runBulkTest(config);
        default:
            std::cerr << "Unknown mode\n";
            return 1;
    }
}
