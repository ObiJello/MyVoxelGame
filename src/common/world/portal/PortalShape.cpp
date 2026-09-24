// File: src/common/world/portal/PortalShape.cpp
//
// Line references are to
// minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/world/level/portal/PortalShape.java.

#include "PortalShape.hpp"

#include "BlockUtil.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <algorithm>

namespace Game {

    namespace {
        inline glm::ivec3 Relative(const glm::ivec3& p, Direction d, int n) {
            return { p.x + StepX(d) * n, p.y + StepY(d) * n, p.z + StepZ(d) * n };
        }
    } // namespace

    // PortalShape.java:162
    bool PortalShape::IsEmptyForPortal(BlockState state, const PortalFamily& family) {
        const BlockID id = state.Block();
        // MC: state.isAir() || state.is(BlockTags.FIRE) || state.is(NETHER_PORTAL).
        // BlockTags.FIRE is exactly {fire, soul_fire} in vanilla data. The
        // portal block is the FAMILY's — a hush_portal inside an obsidian
        // frame is not "empty" for a nether walk, and vice versa.
        // AetherPortalShape.isEmpty adds water: the Aether frame is lit WITH
        // water, so a frame that already holds some must still count as empty.
        return id == BlockID::Air
            || id == BlockID::Fire
            || id == BlockID::SoulFire
            || id == family.portalBlock
            || (family.id == PortalFamilyId::Aether && id == BlockID::Water);
    }

    // PortalShape.java:50
    std::optional<PortalShape> PortalShape::FindEmptyPortalShape(
        const IBlockAccess& level, const glm::ivec3& pos, Axis preferredAxis,
        const PortalFamily& family)
    {
        return FindPortalShape(
            level, pos,
            [](const PortalShape& s) { return s.IsValid() && s.m_numPortalBlocks == 0; },
            preferredAxis, family);
    }

    // PortalShape.java:54
    std::optional<PortalShape> PortalShape::FindPortalShape(
        const IBlockAccess& level, const glm::ivec3& pos,
        const std::function<bool(const PortalShape&)>& isValid, Axis preferredAxis,
        const PortalFamily& family)
    {
        PortalShape first = FindAnyShape(level, pos, preferredAxis, family);
        if (isValid(first)) return first;

        const Axis otherAxis = (preferredAxis == Axis::X) ? Axis::Z : Axis::X;
        PortalShape second = FindAnyShape(level, pos, otherAxis, family);
        if (isValid(second)) return second;

        return std::nullopt;
    }

    // PortalShape.java:64
    //
    // rightDir is WEST for an X-axis portal and SOUTH for a Z-axis one. It is
    // "right" only in the sense of "the direction width grows in"; vanilla
    // picks these two so that bottomLeft ends up at the corner it does, and
    // changing them changes which cell createPortalBlocks starts from.
    PortalShape PortalShape::FindAnyShape(const IBlockAccess& level,
                                          const glm::ivec3& pos, Axis axis,
                                          const PortalFamily& family)
    {
        const Direction rightDir = (axis == Axis::X) ? Direction::West : Direction::South;

        glm::ivec3 bottomLeft{0, 0, 0};
        if (!CalculateBottomLeft(level, family, rightDir, pos, bottomLeft)) {
            return PortalShape(family, axis, 0, rightDir, pos, 0, 0);
        }

        const int width = CalculateWidth(level, family, bottomLeft, rightDir);
        if (width == 0) {
            return PortalShape(family, axis, 0, rightDir, bottomLeft, 0, 0);
        }

        int portalBlockCount = 0;
        const int height = CalculateHeight(level, family, bottomLeft, rightDir, width,
                                           portalBlockCount);
        return PortalShape(family, axis, portalBlockCount, rightDir, bottomLeft, width, height);
    }

