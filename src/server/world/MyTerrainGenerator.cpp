// File: src/server/world/MyTerrainGenerator.cpp
#include "MyTerrainGenerator.hpp"
#include "storage/SectionDataUnpacker.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <chrono>
#include <future>

// Terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/Heightmap.h"
#include "world/biome/OverworldBiomeBuilder.h"

using minecraft::world::level::block::Blocks;
using minecraft::world::BlockRegistry;
using minecraft::BlockState;

static constexpr int MIN_Y = -64;
static constexpr int HEIGHT = 384;
static constexpr int MAX_Y = MIN_Y + HEIGHT;

// Epoch for the thread_local MapBlockType caches. Bumped every generator
// Initialize() because Blocks::bootstrap() may recreate Block objects on
// world reload — a worker's cached pointers from the previous world would
// otherwise alias freshly allocated blocks at reused addresses.
static std::atomic<uint32_t> s_blockMapEpoch{1};

namespace Game {

    MyTerrainGenerator::MyTerrainGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Info("[MyTerrainGenerator] Created with seed: %d", config.seed);
    }

    MyTerrainGenerator::~MyTerrainGenerator() {
        Shutdown();
    }

    bool MyTerrainGenerator::Initialize() {
        if (m_initialized) {
            Log::Warning("[MyTerrainGenerator] Already initialized");
            return true;
        }

        try {
            int64_t seed = static_cast<int64_t>(m_config.seed);
            Log::Info("[MyTerrainGenerator] Initializing with seed: %lld", seed);

            // Invalidate every worker thread's MapBlockType cache — the
            // bootstrap below may recreate Block objects, and stale cached
            // pointers from a previous world could alias reused addresses.
            s_blockMapEpoch.fetch_add(1, std::memory_order_release);

            // ================================================================
            // Step 1: Bootstrap registries (once per program)
            // ================================================================
            Blocks::bootstrap();
            minecraft::levelgen::NoiseRegistry::bootstrap();
            minecraft::levelgen::DensityFunctionRegistry::bootstrap(seed);
            minecraft::levelgen::SurfaceRuleData::initialize();
            Log::Info("[MyTerrainGenerator] Registries bootstrapped");

            // ================================================================
            // Step 2: Cache block states
            // ================================================================
            m_airBlock = Blocks::AIR->defaultBlockState();
            m_stoneBlock = Blocks::STONE->defaultBlockState();

            // ================================================================
            // Step 3: Create block registry
            // ================================================================
            m_blockRegistry = new BlockRegistry();
            m_blockRegistry->registerBlock(m_airBlock);
            m_blockRegistry->registerBlock(m_stoneBlock);
            m_blockRegistry->registerBlock(Blocks::WATER->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::LAVA->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::DEEPSLATE->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::BEDROCK->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::GRASS_BLOCK->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::DIRT->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::SAND->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::GRAVEL->defaultBlockState());
            m_blockRegistry->registerBlock(Blocks::TUFF->defaultBlockState());
            Log::Info("[MyTerrainGenerator] BlockRegistry initialized");

            // ================================================================
            // Step 4: Create world generation components
            // ================================================================
            auto* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);
            auto noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

            // Spawn-target climate list (MC NoiseGeneratorSettings.overworld()
            // passes OverworldBiomeBuilder.spawnTarget()). RandomState hands
            // this to the Climate::Sampler, which is what makes
            // Sampler::findSpawnPosition() work — with an empty list it just
            // returns the origin. The settings API wants opaque
            // ClimateParameterPoint*, so keep value storage here and pass
            // pointers (same reinterpret pattern RandomState uses to read
            // them back).
            m_spawnTargetStorage =
                minecraft::world::biome::OverworldBiomeBuilder().spawnTarget();
            std::vector<minecraft::levelgen::ClimateParameterPoint*> spawnTargetPtrs;
            spawnTargetPtrs.reserve(m_spawnTargetStorage.size());
            for (auto& point : m_spawnTargetStorage) {
                spawnTargetPtrs.push_back(
                    reinterpret_cast<minecraft::levelgen::ClimateParameterPoint*>(&point));
            }

            m_settings = new minecraft::levelgen::NoiseGeneratorSettings(
                noiseSettings,
                Blocks::STONE->defaultBlockState(),
                Blocks::WATER->defaultBlockState(),
                *router, nullptr, spawnTargetPtrs, 63, false, true, true, false
            );

            m_randomState = minecraft::levelgen::RandomState::create(m_settings, seed);

            auto* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

            m_fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
                63, -54,
                Blocks::WATER->defaultBlockState(),
                Blocks::LAVA->defaultBlockState()
            );

            m_biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

            m_generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
                m_settings, m_randomState->surfaceSystem(), surfaceRules,
                m_stoneBlock, m_airBlock, m_fluidPicker, nullptr
            );
            m_generator->setBiomeSource(m_biomeSource.get());
            Log::Info("[MyTerrainGenerator] World generation components created");

            // ================================================================
            // Step 5: Create executors (thread pool + main thread queue)
            // ================================================================
            m_backgroundExecutor = std::make_unique<BackgroundExecutor>();
            m_mainThreadExecutor = std::make_unique<MainThreadExecutor>();
            Log::Info("[MyTerrainGenerator] Executors created (%zu background threads)",
                     static_cast<size_t>(std::thread::hardware_concurrency()));

            // ================================================================
            // Step 6: Create ServerChunkCache (the full async pipeline)
            //
            // This is the SAME pipeline as async_chunk_test and Minecraft's
            // DedicatedServer. Chunks flow through:
            //   ServerChunkCache -> ChunkMap -> DistanceManager ->
            //   ChunkGenerationTask -> Worker Threads
            // ================================================================
            m_chunkCache = std::make_unique<minecraft::server::level::ServerChunkCache>(
                m_generator,
                m_randomState,
                seed,
                m_backgroundExecutor->getExecutor(),
                m_mainThreadExecutor->getExecutor(),
                m_blockRegistry,
                m_airBlock,
                m_stoneBlock,
                MIN_Y,
                HEIGHT
            );

            m_chunkCache->setTaskPoller([this]() {
                if (m_mainThreadExecutor->hasPendingTasks()) {
                    m_mainThreadExecutor->runPendingTasks();
                }
            });
            Log::Info("[MyTerrainGenerator] ServerChunkCache created");

            // ================================================================
            // Step 7: Set target chunk status
            // Full generation: EMPTY -> FULL (phases 0-11)
            // ================================================================
            m_targetStatus = &minecraft::world::chunk::status::ChunkStatus::FULL;
            Log::Info("[MyTerrainGenerator] Target status: %s", m_targetStatus->getName().c_str());

            m_initialized = true;
            Log::Info("[MyTerrainGenerator] Initialization complete!");
            return true;

        } catch (const std::exception& e) {
            Log::Error("[MyTerrainGenerator] Initialization failed: %s", e.what());
            Shutdown();
            return false;
        }
    }

    void MyTerrainGenerator::RequestAbort() {
        if (m_chunkCache) {
            m_chunkCache->requestAbort();
        }
    }

    void MyTerrainGenerator::Shutdown() {
        if (!m_initialized) return;

        Log::Info("[MyTerrainGenerator] Shutting down...");

        // Stop worker threads FIRST so no tasks reference destroyed objects
        m_backgroundExecutor.reset();
        m_mainThreadExecutor.reset();
        m_chunkCache.reset();

        delete m_generator;   m_generator = nullptr;
        m_biomeSource.reset();
        delete m_fluidPicker;  m_fluidPicker = nullptr;
        delete m_randomState;  m_randomState = nullptr;
        delete m_settings;     m_settings = nullptr;
        delete m_blockRegistry; m_blockRegistry = nullptr;
        // After m_settings/m_randomState (they hold pointers into this).
        m_spawnTargetStorage.clear();

        m_initialized = false;
        Log::Info("[MyTerrainGenerator] Shutdown complete");
    }

    glm::ivec3 MyTerrainGenerator::FindSpawnPosition() {
        // Called once per world on the server thread; the generator may not
        // have lazily initialized yet.
        if (!m_initialized && !Initialize()) {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition: init failed, using legacy spawn");
            return glm::ivec3(0, 67, 0);
        }

        // ── Step 1: climate search (MC Climate.SpawnFinder) ────────────────
        // Radial fitness search over the biome parameter space, biased toward
        // the world origin. The library ports the whole algorithm; it needs
        // the spawn-target list wired through NoiseGeneratorSettings (done in
        // Initialize Step 4).
        const auto climatePos = m_randomState->sampler()->findSpawnPosition();
        const int spawnChunkX = climatePos.getX() >> 4;
        const int spawnChunkZ = climatePos.getZ() >> 4;

        const int32_t seaLevel = m_generator->getSeaLevel();
        auto surfaceAt = [&](int blockX, int blockZ) {
            return m_generator->getBaseHeight(blockX, blockZ,
                minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG,
                m_randomState);
        };

        // ── Step 2: chunk spiral (MC setInitialSpawn) ──────────────────────
        // MC walks an 11×11 chunk spiral around the climate chunk and takes
        // the first chunk with a valid spawn block. Full block validation
        // (PlayerSpawnFinder) needs generated chunk data, which doesn't exist
        // yet at world init — the dry-land test (worldgen surface above sea
        // level at the chunk centre) stands in for it, which is also what
        // rules out ocean columns in practice.
        const glm::ivec3 fallback(spawnChunkX * 16 + 8,
                                  std::max(surfaceAt(spawnChunkX * 16 + 8, spawnChunkZ * 16 + 8),
                                           seaLevel + 1),
                                  spawnChunkZ * 16 + 8);

        int xOff = 0, zOff = 0;
        int dx = 0, dz = -1;
        for (int i = 0; i < 11 * 11; ++i) {
            if (xOff >= -5 && xOff <= 5 && zOff >= -5 && zOff <= 5) {
                const int blockX = (spawnChunkX + xOff) * 16 + 8;
                const int blockZ = (spawnChunkZ + zOff) * 16 + 8;
                const int32_t surfaceY = surfaceAt(blockX, blockZ);
                if (surfaceY > seaLevel) {
                    Log::Info("[MyTerrainGenerator] Spawn selected at (%d, %d, %d) "
                              "(climate pos %d,%d; %d chunk probes)",
                              blockX, surfaceY, blockZ,
                              climatePos.getX(), climatePos.getZ(), i + 1);
                    return glm::ivec3(blockX, surfaceY, blockZ);
                }
            }
            // Square-spiral turn rule (matches MC's iteration order).
            if (xOff == zOff || (xOff < 0 && xOff == -zOff) ||
                (xOff > 0 && xOff == 1 - zOff)) {
                const int t = dx;
                dx = -dz;
                dz = t;
            }
            xOff += dx;
            zOff += dz;
        }

        Log::Info("[MyTerrainGenerator] Spawn fallback at (%d, %d, %d) — no dry land "
                  "within 5 chunks of climate pos", fallback.x, fallback.y, fallback.z);
        return fallback;
    }

    ChunkGenerationResult MyTerrainGenerator::GenerateChunk(Math::ChunkPos position) {
        ChunkGenerationResult result;
        result.success = false;

        if (!m_initialized) {
            result.errorMessage = "Generator not initialized";
            return result;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            // ================================================================
            // Generate chunk through the FULL ServerChunkCache pipeline
            //
            // ServerChunkCache.getChunk() goes through:
            //   1. Cache check
            //   2. getChunkFutureMainThread() -> adds ticket
            //   3. runDistanceManagerUpdates()
            //   4. ChunkHolder.scheduleChunkGenerationTask()
            //   5. ChunkMap.scheduleGenerationTask()
            //   6. ChunkTaskDispatcher.submit() -> ConsecutiveExecutor
            //   7. ChunkGenerationTask runs through all statuses
            //      (BIOMES -> NOISE -> SURFACE -> CARVERS -> FEATURES -> ...)
            //   8. managedBlock() pumps tasks until complete
            //
            // This provides multi-chunk neighbor access via WorldGenRegion,
            // so features like trees can span chunk boundaries correctly.
            // ================================================================
            world::IChunk* chunk = nullptr;
            {
                // Time the MC generation pipeline separately from our
                // conversion loop below — the next Tracy capture shows how
                // the per-chunk cost splits between the two.
                PROFILE_ZONE_N("TerrainLibGetChunk");
                chunk = m_chunkCache->getChunk(
                    position.x, position.z, *m_targetStatus, true
                );
            }

            if (!chunk) {
                result.errorMessage = "ServerChunkCache returned null chunk";
                return result;
            }

            // ================================================================
            // Convert from terrain library chunk to game chunk format
            // (section-wise, all-air sections skipped, lock-free block map)
            // ================================================================
            int blocksSet = 0;
            auto gameChunk = ConvertLibChunk(chunk, position, &blocksSet);

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            result.success = true;
            result.chunk = gameChunk;

            m_stats.chunksGenerated++;
            m_stats.totalGenerationTimeMs += duration.count();

            Log::Debug("[MyTerrainGenerator] Chunk (%d, %d) generated in %lldms (%d non-air blocks)",
                      position.x, position.z, duration.count(), blocksSet);

        } catch (const std::exception& e) {
            result.errorMessage = std::string("Exception: ") + e.what();
            Log::Error("[MyTerrainGenerator] Generation failed for chunk (%d, %d): %s",
                      position.x, position.z, e.what());
        }

        return result;
    }

    BlockID MyTerrainGenerator::MapBlockType(minecraft::world::BlockState* blockState) const {
        if (!blockState) return BlockID::Stone;

        // Block* pointers are stable within one bootstrap epoch (created once
        // in Blocks::bootstrap, never moved), so pointer equality suffices.
        const auto* block = blockState->getBlock();
        if (!block) return BlockID::Stone;

        // Lock-free per-thread cache + last-block memo. Terrain is dominated
        // by long runs of the same state (air, stone, deepslate, water), so
        // the memo alone absorbs the vast majority of calls; the map handles
        // the rest. No mutex — the old shared cache took ~98k lock/unlock per
        // converted chunk with every worker contending on it.
        struct ThreadCache {
            uint32_t epoch = 0;
            const void* lastBlock = nullptr;
            BlockID lastId = BlockID::Stone;
            std::unordered_map<const void*, BlockID> map;
        };
        thread_local ThreadCache tc;

        const uint32_t epoch = s_blockMapEpoch.load(std::memory_order_acquire);
        if (tc.epoch != epoch) {
            tc.map.clear();
            tc.lastBlock = nullptr;
            tc.epoch = epoch;
        }

        if (block == tc.lastBlock) {
            return tc.lastId;
        }

        auto it = tc.map.find(block);
        if (it == tc.map.end()) {
            // First encounter on this thread — resolve via string lookup
            // (slow path, bounded by unique block types, ~1150 total)
            Game::BlockStateRegistry::Initialize();
            Game::BlockState gameState;
            gameState.name = block->getIdentifier();
            gameState.resolvedId = Game::BlockStateRegistry::ResolveBlockState(gameState);
            it = tc.map.emplace(block, gameState.resolvedId).first;
        }

        tc.lastBlock = block;
        tc.lastId = it->second;
        return it->second;
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::ConvertLibChunk(minecraft::world::IChunk* chunk,
                                                               Math::ChunkPos position,
                                                               int* outBlocksSet) const {
        PROFILE_ZONE_N("ConvertChunk");
        auto gameChunk = std::make_shared<Chunk>();
        gameChunk->pos = position;

        // Section-wise conversion: skips all-air sections entirely (most of a
        // chunk's 384-block column is sky), reads block states through the
        // section's inline palette accessor instead of per-block virtual
        // IChunk::getBlockState with BlockPos construction, and writes
        // directly into the game section arrays instead of per-block
        // Chunk::SetBlock (which re-validates and re-reads every call).
        // Equivalent output: SetBlock on a fresh chunk was a no-op for air
        // and a plain section Set for everything else (onSectionDirty is
        // unset during conversion).
        const int libMinY = chunk->getMinBuildHeight();
        const int sectionsCount = chunk->getSectionsCount();
        int blocksSet = 0;

        for (int si = 0; si < sectionsCount; ++si) {
            auto& libSection = chunk->getSection(si);
            if (libSection.hasOnlyAir()) continue;

            const int baseY = libMinY + si * 16;
            const int gameSectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(baseY);
            if (gameSectionIndex < 0 || gameSectionIndex >= Math::SECTIONS_PER_CHUNK) continue;

            gameChunk->EnsureSection(gameSectionIndex);
            ChunkSection* outSection = gameChunk->GetSection(gameSectionIndex);
            if (!outSection) continue;

            for (int ly = 0; ly < 16; ++ly) {
                for (int lz = 0; lz < 16; ++lz) {
                    for (int lx = 0; lx < 16; ++lx) {
                        auto* blockState = libSection.getBlockState(lx, ly, lz);
                        const BlockID id = MapBlockType(blockState);
                        if (id != BlockID::Air) {
                            outSection->Set(lx, ly, lz, id);
                            ++blocksSet;
                        }
                    }
                }
            }
        }

        if (outBlocksSet) *outBlocksSet = blocksSet;
        return gameChunk;
    }

    // === Non-blocking async API ===

    bool MyTerrainGenerator::RequestChunkGeneration(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return false;

        // getChunkFuture dispatches to the main thread executor internally.
        // Since we're calling from the server thread (which IS the main thread
        // for the terrain library), this calls getChunkFutureMainThread directly,
        // which adds a ticket and schedules generation — but does NOT block.
        m_chunkCache->getChunkFuture(
            position.x, position.z, *m_targetStatus, true
        );
        return true;
    }

    void MyTerrainGenerator::PumpAsyncTasks() {
        if (!m_initialized || !m_chunkCache) return;

        // Minecraft pattern: loop until no more work to do.
        // Reference: ServerChunkCache.MainThreadExecutor.pollTask() calls
        // runDistanceManagerUpdates() each iteration, which can schedule more
        // work that feeds into the next iteration.
        bool didWork = true;
        int iterations = 0;
        const int MAX_ITERATIONS = 256; // safety cap

        while (didWork && iterations < MAX_ITERATIONS) {
            didWork = false;

            // runDistanceManagerUpdates calls runGenerationTasks internally
            if (m_chunkCache->runDistanceManagerUpdates()) {
                didWork = true;
            }

            // Pump main thread executor tasks (generation callbacks)
            if (m_mainThreadExecutor && m_mainThreadExecutor->hasPendingTasks()) {
                m_mainThreadExecutor->runPendingTasks();
                didWork = true;
            }

            iterations++;
        }
    }

    bool MyTerrainGenerator::IsChunkReady(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return false;
        // getChunkNow returns non-null only if chunk is at FULL status
        return m_chunkCache->getChunkNow(position.x, position.z) != nullptr;
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::GetCompletedChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) return nullptr;

        auto* chunk = m_chunkCache->getChunkNow(position.x, position.z);
        if (!chunk) return nullptr;

        // Convert from terrain library chunk to game chunk format
        // (section-wise, all-air sections skipped, lock-free block map)
        auto gameChunk = ConvertLibChunk(chunk, position, nullptr);

        m_stats.chunksGenerated++;
        return gameChunk;
    }

    // === Configuration methods ===

    void MyTerrainGenerator::SetConfig(const GenerationConfig& config) {
        m_config = config;
        if (m_initialized && config.seed != m_config.seed) {
            Log::Warning("[MyTerrainGenerator] Seed changed after initialization - requires restart");
        }
    }

    GenerationConfig MyTerrainGenerator::GetConfig() const { return m_config; }
    void MyTerrainGenerator::SetSeed(int32_t seed) { m_config.seed = seed; }
    int32_t MyTerrainGenerator::GetSeed() const { return m_config.seed; }
    void MyTerrainGenerator::SetWorldType(const std::string&) {}
    std::string MyTerrainGenerator::GetWorldType() const { return "overworld"; }
    void MyTerrainGenerator::SetPassEnabled(GenerationPass, bool) {}
    bool MyTerrainGenerator::IsPassEnabled(GenerationPass) const { return true; }
    bool MyTerrainGenerator::IsReady() const { return m_initialized; }

    ChunkGenerationResult MyTerrainGenerator::GenerateWithPasses(
        Math::ChunkPos position, const std::vector<GenerationPass>&) {
        return GenerateChunk(position);
    }

    std::future<ChunkGenerationResult> MyTerrainGenerator::GenerateChunkAsync(Math::ChunkPos position) {
        return std::async(std::launch::async, [this, position]() {
            return GenerateChunk(position);
        });
    }

    std::vector<int> MyTerrainGenerator::GenerateHeightMap(Math::ChunkPos) {
        return std::vector<int>(16 * 16, 64);
    }

    std::string MyTerrainGenerator::GenerateBiome(Math::ChunkPos) { return "plains"; }

    IChunkGenerator::GeneratorStats MyTerrainGenerator::GetStats() const { return m_stats; }
    void MyTerrainGenerator::ResetStats() { m_stats = GeneratorStats{}; }
    void MyTerrainGenerator::SetMaxGenerationTime(float) {}
    float MyTerrainGenerator::GetMaxGenerationTime() const { return 0.0f; }
    void MyTerrainGenerator::RegisterTerrainFunction(const std::string&, TerrainFunction) {}
    void MyTerrainGenerator::RegisterFeatureFunction(const std::string&, FeatureFunction) {}
    void MyTerrainGenerator::SetTerrainFunction(const std::string&) {}
    void MyTerrainGenerator::AddFeatureFunction(const std::string&) {}

    IChunkGenerator::DebugInfo MyTerrainGenerator::GetDebugInfo(Math::ChunkPos) {
        DebugInfo info;
        info.biome = "plains";
        info.heightMap = std::vector<int>(16 * 16, 64);
        for (int i = 0; i < 7; ++i) info.generationTimePerPass[i] = 0.0f;
        return info;
    }

    void MyTerrainGenerator::SetDebugMode(bool) {}
    bool MyTerrainGenerator::IsDebugMode() const { return false; }
    std::string MyTerrainGenerator::GetLastError() const { return ""; }
    void MyTerrainGenerator::ClearErrors() {}

} // namespace Game
