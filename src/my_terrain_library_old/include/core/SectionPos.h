#pragma once

#include <cstdint>

namespace minecraft {
namespace core {

/**
 * Section position utilities
 * Reference: SectionPos.java
 *
 * Section coordinates are block coordinates divided by 16
 * A section is 16×16×16 blocks
 */
class SectionPos {
public:
    // Constants (SectionPos.java lines 17-21)
    static constexpr int32_t SECTION_BITS = 4;
    static constexpr int32_t SECTION_SIZE = 16;
    static constexpr int32_t SECTION_MASK = 15;

    /**
     * Convert block coordinate to section coordinate
     * Reference: SectionPos.java lines 80-82
     *
     * Example: blockToSectionCoord(17) = 1
     *          blockToSectionCoord(-17) = -2
     */
    static int32_t blockToSectionCoord(int32_t blockCoord) {
        return blockCoord >> 4;  // Arithmetic right shift (preserves sign)
    }

    static int32_t blockToSectionCoord(double coord);

    /**
     * Convert section coordinate to block coordinate (min block in section)
     * Reference: SectionPos.java lines 127-129
     *
     * Example: sectionToBlockCoord(1) = 16
     *          sectionToBlockCoord(-2) = -32
     */
    static int32_t sectionToBlockCoord(int32_t sectionCoord) {
        return sectionCoord << 4;  // Multiply by 16
    }

    /**
     * Convert section coordinate to block coordinate with offset
     * Reference: SectionPos.java lines 131-133
     *
     * Example: sectionToBlockCoord(1, 5) = 21  (16 + 5)
     */
    static int32_t sectionToBlockCoord(int32_t sectionCoord, int32_t offset) {
        return sectionToBlockCoord(sectionCoord) + offset;
    }

    /**
     * Get relative position within section (0-15)
     * Reference: SectionPos.java lines 88-90
     */
    static int32_t sectionRelative(int32_t blockCoord) {
        return blockCoord & SECTION_MASK;
    }
};

} // namespace core
} // namespace minecraft
