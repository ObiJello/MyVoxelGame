#pragma once

#include "levelgen/structure/StructurePieceBehavior.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <functional>

// Reference: net/minecraft/world/level/levelgen/structure/StructurePiece.java
// protected placement helpers (getWorldPos/placeBlock/generateBox/...) plus
// ScatteredFeaturePiece height adjustment. Base class for all hardcoded-piece
// block-placement behaviors (B6).

namespace minecraft {

namespace world { namespace level { namespace block { namespace state {
class BlockState;
}}}}
using BlockState = world::level::block::state::BlockState;

namespace levelgen {
class WorldGenLevel;
namespace structure {

class OrientedPieceBehavior : public StructurePieceBehavior {
public:
    // Java Mirror / Rotation ordinals.
    enum Mirror { MIRROR_NONE = 0, MIRROR_LEFT_RIGHT = 1, MIRROR_FRONT_BACK = 2 };
    enum Rot { ROT_NONE = 0, ROT_CW90 = 1, ROT_CW180 = 2, ROT_CCW90 = 3 };

    /** orientation: -1 = null, else core::Direction (N/S/W/E). Applies the
     *  setOrientation() mirror/rotation mapping. */
    explicit OrientedPieceBehavior(int orientation);

protected:
    int m_orientation;  // -1 or core::Direction value
    int m_mirror = MIRROR_NONE;
    int m_rotation = ROT_NONE;
    // Reference: ScatteredFeaturePiece.heightPosition (-1 until computed).
    int m_heightPosition = -1;

    // Per-call current piece record (set by subclasses at postProcess entry
    // via bind(); FEATURES is never parallelized, so instance state is safe).
    StructurePieceData* m_self = nullptr;
    void bind(StructurePieceData& self) { m_self = &self; }

    // Reference: StructurePiece.getWorldX/getWorldY/getWorldZ.
    int worldX(int x, int z) const;
    int worldY(int y) const;
    int worldZ(int x, int z) const;
    core::BlockPos worldPos(int x, int y, int z) const {
        return core::BlockPos(worldX(x, z), worldY(y), worldZ(x, z));
    }

    /** Apply this piece's mirror then rotation to a state (Java placeBlock). */
    BlockState* mirrorRotate(BlockState* state) const;

    // Reference: StructurePiece.placeBlock (fluid tick + postprocessing mark
    // are dump-invisible at generation phases and skipped).
    void placeBlock(WorldGenLevel* level, BlockState* state, int x, int y, int z,
                    const BoundingBox& chunkBB) const;

    // Reference: StructurePiece.getBlock - AIR when outside chunkBB.
    BlockState* getBlock(WorldGenLevel* level, int x, int y, int z,
                         const BoundingBox& chunkBB) const;

    // Reference: StructurePiece.canBeReplaced - default true; families
    // override (mineshaft protects its own planks/wood/fence/chains).
    virtual bool canBeReplaced(WorldGenLevel* level, int x, int y, int z,
                               const BoundingBox& chunkBB) const {
        (void)level; (void)x; (void)y; (void)z; (void)chunkBB;
        return true;
    }

    // Reference: StructurePiece.isInterior - world pos above must be inside
    // chunkBB and below the OCEAN_FLOOR_WG height.
    bool isInterior(WorldGenLevel* level, int x, int y, int z,
                    const BoundingBox& chunkBB) const;

    // Reference: StructurePiece.maybeGenerateBlock - nextFloat < probability.
    void maybeGenerateBlock(WorldGenLevel* level, const BoundingBox& chunkBB,
                            WorldgenRandom& random, float probability,
                            int x, int y, int z, BlockState* state) const;

    // Reference: StructurePiece.generateMaybeBox - nextFloat drawn FIRST for
    // every cell (before the skipAir/isInterior tests); accept when <= prob.
    void generateMaybeBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                          WorldgenRandom& random, float probability,
                          int x0, int y0, int z0, int x1, int y1, int z1,
                          BlockState* edgeBlock, BlockState* fillBlock,
                          bool skipAir, bool hasToBeInside) const;

