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

    // Build line geometry (body, limbs, head outline, face features) into `lineVerts`,
    // and triangle geometry (filled back-of-head disc) into `triVerts`. Vertices come
    // out as line-pair lists and triangle-list, both in world space relative to
    // `feetPos` (so feet sit at feetPos and head at feetPos + Y*~1.62m).
    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& triVerts,
                          const glm::vec3& feetPos,
                          float headYawDeg, float bodyYawDeg,
                          float pitchDeg, bool isCrouching);

} // namespace Render
