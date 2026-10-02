// File: src/common/world/block/Vine.cpp
#include "Vine.hpp"
#include "BlockPlacement.hpp"
#include "../chunk/IBlockAccess.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

namespace Game {

    namespace {

        // MC's PROPERTY_BY_DIRECTION for VineBlock. DOWN has no entry at all,
        // which is why every lookup below returns PropertyId::Count for it
        // rather than silently answering about some other face.
        PropertyId FaceProperty(Direction face) {
            switch (face) {
                case Direction::Up:    return PropertyId::UP;
                case Direction::North: return PropertyId::NORTH;
                case Direction::East:  return PropertyId::EAST;
                case Direction::South: return PropertyId::SOUTH;
                case Direction::West:  return PropertyId::WEST;
                default:               return PropertyId::Count;   // Down
            }
        }

        constexpr Direction kFaces[5] = {
            Direction::Up, Direction::North, Direction::East,
            Direction::South, Direction::West,
        };
        constexpr Direction kHorizontal[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };

        glm::ivec3 Step(const glm::ivec3& p, Direction d) {
            return { p.x + StepX(d), p.y + StepY(d), p.z + StepZ(d) };
        }

        // MC VineBlock.isAcceptableNeighbour -> MultifaceBlock.canAttachTo:
        // the neighbour's face TOWARD US must be full.
        bool AcceptableNeighbour(const IBlockAccess& level, const glm::ivec3& pos,
                                 Direction toNeighbour) {
            const glm::ivec3 n = Step(pos, toNeighbour);
            return IsFaceSturdyAt(level, n, Opposite(toNeighbour));
        }

    } // namespace

    bool IsVineBlock(BlockID id) {
        // Exact slug, not a substring: "vine" is a substring of weeping_vines,
        // twisting_vines and cave_vines, which are different blocks entirely.
        return id == BlockID::Vine;
    }

    bool VineFaceOf(BlockState state, Direction face) {
        const PropertyId p = FaceProperty(face);
        if (p == PropertyId::Count) return false;
        return state.GetIndex(p) == 0;                 // booleans list [true, false]
    }

    BlockState VineStateWithFace(BlockState state, Direction face, bool on) {
        const PropertyId p = FaceProperty(face);
        if (p == PropertyId::Count) return state;
        return state.SetIndex(p, on ? 0 : 1);
    }

    int VineCountFaces(BlockState state) {
        int n = 0;
        for (Direction d : kFaces) if (VineFaceOf(state, d)) ++n;
        return n;
    }

    bool VineCanSupportAtFace(const IBlockAccess& level, const glm::ivec3& pos,
                              BlockState state, Direction face) {
        if (face == Direction::Down) return false;
        if (AcceptableNeighbour(level, pos, face)) return true;
        if (AxisOf(face) == Axis::Y) return false;

        // The horizontal fallback: a vine directly above clinging to the SAME
        // face holds this one up. That is what makes a vine curtain hang down
        // a wall past the block it started on.
        const glm::ivec3 above{ pos.x, pos.y + 1, pos.z };
        const BlockState a = level.GetBlockState(above.x, above.y, above.z);
        return IsVineBlock(a.Block()) && VineFaceOf(a, face);
    }

    BlockState VineUpdatedState(const IBlockAccess& level, const glm::ivec3& pos,
                                BlockState state) {
        // UP is resolved against the block above only — no vine fallback, since
        // a vine above would occupy the very cell UP is asking about.
        if (VineFaceOf(state, Direction::Up)) {
            state = VineStateWithFace(state, Direction::Up,
                                      AcceptableNeighbour(level, pos, Direction::Up));
        }
        for (Direction d : kHorizontal) {
            if (!VineFaceOf(state, d)) continue;       // MC only re-tests faces it HAS
            state = VineStateWithFace(state, d, VineCanSupportAtFace(level, pos, state, d));
        }
        return state;
    }