    // PortalShape.java:81
    bool PortalShape::CalculateBottomLeft(const IBlockAccess& level, const PortalFamily& family,
                                          Direction rightDir, glm::ivec3 pos, glm::ivec3& out)
    {
        // Fall to the bottom of the cavity, but never more than 21 blocks —
        // that is the tallest portal, so anything further down cannot be the
        // same frame.
        const int minY = std::max(Math::WorldCoordinates::MIN_WORLD_Y, pos.y - kMaxHeight);
        while (pos.y > minY &&
               IsEmptyForPortal(level.GetBlockState(pos.x, pos.y - 1, pos.z), family)) {
            pos.y -= 1;
        }

        const Direction leftDir = Opposite(rightDir);
        const int edge = DistanceUntilEdgeAboveFrame(level, family, pos, leftDir) - 1;
        if (edge < 0) return false;

        out = Relative(pos, leftDir, edge);
        return true;
    }

    // PortalShape.java:90
    int PortalShape::CalculateWidth(const IBlockAccess& level, const PortalFamily& family,
                                    const glm::ivec3& bottomLeft, Direction rightDir)
    {
        const int width = DistanceUntilEdgeAboveFrame(level, family, bottomLeft, rightDir);
        return (width >= kMinWidth && width <= kMaxWidth) ? width : 0;
    }

    // PortalShape.java:95
    //
    // Walks outward one cell at a time. Each step must be cavity ("empty")
    // with frame directly beneath it; the walk ends successfully the moment it
    // hits a frame block, and that step count IS the width. Anything else —
    // a non-frame solid, a missing floor, or running past 21 — is a failure,
    // reported as 0.
    int PortalShape::DistanceUntilEdgeAboveFrame(const IBlockAccess& level, const PortalFamily& family,
                                                 const glm::ivec3& pos,
                                                 Direction direction)
    {
        for (int width = 0; width <= kMaxWidth; ++width) {
            const glm::ivec3 at = Relative(pos, direction, width);
            const BlockState state = level.GetBlockState(at.x, at.y, at.z);
            if (!IsEmptyForPortal(state, family)) {
                if (IsFrame(state, family)) return width;
                break;
            }
            const BlockState below = level.GetBlockState(at.x, at.y - 1, at.z);
            if (!IsFrame(below, family)) break;
        }
        return 0;
    }

    // PortalShape.java:117
    int PortalShape::CalculateHeight(const IBlockAccess& level, const PortalFamily& family,
                                     const glm::ivec3& bottomLeft, Direction rightDir,
                                     int width, int& portalBlockCount)
    {
        const int height = DistanceUntilTop(level, family, bottomLeft, rightDir, width,
                                            portalBlockCount);
        const bool ok = height >= kMinHeight && height <= kMaxHeight
                     && HasTopFrame(level, family, bottomLeft, rightDir, width, height);
        return ok ? height : 0;
    }

    // PortalShape.java:134
    //
    // Climbs row by row. A row belongs to the portal only if BOTH side frames
    // are present at that height and every interior cell is empty. The portal
    // block tally is collected on the way up so IsComplete can be answered
    // from the same walk.
    int PortalShape::DistanceUntilTop(const IBlockAccess& level, const PortalFamily& family,
                                      const glm::ivec3& bottomLeft, Direction rightDir,
                                      int width, int& portalBlockCount)
    {
        for (int height = 0; height < kMaxHeight; ++height) {
            const glm::ivec3 row = { bottomLeft.x, bottomLeft.y + height, bottomLeft.z };

            const glm::ivec3 leftFrame = Relative(row, rightDir, -1);
            if (!IsFrame(level.GetBlockState(leftFrame.x, leftFrame.y, leftFrame.z), family)) {
                return height;
            }

            const glm::ivec3 rightFrame = Relative(row, rightDir, width);
            if (!IsFrame(level.GetBlockState(rightFrame.x, rightFrame.y, rightFrame.z), family)) {
                return height;
            }

            for (int i = 0; i < width; ++i) {
                const glm::ivec3 cell = Relative(row, rightDir, i);
                const BlockState state = level.GetBlockState(cell.x, cell.y, cell.z);
                if (!IsEmptyForPortal(state, family)) return height;
                if (state.Is(family.portalBlock)) ++portalBlockCount;
            }
        }
        return kMaxHeight;
    }

