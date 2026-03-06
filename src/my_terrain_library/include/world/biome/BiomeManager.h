#pragma once

#include "world/biome/Biome.h"
#include "core/BlockPos.h"
#include <cstdint>

namespace minecraft {
namespace world {
namespace biome {

/**
 * BiomeManager - Manages biome lookups with zooming and blending
 *
 * Key features:
 * - Converts block positions to quart positions (divide by 4)
 * - Uses zoom factor of 4 for smooth biome transitions
 * - Applies random "fiddling" to biome boundaries
 * - Delegates actual biome lookups to NoiseBiomeSource
 *
 * Reference: net/minecraft/world/level/biome/BiomeManager.java
 */
class BiomeManager {
public:
    /**
     * NoiseBiomeSource interface - provides biome lookups at quart positions
     * Reference: BiomeManager.java lines 105-107
     */
    class NoiseBiomeSource {
    public:
        virtual ~NoiseBiomeSource() = default;

        /**
         * Get biome at quart coordinates
         * @param quartX - X coordinate in quart units (blocks / 4)
         * @param quartY - Y coordinate in quart units
         * @param quartZ - Z coordinate in quart units
         * @return Biome at that position
         */
        virtual BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const = 0;
    };

private:
    // Reference: BiomeManager.java lines 12-14
    static constexpr int32_t ZOOM_BITS = 2;
    static constexpr int32_t ZOOM = 4;
    static constexpr int32_t ZOOM_MASK = 3;

    const NoiseBiomeSource* m_noiseBiomeSource;  // Reference: BiomeManager.java line 15
    int64_t m_biomeZoomSeed;                     // Reference: BiomeManager.java line 16

    /**
     * Calculate "fiddled" distance with random perturbation
     * Reference: BiomeManager.java lines 85-98
     *
     * This adds small random offsets to grid points to prevent blocky biome boundaries.
     *
     * @param seed - Base seed for randomness
     * @param xRandom - X coordinate for seed mixing
     * @param yRandom - Y coordinate for seed mixing
     * @param zRandom - Z coordinate for seed mixing
     * @param distanceX - Distance in X direction
     * @param distanceY - Distance in Y direction
     * @param distanceZ - Distance in Z direction
     * @return Squared distance with random perturbation
     */
    static double getFiddledDistance(int64_t seed, int32_t xRandom, int32_t yRandom, int32_t zRandom,
                                    double distanceX, double distanceY, double distanceZ);

    /**
     * Generate random fiddle value from seed
     * Reference: BiomeManager.java lines 100-103
     *
     * @param rval - Random value (seed state)
     * @return Random offset in range [-0.45, 0.45]
     */
    static double getFiddle(int64_t rval);

public:
    /**
     * Chunk center in quart coordinates
     * Reference: BiomeManager.java line 11
     */
    static constexpr int32_t CHUNK_CENTER_QUART = 2;  // QuartPos.fromBlock(8)

    /**
     * Constructor
     * Reference: BiomeManager.java lines 18-21
     *
     * @param noiseBiomeSource - Source for biome lookups
     * @param seed - Random seed for biome zooming
     */
    BiomeManager(const NoiseBiomeSource* noiseBiomeSource, int64_t seed);

    /**
     * Obfuscate seed using SHA-256
     * Reference: BiomeManager.java lines 23-25
     *
     * NOTE: This requires SHA-256 hashing. For now, we'll implement a simple version.
     * The full implementation should use a proper SHA-256 library.
     *
     * @param seed - Input seed
     * @return Obfuscated seed
     */
    static int64_t obfuscateSeed(int64_t seed);

    /**
     * Create a new BiomeManager with a different biome source
     * Reference: BiomeManager.java lines 27-29
     *
     * @param biomeSource - New biome source
     * @return New BiomeManager with same seed but different source
     */
    BiomeManager withDifferentSource(const NoiseBiomeSource* biomeSource) const;

    /**
     * Get biome at a block position (with zooming/blending)
     * Reference: BiomeManager.java lines 31-65
     *
     * This is the main method that implements biome zooming:
     * 1. Offset position by (-2, -2, -2)
     * 2. Convert to parent quart coordinates
     * 3. Find 8 surrounding quart positions (cube corners)
     * 4. Calculate fiddled distance to each corner
     * 5. Return biome from nearest corner
     *
     * @param pos - Block position
     * @return Biome at that position
     */
    BiomeHolder getBiome(const core::BlockPos& pos) const;

    /**
     * Get biome at double position (with zooming)
     * Reference: BiomeManager.java lines 67-72
     *
     * @param x - X coordinate (blocks, can be fractional)
     * @param y - Y coordinate (blocks, can be fractional)
     * @param z - Z coordinate (blocks, can be fractional)
     * @return Biome at that position
     */
    BiomeHolder getNoiseBiomeAtPosition(double x, double y, double z) const;

    /**
     * Get biome at block position (direct lookup, no zooming)
     * Reference: BiomeManager.java lines 74-79
     *
     * @param blockPos - Block position
     * @return Biome at that quart position
     */
    BiomeHolder getNoiseBiomeAtPosition(const core::BlockPos& blockPos) const;

    /**
     * Get biome at quart coordinates (direct lookup, no zooming)
     * Reference: BiomeManager.java lines 81-83
     *
     * @param quartX - X in quart units
     * @param quartY - Y in quart units
     * @param quartZ - Z in quart units
     * @return Biome at that position
     */
    BiomeHolder getNoiseBiomeAtQuart(int32_t quartX, int32_t quartY, int32_t quartZ) const;
};

} // namespace biome
} // namespace world
} // namespace minecraft
