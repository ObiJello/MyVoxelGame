// File: src/client/renderer/environment/AuroraRenderer.cpp
//
// See the header. Geometry lives in the sky's camera-centred space: +Y up,
// the star sphere at radius 100, the sky disc at y = ±16 (radius 512). The
// bands hang 40–62 units up at 25–85 units out, so they sit between ~20°
// and ~70° of elevation — high in the sky, clear of the horizon and the
// terrain silhouette.
#include "AuroraRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../entity/EntityFrame.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Render {

    AuroraRenderer g_auroraRenderer;

    namespace {

        // One curtain. Angles in radians, distances in sky units, speeds per
        // second. Fixed rather than random so the Hush's sky is the same sky
        // every visit.
        struct Band {
            float azimuth;      // direction of the band's middle, from +X toward +Z
            float drift;        // azimuth change per second (the slow wander)
            float distance;     // horizontal distance of the band's middle
            float length;       // across the sky
            float baseY;        // lower edge height
            float height;       // curtain height
            float amp1, freq1, speed1;   // main undulation
            float amp2, freq2, speed2;   // the ripple riding on it
            float hue;          // 0 = teal-led, 1 = violet-led
            float weight;       // relative brightness
            float phase;
        };

        constexpr Band kBandTable[] = {
            { 0.30f,  0.0060f, 55.0f, 230.0f, 52.0f, 38.0f, 10.0f, 0.045f, 0.22f, 4.0f, 0.110f, 0.35f, 0.00f, 1.00f, 0.0f },
            { 2.20f, -0.0040f, 70.0f, 260.0f, 46.0f, 32.0f, 12.0f, 0.035f, 0.18f, 5.0f, 0.090f, 0.28f, 0.35f, 0.80f, 1.7f },
            { 3.90f,  0.0050f, 40.0f, 180.0f, 58.0f, 42.0f,  8.0f, 0.055f, 0.25f, 3.0f, 0.130f, 0.40f, 0.60f, 0.70f, 3.1f },
            { 5.10f, -0.0070f, 85.0f, 240.0f, 40.0f, 28.0f, 14.0f, 0.030f, 0.15f, 6.0f, 0.080f, 0.30f, 0.20f, 0.60f, 4.4f },
            { 1.20f,  0.0030f, 25.0f, 150.0f, 62.0f, 36.0f,  6.0f, 0.060f, 0.20f, 3.0f, 0.150f, 0.45f, 0.85f, 0.50f, 5.9f },
        };

        // #2BD4C0 (the sculk veins) at the lower edge, a pale violet above.
        const glm::vec3 kTeal  (0x2B / 255.0f, 0xD4 / 255.0f, 0xC0 / 255.0f);
        const glm::vec3 kViolet(0xC8 / 255.0f, 0xB6 / 255.0f, 0xF7 / 255.0f);

        // Overall ceiling on vertex alpha; the frame's strength scales it.
        constexpr float kPeakAlpha = 0.9f;
        // Soft fade under the bright edge, in sky units.
        constexpr float kUnderFade = 3.5f;
        // Horizon fade, radians of elevation of the lower edge.
        constexpr float kHorizonFadeFrom = 0.12f;   // ~7°
        constexpr float kHorizonFadeTo   = 0.45f;   // ~26°

        const glm::vec4 kFogOff{1e9f, 1e9f, 1e9f, 1e9f};

        float Smoothstep(float edge0, float edge1, float x) {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        uint8_t ToByte(float v) {
            return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }

    } // namespace

    bool AuroraRenderer::Initialize(ShaderHandle skyShader, TextureHandle white) {
        if (m_initialized) return true;
        if (!g_renderBackend || skyShader == INVALID_SHADER) return false;
        m_shader = skyShader;
        m_white  = white;

        // Static indices: per band, per segment, three quads up the curtain.
        std::vector<uint32_t> indices;
        indices.reserve(kIndexCount);
        for (int b = 0; b < kBands; ++b) {
            const uint32_t base = static_cast<uint32_t>(b * kVertsPerBand);
            for (int i = 0; i < kSegments; ++i) {
                for (int r = 0; r < kRows - 1; ++r) {
                    const uint32_t v00 = base + static_cast<uint32_t>(i * kRows + r);
                    const uint32_t v01 = v00 + 1;                              // up a row
                    const uint32_t v10 = v00 + static_cast<uint32_t>(kRows);   // next column
                    const uint32_t v11 = v10 + 1;
                    indices.insert(indices.end(), { v00, v10, v11, v00, v11, v01 });
                }
            }
        }
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index, indices.size() * sizeof(uint32_t),
                                             indices.data(), BufferAccess::Static);
        if (m_ib == INVALID_BUFFER) return false;

        for (Slot& slot : m_slots) {
            slot.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                                                    kVertexCount * sizeof(Vertex), nullptr,
                                                    BufferAccess::Streaming);
            if (slot.vb == INVALID_BUFFER) { Shutdown(); return false; }
            slot.mesh = g_renderBackend->CreateMesh(slot.vb, m_ib, GetBlockVertexLayout());
            if (slot.mesh == INVALID_MESH) { Shutdown(); return false; }
        }
        m_verts.resize(kVertexCount);
        m_epoch = std::chrono::steady_clock::now();
        m_built = false;
        m_initialized = true;
        return true;
    }

    void AuroraRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (Slot& slot : m_slots) {
            if (slot.mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(slot.mesh); slot.mesh = INVALID_MESH; }
            if (slot.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(slot.vb); slot.vb = INVALID_BUFFER; }
        }
        if (m_ib != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_ib); m_ib = INVALID_BUFFER; }
        // The shader and texture are SkyRenderer's.
        m_shader = INVALID_SHADER;
        m_white  = INVALID_TEXTURE;
        m_built = false;
        m_initialized = false;
    }

    void AuroraRenderer::Build(double t) {
        static_assert(sizeof(kBandTable) / sizeof(kBandTable[0]) == kBands,
                      "kBands must match the band table");
        for (int b = 0; b < kBands; ++b) {
            const Band& band = kBandTable[b];
            Vertex* out = m_verts.data() + b * kVertsPerBand;

            // The band's frame: `radial` points from the camera to its middle,
            // `along` runs across the sky.
            const double az = band.azimuth + band.drift * t;
            const glm::vec2 radial(static_cast<float>(std::cos(az)), static_cast<float>(std::sin(az)));
            const glm::vec2 along(-radial.y, radial.x);
            const glm::vec2 centre = radial * band.distance;
            // Slow breathing of the whole band.
            const float breathe = 0.8f + 0.2f * static_cast<float>(std::sin(0.05 * t + band.phase));
            const float hueShift = 0.5f + 0.5f * static_cast<float>(std::sin(0.03 * t + band.phase * 1.3));

            for (int i = 0; i <= kSegments; ++i) {
                const float s = static_cast<float>(i) / static_cast<float>(kSegments);
                const float u = (s - 0.5f) * band.length;

                // The undulating line, pushed in and out along `radial`.
                const float wobble =
                    band.amp1 * static_cast<float>(std::sin(band.freq1 * u + band.speed1 * t + band.phase)) +
                    band.amp2 * static_cast<float>(std::sin(band.freq2 * u - band.speed2 * t + band.phase * 2.0));
                const glm::vec2 p = centre + along * u + radial * wobble;

                // The edge rises and falls a little along the band, and the
                // curtain's height breathes.
                const float edgeY = band.baseY +
                    4.0f * static_cast<float>(std::sin(0.02 * u + 0.10 * t + band.phase));
                const float h = band.height *
                    (0.75f + 0.25f * static_cast<float>(std::sin(0.035 * u - 0.07 * t + band.phase)));

                // Brightness along the band: gone at both ends, folds that
                // shimmer as they travel, and faded toward the horizon.
                const float ends = std::pow(std::sin(glm::pi<float>() * s), 1.2f);
                const float folds =
                    (0.55f + 0.45f * (0.5f + 0.5f * static_cast<float>(std::sin(0.08 * u + 0.30 * t + band.phase)))) *
                    (0.85f + 0.15f * static_cast<float>(std::sin(0.5 * u - 0.9 * t + band.phase * 2.0)));
                const float elevation = std::atan2(edgeY, glm::length(p));
                const float horizon = Smoothstep(kHorizonFadeFrom, kHorizonFadeTo, elevation);
                const float edgeAlpha = kPeakAlpha * band.weight * breathe * ends * folds * horizon;

                // Colours: teal-led at the edge, violet toward the top.
                const float lowerMix = std::clamp(band.hue * 0.45f + 0.12f * hueShift, 0.0f, 1.0f);
                const glm::vec3 lower = glm::mix(kTeal, kViolet, lowerMix);
                const glm::vec3 upper = glm::mix(kTeal, kViolet, 0.55f + 0.45f * band.hue);
                const glm::vec3 body  = glm::mix(lower, upper, 0.45f);

                struct Row { float y; glm::vec3 c; float a; };
                const Row rows[kRows] = {
                    { edgeY - kUnderFade, lower, 0.0f },
                    { edgeY,              lower, edgeAlpha },
                    { edgeY + h * 0.3f,   body,  edgeAlpha * 0.5f },
                    { edgeY + h,          upper, 0.0f },
                };
                for (int r = 0; r < kRows; ++r) {
                    Vertex& v = out[i * kRows + r];
                    v.x = p.x; v.y = rows[r].y; v.z = p.y;
                    v.u = 0.5f; v.v = 0.5f;
                    v.r = ToByte(rows[r].c.r);
                    v.g = ToByte(rows[r].c.g);
                    v.b = ToByte(rows[r].c.b);
                    v.a = ToByte(rows[r].a);
                }
            }
        }
    }

    void AuroraRenderer::Render(const glm::mat4& viewProj, float strength, const glm::vec3& fogColor) {
        if (!m_initialized || !g_renderBackend || strength <= 0.004f) return;
        PROFILE_ZONE_N("AuroraRender");

        // Rebuild once per frame serial into the next slot; later calls in
        // the same serial (a portal's far-side Hush sky) draw what is there.
        const uint32_t serial = EntityFrame::Serial();
        if (!m_built || serial == 0 || serial != m_lastSerial) {
            m_slot = (m_slot + 1) % kSlots;
            const double t = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_epoch).count();
            Build(t);
            g_renderBackend->UpdateBuffer(m_slots[m_slot].vb, 0, m_verts.size() * sizeof(Vertex),
                                          m_verts.data());
            m_lastSerial = serial;
            m_built = true;
        }

        // MC's "overlay" blend, as the stars: additive, no depth.
        PipelineState state;
        state.depthTestEnabled  = false;
        state.depthWriteEnabled = false;
        state.blendEnabled      = true;
        state.srcBlendFactor    = BlendFactor::SrcAlpha;
        state.dstBlendFactor    = BlendFactor::One;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_white, 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj);
        g_renderBackend->SetUniformVec4(m_shader, "uColor",
                                        glm::vec4(1.0f, 1.0f, 1.0f, std::clamp(strength, 0.0f, 1.0f)));
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", kFogOff);
        g_renderBackend->DrawIndexed(m_slots[m_slot].mesh, static_cast<uint32_t>(kIndexCount), 0);
    }

} // namespace Render
