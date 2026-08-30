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
#include <optional>
#include <sys/resource.h>

// Feature block-change trace
#include "levelgen/feature/BlockChangeTrace.h"

// Structure placement layer (B1)
#include "levelgen/structure/ChunkGeneratorStructureState.h"

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
#include "levelgen/WorldGenTweaks.h"
#include "external/json.hpp"
#include "levelgen/FlatLevelSource.h"
#include "world/biome/FixedBiomeSource.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Heightmap.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/placement/PlacedFeature.h"

// World includes
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/TheEndBiomeSource.h"
#include "core/BlockPos.h"

// Default test parameters
static int64_t SEED = 12345L;
// Level-derived per --dimension (C1): overworld -64/384, nether/end 0/256.
// MAX_Y stays EXCLUSIVE, mirroring the Java harness.
static int MIN_Y = -64;
static int HEIGHT = 384;
static int MAX_Y = MIN_Y + HEIGHT;
static std::string g_dimension = "overworld";  // --dimension overworld|nether|end
// World type (MC WorldPresets, overworld only): default|flat|large_biomes|
// amplified|single_biome_surface, plus the flat/single-biome customization.
static std::string g_worldType = "default";
static std::string g_flatPreset;                        // "" = MC default flat settings
static std::string g_flatLayers;                        // custom "<layers>;<biome>"
static std::string g_singleBiome = "minecraft:plains";

// Phase configuration (set after parsing args)
static const minecraft::world::chunk::status::ChunkStatus* targetStatus = nullptr;
static bool generateStructures = true;
static bool partialStructuresAck = false;  // --partial-structures
static bool dumpBlockEntities = false;     // --dump-block-entities (E lines)
static std::string phasesDescription = "all (0-11)";

static std::optional<minecraft::core::BlockPos> getOverworldRespawnPos(
    ::world::IChunk* chunk,
    int worldX,
    int worldZ
) {
    if (chunk == nullptr) {
        return std::nullopt;
    }

    const int localX = worldX & 15;
    const int localZ = worldZ & 15;
    const int minY = chunk->getMinY();
    const int topY = chunk->getHeight(static_cast<int>(minecraft::levelgen::Heightmap::Types::MOTION_BLOCKING), localX, localZ);
    if (topY < minY) {
        return std::nullopt;
    }

    const int surfaceY = chunk->getHeight(static_cast<int>(minecraft::levelgen::Heightmap::Types::WORLD_SURFACE), localX, localZ);
    const int oceanFloorY = chunk->getHeight(static_cast<int>(minecraft::levelgen::Heightmap::Types::OCEAN_FLOOR), localX, localZ);
    if (surfaceY <= topY && surfaceY > oceanFloorY) {
        return std::nullopt;
    }

    minecraft::levelgen::blockpredicates::IChunkWorldGenLevel level(chunk);
    minecraft::core::BlockPos::MutableBlockPos pos;
    for (int y = topY + 1; y >= minY; --y) {
        minecraft::BlockState* blockState = chunk->getBlockState(localX, y, localZ);
        if (blockState == nullptr) {
            continue;
        }
        if (blockState->hasAnyFluid()) {
            break;
        }
        pos.set(worldX, y, worldZ);
        if (blockState->isFaceSturdy(level, pos, minecraft::core::Direction::UP)) {
            return minecraft::core::BlockPos(worldX, y + 1, worldZ);
        }
    }

    return std::nullopt;
}

