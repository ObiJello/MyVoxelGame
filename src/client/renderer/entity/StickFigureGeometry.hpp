// File: src/client/renderer/entity/StickFigureGeometry.hpp
//
// Pose-driven stick-figure geometry builder. Used by:
//   - PlayerRenderer (renders remote players in the world via GPU lines/triangles)
//   - PlayerInventoryPreview (renders the local player in the inventory's preview
//     box via CPU-projected QuadCommands)
//
// Vertex layout matches the block vertex layout (pos3 + uv2 + color4 ubyte = 24 B)
// so the world renderer can stream it straight into a GPU buffer without copies.
// The UV slots are unused but kept for layout compatibility.
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Render {

    struct StickVertex {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(StickVertex) == 24, "StickVertex must match block vertex stride");

    // Per-player colour for the stick figure. Defaults to the historical neon
    // green so single-player and any callers that don't pass a colour see the
    // same look they always have.
    struct PlayerColor {
        uint8_t r = 0;
        uint8_t g = 255;
        uint8_t b = 60;
        uint8_t a = 255;
    };
    inline constexpr PlayerColor kDefaultPlayerColor{0, 255, 60, 255};

    // Build geometry for a stick-figure player at the given pose. Output goes
    // into three lists:
    //
    //   lineVerts: body, limbs, eyes — line-pair list. Caller decides thickness
    //              (PlayerRenderer expands to camera-facing thick world strips,
    //              PlayerInventoryPreview projects + emits 1 px screen quads).
    //   ringTris : front-face outline + smile, as filled annular ring triangles
    //              in the head's local plane. Triangle list. Same colour as
    //              lineVerts. Wound CCW from the head's lookDir so back-face
    //              culling hides it when viewing the player from behind.
    //   discTris : filled back-of-head disc. Triangle list. Wound CCW from
    //              -lookDir so back-face culling hides it from in front.
    //
    // Splitting ring vs disc lets the inventory preview render only the ring
    // (it always views the player from the front and shouldn't see the disc),
    // while the world renderer batches both into one draw call.
    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& ringTris,
                          std::vector<StickVertex>& discTris,
                          const glm::vec3& feetPos,
                          float headYawDeg, float bodyYawDeg,
                          float pitchDeg, bool isCrouching,
                          PlayerColor color = kDefaultPlayerColor);

} // namespace Render
