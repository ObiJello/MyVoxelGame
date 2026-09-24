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
namespace nbt {
class CompoundTag;
}
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

    /**
     * Reference: StructurePiece.addAdditionalSaveData - the state a piece
     * changes while it places (a height it settled on, a chest it already
     * put down), written into the piece's saved tag under MC's field names.
     * A reloaded start is regenerated from the seed and then handed this
     * state back through loadState, so a structure half placed before an
     * unload finishes exactly as it would have. Stateless pieces keep the
     * default no-ops.
     */
    virtual void saveState(nbt::CompoundTag& tag) const { (void)tag; }
    virtual void loadState(const nbt::CompoundTag& tag, StructurePieceData& self) {
        (void)tag;
        (void)self;
    }
};

} // namespace structure
} // namespace levelgen
} // namespace minecraft
