#pragma once
#include <cstdio>
#include <cstdlib>

#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/IChunk.h"
#include "world/ProtoChunk.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/GenerationStep.h"
#include "levelgen/RandomState.h"
#include "levelgen/FeatureSorter.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/TreeFeature.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/BiomeSource.h"
#include "world/biome/Biomes.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "util/CompletableFuture.h"
#include "server/level/WorldGenRegion.h"
#include "server/level/GenerationChunkHolder.h"
#include "server/level/ServerLevel.h"
#include "levelgen/structure/StructureSerialization.h"
#include "util/StaticCache2D.h"
#include "levelgen/WorldGenRegionLevel.h"
#include "levelgen/Heightmap.h"
#include "levelgen/structure/StructureGeneration.h"
#include "util/TerrainProfiling.h"
#include <vector>
#include <set>

// Reference: net/minecraft/world/level/chunk/status/ChunkStatusTasks.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * ChunkStatusTasks - Static task implementations for each generation step
 *
 * Each task corresponds to a ChunkStatus and performs the actual work.
 * Reference: ChunkStatusTasks.java
 */
class ChunkStatusTasks {
public:
    // Helper to create completed future
    using ChunkFuture = std::shared_ptr<util::CompletableFuture<::world::IChunk*>>;

    static ChunkFuture completed(::world::IChunk* chunk) {
        return util::CompletableFuture<::world::IChunk*>::completed(chunk);
    }

    /**
     * Pass-through task - does nothing, just returns the chunk
     * Used for EMPTY status
     * Reference: ChunkStatusTasks.java lines 36-38
     */
    static ChunkFuture passThrough(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        return completed(chunk);
    }


    /**
     * Generate structure starts
     * Reference: ChunkStatusTasks.java lines 40-48
     *
     * Creates structure start data for the chunk. This determines where
     * structures like villages, temples, etc. will generate.
     */
    static ChunkFuture generateStructureStarts(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        TERRAIN_ZONE_N("Gen.StructureStarts");
        // Reference: ChunkStatusTasks.java generateStructureStarts ->
        // generator.createStructures(...) when worldGenOptions().generateStructures().
        // structureState == nullptr means structures disabled (historical no-op).
        if (context.structureState != nullptr && context.generator != nullptr) {
            minecraft::levelgen::structure::StructureGeneration::createStructures(
                *context.structureState, context.generator, context.randomState, chunk);
        }
        return completed(chunk);
    }

    /**
     * Load structure starts (for already-generated chunks)
     * Reference: ChunkStatusTasks.java lines 50-53
     */
    static ChunkFuture loadStructureStarts(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        TERRAIN_ZONE_N("Gen.LoadStructureStarts");
        // Java re-registers the starts the chunk was saved with
        // (level.onStructureStartsAvailable over chunk.getAllStarts()), which
        // SerializableChunkData rebuilt from their tags. This port rebuilds
        // them the way MC rebuilds an ocean monument
        // (OceanMonumentStructure.regeneratePiecesAfterLoad): the starts are
        // a pure function of the seed, the position and the biome SOURCE,
        // so createStructures makes them again exactly as the generation
        // step did, and the saved record then hands back what the seed
        // cannot reproduce — reference counts, pieces that moved onto the
        // terrain, and each piece's placement state. A chunk the embedder
        // saved (FULL) carries no starts and just gets them regenerated.
        if (context.structureState != nullptr && context.generator != nullptr) {
            minecraft::levelgen::structure::StructureGeneration::createStructures(
                *context.structureState, context.generator, context.randomState, chunk);
        }
        // OBEY_NO_START_RESTORE=1: keep the regenerated starts as they are
        // (A/B: shows what the saved piece state is worth).
        static const bool kNoRestore = std::getenv("OBEY_NO_START_RESTORE") != nullptr;
        if (auto* proto = dynamic_cast<::world::ProtoChunk*>(chunk)) {
            if (std::unique_ptr<nbt::CompoundTag> saved = proto->takeSavedStructureStarts()) {
                if (!kNoRestore) {
                    minecraft::levelgen::structure::StructureSerialization::restoreStarts(*saved, *proto);
                }
            }
        }
        return completed(chunk);
    }

