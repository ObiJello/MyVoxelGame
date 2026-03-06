/**
 * CppChunkGeneratorTest - Tests C++ chunk generation with full async pipeline
 *
 * THIS TEST MIRRORS MinecraftAsyncChunkTest.java EXACTLY:
 *   - Same command line arguments (--radius, --center, --phases, etc.)
 *   - Same output format
 *   - Same phase configurations
 *   - Same step-by-step flow
 *
 * Pipeline:
 *   ServerChunkCache -> ChunkMap -> DistanceManager -> ChunkGenerationTask
 *                    -> ChunkTaskDispatcher -> Worker Threads
 *
 * Phases: 0=EMPTY, 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE,
 *         5=SURFACE, 6=CARVERS, 7=FEATURES, 8=INITIALIZE_LIGHT, 9=LIGHT, 10=SPAWN, 11=FULL
 *
 * Usage:
 *   chunk_generator_test --radius 5 [--center 0 0] [--phases all] [--output file] [--seed 12345]
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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <future>
#include <fstream>

// Feature block-change trace
#include "levelgen/feature/Feature.h"

// Server-level includes (the async pipeline)
#include "server/level/ServerChunkCache.h"
#include "server/level/ChunkMap.h"
#include "server/level/DistanceManager.h"
#include "server/level/ChunkHolder.h"
#include "server/level/ChunkGenerationTask.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkPyramid.h"
#include "util/Profiler.h"

// World generation includes
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
#include "levelgen/placement/PlacedFeature.h"

// World includes
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "core/BlockPos.h"

// Default test parameters
static int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;
static const int MAX_Y = MIN_Y + HEIGHT;

// Phase configuration (set after parsing args)
static const minecraft::world::chunk::status::ChunkStatus* targetStatus = nullptr;
static bool generateStructures = true;
static std::string phasesDescription = "all (0-11)";

/**
 * Background thread pool executor - simulates Minecraft's Util.backgroundExecutor()
 * Reference: Minecraft uses ForkJoinPool.commonPool() for background work
 */
class BackgroundExecutor {
public:
    using Task = std::function<void()>;

    BackgroundExecutor(size_t numThreads = std::thread::hardware_concurrency())
        : m_running(true)
    {
        for (size_t i = 0; i < numThreads; ++i) {
            m_workers.emplace_back([this]() { workerLoop(); });
        }
    }

    ~BackgroundExecutor() { shutdown(); }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
        }
        m_cv.notify_all();
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void submit(Task task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push(std::move(task));
        }
        m_cv.notify_one();
    }

    std::function<void(std::function<void()>)> getExecutor() {
        return [this](std::function<void()> task) {
            this->submit(std::move(task));
        };
    }

private:
    void workerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return !m_running || !m_tasks.empty();
                });

                if (!m_running && m_tasks.empty()) return;

                if (!m_tasks.empty()) {
                    task = std::move(m_tasks.front());
                    m_tasks.pop();
                }
            }

            if (task) {
                try {
                    task();
                } catch (const std::exception& e) {
                    std::cerr << "Background task exception: " << e.what() << std::endl;
                }
            }
        }
    }

    std::vector<std::thread> m_workers;
    std::queue<Task> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_running;
};

/**
 * Main thread executor for main thread tasks
 */
class MainThreadExecutor {
public:
    using Task = std::function<void()>;

    void submit(Task task) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push(std::move(task));
    }

    void runPendingTasks() {
        std::queue<Task> tasksToRun;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::swap(tasksToRun, m_tasks);
        }
        while (!tasksToRun.empty()) {
            tasksToRun.front()();
            tasksToRun.pop();
        }
    }

    bool hasPendingTasks() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_tasks.empty();
    }

    std::function<void(std::function<void()>)> getExecutor() {
        return [this](std::function<void()> task) {
            this->submit(std::move(task));
        };
    }

private:
    std::queue<Task> m_tasks;
    std::mutex m_mutex;
};

/**
 * World generation setup - creates all required components
 */
class WorldGenSetup {
public:
    minecraft::world::BlockRegistry* registry = nullptr;
    minecraft::levelgen::NoiseGeneratorSettings* settings = nullptr;
    minecraft::levelgen::RandomState* randomState = nullptr;
    minecraft::levelgen::RuleSource* surfaceRules = nullptr;
    minecraft::levelgen::FluidPicker* fluidPicker = nullptr;
    std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> biomeSource;
    minecraft::levelgen::NoiseBasedChunkGenerator* generator = nullptr;
    minecraft::BlockState* airBlock = nullptr;
    minecraft::BlockState* stoneBlock = nullptr;

