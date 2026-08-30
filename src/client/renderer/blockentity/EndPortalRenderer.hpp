// File: src/client/renderer/blockentity/EndPortalRenderer.hpp
//
// Draws BlockID::EndPortal — MC's TheEndPortalRenderer /
// AbstractEndPortalRenderer.
//
// The end portal's block model (assets/models/block/end_portal.json) has no
// elements ON PURPOSE: vanilla draws it from a dedicated pass whose fragment
// shader projects a scrolling 15-layer starfield in SCREEN space, which no
// amount of baked cube geometry can express. So the mesher produces nothing
// for these blocks and this renderer supplies all of it.
//
// ── Why this is NOT a BlockEntityRenderer ────────────────────────────────
// MC hangs the renderer off TheEndPortalBlockEntity. We cannot: in this engine
// block entities are created ONLY by World::SetBlock
// (src/common/world/level/World.cpp), are never persisted and are never
// created for world-generated blocks — so a stronghold's end portal, which is
// exactly the case that matters, has no block entity at all and would render
// nothing. Instead ClientChunkManager keeps a per-chunk index of end-portal
// positions (ClientChunk::endPortals) and this class walks that, driven by a
// standalone call from PlatformMain next to the block-entity dispatcher.
//
// Geometry and render state are vanilla (AbstractEndPortalRenderer.java:39-66,
// RenderPipelines.java:154/211): only the two horizontal faces are drawn — the
// DOWN face at y = 0.375 and the UP face at y = 0.75 — with depth test and
// depth write ON, blending OFF and back-face culling ON.
#pragma once

#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Client { class ClientChunkManager; }

namespace Render {

    class EndPortalRenderer {
    public:
        ~EndPortalRenderer();

        // Creates the shader, loads the two standalone textures and reserves
        // the streaming vertex buffer. Returns false (having logged) if any of
        // that fails; Render() then no-ops, so a failure costs the portal
        // visual and nothing else.
        bool Initialize();
        void Shutdown();

        // Per-frame pass. Batches every visible portal quad into ONE draw.
        // `partialTick` only feeds the starfield's clock (MC's GameTime),
        // which is the shader's sole animated input.
        //
        // Called from PlatformMain immediately after
        // BlockEntityRenderDispatcher::RenderAll — the portal is opaque world
        // geometry, so it belongs with the block entities, after the chunk
        // passes and before the portal-gun see-through pass.
        void Render(Client::ClientChunkManager* chunkMgr,
                    const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

    private:
        // The shared 24-byte block vertex layout (GetBlockVertexLayout). The
        // shader reads only the position; uv/colour are dead weight we accept
        // so both backends can use their default vertex-input setup.
        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vert) == 24, "vertex stride must match block layout");

        // MC BlockEntityRenderer.getViewDistance default (64 blocks). Portals
        // are two flat quads, so there is no reason to deviate.
        static constexpr float kViewDistance = 64.0f;

        bool m_initialized = false;

        ShaderHandle  m_shader     = INVALID_SHADER;
        TextureHandle m_skyTexture = INVALID_TEXTURE;   // MC Sampler0
        TextureHandle m_portalTexture = INVALID_TEXTURE; // MC Sampler1

        BufferHandle m_vb = INVALID_BUFFER;
        MeshHandle   m_mesh = INVALID_MESH;
        size_t       m_vbCapacityVerts = 0;

        // Reused across frames so a steady-state frame does no allocation.
        std::vector<Vert> m_verts;

        static const char* s_vertSource;
        static const char* s_fragSource;
    };

    extern EndPortalRenderer g_endPortalRenderer;

} // namespace Render
