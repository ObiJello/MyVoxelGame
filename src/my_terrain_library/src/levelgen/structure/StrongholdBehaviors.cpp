#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// StrongholdPieces.java - block placement half (B6). Layout lives in
// StrongholdLayout.cpp; these behaviors receive the per-piece fields the
// layout captured (entryDoor, orientation, RoomCrossing.type, Straight/
// FiveCrossing child flags, Library isTall, FillerCorridor steps).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;
using world::level::block::FenceBlock;
using world::level::block::state::properties::BlockStateProperties;
namespace props = world::level::block::state::properties;
using core::Direction;

// SmallDoorType ordinals (StrongholdPieces.StrongholdPiece.SmallDoorType).
constexpr int DOOR_OPENING = 0;
constexpr int DOOR_WOOD = 1;
constexpr int DOOR_GRATES = 2;
constexpr int DOOR_IRON = 3;

class StrongholdBehaviorBase : public OrientedPieceBehavior {
public:
    StrongholdBehaviorBase(int orientation, int door)
        : OrientedPieceBehavior(orientation), m_entryDoor(door) {}

protected:
    int m_entryDoor;

    static BlockState* stoneBricks() {
        return Blocks::getDefaultState("minecraft:stone_bricks");
    }
    static BlockState* caveAir() {
        return Blocks::getDefaultState("minecraft:cave_air");
    }
    static BlockState* smoothStoneSlab() {
        return Blocks::getDefaultState("minecraft:smooth_stone_slab");
    }
    static BlockState* wallTorch(Direction facing) {
        return Blocks::getDefaultState("minecraft:wall_torch")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, facing);
    }
    static BlockState* ironBars(bool north, bool south, bool west, bool east) {
        BlockState* state = Blocks::getDefaultState("minecraft:iron_bars");
        if (north) state = state->setValue(*FenceBlock::NORTH, true);
        if (south) state = state->setValue(*FenceBlock::SOUTH, true);
        if (west) state = state->setValue(*FenceBlock::WEST, true);
        if (east) state = state->setValue(*FenceBlock::EAST, true);
        return state;
    }
    static BlockState* upperHalf(BlockState* doorState) {
        using DBH = props::DoubleBlockHalf;
        return doorState->setValue(*BlockStateProperties::DOUBLE_BLOCK_HALF,
                                   DBH(DBH::UPPER));
    }

    // Reference: StrongholdPieces.SmoothStoneSelector.next - nextFloat drawn
    // only for edge cells; interior cells become CAVE_AIR without a draw.
    static BlockState* smoothStoneSelector(WorldgenRandom& random,
                                           int /*x*/, int /*y*/, int /*z*/,
                                           bool isEdge) {
        if (!isEdge) return caveAir();
        float selection = random.nextFloat();
        if (selection < 0.2f) {
            return Blocks::getDefaultState("minecraft:cracked_stone_bricks");
        }
        if (selection < 0.5f) {
            return Blocks::getDefaultState("minecraft:mossy_stone_bricks");
        }
        if (selection < 0.55f) {
            return Blocks::getDefaultState("minecraft:infested_stone_bricks");
        }
        return stoneBricks();
    }

    void generateStoneBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                          int x0, int y0, int z0, int x1, int y1, int z1,
                          bool skipAir, WorldgenRandom& random) const {
        generateBox(level, chunkBB, x0, y0, z0, x1, y1, z1, skipAir, random,
                    &StrongholdBehaviorBase::smoothStoneSelector);
    }

    // Reference: StrongholdPiece.generateSmallDoor (no RNG draws).
    void generateSmallDoor(WorldGenLevel* level, const BoundingBox& chunkBB,
                           int doorType, int x, int y, int z) const {
        switch (doorType) {
            case DOOR_OPENING:
                generateBox(level, chunkBB, x, y, z, x + 2, y + 2, z,
                            caveAir(), caveAir(), false);
                break;
            case DOOR_WOOD: {
                placeBlock(level, stoneBricks(), x, y, z, chunkBB);
                placeBlock(level, stoneBricks(), x, y + 1, z, chunkBB);
                placeBlock(level, stoneBricks(), x, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 1, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y + 1, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y, z, chunkBB);
                BlockState* door = Blocks::getDefaultState("minecraft:oak_door");
                placeBlock(level, door, x + 1, y, z, chunkBB);
                placeBlock(level, upperHalf(door), x + 1, y + 1, z, chunkBB);
                break;
            }
            case DOOR_GRATES:
                placeBlock(level, caveAir(), x + 1, y, z, chunkBB);
                placeBlock(level, caveAir(), x + 1, y + 1, z, chunkBB);
                placeBlock(level, ironBars(false, false, true, false), x, y, z, chunkBB);
                placeBlock(level, ironBars(false, false, true, false), x, y + 1, z, chunkBB);
                placeBlock(level, ironBars(false, false, true, true), x, y + 2, z, chunkBB);
                placeBlock(level, ironBars(false, false, true, true), x + 1, y + 2, z, chunkBB);
                placeBlock(level, ironBars(false, false, true, true), x + 2, y + 2, z, chunkBB);
                placeBlock(level, ironBars(false, false, false, true), x + 2, y + 1, z, chunkBB);
                placeBlock(level, ironBars(false, false, false, true), x + 2, y, z, chunkBB);
                break;
            case DOOR_IRON: {
                placeBlock(level, stoneBricks(), x, y, z, chunkBB);
                placeBlock(level, stoneBricks(), x, y + 1, z, chunkBB);
                placeBlock(level, stoneBricks(), x, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 1, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y + 2, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y + 1, z, chunkBB);
                placeBlock(level, stoneBricks(), x + 2, y, z, chunkBB);
                BlockState* door = Blocks::getDefaultState("minecraft:iron_door");
                placeBlock(level, door, x + 1, y, z, chunkBB);
                placeBlock(level, upperHalf(door), x + 1, y + 1, z, chunkBB);
                BlockState* button = Blocks::getDefaultState("minecraft:stone_button");
                placeBlock(level,
                           button->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                                            Direction::NORTH),
                           x + 2, y + 1, z + 1, chunkBB);
                placeBlock(level,
                           button->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                                            Direction::SOUTH),
                           x + 2, y + 1, z - 1, chunkBB);
                break;
            }
        }
    }
};

