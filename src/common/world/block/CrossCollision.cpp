// File: src/common/world/block/CrossCollision.cpp
#include "CrossCollision.hpp"
#include "BlockPlacement.hpp"
#include "FenceGate.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        // MC's PROPERTY_BY_DIRECTION, filtered to the horizontals. The property
        // NAMES are what the multipart `when` clauses key on, so they have to
        // read exactly as vanilla spells them.
        constexpr Direction kSides[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };
        constexpr std::string_view kSideNames[4] = { "north", "east", "south", "west" };
        // The same four sides as PropertyIds. These are the BOOLEAN north/east/
        // south/west of a fence or pane — NOT the three-valued NORTH_WALL etc.
        // a wall carries, which is a different property that happens to share
        // the name.
        constexpr PropertyId kSideProps[4] = {
            PropertyId::NORTH, PropertyId::EAST, PropertyId::SOUTH, PropertyId::WEST,
        };

        int SideIndex(Direction d) {
            for (int i = 0; i < 4; ++i) if (kSides[i] == d) return i;
            return 0;
        }

        bool EndsWith(const std::string& s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // ── Shape dimensions, straight off the constructor calls ────────────
        //
        //   FenceBlock:     super(4.0F, 16.0F, 4.0F, 16.0F, 24.0F, …)
        //   IronBarsBlock:  super(2.0F, 16.0F, 2.0F, 16.0F, 16.0F, …)
        //
        // as (postWidth, postHeight, wallWidth, wallHeight, collisionHeight).
        struct CrossDims {
            float postWidth, postHeight, wallWidth, wallHeight, collisionHeight;
        };
        constexpr CrossDims kFenceDims{ 4.0f, 16.0f, 4.0f, 16.0f, 24.0f };
        constexpr CrossDims kBarsDims { 2.0f, 16.0f, 2.0f, 16.0f, 16.0f };

        // MC Block.column(width, minY, maxY) — a box of the given width centred
        // on the cell's vertical axis.
        BlockRegistry::BlockShape Column(float widthPx, float minYPx, float maxYPx) {
            const float half = widthPx * 0.5f / 16.0f;
            return BlockRegistry::BlockShape{
                { 0.5f - half, minYPx / 16.0f, 0.5f - half },
                { 0.5f + half, maxYPx / 16.0f, 0.5f + half },
            };
        }

        // MC Block.boxZ(width, minY, maxY, minZ, maxZ) with minZ=0, maxZ=8 —
        // the NORTH arm, which is the orientation `fence_side` is authored in
        // (its blockstate applies y=0 for north, 90 for east, and so on).
        BlockRegistry::BlockShape ArmNorth(float widthPx, float minYPx, float maxYPx) {
            const float half = widthPx * 0.5f / 16.0f;
            return BlockRegistry::BlockShape{
                { 0.5f - half, minYPx / 16.0f, 0.0f },
                { 0.5f + half, maxYPx / 16.0f, 0.5f },
            };
        }

        // One 90° turn about Y, clockwise from above (+X -> +Z) — the same
        // sense as the blockstate JSON's `"y"`, so the boxes track the models.
        BlockRegistry::BlockShape RotateY90(const BlockRegistry::BlockShape& b) {
            const float x0 = 1.0f - b.max.z, x1 = 1.0f - b.min.z;
            const float z0 = b.min.x,        z1 = b.max.x;
            return BlockRegistry::BlockShape{
                { std::min(x0, x1), b.min.y, std::min(z0, z1) },
                { std::max(x0, x1), b.max.y, std::max(z0, z1) },
            };
        }

        const CrossDims& DimsFor(BlockID id) {
            return IsPaneBlock(id) ? kBarsDims : kFenceDims;
        }

        // Shared by the outline and collision builders — they differ only in
        // how tall the post and the arms are, which is exactly how MC's
        // makeShapes is called twice with different heights.
        BlockRegistry::BlockShapeSet BuildShape(BlockState state,
                                                float postHeightPx, float wallTopPx) {
            const CrossDims& d = DimsFor(state.Block());
            BlockRegistry::BlockShapeSet out;
            out.boxes[out.count++] = Column(d.postWidth, 0.0f, postHeightPx);

            const BlockRegistry::BlockShape armNorth =
                ArmNorth(d.wallWidth, 0.0f, wallTopPx);
            for (int i = 0; i < 4; ++i) {
                if (!CrossSideOf(state, kSides[i])) continue;
                // North is turn 0, then one clockwise turn per step through
                // north -> east -> south -> west.
                BlockRegistry::BlockShape arm = armNorth;
                for (int t = 0; t < i; ++t) arm = RotateY90(arm);
                if (out.count < BlockRegistry::kMaxShapeBoxes) {
                    out.boxes[out.count++] = arm;
                }
            }
            return out;
        }

    } // namespace

    bool IsConnectionException(BlockID id) {
        const std::string& n = BlockRegistry::Get(id).modelName;
        if (EndsWith(n, "_leaves")) return true;                 // LeavesBlock
        if (n.find("shulker_box") != std::string::npos) return true;
        return n == "barrier" || n == "carved_pumpkin" || n == "jack_o_lantern" ||
               n == "melon"   || n == "pumpkin";
    }

    bool IsCrossCollisionBlock(BlockID id) {
        return IsFenceBlock(id) || IsPaneBlock(id);
    }

    bool IsFenceBlock(BlockID id) {
        const std::string& n = BlockRegistry::Get(id).modelName;
        // "_fence" would also swallow "_fence_gate", which is a different class
        // with a different property set — check the longer suffix first.
        if (EndsWith(n, "_fence_gate")) return false;
        return EndsWith(n, "_fence");
    }

    bool IsWoodenFence(BlockID id) {
        // #minecraft:wooden_fences is every fence except nether_brick_fence,
        // which is the only non-wooden one in vanilla.
        return IsFenceBlock(id) && BlockRegistry::Get(id).modelName != "nether_brick_fence";
    }

    bool IsPaneBlock(BlockID id) {
        const std::string& n = BlockRegistry::Get(id).modelName;
        return n == "iron_bars" || EndsWith(n, "_pane");
    }

    bool CrossSideOf(BlockState state, Direction dir) {
        if (!IsHorizontal(dir)) return false;
        return state.GetIndex(kSideProps[SideIndex(dir)]) == 0;   // [true, false]
    }

    BlockState CrossStateWithSide(BlockState state, Direction dir, bool on) {
        // One property write. This used to round-trip the whole tuple through a
        // string map to avoid clobbering WATERLOGGED; SetIndex leaves every
        // other property alone by construction.
        return state.SetIndex(kSideProps[SideIndex(dir)], on ? 0 : 1);
    }

    bool CrossConnectsTo(const IBlockAccess& level, const glm::ivec3& pos,
                         BlockID id, Direction dir) {
        const glm::ivec3 p{ pos.x + StepX(dir), pos.y + StepY(dir), pos.z + StepZ(dir) };
        const BlockID neighbour = level.GetBlock(p.x, p.y, p.z);
        if (neighbour == BlockID::Air) return false;

        // Common to both subclasses: a sturdy face on a block that is not one
        // of the connection exceptions. `Opposite(dir)` is the neighbour's face
        // that looks back at us, which is the one MC tests.
        if (!IsConnectionException(neighbour) &&
            IsFaceSturdyAt(level, p, Opposite(dir))) {
            return true;
        }

        if (IsFenceBlock(id)) {
            // FenceBlock.isSameFence: both wooden or both not. This is what
            // keeps a nether brick fence from joining an oak one.
            if (IsFenceBlock(neighbour) && IsWoodenFence(neighbour) == IsWoodenFence(id)) {
                return true;
            }
            // A fence meets the gate's HINGE side, never its face — see
            // FenceGateConnectsToDirection.
            if (IsFenceGateBlock(neighbour)) {
                return FenceGateConnectsToDirection(
                    level.GetBlockState(p.x, p.y, p.z), dir);
            }
            return false;
        }

        // IronBarsBlock.attachsTo: another bars/pane block, or a wall.
        if (IsPaneBlock(neighbour)) return true;
        return EndsWith(BlockRegistry::Get(neighbour).modelName, "_wall");
    }

    BlockState CrossPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState fallback) {
        const BlockID id = fallback.Block();
        BlockState state = fallback;
        for (Direction d : kSides) {
            state = CrossStateWithSide(state, d, CrossConnectsTo(level, pos, id, d));
        }
        return state;
    }

    BlockState CrossUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                BlockState state, Direction toNeighbour) {
        // MC schedules a water tick when waterlogged; there are no fluid ticks
        // here, so only the side matters. Vertical changes are ignored, exactly
        // as vanilla's `directionToNeighbour.getAxis().isHorizontal()` gate.
        if (!IsHorizontal(toNeighbour)) return state;
        return CrossStateWithSide(state, toNeighbour,
                                  CrossConnectsTo(level, pos, state.Block(), toNeighbour));
    }

    BlockRegistry::BlockShapeSet CrossShapeBoxes(BlockState state) {
        const CrossDims& d = DimsFor(state.Block());
        return BuildShape(state, d.postHeight, d.wallHeight);
    }

    BlockRegistry::BlockShapeSet CrossCollisionBoxes(BlockState state) {
        const CrossDims& d = DimsFor(state.Block());
        return BuildShape(state, d.collisionHeight, d.collisionHeight);
    }

} // namespace Game
