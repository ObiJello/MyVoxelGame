#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "random/LegacyRandomSource.h"
#include <algorithm>
#include <set>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/StructurePiece.java
// (createChest/reorient) and structures/BuriedTreasurePieces.java,
// SwampHutPiece.java, DesertPyramidPiece.java, DesertPyramidStructure.java.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;

// Reference: Direction.values() order DOWN, UP, NORTH, SOUTH, WEST, EAST.
constexpr Direction kAllDirections[6] = {
    Direction::DOWN, Direction::UP, Direction::NORTH,
    Direction::SOUTH, Direction::WEST, Direction::EAST};

// Reference: Direction.Plane.HORIZONTAL iteration order NORTH, EAST, SOUTH, WEST.
constexpr Direction kHorizontal[4] = {
    Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST};

int stepY(Direction dir) {
    return dir == Direction::DOWN ? -1 : (dir == Direction::UP ? 1 : 0);
}

core::BlockPos relative(const core::BlockPos& pos, Direction dir) {
    return pos.offset(core::getStepX(dir), stepY(dir), core::getStepZ(dir));
}

// Reference: Direction.getClockWise() (Y axis).
Direction clockWise(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::EAST;
        case Direction::EAST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::WEST;
        case Direction::WEST: return Direction::NORTH;
        default: return dir;
    }
}

// Reference: BuriedTreasurePieces.isLiquid - is(WATER) || is(LAVA).
bool isLiquidState(const BlockState* state) {
    return state->is(Blocks::WATER) || state->is(Blocks::LAVA);
}

// Reference: StructurePiece.reorient().
BlockState* reorient(WorldGenLevel* level, const core::BlockPos& pos, BlockState* state) {
    bool haveSolid = false;
    bool broke = false;
    Direction solidNeighbor = Direction::NORTH;
    for (Direction direction : kHorizontal) {
        core::BlockPos relativePos = relative(pos, direction);
        BlockState* relState = level->getBlockState(relativePos);
        if (relState->is(Blocks::CHEST)) {
            return state;
        }
        if (relState->isSolidRender()) {
            if (haveSolid) {
                haveSolid = false;
                broke = true;
                break;
            }
            haveSolid = true;
            solidNeighbor = direction;
        }
    }
    (void)broke;
    if (haveSolid) {
        return state->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                               core::getOpposite(solidNeighbor));
    }
    // Reference: reorient() else-branch - walk the current facing.
    Direction lockDir = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
    core::BlockPos relativePos = relative(pos, lockDir);
    if (level->getBlockState(relativePos)->isSolidRender()) {
        lockDir = core::getOpposite(lockDir);
        relativePos = relative(pos, lockDir);
    }
    if (level->getBlockState(relativePos)->isSolidRender()) {
        lockDir = clockWise(lockDir);
        relativePos = relative(pos, lockDir);
    }
    if (level->getBlockState(relativePos)->isSolidRender()) {
        lockDir = core::getOpposite(lockDir);
        // Java: blockPos.relative(lockDir) result discarded (vanilla quirk).
    }
    return state->setValue(*BlockStateProperties::HORIZONTAL_FACING, lockDir);
}

// Reference: StructurePiece.createChest(level, chunkBB, random, pos, lootTable,
// null). Block placement + the loot-seed nextLong draw; the block entity
// itself (LootTable + LootTableSeed NBT) is attached in B8.
bool createChest(WorldGenLevel* level, const BoundingBox& chunkBB,
                 WorldgenRandom& random, const core::BlockPos& pos,
                 const char* lootTable) {
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
        || level->getBlockState(pos)->is(Blocks::CHEST)) {
        return false;
    }
    BlockState* state = reorient(level, pos, Blocks::CHEST->defaultBlockState());
    level->setBlock(pos, state, 2);
    // Java: ChestBlockEntity.setLootTable(lootTable, random.nextLong()); the
    // saved BE is {LootTable, LootTableSeed, components, id} (B8).
    int64_t seed = random.nextLong();
    if (lootTable != nullptr) {
        if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
            chunk->setBlockEntityNbt(pos,
                std::string("{LootTable:\"") + lootTable + "\",LootTableSeed:"
                + std::to_string(seed) + "l,components:{},id:\"minecraft:chest\"}");
        }
    }
    return true;
}

// Reference: BuriedTreasurePieces.BuriedTreasurePiece.postProcess.
class BuriedTreasureBehavior final : public StructurePieceBehavior {
public:
    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        (void)referencePos;
        int y = level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG,
                                 self.boundingBox.minX, self.boundingBox.minZ);
        core::BlockPos pos(self.boundingBox.minX, y, self.boundingBox.minZ);
        while (pos.getY() > level->getMinY()) {
            BlockState* currentState = level->getBlockState(pos);
            BlockState* belowState = level->getBlockState(pos.below());
            if (belowState->is(Blocks::SANDSTONE) || belowState->is(Blocks::STONE)
                || belowState->is(Blocks::ANDESITE) || belowState->is(Blocks::GRANITE)
                || belowState->is(Blocks::DIORITE)) {
                BlockState* softState =
                    !currentState->isAir() && !isLiquidState(currentState)
                        ? currentState
                        : Blocks::SAND->defaultBlockState();
                for (Direction direction : kAllDirections) {
                    core::BlockPos relativePos = relative(pos, direction);
                    BlockState* relativeState = level->getBlockState(relativePos);
                    if (relativeState->isAir() || isLiquidState(relativeState)) {
                        core::BlockPos belowRelativePos = relativePos.below();
                        BlockState* belowRelativeState = level->getBlockState(belowRelativePos);
                        if ((belowRelativeState->isAir() || isLiquidState(belowRelativeState))
                            && direction != Direction::UP) {
                            level->setBlock(relativePos, belowState, 3);
                        } else {
                            level->setBlock(relativePos, softState, 3);
                        }
                    }
                }
                // Java: this.boundingBox = new BoundingBox(pos) - re-anchor.
                self.boundingBox = BoundingBox(pos.getX(), pos.getY(), pos.getZ(),
                                               pos.getX(), pos.getY(), pos.getZ());
                createChest(level, chunkBB, random, pos,
                            "minecraft:chests/buried_treasure");
                return;
            }
            pos = pos.below();
        }
    }
};

// Reference: SwampHutPiece.postProcess. Witch/cat spawns are entities (out
// of scope; they draw from the LEVEL random, not the worldgen random, so
// skipping them cannot desync the block RNG stream).
class SwampHutBehavior final : public OrientedPieceBehavior {
public:
    using OrientedPieceBehavior::OrientedPieceBehavior;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        (void)referencePos;
        (void)random;
        bind(self);
        if (!updateAverageGroundHeight(level, chunkBB, 0)) return;

