#pragma once

#include "random/PositionalRandomFactory.h"
#include "world/level/block/state/BlockState.h"
#include "world/IChunk.h"
#include <cstdint>
#include <vector>
#include <functional>

// Forward declarations
namespace minecraft {
    class NormalNoise;
    namespace levelgen {
        class RandomState;
        class NoiseChunk;
        class RuleSource;
        class WorldGenerationContext;
    }
    namespace core {
        class BlockPos;
    }
}

// Reference: net/minecraft/world/level/levelgen/SurfaceSystem.java

namespace minecraft {
namespace levelgen {

/**
 * SurfaceSystem - Applies surface rules to replace default blocks
 *
 * This class handles:
 * - Surface block replacement (stone -> grass, dirt, sand, etc.)
 * - Badlands clay band generation
 * - Iceberg pillar generation
 * - Surface noise for variation
 *
 * Reference: SurfaceSystem.java lines 27-325
 */
class SurfaceSystem {
private:
    // Reference: SurfaceSystem.java lines 37-38
    BlockState* m_defaultBlock;
    int32_t m_seaLevel;

    // Reference: SurfaceSystem.java line 47
    random::PositionalRandomFactory* m_noiseRandom;

    // Clay bands for badlands biomes (Reference: line 39)
    std::vector<BlockState*> m_clayBands;

    // Noise instances for surface generation
    // Reference: SurfaceSystem.java lines 40-49
    NormalNoise* m_clayBandsOffsetNoise;
    NormalNoise* m_badlandsPillarNoise;
    NormalNoise* m_badlandsPillarRoofNoise;
    NormalNoise* m_badlandsSurfaceNoise;
    NormalNoise* m_icebergPillarNoise;
    NormalNoise* m_icebergPillarRoofNoise;
    NormalNoise* m_icebergSurfaceNoise;
    NormalNoise* m_surfaceNoise;
    NormalNoise* m_surfaceSecondaryNoise;

    // Helper methods for clay band generation
    // Reference: SurfaceSystem.java lines 262-293
    static std::vector<BlockState*> generateBands(XoroshiroRandomSource& random);

    // Reference: SurfaceSystem.java lines 295-307
    static void makeBands(XoroshiroRandomSource& random,
                          std::vector<BlockState*>& clayBands,
                          int32_t baseWidth,
                          BlockState* state);

public:
    /**
     * Constructor
     * Reference: SurfaceSystem.java lines 51-65
     */
    SurfaceSystem(RandomState* randomState,
                  BlockState* defaultBlock,
                  int32_t seaLevel,
                  random::PositionalRandomFactory* noiseRandom);

    /**
     * Destructor - clean up allocated resources
     */
    ~SurfaceSystem();

    /**
     * Get surface depth at a position
     * Reference: SurfaceSystem.java lines 161-164
     *
     * @param blockX - Block X coordinate
     * @param blockZ - Block Z coordinate
     * @return Surface depth value
     */
    int32_t getSurfaceDepth(int32_t blockX, int32_t blockZ) const;

    /**
     * Get secondary surface noise value
     * Reference: SurfaceSystem.java lines 166-168
     *
     * @param blockX - Block X coordinate
     * @param blockZ - Block Z coordinate
     * @return Secondary surface noise value
     */
    double getSurfaceSecondary(int32_t blockX, int32_t blockZ) const;

    /**
     * Get clay band at a specific Y level for badlands
     * Reference: SurfaceSystem.java lines 309-312
     *
     * @param worldX - World X coordinate
     * @param y - Y level
     * @param worldZ - World Z coordinate
     * @return Block type for the clay band
     */
    BlockState* getBand(int32_t worldX, int32_t y, int32_t worldZ) const;

    /**
     * Get sea level
     * Reference: SurfaceSystem.java lines 174-176
     */
    int32_t getSeaLevel() const { return m_seaLevel; }

    /**
     * Build surface blocks for a chunk
     * Reference: SurfaceSystem.java lines 67-159
     *
     * @param randomState - Random state for noise generation
     * @param biomeGetter - Function to get biome at a position
     * @param useLegacyRandom - Whether to use legacy random for biome sampling
     * @param generationContext - World generation bounds context
     * @param chunk - The chunk to build surface on
     * @param noiseChunk - Noise chunk for preliminary surface
     * @param ruleSource - Surface rule source to apply
     */
    void buildSurface(
        RandomState* randomState,
        std::function<void*(const ::minecraft::core::BlockPos&)> biomeGetter,
        bool useLegacyRandom,
        const WorldGenerationContext& generationContext,
        ::world::IChunk* chunk,
        NoiseChunk* noiseChunk,
        RuleSource* ruleSource
    );

    // Accessors
    BlockState* defaultBlock() const { return m_defaultBlock; }

private:
    /**
     * Check if a block is "stone" (non-air, non-fluid)
     * Reference: SurfaceSystem.java lines 170-172
     */
    bool isStone(const BlockState* block) const;

    /**
     * Apply eroded badlands pillar extension
     * Reference: SurfaceSystem.java lines 192-219
     */
    void erodedBadlandsExtension(
        ::world::IChunk* chunk,
        int32_t blockX,
        int32_t blockZ,
        int32_t height
    );

    /**
     * Apply frozen ocean iceberg extension
     * Reference: SurfaceSystem.java lines 221-260
     */
    void frozenOceanExtension(
        int32_t minSurfaceLevel,
        const world::biome::Biome* surfaceBiome,
        ::world::IChunk* chunk,
        int32_t blockX,
        int32_t blockZ,
        int32_t height
    );
};

} // namespace levelgen
} // namespace minecraft