// Reference: StrongholdPieces.StairsDown.postProcess (StartPiece shares it).
class StrongholdStairsDownBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 10, 4, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 7, 0);
        generateSmallDoor(level, chunkBB, DOOR_OPENING, 1, 1, 4);
        placeBlock(level, stoneBricks(), 2, 6, 1, chunkBB);
        placeBlock(level, stoneBricks(), 1, 5, 1, chunkBB);
        placeBlock(level, smoothStoneSlab(), 1, 6, 1, chunkBB);
        placeBlock(level, stoneBricks(), 1, 5, 2, chunkBB);
        placeBlock(level, stoneBricks(), 1, 4, 3, chunkBB);
        placeBlock(level, smoothStoneSlab(), 1, 5, 3, chunkBB);
        placeBlock(level, stoneBricks(), 2, 4, 3, chunkBB);
        placeBlock(level, stoneBricks(), 3, 3, 3, chunkBB);
        placeBlock(level, smoothStoneSlab(), 3, 4, 3, chunkBB);
        placeBlock(level, stoneBricks(), 3, 3, 2, chunkBB);
        placeBlock(level, stoneBricks(), 3, 2, 1, chunkBB);
        placeBlock(level, smoothStoneSlab(), 3, 3, 1, chunkBB);
        placeBlock(level, stoneBricks(), 2, 2, 1, chunkBB);
        placeBlock(level, stoneBricks(), 1, 1, 1, chunkBB);
        placeBlock(level, smoothStoneSlab(), 1, 2, 1, chunkBB);
        placeBlock(level, stoneBricks(), 1, 1, 2, chunkBB);
        placeBlock(level, smoothStoneSlab(), 1, 1, 3, chunkBB);
    }
};

// Reference: StrongholdPieces.Straight.postProcess.
class StrongholdStraightBehavior final : public StrongholdBehaviorBase {
public:
    StrongholdStraightBehavior(int orientation, int door, bool leftChild, bool rightChild)
        : StrongholdBehaviorBase(orientation, door),
          m_leftChild(leftChild), m_rightChild(rightChild) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 1, 0);
        generateSmallDoor(level, chunkBB, DOOR_OPENING, 1, 1, 6);
        BlockState* eastTorch = wallTorch(Direction::EAST);
        BlockState* westTorch = wallTorch(Direction::WEST);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 1, eastTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 1, westTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 5, eastTorch);
        maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 5, westTorch);
        if (m_leftChild) {
            generateBox(level, chunkBB, 0, 1, 2, 0, 3, 4, caveAir(), caveAir(), false);
        }
        if (m_rightChild) {
            generateBox(level, chunkBB, 4, 1, 2, 4, 3, 4, caveAir(), caveAir(), false);
        }
    }

private:
    bool m_leftChild;
    bool m_rightChild;
};

// Reference: StrongholdPieces.ChestCorridor.postProcess.
class StrongholdChestCorridorBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 1, 0);
        generateSmallDoor(level, chunkBB, DOOR_OPENING, 1, 1, 6);
        generateBox(level, chunkBB, 3, 1, 2, 3, 1, 4, stoneBricks(), stoneBricks(), false);
        BlockState* slab = Blocks::getDefaultState("minecraft:stone_brick_slab");
        placeBlock(level, slab, 3, 1, 1, chunkBB);
        placeBlock(level, slab, 3, 1, 5, chunkBB);
        placeBlock(level, slab, 3, 2, 2, chunkBB);
        placeBlock(level, slab, 3, 2, 4, chunkBB);
        for (int z = 2; z <= 4; ++z) {
            placeBlock(level, slab, 2, 1, z, chunkBB);
        }
        core::BlockPos chestPos = worldPos(3, 2, 3);
        if (!m_hasPlacedChest
            && chunkBB.isInside(chestPos.getX(), chestPos.getY(), chestPos.getZ())) {
            m_hasPlacedChest = true;
            createChest(level, chunkBB, random, 3, 2, 3, "minecraft:chests/stronghold_corridor");
        }
    }