    /**
     * Generate structure references
     * Reference: ChunkStatusTasks.java lines 55-60
     *
     * For each structure that might extend into this chunk, records a
     * reference so features know to avoid that area.
     */
    static ChunkFuture generateStructureReferences(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        TERRAIN_ZONE_N("Gen.StructureRefs");
        // Reference: ChunkStatusTasks.java generateStructureReferences ->
        // generator.createReferences(region, ...). Reads the +-8 STRUCTURE_STARTS
        // neighborhood from the dependency grid; no RNG.
        if (context.structureState != nullptr) {
            minecraft::levelgen::structure::StructureGeneration::createReferences(chunks, chunk);
        }
        return completed(chunk);
    }

    /**
     * Generate biomes
     * Reference: ChunkStatusTasks.java lines 62-66
     * Reference: ChunkGenerator.java lines 113-117
     *
     * Fills the chunk's biome data using the biome source.
     * This determines climate zones for surface building.
     *
     * IMPORTANT: This task runs ASYNC on the background executor!
     * Java: return CompletableFuture.supplyAsync(() -> {
     *     protoChunk.fillBiomesFromNoise(this.biomeSource, randomState.sampler());
     *     return protoChunk;
     * }, Util.backgroundExecutor().forName("init_biomes"));
     */
    static ChunkFuture generateBiomes(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java line 65
        // context.generator().createBiomes(randomState, Blender.of(region), structureManager, chunk)
        if (!context.generator || !context.randomState) {
            return completed(chunk);
        }

        // If no background executor, run synchronously (fallback)
        if (!context.backgroundExecutor) {
            TERRAIN_ZONE_N("Gen.Biomes");
            Blender blender;
            context.generator->createBiomes(context.randomState, &blender, chunk);
            return completed(chunk);
        }

        // Reference: ChunkGenerator.java lines 113-117
        // return CompletableFuture.supplyAsync(() -> {
        //     protoChunk.fillBiomesFromNoise(this.biomeSource, randomState.sampler());
        //     return protoChunk;
        // }, Util.backgroundExecutor().forName("init_biomes"));
        //
        // Use supplyAsync pattern to run biome generation on thread pool.
        // Zone goes INSIDE the lambda: supplyAsync only enqueues here, so a
        // zone around this call would time the dispatch, not the generation.
        return util::CompletableFuture<::world::IChunk*>::supplyAsync(
            [generator = context.generator, randomState = context.randomState, chunk]() {
                TERRAIN_ZONE_N("Gen.Biomes");
                Blender blender;
                generator->createBiomes(randomState, &blender, chunk);
                return chunk;
            },
            context.backgroundExecutor
        );
    }

    // WorldGenRegion.getBiomeManager(): the fuzzed biome lookup over the
    // biomes stored in the region's chunks (WorldGenRegion.getNoiseBiome
    // routes each quart to its chunk). Reading stored biomes, not
    // recomputing them, is what Java does - and the RTree's thread-local
    // lastResult makes a recomputation order-dependent at biome edges.
    class RegionBiomeSource : public world::biome::BiomeManager::NoiseBiomeSource {
    public:
        RegionBiomeSource(const std::vector<std::vector<::world::IChunk*>>& chunks, ::world::ChunkPos center)
            : m_chunks(chunks), m_centerX(center.x()), m_centerZ(center.z()),
              m_radius((static_cast<int>(chunks.size()) - 1) / 2) {}

