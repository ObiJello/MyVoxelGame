// File: src/common/world/portal/PortalShape.hpp
//
// Port of MC's net.minecraft.world.level.portal.PortalShape.
//
// This is the geometry half of the nether portal: given a position and a
// preferred axis, walk the obsidian frame around it and report the interior
// rectangle. Two callers need it and they want opposite answers:
//
//   * lighting a fire asks findEmptyPortalShape — "is there a complete frame
//     here whose interior is entirely EMPTY", i.e. a portal waiting to be lit;
//   * a nether_portal block whose neighbour changed asks findAnyShape(...)
//     .isComplete() — "is the frame around me still whole AND still filled
//     with portal blocks", i.e. am I allowed to keep existing.
//
// Both go through the same walk, which is why MC keeps one class. The only
// difference is the predicate applied to the result, so this port keeps that
// shape rather than splitting it into two functions that would drift.
//
// WHAT COUNTS AS WHAT (PortalShape.java:31, :162)
//   FRAME   — obsidian, and only obsidian.
//   "empty" — air, fire (either kind), or an existing nether_portal block.
//             Fire is in the set because the frame is lit BY placing fire in
//             it: the fire block is already there when the search runs.
//
// The 2/21 width and 3/21 height bounds are vanilla's and are what make a
// 4x5 frame the smallest portal and a 23x23 frame the largest.
//
// This file is DIMENSION-AGNOSTIC and side-agnostic — it reads through
// IBlockAccess and writes through ILevelWrite, so the client's predicted
// world can run the same search the server does.
#pragma once