private:
    bool m_hasPlacedChest = false;
};

// Reference: StrongholdPieces.StraightStairsDown.postProcess.
class StrongholdStraightStairsDownBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 10, 7, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 7, 0);
        generateSmallDoor(level, chunkBB, DOOR_OPENING, 1, 1, 7);
        BlockState* stairs = Blocks::getDefaultState("minecraft:cobblestone_stairs")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
        for (int i = 0; i < 6; ++i) {
            placeBlock(level, stairs, 1, 6 - i, 1 + i, chunkBB);
            placeBlock(level, stairs, 2, 6 - i, 1 + i, chunkBB);
            placeBlock(level, stairs, 3, 6 - i, 1 + i, chunkBB);
            if (i < 5) {
                placeBlock(level, stoneBricks(), 1, 5 - i, 1 + i, chunkBB);
                placeBlock(level, stoneBricks(), 2, 5 - i, 1 + i, chunkBB);
                placeBlock(level, stoneBricks(), 3, 5 - i, 1 + i, chunkBB);
            }
        }
    }
};

// Reference: StrongholdPieces.LeftTurn.postProcess.
class StrongholdLeftTurnBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 4, 4, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 1, 0);
        if (m_orientation != static_cast<int>(Direction::NORTH)
            && m_orientation != static_cast<int>(Direction::EAST)) {
            generateBox(level, chunkBB, 4, 1, 1, 4, 3, 3, caveAir(), caveAir(), false);
        } else {
            generateBox(level, chunkBB, 0, 1, 1, 0, 3, 3, caveAir(), caveAir(), false);
        }
    }
};

// Reference: StrongholdPieces.RightTurn.postProcess.
class StrongholdRightTurnBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 4, 4, 4, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 1, 0);
        if (m_orientation != static_cast<int>(Direction::NORTH)
            && m_orientation != static_cast<int>(Direction::EAST)) {
            generateBox(level, chunkBB, 0, 1, 1, 0, 3, 3, caveAir(), caveAir(), false);
        } else {
            generateBox(level, chunkBB, 4, 1, 1, 4, 3, 3, caveAir(), caveAir(), false);
        }
    }
};

// Reference: StrongholdPieces.RoomCrossing.postProcess.
class StrongholdRoomCrossingBehavior final : public StrongholdBehaviorBase {
public:
    StrongholdRoomCrossingBehavior(int orientation, int door, int type)
        : StrongholdBehaviorBase(orientation, door), m_type(type) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 10, 6, 10, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 4, 1, 0);
        generateBox(level, chunkBB, 4, 1, 10, 6, 3, 10, caveAir(), caveAir(), false);
        generateBox(level, chunkBB, 0, 1, 4, 0, 3, 6, caveAir(), caveAir(), false);
        generateBox(level, chunkBB, 10, 1, 4, 10, 3, 6, caveAir(), caveAir(), false);
        switch (m_type) {
            case 0:
                placeBlock(level, stoneBricks(), 5, 1, 5, chunkBB);
                placeBlock(level, stoneBricks(), 5, 2, 5, chunkBB);
                placeBlock(level, stoneBricks(), 5, 3, 5, chunkBB);
                placeBlock(level, wallTorch(Direction::WEST), 4, 3, 5, chunkBB);
                placeBlock(level, wallTorch(Direction::EAST), 6, 3, 5, chunkBB);
                placeBlock(level, wallTorch(Direction::SOUTH), 5, 3, 4, chunkBB);
                placeBlock(level, wallTorch(Direction::NORTH), 5, 3, 6, chunkBB);
                placeBlock(level, smoothStoneSlab(), 4, 1, 4, chunkBB);
                placeBlock(level, smoothStoneSlab(), 4, 1, 5, chunkBB);
                placeBlock(level, smoothStoneSlab(), 4, 1, 6, chunkBB);
                placeBlock(level, smoothStoneSlab(), 6, 1, 4, chunkBB);
                placeBlock(level, smoothStoneSlab(), 6, 1, 5, chunkBB);
                placeBlock(level, smoothStoneSlab(), 6, 1, 6, chunkBB);
                placeBlock(level, smoothStoneSlab(), 5, 1, 4, chunkBB);
                placeBlock(level, smoothStoneSlab(), 5, 1, 6, chunkBB);
                break;
            case 1:
                for (int i = 0; i < 5; ++i) {
                    placeBlock(level, stoneBricks(), 3, 1, 3 + i, chunkBB);
                    placeBlock(level, stoneBricks(), 7, 1, 3 + i, chunkBB);
                    placeBlock(level, stoneBricks(), 3 + i, 1, 3, chunkBB);
                    placeBlock(level, stoneBricks(), 3 + i, 1, 7, chunkBB);
                }
                placeBlock(level, stoneBricks(), 5, 1, 5, chunkBB);
                placeBlock(level, stoneBricks(), 5, 2, 5, chunkBB);
                placeBlock(level, stoneBricks(), 5, 3, 5, chunkBB);
                placeBlock(level, Blocks::getDefaultState("minecraft:water"), 5, 4, 5, chunkBB);
                break;
            case 2: {
                BlockState* cobblestone = Blocks::getDefaultState("minecraft:cobblestone");
                for (int z = 1; z <= 9; ++z) {
                    placeBlock(level, cobblestone, 1, 3, z, chunkBB);
                    placeBlock(level, cobblestone, 9, 3, z, chunkBB);
                }
                for (int x = 1; x <= 9; ++x) {
                    placeBlock(level, cobblestone, x, 3, 1, chunkBB);
                    placeBlock(level, cobblestone, x, 3, 9, chunkBB);
                }
                placeBlock(level, cobblestone, 5, 1, 4, chunkBB);
                placeBlock(level, cobblestone, 5, 1, 6, chunkBB);
                placeBlock(level, cobblestone, 5, 3, 4, chunkBB);
                placeBlock(level, cobblestone, 5, 3, 6, chunkBB);
                placeBlock(level, cobblestone, 4, 1, 5, chunkBB);
                placeBlock(level, cobblestone, 6, 1, 5, chunkBB);
                placeBlock(level, cobblestone, 4, 3, 5, chunkBB);
                placeBlock(level, cobblestone, 6, 3, 5, chunkBB);
                for (int y = 1; y <= 3; ++y) {
                    placeBlock(level, cobblestone, 4, y, 4, chunkBB);
                    placeBlock(level, cobblestone, 6, y, 4, chunkBB);
                    placeBlock(level, cobblestone, 4, y, 6, chunkBB);
                    placeBlock(level, cobblestone, 6, y, 6, chunkBB);
                }
                placeBlock(level, Blocks::getDefaultState("minecraft:wall_torch"),
                           5, 3, 5, chunkBB);
                BlockState* planks = Blocks::getDefaultState("minecraft:oak_planks");
                for (int z = 2; z <= 8; ++z) {
                    placeBlock(level, planks, 2, 3, z, chunkBB);
                    placeBlock(level, planks, 3, 3, z, chunkBB);
                    if (z <= 3 || z >= 7) {
                        placeBlock(level, planks, 4, 3, z, chunkBB);
                        placeBlock(level, planks, 5, 3, z, chunkBB);
                        placeBlock(level, planks, 6, 3, z, chunkBB);
                    }
                    placeBlock(level, planks, 7, 3, z, chunkBB);
                    placeBlock(level, planks, 8, 3, z, chunkBB);
                }
                BlockState* ladder = Blocks::getDefaultState("minecraft:ladder")
                    ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);
                placeBlock(level, ladder, 9, 1, 3, chunkBB);
                placeBlock(level, ladder, 9, 2, 3, chunkBB);
                placeBlock(level, ladder, 9, 3, 3, chunkBB);
                createChest(level, chunkBB, random, 3, 4, 8, "minecraft:chests/stronghold_crossing");
                break;
            }
            default:
                // Types 3 and 4 place nothing extra.
                break;
        }
    }

