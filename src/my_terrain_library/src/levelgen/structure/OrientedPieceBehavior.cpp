#include "levelgen/structure/OrientedPieceBehavior.h"

#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/FenceBlock.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "levelgen/Heightmap.h"
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/structure/StructurePiece.java
// and ScatteredFeaturePiece.java - transcribed method by method.

namespace minecraft {
namespace levelgen {
namespace structure {

using world::level::block::Blocks;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;
namespace props = world::level::block::state::properties;

namespace {

int stepY(Direction dir) {
    return dir == Direction::DOWN ? -1 : (dir == Direction::UP ? 1 : 0);
}

core::BlockPos relative(const core::BlockPos& pos, Direction dir) {
    return pos.offset(core::getStepX(dir), stepY(dir), core::getStepZ(dir));
}

// Reference: Rotation.rotate(Direction) - vertical axes unchanged.
Direction rotateDirection(Direction dir, int rotation) {
    if (dir == Direction::UP || dir == Direction::DOWN) return dir;
    for (int i = 0; i < rotation; ++i) {
        switch (dir) {
            case Direction::NORTH: dir = Direction::EAST; break;
            case Direction::EAST: dir = Direction::SOUTH; break;
            case Direction::SOUTH: dir = Direction::WEST; break;
            case Direction::WEST: dir = Direction::NORTH; break;
            default: break;
        }
    }
    return dir;
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

// Reference: Mirror.mirror(Direction): LEFT_RIGHT flips the Z axis,
// FRONT_BACK flips the X axis.
Direction mirrorDirection(Direction dir, int mirror) {
    if (mirror == OrientedPieceBehavior::MIRROR_LEFT_RIGHT
        && (dir == Direction::NORTH || dir == Direction::SOUTH)) {
        return core::getOpposite(dir);
    }
    if (mirror == OrientedPieceBehavior::MIRROR_FRONT_BACK
        && (dir == Direction::WEST || dir == Direction::EAST)) {
        return core::getOpposite(dir);
    }
    return dir;
}

bool isStairs(const BlockState* state) {
    const std::string& name = state->getBlock()->getIdentifier();
    return name.size() > 7 && name.compare(name.size() - 7, 7, "_stairs") == 0;
}

} // namespace

namespace state_transforms {

namespace {

// Four-side property permutation shared by tripwire/fence/vine (bools) and
// redstone wire (RedstoneSide). Reference: TripWireBlock/CrossCollisionBlock
// /VineBlock/RedStoneWireBlock rotate+mirror overrides - all permute the
// N/E/S/W properties identically.
template <typename Prop, typename Value>
BlockState* permuteSidesRotate(BlockState* state, int rotation,
                               Prop& north, Prop& east, Prop& south, Prop& west) {
    Value n = state->getValue(north);
    Value e = state->getValue(east);
    Value s = state->getValue(south);
    Value w = state->getValue(west);
    switch (rotation) {
        case OrientedPieceBehavior::ROT_CW90:
            // new north = old west, east = north, south = east, west = south.
            return state->setValue(north, w)->setValue(east, n)
                        ->setValue(south, e)->setValue(west, s);
        case OrientedPieceBehavior::ROT_CW180:
            return state->setValue(north, s)->setValue(east, w)
                        ->setValue(south, n)->setValue(west, e);
        case OrientedPieceBehavior::ROT_CCW90:
            return state->setValue(north, e)->setValue(east, s)
                        ->setValue(south, w)->setValue(west, n);
        default:
            return state;
    }
}

template <typename Prop, typename Value>
BlockState* permuteSidesMirror(BlockState* state, int mirror,
                               Prop& north, Prop& east, Prop& south, Prop& west) {
    if (mirror == OrientedPieceBehavior::MIRROR_LEFT_RIGHT) {
        Value n = state->getValue(north);
        Value s = state->getValue(south);
        return state->setValue(north, s)->setValue(south, n);
    }
    if (mirror == OrientedPieceBehavior::MIRROR_FRONT_BACK) {
        Value e = state->getValue(east);
        Value w = state->getValue(west);
        return state->setValue(east, w)->setValue(west, e);
    }
    return state;
}

} // namespace

// Reference: BaseRailBlock.rotate(RailShape, Rotation).
props::RailShape rotateRailShape(props::RailShape shape, int rotation) {
    using RS = props::RailShape;
    int v = shape.getValue();
    auto rs = [](int value) { return RS(static_cast<RS::Value>(value)); };
    switch (rotation) {
        case OrientedPieceBehavior::ROT_CW180:
            switch (v) {
                case RS::ASCENDING_EAST: return rs(RS::ASCENDING_WEST);
                case RS::ASCENDING_WEST: return rs(RS::ASCENDING_EAST);
                case RS::ASCENDING_NORTH: return rs(RS::ASCENDING_SOUTH);
                case RS::ASCENDING_SOUTH: return rs(RS::ASCENDING_NORTH);
                case RS::SOUTH_EAST: return rs(RS::NORTH_WEST);
                case RS::SOUTH_WEST: return rs(RS::NORTH_EAST);
                case RS::NORTH_WEST: return rs(RS::SOUTH_EAST);
                case RS::NORTH_EAST: return rs(RS::SOUTH_WEST);
                default: return shape;
            }
        case OrientedPieceBehavior::ROT_CCW90:
            switch (v) {
                case RS::ASCENDING_EAST: return rs(RS::ASCENDING_NORTH);
                case RS::ASCENDING_WEST: return rs(RS::ASCENDING_SOUTH);
                case RS::ASCENDING_NORTH: return rs(RS::ASCENDING_WEST);
                case RS::ASCENDING_SOUTH: return rs(RS::ASCENDING_EAST);
                case RS::NORTH_SOUTH: return rs(RS::EAST_WEST);
                case RS::EAST_WEST: return rs(RS::NORTH_SOUTH);
                case RS::SOUTH_EAST: return rs(RS::NORTH_EAST);
                case RS::SOUTH_WEST: return rs(RS::SOUTH_EAST);
                case RS::NORTH_WEST: return rs(RS::SOUTH_WEST);
                case RS::NORTH_EAST: return rs(RS::NORTH_WEST);
                default: return shape;
            }
        case OrientedPieceBehavior::ROT_CW90:
            switch (v) {
                case RS::ASCENDING_EAST: return rs(RS::ASCENDING_SOUTH);
                case RS::ASCENDING_WEST: return rs(RS::ASCENDING_NORTH);
                case RS::ASCENDING_NORTH: return rs(RS::ASCENDING_EAST);
                case RS::ASCENDING_SOUTH: return rs(RS::ASCENDING_WEST);
                case RS::NORTH_SOUTH: return rs(RS::EAST_WEST);
                case RS::EAST_WEST: return rs(RS::NORTH_SOUTH);
                case RS::SOUTH_EAST: return rs(RS::SOUTH_WEST);
                case RS::SOUTH_WEST: return rs(RS::NORTH_WEST);
                case RS::NORTH_WEST: return rs(RS::NORTH_EAST);
                case RS::NORTH_EAST: return rs(RS::SOUTH_EAST);
                default: return shape;
            }
        default:
            return shape;
    }
}

// Reference: BaseRailBlock.mirror(RailShape, Mirror).
props::RailShape mirrorRailShape(props::RailShape shape, int mirror) {
    using RS = props::RailShape;
    int v = shape.getValue();
    auto rs = [](int value) { return RS(static_cast<RS::Value>(value)); };
    if (mirror == OrientedPieceBehavior::MIRROR_LEFT_RIGHT) {
        switch (v) {
            case RS::ASCENDING_NORTH: return rs(RS::ASCENDING_SOUTH);
            case RS::ASCENDING_SOUTH: return rs(RS::ASCENDING_NORTH);
            case RS::SOUTH_EAST: return rs(RS::NORTH_EAST);
            case RS::SOUTH_WEST: return rs(RS::NORTH_WEST);
            case RS::NORTH_WEST: return rs(RS::SOUTH_WEST);
            case RS::NORTH_EAST: return rs(RS::SOUTH_EAST);
            default: return shape;
        }
    }
    if (mirror == OrientedPieceBehavior::MIRROR_FRONT_BACK) {
        switch (v) {
            case RS::ASCENDING_EAST: return rs(RS::ASCENDING_WEST);
            case RS::ASCENDING_WEST: return rs(RS::ASCENDING_EAST);
            case RS::SOUTH_EAST: return rs(RS::SOUTH_WEST);
            case RS::SOUTH_WEST: return rs(RS::SOUTH_EAST);
            case RS::NORTH_WEST: return rs(RS::NORTH_EAST);
            case RS::NORTH_EAST: return rs(RS::NORTH_WEST);
            default: return shape;
        }
    }
    return shape;
}

BlockState* rotateState(BlockState* state, int rotation) {
    if (rotation == OrientedPieceBehavior::ROT_NONE) return state;
    if (state->hasProperty(BlockStateProperties::RAIL_SHAPE)) {
        return state->setValue(*BlockStateProperties::RAIL_SHAPE,
            rotateRailShape(state->getValue(*BlockStateProperties::RAIL_SHAPE), rotation));
    }
    // Reference: RotatedPillarBlock.rotatePillar - CW90/CCW90 swap X and Z.
    if (state->hasProperty(BlockStateProperties::AXIS)) {
        if (rotation == OrientedPieceBehavior::ROT_CW90
            || rotation == OrientedPieceBehavior::ROT_CCW90) {
            core::Axis axis = state->getValue(*BlockStateProperties::AXIS);
            if (axis == core::Axis::X) {
                return state->setValue(*BlockStateProperties::AXIS, core::Axis::Z);
            }
            if (axis == core::Axis::Z) {
                return state->setValue(*BlockStateProperties::AXIS, core::Axis::X);
            }
        }
        return state;
    }
    // Stairs rotate exactly like the generic horizontal-facing case
    // (StairBlock.rotate only rotates FACING).
    if (state->hasProperty(BlockStateProperties::HORIZONTAL_FACING)) {
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        return state->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                               rotateDirection(facing, rotation));
    }
    // DirectionalBlock (piston, dispenser, ...): vertical facings unchanged.
    if (state->hasProperty(BlockStateProperties::FACING)) {
        Direction facing = state->getValue(*BlockStateProperties::FACING);
        return state->setValue(*BlockStateProperties::FACING,
                               rotateDirection(facing, rotation));
    }
    // Boolean four-side blocks (tripwire via shared props, fence, vine).
    if (state->hasProperty(BlockStateProperties::NORTH)) {
        return permuteSidesRotate<props::BooleanProperty, bool>(
            state, rotation, *BlockStateProperties::NORTH, *BlockStateProperties::EAST,
            *BlockStateProperties::SOUTH, *BlockStateProperties::WEST);
    }
    if (state->hasProperty(world::level::block::FenceBlock::NORTH)) {
        using FB = world::level::block::FenceBlock;
        return permuteSidesRotate<props::BooleanProperty, bool>(
            state, rotation, *FB::NORTH, *FB::EAST, *FB::SOUTH, *FB::WEST);
    }
    if (state->hasProperty(world::level::block::VineBlock::NORTH)) {
        using VB = world::level::block::VineBlock;
        return permuteSidesRotate<props::BooleanProperty, bool>(
            state, rotation, *VB::NORTH, *VB::EAST, *VB::SOUTH, *VB::WEST);
    }
    if (state->hasProperty(BlockStateProperties::NORTH_REDSTONE)) {
        return permuteSidesRotate<props::EnumProperty<props::RedstoneSide>, props::RedstoneSide>(
            state, rotation, *BlockStateProperties::NORTH_REDSTONE,
            *BlockStateProperties::EAST_REDSTONE, *BlockStateProperties::SOUTH_REDSTONE,
            *BlockStateProperties::WEST_REDSTONE);
    }
    // Reference: WallBlock.rotate - the four WallSide properties permute.
    if (state->hasProperty(BlockStateProperties::NORTH_WALL)) {
        return permuteSidesRotate<props::EnumProperty<props::WallSide>, props::WallSide>(
            state, rotation, *BlockStateProperties::NORTH_WALL,
            *BlockStateProperties::EAST_WALL, *BlockStateProperties::SOUTH_WALL,
            *BlockStateProperties::WEST_WALL);
    }
    return state;
}

BlockState* mirrorState(BlockState* state, int mirror) {
    if (mirror == OrientedPieceBehavior::MIRROR_NONE) return state;
    if (state->hasProperty(BlockStateProperties::RAIL_SHAPE)) {
        return state->setValue(*BlockStateProperties::RAIL_SHAPE,
            mirrorRailShape(state->getValue(*BlockStateProperties::RAIL_SHAPE), mirror));
    }
    if (isStairs(state)) {
        // Reference: StairBlock.mirror - active only when FACING is on the
        // mirrored axis; rotate CW180 then swap shapes (LEFT_RIGHT swaps
        // OUTER and INNER pairs; FRONT_BACK swaps only OUTER).
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        props::StairsShape shape = state->getValue(*BlockStateProperties::STAIRS_SHAPE);
        bool axisZ = facing == Direction::NORTH || facing == Direction::SOUTH;
        bool axisX = facing == Direction::WEST || facing == Direction::EAST;
        auto rotated = [&]() {
            return rotateState(state, OrientedPieceBehavior::ROT_CW180);
        };
        if (mirror == OrientedPieceBehavior::MIRROR_LEFT_RIGHT && axisZ) {
            switch (shape.getValue()) {
                case props::StairsShape::OUTER_LEFT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::OUTER_RIGHT));
                case props::StairsShape::OUTER_RIGHT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::OUTER_LEFT));
                case props::StairsShape::INNER_LEFT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::INNER_RIGHT));
                case props::StairsShape::INNER_RIGHT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::INNER_LEFT));
                default:
                    return rotated();
            }
        }
        if (mirror == OrientedPieceBehavior::MIRROR_FRONT_BACK && axisX) {
            switch (shape.getValue()) {
                case props::StairsShape::OUTER_LEFT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::OUTER_RIGHT));
                case props::StairsShape::OUTER_RIGHT:
                    return rotated()->setValue(*BlockStateProperties::STAIRS_SHAPE,
                        props::StairsShape(props::StairsShape::OUTER_LEFT));
                default:
                    // STRAIGHT and INNER shapes keep their shape.
                    return rotated();
            }
        }
        return state;
    }
    if (state->hasProperty(BlockStateProperties::DOOR_HINGE)) {
        // Reference: DoorBlock.mirror - rotate by mirror.getRotation(facing)
        // (flips FACING when its axis matches the mirror), then ALWAYS cycle
        // HINGE (mirror != NONE is guaranteed by the early return above).
        using DH = props::DoorHingeSide;
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        state = state->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                                mirrorDirection(facing, mirror));
        DH hinge = state->getValue(*BlockStateProperties::DOOR_HINGE);
        return state->setValue(*BlockStateProperties::DOOR_HINGE,
                               DH(hinge.getValue() == DH::LEFT ? DH::RIGHT : DH::LEFT));
    }
    if (state->hasProperty(BlockStateProperties::HORIZONTAL_FACING)) {
        Direction facing = state->getValue(*BlockStateProperties::HORIZONTAL_FACING);
        return state->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                               mirrorDirection(facing, mirror));
    }
    if (state->hasProperty(BlockStateProperties::FACING)) {
        Direction facing = state->getValue(*BlockStateProperties::FACING);
        return state->setValue(*BlockStateProperties::FACING,
                               mirrorDirection(facing, mirror));
    }
    if (state->hasProperty(BlockStateProperties::NORTH)) {
        return permuteSidesMirror<props::BooleanProperty, bool>(
            state, mirror, *BlockStateProperties::NORTH, *BlockStateProperties::EAST,
            *BlockStateProperties::SOUTH, *BlockStateProperties::WEST);
    }
    if (state->hasProperty(world::level::block::FenceBlock::NORTH)) {
        using FB = world::level::block::FenceBlock;
        return permuteSidesMirror<props::BooleanProperty, bool>(
            state, mirror, *FB::NORTH, *FB::EAST, *FB::SOUTH, *FB::WEST);
    }
    if (state->hasProperty(world::level::block::VineBlock::NORTH)) {
        using VB = world::level::block::VineBlock;
        return permuteSidesMirror<props::BooleanProperty, bool>(
            state, mirror, *VB::NORTH, *VB::EAST, *VB::SOUTH, *VB::WEST);
    }
    if (state->hasProperty(BlockStateProperties::NORTH_REDSTONE)) {
        return permuteSidesMirror<props::EnumProperty<props::RedstoneSide>, props::RedstoneSide>(
            state, mirror, *BlockStateProperties::NORTH_REDSTONE,
            *BlockStateProperties::EAST_REDSTONE, *BlockStateProperties::SOUTH_REDSTONE,
            *BlockStateProperties::WEST_REDSTONE);
    }
    // Reference: WallBlock.mirror - the four WallSide properties permute.
    if (state->hasProperty(BlockStateProperties::NORTH_WALL)) {
        return permuteSidesMirror<props::EnumProperty<props::WallSide>, props::WallSide>(
            state, mirror, *BlockStateProperties::NORTH_WALL,
            *BlockStateProperties::EAST_WALL, *BlockStateProperties::SOUTH_WALL,
            *BlockStateProperties::WEST_WALL);
    }
    return state;
}

