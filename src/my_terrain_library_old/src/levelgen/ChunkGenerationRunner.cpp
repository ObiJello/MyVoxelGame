#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/Blender.h"
#include "world/ProtoChunk.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/Biomes.h"
#include "core/QuartPos.h"
#include <iostream>

// Reference: Flattened implementation of ChunkStatusTasks.java
// This executes each generation phase sequentially for exact bit parity.

namespace minecraft {
namespace levelgen {

using namespace world::chunk::status;

ChunkGenerationRunner::ChunkGenerationRunner(
    NoiseBasedChunkGenerator* generator,
    RandomState* randomState,
    int64_t seed,
    Config config
)
    : m_generator(generator)
    , m_randomState(randomState)
    , m_seed(seed)
    , m_config(config)
{
}

::world::IChunk* ChunkGenerationRunner::generateChunk(::world::IChunk* chunk) {
    // Run all configured phases in Minecraft's exact order
    // Reference: ChunkStatus static initialization order (ChunkStatus.java lines 113-124)
    // Reference: ChunkPyramid.GENERATION_PYRAMID (ChunkPyramid.java line 17)

    // Phase 1: EMPTY (always runs, just a passthrough)
    chunk = phaseEmpty(chunk);

    // Phase 2-3: Structures (optional)
    if (m_config.runStructures) {
        chunk = phaseStructureStarts(chunk);
        chunk = phaseStructureReferences(chunk);
    }

    // Phase 4: BIOMES - Required for surface rules to work correctly
    // Reference: ChunkStatusTasks.java line 65
    if (m_config.runBiomes) {
        chunk = phaseBiomes(chunk);
    }

    // Phase 5: NOISE - The main terrain generation
    // Reference: ChunkStatusTasks.java line 71
    // This is where fillFromNoise() is called, which triggers the entire
    // density function evaluation chain.
    if (m_config.runNoise) {
        chunk = phaseNoise(chunk);
    }

    // Phase 6: SURFACE - Apply surface blocks (grass, sand, etc.)
    // Reference: ChunkStatusTasks.java line 89
    if (m_config.runSurface) {
        chunk = phaseSurface(chunk);
    }

    // Phase 7: CARVERS - Carve caves and canyons
    // Reference: ChunkStatusTasks.java line 100
    if (m_config.runCarvers) {
        chunk = phaseCarvers(chunk);
    }

    // Phase 8: FEATURES - Place decorations (ores, trees, etc.)
    // Reference: ChunkStatusTasks.java line 109
    if (m_config.runFeatures) {
        chunk = phaseFeatures(chunk);
    }

    // Phase 9-10: Lighting (optional)
    // NOTE: Commented out - only care about blocks, not lighting
    // if (m_config.runLighting) {
    //     chunk = phaseInitializeLight(chunk);
    //     chunk = phaseLight(chunk);
    // }

    // Phase 11: SPAWN - Spawn initial mobs
    // NOTE: Commented out - only care about blocks, not mobs
    // if (m_config.runSpawn) {
    //     chunk = phaseSpawn(chunk);
    // }

    // Phase 12: FULL - Finalize
    chunk = phaseFull(chunk);

    return chunk;
}

::world::IChunk* ChunkGenerationRunner::generateChunkToStatus(
    ::world::IChunk* chunk,
    const ChunkStatus& targetStatus
) {
    // Run phases up to and including the target status
    // This allows stopping at any intermediate point for debugging/comparison

    chunk = phaseEmpty(chunk);
    if (targetStatus == ChunkStatus::EMPTY) return chunk;

    if (m_config.runStructures) {
        chunk = phaseStructureStarts(chunk);
        if (targetStatus == ChunkStatus::STRUCTURE_STARTS) return chunk;

        chunk = phaseStructureReferences(chunk);
        if (targetStatus == ChunkStatus::STRUCTURE_REFERENCES) return chunk;
    }

    if (m_config.runBiomes) {
        chunk = phaseBiomes(chunk);
        if (targetStatus == ChunkStatus::BIOMES) return chunk;
    }

    if (m_config.runNoise) {
        chunk = phaseNoise(chunk);
        if (targetStatus == ChunkStatus::NOISE) return chunk;
    }

    if (m_config.runSurface) {
        chunk = phaseSurface(chunk);
        if (targetStatus == ChunkStatus::SURFACE) return chunk;
    }

    if (m_config.runCarvers) {
        chunk = phaseCarvers(chunk);
        if (targetStatus == ChunkStatus::CARVERS) return chunk;
    }

    if (m_config.runFeatures) {
        chunk = phaseFeatures(chunk);
        if (targetStatus == ChunkStatus::FEATURES) return chunk;
    }

    // NOTE: Commented out - only care about blocks, not lighting
    // if (m_config.runLighting) {
    //     chunk = phaseInitializeLight(chunk);
    //     if (targetStatus == ChunkStatus::INITIALIZE_LIGHT) return chunk;
    //
    //     chunk = phaseLight(chunk);
    //     if (targetStatus == ChunkStatus::LIGHT) return chunk;
    // }

    // NOTE: Commented out - only care about blocks, not mobs
    // if (m_config.runSpawn) {
    //     chunk = phaseSpawn(chunk);
    //     if (targetStatus == ChunkStatus::SPAWN) return chunk;
    // }

    chunk = phaseFull(chunk);
    return chunk;
}

//=============================================================================
// Individual Phase Implementations
// Each matches ChunkStatusTasks.java exactly
//=============================================================================

::world::IChunk* ChunkGenerationRunner::phaseEmpty(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java passThrough() lines 36-38
    // This is a no-op - the chunk is already empty
    updateChunkStatus(chunk, ChunkStatus::EMPTY);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseStructureStarts(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateStructureStarts() lines 40-48
    // Reference: context.generator().createStructures(registryAccess, chunkPosStream, structureManager, chunk)
    //
    // For terrain-only generation, this is a no-op.
    // Full implementation would call generator->createStructures()

    updateChunkStatus(chunk, ChunkStatus::STRUCTURE_STARTS);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseStructureReferences(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateStructureReferences() lines 55-60
    // Reference: context.generator().createReferences(region, structureManager, chunk)
    //
    // For terrain-only generation, this is a no-op.
    // Full implementation would call generator->createReferences()

    updateChunkStatus(chunk, ChunkStatus::STRUCTURE_REFERENCES);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseBiomes(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateBiomes() lines 62-66
    // Exact call: context.generator().createBiomes(
    //     level.getChunkSource().randomState(),
    //     Blender.of(region),
    //     level.structureManager().forWorldGenRegion(region),
    //     chunk
    // )

    if (m_generator && m_randomState) {
        // Create blender - for fresh generation, use empty blender
        // Reference: Blender.of(region) - returns empty for non-upgrade scenarios
        Blender blender;  // Default empty blender

        m_generator->createBiomes(m_randomState, &blender, chunk);
    }

    updateChunkStatus(chunk, ChunkStatus::BIOMES);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseNoise(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateNoise() lines 68-84
    // Exact call: context.generator().fillFromNoise(
    //     Blender.of(region),
    //     level.getChunkSource().randomState(),
    //     level.structureManager().forWorldGenRegion(region),
    //     chunk
    // )
    //
    // This is the CRITICAL phase for terrain parity.
    // fillFromNoise() triggers the entire density function evaluation:
    //   NoiseBasedChunkGenerator.fillFromNoise()
    //     -> doFill()
    //       -> NoiseChunk.getInterpolatedState()
    //         -> finalDensity.compute()
    //           -> [entire density function graph]
    //         -> aquifer.computeSubstance()
    //           -> Returns stone/air/water/lava

    if (m_generator && m_randomState) {
        Blender blender;  // Empty blender for fresh generation
        m_generator->fillFromNoise(m_randomState, &blender, chunk);
    }

    updateChunkStatus(chunk, ChunkStatus::NOISE);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseSurface(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateSurface() lines 86-91
    // Exact call: context.generator().buildSurface(
    //     region,
    //     level.structureManager().forWorldGenRegion(region),
    //     level.getChunkSource().randomState(),
    //     chunk
    // )
    //
    // This applies surface rules to convert stone to grass/dirt/sand/etc.
    // Surface rules evaluate based on:
    //   - Biome
    //   - Y level
    //   - Distance from surface
    //   - Noise values (for variation like coarse dirt patches)

    if (m_generator && m_randomState) {
        // FIX: Use biomeSource directly instead of chunk to avoid & 3 coordinate wrapping
        // When using chunk as NoiseBiomeSource, queries to quarts outside this chunk's bounds
        // get wrapped via (quartX & 3) and return WRONG biomes from the chunk's local data.
        // This causes incorrect biome selection at chunk boundaries (fiddling may select wrong corner).
        //
        // Solution: Create a wrapper NoiseBiomeSource that queries the actual biomeSource directly.
        // This matches what Minecraft does in a real world where all chunks exist.

        // Helper class to wrap BiomeSource + RandomState into a NoiseBiomeSource
        class BiomeSourceWrapper : public world::biome::BiomeManager::NoiseBiomeSource {
        public:
            world::biome::BiomeSource* biomeSource;
            RandomState* randomState;

            BiomeSourceWrapper(world::biome::BiomeSource* bs, RandomState* rs)
                : biomeSource(bs), randomState(rs) {}

            world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                // Query biomeSource directly - no coordinate wrapping
                world::biome::BiomeKey key = biomeSource->getNoiseBiome(quartX, quartY, quartZ, *randomState->sampler());
                return world::biome::Biomes::get(key);
            }
        };

        auto* biomeSource = m_generator->getBiomeSource();
        if (biomeSource) {
            BiomeSourceWrapper biomeSourceWrapper(biomeSource, m_randomState);
            int64_t obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(m_seed);
            world::biome::BiomeManager biomeManager(&biomeSourceWrapper, obfuscatedSeed);

            // Create biome getter that uses BiomeManager.getBiome() with fiddling
            // Reference: SurfaceSystem.java line 97:
            //   SurfaceRules.Context context = new SurfaceRules.Context(..., biomeManager::getBiome, ...)
            BiomeGetter biomeGetter = [&biomeManager](const core::BlockPos& pos) -> world::biome::BiomeHolder {
                return biomeManager.getBiome(pos);
            };

            m_generator->buildSurface(m_randomState, biomeGetter, chunk);
        }
    }

    updateChunkStatus(chunk, ChunkStatus::SURFACE);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseCarvers(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateCarvers() lines 93-102
    // Exact call: context.generator().applyCarvers(
    //     region,
    //     level.getSeed(),
    //     level.getChunkSource().randomState(),
    //     level.getBiomeManager(),
    //     level.structureManager().forWorldGenRegion(region),
    //     chunk
    // )
    //
    // Note: For upgrades, there's also Blender.addAroundOldChunksCarvingMaskFilter()
    // but for fresh generation this is skipped.
    //
    // Carvers run for GenerationStep.Carving.AIR (caves, canyons)
    // and potentially GenerationStep.Carving.LIQUID (underwater caves)

    if (m_generator && m_randomState) {
        // FIX: Use biomeSource directly instead of chunk to avoid & 3 coordinate wrapping
        // (Same fix as in phaseSurface - see comment there for details)

        class BiomeSourceWrapper : public world::biome::BiomeManager::NoiseBiomeSource {
        public:
            world::biome::BiomeSource* biomeSource;
            RandomState* randomState;

            BiomeSourceWrapper(world::biome::BiomeSource* bs, RandomState* rs)
                : biomeSource(bs), randomState(rs) {}

            world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
                world::biome::BiomeKey key = biomeSource->getNoiseBiome(quartX, quartY, quartZ, *randomState->sampler());
                return world::biome::Biomes::get(key);
            }
        };

        auto* biomeSource = m_generator->getBiomeSource();
        if (biomeSource) {
            BiomeSourceWrapper biomeSourceWrapper(biomeSource, m_randomState);
            int64_t obfuscatedSeed = world::biome::BiomeManager::obfuscateSeed(m_seed);
            world::biome::BiomeManager biomeManager(&biomeSourceWrapper, obfuscatedSeed);

            BiomeGetter biomeGetter = [&biomeManager](const core::BlockPos& pos) -> world::biome::BiomeHolder {
                return biomeManager.getBiome(pos);
            };

            // Apply air carvers (caves and canyons)
            m_generator->applyCarvers(
                m_seed,
                m_randomState,
                biomeGetter,
                chunk,
                GenerationStep::Decoration::RAW_GENERATION  // Maps to Carving.AIR
            );
        }
    }

    updateChunkStatus(chunk, ChunkStatus::CARVERS);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseFeatures(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateFeatures() lines 104-114
    // Before features: Heightmap.primeHeightmaps(chunk, EnumSet.of(
    //     MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE
    // ))
    // Exact call: context.generator().applyBiomeDecoration(
    //     region,
    //     chunk,
    //     level.structureManager().forWorldGenRegion(region)
    // )
    //
    // Features include: ores, trees, flowers, grass, lakes, dungeons, etc.
    // This requires the PlacedFeature system to be fully implemented.

    // For now, this is a placeholder - full implementation requires:
    // 1. PlacedFeature registry
    // 2. BiomeGenerationSettings per biome
    // 3. Decoration seeding system

    updateChunkStatus(chunk, ChunkStatus::FEATURES);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseInitializeLight(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java initializeLight() lines 116-122
    // Exact calls:
    //   chunk.initializeLightSources()
    //   protoChunk.setLightEngine(lightEngine)
    //   return lightEngine.initializeLight(chunk, isLighted)
    //
    // For terrain-only generation, lighting is typically skipped.

    updateChunkStatus(chunk, ChunkStatus::INITIALIZE_LIGHT);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseLight(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java light() lines 124-127
    // Exact call: lightEngine.lightChunk(chunk, isLighted)
    //
    // This calculates block light and sky light propagation.
    // For terrain-only generation, this is typically skipped.

    updateChunkStatus(chunk, ChunkStatus::LIGHT);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseSpawn(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java generateSpawn() lines 129-135
    // Exact call (if not upgrading):
    //   context.generator().spawnOriginalMobs(new WorldGenRegion(...))
    //
    // This spawns initial creatures like animals, guardians in monuments, etc.

    if (m_generator) {
        m_generator->spawnOriginalMobs(chunk);
    }

    updateChunkStatus(chunk, ChunkStatus::SPAWN);
    return chunk;
}

::world::IChunk* ChunkGenerationRunner::phaseFull(::world::IChunk* chunk) {
    // Reference: ChunkStatusTasks.java full() lines 137-165
    // This converts ProtoChunk to LevelChunk in Minecraft.
    // In our C++ implementation, we just mark the chunk as complete.
    //
    // The Java code does:
    //   1. Check if already a LevelChunk
    //   2. Upgrade ProtoChunk -> LevelChunk
    //   3. Transfer all data (blocks, entities, block entities, etc.)

    updateChunkStatus(chunk, ChunkStatus::FULL);
    return chunk;
}

//=============================================================================
// Helper Methods
//=============================================================================

ChunkGenerationRunner::BiomeGetter ChunkGenerationRunner::createBiomeGetter(
    ::world::IChunk* chunk
) const {
    // Create a function that returns the biome at a given position
    // This is used by carvers and features to check biome boundaries

    return [chunk](const core::BlockPos& pos) -> world::biome::BiomeHolder {
        return chunk->getBiome(pos);
    };
}

void ChunkGenerationRunner::updateChunkStatus(
    ::world::IChunk* chunk,
    const ChunkStatus& status
) {
    // Update the chunk's persisted status
    // Reference: ChunkStep.java completeChunkGeneration() lines 28-40
    //   if (protochunk.getPersistedStatus().isBefore(targetStatus))
    //       protochunk.setPersistedStatus(targetStatus)

    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (protoChunk) {
        const ChunkStatus* currentStatus = protoChunk->getPersistedStatus();
        if (currentStatus == nullptr || currentStatus->isBefore(status)) {
            protoChunk->setStatus(&status);
        }
    }
}

} // namespace levelgen
} // namespace minecraft