        BlockState* planks = Blocks::getDefaultState("minecraft:spruce_planks");
        BlockState* log = Blocks::OAK_LOG->defaultBlockState();
        BlockState* fence = Blocks::OAK_FENCE->defaultBlockState();
        BlockState* air = Blocks::AIR->defaultBlockState();

        generateBox(level, chunkBB, 1, 1, 1, 5, 1, 7, planks, planks, false);
        generateBox(level, chunkBB, 1, 4, 2, 5, 4, 7, planks, planks, false);
        generateBox(level, chunkBB, 2, 1, 0, 4, 1, 0, planks, planks, false);
        generateBox(level, chunkBB, 2, 2, 2, 3, 3, 2, planks, planks, false);
        generateBox(level, chunkBB, 1, 2, 3, 1, 3, 6, planks, planks, false);
        generateBox(level, chunkBB, 5, 2, 3, 5, 3, 6, planks, planks, false);
        generateBox(level, chunkBB, 2, 2, 7, 4, 3, 7, planks, planks, false);
        generateBox(level, chunkBB, 1, 0, 2, 1, 3, 2, log, log, false);
        generateBox(level, chunkBB, 5, 0, 2, 5, 3, 2, log, log, false);
        generateBox(level, chunkBB, 1, 0, 7, 1, 3, 7, log, log, false);
        generateBox(level, chunkBB, 5, 0, 7, 5, 3, 7, log, log, false);
        placeBlock(level, fence, 2, 3, 2, chunkBB);
        placeBlock(level, fence, 3, 3, 7, chunkBB);
        placeBlock(level, air, 1, 3, 4, chunkBB);
        placeBlock(level, air, 5, 3, 4, chunkBB);
        placeBlock(level, air, 5, 3, 5, chunkBB);
        placeBlock(level, Blocks::getDefaultState("minecraft:potted_red_mushroom"),
                   1, 3, 5, chunkBB);
        placeBlock(level, Blocks::getDefaultState("minecraft:crafting_table"),
                   3, 2, 6, chunkBB);
        placeBlock(level, Blocks::getDefaultState("minecraft:cauldron"), 4, 2, 6, chunkBB);
        placeBlock(level, fence, 1, 2, 1, chunkBB);
        placeBlock(level, fence, 5, 2, 1, chunkBB);

        BlockState* stairsDefault = Blocks::getDefaultState("minecraft:spruce_stairs");
        BlockState* northStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH);
        BlockState* eastStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::EAST);
        BlockState* westStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);
        BlockState* southStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
        generateBox(level, chunkBB, 0, 4, 1, 6, 4, 1, northStairs, northStairs, false);
        generateBox(level, chunkBB, 0, 4, 2, 0, 4, 7, eastStairs, eastStairs, false);
        generateBox(level, chunkBB, 6, 4, 2, 6, 4, 7, westStairs, westStairs, false);
        generateBox(level, chunkBB, 0, 4, 8, 6, 4, 8, southStairs, southStairs, false);
        using SS = world::level::block::state::properties::StairsShape;
        auto shaped = [](BlockState* base, SS::Value shape) {
            return base->setValue(*BlockStateProperties::STAIRS_SHAPE, SS(shape));
        };
        placeBlock(level, shaped(northStairs, SS::OUTER_RIGHT), 0, 4, 1, chunkBB);
        placeBlock(level, shaped(northStairs, SS::OUTER_LEFT), 6, 4, 1, chunkBB);
        placeBlock(level, shaped(southStairs, SS::OUTER_LEFT), 0, 4, 8, chunkBB);
        placeBlock(level, shaped(southStairs, SS::OUTER_RIGHT), 6, 4, 8, chunkBB);

        for (int z = 2; z <= 7; z += 5) {
            for (int x = 1; x <= 5; x += 4) {
                fillColumnDown(level, log, x, -1, z, chunkBB);
            }
        }
        // Witch + cat spawns intentionally omitted (entities).
    }
};

// Reference: DesertPyramidPiece.postProcess + helpers. width/depth = 21,
// height = 15. Chest placement flags + suspicious-sand list + collapsed
// roof pos persist on the behavior across chunk invocations, exactly like
// the Java piece instance.
class DesertPyramidBehavior final : public OrientedPieceBehavior {
public:
    using OrientedPieceBehavior::OrientedPieceBehavior;

    static constexpr int kWidth = 21;
    static constexpr int kDepth = 21;

    bool hasPlacedChest[4] = {false, false, false, false};
    std::vector<core::BlockPos> potentialSuspiciousSandWorldPositions;
    core::BlockPos randomCollapsedRoofPos{0, 0, 0};

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        // Java: updateHeightPositionToLowestGroundHeight(level,
        // -random.nextInt(3)) - the argument draw happens on EVERY call,
        // including after the height is memoized.
        int offset = -random.nextInt(3);
        if (!updateHeightPositionToLowestGroundHeight(level, offset)) return;