    // Reference: StructurePiece.generateUpperHalfSphere (float math).
    void generateUpperHalfSphere(WorldGenLevel* level, const BoundingBox& chunkBB,
                                 int x0, int y0, int z0, int x1, int y1, int z1,
                                 BlockState* fillBlock, bool skipAir) const;

    // Reference: StructurePiece.generateBox (edge/fill variant).
    void generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                     int x0, int y0, int z0, int x1, int y1, int z1,
                     BlockState* edgeBlock, BlockState* fillBlock, bool skipAir) const;

    // Reference: StructurePiece.generateBox (BlockSelector variant) - the
    // selector draws from `random` for every cell that passes the skipAir
    // test (piece-local coords, isEdge flag).
    void generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                     int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir,
                     WorldgenRandom& random,
                     const std::function<BlockState*(WorldgenRandom&, int, int, int, bool)>& selector) const;

    // Reference: StructurePiece.generateAirBox.
    void generateAirBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                        int x0, int y0, int z0, int x1, int y1, int z1) const;

    // Reference: StructurePiece.fillColumnDown.
    void fillColumnDown(WorldGenLevel* level, BlockState* state, int x, int startY,
                        int z, const BoundingBox& chunkBB) const;

    // Reference: StructurePiece.isReplaceableByStructures.
    static bool isReplaceableByStructures(const BlockState* state);

    // Reference: StructurePiece.createChest(pos overload) - places the chest
    // block (reorient when state null), draws the loot-seed nextLong, and
    // stores the {LootTable, LootTableSeed} BE payload when a table is given.
    bool createChest(WorldGenLevel* level, const BoundingBox& chunkBB,
                     WorldgenRandom& random, const core::BlockPos& pos,
                     BlockState* state = nullptr,
                     const char* lootTable = nullptr) const;

    // Reference: ScatteredFeaturePiece.updateAverageGroundHeight - memoized
    // via m_heightPosition; moves m_self->boundingBox vertically.
    bool updateAverageGroundHeight(WorldGenLevel* level, const BoundingBox& chunkBB,
                                   int offset);

    // Reference: ScatteredFeaturePiece.updateHeightPositionToLowestGroundHeight
    // - min over ALL footprint columns (no chunkBB filter), memoized.
    bool updateHeightPositionToLowestGroundHeight(WorldGenLevel* level, int offset);

    // Reference: StructurePiece.createChest(x, y, z overload) - transforms to
    // world coordinates first.
    bool createChest(WorldGenLevel* level, const BoundingBox& chunkBB,
                     WorldgenRandom& random, int x, int y, int z,
                     const char* lootTable = nullptr) const {
        return createChest(level, chunkBB, random, worldPos(x, y, z), nullptr,
                           lootTable);
    }

    // Reference: StructurePiece.createDispenser - facing set directly (no
    // reorient), placeBlock applies the piece transform, then the loot-seed
    // nextLong draw + the B8 BE payload.
    bool createDispenser(WorldGenLevel* level, const BoundingBox& chunkBB,
                         WorldgenRandom& random, int x, int y, int z,
                         core::Direction facing,
                         const char* lootTable = nullptr) const;
};

namespace state_transforms {
/** Reference: BlockState.rotate/mirror per-block virtuals. Handles stairs
 *  (FACING + SHAPE) and generic HORIZONTAL_FACING; other states unchanged.
 *  Extend per family as placement batches land. */
BlockState* rotateState(BlockState* state, int rotation);
BlockState* mirrorState(BlockState* state, int mirror);
/** Reference: StructurePiece.reorient (chest facing from neighbor solidity). */
BlockState* reorientChest(WorldGenLevel* level, const core::BlockPos& pos,
                          BlockState* state);
} // namespace state_transforms

} // namespace structure
} // namespace levelgen
} // namespace minecraft
