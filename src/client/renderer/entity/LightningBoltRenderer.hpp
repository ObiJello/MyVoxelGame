// File: src/client/renderer/entity/LightningBoltRenderer.hpp
//
// MC net.minecraft.client.renderer.entity.LightningBoltRenderer — the jagged,
// branching bolt. Pure geometry: no texture, no model. From the entity's
// `seed` it builds an 8-segment trunk (16 blocks a segment, 128 tall) and two
// 3-segment side branches, each segment a four-sided box of quads, repeated
// in four ever-wider layers; every vertex is (0.45, 0.45, 0.5, 0.3) and the
// whole thing blends additively (MC RenderTypes.lightning: POSITION_COLOR,
// BlendFunction.LIGHTNING = SRC_ALPHA, ONE; default depth test + write;
// back faces culled; fogged).
//
// Drawn in the world pass after opaque terrain, with the block shader over a
// 1×1 white texture so the vertex colour is the colour (sky-light dim pinned
// at 1 — the lightning shader has no lightmap). The quads are built in
// render space (Render::ToRender of the bolt's world position), one batch per
// call, the XpOrbRenderer streaming pattern (EntityFrame double-buffering).
//
// MC affectedByCulling() is false — no frustum test — and
// shouldRenderAtSqrDistance is 64 × the entity view scale.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert (24-byte block layout)
#include "EntityFrame.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Client { class ClientMobManager; }

namespace Render {

    class LightningBoltRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Every LightningBolt in `mobs`. `cameraPos` is the view's world-space
        // eye (the distance cull); `view` is the render-space view matrix.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::dvec3& cameraPos, const Client::ClientMobManager& mobs);

    private:
        // Append one bolt's quads (MC submit) at render-space `origin`.
        void AppendBolt(int64_t seed, const glm::vec3& origin);

        bool m_initialized = false;

        ShaderHandle  m_shader  = INVALID_SHADER;
        TextureHandle m_white   = INVALID_TEXTURE;

        // Two vertex sets alternated per frame, each call appending at a
        // cursor (EntityFrame.hpp); the index buffer is the static quad
        // pattern 0,1,2,0,2,3 for kMaxQuads quads.
        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        BufferHandle m_ib = INVALID_BUFFER;
        EntityFrame::Cursor m_frameCursor;
        size_t m_quadCursor = 0;   // quads already written this frame

        // MC: 4 layers × (8 trunk + 3 + 3 branch segments) × 4 quads.
        static constexpr size_t kQuadsPerBolt = 4 * (8 + 3 + 3) * 4;
        // Bolts per frame set. A thunderstorm has one or two alive at once;
        // a /summon spree past this skips the remainder for a frame.
        static constexpr size_t kMaxBolts = 64;
        static constexpr size_t kMaxQuads = kQuadsPerBolt * kMaxBolts;

        std::vector<ItemCubeVert> m_verts;
    };

} // namespace Render
