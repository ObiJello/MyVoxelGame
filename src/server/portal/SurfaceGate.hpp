// File: src/server/portal/SurfaceGate.hpp
//
// Where a portal built in a SURFACE world stands (today: the Hush side of an
// ancient-city frame, in both portal modes).
//
// The city frame is 22 wide and lives ~120 blocks under the Overworld; the
// Hush side must be the same frame, on the Hush's surface. The generic
// searches are built for a 4×5 nether frame: they look downward first, take
// "anywhere it fits" as a last resort (which on rolling terrain means carving
// a 22-block slot into a hillside), and the vanilla forcer only ever builds a
// 2×3 opening. This planner instead:
//
//   1. walks the loaded columns around the target and scores each centre by
//      how uneven the ground is under the frame's bottom row and the two rows
//      a traveller steps onto (plus a small distance term), taking the
//      flattest nearby spot;
//   2. stands the frame so its bottom row sits on the HIGHEST ground in that
//      footprint — never below the surface, so nothing is dug out;
//   3. under every cell of that bottom row, and under a one-block walkway on
//      each face, fills hushstone bricks down to the ground, so the gate
//      stands on a plinth instead of floating where the ground dips.
#pragma once

#include "common/world/portal/ImmersiveFrame.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace Game { class World; }

namespace Server::SurfaceGate {

    // The minCell a translated copy of `shape` should take so it stands on
    // the surface near `around` (x/z; y is ignored). nullopt when no column
    // of the footprint is in a loaded chunk, or the shape is a floor frame.
    std::optional<glm::ivec3> Plan(const Game::World& world, const Game::Immersive::FrameShape& shape,
                                   const glm::ivec3& around, int radius);

    // After the frame itself is built at `built`: the plinth under its
    // bottom row and the walkway in front of both faces.
    void BuildPlinth(Game::World& world, const Game::Immersive::FrameShape& built);

} // namespace Server::SurfaceGate
