// File: src/client/renderer/entity/EntityCulling.hpp
//
// The per-entity visibility decision every entity renderer shares. (The
// frame serial the streaming-buffer renderers key their double-buffering on
// is EntityFrame.hpp; its rationale is written up below so both live in one
// place.)
//
// ── Visibility — MC LevelRenderer.extractVisibleEntities:747-760 ─────────
//
// MC admits an entity to the frame when BOTH hold:
//
//   1. EntityRenderer.shouldRender: the culling box (getBoundingBoxForCulling,
//      inflated by 0.5) intersects the frustum. The inflate is what stops a
//      mob straddling a frustum plane from popping at the screen edge; the
//      degenerate-box fallback is a 2-block cube around the position.
//   2. isSectionCompiledAndVisible(blockPosition): the section the entity
//      stands in has a mesh (not UNCOMPILED). MC's own list is not consulted
//      there — vanilla does no occlusion culling of entities at all.
//
// This port keeps (1) verbatim and TIGHTENS (2) with the occlusion BFS: an
// entity counts as visible when any section its culling box overlaps is in
// the chunk renderer's draw list for this view. "Any overlapped section"
// rather than MC's blockPosition alone, because the BFS can drop the section
// under a mob's feet while the section its head is in stays visible — MC
// never faces that case since it does not BFS-cull entities, and keying on
// the feet alone would pop mobs at section boundaries.
//
// Sections the draw list can never contain are treated as visible: an
// unloaded chunk (MC returns false there, but this port's server drops
// entities the client has no chunk for, so nothing is lost and a
// mid-load mob does not vanish), and an ALL-AIR section — the draw list
// holds only non-air sections, so a phantom circling the sky or a mob mid-
// jump above a hilltop lives in a section that is not in the list even
// though its whole column is on screen.
//
// ── Frame serial ─────────────────────────────────────────────────────────
//
// The Vulkan backend's UpdateBuffer is a host-visible memcpy: the write lands
// immediately, the draws that read it run at submit. Two consequences the
// streaming renderers must design around:
//
//   * the previous frame's command buffer may still be executing while this
//     frame's CPU writes into the same buffer — so a buffer must not be
//     rewritten until the frame that last read it has retired (two sets,
//     alternated per FRAME, with two frames in flight — GuiRenderer's
//     pattern);
//   * within ONE frame, every draw sees the LAST write — so a renderer that is
//     called more than once per frame (the main pass, then once per portal
//     recursion level) must give each call its own RANGE of the frame's
//     buffer, not overwrite offset 0.
//
// Parity per call would satisfy neither once the portal pass makes three
// calls a frame. So the parity flips per FRAME and each call appends at a
// cursor that resets with it. The frame boundary comes from EntityFrame::
// Begin(), called once per frame from PlatformMain before the first entity
// pass; a renderer that sees the same serial twice is in the same frame and
// appends. If Begin is never called (serial stays 0) the renderers fall back
// to flipping per call, which is GuiRenderer's behaviour and correct for the
// once-per-frame case.
#pragma once

#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/core/Config.hpp"

