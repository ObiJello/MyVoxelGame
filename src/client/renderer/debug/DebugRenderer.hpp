// File: src/client/renderer/debug/DebugRenderer.hpp
//
// The F3 world-space debug renderers — MC 26.3's client/renderer/debug
// (DebugRenderer + the SimpleDebugRenderers the F3 entries switch on) and
// DebugCrosshairRenderer, drawn with Gizmos. Which renderers run follows
// the enabled entries (DebugScreenEntries), re-read whenever the entry
// list's version changes, exactly like MC's refreshRendererList.
//
//   chunk_borders                   ChunkBorderRenderer
//   entity_hitboxes                 EntityHitboxDebugRenderer
//   3d_crosshair                    DebugCrosshairRenderer
//   chunk_section_paths/visibility  ChunkCullingDebugRenderer (+ captured frustum)
//   chunk_section_octree            the frustum grid's visible sections
//   visualize_water_levels          WaterDebugRenderer
//   visualize_heightmap             HeightMapRenderer
//   visualize_collision_boxes       CollisionBoxRenderer
//   visualize_entity_supporting_blocks  SupportBlockRenderer
//   visualize_block/sky_light_levels    LightDebugRenderer
//   visualize_solid_faces           SolidFaceRenderer
//   visualize_chunks_on_server      ChunkDebugRenderer (the per-chunk labels)
//   visualize_sky_light_sections    LightSectionDebugRenderer (no light engine: nothing)
#pragma once

#include <glm/glm.hpp>

namespace Game { class ClientPlayer; }

namespace Render {

    class Camera;

    namespace DebugRenderer {

        struct FrameArgs {
            const Game::ClientPlayer* player = nullptr;
            const Camera* camera = nullptr;
            glm::mat4 proj{1.0f};
            glm::mat4 view{1.0f};
            glm::vec3 cameraPos{0.0f};
            bool firstPerson = true;
            int guiScale = 1;
            float fovDeg = 70.0f;
            int fbWidth = 0, fbHeight = 0;
            // How far through the current client tick the frame is (0..1):
            // entity boxes follow the interpolated render position, as MC's do.
            float partialTick = 0.0f;
        };

        // Queue every enabled renderer's shapes for this frame and flush them.
        // Called after the world (terrain, entities, block highlight) and
        // before the HUD.
        void RenderWorld(const FrameArgs& args);

        // MC Camera.captureFrustum / killFrustum — freezes a copy of the main
        // view's frustum for ChunkCullingDebugRenderer to draw.
        void CaptureFrustum(const glm::mat4& proj, const glm::mat4& view, const glm::vec3& cameraPos);
        void KillFrustum();
        bool HasCapturedFrustum();

        // True when any world-space renderer is enabled (the caller may skip
        // the whole pass otherwise).
        bool AnyEnabled();

        // Session end: drop caches.
        void Reset();

    } // namespace DebugRenderer
} // namespace Render