private:
    int m_type;
};

// Reference: StrongholdPieces.PrisonHall.postProcess.
class StrongholdPrisonHallBehavior final : public StrongholdBehaviorBase {
public:
    using StrongholdBehaviorBase::StrongholdBehaviorBase;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 8, 4, 10, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 1, 1, 0);
        generateBox(level, chunkBB, 1, 1, 10, 3, 3, 10, caveAir(), caveAir(), false);
        generateStoneBox(level, chunkBB, 4, 1, 1, 4, 3, 1, false, random);
        generateStoneBox(level, chunkBB, 4, 1, 3, 4, 3, 3, false, random);
        generateStoneBox(level, chunkBB, 4, 1, 7, 4, 3, 7, false, random);
        generateStoneBox(level, chunkBB, 4, 1, 9, 4, 3, 9, false, random);
        for (int y = 1; y <= 3; ++y) {
            placeBlock(level, ironBars(true, true, false, false), 4, y, 4, chunkBB);
            placeBlock(level, ironBars(true, true, false, true), 4, y, 5, chunkBB);
            placeBlock(level, ironBars(true, true, false, false), 4, y, 6, chunkBB);
            placeBlock(level, ironBars(false, false, true, true), 5, y, 5, chunkBB);
            placeBlock(level, ironBars(false, false, true, true), 6, y, 5, chunkBB);
            placeBlock(level, ironBars(false, false, true, true), 7, y, 5, chunkBB);
        }
        placeBlock(level, ironBars(true, true, false, false), 4, 3, 2, chunkBB);
        placeBlock(level, ironBars(true, true, false, false), 4, 3, 8, chunkBB);
        BlockState* doorBottom = Blocks::getDefaultState("minecraft:iron_door")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);
        BlockState* doorTop = upperHalf(doorBottom);
        placeBlock(level, doorBottom, 4, 1, 2, chunkBB);
        placeBlock(level, doorTop, 4, 2, 2, chunkBB);
        placeBlock(level, doorBottom, 4, 1, 8, chunkBB);
        placeBlock(level, doorTop, 4, 2, 8, chunkBB);
    }
};

