// File: src/common/world/block/RedstoneShapes.cpp
#include "common/world/block/RedstoneShapes.hpp"

#include "common/world/block/Direction.hpp"
#include "common/world/block/RedstoneWire.hpp"

#include <algorithm>
#include <string_view>

namespace Game {

    namespace {

        using BlockShape    = BlockRegistry::BlockShape;
        using BlockShapeSet = BlockRegistry::BlockShapeSet;

        // A pixel-space box, clamped to the cell the way Shapes.box requires.
        BlockShape Px(float x0, float y0, float z0, float x1, float y1, float z1) {
            const glm::vec3 mn = glm::clamp(glm::vec3(x0, y0, z0) / 16.0f, glm::vec3(0.0f), glm::vec3(1.0f));
            const glm::vec3 mx = glm::clamp(glm::vec3(x1, y1, z1) / 16.0f, glm::vec3(0.0f), glm::vec3(1.0f));
            return BlockShape{ mn, mx };
        }

        // Quarter turns about +Y from the NORTH shape: Shapes.rotateHorizontal
        // maps east = one BLOCK_ROT_Y_90, south = two, west = three. One turn
        // takes (x, z) to (16 - z, x) about the block centre — the same
        // convention BlockModel's RotY90 and RotateAabbPixels use.
        int TurnsFor(std::string_view facing) {
            if (facing == "east")  return 1;
            if (facing == "south") return 2;
            if (facing == "west")  return 3;
            return 0;
        }

        BlockShape TurnY(const BlockShape& north, int turns) {
            glm::vec3 a = north.min, b = north.max;
            for (int i = 0; i < turns; ++i) {
                a = glm::vec3(1.0f - a.z, a.y, a.x);
                b = glm::vec3(1.0f - b.z, b.y, b.x);
            }
            return BlockShape{ glm::min(a, b), glm::max(a, b) };
        }

        void Add(BlockShapeSet& set, const BlockShape& box) {
            if (set.count < BlockRegistry::kMaxShapeBoxes) set.boxes[set.count++] = box;
        }

        bool IsPressurePlate(BlockID id) {
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            constexpr std::string_view suffix = "_pressure_plate";
            return slug.size() > suffix.size() &&
                   slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool IsRail(BlockID id) {
            return id == BlockID::Rail || id == BlockID::PoweredRail ||
                   id == BlockID::DetectorRail || id == BlockID::ActivatorRail;
        }

        // ── Redstone wire (RedstoneWireBlock.makeShapes) ─────────────────
        //
        //   dot          = column(10, 0, 1)             → x,z 3..13, y 0..1
        //   floor[north] = boxZ(10, 0, 1, 0, 8)         → x 3..13, y 0..1, z 0..8
        //   up[north]    = boxZ(10, 16, 0, 1)           → x 3..13, y 0..16, z 0..1
        //   SIDE adds the floor arm, UP adds both; all unioned onto the dot.
        //
        // The dot and the floor arms on one axis are a single contiguous
        // strip, so the floor part is at most two boxes (one per axis) —
        // which is what keeps four UP arms inside kMaxShapeBoxes.
        BlockShapeSet WireShape(BlockState state) {
            const RedstoneSide n = RedstoneSideOf(state, Direction::North);
            const RedstoneSide e = RedstoneSideOf(state, Direction::East);
            const RedstoneSide s = RedstoneSideOf(state, Direction::South);
            const RedstoneSide w = RedstoneSideOf(state, Direction::West);

            BlockShapeSet set;
            // North–south strip, always present because it carries the dot.
            Add(set, Px(3.0f, 0.0f, IsConnected(n) ? 0.0f : 3.0f,
                        13.0f, 1.0f, IsConnected(s) ? 16.0f : 13.0f));
            if (IsConnected(e) || IsConnected(w)) {
                Add(set, Px(IsConnected(w) ? 0.0f : 3.0f, 0.0f, 3.0f,
                            IsConnected(e) ? 16.0f : 13.0f, 1.0f, 13.0f));
            }
            if (n == RedstoneSide::Up) Add(set, Px(3.0f, 0.0f, 0.0f, 13.0f, 16.0f, 1.0f));
            if (s == RedstoneSide::Up) Add(set, Px(3.0f, 0.0f, 15.0f, 13.0f, 16.0f, 16.0f));
            if (e == RedstoneSide::Up) Add(set, Px(15.0f, 0.0f, 3.0f, 16.0f, 16.0f, 13.0f));
            if (w == RedstoneSide::Up) Add(set, Px(0.0f, 0.0f, 3.0f, 1.0f, 16.0f, 13.0f));
            return set;
        }

        // ── Hopper (HopperBlock.makeShapes) ──────────────────────────────
        //
        //   inside  = column(12, 11, 16)
        //   outline = column(16, 10, 16) ∪ column(8, 4, 10), minus `inside`
        //   spout   = rotateAll(boxZ(4, 4, 8, 0, 8), pivot (8, 6, 8)) ∩ block
        //
        // The rim minus the inside is four 2-pixel walls and a 1-pixel floor;
        // the middle column sits under it; the spout hangs off the facing.
        // getCollisionShape is not overridden, so collision == outline —
        // which is exactly why you can drop things into it.
        BlockShapeSet HopperShape(BlockState state) {
            BlockShapeSet set;
            Add(set, Px( 0.0f, 10.0f,  0.0f,  2.0f, 16.0f, 16.0f));
            Add(set, Px(14.0f, 10.0f,  0.0f, 16.0f, 16.0f, 16.0f));
            Add(set, Px( 2.0f, 10.0f,  0.0f, 14.0f, 16.0f,  2.0f));
            Add(set, Px( 2.0f, 10.0f, 14.0f, 14.0f, 16.0f, 16.0f));
            Add(set, Px( 2.0f, 10.0f,  2.0f, 14.0f, 11.0f, 14.0f));   // floor of the bowl
            Add(set, Px( 4.0f,  4.0f,  4.0f, 12.0f, 10.0f, 12.0f));   // column(8, 4, 10)

            const std::string_view facing = state.GetValueByName("facing");
            if (facing == "down") {
                // BLOCK_ROT_X_90 of the north spout about (8, 6, 8), then ∩ block:
                // x 6..10, y -2..6 → 0..6, z 6..10.
                Add(set, Px(6.0f, 0.0f, 6.0f, 10.0f, 6.0f, 10.0f));
            } else {
                Add(set, TurnY(Px(6.0f, 4.0f, 0.0f, 10.0f, 8.0f, 8.0f), TurnsFor(facing)));
            }
            return set;
        }

        // ── Lectern (LecternBlock) ───────────────────────────────────────
        //
        //   SHAPE_COLLISION = column(16, 0, 2) ∪ column(8, 2, 14)
        //   SHAPES[north]   = boxZ(16, 10, 14, 1, 5.333) ∪ boxZ(16, 12, 16, 5.333, 9.667)
        //                   ∪ boxZ(16, 14, 18, 9.667, 14) ∪ SHAPE_COLLISION
        BlockShapeSet LecternShape(BlockState state, bool collision) {
            BlockShapeSet set;
            Add(set, Px(0.0f, 0.0f, 0.0f, 16.0f,  2.0f, 16.0f));
            Add(set, Px(4.0f, 2.0f, 4.0f, 12.0f, 14.0f, 12.0f));
            if (collision) return set;

            const int turns = TurnsFor(state.GetValueByName("facing"));
            Add(set, TurnY(Px(0.0f, 10.0f, 1.0f,      16.0f, 14.0f, 5.333333f), turns));
            Add(set, TurnY(Px(0.0f, 12.0f, 5.333333f, 16.0f, 16.0f, 9.666667f), turns));
            Add(set, TurnY(Px(0.0f, 14.0f, 9.666667f, 16.0f, 18.0f, 14.0f),     turns));
            return set;
        }

    } // namespace

