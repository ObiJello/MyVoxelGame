// File: src/client/renderer/debug/Gizmos.hpp
//
// World-space debug primitives — this engine's MC 26.3 Gizmos (line, cuboid,
// rect, point, arrow, billboardText) plus the block-outline edge walk. The
// debug renderers queue shapes every frame; the frame flushes them after
// the world and before the HUD.
//
// LINES ARE DRAWN MINECRAFT'S WAY: through a port of rendertype_lines.vsh.
// Each line is a strip of two triangles whose vertices carry the line
// direction; the vertex shader projects both ends, takes the screen-space
// perpendicular and pushes each vertex ±LineWidth/2 framebuffer pixels
// along it. Width is constant on screen at any distance, thin lines stay
// thin, and the whole thing is one indexed draw per width. The model-view
// is scaled by MC's VIEW_SHRINK (1 - 1/256, in the shader) and the LINES
// render type's VIEW_OFFSET_Z_LAYERING (1 - 1/4096, baked into the MVP),
// which is what keeps a line on a block face from z-fighting with it.
// Opaque lines write depth (RenderTypes.lines); lines with alpha do not
// (linesTranslucentNoDepthWrite); "always on top" lines skip the test.
//
// Default widths are MC's: Gizmos.line = 3 px, stroke() = 2.5 px,
// arrow = 2.5 px. Fills are blended, two-sided triangles.
//
// Text billboards are queued for the HUD pass (DebugScreen::QueueBillboardText).
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Game { enum class Direction : uint8_t; }

namespace Render::Gizmos {

    bool Initialize();
    void Shutdown();

    // `depthBias` is MC's LINES_DEPTH_BIAS pipeline: the line is nudged
    // toward the camera in the depth test so it wins over a line drawn at
    // the SAME depth just before it — the 3D crosshair's coloured axes over
    // their black underlays. Without it an axis pointing along the view
    // direction z-fights its underlay and flickers.
    void Line(const glm::dvec3& a, const glm::dvec3& b, uint32_t argb, float widthPx = 3.0f, bool alwaysOnTop = false,
              bool depthBias = false);
    // 12 edges of the box [min, max] (MC GizmoStyle.stroke default width).
    void Cuboid(const glm::dvec3& min, const glm::dvec3& max, uint32_t stroke, float widthPx = 2.5f, bool alwaysOnTop = false);
    // 6 faces of the box, filled (blended, two-sided).
    void CuboidFill(const glm::dvec3& min, const glm::dvec3& max, uint32_t fill);
    // One face of the box [a, b] — MC Gizmos.rect(cornerA, cornerB, direction, fill).
    void Rect(const glm::dvec3& a, const glm::dvec3& b, Game::Direction face, uint32_t fill);
    // An arbitrary filled quad.
    void Quad(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, uint32_t fill);
    // A screen-sized dot at a point (MC Gizmos.point).
    void Point(const glm::dvec3& p, uint32_t argb, float sizePx);
    // A line with a small head at `to` (MC Gizmos.arrow, 2.5 px).
    void Arrow(const glm::dvec3& from, const glm::dvec3& to, uint32_t argb, float widthPx = 2.5f);
    // Text facing the camera at a world point (drawn in the HUD pass).
    void BillboardText(const std::string& text, const glm::dvec3& pos, uint32_t argb,
                       float scale = 1.0f, bool centered = true, bool alwaysOnTop = false);

    // MC ShapeRenderer.renderShape / VoxelShape.forAllEdges over a block's
    // shape boxes: the boxes are rasterised onto the grid of their own
    // coordinates and only the edges of the UNION are emitted — a stair
    // gets its L profile, not two boxes with a seam. Segments along one
    // grid line are merged (mergeNeighbors). Widths in framebuffer pixels.
    struct ShapeBox { glm::vec3 min, max; };
    void ShapeOutline(const ShapeBox* boxes, size_t count, const glm::dvec3& origin,
                      uint32_t argb, float widthPx);

    // Draw and clear everything queued. fbWidth/fbHeight are the
    // framebuffer size (MC ScreenSize); fovDeg is unused by the line
    // shader and kept for the fills.
    void Flush(const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos,
               float fovDeg, int fbWidth, int fbHeight);
    void Clear();
    size_t QueuedVertexCount();

} // namespace Render::Gizmos