// Reference: StrongholdPieces.Library.postProcess.
class StrongholdLibraryBehavior final : public StrongholdBehaviorBase {
public:
    StrongholdLibraryBehavior(int orientation, int door, bool isTall)
        : StrongholdBehaviorBase(orientation, door), m_isTall(isTall) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        int currentHeight = m_isTall ? 11 : 6;
        generateStoneBox(level, chunkBB, 0, 0, 0, 13, currentHeight - 1, 14, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 4, 1, 0);
        BlockState* cobweb = Blocks::getDefaultState("minecraft:cobweb");
        generateMaybeBox(level, chunkBB, random, 0.07f, 2, 1, 1, 11, 4, 13,
                         cobweb, cobweb, false, false);
        BlockState* planks = Blocks::getDefaultState("minecraft:oak_planks");
        BlockState* bookshelf = Blocks::getDefaultState("minecraft:bookshelf");
        for (int d = 1; d <= 13; ++d) {
            if ((d - 1) % 4 == 0) {
                generateBox(level, chunkBB, 1, 1, d, 1, 4, d, planks, planks, false);
                generateBox(level, chunkBB, 12, 1, d, 12, 4, d, planks, planks, false);
                placeBlock(level, wallTorch(Direction::EAST), 2, 3, d, chunkBB);
                placeBlock(level, wallTorch(Direction::WEST), 11, 3, d, chunkBB);
                if (m_isTall) {
                    generateBox(level, chunkBB, 1, 6, d, 1, 9, d, planks, planks, false);
                    generateBox(level, chunkBB, 12, 6, d, 12, 9, d, planks, planks, false);
                }
            } else {
                generateBox(level, chunkBB, 1, 1, d, 1, 4, d, bookshelf, bookshelf, false);
                generateBox(level, chunkBB, 12, 1, d, 12, 4, d, bookshelf, bookshelf, false);
                if (m_isTall) {
                    generateBox(level, chunkBB, 1, 6, d, 1, 9, d, bookshelf, bookshelf, false);
                    generateBox(level, chunkBB, 12, 6, d, 12, 9, d, bookshelf, bookshelf, false);
                }
            }
        }
        for (int d = 3; d < 12; d += 2) {
            generateBox(level, chunkBB, 3, 1, d, 4, 3, d, bookshelf, bookshelf, false);
            generateBox(level, chunkBB, 6, 1, d, 7, 3, d, bookshelf, bookshelf, false);
            generateBox(level, chunkBB, 9, 1, d, 10, 3, d, bookshelf, bookshelf, false);
        }
        if (m_isTall) {
            generateBox(level, chunkBB, 1, 5, 1, 3, 5, 13, planks, planks, false);
            generateBox(level, chunkBB, 10, 5, 1, 12, 5, 13, planks, planks, false);
            generateBox(level, chunkBB, 4, 5, 1, 9, 5, 2, planks, planks, false);
            generateBox(level, chunkBB, 4, 5, 12, 9, 5, 13, planks, planks, false);
            placeBlock(level, planks, 9, 5, 11, chunkBB);
            placeBlock(level, planks, 8, 5, 11, chunkBB);
            placeBlock(level, planks, 9, 5, 10, chunkBB);
            BlockState* weFence = oakFence(false, false, true, true);
            BlockState* nsFence = oakFence(true, true, false, false);
            generateBox(level, chunkBB, 3, 6, 3, 3, 6, 11, nsFence, nsFence, false);
            generateBox(level, chunkBB, 10, 6, 3, 10, 6, 9, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 6, 2, 9, 6, 2, weFence, weFence, false);
            generateBox(level, chunkBB, 4, 6, 12, 7, 6, 12, weFence, weFence, false);
            placeBlock(level, oakFence(true, false, false, true), 3, 6, 2, chunkBB);
            placeBlock(level, oakFence(false, true, false, true), 3, 6, 12, chunkBB);
            placeBlock(level, oakFence(true, false, true, false), 10, 6, 2, chunkBB);
            for (int i = 0; i <= 2; ++i) {
                placeBlock(level, oakFence(false, true, true, false), 8 + i, 6, 12 - i, chunkBB);
                if (i != 2) {
                    placeBlock(level, oakFence(true, false, false, true), 8 + i, 6, 11 - i, chunkBB);
                }
            }
            BlockState* ladder = Blocks::getDefaultState("minecraft:ladder")
                ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
            placeBlock(level, ladder, 10, 1, 13, chunkBB);
            placeBlock(level, ladder, 10, 2, 13, chunkBB);
            placeBlock(level, ladder, 10, 3, 13, chunkBB);
            placeBlock(level, ladder, 10, 4, 13, chunkBB);
            placeBlock(level, ladder, 10, 5, 13, chunkBB);
            placeBlock(level, ladder, 10, 6, 13, chunkBB);
            placeBlock(level, ladder, 10, 7, 13, chunkBB);
            BlockState* eFence = oakFence(false, false, false, true);
            placeBlock(level, eFence, 6, 9, 7, chunkBB);
            BlockState* wFence = oakFence(false, false, true, false);
            placeBlock(level, wFence, 7, 9, 7, chunkBB);
            placeBlock(level, eFence, 6, 8, 7, chunkBB);
            placeBlock(level, wFence, 7, 8, 7, chunkBB);
            BlockState* nsweFence = oakFence(true, true, true, true);
            placeBlock(level, nsweFence, 6, 7, 7, chunkBB);
            placeBlock(level, nsweFence, 7, 7, 7, chunkBB);
            placeBlock(level, eFence, 5, 7, 7, chunkBB);
            placeBlock(level, wFence, 8, 7, 7, chunkBB);
            placeBlock(level, oakFence(true, false, false, true), 6, 7, 6, chunkBB);
            placeBlock(level, oakFence(false, true, false, true), 6, 7, 8, chunkBB);
            placeBlock(level, oakFence(true, false, true, false), 7, 7, 6, chunkBB);
            placeBlock(level, oakFence(false, true, true, false), 7, 7, 8, chunkBB);
            BlockState* torch = Blocks::getDefaultState("minecraft:torch");
            placeBlock(level, torch, 5, 8, 7, chunkBB);
            placeBlock(level, torch, 8, 8, 7, chunkBB);
            placeBlock(level, torch, 6, 8, 6, chunkBB);
            placeBlock(level, torch, 6, 8, 8, chunkBB);
            placeBlock(level, torch, 7, 8, 6, chunkBB);
            placeBlock(level, torch, 7, 8, 8, chunkBB);
        }
        createChest(level, chunkBB, random, 3, 3, 5, "minecraft:chests/stronghold_library");
        if (m_isTall) {
            placeBlock(level, caveAir(), 12, 9, 1, chunkBB);
            createChest(level, chunkBB, random, 12, 8, 1, "minecraft:chests/stronghold_library");
        }
    }