        world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
            const int32_t chunkX = (quartX << 2) >> 4;   // QuartPos.toBlock, then blockToSectionCoord
            const int32_t chunkZ = (quartZ << 2) >> 4;
            const int gridX = chunkX - m_centerX + m_radius;
            const int gridZ = chunkZ - m_centerZ + m_radius;
            if (gridZ >= 0 && gridZ < static_cast<int>(m_chunks.size()) &&
                gridX >= 0 && gridX < static_cast<int>(m_chunks[gridZ].size())) {
                if (auto* proto = dynamic_cast<::world::ProtoChunk*>(m_chunks[gridZ][gridX])) {
                    return proto->getNoiseBiome(quartX, quartY, quartZ);
                }
            }
            // Outside the region: WorldGenRegion would throw; plains keeps a
            // mis-sized grid from crashing the worker.
            return world::biome::Biomes::get(world::biome::BiomeKeys::PLAINS);
        }

    private:
        const std::vector<std::vector<::world::IChunk*>>& m_chunks;
        int m_centerX, m_centerZ, m_radius;
    };

    // MC 26.3 ChunkStatusTasks.buildTerrain (ChunkStatusTasks.java:85-103)
    // around NoiseBasedChunkGenerator.buildTerrain (fill from noise, the
    // material rules, the carvers, on one NoiseChunk), then the final
    // heightmaps - which 26.3 primes at the end of TERRAIN, not at the start
    // of FEATURES. One task on the worldgen pool, as vanilla's supplyAsync.
    static void buildTerrainStep(
        WorldGenContext& context,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        levelgen::ChunkGenerator::TerrainContext terrain;
        terrain.seed = context.seed;
        // Beardifier.forStructuresInChunk(structureManager, chunk.getPos()),
        // read from the dependency grid (STRUCTURE_STARTS at radius 8).
        if (context.structureState != nullptr) {
            terrain.beardifier = minecraft::levelgen::structure::StructureGeneration::createBeardifier(chunks, chunk);
        }
        RegionBiomeSource regionBiomes(chunks, chunk->getPos());
        world::biome::BiomeManager biomeManager(&regionBiomes, world::biome::BiomeManager::obfuscateSeed(context.seed));
        terrain.biomeGetter = [&biomeManager](const core::BlockPos& pos) -> world::biome::BiomeHolder {
            return biomeManager.getBiome(pos);
        };
        context.generator->buildTerrain(context.randomState, terrain, chunk);
        levelgen::Heightmap::primeHeightmaps(chunk, {
            levelgen::Heightmap::Types::MOTION_BLOCKING,
            levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES,
            levelgen::Heightmap::Types::OCEAN_FLOOR,
            levelgen::Heightmap::Types::WORLD_SURFACE
        });
        debugGenHash(chunk);   // end of TERRAIN: the last step that writes only the centre chunk
    }

    static ChunkFuture buildTerrain(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        if (!context.generator || !context.randomState) {
            return completed(chunk);
        }
        if (!context.backgroundExecutor) {
            buildTerrainStep(context, chunks, chunk);
            return completed(chunk);
        }
        // The neighbour grid is ChunkMap::applyStep's stack copy: the async
        // body gets its own. The WorldGenContext outlives every task.
        return util::CompletableFuture<::world::IChunk*>::supplyAsync(
            [contextPtr = &context, chunksCopy = chunks, chunk]() {
                buildTerrainStep(*contextPtr, chunksCopy, chunk);
                return chunk;
            },
            context.backgroundExecutor
        );
    }

    /**
     * Generate features (trees, ores, etc.)
     * Reference: ChunkStatusTasks.java lines 104-114
     *
     * Places decorations: trees, flowers, ores, etc.
     * Uses the decoration seeding for deterministic placement.
     */
    // Body of generateFeatures (see below for why it is a separate function).
    static void decorateStep(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        if (context.generator && context.randomState) {
            // Ensure feature registries are bootstrapped before any worker
            // reads the shared feature ordering cache.
            data::worldgen::BiomeFeatureRegistry::bootstrap();

            // (The final heightmaps are primed at the end of TERRAIN since
            // 26.3 — buildTerrainStep.)

            // =========================================================================
            // BUILD FEATURES PER STEP ONCE FOR ALL BIOMES (MEMOIZED)
            // Reference: Java's ChunkGenerator.featuresPerStep is built ONCE from
            // biomeSource.possibleBiomes() and reused for all chunks. This is CRITICAL
            // for feature RNG parity - the global index used for seeding depends on
            // the order features are first seen across ALL biomes, not just chunk biomes.
            // =========================================================================
            // CRITICAL: Java builds featuresPerStep PER GENERATOR from that
            // generator's biomeSource.possibleBiomes(). The nether sorter must
            // see ONLY the 5 nether biomes or feature indices (setFeatureSeed)
            // diverge. Dimension detected via the generator's default block.
            bool isNetherDim = false;
            bool isEndDim = false;
            bool isHushDim = false;
            bool isAetherDim = false;
            bool isTwilightDim = false;
            {
                auto* noiseGen = dynamic_cast<levelgen::NoiseBasedChunkGenerator*>(context.generator);
                // The Twilight Forest's default block is vanilla stone, like
                // the Overworld's, so it is told apart by the dimension tag
                // the engine sets on its NoiseGeneratorSettings.
                if (noiseGen && noiseGen->getSettings()) {
                    isTwilightDim = noiseGen->getSettings()->isTwilightForest();
                }
                if (noiseGen && noiseGen->getSettings() && noiseGen->getSettings()->defaultBlock()) {
                    const std::string& defaultBlockId =
                        noiseGen->getSettings()->defaultBlock()->getIdentifier();
                    isNetherDim = defaultBlockId == "minecraft:netherrack";
                    isEndDim = defaultBlockId == "minecraft:end_stone";
                    // The Hush (engine-only): its generator's default block.
                    isHushDim = defaultBlockId == "minecraft:hushstone";
                    // The Aether: skylands.json's default block (holystone).
                    isAetherDim = defaultBlockId == "minecraft:holystone";
                }
            }
            auto buildForKeys = [](const std::vector<std::string>& biomeKeys) {
                return levelgen::FeatureSorter::buildFeaturesPerStep<std::string>(
                    biomeKeys,
                    [](const std::string& biomeKey) -> std::vector<std::vector<levelgen::placement::PlacedFeature*>> {
                        const auto& features = data::worldgen::BiomeFeatureRegistry::getFeaturesForBiome(biomeKey);
                        std::vector<std::vector<levelgen::placement::PlacedFeature*>> result;
                        result.reserve(features.size());
                        for (const auto& stepFeatures : features) {
                            std::vector<levelgen::placement::PlacedFeature*> step;
                            step.reserve(stepFeatures.size());
                            for (const auto* f : stepFeatures) {
                                step.push_back(const_cast<levelgen::placement::PlacedFeature*>(f));
                            }
                            result.push_back(std::move(step));
                        }
                        return result;
                    },
                    true
                );
            };
            const std::vector<levelgen::StepFeatureData>& featuresPerStep =
                [&]() -> const std::vector<levelgen::StepFeatureData>& {
                    // Per-generator override (single-biome and flat worlds):
                    // Java builds featuresPerStep from the generator's own
                    // possibleBiomes(), not the whole dimension.
                    if (const auto* custom = context.generator->customFeaturesPerStep()) {
                        return *custom;
                    }
                    if (isNetherDim) {
                        static const std::vector<levelgen::StepFeatureData> s_netherFeaturesPerStep =
                            buildForKeys(data::worldgen::BiomeFeatureRegistry::getNetherBiomeKeys());
                        return s_netherFeaturesPerStep;
                    }
                    if (isEndDim) {
                        static const std::vector<levelgen::StepFeatureData> s_endFeaturesPerStep =
                            buildForKeys(data::worldgen::BiomeFeatureRegistry::getEndBiomeKeys());
                        return s_endFeaturesPerStep;
                    }
                    if (isHushDim) {
                        static const std::vector<levelgen::StepFeatureData> s_hushFeaturesPerStep =
                            buildForKeys(data::worldgen::BiomeFeatureRegistry::getHushBiomeKeys());
                        return s_hushFeaturesPerStep;
                    }
                    if (isAetherDim) {
                        static const std::vector<levelgen::StepFeatureData> s_aetherFeaturesPerStep =
                            buildForKeys(data::worldgen::BiomeFeatureRegistry::getAetherBiomeKeys());
                        return s_aetherFeaturesPerStep;
                    }
                    if (isTwilightDim) {
                        static const std::vector<levelgen::StepFeatureData> s_twilightFeaturesPerStep =
                            buildForKeys(data::worldgen::BiomeFeatureRegistry::getTwilightBiomeKeys());
                        return s_twilightFeaturesPerStep;
                    }
                    static const std::vector<levelgen::StepFeatureData> s_overworldFeaturesPerStep =
                        buildForKeys(data::worldgen::BiomeFeatureRegistry::getAllBiomeKeys());
                    return s_overworldFeaturesPerStep;
                }();

            // Build multi-chunk WorldGenRegion from neighbor chunk grid
            // Reference: ChunkStatusTasks.java generateFeatures() lines 107-109
            //   WorldGenRegion region = new WorldGenRegion(level, chunks, step, chunk);
            //   context.generator().applyBiomeDecoration(region, chunk, structureManager);
            //
            // The 'chunks' parameter is the full accumulated dependency grid from ChunkMap::applyStep.
            // Using WorldGenRegion allows features to read/write across chunk boundaries,
            // and those writes persist because the chunk objects are shared via ChunkMap.

            ::world::ChunkPos centerPos = chunk->getPos();
            int inputGridSize = static_cast<int>(chunks.size());
            int inputRadius = (inputGridSize - 1) / 2;

            std::vector<std::unique_ptr<server::level::SimpleGenerationChunkHolder>> holderStorage;
            std::vector<server::level::GenerationChunkHolder*> holders;
            holderStorage.reserve(inputGridSize * inputGridSize);
            holders.reserve(inputGridSize * inputGridSize);

            for (int gridZ = 0; gridZ < inputGridSize; ++gridZ) {
                for (int gridX = 0; gridX < static_cast<int>(chunks[gridZ].size()); ++gridX) {
                    ::world::IChunk* neighborChunk = chunks[gridZ][gridX];
                    if (neighborChunk) {
                        holderStorage.push_back(
                            std::make_unique<server::level::SimpleGenerationChunkHolder>(neighborChunk)
                        );
                        holders.push_back(holderStorage.back().get());
                    } else {
                        holders.push_back(nullptr);
                    }
                }
            }

            // Build StaticCache2D for the full accumulated dependency radius.
            auto cache2d = util::StaticCache2D<server::level::GenerationChunkHolder*>::create(
                centerPos.x(), centerPos.z(), inputRadius,
                [&holders, &centerPos, inputRadius, inputGridSize](int x, int z) -> server::level::GenerationChunkHolder* {
                    int gridX = x - centerPos.x() + inputRadius;
                    int gridZ = z - centerPos.z() + inputRadius;
                    if (gridX >= 0 && gridX < inputGridSize && gridZ >= 0 && gridZ < inputGridSize) {
                        return holders[gridZ * inputGridSize + gridX];
                    }
                    return nullptr;
                }
            );

            server::level::ServerLevel* serverLevel =
                static_cast<server::level::ServerLevel*>(context.level);
            static std::unique_ptr<server::level::ServerLevel> s_fallbackServerLevel;
            if (!serverLevel) {
                // Get block info from chunk
                auto* proto = dynamic_cast<::world::ProtoChunk*>(chunk);
                if (!s_fallbackServerLevel) {
                    s_fallbackServerLevel = std::make_unique<server::level::ServerLevel>(
                        -64, 384, nullptr,
                        proto ? proto->getAirBlock() : nullptr,
                        proto ? proto->getDefaultBlock() : nullptr,
                        context.seed
                    );
                }
                serverLevel = s_fallbackServerLevel.get();
            }

            // Create WorldGenRegion with multi-chunk access
            server::level::WorldGenRegion region(*serverLevel, context.randomState, cache2d, step, *chunk);
            levelgen::WorldGenRegionLevel level(&region);

            // Apply biome decoration with multi-chunk WorldGenLevel
            context.generator->applyBiomeDecoration(
                &level,
                chunk,
                featuresPerStep
            );

            // Engine hand-off: the structure boxes spawn_overrides test
            // (IChunk::StructureSpawnArea), recorded after decoration so a
            // piece that settled its height in postProcess is where it ended.
            levelgen::structure::StructureGeneration::recordSpawnOverrideAreas(&level, chunk);

        }

    }

    // OBEY_GEN_HASH=1: log a hash of every block state in the chunk at the end
    // of the carvers step — the exact output of biomes + noise + surface +
    // carvers, and the last point before neighbours' decoration writes in. Decoration is order-dependent
    // between neighbouring chunks (as in vanilla), so the saved region files
    // cannot serve as a determinism oracle for density-function changes;
    // this can. Compare `[GenHash]` lines between two runs.
    static void debugGenHash(::world::IChunk* chunk) {
        static const bool enabled = std::getenv("OBEY_GEN_HASH") != nullptr;
        if (!enabled || !chunk) return;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const std::string& sv) {
            for (unsigned char c : sv) { h ^= c; h *= 1099511628211ull; }
        };
        const int n = chunk->getSectionsCount();
        for (int si = 0; si < n; ++si) {
            auto& sec = chunk->getSection(si);
            if (sec.hasOnlyAir()) { mix("air"); continue; }
            const BlockState* last = nullptr; std::string lastStr;
            for (int y = 0; y < 16; ++y) for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
                const BlockState* st = sec.getBlockState(x, y, z);
                if (st != last) { last = st; lastStr = st ? st->toStateString() : std::string("null"); }
                mix(lastStr);
            }
        }
        std::fprintf(stderr, "[GenHash] %d %d %016llx\n", chunk->getPos().x(), chunk->getPos().z(),
                     static_cast<unsigned long long>(h));
    }

    static ChunkFuture generateFeatures(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {

        // Reference: ChunkStatusTasks.java lines 106-107
        // Heightmap.primeHeightmaps(chunk, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE)
        // Reference: ChunkStatusTasks.java lines 108-110
        // context.generator().applyBiomeDecoration(region, chunk, structureManager)

        // DIVERGENCE FROM VANILLA (deliberate, measured 2026-08-30): vanilla
        // decorates on the single worldgen lane; here that lane measured 59%
        // busy with decoration as ~70% of it, and it is what capped fresh
        // generation at ~100-125 chunks/s with the pool half idle. The step
        // writes only its 3x3 neighbourhood (write radius 1), so steps whose
        // centres are >= 3 chunks apart are independent; FeatureClaims makes
        // any two closer than that take turns. Which of two neighbouring
        // chunks decorates first is already scheduling-dependent in vanilla
        // (the lane's own order is not fixed), so this changes the ORDER
        // vanilla could have produced, not the kind of result.
        if (!context.backgroundExecutor || !context.featureClaims) {
            TERRAIN_ZONE_N("Gen.Features");
            decorateStep(context, step, chunks, chunk);
            return completed(chunk);
        }
        const ::world::ChunkPos pos = chunk->getPos();
        WorldGenContext* ctx = &context;
        const ChunkStep* stepp = &step;
        auto future = std::make_shared<util::CompletableFuture<::world::IChunk*>>();
        auto chunksCopy = std::make_shared<const std::vector<std::vector<::world::IChunk*>>>(chunks);
        // Self-rescheduling attempt: if a neighbour is decorating, go to the
        // back of the pool's queue and try again after the work ahead of us.
        const bool structural = chunk->hasAnyStructureReferences();   // pieces read far; see FeatureClaims
        auto attempt = std::make_shared<std::function<void()>>();
        *attempt = [ctx, stepp, chunksCopy, chunk, pos, future, attempt, structural]() {
            if (!ctx->featureClaims->acquireOrWait(pos.x(), pos.z(), structural, *attempt)) {
                return;   // parked; a release will re-submit us
            }
            auto submit = [ctx](std::function<void()> fn) { (ctx->decorationExecutor ? ctx->decorationExecutor : ctx->backgroundExecutor)(std::move(fn)); };
            try {
                TERRAIN_ZONE_N("Gen.Features");
                decorateStep(*ctx, *stepp, *chunksCopy, chunk);
            } catch (...) {
                ctx->featureClaims->release(pos.x(), pos.z(), submit);
                future->completeExceptionally(std::current_exception());
                return;
            }
            ctx->featureClaims->release(pos.x(), pos.z(), submit);
            future->complete(chunk);
        };
        (context.decorationExecutor ? context.decorationExecutor : context.backgroundExecutor)(*attempt);
        return future;
    }

    /**
     * Initialize light engine
     * Reference: ChunkStatusTasks.java lines 116-122
     *
     * Sets up light sources and prepares for light calculation.
     */
    static ChunkFuture initializeLight(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java lines 118-121
        // chunk.initializeLightSources()
        // protoChunk.setLightEngine(lightEngine)
        // return lightEngine.initializeLight(chunk, isLighted)
        return completed(chunk);
    }

    /**
     * Compute lighting
     * Reference: ChunkStatusTasks.java lines 124-127
     *
     * Calculates block and sky light for the chunk.
     */
    static ChunkFuture light(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java lines 125-126
        // return lightEngine.lightChunk(chunk, isLighted)
        return completed(chunk);
    }

    /**
     * Spawn mobs
     * Reference: ChunkStatusTasks.java lines 129-135
     *
     * Spawns initial mobs like animals, monsters in structures, etc.
     */
    static ChunkFuture generateSpawn(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        TERRAIN_ZONE_N("Gen.Spawn");

        // Reference: ChunkStatusTasks.java lines 130-132
        // if (!chunk.isUpgrading())
        //     context.generator().spawnOriginalMobs(new WorldGenRegion(...))
        if (context.generator) {
            context.generator->spawnOriginalMobs(chunk);
        }
        return completed(chunk);
    }

    /**
     * Convert to full LevelChunk
     * Reference: ChunkStatusTasks.java lines 137-165
     *
     * Converts ProtoChunk to LevelChunk, finishing generation.
     */
    static ChunkFuture full(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        TERRAIN_ZONE_N("Gen.Full");

        // Reference: ChunkStatusTasks.java lines 140-164
        // In Java, this converts ProtoChunk to LevelChunk
        // For our C++ implementation, we just mark it complete.
        if (auto* proto = dynamic_cast<::world::ProtoChunk*>(chunk)) {
            // The embedder saves FULL chunks in its own format; hand it the
            // "structures" compound MC's SerializableChunkData would write
            // (references and the starts begun here), encoded here on the
            // worldgen lane where no piece placement can run concurrently.
            proto->setStructuresAtFull(std::make_shared<const std::vector<uint8_t>>(
                minecraft::levelgen::structure::StructureSerialization::encodeStructureData(
                    proto->getPos(), proto->getAllStructureStarts(),
                    proto->getAllStructureReferences())));
        }
        return completed(chunk);
    }

private:
    /**
     * Check if chunk has been lighted
     * Reference: ChunkStatusTasks.java lines 32-34
     */
    static bool isLighted(const ::world::IChunk* chunk) {
        // return chunk.getPersistedStatus().isOrAfter(ChunkStatus.LIGHT) && chunk.isLightCorrect()
        // For now, just return false
        return false;
    }
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
