// File: src/common/world/portal/ImmersiveFrame.hpp
//
// A portal frame as the Immersive Portals mod sees it: ANY closed loop of
// frame blocks around a flat pocket of air, in any of the three planes —
// not vanilla's rectangle (PortalShape.hpp). The mod's BlockPortalShape.
//
// Which block closes the loop is the frame's PortalFamily (PortalFamily
// .hpp): obsidian for a nether frame, reinforced deepslate for a Hush
// frame. A shape carries its family from the flood fill on, so everything
// downstream — matching a far frame, building one, the integrity sweep —
// asks the same predicate the fill did.
//
// Found by flood-filling from the ignition cell in the two in-plane
// directions: air spreads, frame blocks stop, anything else means the loop
// is not closed. The `area` (air cells) becomes the portal surface, one
// block each; the `frame` (the area's frame-block 4-neighbours in the
// plane) is what integrity checks watch.
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
#include "common/world/portal/PortalFamily.hpp"

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
        // Which frame material closed the loop, and so which portal block,
        // build block and immersive kind belong to it.
        PortalFamilyId family = PortalFamilyId::Nether;
        std::vector<glm::ivec3> area;    // air cells inside the loop
        std::vector<glm::ivec3> frame;   // frame cells bounding them (no corners)
        glm::ivec3 minCell{0};           // bounds of `area`
        glm::ivec3 maxCell{0};

        const PortalFamily& Family() const { return Game::Family(family); }
        // The family's immersive frame predicate — what the fill stopped on.
        bool IsFrameBlock(BlockID id) const { return Family().isImmersiveFrame(id); }

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
        // Flood-fill from an air (or fire) cell, closed by `family`'s frame
        // block. Tries X, then Y, then Z — the mod's order — and returns
        // the first closed loop.
        static std::optional<FrameShape> Find(const IBlockAccess& level, const glm::ivec3& start,
                                              PortalFamilyId family,
                                              int lengthLimit = kDefaultLengthLimit,
                                              int areaLimit = kDefaultAreaLimit);
        static std::optional<FrameShape> FindOnAxis(const IBlockAccess& level, const glm::ivec3& start,
                                                    PortalFamilyId family, Axis axis,
                                                    int lengthLimit, int areaLimit);

        // Frame all frame blocks and area all air (fire counts as air)?
        bool IsIntact(const IBlockAccess& level) const;
        // Would the frame be intact if translated so that minCell lands on
        // `newMin`? (Frame blocks and area air at the moved cells.)
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
        // The cells one block in front of and behind every interior cell.
        // A FLOOR frame (axis Y) instead reserves kFallClearance cells
        // BELOW every interior cell, so the far side of a horizontal portal
        // is a hole and not a window: the crossing fires when the EYE goes
        // through, 1.62 blocks after the feet, and a frame lying flat on
        // the ground holds the body up on the far ground long before that
        // (the mod reserves vertical space the same way). Two cells is the
        // pair that works both ways — a body drops in far enough for the
        // eye to cross, and a jump from the ground beneath still lifts the
        // eye back up through the surface (three would not).
        static constexpr int kFallClearance = 2;
        std::vector<glm::ivec3> Clearance() const;

        // The two families' frame blocks by name, for callers that know
        // which one they mean; IsFrameBlock is the family-agnostic form.
        static bool IsObsidian(BlockID id);
        static bool IsReinforcedDeepslate(BlockID id);
        static bool IsAirLike(BlockID id);   // air, fire, or a family portal block
    };

    // Recover the frame a nether/hush portal record was made from (its cells
    // are the shape's quads, its plane the axes, its family the kind), so an
    // integrity check after a reload needs no separate save. Empty when the
    // record is not a frame-shaped surface.
    std::optional<FrameShape> FrameFromPortal(const Portal& portal);

} // namespace Game::Immersive

#endif // ENABLE_IMMERSIVE_PORTALS
