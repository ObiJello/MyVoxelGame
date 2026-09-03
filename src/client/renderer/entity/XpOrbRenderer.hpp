// File: src/client/renderer/entity/XpOrbRenderer.hpp
//
// Draws experience orbs — MC's ExperienceOrbRenderer.
//
// An orb is a single camera-facing quad textured from
// assets/textures/entity/experience_orb.png (a 4×4 grid of 16 px sprites,
// picked by the orb's value) and pulsed between green and yellow-white by a
// pair of phase-shifted sine waves on the vertex colour. Same shader and
// environment (fog, sky brightness) as ItemEntityRenderer so orbs fade into
// the world exactly like the items lying next to them.
//
// ── Why one buffer for all orbs, one draw ─────────────────────────────────
//
// Every orb shares the one sheet, so the whole set is a single texture batch:
// the quads are built in world space on the CPU and drawn with one indexed
// call, the MobRenderer pattern. The previous design streamed ONE 4-vertex
// buffer and drew it once per orb — on Vulkan the backend's UpdateBuffer is
// an immediate host memcpy and the draws run at submit, so every orb on
// screen rendered the LAST orb's quad (same sprite, same colour phase, same
// position). Batching is the fix as well as the optimisation.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert (24-byte block layout)
#include "EntityFrame.hpp"
#include <glm/glm.hpp>
#include <cstddef>
#include <vector>

struct Frustum;

namespace Render {

    class XpOrbRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw every orb in Client::g_xpOrbManager, plus the pickup flights.
        // `partialTick` blends previous/current tick positions and advances
        // the colour pulse within a tick, same contract as the item renderer.
        // `frustum` is this view's (main or portal recursion) — the MC
        // shouldRender AABB test plus the visible-section gate, see
        // EntityCulling.hpp.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    float partialTick);

    private:
        // Append one orb's world-space quad to m_verts.
        void AppendOrb(int value, const glm::vec3& worldPos, float ageTicks,
                       const glm::mat3& billboard);

        bool m_initialized = false;

        ShaderHandle  m_shader  = INVALID_SHADER;
        TextureHandle m_texture = INVALID_TEXTURE;

        // Two streaming vertex sets alternated per FRAME, each call in a
        // frame (main pass, portal recursions) appending at a cursor — see
        // EntityFrame.hpp. The index buffer is static: the quad pattern
        // 0,1,2,0,2,3 repeated kMaxOrbs times, so orb k's vertices at
        // 4k..4k+3 are addressed by indices 6k..6k+5 and a call's draw is
        // an index range at its vertex cursor.
        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        BufferHandle m_ib = INVALID_BUFFER;
        EntityFrame::Cursor m_frameCursor;
        size_t m_orbCursor = 0;   // orbs already written this frame

        // Per set. MC caps orbs hard through merging (ExperienceOrb.tryMerge)
        // and the 32-block cull below; a few thousand on screen is a mass
        // death of a huge farm, past which the remainder skip a frame.
        static constexpr size_t kMaxOrbs = 4096;

        // Reused across frames so a steady-state frame allocates nothing.
        std::vector<ItemCubeVert> m_verts;

        // MC shouldRenderAtSqrDistance for a 0.5-cube entity: mean extent 0.5
        // × 64 = 32 blocks, times EntityCulling's per-frame view scale (the
        // render-distance term and the Entity Distance option) at the call.
        static constexpr float kMaxRenderDistance = 0.5f * 64.0f;
    };

} // namespace Render
