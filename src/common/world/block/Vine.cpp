// File: src/common/world/block/Vine.cpp
#include "Vine.hpp"
#include "BlockPlacement.hpp"
#include "../chunk/IBlockAccess.hpp"

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