    void initialize(bool verbose) {
        if (verbose) std::cout << "  Getting block types..." << std::endl;
        airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();

        if (verbose) std::cout << "  Creating block registry..." << std::endl;
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

        if (verbose) std::cout << "  Bootstrapping NoiseRegistry..." << std::endl;
        minecraft::levelgen::NoiseRegistry::bootstrap();

        if (verbose) std::cout << "  Bootstrapping DensityFunctionRegistry..." << std::endl;
        minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);

        if (verbose) std::cout << "  Initializing SurfaceRuleData..." << std::endl;
        minecraft::levelgen::SurfaceRuleData::initialize();

        if (verbose) std::cout << "  Building overworld NoiseRouter..." << std::endl;
        minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);

        if (verbose) std::cout << "  Creating NoiseGeneratorSettings..." << std::endl;
        minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

        settings = new minecraft::levelgen::NoiseGeneratorSettings(
            noiseSettings,
            minecraft::world::level::block::Blocks::STONE->defaultBlockState(),
            minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            *router, nullptr, {}, 63, false, true, true, false
        );

        if (verbose) std::cout << "  Creating RandomState..." << std::endl;
        randomState = minecraft::levelgen::RandomState::create(settings, SEED);

        if (verbose) std::cout << "  Creating surface rules..." << std::endl;
        surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

        if (verbose) std::cout << "  Creating FluidPicker..." << std::endl;
        fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
            63, -54,
            minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
            minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
        );

        if (verbose) std::cout << "  Creating MultiNoiseBiomeSource..." << std::endl;
        biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

        if (verbose) std::cout << "  Creating NoiseBasedChunkGenerator..." << std::endl;
        generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
            settings, randomState->surfaceSystem(), surfaceRules,
            stoneBlock, airBlock, fluidPicker, nullptr
        );
        generator->setBiomeSource(biomeSource.get());

        if (verbose) std::cout << "  Setup complete." << std::endl;
    }

    ~WorldGenSetup() {
        delete generator;
        delete randomState;
        delete settings;
        delete registry;
    }
};

void printUsage(const char* programName) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << programName << " --radius <radius> [options]\n";
    std::cerr << "  " << programName << " --single <chunkX> <chunkZ> [options]   (detailed per-block output)\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --radius <n>       Radius of chunks to generate\n";
    std::cerr << "  --single <x> <z>   Generate single chunk with detailed per-block output\n";
    std::cerr << "  --center <x> <z>   Center chunk position (default: 0 0)\n";
    std::cerr << "  --output <file>    Output file path\n";
    std::cerr << "  --feature-log <f>  Log each feature placement to file\n";
    std::cerr << "  --trace-modifiers  Enable detailed per-modifier position tracing\n";
    std::cerr << "  --seed <seed>      World seed (default: 12345)\n";
    std::cerr << "  --phases <spec>    Phase configuration (default: all)\n";
    std::cerr << "  --quiet            Suppress progress output\n";
    std::cerr << "\n";
    std::cerr << "Phase specifications:\n";
    std::cerr << "  3-6    BIOMES->CARVERS, no structures\n";
    std::cerr << "  0-6    EMPTY->CARVERS, with structures\n";
    std::cerr << "  3-7    BIOMES->FEATURES, no structures\n";
    std::cerr << "  0-7    EMPTY->FEATURES, with structures\n";
    std::cerr << "  all    Complete generation (0-11, EMPTY->FULL)\n";
    std::cerr << "\n";
    std::cerr << "Phases: 0=EMPTY, 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE,\n";
    std::cerr << "        5=SURFACE, 6=CARVERS, 7=FEATURES, 8=INITIALIZE_LIGHT, 9=LIGHT, 10=SPAWN, 11=FULL\n";
}

void configurePhases(const std::string& phases) {
    // Reference: MinecraftAsyncChunkTest.java phase configuration
    // Phases: 0=EMPTY, 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE,
    //         5=SURFACE, 6=CARVERS, 7=FEATURES, 8=INITIALIZE_LIGHT, 9=LIGHT, 10=SPAWN, 11=FULL

    using ChunkStatus = minecraft::world::chunk::status::ChunkStatus;

    if (phases == "3-6") {
        targetStatus = &ChunkStatus::CARVERS;
        generateStructures = false;
        phasesDescription = "3-6 (BIOMES->CARVERS, no structures)";
    } else if (phases == "0-6") {
        targetStatus = &ChunkStatus::CARVERS;
        generateStructures = true;
        phasesDescription = "0-6 (EMPTY->CARVERS, with structures)";
    } else if (phases == "3-7") {
        targetStatus = &ChunkStatus::FEATURES;
        generateStructures = false;
        phasesDescription = "3-7 (BIOMES->FEATURES, no structures)";
    } else if (phases == "0-7") {
        targetStatus = &ChunkStatus::FEATURES;
        generateStructures = true;
        phasesDescription = "0-7 (EMPTY->FEATURES, with structures)";
    } else {
        // "all" or default
        targetStatus = &ChunkStatus::FULL;
        generateStructures = true;
        phasesDescription = "all (0-11, complete generation)";
    }
}

