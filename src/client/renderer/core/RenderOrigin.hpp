// File: src/client/renderer/core/RenderOrigin.hpp
//
// Camera-relative rendering, Minecraft's way.
//
// A 32-bit float resolves 0.03 blocks at a coordinate of 300,000, so
// anything the GPU receives as an absolute world position — a section's
// origin, an entity's model matrix, a particle — snaps to a 3 cm grid and
// crawls as the camera moves. Minecraft never sends an absolute coordinate
// to the GPU: LevelRenderer translates every section, entity and block
// entity by (position - cameraPos) computed in DOUBLE (LevelRenderer.java:
// 1052, 1068, 1090), so the float the GPU sees is at most a render distance
// in size, with sub-millimetre precision at any distance from the origin.
//
// This engine does the same through ONE origin per view:
//
//   RenderOrigin()   the integer block position of the camera being drawn
//                    (floor of it). Integer so that fract(worldPos), which
//                    the terrain shaders use for texture tiling, is
//                    unchanged in render space.
//   ToRender(p)      p - RenderOrigin(), in double, then float: what every
//                    model matrix, vertex and clip plane handed to the GPU is
//                    built from.
//   Camera::GetViewMatrix()  is RENDER space: its translation is the
//                    camera's sub-block offset from the origin, never the
//                    camera's world position.
//   Terrain          keeps its section origins as INTEGERS and subtracts
//                    the origin in integer arithmetic inside the vertex
//                    shader (exact for any coordinate the world can hold),
//                    so no table is rewritten when the camera moves.
//   Culling          stays in WORLD space: Frustum objects are built from
//                    Camera::GetWorldViewMatrix(), whose float translation
//                    is the world position — imprecise far out, which only
//                    makes culling a few centimetres loose.
//
// The origin is per VIEW: PlatformMain sets it from the main camera each
// frame (Camera::PrepareRender), and a portal view sets it from its own far
// camera while it draws, so a view into a place a dimension's worth of
// blocks away is as exact as the main one.
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    inline glm::dvec3 g_renderOrigin{0.0};

    inline void SetRenderOrigin(const glm::dvec3& origin) { g_renderOrigin = origin; }
    inline const glm::dvec3& RenderOrigin() { return g_renderOrigin; }

    // The origin a camera at `cameraPos` uses: its block position.
    inline glm::dvec3 RenderOriginFor(const glm::dvec3& cameraPos) { return glm::floor(cameraPos); }

    // World -> render space. The subtraction is the precision-critical
    // step and is done in double; the float result is small.
    inline glm::vec3 ToRender(const glm::dvec3& world) { return glm::vec3(world - g_renderOrigin); }
    inline glm::vec3 ToRender(const glm::vec3& world)  { return glm::vec3(glm::dvec3(world) - g_renderOrigin); }
    inline glm::dvec3 ToWorld(const glm::vec3& render) { return glm::dvec3(render) + g_renderOrigin; }

    // A plane (n, w) with n·x + w = 0 in world space, expressed in render
    // space: x_w = x_r + R, so n·x_r + (w + n·R) = 0.
    // Take the plane in DOUBLE: its w is −n·point, a world-sized number
    // that a float has already rounded by 3 cm at x = 300,000.
    inline glm::vec4 PlaneToRender(const glm::dvec4& worldPlane) {
        const glm::dvec3 n(worldPlane.x, worldPlane.y, worldPlane.z);
        return glm::vec4(glm::vec3(n), static_cast<float>(worldPlane.w + glm::dot(n, g_renderOrigin)));
    }
    inline glm::vec4 PlaneToRender(const glm::vec4& worldPlane) { return PlaneToRender(glm::dvec4(worldPlane)); }

    // A render-space view matrix as a world-space one (for frustum planes):
    // view_render maps x_w - R, so view_world = view_render · T(-R).
    inline glm::mat4 WorldViewFromRenderView(const glm::mat4& renderView) {
        return renderView * glm::translate(glm::mat4(1.0f), -glm::vec3(g_renderOrigin));
    }
    inline glm::mat4 RenderViewFromWorldView(const glm::mat4& worldView) {
        return worldView * glm::translate(glm::mat4(1.0f), glm::vec3(g_renderOrigin));
    }

    // A view THROUGH a portal, in the far side's render space. The portal
    // renderers compose a far view as V_far = V_near · M⁻¹ (M: near → far,
    // world space, double). With both views camera-relative,
    //   V_far_render = V_near_render · T(−R_near) · M⁻¹ · T(R_far),
    // and the bracketed product is small — M⁻¹ carries R_far back to about
    // R_near — so it is exact when built in double and cast once. Building
    // V_near · M⁻¹ in float instead put a 300,000-block translation through
    // a float matrix product, which is the jitter this file exists to end.
    inline glm::mat4 FarRenderView(const glm::mat4& nearRenderView, const glm::dmat4& Minv,
                                   const glm::dvec3& nearOrigin, const glm::dvec3& farOrigin) {
        const glm::dmat4 bridge = glm::translate(glm::dmat4(1.0), -nearOrigin) * Minv *
                                  glm::translate(glm::dmat4(1.0), farOrigin);
        return nearRenderView * glm::mat4(bridge);
    }

} // namespace Render