BlockState* reorientChest(WorldGenLevel* level, const core::BlockPos& pos,
                          BlockState* state) {
    // Reference: StructurePiece.reorient().
    static const Direction kHorizontal[4] = {
        Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST};
    bool haveSolid = false;
    Direction solidNeighbor = Direction::NORTH;
    for (Direction direction : kHorizontal) {
        core::BlockPos relativePos = relative(pos, direction);
        BlockState* relState = level->getBlockState(relativePos);
        if (relState->is(Blocks::CHEST)) return state;
        if (relState->isSolidRender()) {
            if (haveSolid) {
                haveSolid = false;
                break;
            }
            haveSolid = true;
            solidNeighbor = direction;
        }
    }
    if (haveSolid) {
        return state->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                               core::getOpposite(solidNeighbor));
    }
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

} // namespace state_transforms

OrientedPieceBehavior::OrientedPieceBehavior(int orientation)
    : m_orientation(orientation) {
    // Reference: StructurePiece.setOrientation() mirror/rotation mapping.
    if (orientation < 0) {
        m_mirror = MIRROR_NONE;
        m_rotation = ROT_NONE;
    } else {
        switch (static_cast<Direction>(orientation)) {
            case Direction::SOUTH:
                m_mirror = MIRROR_LEFT_RIGHT;
                m_rotation = ROT_NONE;
                break;
            case Direction::WEST:
                m_mirror = MIRROR_LEFT_RIGHT;
                m_rotation = ROT_CW90;
                break;
            case Direction::EAST:
                m_mirror = MIRROR_NONE;
                m_rotation = ROT_CW90;
                break;
            default:  // NORTH
                m_mirror = MIRROR_NONE;
                m_rotation = ROT_NONE;
                break;
        }
    }
}

