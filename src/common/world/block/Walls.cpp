// File: src/common/world/block/Walls.cpp
#include "Walls.hpp"
#include "CrossCollision.hpp"
#include "FenceGate.hpp"
#include "BlockPlacement.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        // MC's PROPERTY_BY_DIRECTION for walls. The property names are what the
        // multipart `when` clauses read.
        constexpr Direction kSides[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };
        constexpr PropertyId kSideProps[4] = {
            PropertyId::NORTH_WALL, PropertyId::EAST_WALL,
            PropertyId::SOUTH_WALL, PropertyId::WEST_WALL,
        };
        constexpr std::string_view kSideNames[4] = { "north", "east", "south", "west" };
        constexpr std::string_view kWallSideNames[3] = { "none", "low", "tall" };

        int SideIndex(Direction d) {
            for (int i = 0; i < 4; ++i) if (kSides[i] == d) return i;
            return 0;
        }

        bool EndsWith(const std::string& s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // ── Shapes, from makeShapes(postHeight, wallTop) ────────────────────
        //   post = column(8, 0, postHeight)
        //   low  = boxZ(6, 0, wallTop,    0, 11)   [north; rotated for the rest]
        //   tall = boxZ(6, 0, postHeight, 0, 11)
        BlockRegistry::BlockShape Column(float widthPx, float minYPx, float maxYPx) {
            const float half = widthPx * 0.5f / 16.0f;
            return BlockRegistry::BlockShape{
                { 0.5f - half, minYPx / 16.0f, 0.5f - half },
                { 0.5f + half, maxYPx / 16.0f, 0.5f + half },
            };
        }
        BlockRegistry::BlockShape ArmNorth(float widthPx, float maxYPx, float maxZPx) {
            const float half = widthPx * 0.5f / 16.0f;
            return BlockRegistry::BlockShape{
                { 0.5f - half, 0.0f,             0.0f },
                { 0.5f + half, maxYPx / 16.0f,   maxZPx / 16.0f },
            };
        }
        BlockRegistry::BlockShape RotateY90(const BlockRegistry::BlockShape& b) {
            const float x0 = 1.0f - b.max.z, x1 = 1.0f - b.min.z;
            const float z0 = b.min.x,        z1 = b.max.x;
            return BlockRegistry::BlockShape{
                { std::min(x0, x1), b.min.y, std::min(z0, z1) },
                { std::max(x0, x1), b.max.y, std::max(z0, z1) },
            };
        }

        BlockRegistry::BlockShapeSet BuildShape(BlockState state,
                                                float postHeightPx, float wallTopPx) {
            BlockRegistry::BlockShapeSet out;
            if (WallUpOf(state)) {
                out.boxes[out.count++] = Column(8.0f, 0.0f, postHeightPx);
            }
            const BlockRegistry::BlockShape low  = ArmNorth(6.0f, wallTopPx,    11.0f);
            const BlockRegistry::BlockShape tall = ArmNorth(6.0f, postHeightPx, 11.0f);
            for (int i = 0; i < 4; ++i) {
                const WallSide side = WallSideOf(state, kSides[i]);
                if (side == WallSide::None) continue;
                BlockRegistry::BlockShape arm = (side == WallSide::Tall) ? tall : low;
                for (int t = 0; t < i; ++t) arm = RotateY90(arm);
                if (out.count < BlockRegistry::kMaxShapeBoxes) out.boxes[out.count++] = arm;
            }
            // A wall with no post and no sides would be an empty shape. MC
            // allows that (Shapes.empty()); here an empty set would read as
            // "no collision at all", so fall back to the post. The state is
            // unreachable through placement — shouldRaisePost answers true for
            // the all-NONE case — but a decoded save could carry it.
            if (out.count == 0) out.boxes[out.count++] = Column(8.0f, 0.0f, postHeightPx);
            return out;
        }

        // MC's TEST_SHAPE_POST / TEST_SHAPES_WALL, reduced to the XZ footprint
        // the coverage test actually needs. Both are 2px-wide probes:
        //   TEST_SHAPE_POST  = column(2, 0, 16)
        //   TEST_SHAPES_WALL = rotateHorizontal(boxZ(2, 16, 0, 9))
        struct Footprint { float minX, minZ, maxX, maxZ; };
        constexpr Footprint kTestPost{ 7.0f / 16.0f, 7.0f / 16.0f, 9.0f / 16.0f, 9.0f / 16.0f };
        constexpr Footprint kTestArmNorth{ 7.0f / 16.0f, 0.0f, 9.0f / 16.0f, 9.0f / 16.0f };

        Footprint RotateFootprintY90(const Footprint& f) {
            const float x0 = 1.0f - f.maxZ, x1 = 1.0f - f.minZ;
            const float z0 = f.minX,        z1 = f.maxX;
            return Footprint{ std::min(x0, x1), std::min(z0, z1),
                              std::max(x0, x1), std::max(z0, z1) };
        }

        // MC isCovered(aboveShape, testShape), where aboveShape is the block
        // above's collision shape sliced to its DOWN face: does the underside
        // of that block cover the probe?
        //
        // Approximated one box at a time, the same way IsFaceSturdy is — a
        // single box of the above block that reaches its own bottom face and
        // spans the probe's footprint. Correct for the cases that decide a
        // wall's look (a full cube overhead, another wall's post, air) and
        // conservative for anything stranger.
        bool IsCoveredFromAbove(const IBlockAccess& level, const glm::ivec3& abovePos,
                                const Footprint& probe) {
            const BlockID id = level.GetBlock(abovePos.x, abovePos.y, abovePos.z);
            if (id == BlockID::Air) return false;
            if (!BlockRegistry::HasCollision(id)) return false;
            const auto set = BlockRegistry::GetBlockCollisionShapeSet(level.GetBlockState(abovePos.x, abovePos.y, abovePos.z));
            constexpr float eps = 1.0e-4f;
            for (const auto& b : set) {
                if (b.min.y > eps) continue;             // must reach the underside
                if (b.min.x <= probe.minX + eps && b.max.x >= probe.maxX - eps &&
                    b.min.z <= probe.minZ + eps && b.max.z >= probe.maxZ - eps) {
                    return true;
                }
            }
            return false;
        }

        // #minecraft:wall_post_override — blocks that keep a wall's post up
        // even when the geometry says it could be dropped, because they need
        // something to stand on.
        bool IsWallPostOverride(BlockID id) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            if (EndsWith(n, "_pressure_plate")) return true;
            if (EndsWith(n, "_sign") || EndsWith(n, "_hanging_sign")) return true;
            if (EndsWith(n, "_banner")) return true;
            return n == "torch" || n == "soul_torch" || n == "redstone_torch" ||
                   n == "wall_torch" || n == "soul_wall_torch" ||
                   n == "redstone_wall_torch" || n == "tripwire";
        }

        // MC makeWallState(connects, aboveShape, testShape).
        WallSide MakeWallState(const IBlockAccess& level, const glm::ivec3& abovePos,
                               bool connects, int sideIndex) {
            if (!connects) return WallSide::None;
            Footprint probe = kTestArmNorth;
            for (int t = 0; t < sideIndex; ++t) probe = RotateFootprintY90(probe);
            return IsCoveredFromAbove(level, abovePos, probe) ? WallSide::Tall
                                                              : WallSide::Low;
        }

        // MC shouldRaisePost(state, topNeighbour, aboveShape).
        bool ShouldRaisePost(const IBlockAccess& level, const glm::ivec3& abovePos,
                             WallSide north, WallSide east, WallSide south, WallSide west) {
            const BlockID above = level.GetBlock(abovePos.x, abovePos.y, abovePos.z);
            if (IsWallBlock(above) &&
                WallUpOf(level.GetBlockState(abovePos.x, abovePos.y, abovePos.z))) {
                return true;                                   // a post to carry on from
            }

            const bool nNone = north == WallSide::None;
            const bool eNone = east  == WallSide::None;
            const bool sNone = south == WallSide::None;
            const bool wNone = west  == WallSide::None;
            // A post is needed unless the wall runs straight through: all four
            // empty is a lone stub, and a mismatch on either axis is a corner,
            // a stub or a T-junction.
            if ((nNone && sNone && wNone && eNone) || (nNone != sNone) || (wNone != eNone)) {
                return true;
            }
            // A straight run whose arms already reach the ceiling needs no post.
            if ((north == WallSide::Tall && south == WallSide::Tall) ||
                (east  == WallSide::Tall && west  == WallSide::Tall)) {
                return false;
            }
            return IsWallPostOverride(above) || IsCoveredFromAbove(level, abovePos, kTestPost);
        }

        // MC's private updateShape(level, state, topPos, topNeighbour, n,e,s,w) —
        // the shared tail of both getStateForPlacement and updateShape.
        // Returned a uint8_t until the BlockState conversion — which silently
        // truncated, because a wall has 324 states and states 256..323 are
        // perfectly reachable. There is no index to truncate any more.
        BlockState ResolveFromConnections(const IBlockAccess& level, const glm::ivec3& pos,
                                          BlockState state,
                                          bool n, bool e, bool s, bool w) {
            const glm::ivec3 above{ pos.x, pos.y + 1, pos.z };
            const WallSide north = MakeWallState(level, above, n, 0);
            const WallSide east  = MakeWallState(level, above, e, 1);
            const WallSide south = MakeWallState(level, above, s, 2);
            const WallSide west  = MakeWallState(level, above, w, 3);
            const bool up = ShouldRaisePost(level, above, north, east, south, west);
            return WallStateFrom(state, up, north, east, south, west);
        }

    } // namespace

    bool IsWallBlock(BlockID id) {
        // Ends-with, not contains: "_wall_sign", "_wall_torch", "_wall_banner"
        // and the coral wall fans all carry "_wall_" in the middle and are not
        // walls at all.
        return EndsWith(BlockRegistry::Get(id).modelName, "_wall");
    }

    WallSide WallSideOf(BlockState state, Direction dir) {
        if (!IsHorizontal(dir)) return WallSide::None;
        // NORTH_WALL etc. — the THREE-valued none/low/tall property, not the
        // boolean north/east/south/west a fence carries under the same name.
        const int v = state.GetIndex(kSideProps[SideIndex(dir)]);
        switch (v) {
            case 1:  return WallSide::Low;
            case 2:  return WallSide::Tall;
            default: return WallSide::None;      // also the -1 "not a wall" case
        }
    }

    bool WallUpOf(BlockState state) {
        return state.GetIndex(PropertyId::UP) == 0;     // [true, false]
    }

    BlockState WallStateFrom(BlockState state, bool up,
                             WallSide north, WallSide east, WallSide south, WallSide west) {
        // MC's shouldRaisePost answers true whenever all four sides are NONE,
        // so "no post and no arms" is unreachable through placement — and it
        // is the one wall state whose multipart file selects no model at all.
        // Pinned here so a decoded save cannot produce an invisible wall.
        if (!up && north == WallSide::None && east == WallSide::None &&
            south == WallSide::None && west == WallSide::None) {
            up = true;
        }

        // WallSide's enum order is None, Low, Tall; the property's value order
        // is none, low, tall — so the enum value IS the property index.
        return state
            .SetIndex(PropertyId::UP, up ? 0 : 1)
            .SetIndex(PropertyId::NORTH_WALL, static_cast<int>(north))
            .SetIndex(PropertyId::EAST_WALL,  static_cast<int>(east))
            .SetIndex(PropertyId::SOUTH_WALL, static_cast<int>(south))
            .SetIndex(PropertyId::WEST_WALL,  static_cast<int>(west));
    }

    bool WallConnectsTo(const IBlockAccess& level, const glm::ivec3& pos, Direction dir) {
        const glm::ivec3 p{ pos.x + StepX(dir), pos.y + StepY(dir), pos.z + StepZ(dir) };
        const BlockID neighbour = level.GetBlock(p.x, p.y, p.z);
        if (neighbour == BlockID::Air) return false;

        // MC's clause order, which matters: a wall connects to another wall
        // even when that wall's own face is not sturdy.
        if (IsWallBlock(neighbour)) return true;
        if (IsPaneBlock(neighbour)) return true;    // `block instanceof IronBarsBlock`
        if (IsFenceGateBlock(neighbour)) {
            return FenceGateConnectsToDirection(level.GetBlockState(p.x, p.y, p.z), dir);
        }
        return IsFaceSturdyAt(level, p, Opposite(dir)) && !IsConnectionException(neighbour);
    }

    BlockState WallPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState fallback) {
        return ResolveFromConnections(level, pos, fallback,
                                      WallConnectsTo(level, pos, Direction::North),
                                      WallConnectsTo(level, pos, Direction::East),
                                      WallConnectsTo(level, pos, Direction::South),
                                      WallConnectsTo(level, pos, Direction::West));
    }

    BlockState WallUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                               BlockState state, Direction toNeighbour) {
        // MC: DOWN falls through to the base updateShape, which does nothing
        // for a wall.
        if (toNeighbour == Direction::Down) return state;

        // Whatever changed, every side's LOW/TALL and the post are re-derived
        // from the block above — a block placed overhead makes all four arms
        // grow at once. Only the CONNECTION of the side that changed is
        // re-resolved (MC's sideUpdate keeps the others as they are).
        bool connected[4];
        for (int i = 0; i < 4; ++i) {
            connected[i] = (toNeighbour == kSides[i])
                               ? WallConnectsTo(level, pos, kSides[i])
                               : WallSideOf(state, kSides[i]) != WallSide::None;
        }
        return ResolveFromConnections(level, pos, state,
                                      connected[0], connected[1], connected[2], connected[3]);
    }

    BlockRegistry::BlockShapeSet WallShapeBoxes(BlockState state) {
        return BuildShape(state, /*postHeight=*/16.0f, /*wallTop=*/14.0f);
    }

    BlockRegistry::BlockShapeSet WallCollisionBoxes(BlockState state) {
        return BuildShape(state, /*postHeight=*/24.0f, /*wallTop=*/24.0f);
    }

} // namespace Game
