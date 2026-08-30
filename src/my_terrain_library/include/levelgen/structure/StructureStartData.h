#pragma once

#include "world/ChunkPos.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
class ChunkGenerator;
class WorldgenRandom;
namespace structure {

struct BoundingBox {
    int minX = 0;
    int minY = 0;
    int minZ = 0;
    int maxX = 0;
    int maxY = 0;
    int maxZ = 0;

    BoundingBox() = default;

    BoundingBox(int x1, int y1, int z1, int x2, int y2, int z2)
        : minX(x1)
        , minY(y1)
        , minZ(z1)
        , maxX(x2)
        , maxY(y2)
        , maxZ(z2)
    {}

    bool intersects(int x1, int z1, int x2, int z2) const {
        return maxX >= x1 && minX <= x2 && maxZ >= z1 && minZ <= z2;
    }

    // Reference: BoundingBox.java intersects(BoundingBox) - full 3D test.
    bool intersects(const BoundingBox& other) const {
        return maxX >= other.minX && minX <= other.maxX
            && maxZ >= other.minZ && minZ <= other.maxZ
            && maxY >= other.minY && minY <= other.maxY;
    }

    void move(int dx, int dy, int dz) {
        minX += dx; minY += dy; minZ += dz;
        maxX += dx; maxY += dy; maxZ += dz;
    }

    int getXSpan() const { return maxX - minX + 1; }
    int getYSpan() const { return maxY - minY + 1; }
    int getZSpan() const { return maxZ - minZ + 1; }

    // Reference: BoundingBox.getCenter() - (max - min + 1) / 2 offsets.
    int centerX() const { return minX + (maxX - minX + 1) / 2; }
    int centerY() const { return minY + (maxY - minY + 1) / 2; }
    int centerZ() const { return minZ + (maxZ - minZ + 1) / 2; }

    // Reference: BoundingBox.isInside(BlockPos).
    bool isInside(int x, int y, int z) const {
        return x >= minX && x <= maxX && y >= minY && y <= maxY
            && z >= minZ && z <= maxZ;
    }
};

struct StructurePieceData {
    std::string pieceType;          // registry id, e.g. "minecraft:tesh"
    BoundingBox boundingBox;
    // P-line fields (FORMAT.md): rotation enum constant name or "-" when the
    // piece has no orientation; genDepth; piece-kind-specific detail.
    std::string rotation = "-";
    int genDepth = 0;
    std::string detail = "-";
    // Structured mirror of the jigsaw fields Beardifier consumes (they are
    // also encoded in `detail` for the P line, but Beardifier must not parse
    // strings). Reference: Beardifier.forStructuresInChunk - only
    // PoolElementStructurePiece with RIGID projection becomes a Rigid;
    // junctions contribute regardless of projection.
    bool poolElement = false;       // piece is a PoolElementStructurePiece
    bool rigidProjection = false;   // element projection == "rigid"
    int groundLevelDelta = 0;
    std::vector<std::array<int, 4>> junctions;  // sourceX, sourceGroundY, sourceZ, deltaY
};

class StructurePieceBehavior;

struct StructureStartData {
    std::string structureName;
    world::ChunkPos startChunkPos;
    int references = 0;
    BoundingBox boundingBox;
    std::vector<StructurePieceData> pieces;
    // Block-placement behaviors, parallel to `pieces` (index i drives piece i;
    // may be shorter/empty for families without placement support yet).
    // shared_ptr so map copies of the start share one behavior instance.
    std::vector<std::shared_ptr<StructurePieceBehavior>> behaviors;
    // Reference: Structure.afterPlace - runs after the piece loop in
    // StructureStart.placeInChunk. Null == Java's default no-op.
    std::function<void(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&,
                       const BoundingBox& /*chunkBB*/, const world::ChunkPos&,
                       StructureStartData&)> afterPlace;

    bool isValid() const {
        return !pieces.empty();
    }
};

using StructureStartMap = std::map<std::string, StructureStartData>;
// References in INSERTION order (Java LongOpenHashSet contents are built by
// the createReferences grid scan; insertion order feeds the linear-probing
// layout that fastutilLongSetOrder replays).
using StructureReferenceMap = std::map<std::string, std::vector<int64_t>>;

/**
 * Reference: it.unimi.dsi.fastutil.longs.LongOpenHashSet iteration order -
 * Java's per-chunk reference sets iterate in HASH TABLE order, not sorted
 * order, and StructureStart.placeInChunk draws interleave across starts in
 * that order. Default table: expected 16, f 0.75 -> n = 32 slots, mask 31;
 * pos = (int)HashCommon.mix(key) & mask with linear probing (insertion order
 * resolves collisions); the iterator yields the 0 key first (containsNull)
 * then scans key[n-1] down to key[0]. Reference counts never approach the
 * rehash threshold (maxFill 24).
 */
inline std::vector<int64_t> fastutilLongSetOrder(const std::vector<int64_t>& insertionOrder) {
    constexpr int kSlots = 32;
    constexpr int kMask = kSlots - 1;
    int64_t table[kSlots] = {};
    bool used[kSlots] = {};
    bool containsNull = false;
    for (int64_t key : insertionOrder) {
        if (key == 0) {
            containsNull = true;
            continue;
        }
        // Reference: HashCommon.mix(long) - LONG_PHI multiply + xorshifts.
        uint64_t h = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 32;
        h ^= h >> 16;
        int pos = static_cast<int>(h & kMask);
        bool duplicate = false;
        while (used[pos]) {
            if (table[pos] == key) {
                duplicate = true;
                break;
            }
            pos = (pos + 1) & kMask;
        }
        if (!duplicate) {
            used[pos] = true;
            table[pos] = key;
        }
    }
    std::vector<int64_t> result;
    result.reserve(insertionOrder.size());
    if (containsNull) result.push_back(0);
    for (int pos = kSlots - 1; pos >= 0; --pos) {
        if (used[pos]) result.push_back(table[pos]);
    }
    return result;
}

} // namespace structure
} // namespace levelgen
} // namespace minecraft