int OrientedPieceBehavior::worldX(int x, int z) const {
    if (m_orientation < 0) return x;
    const BoundingBox& box = m_self->boundingBox;
    switch (static_cast<Direction>(m_orientation)) {
        case Direction::NORTH:
        case Direction::SOUTH:
            return box.minX + x;
        case Direction::WEST:
            return box.maxX - z;
        case Direction::EAST:
            return box.minX + z;
        default:
            return x;
    }
}

int OrientedPieceBehavior::worldY(int y) const {
    return m_orientation < 0 ? y : m_self->boundingBox.minY + y;
}

int OrientedPieceBehavior::worldZ(int x, int z) const {
    if (m_orientation < 0) return z;
    const BoundingBox& box = m_self->boundingBox;
    switch (static_cast<Direction>(m_orientation)) {
        case Direction::NORTH:
            return box.maxZ - z;
        case Direction::SOUTH:
            return box.minZ + z;
        case Direction::WEST:
        case Direction::EAST:
            return box.minZ + x;
        default:
            return z;
    }
}

BlockState* OrientedPieceBehavior::mirrorRotate(BlockState* state) const {
    if (m_mirror != MIRROR_NONE) {
        state = state_transforms::mirrorState(state, m_mirror);
    }
    if (m_rotation != ROT_NONE) {
        state = state_transforms::rotateState(state, m_rotation);
    }
    return state;
}

