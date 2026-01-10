#pragma once

#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/IChunk.h"
#include "world/ProtoChunk.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/GenerationStep.h"
#include "levelgen/RandomState.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/BiomeSource.h"
#include "world/biome/Biomes.h"
#include "util/CompletableFuture.h"
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
            // FIX: Use biomeSource directly instead of chunk to avoid & 3 coordinate wrapping
            // at chunk boundaries. The chunk's getNoiseBiome uses & 3 which wraps coordinates,
            // causing incorrect biomes to be returned for positions outside the chunk bounds.
            // This matters for BiomeManager's fiddling algorithm which samples 8 corners that
            // can span multiple chunks.
            auto* noiseGenerator = dynamic_cast<levelgen::NoiseBasedChunkGenerator*>(context.generator);
            if (noiseGenerator) {
                world::biome::BiomeSource* biomeSource = noiseGenerator->getBiomeSource();

                // Create a NoiseBiomeSource wrapper that queries biomeSource directly
                class BiomeSourceWrapper : public world::biome::BiomeManager::NoiseBiomeSource {
                public:
                    world::biome::BiomeSource* m_biomeSource;
                    levelgen::RandomState* m_randomState;

                    BiomeSourceWrapper(world::biome::BiomeSource* bs, levelgen::RandomState* rs)
                        : m_biomeSource(bs), m_randomState(rs) {}

                    world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                        world::biome::BiomeKey key = m_biomeSource->getNoiseBiome(quartX, quartY, quartZ, *m_randomState->sampler());
                        return world::biome::Biomes::get(key);
                    }
                };

                BiomeSourceWrapper biomeSourceWrapper(biomeSource, context.randomState);
                long obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(context.seed);
                world::biome::BiomeManager biomeManager(&biomeSourceWrapper, obfuscatedSeed);

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
            // FIX: Use biomeSource directly instead of chunk to avoid & 3 coordinate wrapping
            // (Same fix as in generateSurface - see comment there for details)
            auto* noiseGenerator = dynamic_cast<levelgen::NoiseBasedChunkGenerator*>(context.generator);
            if (noiseGenerator) {
                world::biome::BiomeSource* biomeSource = noiseGenerator->getBiomeSource();

                class BiomeSourceWrapper : public world::biome::BiomeManager::NoiseBiomeSource {
                public:
                    world::biome::BiomeSource* m_biomeSource;
                    levelgen::RandomState* m_randomState;

                    BiomeSourceWrapper(world::biome::BiomeSource* bs, levelgen::RandomState* rs)
                        : m_biomeSource(bs), m_randomState(rs) {}

                    world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                        world::biome::BiomeKey key = m_biomeSource->getNoiseBiome(quartX, quartY, quartZ, *m_randomState->sampler());
                        return world::biome::Biomes::get(key);
                    }
                };

                BiomeSourceWrapper biomeSourceWrapper(biomeSource, context.randomState);
                long obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(context.seed);
                world::biome::BiomeManager biomeManager(&biomeSourceWrapper, obfuscatedSeed);

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

        // For features, we need the full decoration pipeline
        // This is a placeholder - full implementation requires PlacedFeature system
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