    // PortalShape.java:123
    bool PortalShape::HasTopFrame(const IBlockAccess& level, const PortalFamily& family,
                                  const glm::ivec3& bottomLeft, Direction rightDir,
                                  int width, int height)
    {
        for (int i = 0; i < width; ++i) {
            const glm::ivec3 top = Relative(
                { bottomLeft.x, bottomLeft.y + height, bottomLeft.z }, rightDir, i);
            if (!IsFrame(level.GetBlockState(top.x, top.y, top.z), family)) return false;
        }
        return true;
    }

    // PortalShape.java:179
    glm::dvec3 PortalShape::GetRelativePosition(const FoundRectangle& rect, Axis axis,
                                                const glm::dvec3& position,
                                                float entityWidth, float entityHeight) {
        // MC Mth.inverseLerp(delta, start, end) with start 0 — so just
        // delta / span, clamped. The `span > 0` guards are not defensive
        // padding: an opening exactly as wide as the entity has nowhere to
        // place them proportionally, and MC's answers there (centre
        // horizontally, bottom vertically) are what stop a division by zero
        // becoming a NaN position.
        const double widthSpan  = static_cast<double>(rect.axis1Size) - entityWidth;
        const double heightSpan = static_cast<double>(rect.axis2Size) - entityHeight;

        const int    axisMin    = (axis == Axis::X) ? rect.minCorner.x : rect.minCorner.z;
        const double axisCoord  = (axis == Axis::X) ? position.x : position.z;

        double relativeRight = 0.5;
        if (widthSpan > 0.0) {
            const double bottomStart = static_cast<double>(axisMin) + entityWidth / 2.0;
            relativeRight = std::clamp((axisCoord - bottomStart) / widthSpan, 0.0, 1.0);
        }

        double relativeUp = 0.0;
        if (heightSpan > 0.0) {
            relativeUp = std::clamp(
                (position.y - static_cast<double>(rect.minCorner.y)) / heightSpan,
                0.0, 1.0);
        }

        // The axis the portal is THIN on — where "in front of" and "behind"
        // live. Unlike the other two this is a signed distance in blocks, not
        // a fraction, because the destination portal is the same one block
        // thick whatever its size.
        const int    forwardMin   = (axis == Axis::X) ? rect.minCorner.z : rect.minCorner.x;
        const double forwardCoord = (axis == Axis::X) ? position.z : position.x;
        const double relativeForward = forwardCoord - (static_cast<double>(forwardMin) + 0.5);

        return { relativeRight, relativeUp, relativeForward };
    }

    namespace {

