// File: src/game/WorldMath.hpp
#pragma once

#include <cstdint>

namespace Game::Math {

    // A chunk is 16×16×384; each chunk is subdivided into 24 sections of height 16.
    constexpr int CHUNK_SIZE_X         = 16;               // X width
    constexpr int CHUNK_SIZE_Z         = 16;               // Z depth
    constexpr int SECTION_HEIGHT       = 16;               // Y height of a sub-chunk
    constexpr int SECTIONS_PER_CHUNK   = 24;               // 384 / 16 = 24 (FIXED: was 16, now 24)
    constexpr int CHUNK_TOTAL_HEIGHT   = SECTION_HEIGHT * SECTIONS_PER_CHUNK; // 384

    // 2D chunk grid position (which chunk in world)
    struct ChunkPos {
        int32_t x;
        int32_t z;
    };

    // Equality comparison so ChunkPos can be used in containers
    inline constexpr bool operator==(const ChunkPos& a, const ChunkPos& b) noexcept {
        return a.x == b.x && a.z == b.z;
    }

    inline constexpr bool operator!=(const ChunkPos& a, const ChunkPos& b) noexcept {
        return !(a == b);
    }

    // Convert a local (x [0..15], y [0..15], z [0..15]) within a single section
    // into a linear index (0..4095). Memory layout is Y-major, then Z, then X.
    inline constexpr uint32_t LocalIndex(int x, int y, int z) {
        // (y * CHUNK_SIZE_X + z) * CHUNK_SIZE_Z + x
        return static_cast<uint32_t>(( (y * CHUNK_SIZE_X) + z ) * CHUNK_SIZE_Z + x);
    }

} // namespace Game::Math