#include <glm/glm.hpp>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Render {

    // EntityFrame lives in EntityFrame.hpp so the renderer HEADERS can hold a
    // Cursor without dragging the chunk renderer and chunk manager into
    // everything that includes them.

    namespace EntityCulling {
        // The portal CROSSING passes (ImmersivePortalRenderer) draw a level's
        // entities a second time, through a portal, clipped at its plane.
        // Only entities that actually intersect the surface belong in that
        // pass: without this filter everything BEHIND the near surface — a
        // herd standing beyond the portal in the same dimension — would be
        // painted into the far view. Set for the duration of such a pass,
        // null otherwise; every entity renderer asks it per entity.
        inline std::function<bool(const glm::vec3&, const glm::vec3&)> g_crossingFilter;
        inline bool PassesCrossingFilter(const glm::vec3& bmin, const glm::vec3& bmax) {
            return !g_crossingFilter || g_crossingFilter(bmin, bmax);
        }

        // ── Distance — MC Entity.shouldRenderAtSqrDistance ───────────────
        //
        // MC's other half of EntityRenderer.shouldRender. The cutoff scales
        // with the entity's size and a per-frame view scale:
        //
        //     size = boundingBox.getSize()        // mean of the three extents
        //     size *= 64.0 * viewScale
        //     return distSq < size * size         (Entity.java:1996-2003)
        //
        // and LevelRenderer sets the view scale once per frame (:754):
        //
        //     Entity.setViewScale(clamp(renderDistance / 8, 1, 2.5) * entityDistanceScaling)
        //
        // so a 1.8-tall zombie is drawn to 67 blocks on an 8-chunk view and
        // 168 on 32; a 0.7-tall chicken to 32 and 80. The Entity Distance
        // slider is that last factor, 50%..500%. PlatformMain publishes it
        // each frame from the EFFECTIVE render distance (the server may have
        // clamped what it sends) and the option.
        inline float g_viewScale = 1.0f;

        inline void SetViewScale(int effectiveRenderDistanceChunks, float entityDistanceScaling) {
            const float byDistance = std::clamp(static_cast<float>(effectiveRenderDistanceChunks) / 8.0f,
                                                1.0f, 2.5f);
            g_viewScale = byDistance * entityDistanceScaling;
        }
        inline float GetViewScale() { return g_viewScale; }

        // MC AABB.getSize() is the MEAN of the three extents; a box with a
        // NaN size counts as 1 (Entity.java:1998).
        inline bool ShouldRenderAtSqrDistance(double distSq, float width, float height) {
            double size = (static_cast<double>(width) * 2.0 + static_cast<double>(height)) / 3.0;
            if (std::isnan(size)) size = 1.0;
            size *= 64.0 * static_cast<double>(g_viewScale);
            return distSq < size * size;
        }
        inline bool ShouldRenderAtSqrDistance(const glm::vec3& cameraPos, const glm::vec3& pos,
                                              float width, float height) {
            const glm::dvec3 d = glm::dvec3(pos) - glm::dvec3(cameraPos);
            return ShouldRenderAtSqrDistance(glm::dot(d, d), width, height);
        }

        // MC EntityRenderer.shouldRender's frustum half: the feet-anchored
        // box of (width, height) around `pos`, inflated by 0.5, or the
        // 2-block fallback when the box is degenerate. `pos` is the
        // INTERPOLATED render position — culling on one position and drawing
        // at another is exactly the edge pop the inflate exists to prevent.
        inline bool BoxInFrustum(const Frustum& frustum, const glm::vec3& pos,
                                 float width, float height,
                                 glm::vec3* outMin = nullptr, glm::vec3* outMax = nullptr) {
            glm::vec3 bmin, bmax;
            const bool degenerate = !(width > 0.0f) || !(height > 0.0f)
                                 || std::isnan(width) || std::isnan(height);
            if (degenerate) {
                bmin = pos - glm::vec3(2.0f);
                bmax = pos + glm::vec3(2.0f);
            } else {
                const float hw = width * 0.5f + 0.5f;
                bmin = glm::vec3(pos.x - hw, pos.y - 0.5f,          pos.z - hw);
                bmax = glm::vec3(pos.x + hw, pos.y + height + 0.5f, pos.z + hw);
            }
            if (outMin) *outMin = bmin;
            if (outMax) *outMax = bmax;
            return frustum.TestAABB(bmin, bmax) != FrustumResult::Outside;
        }

        // The section gate described in the header, over the box the frustum
        // test used. True when no renderer/chunk manager is up (nothing to
        // gate against — draw, as before this existed).
        inline bool BoxTouchesVisibleSection(const glm::vec3& bmin, const glm::vec3& bmax) {
            const ChunkRenderer* renderer = g_chunkRenderer;
            const Client::ClientChunkManager* chunks = Client::g_clientChunkManager;
            if (!renderer || !chunks) return true;

            const int cx0 = static_cast<int>(std::floor(bmin.x)) >> 4;
            const int cx1 = static_cast<int>(std::floor(bmax.x)) >> 4;
            const int cz0 = static_cast<int>(std::floor(bmin.z)) >> 4;
            const int cz1 = static_cast<int>(std::floor(bmax.z)) >> 4;
            int sy0 = (static_cast<int>(std::floor(bmin.y)) - Config::MinY) >> 4;
            int sy1 = (static_cast<int>(std::floor(bmax.y)) - Config::MinY) >> 4;
            // Outside the build height, MC's isOutsideBuildHeight branch: not
            // gated at all. Clamp the in-range part and let the rest pass.
            if (sy1 < 0 || sy0 >= Game::Math::SECTIONS_PER_CHUNK) return true;
            sy0 = sy0 < 0 ? 0 : sy0;
            sy1 = sy1 >= Game::Math::SECTIONS_PER_CHUNK ? Game::Math::SECTIONS_PER_CHUNK - 1 : sy1;

            for (int cx = cx0; cx <= cx1; ++cx) {
                for (int cz = cz0; cz <= cz1; ++cz) {
                    const Game::Math::ChunkPos cp{ cx, cz };
                    for (int sy = sy0; sy <= sy1; ++sy) {
                        const Client::SectionInfo* si = chunks->GetSectionInfo(cp, sy);
                        // No chunk, or a section the draw list never lists.
                        if (!si || si->isAllAir) return true;
                        if (renderer->IsSectionVisible(cp, sy)) return true;
                    }
                }
            }
            return false;
        }

        // Both halves. The one call the entity renderers make per entity.
        inline bool ShouldRender(const Frustum& frustum, const glm::vec3& pos,
                                 float width, float height) {
            glm::vec3 bmin, bmax;
            if (!BoxInFrustum(frustum, pos, width, height, &bmin, &bmax)) return false;
            // A crossing pass draws only the entities standing in a portal
            // surface, and the surface being drawn is what proves they are
            // in view. The section gate answers for the OTHER level's view
            // (the bound level's renderer holds that level's last pass),
            // so it is not asked here.
            if (g_crossingFilter) return true;
            return BoxTouchesVisibleSection(bmin, bmax);
        }

    } // namespace EntityCulling

} // namespace Render