private:
    bool m_isTall;

    static BlockState* oakFence(bool north, bool south, bool west, bool east) {
        BlockState* state = Blocks::getDefaultState("minecraft:oak_fence");
        if (north) state = state->setValue(*FenceBlock::NORTH, true);
        if (south) state = state->setValue(*FenceBlock::SOUTH, true);
        if (west) state = state->setValue(*FenceBlock::WEST, true);
        if (east) state = state->setValue(*FenceBlock::EAST, true);
        return state;
    }
};

// Reference: StrongholdPieces.FiveCrossing.postProcess.
class StrongholdFiveCrossingBehavior final : public StrongholdBehaviorBase {
public:
    StrongholdFiveCrossingBehavior(int orientation, int door, bool leftLow,
                                   bool leftHigh, bool rightLow, bool rightHigh)
        : StrongholdBehaviorBase(orientation, door), m_leftLow(leftLow),
          m_leftHigh(leftHigh), m_rightLow(rightLow), m_rightHigh(rightHigh) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 9, 8, 10, true, random);
        generateSmallDoor(level, chunkBB, m_entryDoor, 4, 3, 0);
        if (m_leftLow) {
            generateBox(level, chunkBB, 0, 3, 1, 0, 5, 3, caveAir(), caveAir(), false);
        }
        if (m_rightLow) {
            generateBox(level, chunkBB, 9, 3, 1, 9, 5, 3, caveAir(), caveAir(), false);
        }
        if (m_leftHigh) {
            generateBox(level, chunkBB, 0, 5, 7, 0, 7, 9, caveAir(), caveAir(), false);
        }
        if (m_rightHigh) {
            generateBox(level, chunkBB, 9, 5, 7, 9, 7, 9, caveAir(), caveAir(), false);
        }
        generateBox(level, chunkBB, 5, 1, 10, 7, 3, 10, caveAir(), caveAir(), false);
        generateStoneBox(level, chunkBB, 1, 2, 1, 8, 2, 6, false, random);
        generateStoneBox(level, chunkBB, 4, 1, 5, 4, 4, 9, false, random);
        generateStoneBox(level, chunkBB, 8, 1, 5, 8, 4, 9, false, random);
        generateStoneBox(level, chunkBB, 1, 4, 7, 3, 4, 9, false, random);
        generateStoneBox(level, chunkBB, 1, 3, 5, 3, 3, 6, false, random);
        BlockState* slab = smoothStoneSlab();
        generateBox(level, chunkBB, 1, 3, 4, 3, 3, 4, slab, slab, false);
        generateBox(level, chunkBB, 1, 4, 6, 3, 4, 6, slab, slab, false);
        generateStoneBox(level, chunkBB, 5, 1, 7, 7, 1, 8, false, random);
        generateBox(level, chunkBB, 5, 1, 9, 7, 1, 9, slab, slab, false);
        generateBox(level, chunkBB, 5, 2, 7, 7, 2, 7, slab, slab, false);
        generateBox(level, chunkBB, 4, 5, 7, 4, 5, 9, slab, slab, false);
        generateBox(level, chunkBB, 8, 5, 7, 8, 5, 9, slab, slab, false);
        using ST = props::SlabType;
        BlockState* doubleSlab = slab->setValue(*BlockStateProperties::SLAB_TYPE,
                                                ST(ST::DOUBLE));
        generateBox(level, chunkBB, 5, 5, 7, 7, 5, 9, doubleSlab, doubleSlab, false);
        placeBlock(level, wallTorch(Direction::SOUTH), 6, 5, 6, chunkBB);
    }

