// File: src/common/world/block/MultifaceBlock.cpp
#include "MultifaceBlock.hpp"
#include "BlockPlacement.hpp"
#include "../chunk/IBlockAccess.hpp"

namespace Game {

    namespace {

        // MC's PROPERTY_BY_DIRECTION. Unlike the vine's, this one HAS a DOWN.
        PropertyId FaceProperty(Direction face) {
            switch (face) {
                case Direction::Down:  return PropertyId::DOWN;
                case Direction::Up:    return PropertyId::UP;
                case Direction::North: return PropertyId::NORTH;
                case Direction::East:  return PropertyId::EAST;
                case Direction::South: return PropertyId::SOUTH;
                case Direction::West:  return PropertyId::WEST;
            }
            return PropertyId::Count;
        }

        constexpr Direction kAll[6] = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::East, Direction::South, Direction::West,
        };

        glm::ivec3 Step(const glm::ivec3& p, Direction d) {
            return { p.x + StepX(d), p.y + StepY(d), p.z + StepZ(d) };
        }

    } // namespace

    bool IsMultifaceBlock(BlockID id) {
        return id == BlockID::GlowLichen
            || id == BlockID::SculkVein
            || id == BlockID::ResinClump;
    }

    bool MultifaceFaceOf(BlockState state, Direction face) {
        const PropertyId p = FaceProperty(face);
        if (p == PropertyId::Count) return false;
        return state.GetIndex(p) == 0;                  // booleans list [true, false]
    }

    BlockState MultifaceStateWithFace(BlockState state, Direction face, bool on) {
        const PropertyId p = FaceProperty(face);
        if (p == PropertyId::Count) return state;
        return state.SetIndex(p, on ? 0 : 1);
    }

    int MultifaceCountFaces(BlockState state) {
        int n = 0;
        for (Direction d : kAll) if (MultifaceFaceOf(state, d)) ++n;
        return n;
    }

    bool MultifaceHasAnyVacantFace(BlockState state) {
        for (Direction d : kAll) if (!MultifaceFaceOf(state, d)) return true;
        return false;
    }

    bool MultifaceCanAttachTo(const IBlockAccess& level, const glm::ivec3& pos,
                              Direction toNeighbour) {
        const glm::ivec3 n = Step(pos, toNeighbour);
        return IsFaceSturdyAt(level, n, Opposite(toNeighbour));
    }

    bool MultifaceCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                             BlockState state) {
        // MC returns false the moment ANY worn face has lost its surface — the
        // whole clump goes, not just that face. That is the opposite of the
        // vine's "any surviving face keeps me alive".
        bool any = false;
        for (Direction d : kAll) {
            if (!MultifaceFaceOf(state, d)) continue;
            if (!MultifaceCanAttachTo(level, pos, d)) return false;
            any = true;
        }
        return any;
    }

    BlockState MultifaceUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state, Direction toNeighbour) {
        if (MultifaceCountFaces(state) == 0) return BlockState{};      // air
        if (!MultifaceFaceOf(state, toNeighbour)) return state;        // face we don't wear
        if (MultifaceCanAttachTo(level, pos, toNeighbour)) return state;

        // MC removeFace: clear it, and if nothing is left the block is air.
        const BlockState without = MultifaceStateWithFace(state, toNeighbour, false);
        return MultifaceCountFaces(without) > 0 ? without : BlockState{};
    }

    BlockState MultifacePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state, Direction clickedFace) {
        // Clicking a block's north side puts the clump in the cell north of it,
        // so the surface is to its SOUTH — the opposite of the clicked face.
        const Direction preferred = Opposite(clickedFace);

        Direction order[6];
        int n = 0;
        order[n++] = preferred;
        for (Direction d : kAll) {
            if (d == preferred) continue;
            order[n++] = d;
        }

        for (int i = 0; i < n; ++i) {
            const Direction d = order[i];
            if (MultifaceFaceOf(state, d)) continue;    // MC isValidStateForPlacement
            if (MultifaceCanAttachTo(level, pos, d)) {
                return MultifaceStateWithFace(state, d, true);
            }
        }
        return state;
    }

    BlockRegistry::BlockShapeSet MultifaceShapeBoxes(BlockState state) {
        constexpr float k = 1.0f / 16.0f;
        BlockRegistry::BlockShapeSet out;

        auto push = [&](glm::vec3 mn, glm::vec3 mx) {
            if (out.count < BlockRegistry::kMaxShapeBoxes) {
                out.boxes[out.count++] = BlockRegistry::BlockShape{ mn, mx };
            }
        };

        if (MultifaceFaceOf(state, Direction::North)) push({0, 0, 0},     {1, 1, k});
        if (MultifaceFaceOf(state, Direction::South)) push({0, 0, 1 - k}, {1, 1, 1});
        if (MultifaceFaceOf(state, Direction::West))  push({0, 0, 0},     {k, 1, 1});
        if (MultifaceFaceOf(state, Direction::East))  push({1 - k, 0, 0}, {1, 1, 1});
        if (MultifaceFaceOf(state, Direction::Up))    push({0, 1 - k, 0}, {1, 1, 1});
        if (MultifaceFaceOf(state, Direction::Down))  push({0, 0, 0},     {1, k, 1});

        if (out.count == 0) push({0, 0, 0}, {1, 1, 1});   // MC Shapes.block()
        return out;
    }

} // namespace Game