#include "BlockUtil.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <functional>
#include <optional>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;

    class PortalShape {
    public:
        // PortalShape.java:27-30. MIN/MAX are on the PORTAL INTERIOR, not the
        // frame: the 2x3 minimum is the classic 4x5 obsidian rectangle.
        static constexpr int kMinWidth  = 2;
        static constexpr int kMaxWidth  = 21;
        static constexpr int kMinHeight = 3;
        static constexpr int kMaxHeight = 21;

        // PortalShape.java:50 — a complete frame with nothing in it yet. This
        // is the flint-and-steel path.
        static std::optional<PortalShape> FindEmptyPortalShape(
            const IBlockAccess& level, const glm::ivec3& pos, Axis preferredAxis);

        // PortalShape.java:54 — try `preferredAxis` first, then the other one,
        // returning the first shape the predicate accepts. Both axes are
        // searched because a fire lit against a frame does not say which way
        // the frame runs.
        static std::optional<PortalShape> FindPortalShape(
            const IBlockAccess& level, const glm::ivec3& pos,
            const std::function<bool(const PortalShape&)>& isValid,
            Axis preferredAxis);

        // PortalShape.java:64 — the raw walk on one axis. Always returns a
        // shape; an invalid one has width or height 0. Callers ask IsValid()
        // or IsComplete() rather than checking for a null.
        static PortalShape FindAnyShape(const IBlockAccess& level,
                                        const glm::ivec3& pos, Axis axis);

        // PortalShape.java:166 — the frame is whole and the interior is within
        // bounds. Says nothing about what is IN the interior.
        bool IsValid() const {
            return m_width  >= kMinWidth  && m_width  <= kMaxWidth
                && m_height >= kMinHeight && m_height <= kMaxHeight;
        }

        // PortalShape.java:175 — valid AND every interior cell already holds a
        // nether_portal block. This is the "may I keep existing" test.
        bool IsComplete() const {
            return IsValid() && m_numPortalBlocks == m_width * m_height;
        }

        // PortalShape.java:170 — fill the interior with nether_portal blocks
        // oriented along this shape's axis.
        //
        // MC passes flag 18 = UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE. The second
        // bit is load-bearing and not cosmetic: without it, writing the first
        // portal block runs updateShape on its neighbours, NetherPortalBlock
        // .updateShape finds an incomplete portal (the rest is not placed yet)
        // and deletes the block that was just written. The engine equivalent
        // is "MarkDirty only" — no NotifyNeighbors, no UpdateShapes — which
        // still reaches the change accumulator and so still reaches clients.
        void CreatePortalBlocks(ILevelWrite& level) const;

        Axis       GetAxis()    const { return m_axis; }
        glm::ivec3 BottomLeft() const { return m_bottomLeft; }
        int        Width()      const { return m_width; }
        int        Height()     const { return m_height; }

        // PortalShape.java:162 — air, fire, soul fire, or nether portal.
        // Public because BaseFireBlock's placement test needs the same notion
        // of "inside a frame" and NetherPortalBlock reuses it.
        static bool IsEmptyForPortal(BlockState state);

        // PortalShape.java:31 — FRAME. Obsidian only; crying obsidian does
        // NOT work in vanilla and must not work here.
        static bool IsFrame(BlockState state) {
            return state.Is(BlockID::Obsidian);
        }

        // PortalShape.java:179 getRelativePosition — where inside a portal an
        // entity is standing, as fractions.
        //
        // This is what makes a portal preserve WHERE you walked in: the
        // returned .x is how far across the opening you were (0 = the left
        // edge, 1 = the right), .y how far up, and .z your signed offset in
        // front of or behind the portal plane in blocks. The destination
        // reconstructs a position from the same three numbers against ITS
        // opening, so stepping into the left side of a wide portal puts you
        // out of the left side of the exit.
        //
        // `rect` is the largest rectangle of portal blocks around the entry
        // cell (BlockUtil::GetLargestRectangleAround), `axis` the portal's
        // horizontal axis, `dims` the entity's (width, height) in blocks.
        static glm::dvec3 GetRelativePosition(const FoundRectangle& rect,
                                              Axis axis, const glm::dvec3& position,
                                              float entityWidth, float entityHeight);

        // PortalShape.java:204 findCollisionFreePosition — nudge the arrival
        // point until the entity's box is clear of blocks.
        //
        // MC expresses this as VoxelShape algebra: build the set of allowed
        // CENTRES (a width x 1 x width box at the entity's middle, expanded one
        // block upward), subtract every block collision inflated by half the
        // entity size, and take the closest surviving point to the preferred
        // centre. This engine has no shape boolean ops, so the same set is
        // searched by sampling — the answer is MC's to within the sample step,
        // and the search volume is deliberately as small as vanilla's: half an
        // entity width sideways and at most one block up. It is a nudge, not a
        // rescue, and MC's own answer when nothing is free is the unadjusted
        // input.
        //
        // Matters most in creative, where the transition delay is zero and the
        // teleport fires on the first tick of contact — the moment when the
        // entity is least likely to be squarely inside the portal.
        static glm::dvec3 FindCollisionFreePosition(const IBlockAccess& level,
                                                    const glm::dvec3& bottomCenter,
                                                    float entityWidth, float entityHeight);

    private:
        PortalShape() = default;
        PortalShape(Axis axis, int portalBlockCount, Direction rightDir,
                    const glm::ivec3& bottomLeft, int width, int height)
            : m_axis(axis), m_rightDir(rightDir), m_numPortalBlocks(portalBlockCount),
              m_bottomLeft(bottomLeft), m_height(height), m_width(width) {}

        // PortalShape.java:81 — drop to the floor of the cavity, then walk
        // left along the bottom row to the frame. Returns false when there is
        // no frame corner to anchor on.
        static bool CalculateBottomLeft(const IBlockAccess& level, Direction rightDir,
                                        glm::ivec3 pos, glm::ivec3& out);

        // PortalShape.java:95 — how far you can travel in `direction` while
        // staying in the cavity with frame underneath, stopping ON the frame
        // block that closes it. 0 means "no closing frame within 21".
        static int DistanceUntilEdgeAboveFrame(const IBlockAccess& level,
                                               const glm::ivec3& pos,
                                               Direction direction);

        static int CalculateWidth(const IBlockAccess& level, const glm::ivec3& bottomLeft,
                                  Direction rightDir);
        static int CalculateHeight(const IBlockAccess& level, const glm::ivec3& bottomLeft,
                                   Direction rightDir, int width, int& portalBlockCount);
        static int DistanceUntilTop(const IBlockAccess& level, const glm::ivec3& bottomLeft,
                                    Direction rightDir, int width, int& portalBlockCount);
        static bool HasTopFrame(const IBlockAccess& level, const glm::ivec3& bottomLeft,
                                Direction rightDir, int width, int height);

        Axis       m_axis            = Axis::X;
        Direction  m_rightDir        = Direction::West;
        int        m_numPortalBlocks = 0;
        glm::ivec3 m_bottomLeft{0, 0, 0};
        int        m_height          = 0;
        int        m_width           = 0;
    };

} // namespace Game