static std::optional<minecraft::core::BlockPos> getSpawnPosInChunk(
    minecraft::server::level::ServerChunkCache& chunkCache,
    const std::function<bool()>& pumpMainThreadTasks,
    int chunkX,
    int chunkZ
) {
    auto loadFuture = chunkCache.addTicketAndLoadWithRadius(
        minecraft::server::level::TicketType::SPAWN_SEARCH,
        minecraft::world::ChunkPos(chunkX, chunkZ),
        0
    );
    while (!loadFuture->isDone()) {
        bool didWork = chunkCache.runDistanceManagerUpdates();
        didWork = pumpMainThreadTasks() || didWork;
        if (!didWork) {
            std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    auto loadResult = loadFuture->join();
    if (!loadResult || !loadResult->isSuccess()) {
        return std::nullopt;
    }

    ::world::IChunk* chunk = chunkCache.getChunkNow(chunkX, chunkZ);
    if (chunk == nullptr) {
        return std::nullopt;
    }

    minecraft::world::ChunkPos chunkPos(chunkX, chunkZ);
    for (int worldX = chunkPos.getMinBlockX(); worldX <= chunkPos.getMaxBlockX(); ++worldX) {
        for (int worldZ = chunkPos.getMinBlockZ(); worldZ <= chunkPos.getMaxBlockZ(); ++worldZ) {
            std::optional<minecraft::core::BlockPos> spawnPos = getOverworldRespawnPos(chunk, worldX, worldZ);
            if (spawnPos.has_value()) {
                return spawnPos;
            }
        }
    }

    return std::nullopt;
}

static void prepareInitialSpawn(
    minecraft::server::level::ServerChunkCache& chunkCache,
    minecraft::levelgen::RandomState* randomState,
    const std::function<bool()>& pumpMainThreadTasks,
    bool verbose
) {
    if (randomState == nullptr || randomState->sampler() == nullptr) {
        return;
    }

    minecraft::core::BlockPos spawnSuggestion = randomState->sampler()->findSpawnPosition();
    minecraft::world::ChunkPos spawnChunk(spawnSuggestion.getX() >> 4, spawnSuggestion.getZ() >> 4);

    if (verbose) {
        std::cout << "  Initial spawn suggestion: (" << spawnSuggestion.getX() << ", "
                  << spawnSuggestion.getY() << ", " << spawnSuggestion.getZ() << ")" << std::endl;
        std::cout << "  Spawn search chunk: (" << spawnChunk.x() << ", " << spawnChunk.z() << ")" << std::endl;
    }

    int xChunkOffset = 0;
    int zChunkOffset = 0;
    int dXChunk = 0;
    int dZChunk = -1;

    for (int i = 0; i < 11 * 11; ++i) {
        if (xChunkOffset >= -5 && xChunkOffset <= 5 && zChunkOffset >= -5 && zChunkOffset <= 5) {
            std::optional<minecraft::core::BlockPos> spawnPos = getSpawnPosInChunk(
                chunkCache,
                pumpMainThreadTasks,
                spawnChunk.x() + xChunkOffset,
                spawnChunk.z() + zChunkOffset
            );
            if (spawnPos.has_value()) {
                if (verbose) {
                    std::cout << "  Spawn found in chunk (" << (spawnChunk.x() + xChunkOffset) << ", "
                              << (spawnChunk.z() + zChunkOffset) << ") at "
                              << spawnPos->getX() << ", " << spawnPos->getY() << ", " << spawnPos->getZ()
                              << std::endl;
                }
                break;
            }
        }

        if (xChunkOffset == zChunkOffset ||
            (xChunkOffset < 0 && xChunkOffset == -zChunkOffset) ||
            (xChunkOffset > 0 && xChunkOffset == 1 - zChunkOffset)) {
            int oldDx = dXChunk;
            dXChunk = -dZChunk;
            dZChunk = oldDx;
        }

        xChunkOffset += dXChunk;
        zChunkOffset += dZChunk;
    }
}

/**
 * Background thread pool executor - simulates Minecraft's Util.backgroundExecutor()
 * Reference: Minecraft uses ForkJoinPool.commonPool() for background work
 */
class BackgroundExecutor {
public:
    using Task = std::function<void()>;

    // MC_BG_THREADS overrides the worker count (harness-only; output is
    // thread-count independent). Needed for ASan runs, whose serialized
    // allocator livelocks under hardware_concurrency() workers.
    static size_t defaultThreadCount() {
        if (const char* env = std::getenv("MC_BG_THREADS")) {
            long n = std::atol(env);
            if (n > 0) {
                return static_cast<size_t>(n);
            }
        }
        return std::thread::hardware_concurrency();
    }

    BackgroundExecutor(size_t numThreads = defaultThreadCount())
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
    std::unique_ptr<minecraft::world::biome::BiomeSource> biomeSource;
    std::vector<minecraft::world::biome::Climate::ParameterPoint> spawnTargetValues;
    std::vector<minecraft::levelgen::ClimateParameterPoint*> spawnTargetPointers;
    // NoiseBasedChunkGenerator for noise world types, FlatLevelSource for flat.
    minecraft::levelgen::ChunkGenerator* generator = nullptr;
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

        // ---- Dimension seam (C2): everything below differs per dimension.
        // Reference: the noise_settings JSONs (overworld/nether/end) +
        // NoiseRouterData / SurfaceRuleData / biome-source factories.
        minecraft::levelgen::NoiseRouter* router = nullptr;
        minecraft::levelgen::NoiseSettings noiseSettings =
            minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
        if (g_dimension == "nether") {
            if (verbose) std::cout << "  Building nether NoiseRouter..." << std::endl;
            router = minecraft::levelgen::NoiseRouterData::nether();
            biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createNether();
            noiseSettings = minecraft::levelgen::NoiseSettings::NETHER_NOISE_SETTINGS;
        } else if (g_dimension == "end") {
            if (verbose) std::cout << "  Building end NoiseRouter..." << std::endl;
            router = minecraft::levelgen::NoiseRouterData::end();
            biomeSource = std::make_unique<minecraft::world::biome::TheEndBiomeSource>(SEED);
            noiseSettings = minecraft::levelgen::NoiseSettings::END_NOISE_SETTINGS;
        } else {
            // Reference: NoiseGeneratorSettings.overworld(ctx, amplified,
            // large) - large_biomes/amplified differ ONLY in the router.
            const bool largeBiomes = (g_worldType == "large_biomes");
            const bool amplified = (g_worldType == "amplified");
            if (verbose) std::cout << "  Building overworld NoiseRouter (large="
                                   << largeBiomes << " amplified=" << amplified
                                   << ")..." << std::endl;
            router = minecraft::levelgen::NoiseRouterData::overworld(largeBiomes, amplified);
            if (g_worldType == "single_biome_surface") {
                // Reference: WorldPresets SINGLE_BIOME_SURFACE.
                biomeSource = std::make_unique<minecraft::world::biome::FixedBiomeSource>(
                    g_singleBiome);
            } else if (g_worldType != "flat") {
                biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();
            }
            // flat: the FlatLevelSource owns its own FixedBiomeSource; the
            // RandomState here plays dummy()'s role (nothing samples it).
        }

        spawnTargetValues = biomeSource ? biomeSource->getSpawnTarget()
                                        : std::vector<minecraft::world::biome::Climate::ParameterPoint>{};
        spawnTargetPointers.clear();
        spawnTargetPointers.reserve(spawnTargetValues.size());
        for (auto& point : spawnTargetValues) {
            spawnTargetPointers.push_back(
                reinterpret_cast<minecraft::levelgen::ClimateParameterPoint*>(&point)
            );
        }

        if (verbose) std::cout << "  Creating NoiseGeneratorSettings..." << std::endl;
        if (g_dimension == "nether") {
            // nether.json: sea_level 32, netherrack/lava, no mob-gen disable,
            // aquifers/ore veins OFF, legacy_random_source TRUE.
            settings = new minecraft::levelgen::NoiseGeneratorSettings(
                noiseSettings,
                minecraft::world::level::block::Blocks::getDefaultState("minecraft:netherrack"),
                minecraft::world::level::block::Blocks::LAVA->defaultBlockState(),
                *router, nullptr, spawnTargetPointers, 32, false, false, false, true
            );
        } else if (g_dimension == "end") {
            // end.json: sea_level 0, end_stone/air, aquifers/ore veins OFF,
            // legacy_random_source TRUE.
            settings = new minecraft::levelgen::NoiseGeneratorSettings(
                noiseSettings,
                minecraft::world::level::block::Blocks::getDefaultState("minecraft:end_stone"),
                minecraft::world::level::block::Blocks::AIR->defaultBlockState(),
                *router, nullptr, spawnTargetPointers, 0, false, false, false, true
            );
        } else {
            settings = new minecraft::levelgen::NoiseGeneratorSettings(
                noiseSettings,
                minecraft::world::level::block::Blocks::STONE->defaultBlockState(),
                minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
                *router, nullptr, spawnTargetPointers, 63, false, true, true, false
            );
        }

        if (verbose) std::cout << "  Creating RandomState..." << std::endl;
        randomState = minecraft::levelgen::RandomState::create(settings, SEED);

        if (verbose) std::cout << "  Creating surface rules + FluidPicker..." << std::endl;
        if (g_dimension == "nether") {
            surfaceRules = minecraft::levelgen::SurfaceRuleData::nether();
            // Reference: NoiseBasedChunkGenerator lava picker for the nether.
            fluidPicker = new minecraft::levelgen::SeaLevelFluidPicker(
                32, minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
        } else if (g_dimension == "end") {
            surfaceRules = minecraft::levelgen::SurfaceRuleData::end();
            // (0, AIR): sea level 0 with an air "fluid".
            fluidPicker = new minecraft::levelgen::SeaLevelFluidPicker(
                0, minecraft::world::level::block::Blocks::AIR->defaultBlockState());
        } else {
            surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();
            fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
                63, -54,
                minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
                minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
            );
        }

        if (g_worldType == "flat") {
            // Reference: WorldPresets FLAT - FlatLevelSource with the chosen
            // preset (default = FlatLevelGeneratorSettings.getDefault()),
            // optionally overridden by a custom "<layers>;<biome>" string.
            if (verbose) std::cout << "  Creating FlatLevelSource..." << std::endl;
            minecraft::levelgen::flat::FlatLevelGeneratorSettings flatSettings =
                g_flatPreset.empty()
                    ? minecraft::levelgen::flat::FlatLevelGeneratorSettings::getDefault()
                    : minecraft::levelgen::flat::FlatLevelGeneratorSettings::preset(g_flatPreset);
            if (!g_flatLayers.empty()) {
                flatSettings = minecraft::levelgen::flat::FlatLevelGeneratorSettings::fromString(
                    g_flatLayers, flatSettings);
            }
            std::cout << "  Flat settings: " << flatSettings.toString() << std::endl;
            auto* flatGenerator =
                new minecraft::levelgen::FlatLevelSource(std::move(flatSettings));
            flatGenerator->setLevelHeightRange(MIN_Y, HEIGHT);
            generator = flatGenerator;
        } else {
            if (verbose) std::cout << "  Creating NoiseBasedChunkGenerator..." << std::endl;
            // The generator's fill block is the dimension default_block.
            minecraft::BlockState* fillBlock = settings->defaultBlock();
            auto* noiseGenerator = new minecraft::levelgen::NoiseBasedChunkGenerator(
                settings, randomState->surfaceSystem(), surfaceRules,
                fillBlock, airBlock, fluidPicker, nullptr
            );
            noiseGenerator->setBiomeSource(biomeSource.get());
            generator = noiseGenerator;
        }

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
    std::cerr << "  --trace-modifiers  Trace the actual modifier/placement path; uses stderr if --feature-log is omitted\n";
    std::cerr << "  --block-trace <f>  Write block mutations to a separate file\n";
    std::cerr << "  --seed <seed>      World seed (default: 12345)\n";
    std::cerr << "  --phases <spec>    Phase configuration (default: all)\n";
    std::cerr << "  --dump-full        Radius mode: write canonical full-state dump (FORMAT.md) instead of histograms\n";
    std::cerr << "  --world-type <t>   default|flat|large_biomes|amplified|single_biome_surface (overworld only)\n";
    std::cerr << "  --flat-preset <p>  flat: vanilla preset short name (default: MC default flat settings)\n";
    std::cerr << "  --flat-layers <s>  flat: custom \"<layers>;<biome>\" preset string\n";
    std::cerr << "  --single-biome <b> single_biome_surface: biome id (default minecraft:plains)\n";
    std::cerr << "  --quiet            Suppress progress output\n";
    std::cerr << "\n";
    std::cerr << "Phase specifications: \"all\", or A-B where A is 0 (EMPTY start,\n";
    std::cerr << "structures ON - NOT YET SUPPORTED in C++) or 3 (BIOMES start, structures\n";
    std::cerr << "off) and B is 1..7 (>= 3 when A is 3). Examples:\n";
    std::cerr << "  3-7    BIOMES->FEATURES, no structures (canonical parity spec)\n";
    std::cerr << "  3-5    BIOMES->SURFACE, no structures\n";
    std::cerr << "  all    Complete generation (0-11, EMPTY->FULL)\n";
    std::cerr << "\n";
    std::cerr << "Phases: 0=EMPTY, 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE,\n";
    std::cerr << "        5=SURFACE, 6=CARVERS, 7=FEATURES, 8=INITIALIZE_LIGHT, 9=LIGHT, 10=SPAWN, 11=FULL\n";
}

void configurePhases(const std::string& phases) {
    // Reference: MinecraftAsyncChunkTest.java configurePhaseSpec() - must stay
    // semantically identical.
    // Phases: 0=EMPTY, 1=STRUCTURE_STARTS, 2=STRUCTURE_REFS, 3=BIOMES, 4=NOISE,
    //         5=SURFACE, 6=CARVERS, 7=FEATURES, 8=INITIALIZE_LIGHT, 9=LIGHT, 10=SPAWN, 11=FULL
    // Accepted: "all", or "A-B" with A in {0,3} (0 = structures ON) and B in 1..7
    // (B >= 3 when A is 3). An unrecognized spec must fail loudly: silently
    // mapping it to "all" runs the racy FULL pipeline and produces
    // nondeterministic dumps.

    using ChunkStatus = minecraft::world::chunk::status::ChunkStatus;

    int start = -1, end = -1;
    if (phases == "all") {
        targetStatus = &ChunkStatus::FULL;
        generateStructures = true;
        phasesDescription = "all (0-11, complete generation)";
    } else if (phases.size() == 3 && (phases[0] == '0' || phases[0] == '3') && phases[1] == '-'
               && phases[2] >= '1' && phases[2] <= '7') {
        start = phases[0] - '0';
        end = phases[2] - '0';
        if (start == 3 && end < 3) {
            std::cerr << "Error: invalid --phases spec '" << phases << "': end phase "
                      << end << " precedes start phase 3\n";
            std::exit(2);
        }
        static const ChunkStatus* endStatuses[8] = {
            nullptr, &ChunkStatus::STRUCTURE_STARTS, &ChunkStatus::STRUCTURE_REFERENCES,
            &ChunkStatus::BIOMES, &ChunkStatus::NOISE, &ChunkStatus::SURFACE,
            &ChunkStatus::CARVERS, &ChunkStatus::FEATURES
        };
        static const char* endNames[8] = {
            nullptr, "STRUCTURE_STARTS", "STRUCTURE_REFS", "BIOMES", "NOISE",
            "SURFACE", "CARVERS", "FEATURES"
        };
        targetStatus = endStatuses[end];
        generateStructures = (start == 0);
        phasesDescription = phases + " (" + (start == 0 ? "EMPTY" : "BIOMES") + "->" + endNames[end]
            + ", " + (generateStructures ? "with structures" : "no structures") + ")";
    } else {
        std::cerr << "Error: unknown --phases spec '" << phases
                  << "' (valid: all, or A-B with A in {0,3}, B in 1..7, e.g. 3-7, 0-2, 3-5)\n";
        std::exit(2);
    }

    // GUARD: C++ structure generation is PARTIAL (see Structures::isImplemented).
    // All OVERWORLD structures generate at full block parity (B7 complete,
    // 2026-08-13), so structures-ON overworld dumps are complete and the old
    // partial-subset guard is lifted. --partial-structures remains accepted
    // as a no-op for script compatibility. (Nether/End structures are Part D;
    // re-add a guard if dimension support lands before they do.)
    (void)partialStructuresAck;
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

/**
 * Canonical per-chunk parity dump. See tests/parity/FORMAT.md.
 * Must stay byte-identical to MinecraftAsyncChunkTest.writeCanonicalChunk().
 *   C,<chunkX>,<chunkZ>
 *   B,<x>,<y>,<z>,<full_state>     (y outer, then z, then x; plain air omitted)
 *   Q,<qx>,<qy>,<qz>,<biome>       (absolute quart coords; qx outer, qy, qz)
 *   H,<WS|OF>,<x>,<z>,<height>     (type outer, then z, then x)
 */
void writeCanonicalChunk(std::ostream& out, world::IChunk* chunk, int chunkX, int chunkZ) {
    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    out << "C," << chunkX << "," << chunkZ << "\n";

    using ChunkStatusT = minecraft::world::chunk::status::ChunkStatus;

    // S/P/R lines (FORMAT.md): structures-on only. Must stay byte-identical to
    // the Java writeCanonicalStructures(). StructureStartMap / the reference
    // map are std::map<std::string, ...> - already sorted by structure id.
    if (generateStructures) {
        for (const auto& [name, start] : chunk->getAllStructureStarts()) {
            const auto& bb = start.boundingBox;
            out << "S," << name << "," << start.references
                << "," << bb.minX << "," << bb.minY << "," << bb.minZ
                << "," << bb.maxX << "," << bb.maxY << "," << bb.maxZ << "\n";
            for (const auto& piece : start.pieces) {
                const auto& pb = piece.boundingBox;
                out << "P," << name << "," << piece.pieceType
                    << "," << piece.rotation << "," << piece.genDepth
                    << "," << pb.minX << "," << pb.minY << "," << pb.minZ
                    << "," << pb.maxX << "," << pb.maxY << "," << pb.maxZ
                    << "," << piece.detail << "\n";
            }
        }
        if (targetStatus->isOrAfter(ChunkStatusT::STRUCTURE_REFERENCES)) {
            for (const auto& [name, refs] : chunk->getAllStructureReferences()) {
                if (refs.empty()) continue;
                // std::set<int64_t> ordering is numeric on the packed value
                // (z in high bits, x low) - NOT (cx, cz) order. Sort explicitly.
                std::vector<std::pair<int, int>> positions;
                positions.reserve(refs.size());
                for (int64_t packed : refs) {
                    minecraft::world::ChunkPos pos(packed);
                    positions.emplace_back(pos.x(), pos.z());
                }
                std::sort(positions.begin(), positions.end());
                out << "R," << name;
                for (const auto& [rx, rz] : positions) {
                    out << "," << rx << ";" << rz;
                }
                out << "\n";
            }
        }
    }

    // FORMAT.md "Section presence by phase spec": B/Q require target >= BIOMES.
    if (!targetStatus->isOrAfter(ChunkStatusT::BIOMES)) return;

    for (int y = MIN_Y; y < MAX_Y; y++) {
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                minecraft::core::BlockPos blockPos(startX + x, y, startZ + z);
                minecraft::BlockState* state = chunk->getBlockState(blockPos);
                if (state == nullptr) continue;
                // Plain air is implicit; cave_air/void_air are real parity signal.
                if (state->getIdentifier() == "minecraft:air") continue;
                out << "B," << x << "," << y << "," << z << "," << state->toStateString() << "\n";
            }
        }
    }

    int quartMinX = startX >> 2;
    int quartMinZ = startZ >> 2;
    int quartMinY = MIN_Y >> 2;
    int quartMaxY = (MAX_Y >> 2) - 1;
    for (int qx = quartMinX; qx < quartMinX + 4; qx++) {
        for (int qy = quartMinY; qy <= quartMaxY; qy++) {
            for (int qz = quartMinZ; qz < quartMinZ + 4; qz++) {
                // IChunk exposes getBiome(BlockPos), which floors back to these
                // exact quart coords (QuartPos.fromBlock(q*4) == q).
                minecraft::world::biome::BiomeHolder biome =
                    chunk->getBiome(minecraft::core::BlockPos(qx * 4, qy * 4, qz * 4));
                out << "Q," << qx << "," << qy << "," << qz << ","
                    << (biome ? biome->getName() : "unknown") << "\n";
            }
        }
    }

    // H lines require target >= NOISE (heightmaps are a NOISE-phase product).
    if (!targetStatus->isOrAfter(ChunkStatusT::NOISE)) return;
    struct { const char* label; minecraft::levelgen::Heightmap::Types type; } heightmapTypes[] = {
        {"WS", minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG},
        {"OF", minecraft::levelgen::Heightmap::Types::OCEAN_FLOOR_WG},
    };
    for (const auto& hm : heightmapTypes) {
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                out << "H," << hm.label << "," << x << "," << z << ","
                    << chunk->getHeight(static_cast<int>(hm.type), x, z) << "\n";
            }
        }
    }

    // E lines: canonical block-entity payloads (FORMAT.md), gated behind
    // --dump-block-entities and target >= FEATURES; local x/z, absolute y,
    // ordered y outer, z, x (the store key order).
    if (dumpBlockEntities && targetStatus->isOrAfter(ChunkStatusT::FEATURES)) {
        if (const auto* blockEntities = chunk->getBlockEntityNbts()) {
            for (const auto& [key, payload] : *blockEntities) {
                const auto& [y, wz, wx] = key;
                out << "E," << (wx - chunkX * 16) << "," << y << ","
                    << (wz - chunkZ * 16) << "," << payload << "\n";
            }
        }
    }
}