        BlockState* sandstone = Blocks::getDefaultState("minecraft:sandstone");
        BlockState* cut = Blocks::getDefaultState("minecraft:cut_sandstone");
        BlockState* chiseled = Blocks::getDefaultState("minecraft:chiseled_sandstone");
        BlockState* orange = Blocks::getDefaultState("minecraft:orange_terracotta");
        BlockState* blue = Blocks::getDefaultState("minecraft:blue_terracotta");
        BlockState* air = Blocks::AIR->defaultBlockState();
        BlockState* stairsDefault = Blocks::getDefaultState("minecraft:sandstone_stairs");
        BlockState* northStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH);
        BlockState* southStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
        BlockState* eastStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::EAST);
        BlockState* westStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);

        generateBox(level, chunkBB, 0, -4, 0, kWidth - 1, 0, kDepth - 1, sandstone, sandstone, false);
        for (int i = 1; i <= 9; ++i) {
            generateBox(level, chunkBB, i, i, i, kWidth - 1 - i, i, kDepth - 1 - i, sandstone, sandstone, false);
            generateBox(level, chunkBB, i + 1, i, i + 1, kWidth - 2 - i, i, kDepth - 2 - i, air, air, false);
        }
        for (int x = 0; x < kWidth; ++x) {
            for (int z = 0; z < kDepth; ++z) {
                fillColumnDown(level, sandstone, x, -5, z, chunkBB);
            }
        }
        generateBox(level, chunkBB, 0, 0, 0, 4, 9, 4, sandstone, air, false);
        generateBox(level, chunkBB, 1, 10, 1, 3, 10, 3, sandstone, sandstone, false);
        placeBlock(level, northStairs, 2, 10, 0, chunkBB);
        placeBlock(level, southStairs, 2, 10, 4, chunkBB);
        placeBlock(level, eastStairs, 0, 10, 2, chunkBB);
        placeBlock(level, westStairs, 4, 10, 2, chunkBB);
        generateBox(level, chunkBB, kWidth - 5, 0, 0, kWidth - 1, 9, 4, sandstone, air, false);
        generateBox(level, chunkBB, kWidth - 4, 10, 1, kWidth - 2, 10, 3, sandstone, sandstone, false);
        placeBlock(level, northStairs, kWidth - 3, 10, 0, chunkBB);
        placeBlock(level, southStairs, kWidth - 3, 10, 4, chunkBB);
        placeBlock(level, eastStairs, kWidth - 5, 10, 2, chunkBB);
        placeBlock(level, westStairs, kWidth - 1, 10, 2, chunkBB);
        generateBox(level, chunkBB, 8, 0, 0, 12, 4, 4, sandstone, air, false);
        generateBox(level, chunkBB, 9, 1, 0, 11, 3, 4, air, air, false);
        placeBlock(level, cut, 9, 1, 1, chunkBB);
        placeBlock(level, cut, 9, 2, 1, chunkBB);
        placeBlock(level, cut, 9, 3, 1, chunkBB);
        placeBlock(level, cut, 10, 3, 1, chunkBB);
        placeBlock(level, cut, 11, 3, 1, chunkBB);
        placeBlock(level, cut, 11, 2, 1, chunkBB);
        placeBlock(level, cut, 11, 1, 1, chunkBB);
        generateBox(level, chunkBB, 4, 1, 1, 8, 3, 3, sandstone, air, false);
        generateBox(level, chunkBB, 4, 1, 2, 8, 2, 2, air, air, false);
        generateBox(level, chunkBB, 12, 1, 1, 16, 3, 3, sandstone, air, false);
        generateBox(level, chunkBB, 12, 1, 2, 16, 2, 2, air, air, false);
        generateBox(level, chunkBB, 5, 4, 5, kWidth - 6, 4, kDepth - 6, sandstone, sandstone, false);
        generateBox(level, chunkBB, 9, 4, 9, 11, 4, 11, air, air, false);
        generateBox(level, chunkBB, 8, 1, 8, 8, 3, 8, cut, cut, false);
        generateBox(level, chunkBB, 12, 1, 8, 12, 3, 8, cut, cut, false);
        generateBox(level, chunkBB, 8, 1, 12, 8, 3, 12, cut, cut, false);
        generateBox(level, chunkBB, 12, 1, 12, 12, 3, 12, cut, cut, false);
        generateBox(level, chunkBB, 1, 1, 5, 4, 4, 11, sandstone, sandstone, false);
        generateBox(level, chunkBB, kWidth - 5, 1, 5, kWidth - 2, 4, 11, sandstone, sandstone, false);
        generateBox(level, chunkBB, 6, 7, 9, 6, 7, 11, sandstone, sandstone, false);
        generateBox(level, chunkBB, kWidth - 7, 7, 9, kWidth - 7, 7, 11, sandstone, sandstone, false);
        generateBox(level, chunkBB, 5, 5, 9, 5, 7, 11, cut, cut, false);
        generateBox(level, chunkBB, kWidth - 6, 5, 9, kWidth - 6, 7, 11, cut, cut, false);
        placeBlock(level, air, 5, 5, 10, chunkBB);
        placeBlock(level, air, 5, 6, 10, chunkBB);
        placeBlock(level, air, 6, 6, 10, chunkBB);
        placeBlock(level, air, kWidth - 6, 5, 10, chunkBB);
        placeBlock(level, air, kWidth - 6, 6, 10, chunkBB);
        placeBlock(level, air, kWidth - 7, 6, 10, chunkBB);
        generateBox(level, chunkBB, 2, 4, 4, 2, 6, 4, air, air, false);
        generateBox(level, chunkBB, kWidth - 3, 4, 4, kWidth - 3, 6, 4, air, air, false);
        placeBlock(level, northStairs, 2, 4, 5, chunkBB);
        placeBlock(level, northStairs, 2, 3, 4, chunkBB);
        placeBlock(level, northStairs, kWidth - 3, 4, 5, chunkBB);
        placeBlock(level, northStairs, kWidth - 3, 3, 4, chunkBB);
        generateBox(level, chunkBB, 1, 1, 3, 2, 2, 3, sandstone, sandstone, false);
        generateBox(level, chunkBB, kWidth - 3, 1, 3, kWidth - 2, 2, 3, sandstone, sandstone, false);
        placeBlock(level, sandstone, 1, 1, 2, chunkBB);
        placeBlock(level, sandstone, kWidth - 2, 1, 2, chunkBB);
        placeBlock(level, Blocks::getDefaultState("minecraft:sandstone_slab"), 1, 2, 2, chunkBB);
        placeBlock(level, Blocks::getDefaultState("minecraft:sandstone_slab"), kWidth - 2, 2, 2, chunkBB);
        placeBlock(level, westStairs, 2, 1, 2, chunkBB);
        placeBlock(level, eastStairs, kWidth - 3, 1, 2, chunkBB);
        generateBox(level, chunkBB, 4, 3, 5, 4, 3, 17, sandstone, sandstone, false);
        generateBox(level, chunkBB, kWidth - 5, 3, 5, kWidth - 5, 3, 17, sandstone, sandstone, false);
        generateBox(level, chunkBB, 3, 1, 5, 4, 2, 16, air, air, false);
        generateBox(level, chunkBB, kWidth - 6, 1, 5, kWidth - 5, 2, 16, air, air, false);
        for (int z = 5; z <= 17; z += 2) {
            placeBlock(level, cut, 4, 1, z, chunkBB);
            placeBlock(level, chiseled, 4, 2, z, chunkBB);
            placeBlock(level, cut, kWidth - 5, 1, z, chunkBB);
            placeBlock(level, chiseled, kWidth - 5, 2, z, chunkBB);
        }
        placeBlock(level, orange, 10, 0, 7, chunkBB);
        placeBlock(level, orange, 10, 0, 8, chunkBB);
        placeBlock(level, orange, 9, 0, 9, chunkBB);
        placeBlock(level, orange, 11, 0, 9, chunkBB);
        placeBlock(level, orange, 8, 0, 10, chunkBB);
        placeBlock(level, orange, 12, 0, 10, chunkBB);
        placeBlock(level, orange, 7, 0, 10, chunkBB);
        placeBlock(level, orange, 13, 0, 10, chunkBB);
        placeBlock(level, orange, 9, 0, 11, chunkBB);
        placeBlock(level, orange, 11, 0, 11, chunkBB);
        placeBlock(level, orange, 10, 0, 12, chunkBB);
        placeBlock(level, orange, 10, 0, 13, chunkBB);
        placeBlock(level, blue, 10, 0, 10, chunkBB);
        for (int x = 0; x <= kWidth - 1; x += kWidth - 1) {
            placeBlock(level, cut, x, 2, 1, chunkBB);
            placeBlock(level, orange, x, 2, 2, chunkBB);
            placeBlock(level, cut, x, 2, 3, chunkBB);
            placeBlock(level, cut, x, 3, 1, chunkBB);
            placeBlock(level, orange, x, 3, 2, chunkBB);
            placeBlock(level, cut, x, 3, 3, chunkBB);
            placeBlock(level, orange, x, 4, 1, chunkBB);
            placeBlock(level, chiseled, x, 4, 2, chunkBB);
            placeBlock(level, orange, x, 4, 3, chunkBB);
            placeBlock(level, cut, x, 5, 1, chunkBB);
            placeBlock(level, orange, x, 5, 2, chunkBB);
            placeBlock(level, cut, x, 5, 3, chunkBB);
            placeBlock(level, orange, x, 6, 1, chunkBB);
            placeBlock(level, chiseled, x, 6, 2, chunkBB);
            placeBlock(level, orange, x, 6, 3, chunkBB);
            placeBlock(level, orange, x, 7, 1, chunkBB);
            placeBlock(level, orange, x, 7, 2, chunkBB);
            placeBlock(level, orange, x, 7, 3, chunkBB);
            placeBlock(level, cut, x, 8, 1, chunkBB);
            placeBlock(level, cut, x, 8, 2, chunkBB);
            placeBlock(level, cut, x, 8, 3, chunkBB);
        }
        for (int x = 2; x <= kWidth - 3; x += kWidth - 3 - 2) {
            placeBlock(level, cut, x - 1, 2, 0, chunkBB);
            placeBlock(level, orange, x, 2, 0, chunkBB);
            placeBlock(level, cut, x + 1, 2, 0, chunkBB);
            placeBlock(level, cut, x - 1, 3, 0, chunkBB);
            placeBlock(level, orange, x, 3, 0, chunkBB);
            placeBlock(level, cut, x + 1, 3, 0, chunkBB);
            placeBlock(level, orange, x - 1, 4, 0, chunkBB);
            placeBlock(level, chiseled, x, 4, 0, chunkBB);
            placeBlock(level, orange, x + 1, 4, 0, chunkBB);
            placeBlock(level, cut, x - 1, 5, 0, chunkBB);
            placeBlock(level, orange, x, 5, 0, chunkBB);
            placeBlock(level, cut, x + 1, 5, 0, chunkBB);
            placeBlock(level, orange, x - 1, 6, 0, chunkBB);
            placeBlock(level, chiseled, x, 6, 0, chunkBB);
            placeBlock(level, orange, x + 1, 6, 0, chunkBB);
            placeBlock(level, orange, x - 1, 7, 0, chunkBB);
            placeBlock(level, orange, x, 7, 0, chunkBB);
            placeBlock(level, orange, x + 1, 7, 0, chunkBB);
            placeBlock(level, cut, x - 1, 8, 0, chunkBB);
            placeBlock(level, cut, x, 8, 0, chunkBB);
            placeBlock(level, cut, x + 1, 8, 0, chunkBB);
        }
        generateBox(level, chunkBB, 8, 4, 0, 12, 6, 0, cut, cut, false);
        placeBlock(level, air, 8, 6, 0, chunkBB);
        placeBlock(level, air, 12, 6, 0, chunkBB);
        placeBlock(level, orange, 9, 5, 0, chunkBB);
        placeBlock(level, chiseled, 10, 5, 0, chunkBB);
        placeBlock(level, orange, 11, 5, 0, chunkBB);
        generateBox(level, chunkBB, 8, -14, 8, 12, -11, 12, cut, cut, false);
        generateBox(level, chunkBB, 8, -10, 8, 12, -10, 12, chiseled, chiseled, false);
        generateBox(level, chunkBB, 8, -9, 8, 12, -9, 12, cut, cut, false);
        generateBox(level, chunkBB, 8, -8, 8, 12, -1, 12, sandstone, sandstone, false);
        generateBox(level, chunkBB, 9, -11, 9, 11, -1, 11, air, air, false);
        placeBlock(level, Blocks::getDefaultState("minecraft:stone_pressure_plate"),
                   10, -11, 10, chunkBB);
        generateBox(level, chunkBB, 9, -13, 9, 11, -13, 11,
                    Blocks::getDefaultState("minecraft:tnt"), air, false);
        placeBlock(level, air, 8, -11, 10, chunkBB);
        placeBlock(level, air, 8, -10, 10, chunkBB);
        placeBlock(level, chiseled, 7, -10, 10, chunkBB);
        placeBlock(level, cut, 7, -11, 10, chunkBB);
        placeBlock(level, air, 12, -11, 10, chunkBB);
        placeBlock(level, air, 12, -10, 10, chunkBB);
        placeBlock(level, chiseled, 13, -10, 10, chunkBB);
        placeBlock(level, cut, 13, -11, 10, chunkBB);
        placeBlock(level, air, 10, -11, 8, chunkBB);
        placeBlock(level, air, 10, -10, 8, chunkBB);
        placeBlock(level, chiseled, 10, -10, 7, chunkBB);
        placeBlock(level, cut, 10, -11, 7, chunkBB);
        placeBlock(level, air, 10, -11, 12, chunkBB);
        placeBlock(level, air, 10, -10, 12, chunkBB);
        placeBlock(level, chiseled, 10, -10, 13, chunkBB);
        placeBlock(level, cut, 10, -11, 13, chunkBB);

        // Reference: Direction.Plane.HORIZONTAL [N,E,S,W];
        // get2DDataValue: SOUTH=0, WEST=1, NORTH=2, EAST=3.
        static constexpr Direction kPlane[4] = {
            Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST};
        static constexpr int k2D[4] = {2, 3, 0, 1};  // parallel to kPlane
        for (int i = 0; i < 4; ++i) {
            if (!hasPlacedChest[k2D[i]]) {
                int xo = core::getStepX(kPlane[i]) * 2;
                int zo = core::getStepZ(kPlane[i]) * 2;
                hasPlacedChest[k2D[i]] =
                    createChest(level, chunkBB, random, 10 + xo, -11, 10 + zo, "minecraft:chests/desert_pyramid");
            }
        }

        addCellar(level, chunkBB);
    }

