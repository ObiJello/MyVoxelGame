#pragma once

#include <cstdint>

// Reference: net/minecraft/core/QuartPos.java

namespace minecraft {
namespace core {

/**
 * QuartPos - Quart position utilities
 *
 * A "quart" is a quarter-chunk position. Since biomes are sampled at 4-block intervals,
 * quart positions are used for biome coordinates.
 *
 * Reference: QuartPos.java
 */
class QuartPos {
public:
    // Reference: QuartPos.java lines 4-6
    static constexpr int32_t BITS = 2;
    static constexpr int32_t SIZE = 4;
    static constexpr int32_t MASK = 3;

    // Reference: QuartPos.java line 7
    static constexpr int32_t SECTION_TO_QUARTS_BITS = 2;

    /**
     * Convert block coordinate to quart coordinate
     * Reference: QuartPos.java lines 12-14
     */
    static int32_t fromBlock(int32_t blockCoord) {
        return blockCoord >> 2;
    }

    /**
     * Get local quart coordinate within a block
     * Reference: QuartPos.java lines 16-18
     */
    static int32_t quartLocal(int32_t blockCoord) {
        return blockCoord & 3;
    }

    /**
     * Convert quart coordinate to block coordinate
     * Reference: QuartPos.java lines 20-22
     */
    static int32_t toBlock(int32_t quart) {
        return quart << 2;
    }

    /**
     * Convert section coordinate to quart coordinate
     * Reference: QuartPos.java lines 24-26
     */
    static int32_t fromSection(int32_t section) {
        return section << 2;
    }

    /**
     * Convert quart coordinate to section coordinate
     * Reference: QuartPos.java lines 28-30
     */
    static int32_t toSection(int32_t quart) {
        return quart >> 2;
    }
};

} // namespace core
} // namespace minecraft
