// File: src/client/renderer/culling/VisibilitySet.hpp
// Compact encoding of which direction pairs can see through a 16x16x16 section.
// Mirrors Minecraft's VisibilitySet: a 6x6 bit matrix (36 bits) where bit
// (from + to*6) indicates whether vision can pass from face `from` to face `to`.
#pragma once

#include <cstdint>

namespace Render {

    // Direction indices matching standard face ordering.
    // Must be consistent with VisGraph edge detection and SectionOcclusionGraph neighbor lookup.
    namespace Direction {
        static constexpr int Down  = 0;
        static constexpr int Up    = 1;
        static constexpr int North = 2;  // -Z
        static constexpr int South = 3;  // +Z
        static constexpr int West  = 4;  // -X
        static constexpr int East  = 5;  // +X
        static constexpr int Count = 6;

        // Returns the opposite direction (Down↔Up, North↔South, West↔East)
        inline constexpr int opposite(int dir) {
            return dir ^ 1;  // Works because pairs differ by bit 0: 0↔1, 2↔3, 4↔5
        }
    }

    class VisibilitySet {
    public:
        VisibilitySet() = default;

        // Mark that vision can pass between dir1 and dir2 (symmetric)
        void addPair(int dir1, int dir2) {
            m_bits |= (1ULL << (dir1 + dir2 * 6));
            m_bits |= (1ULL << (dir2 + dir1 * 6));
        }

        // Mark all direction pairs from a set of reachable faces
        // Called after flood-fill returns a bitmask of reached faces
        void addFaces(uint8_t faceMask) {
            for (int d1 = 0; d1 < 6; d1++) {
                if (!(faceMask & (1 << d1))) continue;
                for (int d2 = d1; d2 < 6; d2++) {
                    if (!(faceMask & (1 << d2))) continue;
                    addPair(d1, d2);
                }
            }
        }

        // Query: can vision pass from face `from` to face `to`?
        bool canSeeThrough(int from, int to) const {
            return (m_bits >> (from + to * 6)) & 1;
        }

        // Set all 36 bits (all-visible or all-invisible)
        void setAll(bool visible) {
            m_bits = visible ? 0x0000000FFFFFFFFFULL : 0;  // 36 bits set
        }

        bool isAllVisible() const { return m_bits == 0x0000000FFFFFFFFFULL; }

        uint64_t raw() const { return m_bits; }

    private:
        uint64_t m_bits = 0;
    };

} // namespace Render