void OrientedPieceBehavior::placeBlock(WorldGenLevel* level, BlockState* state,
                                       int x, int y, int z,
                                       const BoundingBox& chunkBB) const {
    core::BlockPos pos = worldPos(x, y, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    if (!canBeReplaced(level, x, y, z, chunkBB)) return;
    state = mirrorRotate(state);
    level->setBlock(pos, state, 2);
    // Java also schedules a fluid tick and marks SHAPE_CHECK_BLOCKS for
    // post-processing; both act after our dumped phases.
}

bool OrientedPieceBehavior::isInterior(WorldGenLevel* level, int x, int y, int z,
                                       const BoundingBox& chunkBB) const {
    core::BlockPos pos = worldPos(x, y + 1, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return false;
    return pos.getY() < level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG,
                                         pos.getX(), pos.getZ());
}

void OrientedPieceBehavior::maybeGenerateBlock(WorldGenLevel* level,
                                               const BoundingBox& chunkBB,
                                               WorldgenRandom& random, float probability,
                                               int x, int y, int z,
                                               BlockState* state) const {
    if (random.nextFloat() < probability) {
        placeBlock(level, state, x, y, z, chunkBB);
    }
}

void OrientedPieceBehavior::generateMaybeBox(WorldGenLevel* level,
                                             const BoundingBox& chunkBB,
                                             WorldgenRandom& random, float probability,
                                             int x0, int y0, int z0,
                                             int x1, int y1, int z1,
                                             BlockState* edgeBlock, BlockState* fillBlock,
                                             bool skipAir, bool hasToBeInside) const {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                // Java: !(random.nextFloat() > probability) && ... - the draw
                // always happens, then short-circuits.
                if (random.nextFloat() > probability) continue;
                if (skipAir && getBlock(level, x, y, z, chunkBB)->isAir()) continue;
                if (hasToBeInside && !isInterior(level, x, y, z, chunkBB)) continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1) {
                    placeBlock(level, fillBlock, x, y, z, chunkBB);
                } else {
                    placeBlock(level, edgeBlock, x, y, z, chunkBB);
                }
            }
        }
    }
}