void writeChunkData(std::ostream& out, world::IChunk* chunk, int chunkX, int chunkZ) {
    // Reference: MinecraftAsyncChunkTest.java writeChunkData()
    // Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,...

    std::map<std::string, int64_t> blockCounts;
    int64_t airCount = 0;
    int64_t nonAirCount = 0;

    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                std::string blockName = state ? state->getIdentifier() : "minecraft:air";

                if (blockName == "minecraft:air" || blockName == "minecraft:cave_air" || blockName == "minecraft:void_air") {
                    airCount++;
                } else {
                    nonAirCount++;
                    blockCounts[blockName]++;
                }
            }
        }
    }

    // Build output line
    out << chunkX << "," << chunkZ << "," << airCount << "," << nonAirCount;

    // Sort by count descending
    std::vector<std::pair<std::string, int64_t>> sorted(blockCounts.begin(), blockCounts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& entry : sorted) {
        out << "," << entry.first << ":" << entry.second;
    }
    out << "\n";
}

void writeSingleChunkDetailedOutput(std::ostream& out, world::IChunk* chunk, int chunkX, int chunkZ) {
    // Detailed per-block output for single chunk mode
    // Format: x,y,z,block_name (same as chunk_parity_test --single)

    std::map<std::string, int64_t> blockCounts;
    int64_t airCount = 0;
    int64_t nonAirCount = 0;

    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    // Header
    out << "# C++ Async Single Chunk Parity Test Output\n";
    out << "# USES: ServerChunkCache + ChunkMap Pipeline\n";
    out << "# Seed: " << SEED << "\n";
    out << "# Chunk: (" << chunkX << ", " << chunkZ << ")\n";
    out << "# Phases: " << phasesDescription << "\n";
    out << "# Target: " << targetStatus->getName() << "\n";
    out << "# Format: x,y,z,block_name\n";
    out << "\n";

    // Write every block position
    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                std::string blockName = state ? state->getIdentifier() : "minecraft:air";

                out << x << "," << y << "," << z << "," << blockName << "\n";

                blockCounts[blockName]++;
                if (blockName == "minecraft:air" || blockName == "minecraft:cave_air" || blockName == "minecraft:void_air") {
                    airCount++;
                } else {
                    nonAirCount++;
                }
            }
        }
    }

    // Summary at bottom
    out << "\n";
    out << "# ===========================================\n";
    out << "# SUMMARY\n";
    out << "# ===========================================\n";
    out << "# Total blocks: " << (airCount + nonAirCount) << "\n";
    out << "# Air blocks: " << airCount << "\n";
    out << "# Non-air blocks: " << nonAirCount << "\n";
    out << "#\n";
    out << "# Block counts (sorted by frequency):\n";

    std::vector<std::pair<std::string, int64_t>> sorted(blockCounts.begin(), blockCounts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& entry : sorted) {
        out << "#   " << entry.first << ": " << entry.second << "\n";
    }
}

