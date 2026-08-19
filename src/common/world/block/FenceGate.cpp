// File: src/common/world/block/FenceGate.cpp
#include "FenceGate.hpp"
#include "Walls.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        bool EndsWith(const std::string& s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // MC Block.box in [0,1] block space, from pixel coordinates.
        BlockRegistry::BlockShape Box(float x0, float y0, float z0,
                                      float x1, float y1, float z1) {
            constexpr float k = 1.0f / 16.0f;
            return BlockRegistry::BlockShape{ { x0 * k, y0 * k, z0 * k },
                                              { x1 * k, y1 * k, z1 * k } };
        }

    } // namespace

    bool IsFenceGateBlock(BlockID id) {
        return EndsWith(BlockRegistry::Get(id).modelName, "_fence_gate");
    }

    Direction FenceGateFacing(BlockState state) {
        return HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
    }

    // Booleans list [true, false], so index 0 IS true.
    bool FenceGateOpen(BlockState state) {
        return state.GetIndex(PropertyId::OPEN) == 0;
    }

    bool FenceGateInWall(BlockState state) {
        return state.GetIndex(PropertyId::IN_WALL) == 0;
    }

    bool FenceGatePowered(BlockState state) {
        return state.GetIndex(PropertyId::POWERED) == 0;
    }

    BlockState FenceGateStateFrom(BlockState state,
                                  Direction facing, bool open, bool powered, bool inWall) {
        return state
            .SetIndex(PropertyId::HORIZONTAL_FACING, HorizontalFacingIndex(facing))
            .SetIndex(PropertyId::OPEN,    open    ? 0 : 1)
            .SetIndex(PropertyId::POWERED, powered ? 0 : 1)
            .SetIndex(PropertyId::IN_WALL, inWall  ? 0 : 1);
    }

    bool FenceGateConnectsToDirection(BlockState state, Direction dir) {
        return AxisOf(FenceGateFacing(state)) == AxisOf(ClockWise(dir));
    }

    BlockState FenceGatePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState fallback) {
        // MC:
        //   inWall = axis == Z && (isWall(west) || isWall(east))
        //         || axis == X && (isWall(north) || isWall(south))
        // i.e. look along the gate's HINGE axis — the direction the gate spans,
        // which is perpendicular to FACING.
        const Direction facing = FenceGateFacing(fallback);
        const Direction side   = ClockWise(facing);
        auto wallAt = [&](Direction d) {
            const glm::ivec3 p{ pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d) };
            return IsWallBlock(level.GetBlock(p.x, p.y, p.z));
        };
        const bool inWall = wallAt(side) || wallAt(Opposite(side));

        // OPEN and POWERED both come from `level.hasNeighborSignal(pos)` in
        // vanilla. Nothing produces a signal here, so both stay false — which
        // is exactly what the no-redstone answer is.
        return FenceGateStateFrom(fallback, facing,
                                  FenceGateOpen(fallback), /*powered=*/false, inWall);
    }

    BlockState FenceGateUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state, Direction toNeighbour) {
        // MC: `if (FACING.getClockWise().getAxis() != direction.getAxis())`
        // fall through — only the hinge axis can change IN_WALL.
        const Direction facing = FenceGateFacing(state);
        if (AxisOf(ClockWise(facing)) != AxisOf(toNeighbour)) return state;

        const glm::ivec3 a{ pos.x + StepX(toNeighbour),
                            pos.y + StepY(toNeighbour),
                            pos.z + StepZ(toNeighbour) };
        const Direction opp = Opposite(toNeighbour);
        const glm::ivec3 b{ pos.x + StepX(opp), pos.y + StepY(opp), pos.z + StepZ(opp) };
        const bool inWall = IsWallBlock(level.GetBlock(a.x, a.y, a.z)) ||
                            IsWallBlock(level.GetBlock(b.x, b.y, b.z));
        return FenceGateStateFrom(state, facing, FenceGateOpen(state),
                                  FenceGatePowered(state), inWall);
    }

    BlockState FenceGateToggle(BlockState state, Direction playerFacing) {
        const bool open    = FenceGateOpen(state);
        Direction  facing  = FenceGateFacing(state);
        const bool powered = FenceGatePowered(state);
        const bool inWall  = FenceGateInWall(state);

        if (open) {
            return FenceGateStateFrom(state, facing, false, powered, inWall);
        }
        // MC: a gate opened from its back side turns to face the player first,
        // so it always swings AWAY from whoever opened it.
        if (facing == Opposite(playerFacing)) facing = playerFacing;
        return FenceGateStateFrom(state, facing, true, powered, inWall);
    }

    BlockRegistry::BlockShapeSet FenceGateShapeBoxes(BlockState state) {
        // SHAPES = rotateHorizontalAxis(cube(16, 16, 4)) — a slab of the cell
        // 4 pixels thick across the gate's facing axis, full height.
        // SHAPES_WALL = the same minus everything above y=13.
        const float top = FenceGateInWall(state) ? 13.0f : 16.0f;
        BlockRegistry::BlockShapeSet out;
        out.count = 1;
        out.boxes[0] = (AxisOf(FenceGateFacing(state)) == Axis::Z)
                           ? Box(0.0f, 0.0f, 6.0f, 16.0f, top, 10.0f)
                           : Box(6.0f, 0.0f, 0.0f, 10.0f, top, 16.0f);
        return out;
    }

    BlockRegistry::BlockShapeSet FenceGateCollisionBoxes(BlockState state) {
        BlockRegistry::BlockShapeSet out;
        // `return state.getValue(OPEN) ? Shapes.empty() : SHAPE_COLLISION`.
        // An empty set IS the empty shape: nothing to collide with, which is
        // the whole point of opening a gate.
        if (FenceGateOpen(state)) return out;

        // SHAPE_COLLISION = rotateHorizontalAxis(column(16, 4, 0, 24)) — the
        // same 4-pixel slab, 24 tall, so a shut gate is as unjumpable as the
        // fence it sits in.
        out.count = 1;
        out.boxes[0] = (AxisOf(FenceGateFacing(state)) == Axis::Z)
                           ? Box(0.0f, 0.0f, 6.0f, 16.0f, 24.0f, 10.0f)
                           : Box(6.0f, 0.0f, 0.0f, 10.0f, 24.0f, 16.0f);
        return out;
    }

} // namespace Game