/**
 * Placement-layer trace, mirroring the Java harness tracePlacements() line
 * grammar exactly (POSSIBLE / RING / PS; see MinecraftAsyncChunkTest.java).
 * Compared with the Java sidecar via diff ignoring '#' header lines.
 */
void writePlacementTrace(const std::string& path,
                         minecraft::world::biome::BiomeSource* biomeSource,
                         const minecraft::world::biome::Climate::Sampler* sampler,
                         int centerX, int centerZ, int scanRadius) {
    using namespace minecraft::levelgen::structure;

    auto state = ChunkGeneratorStructureState::createForNormal(
        sampler, SEED, biomeSource, StructureSets::all());
    state.ensureStructuresGenerated();

    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "Failed to open placement trace file: " << path << "\n";
        std::exit(2);
    }
    out << "# C++ Placement Trace\n";
    out << "# Seed: " << SEED << "\n";
    out << "# Scan: center (" << centerX << "," << centerZ << ") radius " << scanRadius << "\n";

    for (const StructureSet* set : state.possibleStructureSets()) {
        out << "POSSIBLE," << set->name << "\n";
    }
    for (const StructureSet* set : state.possibleStructureSets()) {
        if (const auto* rings = dynamic_cast<const ConcentricRingsStructurePlacement*>(set->placement.get())) {
            if (const auto* positions = state.getRingPositionsFor(rings)) {
                int index = 0;
                for (const auto& pos : *positions) {
                    out << "RING," << set->name << "," << (index++) << "," << pos.first << "," << pos.second << "\n";
                }
            }
        }
        for (int cx = centerX - scanRadius; cx <= centerX + scanRadius; cx++) {
            for (int cz = centerZ - scanRadius; cz <= centerZ + scanRadius; cz++) {
                if (set->placement->isStructureChunk(state, cx, cz)) {
                    out << "PS," << set->name << "," << cx << "," << cz << "\n";
                }
            }
        }
    }
}