private:
    bool m_leftLow;
    bool m_leftHigh;
    bool m_rightLow;
    bool m_rightHigh;
};

// Reference: StrongholdPieces.PortalRoom.postProcess.
class StrongholdPortalRoomBehavior final : public StrongholdBehaviorBase {
public:
    explicit StrongholdPortalRoomBehavior(int orientation)
        : StrongholdBehaviorBase(orientation, DOOR_OPENING) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)referencePos;
        bind(self);
        generateStoneBox(level, chunkBB, 0, 0, 0, 10, 7, 15, false, random);
        generateSmallDoor(level, chunkBB, DOOR_GRATES, 4, 1, 0);
        generateStoneBox(level, chunkBB, 1, 6, 1, 1, 6, 14, false, random);
        generateStoneBox(level, chunkBB, 9, 6, 1, 9, 6, 14, false, random);
        generateStoneBox(level, chunkBB, 2, 6, 1, 8, 6, 2, false, random);
        generateStoneBox(level, chunkBB, 2, 6, 14, 8, 6, 14, false, random);
        generateStoneBox(level, chunkBB, 1, 1, 1, 2, 1, 4, false, random);
        generateStoneBox(level, chunkBB, 8, 1, 1, 9, 1, 4, false, random);
        BlockState* lava = Blocks::getDefaultState("minecraft:lava");
        generateBox(level, chunkBB, 1, 1, 1, 1, 1, 3, lava, lava, false);
        generateBox(level, chunkBB, 9, 1, 1, 9, 1, 3, lava, lava, false);
        generateStoneBox(level, chunkBB, 3, 1, 8, 7, 1, 12, false, random);
        generateBox(level, chunkBB, 4, 1, 9, 6, 1, 11, lava, lava, false);
        BlockState* nsBars = ironBars(true, true, false, false);
        BlockState* weBars = ironBars(false, false, true, true);
        for (int z = 3; z < 14; z += 2) {
            generateBox(level, chunkBB, 0, 3, z, 0, 4, z, nsBars, nsBars, false);
            generateBox(level, chunkBB, 10, 3, z, 10, 4, z, nsBars, nsBars, false);
        }
        for (int x = 2; x < 9; x += 2) {
            generateBox(level, chunkBB, x, 3, 15, x, 4, 15, weBars, weBars, false);
        }
        BlockState* stairs = Blocks::getDefaultState("minecraft:stone_brick_stairs")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH);
        generateStoneBox(level, chunkBB, 4, 1, 5, 6, 1, 7, false, random);
        generateStoneBox(level, chunkBB, 4, 2, 6, 6, 2, 7, false, random);
        generateStoneBox(level, chunkBB, 4, 3, 7, 6, 3, 7, false, random);
        for (int x = 4; x <= 6; ++x) {
            placeBlock(level, stairs, x, 1, 4, chunkBB);
            placeBlock(level, stairs, x, 2, 5, chunkBB);
            placeBlock(level, stairs, x, 3, 6, chunkBB);
        }
        BlockState* frameBase = Blocks::getDefaultState("minecraft:end_portal_frame");
        BlockState* northFrame = frameBase->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH);
        BlockState* southFrame = frameBase->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
        BlockState* eastFrame = frameBase->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::EAST);
        BlockState* westFrame = frameBase->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);
        bool allEyes = true;
        bool eyes[12];
        for (int i = 0; i < 12; ++i) {
            eyes[i] = random.nextFloat() > 0.9f;
            allEyes &= eyes[i];
        }
        auto withEye = [](BlockState* frame, bool eye) {
            return frame->setValue(*BlockStateProperties::EYE, eye);
        };
        placeBlock(level, withEye(northFrame, eyes[0]), 4, 3, 8, chunkBB);
        placeBlock(level, withEye(northFrame, eyes[1]), 5, 3, 8, chunkBB);
        placeBlock(level, withEye(northFrame, eyes[2]), 6, 3, 8, chunkBB);
        placeBlock(level, withEye(southFrame, eyes[3]), 4, 3, 12, chunkBB);
        placeBlock(level, withEye(southFrame, eyes[4]), 5, 3, 12, chunkBB);
        placeBlock(level, withEye(southFrame, eyes[5]), 6, 3, 12, chunkBB);
        placeBlock(level, withEye(eastFrame, eyes[6]), 3, 3, 9, chunkBB);
        placeBlock(level, withEye(eastFrame, eyes[7]), 3, 3, 10, chunkBB);
        placeBlock(level, withEye(eastFrame, eyes[8]), 3, 3, 11, chunkBB);
        placeBlock(level, withEye(westFrame, eyes[9]), 7, 3, 9, chunkBB);
        placeBlock(level, withEye(westFrame, eyes[10]), 7, 3, 10, chunkBB);
        placeBlock(level, withEye(westFrame, eyes[11]), 7, 3, 11, chunkBB);
        if (allEyes) {
            BlockState* portal = Blocks::getDefaultState("minecraft:end_portal");
            placeBlock(level, portal, 4, 3, 9, chunkBB);
            placeBlock(level, portal, 5, 3, 9, chunkBB);
            placeBlock(level, portal, 6, 3, 9, chunkBB);
            placeBlock(level, portal, 4, 3, 10, chunkBB);
            placeBlock(level, portal, 5, 3, 10, chunkBB);
            placeBlock(level, portal, 6, 3, 10, chunkBB);
            placeBlock(level, portal, 4, 3, 11, chunkBB);
            placeBlock(level, portal, 5, 3, 11, chunkBB);
            placeBlock(level, portal, 6, 3, 11, chunkBB);
        }
        if (!m_hasPlacedSpawner) {
            core::BlockPos pos = worldPos(5, 3, 6);
            if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
                m_hasPlacedSpawner = true;
                level->setBlock(pos, Blocks::getDefaultState("minecraft:spawner"), 2);
                // Java: SpawnerBlockEntity.setEntityId(SILVERFISH, random)
                // draws nothing (empty spawnPotentials); the saved BE carries
                // the BaseSpawner defaults + silverfish (B8).
                if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                    chunk->setBlockEntityNbt(pos,
                        "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,"
                        "MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,"
                        "SpawnData:{entity:{id:\"minecraft:silverfish\"}},"
                        "SpawnPotentials:[],SpawnRange:4s,components:{},"
                        "id:\"minecraft:mob_spawner\"}");
                }
            }
        }
    }

