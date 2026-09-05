#include <climits>
#include <map>
// File: src/server/world/MyTerrainGenerator.cpp
#include "MyTerrainGenerator.hpp"
#include "storage/SectionDataUnpacker.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <chrono>
#include <future>
#include <stdexcept>   // std::runtime_error — libc++ pulls it in transitively, MSVC does not

// Terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/Heightmap.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/biome/TheEndBiomeSource.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/WorldGenTweaks.h"
#include <nlohmann/json.hpp>

using minecraft::world::level::block::Blocks;
using minecraft::world::BlockRegistry;
using minecraft::BlockState;

// MC DimensionType level heights (data/minecraft/dimension_type/*.json):
// overworld min_y -64 / height 384, the_nether and the_end min_y 0 /
// height 256.
//
// This is the LEVEL height — what the ServerChunkCache sizes its chunks to —
// and it is deliberately NOT the noise height. NETHER_NOISE_SETTINGS is
// (0, 128) inside a 256-tall nether and END_NOISE_SETTINGS is (0, 128) inside
// a 256-tall end (NoiseSettings.cpp:14-15); ChunkGenerator.cpp:715-721 clamps
// the noise range into the level range on purpose, which is what puts the
// nether's carve ceiling at y<=120. Do not "fix" the mismatch.
namespace {
    struct DimensionHeight {
        int minY;
        int height;
    };
    constexpr DimensionHeight OVERWORLD_LEVEL{-64, 384};
    constexpr DimensionHeight NETHER_LEVEL{0, 256};
    constexpr DimensionHeight END_LEVEL{0, 256};
}

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

    // Two dedicated threads for the library's serial schedulers (dispatcher
    // mailbox + worldgen lane) — see ChunkMap's constructor for why they must
    // not share the FIFO worldgen pool.
    BackgroundExecutor& SharedLaneExecutor() {
        static BackgroundExecutor lane(2);
        return lane;
    }

    BackgroundExecutor& SharedBackgroundExecutor() {
        // Leaked on purpose — see the declaration for why it must outlive
        // static destruction rather than race it.
        static BackgroundExecutor* pool = [] {
            auto* p = new BackgroundExecutor();
            Log::Info("[MyTerrainGenerator] Shared worldgen pool started (%zu threads, "
                      "shared by every dimension)",
                      BackgroundExecutor::DefaultThreadCount());
            return p;
        }();
        return *pool;
    }

    MyTerrainGenerator::MyTerrainGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Info("[MyTerrainGenerator] Created for dimension '%s' with seed: %lld",
                  config.dimension.c_str(), static_cast<long long>(config.seed));
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
            //
            // Two INDEPENDENT axes:
            //
            //   dimension  - "overworld" / "nether" / "end" (MC LevelStem).
            //                Selects the noise router, noise settings, biome
            //                source, surface rules, fluid picker, default
            //                block/fluid, sea level, random algorithm and
            //                level height.
            //
            //   world type - MC WorldPresets: "default", "large_biomes",
            //                "amplified", "single_biome_surface", "flat".
            //                Large/amplified differ from default ONLY in the
            //                noise router (NoiseGeneratorSettings.overworld(
            //                ctx, amplified, large)); single-biome swaps the
            //                biome source for a FixedBiomeSource; flat swaps
            //                the whole generator for FlatLevelSource.
            //
            // The presets are all defined against the overworld router/biome
            // source, so they are OVERWORLD ONLY — the parity harness rejects
            // the combination outright ("--world-type is overworld-only",
            // CppChunkGeneratorTest.cpp:963-966). Here the nether/end are
            // constructed by the engine rather than typed by a user, so an
            // accidental combination is forced back to "default" and logged
            // instead of failing world creation.
            // ================================================================
            const bool isNether = (m_config.dimension == "nether");
            const bool isEnd    = (m_config.dimension == "end");
            const bool isOverworld = !isNether && !isEnd;
            if (isOverworld && m_config.dimension != "overworld") {
                Log::Warning("[MyTerrainGenerator] Unknown dimension '%s' — generating"
                             " overworld", m_config.dimension.c_str());
            }
            Log::Info("[MyTerrainGenerator] Dimension: %s",
                      isNether ? "nether" : (isEnd ? "end" : "overworld"));

            if (!isOverworld && !m_config.worldType.empty()
                && m_config.worldType != "default") {
                Log::Warning("[MyTerrainGenerator] World type '%s' is overworld-only —"
                             " ignoring it for the %s", m_config.worldType.c_str(),
                             isNether ? "nether" : "end");
            }
            const std::string worldType = isOverworld ? m_config.worldType : "default";
            const bool isFlat = (worldType == "flat" || worldType == "superflat");
            const bool isAmplified = (worldType == "amplified");
            const bool isLargeBiomes = (worldType == "large_biomes");
            const bool isSingleBiome =
                (worldType == "single_biome" || worldType == "single_biome_surface");
            Log::Info("[MyTerrainGenerator] World type: %s", worldType.c_str());

            // MC DimensionType level height for this dimension. Read the
            // DimensionHeight comment at the top of this file before touching
            // it — the nether/end level height is NOT their noise height.
            const DimensionHeight levelHeight =
                isNether ? NETHER_LEVEL : (isEnd ? END_LEVEL : OVERWORLD_LEVEL);

            // World Properties sandbox tweaks: reset to pure vanilla, then
            // apply the world's JSON BEFORE any biome source / generator
            // construction (the MultiNoise biome filter reads them in its
            // constructor). Empty JSON = untouched vanilla generation.
            //
            // OVERWORLD ONLY, and that is a correctness guard, not a feature
            // decision: WorldGenTweaks::get() is one PROCESS-GLOBAL struct
            // (WorldGenTweaks.h) and reset() overwrites it wholesale. With a
            // generator per dimension, letting the nether initialize second
            // would wipe the overworld's tweaks — silently, with no crash, and
            // presenting to the player as "my amplified/no-caves world stopped
            // being amplified" long after world creation. Making the overworld
            // generator the single writer removes the hazard entirely and does
            // not depend on which dimension initializes first (a std::once_flag
            // would, and would hand the tweaks to whichever dimension happened
            // to come up first). The tweaks are a whole-world property and
            // still apply to every dimension: this only controls who WRITES
            // them.
            if (isOverworld) {
                minecraft::levelgen::WorldGenTweaks::reset();
                if (!m_config.worldgenTweaks.empty()) {
                    try {
                        auto& tweaks = minecraft::levelgen::WorldGenTweaks::get();
                        nlohmann::json tj = nlohmann::json::parse(m_config.worldgenTweaks);
                        tweaks.carversEnabled = tj.value("caves", true);
                        if (tj.contains("steps") && tj["steps"].is_array()) {
                            for (size_t i = 0; i < tweaks.featureStepEnabled.size()
                                               && i < tj["steps"].size(); ++i) {
                                tweaks.featureStepEnabled[i] = tj["steps"][i].get<bool>();
                            }
                        }
                        tweaks.featureDensity     = tj.value("featureDensity", 1.0f);
                        tweaks.oreDensity         = tj.value("oreDensity", 1.0f);
                        tweaks.vegetationDensity  = tj.value("vegetationDensity", 1.0f);
                        tweaks.structureFrequency = tj.value("structureFrequency", 1.0f);
                        for (const auto& b : tj.value("disabledBiomes",
                                                      nlohmann::json::array())) {
                            tweaks.disabledBiomes.insert(b.get<std::string>());
                        }
                        if (!tweaks.isDefault()) {
                            Log::Info("[MyTerrainGenerator] World Properties tweaks ACTIVE"
                                      " (non-vanilla generation)");
                        }
                    } catch (const std::exception& e) {
                        Log::Warning("[MyTerrainGenerator] Bad worldgenTweaks JSON (%s)"
                                     " - using vanilla generation", e.what());
                        minecraft::levelgen::WorldGenTweaks::reset();
                    }
                }
            }

            // ---- Router + biome source + noise settings, per dimension.
            // Same construction order as the parity harness
            // (CppChunkGeneratorTest.cpp:425-459), which builds the biome
            // source up here with the router rather than down beside the
            // generator: it keeps everything the dimension selects in one
            // block, and the End's source needs the seed, not the settings.
            minecraft::levelgen::NoiseRouter* router = nullptr;
            minecraft::levelgen::NoiseSettings noiseSettings =
                minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
            if (isNether) {
                // NoiseRouterData.nether() = noNewCaves(slideNetherLike(0,128)).
                router = minecraft::levelgen::NoiseRouterData::nether();
                m_biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createNether();
                // NoiseSettings.cpp:14 — NETHER_NOISE_SETTINGS(0, 128, 1, 2).
                noiseSettings = minecraft::levelgen::NoiseSettings::NETHER_NOISE_SETTINGS;
            } else if (isEnd) {
                router = minecraft::levelgen::NoiseRouterData::end();
                // TheEndBiomeSource is the one biome source that takes the
                // seed directly — the End's island layout is simplex noise
                // over the world seed, not a climate lookup.
                m_biomeSource =
                    std::make_unique<minecraft::world::biome::TheEndBiomeSource>(seed);
                // NoiseSettings.cpp:15 — END_NOISE_SETTINGS(0, 128, 2, 1).
                noiseSettings = minecraft::levelgen::NoiseSettings::END_NOISE_SETTINGS;
            } else {
                // NoiseRouterData::overworld(largeBiomes, amplified) - for flat
                // this feeds only the RandomState (Java uses dummy(); nothing in a
                // flat world samples the router).
                router = minecraft::levelgen::NoiseRouterData::overworld(
                    isLargeBiomes, isAmplified);
                if (isSingleBiome) {
                    // Reference: WorldPresets SINGLE_BIOME_SURFACE -
                    // FixedBiomeSource + normal overworld noise settings.
                    std::string biome = m_config.singleBiome.empty()
                        ? "minecraft:plains" : m_config.singleBiome;
                    if (biome.find(':') == std::string::npos) biome = "minecraft:" + biome;
                    if (!minecraft::data::worldgen::BiomeFeatureRegistry::isKnownBiomeKey(biome)) {
                        Log::Warning("[MyTerrainGenerator] Unknown single biome '%s', using plains",
                                     biome.c_str());
                        biome = "minecraft:plains";
                    }
                    Log::Info("[MyTerrainGenerator] Single biome: %s", biome.c_str());
                    m_biomeSource =
                        std::make_unique<minecraft::world::biome::FixedBiomeSource>(biome);
                } else if (!isFlat) {
                    m_biomeSource =
                        minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();
                }
                // flat: FlatLevelSource owns its own FixedBiomeSource, so
                // m_biomeSource stays null (matches harness:454-459).
            }

            // Spawn-target climate list (MC NoiseGeneratorSettings.overworld()
            // passes OverworldBiomeBuilder.spawnTarget(); nether.json and
            // end.json carry NO spawn target). RandomState hands this to the
            // Climate::Sampler, which is what makes Sampler::findSpawnPosition()
            // work — with an empty list it just returns the origin, which is
            // the correct behaviour for dimensions MC never climate-searches.
            // The settings API wants opaque ClimateParameterPoint*, so keep
            // value storage here and pass pointers (same reinterpret pattern
            // RandomState uses to read them back).
            //
            // Deliberately NOT m_biomeSource->getSpawnTarget(), even though
            // that accessor exists and would give the same three answers: for
            // the overworld it would ALSO change flat (null source) and
            // single_biome (FixedBiomeSource -> empty list) worlds, which today
            // carry the overworld list on purpose — FindSpawnPosition below
            // documents and relies on that.
            m_spawnTargetStorage = isOverworld
                ? minecraft::world::biome::OverworldBiomeBuilder().spawnTarget()
                : std::vector<minecraft::world::biome::Climate::ParameterPoint>{};
            std::vector<minecraft::levelgen::ClimateParameterPoint*> spawnTargetPtrs;
            spawnTargetPtrs.reserve(m_spawnTargetStorage.size());
            for (auto& point : m_spawnTargetStorage) {
                spawnTargetPtrs.push_back(
                    reinterpret_cast<minecraft::levelgen::ClimateParameterPoint*>(&point));
            }

            // ---- NoiseGeneratorSettings, per dimension. Argument order is
            // (noiseSettings, defaultBlock, defaultFluid, router, surfaceRule,
            // spawnTarget, seaLevel, disableMobGeneration, aquifersEnabled,
            // oreVeinsEnabled, useLegacyRandomSource) — NoiseGeneratorSettings.h:29.
            //
            // THE DEFAULT BLOCK IS LOAD-BEARING BEYOND THE FILL COLOUR. The
            // library has no dimension enum; it infers the dimension from this
            // string. ChunkGenerator.cpp:751-753 picks the nether carvers when
            // it reads "minecraft:netherrack", and ChunkStatusTasks.h:457-509
            // picks the nether/end featuresPerStep from "minecraft:netherrack"
            // / "minecraft:end_stone" — and featuresPerStep drives
            // setFeatureSeed, so the wrong one silently reseeds every feature.
            // Pass the wrong block and you get overworld caves and overworld
            // feature RNG in the nether, with no error anywhere.
            if (isNether) {
                // nether.json: netherrack/lava, sea_level 32, aquifers OFF,
                // ore veins OFF, legacy_random_source TRUE (LEGACY = Java LCG,
                // RandomAlgorithm in NoiseGeneratorSettings.h:18-21).
                // `auto*`, not `BlockState*`: this file is inside namespace
                // Game, where an unqualified BlockState is GAME's 32-bit state
                // handle rather than the terrain library's pointer type. The
                // two are unrelated and the mistake reads as correct.
                auto* netherrack = Blocks::getDefaultState("minecraft:netherrack");
                if (!netherrack) {
                    // Blocks::getDefaultState returns null for an unregistered
                    // id; a null default block would fall through as "not the
                    // nether" in both dimension tests above.
                    throw std::runtime_error(
                        "minecraft:netherrack missing from the block registry");
                }
                m_settings = new minecraft::levelgen::NoiseGeneratorSettings(
                    noiseSettings,
                    netherrack,
                    Blocks::LAVA->defaultBlockState(),
                    *router, nullptr, spawnTargetPtrs, 32, false, false, false, true
                );
            } else if (isEnd) {
                // end.json: end_stone/air, sea_level 0, aquifers OFF, ore veins
                // OFF, legacy_random_source TRUE. The "fluid" really is air —
                // the End has no sea.
                auto* endStone = Blocks::getDefaultState("minecraft:end_stone");   // see the note above
                if (!endStone) {
                    throw std::runtime_error(
                        "minecraft:end_stone missing from the block registry");
                }
                m_settings = new minecraft::levelgen::NoiseGeneratorSettings(
                    noiseSettings,
                    endStone,
                    Blocks::AIR->defaultBlockState(),
                    *router, nullptr, spawnTargetPtrs, 0, false, false, false, true
                );
            } else {
                m_settings = new minecraft::levelgen::NoiseGeneratorSettings(
                    noiseSettings,
                    Blocks::STONE->defaultBlockState(),
                    Blocks::WATER->defaultBlockState(),
                    *router, nullptr, spawnTargetPtrs, 63, false, true, true, false
                );
            }

            m_randomState = minecraft::levelgen::RandomState::create(m_settings, seed);

            // ---- Surface rules + fluid picker, per dimension.
            minecraft::levelgen::RuleSource* surfaceRules = nullptr;
            if (isNether) {
                surfaceRules = minecraft::levelgen::SurfaceRuleData::nether();
                // Reference: NoiseBasedChunkGenerator's nether lava picker —
                // a lava "sea" at the nether sea level (32), not water.
                m_fluidPicker = new minecraft::levelgen::SeaLevelFluidPicker(
                    32, Blocks::LAVA->defaultBlockState());
            } else if (isEnd) {
                surfaceRules = minecraft::levelgen::SurfaceRuleData::end();
                // Sea level 0 with air as the fluid: nothing is ever flooded.
                m_fluidPicker = new minecraft::levelgen::SeaLevelFluidPicker(
                    0, Blocks::AIR->defaultBlockState());
            } else {
                surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();
                m_fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
                    63, -54,
                    Blocks::WATER->defaultBlockState(),
                    Blocks::LAVA->defaultBlockState()
                );
            }

            if (isFlat) {
                // Reference: WorldPresets FLAT - FlatLevelSource with the
                // selected preset (default = FlatLevelGeneratorSettings.
                // getDefault()), optionally overridden by a custom
                // "<layers>;<biome>" string (PresetFlatWorldScreen format).
                minecraft::levelgen::flat::FlatLevelGeneratorSettings flatSettings =
                    m_config.flatPreset.empty()
                        ? minecraft::levelgen::flat::FlatLevelGeneratorSettings::getDefault()
                        : minecraft::levelgen::flat::FlatLevelGeneratorSettings::preset(
                              m_config.flatPreset);
                if (!m_config.flatLayers.empty()) {
                    flatSettings = minecraft::levelgen::flat::FlatLevelGeneratorSettings::
                        fromString(m_config.flatLayers, flatSettings);
                }
                Log::Info("[MyTerrainGenerator] Flat settings: %s",
                          flatSettings.toString().c_str());
                auto* flatGenerator =
                    new minecraft::levelgen::FlatLevelSource(std::move(flatSettings));
                flatGenerator->setLevelHeightRange(levelHeight.minY, levelHeight.height);
                m_generator = flatGenerator;
            } else {
                // The generator's fill block is the dimension's default block —
                // netherrack / end_stone / stone (harness:542-543). Not
                // m_stoneBlock: that would fill the nether and the end with
                // overworld stone.
                auto* noiseGenerator = new minecraft::levelgen::NoiseBasedChunkGenerator(
                    m_settings, m_randomState->surfaceSystem(), surfaceRules,
                    m_settings->defaultBlock(), m_airBlock, m_fluidPicker, nullptr
                );
                noiseGenerator->setBiomeSource(m_biomeSource.get());
                m_generator = noiseGenerator;
            }
            Log::Info("[MyTerrainGenerator] World generation components created");

            // ================================================================
            // Step 5: Create executors (lease on the shared pool + main thread
            // queue). The pool itself is process-wide and shared by every
            // dimension — see SharedBackgroundExecutor in the header.
            // ================================================================
            m_backgroundLease = std::make_unique<SharedExecutorLease>();
            if (const char* deco = std::getenv("OBEY_DECO_THREADS")) {
                const int n = std::atoi(deco);
                if (n > 0) m_decorationPool = std::make_unique<BackgroundExecutor>(static_cast<size_t>(n), /*elevated=*/true);
            }
            m_mainThreadExecutor = std::make_unique<MainThreadExecutor>();
            {
                MainThreadExecutor* exec = m_mainThreadExecutor.get();
                std::lock_guard<std::mutex> lock(m_sink->mutex);
                m_sink->closed = false;
                m_sink->wake = [exec]() { exec->wakeAll(); };
            }
            Log::Info("[MyTerrainGenerator] Executors created (leased the shared %zu-thread"
                      " worldgen pool)", BackgroundExecutor::DefaultThreadCount());

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
                m_backgroundLease->getExecutor(),
                m_mainThreadExecutor->getExecutor(),
                nullptr,   // lane executor: share the pool (a dedicated one measured no gain and complicated shutdown)
                m_blockRegistry,
                m_airBlock,
                m_stoneBlock,
                // LEVEL height for this dimension (harness:1105-1106 with
                // --dimension, harness:922-933). 256-tall nether/end even
                // though their noise settings are 128 tall.
                levelHeight.minY,
                levelHeight.height,
                m_config.storagePath);

            if (m_decorationPool) m_chunkCache->getChunkMap().worldGenContextMutable().decorationExecutor = m_decorationPool->getExecutor();
            m_chunkCache->setTaskPoller([this]() {
                if (m_mainThreadExecutor->hasPendingTasks()) {
                    m_mainThreadExecutor->runPendingTasks();
                }
            });
            Log::Info("[MyTerrainGenerator] ServerChunkCache created");
            {   // Diagnostics: accumulated radii of both pyramids for a FULL target.
                using minecraft::world::chunk::status::ChunkPyramid;
                using minecraft::world::chunk::status::ChunkStatus;
                std::string gen, load;
                for (const auto* st : ChunkStatus::getStatusList()) {
                    gen  += st->getName() + "=" + std::to_string(ChunkPyramid::getGenerationPyramid().getStepTo(ChunkStatus::FULL).getAccumulatedRadiusOf(*st)) + " ";
                    load += st->getName() + "=" + std::to_string(ChunkPyramid::getLoadingPyramid().getStepTo(ChunkStatus::FULL).getAccumulatedRadiusOf(*st)) + " ";
                }
                Log::Info("[LibDiag] generation pyramid radii (FULL): %s", gen.c_str());
                Log::Info("[LibDiag] loading pyramid radii (FULL):    %s", load.c_str());
            }

            // ================================================================
            // Step 6.5: Structure generation (MC WorldOptions.generateStructures)
            //
            // Mirrors MC ChunkStatusTasks.generateStructureStarts: when the
            // world option is off, no structure state exists and the starts
            // task is a no-op — references and the decoration structure pass
            // then naturally do nothing. When on, build the placement state
            // (structure_set/structure JSONs from data/) and inject it into
            // the pipeline's WorldGenContext, exactly like the parity harness.
            // ================================================================
            if (m_config.generateStructures) {
                if (auto* flatGenerator =
                        dynamic_cast<minecraft::levelgen::FlatLevelSource*>(m_generator)) {
                    // Reference: FlatLevelSource.createState - the flat
                    // settings' structure_overrides (or ALL sets when none),
                    // via createForFlat (concentricRingsSeed = 0).
                    std::vector<const minecraft::levelgen::structure::StructureSet*> sets;
                    const auto& overrides = flatGenerator->settings().structureOverrides();
                    if (overrides.has_value()) {
                        for (const std::string& setName : *overrides) {
                            sets.push_back(&minecraft::levelgen::structure::StructureSets::byName(setName));
                        }
                    } else {
                        sets = minecraft::levelgen::structure::StructureSets::all();
                    }
                    m_structureState = std::make_unique<
                        minecraft::levelgen::structure::ChunkGeneratorStructureState>(
                        minecraft::levelgen::structure::ChunkGeneratorStructureState::createForFlat(
                            m_randomState->sampler(), seed, flatGenerator->biomeSource(), sets));
                } else {
                    m_structureState = std::make_unique<
                        minecraft::levelgen::structure::ChunkGeneratorStructureState>(
                        minecraft::levelgen::structure::ChunkGeneratorStructureState::createForNormal(
                            m_randomState->sampler(), seed, m_biomeSource.get(),
                            minecraft::levelgen::structure::StructureSets::all()));
                }
                // Stronghold ring positions: start now, on the terrain pool,
                // exactly as Java's supplyAsync tasks do — measured 2.2 s of
                // serial lane time at the first structure step otherwise.
                m_structureState->startRingGeneration(m_backgroundLease->getExecutor());
                m_chunkCache->getChunkMap().worldGenContextMutable().structureState =
                    m_structureState.get();
                // Gates the structure pass in applyBiomeDecoration (Java:
                // structureManager.shouldGenerateStructures()).
                m_generator->setGenerateStructures(true);
                Log::Info("[MyTerrainGenerator] Structures ENABLED (%zu structure sets)",
                          m_structureState->possibleStructureSets().size());
            } else {
                Log::Info("[MyTerrainGenerator] Structures DISABLED (world option)");
            }

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

    bool MyTerrainGenerator::IsAbortRequested() const {
        return m_chunkCache && m_chunkCache->isAbortRequested();
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
        // Orphan the completion sink first: futures may still complete on
        // pool threads while the executors below are torn down.
        {
            std::lock_guard<std::mutex> lock(m_sink->mutex);
            m_sink->closed = true;
            m_sink->wake = nullptr;
            m_sink->completions.clear();
        }
        if (!m_initialized) return;

        Log::Info("[MyTerrainGenerator] Shutting down...");

        // Fence this generator's background work FIRST so no task references
        // destroyed objects. The shared pool is NOT stopped — the other
        // dimensions are still running on it; closeAndWait blocks until every
        // task submitted through THIS lease has finished and drops anything
        // submitted afterwards, which is exactly what the old
        // per-generator `m_backgroundExecutor.reset()` did.
        if (m_backgroundLease) {
            m_backgroundLease->closeAndWait();
        }
        m_mainThreadExecutor.reset();
        m_chunkCache.reset();
        // After m_chunkCache: its executor closure holds the lease pointer.
        m_backgroundLease.reset();
        // After m_chunkCache: the pipeline's WorldGenContext pointed at this.
        m_structureState.reset();

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
        // Overworld-only algorithm, and NOTHING SHOULD CALL IT on the other two
        // dimensions: MC's setInitialSpawn runs against the overworld LevelStem
        // alone, and arrival elsewhere is by portal (nether) or onto the fixed
        // obsidian platform (end), neither of which asks the generator. Both
        // steps below would be wrong there anyway — the climate search needs a
        // spawn target these dimensions do not carry (Initialize gives them an
        // empty list, so it would just return the origin), and "worldgen
        // surface above sea level" means nothing in a nether whose sea is lava
        // at y=32 or an End whose sea level is 0.
        //
        // Answered before the lazy Initialize below: there is no reason to
        // stand a whole pipeline up to return a constant.
        if (m_config.dimension == "nether") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the nether —"
                         " world spawn is an overworld concept; returning a placeholder");
            // No MC counterpart to copy (the nether is entered by portal);
            // mid-height above the lava sea is the least harmful constant.
            return glm::ivec3(0, 64, 0);
        }
        if (m_config.dimension == "end") {
            Log::Warning("[MyTerrainGenerator] FindSpawnPosition called on the end —"
                         " world spawn is an overworld concept; returning the"
                         " obsidian-platform point");
            // ServerLevel.java:188 — END_SPAWN_POINT = BlockPos(100, 50, 0).
            return glm::ivec3(100, 50, 0);
        }

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
        // Flat worlds: MC's RandomState there is built from
        // NoiseGeneratorSettings.dummy() whose spawnTarget is EMPTY, so the
        // climate search degenerates to the origin. Ours carries the
        // overworld spawn target (shared construction), so skip it here.
        const bool flatWorld =
            dynamic_cast<minecraft::levelgen::FlatLevelSource*>(m_generator) != nullptr;
        const auto climatePos = flatWorld
            ? minecraft::core::BlockPos(0, 0, 0)
            : m_randomState->sampler()->findSpawnPosition();
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
            Game::NbtBlockState gameState = Game::BlockStateRegistry::CreateBlockState(
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
            values.push_back(Game::BlockStates::FromIndex(mapped.id, mapped.state).RawId());
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
            Game::PaletteStrategy::ForBlockStates(Game::kBlockStateBits),
            Game::BlockState{}.RawId());
        if (!built.BuildFrom(values, indices)) return false;

        int nonAir = 0;
        for (int i = 0; i < ChunkSection::TOTAL; ++i) {
            if (Game::BlockState::FromRawId(values[indices[i]]).Block() != BlockID::Air) ++nonAir;
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

            // Sections are always allocated now (MC replaceMissingSections),
            // so this is a plain fetch. It used to EnsureSection first — and
            // that call, plus the one in SetBiomeQuart below, is why a
            // generated chunk already carried all 24 sections before this
            // change.
            blocksSet += ConvertSection(libSection,
                                        *gameChunk->GetSection(gameSectionIndex));
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
                // Not consumed by the engine, but vanilla reads them out of a
                // saved chunk and primes only the ones that are ABSENT — so a
                // key we write zero-filled would be believed. The library
                // computes all four, so copying them is free.
                { HeightmapType::OceanFloor,             LibTypes::OCEAN_FLOOR },
                { HeightmapType::MotionBlocking,         LibTypes::MOTION_BLOCKING },
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
        // loadOrGenerate=false: NO per-chunk UNKNOWN ticket. The viewer's
        // radius ticket (SetViewTicket) already holds the area at the right
        // levels, exactly as MC's player tickets do; we only attach to the
        // holder's FULL future. Per-chunk one-tick tickets meant thousands
        // of expiries per tick — a level-change/resort storm through the
        // dispatcher that left tasks stranded (measured 2026-08-30).
        // loadOrGenerate=true: a per-chunk UNKNOWN ticket at level 33 (FULL).
        // This is what the old blocking path did and it measured fastest. A
        // radius "view ticket" (tried 2026-08-30) puts near chunks at levels
        // <= 32, which makes the library schedule block/entity-ticking
        // promotions — an extra task pyramid for every chunk in view — and
        // fresh generation dropped ~12-25%. Tickets are never purged while
        // library unloading is disabled, so the holder stays at FULL.
        auto future = m_chunkCache->getChunkFuture(
            position.x, position.z, *m_targetStatus, true
        );
        if (!future) return false;
        {
            // Already resolved (holder missing or below the level the view
            // ticket gives it): report failure now so the caller retries
            // next tick once the distance manager has propagated levels.
            auto now = future->getNow(nullptr);
            if (now && !now->isSuccess()) return false;
        }
        // Pinned from request until the game has converted (or dropped) the
        // result, so processUnloads cannot destroy the holder while a raw
        // chunk pointer is on its way to us.
        PinConversion(position);
        std::shared_ptr<CompletionSink> sink = m_sink;
        future->thenAccept([sink, position](
                const minecraft::server::level::ServerChunkCache::ChunkResultType& result) {
            minecraft::world::IChunk* chunk = result ? result->orElse(nullptr) : nullptr;
            {
                std::lock_guard<std::mutex> lock(sink->mutex);
                if (sink->closed) return;
                sink->completions.push_back(Completion{position, chunk});
            }
            if (sink->wake) sink->wake();
        });
        return true;
    }

    void MyTerrainGenerator::EnqueueGenerationRequest(Math::ChunkPos position) {
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_requests.push_back(position);
        }
        // The server thread parks in WaitForPipelineWork between ticks;
        // give it a reason to service the queue now rather than next tick.
        if (m_mainThreadExecutor) m_mainThreadExecutor->wakeAll();
    }

    void MyTerrainGenerator::TakeRequests(std::vector<Math::ChunkPos>& out) {
        // Make sure the view ticket added this tick has produced holders at
        // their levels before the requests below attach to them.
        if (m_chunkCache) m_chunkCache->runDistanceManagerUpdates();
        std::lock_guard<std::mutex> lock(m_requestMutex);
        out.insert(out.end(), m_requests.begin(), m_requests.end());
        m_requests.clear();
    }

    void MyTerrainGenerator::TakeCompletions(std::vector<Completion>& out) {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        out.insert(out.end(), m_sink->completions.begin(), m_sink->completions.end());
        m_sink->completions.clear();
    }

    std::shared_ptr<Chunk> MyTerrainGenerator::ConvertCompletedChunk(minecraft::world::IChunk* chunk,
                                                                     Math::ChunkPos position) {
        if (!chunk) return nullptr;
        PROFILE_ZONE_N("ConvertCompleted");
        auto gameChunk = ConvertLibChunk(chunk, position, nullptr);
        m_stats.chunksGenerated++;
        return gameChunk;
    }

    void MyTerrainGenerator::SetViewTicket(uint32_t viewerId, Math::ChunkPos center, int radius) {
        if (!m_initialized || !m_chunkCache) return;
        using minecraft::server::level::TicketType;
        auto it = m_viewTickets.find(viewerId);
        if (it != m_viewTickets.end()) {
            if (it->second.center == center && it->second.radius == radius) return;
            m_chunkCache->removeTicketWithRadius(TicketType::PLAYER_LOADING,
                minecraft::world::ChunkPos(it->second.center.x, it->second.center.z), it->second.radius);
        }
        m_chunkCache->addTicketWithRadius(TicketType::PLAYER_LOADING,
            minecraft::world::ChunkPos(center.x, center.z), radius);
        m_viewTickets[viewerId] = ViewTicket{center, radius};
    }

    void MyTerrainGenerator::ClearViewTicket(uint32_t viewerId) {
        if (!m_initialized || !m_chunkCache) return;
        auto it = m_viewTickets.find(viewerId);
        if (it == m_viewTickets.end()) return;
        m_chunkCache->removeTicketWithRadius(minecraft::server::level::TicketType::PLAYER_LOADING,
            minecraft::world::ChunkPos(it->second.center.x, it->second.center.z), it->second.radius);
        m_viewTickets.erase(it);
    }

    void MyTerrainGenerator::PinConversion(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        m_pinned.insert(position);
    }
    void MyTerrainGenerator::UnpinConversion(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        m_pinned.erase(position);
    }
    bool MyTerrainGenerator::IsConversionPinned(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_pinMutex);
        return m_pinned.count(position) != 0;
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

    // WHY THIS EXISTS. The library never unloads: every UNKNOWN ticket lives
    // forever (nothing calls ServerChunkCache::tick), so every holder it ever
    // created stays. A holder that reached FULL already dropped its NoiseChunk
    // (ChunkStatusTasks::full), but the ring of chunks around each visited
    // area that only reached BIOMES/NOISE/SURFACE/CARVERS keeps one — and a
    // NoiseChunk is the per-chunk copy of the whole density-function DAG plus
    // its caches, ~600 KB. Measured 2026-08-29 (`heap` at the RSS peak): 3.9 GB
    // of malloc, dominated by 5.1M DensityFunctions::Marker, 1.9M MulOrAdd,
    // 1M ShiftedNoise ... all live NoiseChunks of ~5,500 ring chunks after
    // two areas; the process hit 5.6 GB and stalled for 25 s paging.
    //
    // The NoiseChunk is a CACHE (ProtoChunk::getOrCreateNoiseChunk recreates
    // it deterministically from seed and position, as MC's lazy supplier
    // does), so dropping it when nothing is using the chunk loses nothing but
    // ~10 ms if that ring chunk is ever generated further. "Nothing using it"
    // is generationRefCount()==0: every ChunkGenerationTask acquires every
    // holder in its dependency radius for its whole life, so zero means no
    // step is running on, or reading, this chunk. Tasks are only created on
    // this thread (runGenerationTasks), so a zero seen here stays zero until
    // we schedule more.
    //
    // The MC-faithful fix is real unloading (ticket expiry + ChunkMap
    // processUnloads); this is the contained version.
    size_t MyTerrainGenerator::TickLibrary(std::chrono::steady_clock::time_point deadline) {
        if (!m_initialized || !m_chunkCache) return 0;
        PROFILE_ZONE_N("Lib.Tick");
        auto haveTime = [deadline]() { return std::chrono::steady_clock::now() < deadline; };
        // Java runs purgeStaleTickets + runDistanceManagerUpdates every tick;
        // that is what turns a removed view ticket into unload candidates.
        m_chunkCache->tick(haveTime, /*tickChunks=*/false);
        return m_chunkCache->getChunkMap().processUnloads(haveTime, [this](int64_t key) {
            const minecraft::world::ChunkPos pos(key);
            return !IsConversionPinned(Math::ChunkPos{pos.x(), pos.z()});
        });
    }

    MyTerrainGenerator::UnloadDiag MyTerrainGenerator::GetUnloadDiag() const {
        UnloadDiag d;
        if (!m_initialized || !m_chunkCache) return d;
        const int maxLevel = minecraft::server::level::ChunkLevel::getMaxLevel();
        auto& map = const_cast<minecraft::server::level::ChunkMap&>(m_chunkCache->getChunkMap());
        map.forEachHolder([&](minecraft::server::level::ChunkHolder& h) {
            if (h.getTicketLevel() > maxLevel) ++d.aboveMax;
            if (h.generationRefCount() != 0) ++d.refHeld;
        });
        d.pendingUnload = map.pendingUnloadCount();
        std::vector<Math::ChunkPos> sample;
        {
            std::lock_guard<std::mutex> lock(m_pinMutex);
            d.pinned = m_pinned.size();
            for (const auto& p : m_pinned) { sample.push_back(p); if (sample.size() >= 4) break; }
        }
        for (const auto& p : sample) {
            auto* h = map.getUpdatingChunkIfPresent(minecraft::world::ChunkPos::asLong(p.x, p.z));
            Log::Info("[LibDiag] pinned (%d,%d): %s", p.x, p.z, h ? h->debugState().c_str() : "no holder");
            // Follow the wait chain up to 6 hops.
            for (int hop = 0; h && hop < 6; ++hop) {
                auto w = h->debugWaitingOn();
                if (w.second < 0) break;
                auto* next = map.getUpdatingChunkIfPresent(w.first);
                minecraft::world::ChunkPos wp(w.first);
                Log::Info("[LibDiag]   -> waits on (%d,%d)@%d: %s", wp.x(), wp.z(), w.second,
                          next ? next->debugState().c_str() : "NO HOLDER");
                if (next == h) break;
                h = next;
            }
        }
        {
            // Holder histogram by latest status, and by ticket level band.
            std::map<std::string, int> byStatus; int refHeldNoTask = 0, withTask = 0, taskNeverRan = 0;
            map.forEachHolder([&](minecraft::server::level::ChunkHolder& hh) {
                const auto* st = hh.getLatestStatus();
                byStatus[st ? st->getName() : "none"]++;
                const int runs = hh.debugTaskRuns();
                if (runs >= 0) { ++withTask; if (runs == 0) ++taskNeverRan; }
                else if (hh.generationRefCount() != 0) ++refHeldNoTask;
            });
            std::string hist;
            for (auto& [k, v] : byStatus) hist += k + "=" + std::to_string(v) + " ";
            Log::Info("[LibDiag] pool probe (spin ms / cpu:wall):%s", SharedBackgroundExecutor().DebugProbe().c_str());
            Log::Info("[LibDiag] holders by status: %s | withTask=%d taskNeverRan=%d refHeldNoTask=%d claimRetries=%zu claimWaiters=%zu",
                      hist.c_str(), withTask, taskNeverRan, refHeldNoTask,
                      map.featureClaimRetries(), map.featureClaimWaiters());
        }
        {
            auto st = map.worldgenDispatcherStats();
            Log::Info("[LibDiag] hot tasks:%s", map.debugHotTasks(4).c_str());
            Log::Info("[LibDiag] worldgen dispatcher: submitted=%zu popped=%zu executed=%zu polls=%zu hasWork=%d sleeping=%d stranded=%zu pendingGenTasks=%zu",
                      st.submitted, st.popped, st.executed, st.polls, (int)st.hasWork, (int)st.sleeping, st.stranded,
                      map.pendingGenerationTaskCount());
        }
        return d;
    }

    size_t MyTerrainGenerator::ReleaseIdleNoiseChunks(std::chrono::steady_clock::time_point deadline) {
        if (!m_initialized || !m_chunkCache) return 0;
        using minecraft::server::level::ChunkHolder;
        using minecraft::world::ProtoChunk;
        auto& map = m_chunkCache->getChunkMap();

        if (m_noiseReleaseQueue.empty()) {
            if (++m_noiseReleaseRescan < 100) return 0;
            m_noiseReleaseRescan = 0;
            constexpr int kIdleScansRequired = 3;   // ~15 s quiet
            std::unordered_map<int64_t, int> next;
            map.forEachHolder([&](ChunkHolder& holder) {
                if (holder.generationRefCount() != 0) return;
                auto* proto = dynamic_cast<ProtoChunk*>(holder.getLatestChunk());
                if (!proto || proto->getNoiseChunk() == nullptr) return;
                const int64_t key = holder.getPos().toLong();
                auto it = m_noiseIdleScans.find(key);
                const int scans = (it == m_noiseIdleScans.end() ? 0 : it->second) + 1;
                if (scans >= kIdleScansRequired) m_noiseReleaseQueue.push_back(key);
                else next[key] = scans;
            });
            m_noiseIdleScans.swap(next);
            if (m_noiseReleaseQueue.empty()) return 0;
        }

        // MC processUnloads: while (floor > 0 || haveTime()) — a floor so an
        // already-late tick still drains something, the deadline for the rest.
        constexpr size_t kFloor = 4;
        size_t released = 0;
        while (!m_noiseReleaseQueue.empty() &&
               (released < kFloor || std::chrono::steady_clock::now() < deadline)) {
            const int64_t key = m_noiseReleaseQueue.back();
            m_noiseReleaseQueue.pop_back();
            ChunkHolder* holder = map.getUpdatingChunkIfPresent(key);
            if (!holder || holder->generationRefCount() != 0) continue;   // re-check: may be busy again
            auto* proto = dynamic_cast<ProtoChunk*>(holder->getLatestChunk());
            if (!proto || proto->getNoiseChunk() == nullptr) continue;
            proto->setNoiseChunk(nullptr);
            ++released;
        }
        return released;
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
        // Everything the dimension selects — router, biome source, settings,
        // surface rules, level height — is baked in at Initialize, so a late
        // change cannot take effect. Keep the field describing what this
        // generator ACTUALLY produces: FindSpawnPosition and the logs read it,
        // and letting it drift from the live pipeline is worse than ignoring
        // the write.
        const std::string activeDimension = m_config.dimension;
        m_config = config;
        if (m_initialized && m_config.dimension != activeDimension) {
            Log::Warning("[MyTerrainGenerator] Dimension changed after initialization"
                         " ('%s' -> '%s') - ignored, build a new generator instead",
                         activeDimension.c_str(), m_config.dimension.c_str());
            m_config.dimension = activeDimension;
        }
        if (m_initialized && config.seed != m_config.seed) {
            Log::Warning("[MyTerrainGenerator] Seed changed after initialization - requires restart");
        }
    }

    GenerationConfig MyTerrainGenerator::GetConfig() const { return m_config; }
    void MyTerrainGenerator::SetSeed(int64_t seed) { m_config.seed = seed; }
    int64_t MyTerrainGenerator::GetSeed() const { return m_config.seed; }
    void MyTerrainGenerator::SetWorldType(const std::string&) {}
    // NOT the dimension — IChunkGenerator's "world type" is the MC WorldPreset
    // (default/flat/amplified/...). The literal is historical and no caller
    // reads it (the only other implementation is ProceduralChunkGenerator);
    // m_config.worldType and m_config.dimension are the live values.
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

    int MyTerrainGenerator::SurfaceHeightAt(int blockX, int blockZ) const {
        if (!m_generator || !m_randomState) return INT_MIN;
        return m_generator->getBaseHeight(blockX, blockZ,
            minecraft::levelgen::Heightmap::Types::WORLD_SURFACE_WG, m_randomState);
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