        // Does an entity box centred here overlap any block's COLLISION shape?
        //
        // Collision, not outline: a nether portal has an outline but no
        // collision, so an arrival standing inside the portal it came out of
        // must not count as blocked — which is every arrival.
        bool BoxCollides(const IBlockAccess& level, const glm::dvec3& center,
                         double halfWidth, double halfHeight) {
            const glm::dvec3 lo{ center.x - halfWidth, center.y - halfHeight,
                                 center.z - halfWidth };
            const glm::dvec3 hi{ center.x + halfWidth, center.y + halfHeight,
                                 center.z + halfWidth };

            // Same epsilon as Entity::CheckInsideBlocks, for the same reason:
            // a box resting exactly on a boundary must not claim the cell it is
            // merely touching, or every arrival flush against the portal floor
            // reads as blocked.
            constexpr double kShrink = 0.001;

            const int minX = static_cast<int>(std::floor(lo.x + kShrink));
            const int minY = static_cast<int>(std::floor(lo.y + kShrink));
            const int minZ = static_cast<int>(std::floor(lo.z + kShrink));
            const int maxX = static_cast<int>(std::floor(hi.x - kShrink));
            const int maxY = static_cast<int>(std::floor(hi.y - kShrink));
            const int maxZ = static_cast<int>(std::floor(hi.z - kShrink));

            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    for (int x = minX; x <= maxX; ++x) {
                        const BlockState state = level.GetBlockState(x, y, z);
                        if (!BlockRegistry::HasCollision(state.Block())) continue;
                        for (const auto& s : BlockRegistry::GetBlockCollisionShapeSet(state)) {
                            if (hi.x - kShrink > x + s.min.x && lo.x + kShrink < x + s.max.x &&
                                hi.y - kShrink > y + s.min.y && lo.y + kShrink < y + s.max.y &&
                                hi.z - kShrink > z + s.min.z && lo.z + kShrink < z + s.max.z) {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        }

    } // namespace

    // PortalShape.java:204
    glm::dvec3 PortalShape::FindCollisionFreePosition(const IBlockAccess& level,
                                                      const glm::dvec3& bottomCenter,
                                                      float entityWidth,
                                                      float entityHeight) {
        // MC SAFE_TRAVEL_MAX_ENTITY_XY (PortalShape.java:32) — anything bigger
        // than this is not nudged at all, on the grounds that finding room for
        // it would move it further than the fix is worth.
        constexpr float kSafeTravelMaxEntityXY = 4.0f;
        if (entityWidth > kSafeTravelMaxEntityXY || entityHeight > kSafeTravelMaxEntityXY) {
            return bottomCenter;
        }

        const double halfHeight = static_cast<double>(entityHeight) / 2.0;
        const double halfWidth  = static_cast<double>(entityWidth) / 2.0;
        const glm::dvec3 preferred = bottomCenter + glm::dvec3(0.0, halfHeight, 0.0);

        // The overwhelmingly common case: the spot MC computed is already
        // clear, and the whole search is skipped.
        if (!BoxCollides(level, preferred, halfWidth, halfHeight)) {
            return bottomCenter;
        }

        // MC's allowed-centre volume (PortalShape.java:208):
        //   AABB.ofSize(center, width, 0, width).expandTowards(0, 1, 0)
        // — a flat width x width square at the entity's middle, swept one block
        // upward. Sideways travel is capped at half a width by construction,
        // which is what stops a portal quietly relocating you.
        //
        // MC takes the exact closest point of the remaining shape; sampling
        // takes the closest of a grid over it. SAFE_TRAVEL_MAX_VERTICAL_DELTA
        // (:33) is that one block of upward room.
        constexpr double kVerticalDelta = 1.0;
        constexpr int    kHorizontalSteps = 8;
        constexpr int    kVerticalSteps   = 16;

        glm::dvec3 best = bottomCenter;
        double     bestDistSqr = -1.0;

        for (int yi = 0; yi <= kVerticalSteps; ++yi) {
            const double dy = kVerticalDelta * yi / kVerticalSteps;
            for (int xi = -kHorizontalSteps; xi <= kHorizontalSteps; ++xi) {
                const double dx = halfWidth * xi / kHorizontalSteps;
                for (int zi = -kHorizontalSteps; zi <= kHorizontalSteps; ++zi) {
                    const double dz = halfWidth * zi / kHorizontalSteps;

                    const double distSqr = dx * dx + dy * dy + dz * dz;
                    if (bestDistSqr >= 0.0 && distSqr >= bestDistSqr) continue;

                    const glm::dvec3 candidate = preferred + glm::dvec3(dx, dy, dz);
                    if (BoxCollides(level, candidate, halfWidth, halfHeight)) continue;

                    bestDistSqr = distSqr;
                    best = candidate - glm::dvec3(0.0, halfHeight, 0.0);
                }
            }
        }

        // MC's orElse(bottomCenter): nothing free means the unadjusted point,
        // not a failure.
        return best;
    }

    // PortalShape.java:170
    void PortalShape::CreatePortalBlocks(ILevelWrite& level) const {
        // hush_portal aliases nether_portal's state set (gen_block_states
        // ALIAS_EXACT), so the same HORIZONTAL_AXIS write fits both.
        const BlockState portalState =
            BlockStates::Default(m_family->portalBlock)
                .SetName(PropertyId::HORIZONTAL_AXIS, NameOf(m_axis));

        // See the header for why the flags are MarkDirty and nothing else.
        for (int h = 0; h < m_height; ++h) {
            for (int w = 0; w < m_width; ++w) {
                const glm::ivec3 cell = Relative(
                    { m_bottomLeft.x, m_bottomLeft.y + h, m_bottomLeft.z }, m_rightDir, w);
                level.SetBlock(cell.x, cell.y, cell.z, portalState,
                               World::UpdateFlags::MarkDirty);
            }
        }
    }

} // namespace Game