private:
    void addCellar(WorldGenLevel* level, const BoundingBox& chunkBB) {
        core::BlockPos roomCenter(16, -4, 13);
        addCellarStairs(roomCenter, level, chunkBB);
        addCellarRoom(roomCenter, level, chunkBB);
    }

    void addCellarStairs(const core::BlockPos& roomCenter, WorldGenLevel* level,
                         const BoundingBox& chunkBB) {
        int x = roomCenter.getX();
        int y = roomCenter.getY();
        int z = roomCenter.getZ();
        BlockState* stairs = state_transforms::rotateState(
            Blocks::getDefaultState("minecraft:sandstone_stairs"), ROT_CCW90);
        placeBlock(level, stairs, 13, -1, 17, chunkBB);
        placeBlock(level, stairs, 14, -2, 17, chunkBB);
        placeBlock(level, stairs, 15, -3, 17, chunkBB);
        BlockState* sand = Blocks::SAND->defaultBlockState();
        BlockState* sandstone = Blocks::getDefaultState("minecraft:sandstone");
        // Reference: level.getRandom().nextBoolean() - the deterministic
        // per-region worldgen_region_random, not the passed random.
        bool variant = level->getRandom().nextBoolean();
        placeBlock(level, sand, x - 4, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 3, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 2, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 1, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 2, y + 3, z + 4, chunkBB);
        placeBlock(level, variant ? sand : sandstone, x - 1, y + 3, z + 4, chunkBB);
        placeBlock(level, !variant ? sand : sandstone, x, y + 3, z + 4, chunkBB);
        placeBlock(level, sand, x - 1, y + 2, z + 4, chunkBB);
        placeBlock(level, sandstone, x, y + 2, z + 4, chunkBB);
        placeBlock(level, sand, x, y + 1, z + 4, chunkBB);
    }

    void addCellarRoom(const core::BlockPos& roomCenter, WorldGenLevel* level,
                       const BoundingBox& chunkBB) {
        int x = roomCenter.getX();
        int y = roomCenter.getY();
        int z = roomCenter.getZ();
        BlockState* cut = Blocks::getDefaultState("minecraft:cut_sandstone");
        BlockState* chiseled = Blocks::getDefaultState("minecraft:chiseled_sandstone");
        generateBox(level, chunkBB, x - 3, y + 1, z - 3, x - 3, y + 1, z + 2, cut, cut, true);
        generateBox(level, chunkBB, x + 3, y + 1, z - 3, x + 3, y + 1, z + 2, cut, cut, true);
        generateBox(level, chunkBB, x - 3, y + 1, z - 3, x + 3, y + 1, z - 2, cut, cut, true);
        generateBox(level, chunkBB, x - 3, y + 1, z + 3, x + 3, y + 1, z + 3, cut, cut, true);
        generateBox(level, chunkBB, x - 3, y + 2, z - 3, x - 3, y + 2, z + 2, chiseled, chiseled, true);
        generateBox(level, chunkBB, x + 3, y + 2, z - 3, x + 3, y + 2, z + 2, chiseled, chiseled, true);
        generateBox(level, chunkBB, x - 3, y + 2, z - 3, x + 3, y + 2, z - 2, chiseled, chiseled, true);
        generateBox(level, chunkBB, x - 3, y + 2, z + 3, x + 3, y + 2, z + 3, chiseled, chiseled, true);
        generateBox(level, chunkBB, x - 3, -1, z - 3, x - 3, -1, z + 2, cut, cut, true);
        generateBox(level, chunkBB, x + 3, -1, z - 3, x + 3, -1, z + 2, cut, cut, true);
        generateBox(level, chunkBB, x - 3, -1, z - 3, x + 3, -1, z - 2, cut, cut, true);
        generateBox(level, chunkBB, x - 3, -1, z + 3, x + 3, -1, z + 3, cut, cut, true);
        placeSandBox(x - 2, y + 1, z - 2, x + 2, y + 3, z + 2);
        placeCollapsedRoof(level, chunkBB, x - 2, y + 4, z - 2, x + 2, z + 2);
        BlockState* orange = Blocks::getDefaultState("minecraft:orange_terracotta");
        BlockState* blue = Blocks::getDefaultState("minecraft:blue_terracotta");
        placeBlock(level, blue, x, y, z, chunkBB);
        placeBlock(level, orange, x + 1, y, z - 1, chunkBB);
        placeBlock(level, orange, x + 1, y, z + 1, chunkBB);
        placeBlock(level, orange, x - 1, y, z - 1, chunkBB);
        placeBlock(level, orange, x - 1, y, z + 1, chunkBB);
        placeBlock(level, orange, x + 2, y, z, chunkBB);
        placeBlock(level, orange, x - 2, y, z, chunkBB);
        placeBlock(level, orange, x, y, z + 2, chunkBB);
        placeBlock(level, orange, x, y, z - 2, chunkBB);
        placeBlock(level, orange, x + 3, y, z, chunkBB);
        placeSand(x + 3, y + 1, z);
        placeSand(x + 3, y + 2, z);
        placeBlock(level, cut, x + 4, y + 1, z, chunkBB);
        placeBlock(level, chiseled, x + 4, y + 2, z, chunkBB);
        placeBlock(level, orange, x - 3, y, z, chunkBB);
        placeSand(x - 3, y + 1, z);
        placeSand(x - 3, y + 2, z);
        placeBlock(level, cut, x - 4, y + 1, z, chunkBB);
        placeBlock(level, chiseled, x - 4, y + 2, z, chunkBB);
        placeBlock(level, orange, x, y, z + 3, chunkBB);
        placeSand(x, y + 1, z + 3);
        placeSand(x, y + 2, z + 3);
        placeBlock(level, orange, x, y, z - 3, chunkBB);
        placeSand(x, y + 1, z - 3);
        placeSand(x, y + 2, z - 3);
        placeBlock(level, cut, x, y + 1, z - 4, chunkBB);
        placeBlock(level, chiseled, x, -2, z - 4, chunkBB);
    }

    void placeSand(int x, int y, int z) {
        potentialSuspiciousSandWorldPositions.push_back(worldPos(x, y, z));
    }

    void placeSandBox(int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                for (int z = z0; z <= z1; ++z) {
                    placeSand(x, y, z);
                }
            }
        }
    }

    void placeCollapsedRoofPiece(WorldGenLevel* level, int x, int y, int z,
                                 const BoundingBox& chunkBB) {
        if (level->getRandom().nextFloat() < 0.33f) {
            placeBlock(level, Blocks::getDefaultState("minecraft:sandstone"), x, y, z, chunkBB);
        } else {
            placeBlock(level, Blocks::SAND->defaultBlockState(), x, y, z, chunkBB);
        }
    }

    void placeCollapsedRoof(WorldGenLevel* level, const BoundingBox& chunkBB,
                            int x0, int y0, int z0, int x1, int z1) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                placeCollapsedRoofPiece(level, x, y0, z, chunkBB);
            }
        }
        // Reference: RandomSource.create(seed).forkPositional().at(worldPos).
        core::BlockPos anchor = worldPos(x0, y0, z0);
        LegacyRandomSource base(level->getSeed());
        LegacyRandomSource random =
            base.forkPositional().at(anchor.getX(), anchor.getY(), anchor.getZ());
        int roofPosX = random.nextInt(x1 - x0 + 1) + x0;  // nextIntBetweenInclusive
        int roofPosZ = random.nextInt(z1 - z0 + 1) + z0;
        randomCollapsedRoofPos = core::BlockPos(
            worldX(roofPosX, roofPosZ), worldY(y0), worldZ(roofPosX, roofPosZ));
    }
};