void OrientedPieceBehavior::generateUpperHalfSphere(WorldGenLevel* level,
                                                    const BoundingBox& chunkBB,
                                                    int x0, int y0, int z0,
                                                    int x1, int y1, int z1,
                                                    BlockState* fillBlock,
                                                    bool skipAir) const {
    float diagX = static_cast<float>(x1 - x0 + 1);
    float diagY = static_cast<float>(y1 - y0 + 1);
    float diagZ = static_cast<float>(z1 - z0 + 1);
    float cx = static_cast<float>(x0) + diagX / 2.0f;
    float cz = static_cast<float>(z0) + diagZ / 2.0f;
    for (int y = y0; y <= y1; ++y) {
        float ny = static_cast<float>(y - y0) / diagY;
        for (int x = x0; x <= x1; ++x) {
            float nx = (static_cast<float>(x) - cx) / (diagX * 0.5f);
            for (int z = z0; z <= z1; ++z) {
                float nz = (static_cast<float>(z) - cz) / (diagZ * 0.5f);
                if (skipAir && getBlock(level, x, y, z, chunkBB)->isAir()) continue;
                float dist = nx * nx + ny * ny + nz * nz;
                if (dist <= 1.05f) {
                    placeBlock(level, fillBlock, x, y, z, chunkBB);
                }
            }
        }
    }
}

