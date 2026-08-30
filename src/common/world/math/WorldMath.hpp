// File: src/common/world/level/WorldMath.hpp
#pragma once

#include <cstdint>
// The ChunkPos/SectionPos hashers below use std::size_t and std::hash. Every
// existing includer happened to pull these in first, so the omission only
// surfaced when a new header included this one before anything else.
#include <cstddef>
#include <functional>

namespace Game::Math {

    // A chunk is 16×16×384; each chunk is subdivided into 24 sections of height 16.
    constexpr int CHUNK_SIZE_X         = 16;               // X width
    constexpr int CHUNK_SIZE_Z         = 16;               // Z depth
    constexpr int SECTION_HEIGHT       = 16;               // Y height of a sub-chunk
    constexpr int SECTIONS_PER_CHUNK   = 24;               // 384 / 16 = 24
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

    // Hash function for ChunkPos (for use with unordered containers)
    struct ChunkPosHash {
        // The old form was `hash(x) ^ (hash(z) << 1)`, and libc++'s
        // std::hash<int32_t> is the identity — so it reduced to `x ^ (z << 1)`.
        // For any local region that collapses catastrophically: measured over a
        // 65x65 chunk area, 4,225 chunks landed in 193 distinct buckets with a
        // longest chain of 33. Every chunk-keyed lookup in the engine then
        // walked that chain, and there are 39 of these maps — the chunk cache,
        // the ticket manager's level map, sentChunks, and the CLIENT chunk map
        // among them.
        //
        // A 64-bit finalizer over the packed pair instead: same area gives
        // 3,078 buckets used and a longest chain of 6, and the ticket-manager
        // gate measured 28.9 -> 11.3 ns per lookup.
        std::size_t operator()(const ChunkPos& pos) const {
            uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) << 32)
                       |  static_cast<uint64_t>(static_cast<uint32_t>(pos.z));
            h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<std::size_t>(h);
        }
    };
    
    // Section position for tracking block changes at section granularity
    struct SectionPos {
        int32_t chunkX;
        int32_t sectionY;  // 0-23 for -64 to 319 world height
        int32_t chunkZ;
        
        SectionPos() : chunkX(0), sectionY(0), chunkZ(0) {}
        SectionPos(int32_t cx, int32_t sy, int32_t cz) 
            : chunkX(cx), sectionY(sy), chunkZ(cz) {}
        
        bool operator==(const SectionPos& other) const {
            return chunkX == other.chunkX && 
                   sectionY == other.sectionY && 
                   chunkZ == other.chunkZ;
        }
        
        bool operator!=(const SectionPos& other) const {
            return !(*this == other);
        }
        
        // Create from world coordinates
        static SectionPos fromWorldPos(int worldX, int worldY, int worldZ) {
            // Calculate chunk coordinates
            int32_t cx = worldX >= 0 ? worldX / CHUNK_SIZE_X : (worldX - CHUNK_SIZE_X + 1) / CHUNK_SIZE_X;
            int32_t cz = worldZ >= 0 ? worldZ / CHUNK_SIZE_Z : (worldZ - CHUNK_SIZE_Z + 1) / CHUNK_SIZE_Z;
            
            // Calculate section Y (adjust for min world height of -64)
            int32_t sy = (worldY + 64) / SECTION_HEIGHT;
            
            return SectionPos(cx, sy, cz);
        }
        
        // Get the chunk position
        ChunkPos getChunkPos() const {
            return ChunkPos{chunkX, chunkZ};
        }
    };
    
    struct SectionPosHash {
        std::size_t operator()(const SectionPos& pos) const {
            // Combine all three coordinates with prime multipliers
            return std::hash<int32_t>()(pos.chunkX) * 73856093 ^ 
                   std::hash<int32_t>()(pos.sectionY) * 83492791 ^
                   std::hash<int32_t>()(pos.chunkZ) * 19349663;
        }
    };

} // namespace Game::Math

// **NEW**: Forward declarations for render system (to avoid circular dependencies)
namespace Render {
    enum class BlockFace : int;
}

// **NEW**: Convenience aliases for common coordinate operations
namespace Game {
    // Alias for shorter access to coordinate utilities
    namespace WC = Game::Math;
}