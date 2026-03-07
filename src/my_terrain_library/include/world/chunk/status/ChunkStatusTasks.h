#pragma once

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
#include "util/StaticCache2D.h"
#include "levelgen/WorldGenRegionLevel.h"
#include "levelgen/Heightmap.h"
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
        // Structure generation would go here
        // Reference: context.generator().createStructures(...)
        // For terrain-only generation, this is a no-op
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
        // Reference: context.generator().createReferences(region, ...)
        // For terrain-only generation, this is a no-op
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
        // Use supplyAsync pattern to run biome generation on thread pool
        return util::CompletableFuture<::world::IChunk*>::supplyAsync(
            [generator = context.generator, randomState = context.randomState, chunk]() {
                Blender blender;
                generator->createBiomes(randomState, &blender, chunk);
                return chunk;
            },
            context.backgroundExecutor
        );
    }

    /**
     * Generate noise (base terrain)
     * Reference: ChunkStatusTasks.java lines 68-84
     * Reference: NoiseBasedChunkGenerator.java fillFromNoise() lines 233-261
     *
     * This is the core terrain generation step. Uses the NoiseChunk
     * system to fill the chunk with stone/air/water based on density functions.
     *
     * IMPORTANT: This is the only task that runs ASYNC on the background executor!
     * Java: return CompletableFuture.supplyAsync(() -> doFill(...), Util.backgroundExecutor())
     *
     * This enables parallel noise generation across multiple chunks.
     */
    static ChunkFuture generateNoise(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java lines 70-71
        // context.generator().fillFromNoise(Blender.of(region), randomState, structureManager, chunk)
        if (!context.generator || !context.randomState) {
            return completed(chunk);
        }

        // If no background executor, run synchronously (like old behavior)
        if (!context.backgroundExecutor) {
            Blender blender;
            context.generator->fillFromNoise(context.randomState, &blender, chunk);
            return completed(chunk);
        }

        // Reference: NoiseBasedChunkGenerator.java line 238-260
        // return CompletableFuture.supplyAsync(() -> {
        //     doFill(blender, structureManager, randomState, centerChunk, cellMinY, cellCountY)
        // }, Util.backgroundExecutor().forName("wgen_fill_noise"));
        //
        // Use supplyAsync pattern to run noise generation on thread pool
        return util::CompletableFuture<::world::IChunk*>::supplyAsync(
            [generator = context.generator, randomState = context.randomState, chunk]() {
                Blender blender;
                generator->fillFromNoise(randomState, &blender, chunk);
                return chunk;
            },
            context.backgroundExecutor
        );
    }

    /**
     * Generate surface
     * Reference: ChunkStatusTasks.java lines 86-91
     *
     * Applies surface rules to convert stone to grass/dirt/sand/etc.
     * based on biome and noise.
     */
    static ChunkFuture generateSurface(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java line 89
        // context.generator().buildSurface(region, structureManager, randomState, chunk)
        if (context.generator && context.randomState) {
            auto* noiseGenerator = dynamic_cast<levelgen::NoiseBasedChunkGenerator*>(context.generator);
            if (noiseGenerator) {
                // FIX: Match Java's WorldGenRegion behavior - read biomes from stored chunk data
                // instead of recomputing through BiomeSource::getNoiseBiome() (which uses the RTree).
                // The RTree has a thread-local lastResult cache that can cause non-deterministic
                // results at biome boundaries. Java avoids this because WorldGenRegion reads from
                // the chunk's stored PalettedContainer<Biome> (a direct array lookup, no caching).
                // The chunk grid provides access to neighbor chunks (BIOMES at radius 1) so the
                // BiomeManager's fiddling algorithm can correctly sample cross-chunk positions.
                ::world::ChunkPos centerPos = chunk->getPos();
                int inputGridSize = static_cast<int>(chunks.size());
                int inputRadius = (inputGridSize - 1) / 2;

                class ChunkGridBiomeSource : public world::biome::BiomeManager::NoiseBiomeSource {
                public:
                    const std::vector<std::vector<::world::IChunk*>>& m_chunks;
                    int m_centerChunkX, m_centerChunkZ, m_gridRadius;

                    ChunkGridBiomeSource(const std::vector<std::vector<::world::IChunk*>>& chunks,
                                         int centerX, int centerZ, int radius)
                        : m_chunks(chunks), m_centerChunkX(centerX), m_centerChunkZ(centerZ), m_gridRadius(radius) {}

                    world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                        // Reference: WorldGenRegion.getNoiseBiome() - routes to correct chunk
                        int32_t chunkX = (quartX << 2) >> 4;  // QuartPos.toBlock then blockToSectionCoord
                        int32_t chunkZ = (quartZ << 2) >> 4;

                        int gridX = chunkX - m_centerChunkX + m_gridRadius;
                        int gridZ = chunkZ - m_centerChunkZ + m_gridRadius;

                        if (gridZ >= 0 && gridZ < static_cast<int>(m_chunks.size()) &&
                            gridX >= 0 && gridX < static_cast<int>(m_chunks[gridZ].size())) {
                            auto* proto = dynamic_cast<::world::ProtoChunk*>(m_chunks[gridZ][gridX]);
                            if (proto) {
                                return proto->getNoiseBiome(quartX, quartY, quartZ);
                            }
                        }
                        // Fallback: should not normally happen
                        return world::biome::Biomes::get(world::biome::BiomeKeys::PLAINS);
                    }
                };

                ChunkGridBiomeSource gridBiomeSource(chunks, centerPos.x(), centerPos.z(), inputRadius);
                long obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(context.seed);
                world::biome::BiomeManager biomeManager(&gridBiomeSource, obfuscatedSeed);

                auto biomeGetter = [&biomeManager](const core::BlockPos& pos) -> world::biome::BiomeHolder {
                    return biomeManager.getBiome(pos);
                };
                noiseGenerator->buildSurface(context.randomState, biomeGetter, chunk);
            }
        }
        return completed(chunk);
    }

    /**
     * Generate carvers (caves and canyons)
     * Reference: ChunkStatusTasks.java lines 93-102
     *
     * Carves caves and canyons into the terrain using world carvers.
     */
    static ChunkFuture generateCarvers(
        WorldGenContext& context,
        const ChunkStep& step,
        const std::vector<std::vector<::world::IChunk*>>& chunks,
        ::world::IChunk* chunk
    ) {
        // Reference: ChunkStatusTasks.java lines 96-98, 100
        // Blender.addAroundOldChunksCarvingMaskFilter(region, protoChunk) - for upgrades
        // context.generator().applyCarvers(region, seed, randomState, biomeManager, structureManager, chunk)

        if (context.generator && context.randomState) {
            auto* noiseGenerator = dynamic_cast<levelgen::NoiseBasedChunkGenerator*>(context.generator);
            if (noiseGenerator) {
                // FIX: Match Java's WorldGenRegion behavior - read biomes from stored chunk data
                // (Same fix as in generateSurface - see comment there for details)
                ::world::ChunkPos centerPos = chunk->getPos();
                int inputGridSize = static_cast<int>(chunks.size());
                int inputRadius = (inputGridSize - 1) / 2;

                class ChunkGridBiomeSource : public world::biome::BiomeManager::NoiseBiomeSource {
                public:
                    const std::vector<std::vector<::world::IChunk*>>& m_chunks;
                    int m_centerChunkX, m_centerChunkZ, m_gridRadius;

                    ChunkGridBiomeSource(const std::vector<std::vector<::world::IChunk*>>& chunks,
                                         int centerX, int centerZ, int radius)
                        : m_chunks(chunks), m_centerChunkX(centerX), m_centerChunkZ(centerZ), m_gridRadius(radius) {}

                    world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                        int32_t chunkX = (quartX << 2) >> 4;
                        int32_t chunkZ = (quartZ << 2) >> 4;

                        int gridX = chunkX - m_centerChunkX + m_gridRadius;
                        int gridZ = chunkZ - m_centerChunkZ + m_gridRadius;

                        if (gridZ >= 0 && gridZ < static_cast<int>(m_chunks.size()) &&
                            gridX >= 0 && gridX < static_cast<int>(m_chunks[gridZ].size())) {
                            auto* proto = dynamic_cast<::world::ProtoChunk*>(m_chunks[gridZ][gridX]);
                            if (proto) {
                                return proto->getNoiseBiome(quartX, quartY, quartZ);
                            }
                        }
                        return world::biome::Biomes::get(world::biome::BiomeKeys::PLAINS);
                    }
                };

                ChunkGridBiomeSource gridBiomeSource(chunks, centerPos.x(), centerPos.z(), inputRadius);
                long obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(context.seed);
                world::biome::BiomeManager biomeManager(&gridBiomeSource, obfuscatedSeed);

                auto biomeGetter = [&biomeManager](const core::BlockPos& pos) -> world::biome::BiomeHolder {
                    return biomeManager.getBiome(pos);
                };
                noiseGenerator->applyCarvers(
                    context.seed,
                    context.randomState,
                    biomeGetter,
                    chunk,
                    levelgen::GenerationStep::Decoration::RAW_GENERATION  // Air carving step
                );
            }
        }
        return completed(chunk);
    }

    /**
     * Generate features (trees, ores, etc.)
     * Reference: ChunkStatusTasks.java lines 104-114
     *
     * Places decorations: trees, flowers, ores, etc.
     * Uses the decoration seeding for deterministic placement.
     */
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

        if (context.generator && context.randomState) {
            // Ensure feature registries are bootstrapped
            if (!data::worldgen::BiomeFeatureRegistry::isInitialized()) {
                data::worldgen::BiomeFeatureRegistry::bootstrap();
            }

            // Prime heightmaps
            levelgen::Heightmap::primeHeightmaps(chunk, {
                levelgen::Heightmap::Types::MOTION_BLOCKING,
                levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES,
                levelgen::Heightmap::Types::OCEAN_FLOOR,
                levelgen::Heightmap::Types::WORLD_SURFACE
            });

            // =========================================================================
            // BUILD FEATURES PER STEP ONCE FOR ALL BIOMES (MEMOIZED)
            // Reference: Java's ChunkGenerator.featuresPerStep is built ONCE from
            // biomeSource.possibleBiomes() and reused for all chunks. This is CRITICAL
            // for feature RNG parity - the global index used for seeding depends on
            // the order features are first seen across ALL biomes, not just chunk biomes.
            // =========================================================================
            static std::vector<levelgen::StepFeatureData> s_cachedFeaturesPerStep;
            static bool s_featuresPerStepBuilt = false;

            if (!s_featuresPerStepBuilt) {
                // Get ALL biome keys in bootstrap order (matches Java's biomeSource.possibleBiomes())
                const auto& allBiomeKeys = data::worldgen::BiomeFeatureRegistry::getAllBiomeKeys();

                // Use FeatureSorter to build global feature order
                // Reference: FeatureSorter.buildFeaturesPerStep(List.copyOf(biomeSource.possibleBiomes()), ...)
                s_cachedFeaturesPerStep = levelgen::FeatureSorter::buildFeaturesPerStep<std::string>(
                    allBiomeKeys,
                    [](const std::string& biomeKey) -> std::vector<std::vector<levelgen::placement::PlacedFeature*>> {
                        const auto& features = data::worldgen::BiomeFeatureRegistry::getFeaturesForBiome(biomeKey);
                        // Convert const PlacedFeature* to PlacedFeature* (FeatureSorter needs non-const)
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
                    true  // tryReducingError
                );

                s_featuresPerStepBuilt = true;
            }

            // Use the cached/memoized features for this chunk
            const std::vector<levelgen::StepFeatureData>& featuresPerStep = s_cachedFeaturesPerStep;

            // Build multi-chunk WorldGenRegion from neighbor chunk grid
            // Reference: ChunkStatusTasks.java generateFeatures() lines 107-109
            //   WorldGenRegion region = new WorldGenRegion(level, chunks, step, chunk);
            //   context.generator().applyBiomeDecoration(region, chunk, structureManager);
            //
            // The 'chunks' parameter is a 2D grid from ChunkMap::applyStep.
            // For FEATURES (radius 1), it's 3x3 with neighbors at CARVERS status.
            // Using WorldGenRegion allows features to read/write across chunk boundaries,
            // and those writes persist because the chunk objects are shared via ChunkMap.

            ::world::ChunkPos centerPos = chunk->getPos();
            int inputGridSize = static_cast<int>(chunks.size());
            int inputRadius = (inputGridSize - 1) / 2;

            // Note: inputGridSize may be larger than 3 (e.g., 21 for accumulated deps including STRUCTURE_STARTS at radius 8)
            // We extract only the 3x3 center for WorldGenRegion (featureRadius=1)

            // FEATURES needs radius 1 (3x3) for WorldGenRegion
            // The input grid may be larger (e.g., 17x17 if accumulated deps include STRUCTURE_STARTS at radius 8)
            // Extract the 3x3 center from the input grid
            constexpr int featureRadius = 1;

            // Create chunk holders for the 3x3 neighbor grid
            std::vector<server::level::SimpleGenerationChunkHolder*> holders;
            holders.reserve(9);

            for (int dz = -featureRadius; dz <= featureRadius; ++dz) {
                for (int dx = -featureRadius; dx <= featureRadius; ++dx) {
                    int gridZ = dz + inputRadius;
                    int gridX = dx + inputRadius;
                    ::world::IChunk* neighborChunk = nullptr;
                    if (gridZ >= 0 && gridZ < inputGridSize &&
                        gridX >= 0 && gridX < static_cast<int>(chunks[gridZ].size())) {
                        neighborChunk = chunks[gridZ][gridX];
                    }

                    if (neighborChunk) {
                        // Prime heightmaps for neighbor chunks
                        levelgen::Heightmap::primeHeightmaps(neighborChunk, {
                            levelgen::Heightmap::Types::MOTION_BLOCKING,
                            levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES,
                            levelgen::Heightmap::Types::OCEAN_FLOOR,
                            levelgen::Heightmap::Types::WORLD_SURFACE
                        });
                        holders.push_back(new server::level::SimpleGenerationChunkHolder(
                            dynamic_cast<::world::ProtoChunk*>(neighborChunk)));
                    } else {
                        holders.push_back(nullptr);
                    }
                }
            }

            // Build StaticCache2D for radius 1 (3x3)
            auto cache2d = util::StaticCache2D<server::level::GenerationChunkHolder*>::create(
                centerPos.x(), centerPos.z(), featureRadius,
                [&holders, &centerPos](int x, int z) -> server::level::GenerationChunkHolder* {
                    int dx = x - centerPos.x() + 1;  // featureRadius=1
                    int dz = z - centerPos.z() + 1;
                    if (dx >= 0 && dx < 3 && dz >= 0 && dz < 3) {
                        return holders[dz * 3 + dx];
                    }
                    return nullptr;
                }
            );

            // Create ServerLevel (reused across calls via static)
            static std::unique_ptr<server::level::ServerLevel> s_serverLevel;
            if (!s_serverLevel) {
                // Get block info from chunk
                auto* proto = dynamic_cast<::world::ProtoChunk*>(chunk);
                s_serverLevel = std::make_unique<server::level::ServerLevel>(
                    -64, 384, nullptr,
                    proto ? proto->getAirBlock() : nullptr,
                    proto ? proto->getDefaultBlock() : nullptr,
                    context.seed
                );
            }

            // Create WorldGenRegion with multi-chunk access
            server::level::WorldGenRegion region(*s_serverLevel, cache2d, step, *chunk);
            levelgen::WorldGenRegionLevel level(&region);

            // Apply biome decoration with multi-chunk WorldGenLevel
            context.generator->applyBiomeDecoration(
                &level,
                chunk,
                featuresPerStep
            );

            // Cleanup holders
            for (auto* h : holders) {
                delete h;
            }
        }

        return completed(chunk);
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
        // Reference: ChunkStatusTasks.java lines 140-164
        // In Java, this converts ProtoChunk to LevelChunk
        // For our C++ implementation, we just mark it complete
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