void writeCanonicalHeader(std::ostream& out, const char* mode) {
    out << "# C++ Canonical Parity Dump (see tests/parity/FORMAT.md)\n";
    out << "# USES: ServerChunkCache + ChunkMap Pipeline\n";
    out << "# Mode: " << mode << "\n";
    out << "# Seed: " << SEED << "\n";
    out << "# Phases: " << phasesDescription << "\n";
    out << "# Target: " << targetStatus->getName() << "\n";
}

int main(int argc, char* argv[]) {
    auto mainStart = std::chrono::steady_clock::now();
    // Parse arguments
    int radius = -1;  // -1 means not set
    int centerX = 0;
    int centerZ = 0;
    bool singleMode = false;
    int singleChunkX = 0;
    int singleChunkZ = 0;
    std::string outputPath;
    std::string featureLogPath;  // Path for feature log output
    std::string blockTracePath;
    std::string perfJsonPath;
    std::string tracePlacementsPath;
    int placementRadius = -1;  // -1 = use the run's radius
    bool verbose = true;
    std::string phases = "all";
    bool traceModifiers = false;
    bool dumpFull = false;

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
        } else if (arg == "--dump-full") {
            dumpFull = true;
        } else if (arg == "--trace-modifiers") {
            traceModifiers = true;
        } else if (arg == "--block-trace" && i + 1 < argc) {
            blockTracePath = argv[++i];
        } else if (arg == "--perf-json" && i + 1 < argc) {
            perfJsonPath = argv[++i];
        } else if (arg == "--trace-placements" && i + 1 < argc) {
            tracePlacementsPath = argv[++i];
        } else if (arg == "--placement-radius" && i + 1 < argc) {
            placementRadius = std::atoi(argv[++i]);
        } else if (arg == "--partial-structures") {
            partialStructuresAck = true;
        } else if (arg == "--dump-block-entities") {
            dumpBlockEntities = true;
        } else if (arg == "--dimension" && i + 1 < argc) {
            g_dimension = argv[++i];
            if (g_dimension == "overworld") {
                // default heights already set
            } else if (g_dimension == "nether") {
                MIN_Y = 0;
                HEIGHT = 256;
                MAX_Y = MIN_Y + HEIGHT;
            } else if (g_dimension == "end") {
                MIN_Y = 0;
                HEIGHT = 256;
                MAX_Y = MIN_Y + HEIGHT;
            } else {
                std::cerr << "Error: unknown --dimension '" << g_dimension << "'\n";
                return 2;
            }
        } else if (arg == "--world-type" && i + 1 < argc) {
            g_worldType = argv[++i];
        } else if (arg == "--flat-preset" && i + 1 < argc) {
            g_flatPreset = argv[++i];
        } else if (arg == "--flat-layers" && i + 1 < argc) {
            g_flatLayers = argv[++i];
        } else if (arg == "--single-biome" && i + 1 < argc) {
            g_singleBiome = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            // Unknown args must fail loudly: silently ignoring a mistyped flag
            // (e.g. a bad --dimension) would compare mismatched configurations.
            std::cerr << "Error: unknown argument '" << arg << "' (see --help)\n";
            return 2;
        }
    }

    // Validate world-type arguments
    if (g_worldType != "default" && g_worldType != "flat" && g_worldType != "large_biomes"
        && g_worldType != "amplified" && g_worldType != "single_biome_surface") {
        std::cerr << "Error: unknown --world-type '" << g_worldType << "'\n";
        return 2;
    }
    if (g_worldType != "default" && g_dimension != "overworld") {
        std::cerr << "Error: --world-type is overworld-only\n";
        return 2;
    }
    if (!g_singleBiome.empty() && g_singleBiome.find(':') == std::string::npos) {
        g_singleBiome = "minecraft:" + g_singleBiome;
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

    minecraft::levelgen::placement::PlacedFeature::setModifierTracingEnabled(traceModifiers);

    // Setup feature logging if requested, or send traced output to stderr by default
    std::ofstream featureLogFile;
    std::ostream* featureLogStream = nullptr;
    if (!featureLogPath.empty()) {
        featureLogFile.open(featureLogPath);
        if (featureLogFile.is_open()) {
            std::cout << "Feature logging enabled: " << featureLogPath << std::endl;
            featureLogStream = &featureLogFile;
        } else {
            std::cerr << "Warning: Could not open feature log file: " << featureLogPath << std::endl;
        }
    } else if (traceModifiers) {
        featureLogStream = &std::cerr;
    }

    if (featureLogStream) {
        minecraft::levelgen::placement::PlacedFeature::setLoggingEnabled(true);
        minecraft::levelgen::placement::PlacedFeature::setLogStream(featureLogStream);
        minecraft::levelgen::ChunkGenerator::setFeatureLoggingEnabled(true, traceModifiers ? 1 : 2);
        minecraft::levelgen::ChunkGenerator::setFeatureLogStream(featureLogStream);
    }

    std::ofstream blockTraceFile;
    if (!blockTracePath.empty()) {
        blockTraceFile.open(blockTracePath);
        if (blockTraceFile.is_open()) {
            std::cout << "Block tracing enabled: " << blockTracePath << std::endl;
            minecraft::levelgen::feature::BlockChangeTrace::stream = &blockTraceFile;
        } else {
            std::cerr << "Warning: Could not open block trace file: " << blockTracePath << std::endl;
        }
    }

    minecraft::levelgen::feature::BlockChangeTrace::enabled =
        traceModifiers || minecraft::levelgen::feature::BlockChangeTrace::stream != nullptr;

    try {
        // ========== Step 1: Bootstrap ==========
        // Debug hook: MC_WORLDGEN_TWEAKS='{"caves":false,...}' applies World
        // Properties tweaks (same JSON schema as the game) for crash repro /
        // sandbox testing. NEVER set during parity gates.
        if (const char* tweaksEnv = std::getenv("MC_WORLDGEN_TWEAKS")) {
            minecraft::levelgen::WorldGenTweaks::reset();
            try {
                auto& tw = minecraft::levelgen::WorldGenTweaks::get();
                nlohmann::json tj = nlohmann::json::parse(tweaksEnv);
                tw.carversEnabled = tj.value("caves", true);
                if (tj.contains("steps") && tj["steps"].is_array()) {
                    for (size_t i = 0; i < tw.featureStepEnabled.size()
                                       && i < tj["steps"].size(); ++i) {
                        tw.featureStepEnabled[i] = tj["steps"][i].get<bool>();
                    }
                }
                tw.featureDensity     = tj.value("featureDensity", 1.0f);
                tw.oreDensity         = tj.value("oreDensity", 1.0f);
                tw.vegetationDensity  = tj.value("vegetationDensity", 1.0f);
                tw.structureFrequency = tj.value("structureFrequency", 1.0f);
                for (const auto& b : tj.value("disabledBiomes", nlohmann::json::array())) {
                    tw.disabledBiomes.insert(b.get<std::string>());
                }
                std::cout << "  MC_WORLDGEN_TWEAKS ACTIVE (non-vanilla)" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Bad MC_WORLDGEN_TWEAKS: " << e.what() << std::endl;
                return 2;
            }
        }

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

        // Structures-on: build the placement state and inject it into the
        // pipeline context (nullptr = structures disabled, historical no-op).
        std::optional<minecraft::levelgen::structure::ChunkGeneratorStructureState> structureState;
        if (generateStructures) {
            if (auto* flatGenerator =
                    dynamic_cast<minecraft::levelgen::FlatLevelSource*>(worldGen.generator)) {
                // Reference: FlatLevelSource.createState - structure_overrides
                // (or all sets when none) via createForFlat (rings seed 0).
                std::vector<const minecraft::levelgen::structure::StructureSet*> sets;
                const auto& overrides = flatGenerator->settings().structureOverrides();
                if (overrides.has_value()) {
                    for (const std::string& setName : *overrides) {
                        sets.push_back(&minecraft::levelgen::structure::StructureSets::byName(setName));
                    }
                } else {
                    sets = minecraft::levelgen::structure::StructureSets::all();
                }
                structureState.emplace(
                    minecraft::levelgen::structure::ChunkGeneratorStructureState::createForFlat(
                        worldGen.randomState->sampler(), SEED, flatGenerator->biomeSource(), sets));
            } else {
                structureState.emplace(minecraft::levelgen::structure::ChunkGeneratorStructureState::createForNormal(
                    worldGen.randomState->sampler(), SEED, worldGen.biomeSource.get(),
                    minecraft::levelgen::structure::StructureSets::all()));
            }
            chunkCache.getChunkMap().worldGenContextMutable().structureState = &*structureState;
            // Gates the structure pass in applyBiomeDecoration (Java:
            // structureManager.shouldGenerateStructures()).
            worldGen.generator->setGenerateStructures(true);
            std::cout << "  Structure state injected (PARTIAL structure support)." << std::endl;
        }

        // Step 4.5 (spawn preparation) is deliberately SKIPPED, mirroring the
        // Java harness: spawn-area pregeneration runs FEATURES steps for
        // adjacent chunks in parallel, making cross-chunk features (clay/moss
        // patches, sculk) scheduling-dependent - verified nondeterministic on
        // both sides. The request loop below serializes every FEATURES step.
        std::cout << "Step 4.5: Skipping spawn preparation (parity determinism)." << std::endl;

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

        if (!tracePlacementsPath.empty()) {
            std::cout << "Tracing structure placements..." << std::endl;
            int scanRadius = placementRadius >= 0 ? placementRadius : radius;
            writePlacementTrace(tracePlacementsPath, worldGen.biomeSource.get(),
                                worldGen.randomState->sampler(), centerX, centerZ, scanRadius);
            std::cout << "Placement trace: " << tracePlacementsPath << std::endl;
        }

        // ========== Step 6: Write output ==========
        std::cout << "Step 6: Writing output..." << std::endl;
        std::ofstream outFile(outputPath);
        if (!outFile.is_open()) {
            std::cerr << "Failed to open output file: " << outputPath << std::endl;
            return 1;
        }

        if (singleMode || dumpFull) {
            // Canonical full-state dump (see tests/parity/FORMAT.md).
            // generatedChunks is std::map<pair<cx,cz>> so iteration is already
            // (chunkX, chunkZ) ascending, matching FORMAT.md chunk ordering.
            writeCanonicalHeader(outFile, singleMode ? "single" : "radius");
            for (const auto& entry : generatedChunks) {
                writeCanonicalChunk(outFile, entry.second, entry.first.first, entry.first.second);
            }
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

        if (!perfJsonPath.empty()) {
            auto dumpEnd = std::chrono::steady_clock::now();
            double setupSeconds = std::chrono::duration<double>(startTime - mainStart).count();
            double dumpSeconds = std::chrono::duration<double>(dumpEnd - endTime).count();
            struct rusage ru {};
            getrusage(RUSAGE_SELF, &ru);
            std::ofstream perf(perfJsonPath);
            perf << "{\n"
                 << "  \"seed\": " << SEED << ",\n"
                 << "  \"mode\": \"" << (singleMode ? "single" : "radius") << "\",\n"
                 << "  \"radius\": " << radius << ",\n"
                 << "  \"center\": [" << centerX << ", " << centerZ << "],\n"
                 << "  \"phases\": \"" << phases << "\",\n"
                 << "  \"chunks\": " << generatedChunks.size() << ",\n"
                 << std::fixed << std::setprecision(4)
                 << "  \"generation_seconds\": " << totalTime << ",\n"
                 << "  \"chunks_per_sec\": " << rate << ",\n"
                 << "  \"ms_per_chunk\": " << (totalTime * 1000.0 / totalChunks) << ",\n"
                 << "  \"setup_seconds\": " << setupSeconds << ",\n"
                 << "  \"dump_seconds\": " << dumpSeconds << ",\n"
                 << "  \"max_rss_bytes\": " << static_cast<long long>(ru.ru_maxrss) << "\n"
                 << "}\n";
        }

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
