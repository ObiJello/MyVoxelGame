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
#pragma once

#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>

namespace Render {

    class XpOrbRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw every orb in Client::g_xpOrbManager, plus the pickup flights.
        // `partialTick` blends previous/current tick positions and advances
        // the colour pulse within a tick, same contract as the item renderer.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

    private:
        void DrawOrb(int value, const glm::vec3& worldPos, float ageTicks,
                     const glm::mat4& viewProj, const glm::mat3& billboard);

        bool m_initialized = false;

        ShaderHandle  m_shader  = INVALID_SHADER;
        TextureHandle m_texture = INVALID_TEXTURE;

        BufferHandle m_vb   = INVALID_BUFFER;
        BufferHandle m_ib   = INVALID_BUFFER;
        MeshHandle   m_mesh = INVALID_MESH;

        // MC shouldRenderAtSqrDistance for a 0.5-cube entity: mean extent 0.5
        // × 64 = 32 blocks, matching the item renderer's derivation.
        static constexpr float kMaxRenderDistance = 0.5f * 64.0f;
    };

} // namespace Render