int main(int argc, char* argv[]) {
    // Parse arguments
    int radius = -1;  // -1 means not set
    int centerX = 0;
    int centerZ = 0;
    bool singleMode = false;
    int singleChunkX = 0;
    int singleChunkZ = 0;
    std::string outputPath;
    std::string featureLogPath;  // Path for feature log output
    bool verbose = true;
    std::string phases = "all";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--radius" && i + 1 < argc) {
            radius = std::atoi(argv[++i]);
        } else if (arg == "--single" && i + 2 < argc) {
            singleMode = true;
            singleChunkX = std::atoi(argv[++i]);
            singleChunkZ = std::atoi(argv[++i]);
        } else if (arg == "--center" && i + 2 < argc) {
            centerX = std::atoi(argv[++i]);
            centerZ = std::atoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--feature-log" && i + 1 < argc) {
            featureLogPath = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            SEED = std::atoll(argv[++i]);
        } else if (arg == "--phases" && i + 1 < argc) {
            phases = argv[++i];
        } else if (arg == "--quiet") {
            verbose = false;
        } else if (arg == "--trace-modifiers") {
            // Enable detailed per-modifier tracing
            minecraft::levelgen::placement::PlacedFeature::setModifierTracingEnabled(true);
        } else if (arg == "--block-trace" && i + 1 < argc) {
            // Enable block-change tracing to file
            static std::ofstream blockTraceFile;
            blockTraceFile.open(argv[++i]);
            if (blockTraceFile.is_open()) {
                minecraft::levelgen::feature::BlockChangeTrace::enabled = true;
                minecraft::levelgen::feature::BlockChangeTrace::stream = &blockTraceFile;
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Validate arguments
    if (!singleMode && radius < 0) {
        std::cerr << "Error: Must specify either --radius or --single\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // Set default output path
    if (outputPath.empty()) {
        if (singleMode) {
            outputPath = "tests/output/cpp_single_" + std::to_string(singleChunkX) + "_" + std::to_string(singleChunkZ) + ".txt";
        } else {
            outputPath = "tests/output/cpp_chunk_generator.txt";
        }
    }

    int totalChunks = singleMode ? 1 : (radius * 2 + 1) * (radius * 2 + 1);

    std::cout << "=== C++ Chunk Generator Test ===" << std::endl;
    std::cout << ">>> USES FULL ServerChunkCache + ChunkMap Pipeline <<<" << std::endl;
    std::cout << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    if (singleMode) {
        std::cout << "Mode: SINGLE CHUNK (detailed per-block output)" << std::endl;
        std::cout << "Chunk: (" << singleChunkX << ", " << singleChunkZ << ")" << std::endl;
    } else {
        std::cout << "Center: (" << centerX << ", " << centerZ << ")" << std::endl;
        std::cout << "Radius: " << radius << std::endl;
    }
    std::cout << "Total chunks: " << totalChunks << std::endl;
    std::cout << "Phases: " << phases << std::endl;
    std::cout << std::endl;

    // Setup feature logging if requested
    std::ofstream featureLogFile;
    if (!featureLogPath.empty()) {
        featureLogFile.open(featureLogPath);
        if (featureLogFile.is_open()) {
            std::cout << "Feature logging enabled: " << featureLogPath << std::endl;
            minecraft::levelgen::placement::PlacedFeature::setLoggingEnabled(true);
            minecraft::levelgen::placement::PlacedFeature::setLogStream(&featureLogFile);
        } else {
            std::cerr << "Warning: Could not open feature log file: " << featureLogPath << std::endl;
        }
    }

    try {
        // ========== Step 1: Bootstrap ==========
        std::cout << "Step 1: Bootstrapping..." << std::endl;
        minecraft::world::level::block::Blocks::bootstrap();

        // Configure phases AFTER bootstrap (ChunkStatus requires initialization)
        configurePhases(phases);
        std::cout << "  Phase config: " << phasesDescription << std::endl;
        std::cout << "  Target status: " << targetStatus->getName() << std::endl;
        std::cout << "  Generate structures: " << (generateStructures ? "true" : "false") << std::endl;
        std::cout << "  Bootstrap complete." << std::endl;

        // ========== Step 2: Setup world generation ==========
        std::cout << "Step 2: Setting up world generation..." << std::endl;
        WorldGenSetup worldGen;
        worldGen.initialize(verbose);

        // ========== Step 3: Create executors ==========
        std::cout << "Step 3: Creating executors..." << std::endl;
        BackgroundExecutor backgroundExecutor;
        MainThreadExecutor mainThreadExecutor;
        std::cout << "  Background threads: " << std::thread::hardware_concurrency() << std::endl;

        // ========== Step 4: Create ServerChunkCache ==========
        std::cout << "Step 4: Creating ServerChunkCache..." << std::endl;
        minecraft::server::level::ServerChunkCache chunkCache(
            worldGen.generator,
            worldGen.randomState,
            SEED,
            backgroundExecutor.getExecutor(),
            mainThreadExecutor.getExecutor(),
            worldGen.registry,
            worldGen.airBlock,
            worldGen.stoneBlock,
            MIN_Y,
            HEIGHT
        );

        chunkCache.setTaskPoller([&mainThreadExecutor]() {
            if (mainThreadExecutor.hasPendingTasks()) {
                mainThreadExecutor.runPendingTasks();
            }
        });
        std::cout << "  ServerChunkCache created." << std::endl;

        // ========== Step 5: Request chunks ==========
        std::cout << std::endl;
        std::cout << "Step 5: Requesting " << totalChunks << " chunks through ServerChunkCache.getChunk()..." << std::endl;
        std::cout << "  This goes through: DistanceManager -> ChunkMap -> ChunkTaskDispatcher -> etc." << std::endl;
        std::cout << std::endl;

        std::map<std::pair<int,int>, world::IChunk*> generatedChunks;
        auto startTime = std::chrono::steady_clock::now();
        int completed = 0;
        int lastReported = 0;

        if (singleMode) {
            // Single chunk mode
            world::IChunk* chunk = chunkCache.getChunk(singleChunkX, singleChunkZ, *targetStatus, true);
            if (chunk != nullptr) {
                generatedChunks[{singleChunkX, singleChunkZ}] = chunk;
            }
            completed = 1;
        } else {
            // Radius mode
            for (int z = -radius; z <= radius; z++) {
                for (int x = -radius; x <= radius; x++) {
                    int cx = centerX + x;
                    int cz = centerZ + z;

                    // THIS IS THE ACTUAL MINECRAFT CHUNK LOADING FLOW!
                    // ServerChunkCache.getChunk() goes through:
                    // 1. Cache check
                    // 2. getChunkFutureMainThread() -> adds ticket
                    // 3. runDistanceManagerUpdates()
                    // 4. ChunkHolder.scheduleChunkGenerationTask()
                    // 5. ChunkMap.scheduleGenerationTask()
                    // 6. ChunkTaskDispatcher.submit() -> ConsecutiveExecutor
                    // 7. ChunkGenerationTask runs through all statuses
                    // 8. managedBlock() pumps tasks until complete
                    world::IChunk* chunk = chunkCache.getChunk(cx, cz, *targetStatus, true);

                    if (chunk != nullptr) {
                        generatedChunks[{cx, cz}] = chunk;
                    }

                    completed++;

                    // Progress reporting
                    if (verbose && completed / 10 != lastReported / 10) {
                        lastReported = completed;
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - startTime).count();
                        double rate = elapsed > 0 ? completed / elapsed : 0;
                        std::cout << "Progress: " << completed << "/" << totalChunks
                                  << " - " << std::fixed << std::setprecision(1) << rate << " chunks/sec" << std::endl;
                    }
                }
            }
        }

        auto endTime = std::chrono::steady_clock::now();
        double totalTime = std::chrono::duration<double>(endTime - startTime).count();
        double rate = totalChunks / totalTime;

        std::cout << std::endl;
        std::cout << "All chunks generated!" << std::endl;

        // ========== Step 6: Write output ==========
        std::cout << "Step 6: Writing output..." << std::endl;
        std::ofstream outFile(outputPath);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open output file: " << outputPath << std::endl;
            return 1;
        }

        if (singleMode) {
            // Detailed per-block output for single chunk
            auto& entry = *generatedChunks.begin();
            writeSingleChunkDetailedOutput(outFile, entry.second, entry.first.first, entry.first.second);
        } else {
            // Summary output for radius mode
            outFile << "# C++ Chunk Generator Test Output\n";
            outFile << "# USES: ServerChunkCache + ChunkMap Pipeline\n";
            outFile << "# Pipeline: DistanceManager -> ChunkMap -> ChunkTaskDispatcher -> ConsecutiveExecutor\n";
            outFile << "# Seed: " << SEED << "\n";
            outFile << "# Center: (" << centerX << ", " << centerZ << ")\n";
            outFile << "# Radius: " << radius << "\n";
            outFile << "# Phases: " << phasesDescription << "\n";
            outFile << "# Target: " << targetStatus->getName() << "\n";
            outFile << "# Structures: " << (generateStructures ? "true" : "false") << "\n";
            outFile << "# Format: chunkX,chunkZ,airBlocks,nonAirBlocks,block1:count1,...\n";
            outFile << "\n";

            for (const auto& entry : generatedChunks) {
                writeChunkData(outFile, entry.second, entry.first.first, entry.first.second);
            }
        }

        outFile.close();

        // ========== Step 7: Print results ==========
        std::cout << std::endl;
        std::cout << "=== C++ Chunk Generator Test Complete ===" << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(2) << totalTime << " seconds" << std::endl;
        std::cout << "Rate: " << std::fixed << std::setprecision(1) << rate << " chunks/sec" << std::endl;
        std::cout << "Chunks generated: " << generatedChunks.size() << std::endl;
        std::cout << "Output: " << outputPath << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