private:
    bool m_hasPlacedSpawner = false;
};

// Reference: StrongholdPieces.FillerCorridor.postProcess.
class StrongholdFillerCorridorBehavior final : public StrongholdBehaviorBase {
public:
    StrongholdFillerCorridorBehavior(int orientation, int steps)
        : StrongholdBehaviorBase(orientation, DOOR_OPENING), m_steps(steps) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        for (int i = 0; i < m_steps; ++i) {
            placeBlock(level, stoneBricks(), 0, 0, i, chunkBB);
            placeBlock(level, stoneBricks(), 1, 0, i, chunkBB);
            placeBlock(level, stoneBricks(), 2, 0, i, chunkBB);
            placeBlock(level, stoneBricks(), 3, 0, i, chunkBB);
            placeBlock(level, stoneBricks(), 4, 0, i, chunkBB);
            for (int y = 1; y <= 3; ++y) {
                placeBlock(level, stoneBricks(), 0, y, i, chunkBB);
                placeBlock(level, caveAir(), 1, y, i, chunkBB);
                placeBlock(level, caveAir(), 2, y, i, chunkBB);
                placeBlock(level, caveAir(), 3, y, i, chunkBB);
                placeBlock(level, stoneBricks(), 4, y, i, chunkBB);
            }
            placeBlock(level, stoneBricks(), 0, 4, i, chunkBB);
            placeBlock(level, stoneBricks(), 1, 4, i, chunkBB);
            placeBlock(level, stoneBricks(), 2, 4, i, chunkBB);
            placeBlock(level, stoneBricks(), 3, 4, i, chunkBB);
            placeBlock(level, stoneBricks(), 4, 4, i, chunkBB);
        }
    }

private:
    int m_steps;
};

} // namespace

std::shared_ptr<StructurePieceBehavior> strongholdStairsDown(int orientation, int door) {
    return std::make_shared<StrongholdStairsDownBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdStraight(
    int orientation, int door, bool leftChild, bool rightChild) {
    return std::make_shared<StrongholdStraightBehavior>(orientation, door,
                                                        leftChild, rightChild);
}

std::shared_ptr<StructurePieceBehavior> strongholdChestCorridor(int orientation, int door) {
    return std::make_shared<StrongholdChestCorridorBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdStraightStairsDown(int orientation, int door) {
    return std::make_shared<StrongholdStraightStairsDownBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdLeftTurn(int orientation, int door) {
    return std::make_shared<StrongholdLeftTurnBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdRightTurn(int orientation, int door) {
    return std::make_shared<StrongholdRightTurnBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdRoomCrossing(
    int orientation, int door, int type) {
    return std::make_shared<StrongholdRoomCrossingBehavior>(orientation, door, type);
}

std::shared_ptr<StructurePieceBehavior> strongholdPrisonHall(int orientation, int door) {
    return std::make_shared<StrongholdPrisonHallBehavior>(orientation, door);
}

std::shared_ptr<StructurePieceBehavior> strongholdLibrary(
    int orientation, int door, bool isTall) {
    return std::make_shared<StrongholdLibraryBehavior>(orientation, door, isTall);
}

std::shared_ptr<StructurePieceBehavior> strongholdFiveCrossing(
    int orientation, int door, bool leftLow, bool leftHigh, bool rightLow, bool rightHigh) {
    return std::make_shared<StrongholdFiveCrossingBehavior>(
        orientation, door, leftLow, leftHigh, rightLow, rightHigh);
}

std::shared_ptr<StructurePieceBehavior> strongholdPortalRoom(int orientation) {
    return std::make_shared<StrongholdPortalRoomBehavior>(orientation);
}

std::shared_ptr<StructurePieceBehavior> strongholdFillerCorridor(int orientation, int steps) {
    return std::make_shared<StrongholdFillerCorridorBehavior>(orientation, steps);
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
