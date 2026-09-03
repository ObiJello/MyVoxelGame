// File: src/common/world/portal/ImmersiveFrame.hpp
//
// An obsidian frame as the Immersive Portals mod sees it: ANY closed loop
// of obsidian around a flat pocket of air, in any of the three planes —
// not vanilla's rectangle (PortalShape.hpp). The mod's BlockPortalShape.
//
// Found by flood-filling from the ignition cell in the two in-plane
// directions: air spreads, obsidian stops, anything else means the loop is
// not closed. The `area` (air cells) becomes the portal surface, one block
// each; the `frame` (the area's obsidian 4-neighbours in the plane) is what
// integrity checks watch.
//
// Plane conventions (the portal's own axes, see ImmersivePortal.hpp):
//   axis X (plane YZ): axisW = +Z, axisH = +Y   → normal −X
//   axis Y (plane XZ): axisW = +X, axisH = +Z   → normal −Y (a floor portal)
//   axis Z (plane XY): axisW = +X, axisH = +Y   → normal +Z
// The flipped twin faces the other way, so which side is "front" does not
// matter to the player.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Game { struct IBlockAccess; }

namespace Game::Immersive {

    struct FrameShape {
        // The mod's defaults: a loop no longer than this on either side,
        // and no more air than this inside it.
        static constexpr int kDefaultLengthLimit = 64;
        static constexpr int kDefaultAreaLimit   = 1024;

        Axis axis = Axis::Z;
        std::vector<glm::ivec3> area;    // air cells inside the loop
        std::vector<glm::ivec3> frame;   // obsidian cells bounding them (no corners)
        glm::ivec3 minCell{0};           // bounds of `area`
        glm::ivec3 maxCell{0};

        bool  Empty() const { return area.empty(); }
        // Every cell of the bounding rectangle is in `area`.
        bool  IsRectangle() const;
        // In-plane extents of the bounds, in blocks: (along axisW, along axisH).
        glm::ivec2 Size() const;
        // The corner cells too — needed when BUILDING a frame, not when
        // checking one (vanilla and the mod both ignore corners on check).
        std::vector<glm::ivec3> FrameWithCorners() const;

        // The surface's frame vectors for this plane (see file comment).
        void PlaneAxes(glm::dvec3& axisW, glm::dvec3& axisH) const;
        // Centre of the bounds, on the mid-plane of the blocks.
        glm::dvec3 Center() const;

        // Fill a portal record's geometry from this frame: origin, axes,
        // size, and a Mesh shape (one quad per cell) unless it is a plain
        // rectangle. Dimension/destination are the caller's.
        void FillPortal(Portal& portal) const;

        // Is a block part of this frame or its interior?
        bool ContainsFrameCell(const glm::ivec3& pos) const;
        bool ContainsAreaCell(const glm::ivec3& pos) const;

        // Same shape moved by `delta`.
        FrameShape Translated(const glm::ivec3& delta) const;

        // ── Against a world ─────────────────────────────────────────────
        // Flood-fill from an air (or fire) cell. Tries X, then Y, then Z —
        // the mod's order — and returns the first closed loop.
        static std::optional<FrameShape> Find(const IBlockAccess& level, const glm::ivec3& start,
                                              int lengthLimit = kDefaultLengthLimit,
                                              int areaLimit = kDefaultAreaLimit);
        static std::optional<FrameShape> FindOnAxis(const IBlockAccess& level, const glm::ivec3& start,
                                                    Axis axis, int lengthLimit, int areaLimit);

        // Frame all obsidian and area all air (fire counts as air)?
        bool IsIntact(const IBlockAccess& level) const;
        // Would the frame be intact if translated so that minCell lands on
        // `newMin`? (Frame obsidian and area air at the moved cells.)
        bool MatchesAt(const IBlockAccess& level, const glm::ivec3& newMin) const;
        // Room to BUILD it here: frame-with-corners and area are all air
        // (or replaceable), the row under the bottom frame is solid, and —
        // with `requireClearance` — the cells one block in front of and
        // behind the interior are air too. MC's PortalForcer prefers a spot
        // with that clearance (canHostFrame at perpendicular offsets ±1)
        // and settles for one without; an immersive portal built flush
        // against netherrack is a portal nobody can step through, since the
        // wall behind its far face IS the collision on the near side.
        bool CanBuildAt(const IBlockAccess& level, const glm::ivec3& newMin, bool requireGround,
                        bool requireClearance = false) const;
        // The cells one block in front of and behind every interior cell
        // (above only, for a floor frame).
        std::vector<glm::ivec3> Clearance() const;

        static bool IsObsidian(BlockID id);
        static bool IsAirLike(BlockID id);   // air or fire
    };

    // Recover the frame a nether portal record was made from (its cells are
    // the shape's quads, its plane the axes), so an integrity check after a
    // reload needs no separate save. Empty when the record is not a
    // nether-portal-shaped surface.
    std::optional<FrameShape> FrameFromPortal(const Portal& portal);

} // namespace Game::Immersive

#endif // ENABLE_IMMERSIVE_PORTALS
