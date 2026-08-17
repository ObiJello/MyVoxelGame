// File: src/server/world/MyTerrainGenerator.cpp
#include "MyTerrainGenerator.hpp"
#include "storage/SectionDataUnpacker.hpp"
#include "common/world/biome/Biomes.hpp"
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

// Run BOTH conversion paths and compare, logging any disagreement.
//
// This is the gate for the palette remap. tools/terrain_parity cannot serve as
// one: CLAUDE.md is explicit that it "links terrain_library alone (no game
// code)", so it is unchanged by anything in this file by construction and would
// pass trivially. Checking against the per-voxel original instead covers every
// section of every chunk a real session generates, which is strictly more.
//
// Costs roughly double conversion when on. Off in every normal build.
static constexpr bool kVerifyPaletteConvert = false;

namespace Game {

    MyTerrainGenerator::MyTerrainGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Info("[MyTerrainGenerator] Created with seed: %lld",
                  static_cast<long long>(config.seed));
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
        // The server thread may be parked in waitForTasks until the next tick
        // deadline. Wake it so shutdown does not wait out the remainder of the
        // idle window.
        if (m_mainThreadExecutor) {
            m_mainThreadExecutor->wakeAll();
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

    MyTerrainGenerator::MappedBlock
    MyTerrainGenerator::MapBlockType(minecraft::world::BlockState* blockState) const {
        if (!blockState) return { BlockID::Stone, 0 };

        // Keyed on the BlockState*, not the Block*. Library states are interned
        // per (block, property tuple) by StateDefinition and, like Block*, are
        // created once per bootstrap epoch and never moved — so pointer
        // equality still suffices, but now `leaf_litter{facing=west,
        // segment_amount=3}` and `leaf_litter{facing=north,segment_amount=1}`
        // no longer collide. Keying on the Block* is what made every generated
        // furnace, log and leaf litter clump come out in its default state.
        //
        // Lock-free per-thread cache + last-state memo. Terrain is dominated by
        // long runs of the identical state (air, stone, deepslate, water), so
        // the memo alone absorbs the vast majority of calls; the map handles the
        // rest. No mutex — the old shared cache took ~98k lock/unlock per
        // converted chunk with every worker contending on it. The map is now
        // bounded by distinct states rather than distinct blocks, which is a
        // few thousand for a real world instead of ~1150.
        struct ThreadCache {
            uint32_t epoch = 0;
            const void* lastState = nullptr;
            MappedBlock lastMapped{ BlockID::Stone, 0 };
            std::unordered_map<const void*, MappedBlock> map;
        };
        thread_local ThreadCache tc;

        const uint32_t epoch = s_blockMapEpoch.load(std::memory_order_acquire);
        if (tc.epoch != epoch) {
            tc.map.clear();
            tc.lastState = nullptr;
            tc.epoch = epoch;
        }

        if (blockState == tc.lastState) {
            return tc.lastMapped;
        }

        auto it = tc.map.find(blockState);
        if (it == tc.map.end()) {
            // First encounter on this thread — resolve via string lookup
            // (slow path, one hit per distinct state per worker thread).
            Game::BlockStateRegistry::Initialize();
            Game::BlockState gameState = Game::BlockStateRegistry::CreateBlockState(
                blockState->getIdentifier(), blockState->getProperties());
            it = tc.map.emplace(blockState,
                                MappedBlock{ gameState.resolvedId, gameState.resolvedState }).first;
        }

        tc.lastState = blockState;
        tc.lastMapped = it->second;
        return it->second;
    }

    uint16_t MyTerrainGenerator::MapBiome(const void* libBiome, const std::string& name) const {
        if (!libBiome) return Game::BiomeRegistry::Fallback();

        struct ThreadCache {
            uint32_t epoch = 0;
            const void* last = nullptr;
            uint16_t lastId = 0;
            std::unordered_map<const void*, uint16_t> map;
        };
        thread_local ThreadCache tc;

        const uint32_t epoch = s_blockMapEpoch.load(std::memory_order_acquire);
        if (tc.epoch != epoch) {
            tc.map.clear();
            tc.last = nullptr;
            tc.epoch = epoch;
        }
        if (libBiome == tc.last) return tc.lastId;

        auto it = tc.map.find(libBiome);
        if (it == tc.map.end()) {
            it = tc.map.emplace(libBiome, Game::BiomeRegistry::FromName(name)).first;
        }
        tc.last = libBiome;
        tc.lastId = it->second;
        return it->second;
    }

    // Try the palette-to-palette path. Returns false when the shapes cannot be
    // lined up, leaving `outSection` untouched for the caller's fallback.
    bool MyTerrainGenerator::TryConvertSectionByPalette(
            const minecraft::world::LevelChunkSection& libSection,
            ChunkSection& outSection, int& outNonAir) const {

        const auto& libStates = libSection.getStates();
        const std::vector<minecraft::world::BlockState*> libPalette =
            libStates.getPaletteEntries();

        // Empty means the library fell back to a GLOBAL palette, where the
        // value is its own index in the library's id space — which is not ours.
        if (libPalette.empty()) return false;

        // Map each DISTINCT state once. Two library states can collapse onto
        // one game state (a property this port does not model); the resulting
        // duplicate palette entries are harmless — both indices resolve to the
        // same value.
        std::vector<uint32_t> values;
        values.reserve(libPalette.size());
        for (auto* st : libPalette) {
            const MappedBlock mapped = MapBlockType(st);
            values.push_back(Game::BlockStateIds::Pack(mapped.id, mapped.state));
        }

        // Unpack the library's indices: pure shift-and-mask over the packed
        // words. No palette lookups, no map lookups, no virtual calls.
        std::vector<uint32_t> indices(ChunkSection::TOTAL, 0);
        const int libBits = libStates.getBitsPerEntry();
        if (libBits > 0) {
            const std::vector<int64_t> raw = libStates.getRawData();
            const int perLong = 64 / libBits;
            const size_t needed =
                static_cast<size_t>((ChunkSection::TOTAL + perLong - 1) / perLong);
            if (raw.size() < needed) return false;

            const uint64_t mask = (1ULL << libBits) - 1ULL;
            for (int i = 0; i < ChunkSection::TOTAL; ++i) {
                const int cell = i / perLong;
                const int bit  = (i - cell * perLong) * libBits;
                indices[i] = static_cast<uint32_t>(
                    (static_cast<uint64_t>(raw[cell]) >> bit) & mask);
            }
        }
        // libBits == 0 is a single-value palette: every index is 0, which
        // `indices` already is.

        Game::PalettedContainer built(
            Game::PaletteStrategy::ForBlockStates(Game::BlockStateIds::Bits()),
            Game::BlockStateIds::Pack(BlockID::Air, 0));
        if (!built.BuildFrom(values, indices)) return false;

        int nonAir = 0;
        for (int i = 0; i < ChunkSection::TOTAL; ++i) {
            if (Game::BlockStateIds::Unpack(values[indices[i]]).id != BlockID::Air) ++nonAir;
        }

        outSection.AdoptStates(std::move(built));
        outNonAir = nonAir;
        return true;
    }

    // Per-voxel conversion. The original path, kept as the fallback AND as the
    // reference the palette path is checked against — see kVerifyPaletteConvert.
    int MyTerrainGenerator::ConvertSectionPerVoxel(
            const minecraft::world::LevelChunkSection& libSection,
            ChunkSection& outSection) const {
        int nonAir = 0;
        for (int ly = 0; ly < 16; ++ly) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const MappedBlock mapped =
                        MapBlockType(libSection.getBlockState(lx, ly, lz));
                    if (mapped.id != BlockID::Air) {
                        outSection.SetBlockState(lx, ly, lz, mapped.id, mapped.state);
                        ++nonAir;
                    }
                }
            }
        }
        return nonAir;
    }

    // Translate ONE library section into a game section.
    //
    // The library stores sections in its own PalettedContainer — a port of the
    // same MC class ours now is — so the two agree on everything except which
    // ids the palette entries carry. That makes the conversion a mapping of the
    // PALETTE (a handful of entries) plus an unpack of the indices, instead of
    // 4096 lookups through MapBlockType and 4096 paletted writes.
    //
    // MC needs none of this: its generator writes into the container the world
    // keeps. This is as close to that as a vendored generator allows.
    int MyTerrainGenerator::ConvertSection(const minecraft::world::LevelChunkSection& libSection,
                                           ChunkSection& outSection) const {
        PROFILE_ZONE_N("ConvertSection");

        int nonAir = 0;
        if (TryConvertSectionByPalette(libSection, outSection, nonAir)) {
            // Cross-check against the path this replaced. terrain_parity cannot
            // cover this — it links the library alone, with no game code — so
            // the palette remap is verified against the per-voxel original
            // instead, over every section of every chunk actually generated.
            //
            // Compiled out entirely by default; flip to true, run a session,
            // and any disagreement is logged with its coordinates.
            if constexpr (kVerifyPaletteConvert) {
                ChunkSection reference;
                const int refNonAir = ConvertSectionPerVoxel(libSection, reference);
                if (refNonAir != nonAir) {
                    Log::Error("[ConvertSection] non-air count differs: palette=%d per-voxel=%d",
                               nonAir, refNonAir);
                }
                for (int ly = 0; ly < 16; ++ly) {
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int lx = 0; lx < 16; ++lx) {
                            if (outSection.Get(lx, ly, lz) != reference.Get(lx, ly, lz) ||
                                outSection.GetState(lx, ly, lz) != reference.GetState(lx, ly, lz)) {
                                Log::Error("[ConvertSection] MISMATCH at (%d,%d,%d): "
                                           "palette=%u/%u per-voxel=%u/%u",
                                           lx, ly, lz,
                                           outSection.Get(lx, ly, lz), outSection.GetState(lx, ly, lz),
                                           reference.Get(lx, ly, lz), reference.GetState(lx, ly, lz));
                                return nonAir;   // one report per section is enough
                            }
                        }
                    }
                }
            }
            return nonAir;
        }

        return ConvertSectionPerVoxel(libSection, outSection);
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::ConvertLibChunk(minecraft::world::IChunk* chunk,
                                                               Math::ChunkPos position,
                                                               int* outBlocksSet) const {
        PROFILE_ZONE_N("ConvertChunk");
        auto gameChunk = std::make_shared<Chunk>();
        gameChunk->pos = position;

        // Section-wise, and PALETTE-WISE — see ConvertSection. All-air sections
        // are skipped entirely (most of a 384-block column is sky).
        //
        // MC has no conversion step at all: its generator writes into the very
        // container the world keeps, so there is nothing to translate. Ours
        // exists only because generation lives in a vendored library with its
        // own palette. Translating palette-to-palette rather than voxel-by-voxel
        // is as close to MC's absence of a conversion as this shape allows.
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

            blocksSet += ConvertSection(libSection, *outSection);
        }

        // ── Biomes ──────────────────────────────────────────────────────────
        // One entry per 4x4x4 cell, matching MC's noise-biome resolution.
        // IChunk exposes getBiome(BlockPos), which is ChunkAccess's own
        // block -> quart conversion (QuartPos::fromBlock, i.e. >> 2) followed by
        // getNoiseBiome — so feeding it the BLOCK coordinate of each cell's
        // corner samples exactly the cell we want to store.
        {
            const int baseX = position.x * Math::CHUNK_SIZE_X;
            const int baseZ = position.z * Math::CHUNK_SIZE_Z;

            for (int qy = 0; qy < Chunk::BIOME_VERTICAL; ++qy) {
                const int blockY = Math::WorldCoordinates::MIN_WORLD_Y + qy * 4;
                for (int qz = 0; qz < Chunk::BIOME_HORIZONTAL; ++qz) {
                    for (int qx = 0; qx < Chunk::BIOME_HORIZONTAL; ++qx) {
                        const auto* biome = chunk->getBiome(minecraft::core::BlockPos(
                            baseX + qx * 4, blockY, baseZ + qz * 4));
                        gameChunk->SetBiomeQuart(
                            qx, qy, qz,
                            MapBiome(biome, biome ? biome->getName() : std::string{}));
                    }
                }
            }
        }

        // ── Heightmaps ──────────────────────────────────────────────────────
        //
        // COPIED from the library rather than recomputed. The library primes
        // MOTION_BLOCKING_NO_LEAVES and WORLD_SURFACE at its generateFeatures
        // stage exactly as MC does, so the values are already there and already
        // correct; a fresh 256-column scan here would cost real time on the
        // chunk pipeline — which CLAUDE.md is explicit is the most expensive
        // thing in the program — to arrive at the same answer.
        //
        // One consequence worth knowing: these heights were computed against
        // the LIBRARY's block predicates, and every later incremental update
        // uses the GAME's (Heightmap.cpp's table). Those can disagree for a
        // block whose type mapping is approximate. The drift is bounded and
        // self-correcting — a column only re-evaluates when something writes to
        // it, and from then on it is consistently the game's predicate — and
        // the library's answer is the more MC-faithful of the two to start from.
        {
            using LibTypes = minecraft::levelgen::Heightmap::Types;

            struct Mapping { HeightmapType game; LibTypes lib; };
            // NOTE the library's enum order differs from MC's, so these are
            // named rather than cast from an index.
            const Mapping mappings[] = {
                { HeightmapType::MotionBlockingNoLeaves, LibTypes::MOTION_BLOCKING_NO_LEAVES },
                { HeightmapType::WorldSurface,           LibTypes::WORLD_SURFACE },
            };

            for (const Mapping& m : mappings) {
                Heightmap& out = gameChunk->GetHeightmap(m.game);
                for (int lx = 0; lx < Math::CHUNK_SIZE_X; ++lx) {
                    for (int lz = 0; lz < Math::CHUNK_SIZE_Z; ++lz) {
                        // getHeight is MC's getHighestTaken (the topmost
                        // matching block); the heightmap stores first-available,
                        // which is one higher.
                        const int height =
                            chunk->getHeight(static_cast<int>(m.lib), lx, lz);
                        out.SetHeight(lx, lz, height + 1);
                    }
                }
            }

            gameChunk->MarkHeightmapsPrimed();
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

    bool MyTerrainGenerator::PumpOneTask() {
        if (!m_initialized || !m_chunkCache) return false;

        // runDistanceManagerUpdates propagates ticket levels, promotes the
        // visible chunk map and dispatches generation tasks. One pass only —
        // the loop belongs to the caller, which owns the deadline.
        if (m_chunkCache->runDistanceManagerUpdates()) {
            return true;
        }

        // Otherwise one generation callback. MC's pollTask falls through to
        // super.pollTask() here in exactly the same way.
        return m_mainThreadExecutor && m_mainThreadExecutor->runOnePendingTask();
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
    void MyTerrainGenerator::SetSeed(int64_t seed) { m_config.seed = seed; }
    int64_t MyTerrainGenerator::GetSeed() const { return m_config.seed; }
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
