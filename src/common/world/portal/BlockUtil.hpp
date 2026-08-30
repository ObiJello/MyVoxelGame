// File: src/common/world/portal/BlockUtil.hpp
//
// Port of the one function this engine needs from
// net.minecraft.util.BlockUtil: getLargestRectangleAround.
//
// WHY A PORTAL NEEDS THIS AT ALL
// ------------------------------
// A nether portal's interior is not necessarily a rectangle by the time you
// walk into it — a player can mine part of the obsidian frame and leave an
// L-shaped hole full of portal blocks, and MC's `updateShape` only deletes
// portal blocks whose own frame walk fails. So "where inside the portal am I,
// proportionally" and "where inside the DESTINATION portal should I come out"
// are both asked against the largest axis-aligned rectangle of matching
// blocks around a point, not against the shape that was originally lit.
//
// Keeping vanilla's algorithm matters for parity: the exit offset is derived
// from the entry rectangle's size, so a different rectangle puts the player
// somewhere else. It is the maximal-rectangle-in-histogram problem run once
// per column, exactly as BlockUtil.java does it, stack and all.
//
// It lives under world/portal rather than a general util folder because the
// portal classes are its only callers here, as in vanilla.
#pragma once

#include "common/world/block/Direction.hpp"

#include <functional>
#include <glm/glm.hpp>

namespace Game {

    // BlockUtil.FoundRectangle. `minCorner` is the low corner on BOTH axes;
    // axis1Size/axis2Size are extents along the axes passed to the finder, in
    // that order.
    struct FoundRectangle {
        glm::ivec3 minCorner{0, 0, 0};
        int axis1Size = 0;
        int axis2Size = 0;
    };

    // BlockUtil.java:16. `test` answers "does this position belong to the
    // region", `limit1`/`limit2` bound how far the search walks along each
    // axis (portals pass 21, the maximum portal dimension).
    //
    // The returned rectangle always contains `center` — a center that fails
    // `test` yields a 1x1 at `center`, which is what the callers rely on when
    // the world changed under them.
    FoundRectangle GetLargestRectangleAround(
        const glm::ivec3& center,
        Axis axis1, int limit1,
        Axis axis2, int limit2,
        const std::function<bool(const glm::ivec3&)>& test);

} // namespace Game