// Reference: JungleTemplePiece.postProcess. Traps/chests memoize on the
// behavior instance like the Java piece fields.
class JungleTempleBehavior final : public OrientedPieceBehavior {
public:
    using OrientedPieceBehavior::OrientedPieceBehavior;

    static constexpr int kWidth = 12;
    static constexpr int kDepth = 15;

    bool placedMainChest = false;
    bool placedHiddenChest = false;
    bool placedTrap1 = false;
    bool placedTrap2 = false;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        if (!updateAverageGroundHeight(level, chunkBB, 0)) return;

        // Reference: MossStoneSelector - nextFloat < 0.4 -> cobblestone,
        // else mossy_cobblestone.
        BlockState* cobble = Blocks::getDefaultState("minecraft:cobblestone");
        BlockState* mossy = Blocks::getDefaultState("minecraft:mossy_cobblestone");
        auto stone = [cobble, mossy](WorldgenRandom& r, int, int, int, bool) {
            return r.nextFloat() < 0.4f ? cobble : mossy;
        };
        BlockState* air = Blocks::AIR->defaultBlockState();

        generateBox(level, chunkBB, 0, -4, 0, kWidth - 1, 0, kDepth - 1, false, random, stone);
        generateBox(level, chunkBB, 2, 1, 2, 9, 2, 2, false, random, stone);
        generateBox(level, chunkBB, 2, 1, 12, 9, 2, 12, false, random, stone);
        generateBox(level, chunkBB, 2, 1, 3, 2, 2, 11, false, random, stone);
        generateBox(level, chunkBB, 9, 1, 3, 9, 2, 11, false, random, stone);
        generateBox(level, chunkBB, 1, 3, 1, 10, 6, 1, false, random, stone);
        generateBox(level, chunkBB, 1, 3, 13, 10, 6, 13, false, random, stone);
        generateBox(level, chunkBB, 1, 3, 2, 1, 6, 12, false, random, stone);
        generateBox(level, chunkBB, 10, 3, 2, 10, 6, 12, false, random, stone);
        generateBox(level, chunkBB, 2, 3, 2, 9, 3, 12, false, random, stone);
        generateBox(level, chunkBB, 2, 6, 2, 9, 6, 12, false, random, stone);
        generateBox(level, chunkBB, 3, 7, 3, 8, 7, 11, false, random, stone);
        generateBox(level, chunkBB, 4, 8, 4, 7, 8, 10, false, random, stone);
        generateAirBox(level, chunkBB, 3, 1, 3, 8, 2, 11);
        generateAirBox(level, chunkBB, 4, 3, 6, 7, 3, 9);
        generateAirBox(level, chunkBB, 2, 4, 2, 9, 5, 12);
        generateAirBox(level, chunkBB, 4, 6, 5, 7, 6, 9);
        generateAirBox(level, chunkBB, 5, 7, 6, 6, 7, 8);
        generateAirBox(level, chunkBB, 5, 1, 2, 6, 2, 2);
        generateAirBox(level, chunkBB, 5, 2, 12, 6, 2, 12);
        generateAirBox(level, chunkBB, 5, 5, 1, 6, 5, 1);
        generateAirBox(level, chunkBB, 5, 5, 13, 6, 5, 13);
        placeBlock(level, air, 1, 5, 5, chunkBB);
        placeBlock(level, air, 10, 5, 5, chunkBB);
        placeBlock(level, air, 1, 5, 9, chunkBB);
        placeBlock(level, air, 10, 5, 9, chunkBB);
        for (int z = 0; z <= 14; z += 14) {
            generateBox(level, chunkBB, 2, 4, z, 2, 5, z, false, random, stone);
            generateBox(level, chunkBB, 4, 4, z, 4, 5, z, false, random, stone);
            generateBox(level, chunkBB, 7, 4, z, 7, 5, z, false, random, stone);
            generateBox(level, chunkBB, 9, 4, z, 9, 5, z, false, random, stone);
        }
        generateBox(level, chunkBB, 5, 6, 0, 6, 6, 0, false, random, stone);
        for (int x = 0; x <= 11; x += 11) {
            for (int z = 2; z <= 12; z += 2) {
                generateBox(level, chunkBB, x, 4, z, x, 5, z, false, random, stone);
            }
            generateBox(level, chunkBB, x, 6, 5, x, 6, 5, false, random, stone);
            generateBox(level, chunkBB, x, 6, 9, x, 6, 9, false, random, stone);
        }
        generateBox(level, chunkBB, 2, 7, 2, 2, 9, 2, false, random, stone);
        generateBox(level, chunkBB, 9, 7, 2, 9, 9, 2, false, random, stone);
        generateBox(level, chunkBB, 2, 7, 12, 2, 9, 12, false, random, stone);
        generateBox(level, chunkBB, 9, 7, 12, 9, 9, 12, false, random, stone);
        generateBox(level, chunkBB, 4, 9, 4, 4, 9, 4, false, random, stone);
        generateBox(level, chunkBB, 7, 9, 4, 7, 9, 4, false, random, stone);
        generateBox(level, chunkBB, 4, 9, 10, 4, 9, 10, false, random, stone);
        generateBox(level, chunkBB, 7, 9, 10, 7, 9, 10, false, random, stone);
        generateBox(level, chunkBB, 5, 9, 7, 6, 9, 7, false, random, stone);

