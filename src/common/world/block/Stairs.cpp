// File: src/common/world/block/Stairs.cpp
#include "Stairs.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        constexpr std::string_view kShapeNames[5] = {
            "straight", "inner_left", "inner_right", "outer_left", "outer_right",
        };
        constexpr std::string_view kHalfNames[2] = { "bottom", "top" };

        // ── Base geometry ───────────────────────────────────────────────────
        //
        // Taken from the model JSONs (assets/models/block/{stairs,inner_stairs,
        // outer_stairs}.json) rather than transcribed from MC's Shapes.or
        // chain, because the blockstate file pins the two together: every
        // stairs blockstate maps `facing=east,half=bottom,shape=straight` to
        // the UNROTATED model, so the model as authored IS the east-facing
        // bottom stair and the shape has to agree with it box for box.
        //
        // MC pixel space /16. The step boxes sit on top of the slab, so a
        // straight stair is slab + one half, an outer corner slab + one
        // quarter, an inner corner slab + a half + a quarter.
        const BlockRegistry::BlockShape kSlab{ {0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f} };
        // (8,8,0)-(16,16,16): the east half, full depth.
        const BlockRegistry::BlockShape kStepEast{ {0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f} };
        // (0,8,8)-(8,16,16): the south-west quarter, which is what turns the
        // east half into an inner corner.
        const BlockRegistry::BlockShape kStepSouthWest{ {0.0f, 0.5f, 0.5f}, {0.5f, 1.0f, 1.0f} };
        // (8,8,8)-(16,16,16): the south-east quarter — the outer corner.
        const BlockRegistry::BlockShape kStepSouthEast{ {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f} };

        // Quarter turns clockwise from the authored east-facing orientation.
        // MC's Direction has no such index (its 2D order starts at south), and
        // this one has to start at east for the base model to be turn 0.
        int TurnsFromEast(Direction facing) {
            switch (facing) {
                case Direction::East:  return 0;
                case Direction::South: return 1;
                case Direction::West:  return 2;
                default:               return 3;   // north
            }
        }

        // One 90° turn about Y, clockwise seen from above: east -> south, i.e.
        // +X -> +Z. Same sense as the blockstate JSON's `"y"` rotation, which
        // is what the model baker applies, so shape and geometry stay locked.
        BlockRegistry::BlockShape RotateY90(const BlockRegistry::BlockShape& b) {
            // (x, z) -> (1 - z, x), applied to both corners; a quarter turn
            // keeps the box axis-aligned but can swap which corner is minimum.
            const float x0 = 1.0f - b.max.z, x1 = 1.0f - b.min.z;
            const float z0 = b.min.x,        z1 = b.max.x;
            return BlockRegistry::BlockShape{
                { std::min(x0, x1), b.min.y, std::min(z0, z1) },
                { std::max(x0, x1), b.max.y, std::max(z0, z1) },
            };
        }

        // MC OctahedralGroup.INVERT_Y — how SHAPE_TOP_* is derived from
        // SHAPE_* in StairBlock's static block. A pure Y flip, so an upside
        // down stair is its right-way-up self mirrored through y = 0.5.
        BlockRegistry::BlockShape InvertY(const BlockRegistry::BlockShape& b) {
            return BlockRegistry::BlockShape{
                { b.min.x, 1.0f - b.max.y, b.min.z },
                { b.max.x, 1.0f - b.min.y, b.max.z },
            };
        }

    } // namespace

    bool IsStairs(BlockID id) {
        const std::string& n = BlockRegistry::Get(id).modelName;
        static constexpr std::string_view kSuffix = "_stairs";
        return n.size() >= kSuffix.size() &&
               n.compare(n.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
    }

    // MC's HORIZONTAL_FACING value order is north, south, west, east — NOT
    // compass order, and not the order this engine used to hand-write.
    Direction StairFacing(BlockState state) {
        // -1 (not a stair) falls through to north, as before.
        return HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
    }

    StairHalf StairHalfOf(BlockState state) {
        // HALF is top, bottom — index 0 is TOP.
        return state.GetIndex(PropertyId::HALF) == 0 ? StairHalf::Top : StairHalf::Bottom;
    }

    StairsShape StairShapeOf(BlockState state) {
        const int v = state.GetIndex(PropertyId::STAIRS_SHAPE);
        // kShapeNames is in the property's own value order, so the index IS the
        // enum value; a non-stair answers -1 and falls back to straight.
        return (v >= 0 && v < 5) ? static_cast<StairsShape>(v) : StairsShape::Straight;
    }

    BlockState StairStateFrom(BlockState state,
                              Direction facing, StairHalf half, StairsShape shape) {
        // Three property writes on the state itself. This used to round-trip
        // the whole tuple through a string map to avoid clobbering WATERLOGGED;
        // SetIndex touches one property, so that is now the default behaviour
        // rather than something the code has to arrange.
        return state
            .SetIndex(PropertyId::HORIZONTAL_FACING, HorizontalFacingIndex(facing))
            .SetIndex(PropertyId::HALF, half == StairHalf::Top ? 0 : 1)
            .SetIndex(PropertyId::STAIRS_SHAPE, static_cast<int>(shape));
    }

    BlockRegistry::BlockShapeSet StairShapeBoxes(BlockState state) {
        const Direction   facing = StairFacing(state);
        const StairsShape shape  = StairShapeOf(state);
        const StairHalf   half   = StairHalfOf(state);

        BlockRegistry::BlockShapeSet out;
        out.boxes[out.count++] = kSlab;
        switch (shape) {
            case StairsShape::Straight:
                out.boxes[out.count++] = kStepEast;
                break;
            case StairsShape::InnerLeft:
            case StairsShape::InnerRight:
                out.boxes[out.count++] = kStepEast;
                out.boxes[out.count++] = kStepSouthWest;
                break;
            case StairsShape::OuterLeft:
            case StairsShape::OuterRight:
                out.boxes[out.count++] = kStepSouthEast;
                break;
        }

        // The *_LEFT shapes are the *_RIGHT geometry one quarter turn back —
        // the same relationship the blockstate JSON encodes by giving
        // `shape=inner_left` a y rotation 90° behind `shape=inner_right` for
        // every facing. MC expresses it the other way round, as a different
        // key into the same shape map (`facing.getCounterClockWise()` for
        // INNER_LEFT, `facing.getClockWise()` for OUTER_RIGHT); both land on
        // the same box set.
        int turns = TurnsFromEast(facing);
        if (shape == StairsShape::InnerLeft || shape == StairsShape::OuterLeft) {
            turns = (turns + 3) % 4;
        }
        for (int t = 0; t < turns; ++t) {
            for (uint8_t i = 0; i < out.count; ++i) out.boxes[i] = RotateY90(out.boxes[i]);
        }

        if (half == StairHalf::Top) {
            for (uint8_t i = 0; i < out.count; ++i) out.boxes[i] = InvertY(out.boxes[i]);
        }
        return out;
    }

    namespace {

        // MC StairBlock.canTakeShape. The stair at `neighbour` blocks the
        // corner if it is a stair pointing the SAME way in the SAME half —
        // that is what stops a straight run from folding itself into corners
        // at every joint.
        bool CanTakeShape(const IBlockAccess& level, const glm::ivec3& pos,
                          BlockState state, Direction neighbour) {
            const glm::ivec3 p{ pos.x + StepX(neighbour),
                                pos.y + StepY(neighbour),
                                pos.z + StepZ(neighbour) };
            const BlockState n = level.GetBlockState(p.x, p.y, p.z);
            if (!IsStairs(n.Block())) return true;
            return StairFacing(n) != StairFacing(state) ||
                   StairHalfOf(n) != StairHalfOf(state);
        }

    } // namespace

    StairsShape StairsShapeAt(const IBlockAccess& level, const glm::ivec3& pos,
                              BlockState state) {
        const Direction facing = StairFacing(state);
        const StairHalf half   = StairHalfOf(state);

        auto stairAt = [&](Direction d, BlockState& out) {
            const glm::ivec3 p{ pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d) };
            out = level.GetBlockState(p.x, p.y, p.z);
            return IsStairs(out.Block());
        };

        // The stair AHEAD of this one (the way it faces). A perpendicular one
        // there means this stair is the outside of a turn.
        {
            BlockState n;
            if (stairAt(facing, n) && StairHalfOf(n) == half) {
                const Direction behind = StairFacing(n);
                if (AxisOf(behind) != AxisOf(facing) &&
                    CanTakeShape(level, pos, state, Opposite(behind))) {
                    return behind == CounterClockWise(facing) ? StairsShape::OuterLeft
                                                              : StairsShape::OuterRight;
                }
            }
        }

        // The stair BEHIND it. A perpendicular one there means this stair is
        // the inside of a turn.
        {
            BlockState n;
            if (stairAt(Opposite(facing), n) && StairHalfOf(n) == half) {
                const Direction front = StairFacing(n);
                if (AxisOf(front) != AxisOf(facing) &&
                    CanTakeShape(level, pos, state, front)) {
                    return front == CounterClockWise(facing) ? StairsShape::InnerLeft
                                                             : StairsShape::InnerRight;
                }
            }
        }

        return StairsShape::Straight;
    }

    BlockState StairsPlacementState(BlockID id, Direction horizontalFacing,
                                    Direction clickedFace, float hitY) {
        // StairBlock.java:113-117, verbatim:
        //   HALF = clickedFace != DOWN && (clickedFace == UP ||
        //          !(clickLocation.y - pos.getY() > 0.5)) ? BOTTOM : TOP
        // i.e. clicking a ceiling always gives TOP, clicking a floor always
        // gives BOTTOM, and clicking a side splits on the cell's midpoint —
        // the same rule the slab family already follows.
        const StairHalf half =
            (clickedFace != Direction::Down &&
             (clickedFace == Direction::Up || !(hitY > 0.5f)))
                ? StairHalf::Bottom : StairHalf::Top;
        // From the block's DEFAULT state, not its state 0. WATERLOGGED is the
        // one property this does not set, and state 0 has it TRUE (MC lists
        // `true` first). ComputeWorldPlacementState happens to overwrite it a
        // moment later, so starting from 0 was survivable — but only by
        // accident, and only as long as that pass keeps running afterwards.
        return StairStateFrom(BlockStates::Default(id),
                              horizontalFacing, half, StairsShape::Straight);
    }

    BlockState StairsWorldPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state) {
        return StairStateFrom(state,
                              StairFacing(state),
                              StairHalfOf(state),
                              StairsShapeAt(level, pos, state));
    }

    BlockState StairsUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                 BlockState state, Direction toNeighbour) {
        // MC schedules a water tick here when WATERLOGGED. There are no fluid
        // ticks in this engine — waterlogging is a static flag on the state —
        // so that half of updateShape has nothing to do.
        if (!IsHorizontal(toNeighbour)) return state;
        return StairsWorldPlacementState(level, pos, state);
    }

} // namespace Game
