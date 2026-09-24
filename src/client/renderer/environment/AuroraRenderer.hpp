// File: src/client/renderer/environment/AuroraRenderer.hpp
//
// The Hush's auroras (docs/the-hush.md, Atmosphere): a handful of
// translucent curtain bands high in the frozen night, drawn in the sky pass
// right after the stars (SkyRenderer::RenderFixedNight).
//
// Each band is a vertical curtain hung along a gently curving line across
// the sky: bright at its lower edge, fading to nothing at the top, with a
// short soft fade under the edge so it never reads as a hard line. The line
// undulates (two travelling sine waves across it), the whole band slowly
// drifts round the sky, and its brightness shimmers in folds along its
// length and fades out at both ends and toward the horizon. Colours run from
// the sculk veins' #2BD4C0 at the lower edge to a pale violet at the top,
// each band at its own point between the two.
//
// No shader of its own: the ribbons are ordinary position/uv/colour
// vertices (the 24-byte block layout) drawn with the SKY shader SkyRenderer
// already owns in both backends (sky.vert/frag, sky_vk twins), additive
// ("overlay": SRC_ALPHA, ONE) like the stars, fog off. The animation is
// CPU-side: the whole set is ~1.5 k vertices, rebuilt once per frame into
// a streaming vertex buffer — cheaper than a new pipeline pair, and it
// keeps the VK and GL paths identical.
//
// Strength comes from the frame (EnvironmentFrame::auroraStrength, composed
// by HushAtmosphere: faint everywhere in the Hush, full over aurora_steppe,
// dimmed by a stillness, hidden underground) and multiplies the vertex
// alpha through the shader's uColor.
#pragma once

#include "../backend/RenderTypes.hpp"

#include <glm/glm.hpp>
#include <chrono>
#include <cstdint>
#include <vector>

namespace Render {

    class AuroraRenderer {
    public:
        // `skyShader` and `white` are SkyRenderer's; this renderer only owns
        // its buffers. False (and every Render a no-op) when the buffers
        // could not be made.
        bool Initialize(ShaderHandle skyShader, TextureHandle white);
        void Shutdown();

        // Inside the sky pass, after the stars. `viewProj` is the sky's
        // camera-centred projection × rotation-only view. Leaves the
        // pipeline state as it found it only in the sense the sky pass
        // needs: the caller restores the default state after its last draw.
        void Render(const glm::mat4& viewProj, float strength, const glm::vec3& fogColor);

    private:
        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "must match GetBlockVertexLayout stride");

        static constexpr int kBands    = 5;
        static constexpr int kSegments = 72;   // quads across a band
        static constexpr int kRows     = 4;    // under-fade, lower edge, body, top
        static constexpr int kVertsPerBand   = (kSegments + 1) * kRows;
        static constexpr int kIndicesPerBand = kSegments * (kRows - 1) * 6;
        static constexpr int kVertexCount    = kBands * kVertsPerBand;
        static constexpr int kIndexCount     = kBands * kIndicesPerBand;
        // Streaming buffers the frames rotate through. The Vulkan backend's
        // UpdateBuffer is an immediate host write and two frames are in
        // flight; the rebuild is keyed on the entity frame serial, which the
        // sky can see twice in one real frame (the main sky runs before the
        // serial bumps, a portal's far-side sky after) — so up to two writes
        // a frame, and six slots keep every write clear of the GPU.
        static constexpr int kSlots = 6;

        void Build(double timeSeconds);

        ShaderHandle  m_shader = INVALID_SHADER;
        TextureHandle m_white  = INVALID_TEXTURE;
        BufferHandle  m_ib     = INVALID_BUFFER;
        struct Slot {
            BufferHandle vb   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        Slot     m_slots[kSlots];
        int      m_slot = 0;
        uint32_t m_lastSerial = 0;
        bool     m_built = false;

        std::vector<Vertex> m_verts;
        std::chrono::steady_clock::time_point m_epoch{};
        bool m_initialized = false;
    };

    extern AuroraRenderer g_auroraRenderer;

} // namespace Render