        BlockState* stairsDefault = Blocks::getDefaultState("minecraft:cobblestone_stairs");
        BlockState* eastStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::EAST);
        BlockState* westStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::WEST);
        BlockState* southStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH);
        BlockState* northStairs = stairsDefault->setValue(
            *BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH);
        placeBlock(level, northStairs, 5, 9, 6, chunkBB);
        placeBlock(level, northStairs, 6, 9, 6, chunkBB);
        placeBlock(level, southStairs, 5, 9, 8, chunkBB);
        placeBlock(level, southStairs, 6, 9, 8, chunkBB);
        placeBlock(level, northStairs, 4, 0, 0, chunkBB);
        placeBlock(level, northStairs, 5, 0, 0, chunkBB);
        placeBlock(level, northStairs, 6, 0, 0, chunkBB);
        placeBlock(level, northStairs, 7, 0, 0, chunkBB);
        placeBlock(level, northStairs, 4, 1, 8, chunkBB);
        placeBlock(level, northStairs, 4, 2, 9, chunkBB);
        placeBlock(level, northStairs, 4, 3, 10, chunkBB);
        placeBlock(level, northStairs, 7, 1, 8, chunkBB);
        placeBlock(level, northStairs, 7, 2, 9, chunkBB);
        placeBlock(level, northStairs, 7, 3, 10, chunkBB);
        generateBox(level, chunkBB, 4, 1, 9, 4, 1, 9, false, random, stone);
        generateBox(level, chunkBB, 7, 1, 9, 7, 1, 9, false, random, stone);
        generateBox(level, chunkBB, 4, 1, 10, 7, 2, 10, false, random, stone);
        generateBox(level, chunkBB, 5, 4, 5, 6, 4, 5, false, random, stone);
        placeBlock(level, eastStairs, 4, 4, 5, chunkBB);
        placeBlock(level, westStairs, 7, 4, 5, chunkBB);
        for (int i = 0; i < 4; ++i) {
            placeBlock(level, southStairs, 5, 0 - i, 6 + i, chunkBB);
            placeBlock(level, southStairs, 6, 0 - i, 6 + i, chunkBB);
            generateAirBox(level, chunkBB, 5, 0 - i, 7 + i, 6, 0 - i, 9 + i);
        }
        generateAirBox(level, chunkBB, 1, -3, 12, 10, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 3, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 9, -1, 5);
        for (int z = 1; z <= 13; z += 2) {
            generateBox(level, chunkBB, 1, -3, z, 1, -2, z, false, random, stone);
        }
        for (int z = 2; z <= 12; z += 2) {
            generateBox(level, chunkBB, 1, -1, z, 3, -1, z, false, random, stone);
        }
        generateBox(level, chunkBB, 2, -2, 1, 5, -2, 1, false, random, stone);
        generateBox(level, chunkBB, 7, -2, 1, 9, -2, 1, false, random, stone);
        generateBox(level, chunkBB, 6, -3, 1, 6, -3, 1, false, random, stone);
        generateBox(level, chunkBB, 6, -1, 1, 6, -1, 1, false, random, stone);

        BlockState* hookDefault = Blocks::getDefaultState("minecraft:tripwire_hook");
        BlockState* tripwireDefault = Blocks::getDefaultState("minecraft:tripwire");
        placeBlock(level,
                   hookDefault->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::EAST)
                       ->setValue(*BlockStateProperties::ATTACHED, true),
                   1, -3, 8, chunkBB);
        placeBlock(level,
                   hookDefault->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::WEST)
                       ->setValue(*BlockStateProperties::ATTACHED, true),
                   4, -3, 8, chunkBB);
        BlockState* tripwireEW = tripwireDefault
            ->setValue(*BlockStateProperties::EAST, true)
            ->setValue(*BlockStateProperties::WEST, true)
            ->setValue(*BlockStateProperties::ATTACHED, true);
        placeBlock(level, tripwireEW, 2, -3, 8, chunkBB);
        placeBlock(level, tripwireEW, 3, -3, 8, chunkBB);

        using RS = world::level::block::state::properties::RedstoneSide;
        BlockState* wireDefault = Blocks::getDefaultState("minecraft:redstone_wire");
        BlockState* wireNS = wireDefault
            ->setValue(*BlockStateProperties::NORTH_REDSTONE, RS(RS::SIDE))
            ->setValue(*BlockStateProperties::SOUTH_REDSTONE, RS(RS::SIDE));
        placeBlock(level, wireNS, 5, -3, 7, chunkBB);
        placeBlock(level, wireNS, 5, -3, 6, chunkBB);
        placeBlock(level, wireNS, 5, -3, 5, chunkBB);
        placeBlock(level, wireNS, 5, -3, 4, chunkBB);
        placeBlock(level, wireNS, 5, -3, 3, chunkBB);
        placeBlock(level, wireNS, 5, -3, 2, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::NORTH_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::WEST_REDSTONE, RS(RS::SIDE)),
                   5, -3, 1, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::EAST_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::WEST_REDSTONE, RS(RS::SIDE)),
                   4, -3, 1, chunkBB);
        placeBlock(level, mossy, 3, -3, 1, chunkBB);
        if (!placedTrap1) {
            placedTrap1 = createDispenser(level, chunkBB, random, 3, -2, 1, Direction::NORTH, "minecraft:chests/jungle_temple_dispenser");
        }
        using VB = world::level::block::VineBlock;
        placeBlock(level,
                   Blocks::getDefaultState("minecraft:vine")->setValue(*VB::SOUTH, true),
                   3, -2, 2, chunkBB);
        placeBlock(level,
                   hookDefault->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH)
                       ->setValue(*BlockStateProperties::ATTACHED, true),
                   7, -3, 1, chunkBB);
        placeBlock(level,
                   hookDefault->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::SOUTH)
                       ->setValue(*BlockStateProperties::ATTACHED, true),
                   7, -3, 5, chunkBB);
        BlockState* tripwireNS = tripwireDefault
            ->setValue(*BlockStateProperties::NORTH, true)
            ->setValue(*BlockStateProperties::SOUTH, true)
            ->setValue(*BlockStateProperties::ATTACHED, true);
        placeBlock(level, tripwireNS, 7, -3, 2, chunkBB);
        placeBlock(level, tripwireNS, 7, -3, 3, chunkBB);
        placeBlock(level, tripwireNS, 7, -3, 4, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::EAST_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::WEST_REDSTONE, RS(RS::SIDE)),
                   8, -3, 6, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::WEST_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::SOUTH_REDSTONE, RS(RS::SIDE)),
                   9, -3, 6, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::NORTH_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::SOUTH_REDSTONE, RS(RS::UP)),
                   9, -3, 5, chunkBB);
        placeBlock(level, mossy, 9, -3, 4, chunkBB);
        placeBlock(level, wireNS, 9, -2, 4, chunkBB);
        if (!placedTrap2) {
            placedTrap2 = createDispenser(level, chunkBB, random, 9, -2, 3, Direction::WEST, "minecraft:chests/jungle_temple_dispenser");
        }
        placeBlock(level,
                   Blocks::getDefaultState("minecraft:vine")->setValue(*VB::EAST, true),
                   8, -1, 3, chunkBB);
        placeBlock(level,
                   Blocks::getDefaultState("minecraft:vine")->setValue(*VB::EAST, true),
                   8, -2, 3, chunkBB);
        if (!placedMainChest) {
            placedMainChest = createChest(level, chunkBB, random, 8, -3, 3, "minecraft:chests/jungle_temple");
        }
        placeBlock(level, mossy, 9, -3, 2, chunkBB);
        placeBlock(level, mossy, 8, -3, 1, chunkBB);
        placeBlock(level, mossy, 4, -3, 5, chunkBB);
        placeBlock(level, mossy, 5, -2, 5, chunkBB);
        placeBlock(level, mossy, 5, -1, 5, chunkBB);
        placeBlock(level, mossy, 6, -3, 5, chunkBB);
        placeBlock(level, mossy, 7, -2, 5, chunkBB);
        placeBlock(level, mossy, 7, -1, 5, chunkBB);
        placeBlock(level, mossy, 8, -3, 5, chunkBB);
        generateBox(level, chunkBB, 9, -1, 1, 9, -1, 5, false, random, stone);
        generateAirBox(level, chunkBB, 8, -3, 8, 10, -1, 10);
        BlockState* chiseledBricks = Blocks::getDefaultState("minecraft:chiseled_stone_bricks");
        placeBlock(level, chiseledBricks, 8, -2, 11, chunkBB);
        placeBlock(level, chiseledBricks, 9, -2, 11, chunkBB);
        placeBlock(level, chiseledBricks, 10, -2, 11, chunkBB);
        using AF = world::level::block::state::properties::AttachFace;
        BlockState* lever = Blocks::getDefaultState("minecraft:lever")
            ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH)
            ->setValue(*BlockStateProperties::ATTACH_FACE, AF(AF::WALL));
        placeBlock(level, lever, 8, -2, 12, chunkBB);
        placeBlock(level, lever, 9, -2, 12, chunkBB);
        placeBlock(level, lever, 10, -2, 12, chunkBB);
        generateBox(level, chunkBB, 8, -3, 8, 8, -3, 10, false, random, stone);
        generateBox(level, chunkBB, 10, -3, 8, 10, -3, 10, false, random, stone);
        placeBlock(level, mossy, 10, -2, 9, chunkBB);
        placeBlock(level, wireNS, 8, -2, 9, chunkBB);
        placeBlock(level, wireNS, 8, -2, 10, chunkBB);
        placeBlock(level,
                   wireDefault->setValue(*BlockStateProperties::NORTH_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::SOUTH_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::EAST_REDSTONE, RS(RS::SIDE))
                       ->setValue(*BlockStateProperties::WEST_REDSTONE, RS(RS::SIDE)),
                   10, -1, 9, chunkBB);
        BlockState* pistonDefault = Blocks::getDefaultState("minecraft:sticky_piston");
        placeBlock(level,
                   pistonDefault->setValue(*BlockStateProperties::FACING, Direction::UP),
                   9, -2, 8, chunkBB);
        placeBlock(level,
                   pistonDefault->setValue(*BlockStateProperties::FACING, Direction::WEST),
                   10, -2, 8, chunkBB);
        placeBlock(level,
                   pistonDefault->setValue(*BlockStateProperties::FACING, Direction::WEST),
                   10, -1, 8, chunkBB);
        placeBlock(level,
                   Blocks::getDefaultState("minecraft:repeater")
                       ->setValue(*BlockStateProperties::HORIZONTAL_FACING, Direction::NORTH),
                   10, -2, 10, chunkBB);
        if (!placedHiddenChest) {
            placedHiddenChest = createChest(level, chunkBB, random, 9, -3, 10, "minecraft:chests/jungle_temple");
        }
    }
};