BlockState* OrientedPieceBehavior::getBlock(WorldGenLevel* level, int x, int y,
                                            int z, const BoundingBox& chunkBB) const {
    core::BlockPos pos = worldPos(x, y, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
        return Blocks::AIR->defaultBlockState();
    }
    return level->getBlockState(pos);
}

void OrientedPieceBehavior::generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                                        int x0, int y0, int z0, int x1, int y1, int z1,
                                        BlockState* edgeBlock, BlockState* fillBlock,
                                        bool skipAir) const {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && getBlock(level, x, y, z, chunkBB)->isAir()) continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1) {
                    placeBlock(level, fillBlock, x, y, z, chunkBB);
                } else {
                    placeBlock(level, edgeBlock, x, y, z, chunkBB);
                }
            }
        }
    }
}

void OrientedPieceBehavior::generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                                        int x0, int y0, int z0, int x1, int y1, int z1,
                                        bool skipAir, WorldgenRandom& random,
                                        const std::function<BlockState*(WorldgenRandom&, int, int, int, bool)>& selector) const {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && getBlock(level, x, y, z, chunkBB)->isAir()) continue;
                bool isEdge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
                placeBlock(level, selector(random, x, y, z, isEdge), x, y, z, chunkBB);
            }
        }
    }
}

