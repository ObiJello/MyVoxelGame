#pragma once

#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/Blender.h"
#include "levelgen/GenerationStep.h"
#include "world/IChunk.h"
#include "world/ProtoChunk.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/biome/Biome.h"
#include "core/BlockPos.h"
#include "util/CompletableFuture.h"
#include <cstdint>
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Reference: Flattened version of ChunkStatusTasks.java / ChunkGenerationTask.java
// This provides a sequential, single-threaded execution of chunk generation phases
// for exact bit parity with Minecraft's world generation.

namespace minecraft {
namespace levelgen {

/**
 * ChunkGenerationRunner - Sequential chunk generation orchestrator
 *
 * This class provides a flattened version of Minecraft's ChunkStatusTasks system.
 * Instead of using the async task-based ChunkGenerationTask system, this executes
 * each generation phase sequentially in the exact order Minecraft does.
 *
 * For 100% bit parity, call generateChunk() with the same:
 *   - World seed
 *   - Chunk position
 *   - RandomState (properly initialized from seed)
 *   - ChunkGenerator (properly configured with NoiseGeneratorSettings)
 *
 * Phase order (matching ChunkStatus progression):
 *   1. EMPTY         - Initial empty chunk (passthrough)
 *   2. STRUCTURE_STARTS - Structure placement starts (optional for terrain-only)
 *   3. STRUCTURE_REFERENCES - Structure references (optional for terrain-only)
 *   4. BIOMES        - Biome assignment via createBiomes()
 *   5. NOISE         - Base terrain via fillFromNoise()
 *   6. SURFACE       - Surface blocks via buildSurface()
 *   7. CARVERS       - Cave carving via applyCarvers()
 *   8. FEATURES      - Decorations via applyBiomeDecoration()
 *   9. INITIALIZE_LIGHT - Light engine setup
 *  10. LIGHT         - Light calculation
 *  11. SPAWN         - Initial mob spawning
 *  12. FULL          - Convert to LevelChunk
 *
 * Reference: ChunkStatusTasks.java, ChunkGenerationTask.java
 */
class ChunkGenerationRunner {
public:
    /**
     * Configuration for what phases to run
     */
    struct Config {
        bool runStructures = false;      // STRUCTURE_STARTS and STRUCTURE_REFERENCES
        bool runBiomes = true;           // BIOMES - required for surface rules
        bool runNoise = true;            // NOISE - base terrain (the main phase)
        bool runSurface = true;          // SURFACE - grass, sand, etc.
        bool runCarvers = true;          // CARVERS - caves and canyons
        bool runFeatures = false;        // FEATURES - ores, trees (requires more setup)
        bool runLighting = false;        // INITIALIZE_LIGHT and LIGHT
        bool runSpawn = false;           // SPAWN - mob spawning

        // Convenience presets
        static Config terrainOnly() {
            return Config{false, true, true, true, false, false, false, false};
        }

        static Config terrainWithCaves() {
            return Config{false, true, true, true, true, false, false, false};
        }

        static Config full() {
            return Config{true, true, true, true, true, true, true, true};
        }
    };

private:
    NoiseBasedChunkGenerator* m_generator;
    RandomState* m_randomState;
    int64_t m_seed;
    Config m_config;

    // Biome getter function type
    using BiomeGetter = std::function<world::biome::BiomeHolder(const core::BlockPos&)>;

public:
    /**
     * Constructor
     *
     * @param generator - The chunk generator (must be NoiseBasedChunkGenerator for terrain)
     * @param randomState - The random state initialized from world seed
     * @param seed - The world seed
     * @param config - Configuration for which phases to run
     */
    ChunkGenerationRunner(
        NoiseBasedChunkGenerator* generator,
        RandomState* randomState,
        int64_t seed,
        Config config = Config::terrainWithCaves()
    );

    /**
     * Generate a complete chunk through all configured phases
     *
     * This is the main entry point. It runs each phase in order,
     * exactly matching Minecraft's ChunkStatusTasks execution order.
     *
     * @param chunk - The ProtoChunk to generate into
     * @return The generated chunk (same pointer, for chaining)
     *
     * Reference: ChunkGenerationTask.java runUntilWait() -> scheduleNextLayer()
     */
    ::world::IChunk* generateChunk(::world::IChunk* chunk);

    /**
     * Generate a chunk up to a specific status
     *
     * Runs all phases from EMPTY up to and including the target status.
     *
     * @param chunk - The ProtoChunk to generate into
     * @param targetStatus - Stop after reaching this status
     * @return The generated chunk
     */
    ::world::IChunk* generateChunkToStatus(
        ::world::IChunk* chunk,
        const world::chunk::status::ChunkStatus& targetStatus
    );