    bool RedstoneShapeFor(BlockState state, BlockShape& out) {
        const BlockID id = state.Block();
        switch (id) {
            // WallTorchBlock.SHAPES (RedstoneWallTorchBlock and the soul
            // variant reuse it): rotateHorizontal(boxZ(5, 3, 13, 11, 16)).
            case BlockID::WallTorch:
            case BlockID::RedstoneWallTorch:
            case BlockID::SoulWallTorch:
            case BlockID::AmbrosiumWallTorch:   // Aether: a WallTorchBlock copy of Blocks.WALL_TORCH
                out = TurnY(Px(5.5f, 3.0f, 11.0f, 10.5f, 13.0f, 16.0f),
                            TurnsFor(state.GetValueByName("facing")));
                return true;

            // TripWireHookBlock.SHAPES: rotateHorizontal(boxZ(6, 0, 10, 10, 16)).
            case BlockID::TripwireHook:
                out = TurnY(Px(5.0f, 0.0f, 10.0f, 11.0f, 10.0f, 16.0f),
                            TurnsFor(state.GetValueByName("facing")));
                return true;

            // TripWireBlock: attached = column(16, 1, 2.5), else column(16, 0, 8).
            case BlockID::Tripwire:
                out = (state.GetValueByName("attached") == "true")
                    ? Px(0.0f, 1.0f, 0.0f, 16.0f, 2.5f, 16.0f)
                    : Px(0.0f, 0.0f, 0.0f, 16.0f, 8.0f, 16.0f);
                return true;

            default:
                break;
        }

        // BaseRailBlock: SHAPE_SLOPE = column(16, 0, 8) on the ascending
        // shapes, SHAPE_FLAT = column(16, 0, 2) otherwise.
        if (IsRail(id)) {
            const std::string_view shape = state.GetValueByName("shape");
            const bool ascending = shape.rfind("ascending", 0) == 0;
            out = Px(0.0f, 0.0f, 0.0f, 16.0f, ascending ? 8.0f : 2.0f, 16.0f);
            return true;
        }

        // BasePressurePlateBlock: SHAPE = column(14, 0, 1), SHAPE_PRESSED =
        // column(14, 0, 0.5). Pressed is `powered` on the stone/wood plates
        // and `power > 0` on the weighted ones.
        if (IsPressurePlate(id)) {
            const std::string_view powered = state.GetValueByName("powered");
            const std::string_view power   = state.GetValueByName("power");
            const bool pressed = (powered == "true") || (!power.empty() && power != "0");
            out = Px(1.0f, 0.0f, 1.0f, 15.0f, pressed ? 0.5f : 1.0f, 15.0f);
            return true;
        }
        return false;
    }

    bool IsRedstoneMultiBoxBlock(BlockID id) {
        return id == BlockID::RedstoneWire || id == BlockID::Hopper || id == BlockID::Lectern;
    }

    BlockShapeSet RedstoneMultiBoxShape(BlockState state, bool collision) {
        switch (state.Block()) {
            case BlockID::RedstoneWire: return WireShape(state);
            case BlockID::Hopper:       return HopperShape(state);
            case BlockID::Lectern:      return LecternShape(state, collision);
            default: {
                BlockShapeSet set;
                Add(set, BlockShape{});
                return set;
            }
        }
    }

} // namespace Game