bool OrientedPieceBehavior::createDispenser(WorldGenLevel* level, const BoundingBox& chunkBB,
                                            WorldgenRandom& random, int x, int y, int z,
                                            Direction facing, const char* lootTable) const {
    core::BlockPos pos = worldPos(x, y, z);
    Block* dispenser = world::level::block::Blocks::getBlock("minecraft:dispenser");
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
        || level->getBlockState(pos)->is(dispenser)) {
        return false;
    }
    BlockState* state = world::level::block::Blocks::getDefaultState("minecraft:dispenser")
        ->setValue(*BlockStateProperties::FACING, facing);
    placeBlock(level, state, x, y, z, chunkBB);
    // Java: DispenserBlockEntity.setLootTable(lootTable, random.nextLong()).
    int64_t seed = random.nextLong();
    if (lootTable != nullptr) {
        if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
            chunk->setBlockEntityNbt(pos,
                std::string("{LootTable:\"") + lootTable + "\",LootTableSeed:"
                + std::to_string(seed)
                + "l,components:{},id:\"minecraft:dispenser\"}");
        }
    }
    return true;
}

void OrientedPieceBehavior::generateAirBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                                           int x0, int y0, int z0,
                                           int x1, int y1, int z1) const {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                placeBlock(level, Blocks::AIR->defaultBlockState(), x, y, z, chunkBB);
            }
        }
    }
}

void OrientedPieceBehavior::fillColumnDown(WorldGenLevel* level, BlockState* state,
                                           int x, int startY, int z,
                                           const BoundingBox& chunkBB) const {
    core::BlockPos pos = worldPos(x, startY, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    while (isReplaceableByStructures(level->getBlockState(pos))
           && pos.getY() > level->getMinY() + 1) {
        level->setBlock(pos, state, 2);
        pos = pos.below();
    }
}

bool OrientedPieceBehavior::isReplaceableByStructures(const BlockState* state) {
    return state->isAir() || state->isFluid()
        || state->is(Blocks::GLOW_LICHEN) || state->is(Blocks::SEAGRASS)
        || state->is(Blocks::TALL_SEAGRASS);
}

bool OrientedPieceBehavior::createChest(WorldGenLevel* level, const BoundingBox& chunkBB,
                                        WorldgenRandom& random, const core::BlockPos& pos,
                                        BlockState* state, const char* lootTable) const {
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
        || level->getBlockState(pos)->is(Blocks::CHEST)) {
        return false;
    }
    if (state == nullptr) {
        state = state_transforms::reorientChest(level, pos, Blocks::CHEST->defaultBlockState());
    }
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

bool OrientedPieceBehavior::updateHeightPositionToLowestGroundHeight(WorldGenLevel* level,
                                                                     int offset) {
    // Reference: ScatteredFeaturePiece.updateHeightPositionToLowestGroundHeight.
    if (m_heightPosition >= 0) return true;
    int lowest = level->getMaxY() + 1;
    bool found = false;
    const BoundingBox& box = m_self->boundingBox;
    for (int z = box.minZ; z <= box.maxZ; ++z) {
        for (int x = box.minX; x <= box.maxX; ++x) {
            lowest = std::min(lowest,
                level->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z));
            found = true;
        }
    }
    if (!found) return false;
    m_heightPosition = lowest;
    m_self->boundingBox.move(0, m_heightPosition - box.minY + offset, 0);
    return true;
}

bool OrientedPieceBehavior::updateAverageGroundHeight(WorldGenLevel* level,
                                                      const BoundingBox& chunkBB,
                                                      int offset) {
    // Reference: ScatteredFeaturePiece.updateAverageGroundHeight.
    if (m_heightPosition >= 0) return true;
    int total = 0;
    int count = 0;
    const BoundingBox& box = m_self->boundingBox;
    for (int z = box.minZ; z <= box.maxZ; ++z) {
        for (int x = box.minX; x <= box.maxX; ++x) {
            if (chunkBB.isInside(x, 64, z)) {
                total += level->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
                ++count;
            }
        }
    }
    if (count == 0) return false;
    m_heightPosition = total / count;
    m_self->boundingBox.move(0, m_heightPosition - box.minY + offset, 0);
    return true;
}

} // namespace structure
} // namespace levelgen
} // namespace minecraft