    bool VineCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        return VineCountFaces(VineUpdatedState(level, pos, state)) > 0;
    }

    BlockState VineUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                               BlockState state, Direction toNeighbour) {
        if (toNeighbour == Direction::Down) return state;
        const BlockState updated = VineUpdatedState(level, pos, state);
        // Air's state means "I cannot exist any more" — the same signal the
        // redstone wire and the face-attached family use.
        return VineCountFaces(updated) > 0 ? updated : BlockState{};
    }

    BlockState VinePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState state, Direction clickedFace) {
        // MC walks `context.getNearestLookingDirections()`, whose first entry is
        // the face the click implies. Clicking the north side of a block puts the
        // vine in the cell to its north, and the support is then to the SOUTH of
        // the vine — hence the opposite.
        const Direction preferred = Opposite(clickedFace);

        Direction order[5];
        int n = 0;
        if (preferred != Direction::Down) order[n++] = preferred;
        for (Direction d : kFaces) {
            if (d == preferred) continue;
            order[n++] = d;
        }

        for (int i = 0; i < n; ++i) {
            const Direction d = order[i];
            if (VineFaceOf(state, d)) continue;        // MC skips an occupied face
            if (VineCanSupportAtFace(level, pos, state, d)) {
                return VineStateWithFace(state, d, true);
            }
        }
        return state;                                  // nothing holds it; caller refuses
    }

    namespace {

        bool HasHorizontalConnection(BlockState state) {
            for (Direction d : kHorizontal) if (VineFaceOf(state, d)) return true;
            return false;
        }

        bool IsEmptyCell(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlock(p.x, p.y, p.z) == BlockID::Air;
        }

        // MC VineBlock.canSpread: at most four vines in the box from
        // pos - (4, 1, 4) to pos + (4, 1, 4) (BlockMatcher.atMostMatched(4)).
        bool CanSpread(const IBlockAccess& level, const glm::ivec3& pos) {
            int found = 0;
            for (int x = pos.x - 4; x <= pos.x + 4; ++x) {
                for (int y = pos.y - 1; y <= pos.y + 1; ++y) {
                    for (int z = pos.z - 4; z <= pos.z + 4; ++z) {
                        if (IsVineBlock(level.GetBlock(x, y, z)) && ++found > 4) return false;
                    }
                }
            }
            return true;
        }

        // MC VineBlock.copyRandomFaces: each horizontal face `from` has is
        // copied onto `to` on a coin flip (the flip is drawn for every face).
        BlockState CopyRandomFaces(BlockState from, BlockState to, JavaRandom& random) {
            for (Direction d : kHorizontal) {
                if (random.NextBool() && VineFaceOf(from, d)) {
                    to = VineStateWithFace(to, d, true);
                }
            }
            return to;
        }

        // MC Direction.getRandom: VALUES[nextInt(6)] — DOWN, UP, NORTH,
        // SOUTH, WEST, EAST, the engine's enum order.
        Direction RandomDirection(JavaRandom& random) {
            return static_cast<Direction>(random.NextInt(6));
        }

    } // namespace

    void VineRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                        JavaRandom& random) {
        if (!Rules::GetBool(Rules::Id::SpreadVines)) return;
        if (random.NextInt(4) != 0) return;

        constexpr uint32_t kFlags = World::UpdateFlags::UpdateClients;   // MC flag 2
        const BlockState vine = BlockStates::Default(BlockID::Vine);
        const Direction testDirection = RandomDirection(random);
        const glm::ivec3 abovePos{ pos.x, pos.y + 1, pos.z };

        if (AxisOf(testDirection) != Axis::Y && !VineFaceOf(state, testDirection)) {
            if (!CanSpread(level, pos)) return;
            const glm::ivec3 testPos = Step(pos, testDirection);
            if (IsEmptyCell(level, testPos)) {
                // Into the open cell beside: wrap round a corner, or hang
                // from the block above it.
                const Direction cw  = ClockWise(testDirection);
                const Direction ccw = CounterClockWise(testDirection);
                const bool cwHasConnectingFace  = VineFaceOf(state, cw);
                const bool ccwHasConnectingFace = VineFaceOf(state, ccw);
                const glm::ivec3 cwTestPos  = Step(testPos, cw);
                const glm::ivec3 ccwTestPos = Step(testPos, ccw);
                if (cwHasConnectingFace && AcceptableNeighbour(level, testPos, cw)) {
                    level.SetBlock(testPos.x, testPos.y, testPos.z,
                                   VineStateWithFace(vine, cw, true), kFlags);
                } else if (ccwHasConnectingFace && AcceptableNeighbour(level, testPos, ccw)) {
                    level.SetBlock(testPos.x, testPos.y, testPos.z,
                                   VineStateWithFace(vine, ccw, true), kFlags);
                } else {
                    const Direction opposite = Opposite(testDirection);
                    // isAcceptableNeighbour(level, pos.relative(cw), opposite)
                    // — the block beside us, seen from the corner cell.
                    if (cwHasConnectingFace && IsEmptyCell(level, cwTestPos) &&
                        AcceptableNeighbour(level, cwTestPos, opposite)) {
                        level.SetBlock(cwTestPos.x, cwTestPos.y, cwTestPos.z,
                                       VineStateWithFace(vine, opposite, true), kFlags);
                    } else if (ccwHasConnectingFace && IsEmptyCell(level, ccwTestPos) &&
                               AcceptableNeighbour(level, ccwTestPos, opposite)) {
                        level.SetBlock(ccwTestPos.x, ccwTestPos.y, ccwTestPos.z,
                                       VineStateWithFace(vine, opposite, true), kFlags);
                    } else if (static_cast<double>(random.NextFloat()) < 0.05 &&
                               AcceptableNeighbour(level, testPos, Direction::Up)) {
                        level.SetBlock(testPos.x, testPos.y, testPos.z,
                                       VineStateWithFace(vine, Direction::Up, true), kFlags);
                    }
                }
            } else if (AcceptableNeighbour(level, pos, testDirection)) {
                // A solid face beside: cling to it too.
                level.SetBlock(pos.x, pos.y, pos.z,
                               VineStateWithFace(state, testDirection, true), kFlags);
            }
            return;
        }

        // pos.getY() < level.getMaxY(): there is a cell above inside the world.
        if (testDirection == Direction::Up && level.IsValidPosition(abovePos.x, abovePos.y, abovePos.z)) {
            if (VineCanSupportAtFace(level, pos, state, Direction::Up)) {
                level.SetBlock(pos.x, pos.y, pos.z,
                               VineStateWithFace(state, Direction::Up, true), kFlags);
                return;
            }
            if (IsEmptyCell(level, abovePos)) {
                if (!CanSpread(level, pos)) return;
                // Grow upward: a copy of this vine, each horizontal face kept
                // only on a losing coin flip and where it has support above.
                BlockState aboveState = state;
                for (Direction d : kHorizontal) {
                    if (random.NextBool() || !AcceptableNeighbour(level, abovePos, d)) {
                        aboveState = VineStateWithFace(aboveState, d, false);
                    }
                }
                if (HasHorizontalConnection(aboveState)) {
                    level.SetBlock(abovePos.x, abovePos.y, abovePos.z, aboveState, kFlags);
                }
                return;
            }
        }

        // pos.getY() > level.getMinY(): grow down into air, or add faces to
        // the vine below.
        const glm::ivec3 belowPos{ pos.x, pos.y - 1, pos.z };
        if (!level.IsValidPosition(belowPos.x, belowPos.y, belowPos.z)) return;
        const BlockState belowState = level.GetBlockState(belowPos.x, belowPos.y, belowPos.z);
        const bool belowAir = belowState.Block() == BlockID::Air;
        if (belowAir || IsVineBlock(belowState.Block())) {
            const BlockState before = belowAir ? vine : belowState;
            const BlockState after  = CopyRandomFaces(state, before, random);
            if (before != after && HasHorizontalConnection(after)) {
                level.SetBlock(belowPos.x, belowPos.y, belowPos.z, after, kFlags);
            }
        }
    }

    BlockRegistry::BlockShapeSet VineShapeBoxes(BlockState state) {
        // Block.boxZ(16, 0, 1) is the north slab; rotateAll gives the other five.
        // Pixels/16, matching every other shape in this engine.
        constexpr float k = 1.0f / 16.0f;
        BlockRegistry::BlockShapeSet out;

        auto push = [&](glm::vec3 mn, glm::vec3 mx) {
            if (out.count < BlockRegistry::kMaxShapeBoxes) {
                out.boxes[out.count++] = BlockRegistry::BlockShape{ mn, mx };
            }
        };

        if (VineFaceOf(state, Direction::North)) push({0, 0, 0},        {1, 1, k});
        if (VineFaceOf(state, Direction::South)) push({0, 0, 1 - k},    {1, 1, 1});
        if (VineFaceOf(state, Direction::West))  push({0, 0, 0},        {k, 1, 1});
        if (VineFaceOf(state, Direction::East))  push({1 - k, 0, 0},    {1, 1, 1});
        if (VineFaceOf(state, Direction::Up))    push({0, 1 - k, 0},    {1, 1, 1});

        // `shape.isEmpty() ? Shapes.block()` — a face-less vine is a full cube,
        // which is also the state the blockstate JSON draws on all five sides.
        if (out.count == 0) push({0, 0, 0}, {1, 1, 1});
        return out;
    }

} // namespace Game
