#pragma once

#include "levelgen/structure/StructureStartData.h"
#include "world/ChunkPos.h"
#include "core/BlockPos.h"
#include <memory>

// Reference: net/minecraft/world/level/levelgen/structure/StructurePiece.java
// postProcess() - the block-placement half of a structure piece (B6). Layout
// (bbox/rotation/detail) already lives in StructurePieceData; a behavior is
// the per-family placement logic paired with one StructurePieceData entry.

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
class ChunkGenerator;
class WorldgenRandom;
namespace structure {

class StructurePieceBehavior {
public:
    virtual ~StructurePieceBehavior() = default;

    /**
     * Reference: StructurePiece.postProcess(level, structureManager,
     * generator, random, chunkBB, chunkPos, referencePos).
     * `self` is the piece's data record inside the chunk-stored
     * StructureStartData - Java pieces mutate this.boundingBox (buried
     * treasure re-anchors; shipwreck/ocean ruin move to the floor), and the
     * dump emitters must see those mutations, so behaviors write through.
     */
    virtual void postProcess(WorldGenLevel* level,
                             ChunkGenerator* generator,
                             WorldgenRandom& random,
                             const BoundingBox& chunkBB,
                             const ::world::ChunkPos& chunkPos,
                             const core::BlockPos& referencePos,
                             StructurePieceData& self) = 0;
};

} // namespace structure
} // namespace levelgen
} // namespace minecraft