// Reference: DesertPyramidStructure.afterPlace - suspicious sand pass.
void desertPyramidAfterPlace(WorldGenLevel* level, ChunkGenerator* generator,
                             WorldgenRandom& random, const BoundingBox& chunkBB,
                             const ::world::ChunkPos& chunkPos,
                             StructureStartData& start,
                             const std::shared_ptr<DesertPyramidBehavior>& piece) {
    (void)generator;
    (void)random;
    (void)chunkPos;
    // Reference: SortedArraySet.create(Vec3i::compareTo) - y, then z, then x.
    auto cmp = [](const core::BlockPos& a, const core::BlockPos& b) {
        if (a.getY() != b.getY()) return a.getY() < b.getY();
        if (a.getZ() != b.getZ()) return a.getZ() < b.getZ();
        return a.getX() < b.getX();
    };
    std::set<core::BlockPos, decltype(cmp)> unique(cmp);
    for (const core::BlockPos& pos : piece->potentialSuspiciousSandWorldPositions) {
        unique.insert(pos);
    }
    auto placeSuspiciousSand = [&](const core::BlockPos& pos) {
        if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
            level->setBlock(pos, Blocks::getDefaultState("minecraft:suspicious_sand"), 2);
            // Reference: DesertPyramidStructure.afterPlace - BrushableBlock
            // entity with LootTableSeed = pos.asLong() (NO RNG draw).
            if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->setBlockEntityNbt(pos,
                    "{LootTable:\"minecraft:archaeology/desert_pyramid\","
                    "LootTableSeed:" + std::to_string(pos.asLong())
                    + "l,components:{},id:\"minecraft:brushable_block\"}");
            }
        }
    };
    placeSuspiciousSand(piece->randomCollapsedRoofPos);

    std::vector<core::BlockPos> shuffled(unique.begin(), unique.end());
    // Reference: pieces.calculateBoundingBox().getCenter() - the piece
    // container box (single piece here, NOT the inflated start box).
    const BoundingBox& box = start.pieces[0].boundingBox;
    LegacyRandomSource base(level->getSeed());
    LegacyRandomSource positional =
        base.forkPositional().at(box.centerX(), box.centerY(), box.centerZ());
    // Util.shuffle: reverse Fisher-Yates.
    for (size_t i = shuffled.size(); i > 1; --i) {
        std::swap(shuffled[i - 1],
                  shuffled[static_cast<size_t>(positional.nextInt(static_cast<int32_t>(i)))]);
    }
    int toPlace = std::min(static_cast<int>(unique.size()), positional.nextInt(3) + 5);
    for (const core::BlockPos& pos : shuffled) {
        if (toPlace > 0) {
            --toPlace;
            placeSuspiciousSand(pos);
        } else if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
            level->setBlock(pos, Blocks::SAND->defaultBlockState(), 2);
        }
    }
}

} // namespace

std::shared_ptr<StructurePieceBehavior> buriedTreasure() {
    return std::make_shared<BuriedTreasureBehavior>();
}

std::shared_ptr<StructurePieceBehavior> swampHut(int orientation) {
    return std::make_shared<SwampHutBehavior>(orientation);
}

std::shared_ptr<StructurePieceBehavior> jungleTemple(int orientation) {
    return std::make_shared<JungleTempleBehavior>(orientation);
}

std::shared_ptr<StructurePieceBehavior> desertPyramid(
    int orientation,
    std::function<void(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&,
                       const BoundingBox&, const ::world::ChunkPos&,
                       StructureStartData&)>& outAfterPlace) {
    auto behavior = std::make_shared<DesertPyramidBehavior>(orientation);
    outAfterPlace = [behavior](WorldGenLevel* level, ChunkGenerator* generator,
                               WorldgenRandom& random, const BoundingBox& chunkBB,
                               const ::world::ChunkPos& chunkPos,
                               StructureStartData& start) {
        desertPyramidAfterPlace(level, generator, random, chunkBB, chunkPos,
                                start, behavior);
    };
    return behavior;
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