    // Individual phase methods - can be called directly for fine control
    // Each matches the corresponding ChunkStatusTasks method exactly

    /**
     * Phase 1: EMPTY - passthrough, chunk is already empty
     * Reference: ChunkStatusTasks.java passThrough() lines 36-38
     */
    ::world::IChunk* phaseEmpty(::world::IChunk* chunk);

    /**
     * Phase 2: STRUCTURE_STARTS - create structure starting points
     * Reference: ChunkStatusTasks.java generateStructureStarts() lines 40-48
     */
    ::world::IChunk* phaseStructureStarts(::world::IChunk* chunk);

    /**
     * Phase 3: STRUCTURE_REFERENCES - link structures across chunks
     * Reference: ChunkStatusTasks.java generateStructureReferences() lines 55-60
     */
    ::world::IChunk* phaseStructureReferences(::world::IChunk* chunk);

    /**
     * Phase 4: BIOMES - assign biomes to chunk
     * Reference: ChunkStatusTasks.java generateBiomes() lines 62-66
     *
     * Calls: generator.createBiomes(randomState, blender, chunk)
     */
    ::world::IChunk* phaseBiomes(::world::IChunk* chunk);

    /**
     * Phase 5: NOISE - generate base terrain
     * Reference: ChunkStatusTasks.java generateNoise() lines 68-84
     *
     * This is the MAIN terrain generation phase.
     * Calls: generator.fillFromNoise(blender, randomState, chunk)
     *
     * After this phase, the chunk contains stone/air/water/lava
     * based on density functions and aquifer logic.
     */
    ::world::IChunk* phaseNoise(::world::IChunk* chunk);

    /**
     * Phase 6: SURFACE - apply surface blocks
     * Reference: ChunkStatusTasks.java generateSurface() lines 86-91
     *
     * Calls: generator.buildSurface(randomState, chunk)
     *
     * After this phase, stone is replaced with grass/dirt/sand/etc.
     * based on biome and surface rules.
     */
    ::world::IChunk* phaseSurface(::world::IChunk* chunk);

    /**
     * Phase 7: CARVERS - carve caves and canyons
     * Reference: ChunkStatusTasks.java generateCarvers() lines 93-102
     *
     * Calls: generator.applyCarvers(seed, randomState, biomeGetter, chunk, step)
     *
     * After this phase, caves have been carved into the terrain.
     */
    ::world::IChunk* phaseCarvers(::world::IChunk* chunk);

    /**
     * Phase 8: FEATURES - place decorations
     * Reference: ChunkStatusTasks.java generateFeatures() lines 104-114
     *
     * Calls: generator.applyBiomeDecoration(seed, chunk, biomeGetter, features)
     *
     * After this phase, ores/trees/flowers have been placed.
     */
    ::world::IChunk* phaseFeatures(::world::IChunk* chunk);

    /**
     * Phase 9: INITIALIZE_LIGHT - prepare lighting engine
     * Reference: ChunkStatusTasks.java initializeLight() lines 116-122
     */
    ::world::IChunk* phaseInitializeLight(::world::IChunk* chunk);

    /**
     * Phase 10: LIGHT - calculate block/sky light
     * Reference: ChunkStatusTasks.java light() lines 124-127
     */
    ::world::IChunk* phaseLight(::world::IChunk* chunk);

    /**
     * Phase 11: SPAWN - spawn initial mobs
     * Reference: ChunkStatusTasks.java generateSpawn() lines 129-135
     */
    ::world::IChunk* phaseSpawn(::world::IChunk* chunk);

    /**
     * Phase 12: FULL - finalize chunk
     * Reference: ChunkStatusTasks.java full() lines 137-165
     */
    ::world::IChunk* phaseFull(::world::IChunk* chunk);

    // Accessors
    NoiseBasedChunkGenerator* getGenerator() const { return m_generator; }
    RandomState* getRandomState() const { return m_randomState; }
    int64_t getSeed() const { return m_seed; }
    const Config& getConfig() const { return m_config; }

    // Setters for reconfiguration
    void setConfig(const Config& config) { m_config = config; }
    void setGenerator(NoiseBasedChunkGenerator* generator) { m_generator = generator; }
    void setRandomState(RandomState* randomState) { m_randomState = randomState; }
    void setSeed(int64_t seed) { m_seed = seed; }

private:
    /**
     * Get a biome getter function for the chunk
     * Used by carvers and features
     */
    BiomeGetter createBiomeGetter(::world::IChunk* chunk) const;

    /**
     * Update chunk status after completing a phase
     */
    void updateChunkStatus(::world::IChunk* chunk, const world::chunk::status::ChunkStatus& status);
};

} // namespace levelgen
} // namespace minecraft
