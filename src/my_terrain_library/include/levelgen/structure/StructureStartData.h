#pragma once

#include "world/ChunkPos.h"
#include <map>
#include <string>
#include <vector>

namespace minecraft {
namespace levelgen {
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
};

struct StructurePieceData {
    std::string pieceType;
    BoundingBox boundingBox;
};

struct StructureStartData {
    std::string structureName;
    world::ChunkPos startChunkPos;
    int references = 0;
    BoundingBox boundingBox;
    std::vector<StructurePieceData> pieces;

    bool isValid() const {
        return !pieces.empty();
    }
};

using StructureStartMap = std::map<std::string, StructureStartData>;
using StructureReferenceMap = std::map<std::string, std::set<int64_t>>;

} // namespace structure
} // namespace levelgen
} // namespace minecraft
