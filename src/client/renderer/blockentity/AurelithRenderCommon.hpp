// File: src/client/renderer/blockentity/AurelithRenderCommon.hpp
//
// Shared plumbing for Aurelith's two set-piece renderers — the Heart's hanging
// rings (AurelithHeartRenderer) and the gate towers' sky beams
// (VoiceBeaconRenderer). Both draw with the shared block-entity shader
// (BlockEntityShader.hpp: texture x vertex colour x uBlockEntityLight, fogged
// like the terrain), from meshes baked once at Initialize in the 24-byte
// block vertex layout, and both are pure functions of the level's game time
// and the block's position — the same contract HushLighthouseRenderer keeps,
// so every player sees the same rings and the same pulses.
//
// Everything here is header-only and internal to those two renderers
// (namespace Render::Aurelith).
#pragma once

#include "../backend/RenderBackend.hpp"
#include "../backend/RenderTypes.hpp"
#include "../environment/EnvironmentState.hpp"
#include "client/world/ClientBlockAccess.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace Render::Aurelith {

    inline constexpr double kPi    = 3.14159265358979323846;
    inline constexpr double kTwoPi = 2.0 * kPi;
    inline constexpr double kDeg   = kPi / 180.0;

    // The block vertex layout (GetBlockVertexLayout): position, uv, RGBA8.
    struct Vert {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(Vert) == 24, "must match GetBlockVertexLayout");

    struct Rgb { uint8_t r, g, b; };

    inline uint8_t Lerp8(uint8_t a, uint8_t b, double t) {
        return static_cast<uint8_t>(std::lround(a + (b - a) * std::clamp(t, 0.0, 1.0)));
    }
    inline Rgb Mix(Rgb a, Rgb b, double t) {
        return {Lerp8(a.r, b.r, t), Lerp8(a.g, b.g, t), Lerp8(a.b, b.b, t)};
    }
    inline Rgb Scale(Rgb c, double k) {
        auto s = [k](uint8_t v) {
            return static_cast<uint8_t>(std::lround(std::clamp(v * k, 0.0, 255.0)));
        };
        return {s(c.r), s(c.g), s(c.b)};
    }
    inline uint8_t Alpha8(double a) {
        return static_cast<uint8_t>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0));
    }

    inline double SmoothStep(double e0, double e1, double x) {
        const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    // A symmetric sigmoid on [0, 1]: eases in and out, steeper as k grows
    // (k = 1 is the identity).
    inline double Ease(double s, double k) {
        if (s <= 0.0) return 0.0;
        if (s >= 1.0) return 1.0;
        const double a = std::pow(s, k);
        const double b = std::pow(1.0 - s, k);
        return a / (a + b);
    }

    // ── hashing (as the lighthouse: every draw is a hash of seed, index and
    // salt, so any moment is computed directly and nothing accumulates) ────
    inline uint64_t Mix64(uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    inline double Unit(uint64_t h) {                               // [0, 1)
        return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
    }
    inline double Draw(uint64_t seed, int64_t n, uint64_t salt) {
        return Unit(Mix64(seed ^ Mix64(static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull
                                       ^ salt * 0xD1B54A32D192ED03ull)));
    }

    // MC Mth.getSeed(x, y, z), unsigned so Java's wrap-around is defined.
    inline uint64_t PositionSeed(const glm::ivec3& p) {
        const int32_t xs = static_cast<int32_t>(static_cast<uint32_t>(p.x) * 3129871u);
        uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(xs))
                      ^ (static_cast<uint64_t>(static_cast<int64_t>(p.z)) * 116129781ull)
                      ^ static_cast<uint64_t>(static_cast<int64_t>(p.y));
        seed = seed * seed * 42317861ull + seed * 11ull;
        return static_cast<uint64_t>(static_cast<int64_t>(seed) >> 16);
    }

    inline double ValueNoise(uint64_t seed, double x, uint64_t salt) {
        const double f = std::floor(x);
        const int64_t i = static_cast<int64_t>(f);
        const double t = x - f;
        const double a = Draw(seed, i, salt);
        const double b = Draw(seed, i + 1, salt);
        return a + (b - a) * t * t * (3.0 - 2.0 * t);
    }

    // The local player's eyes, when this view is in the player's world. A
    // portal view is not (its frame override is set), and draws the plain
    // animation — exactly the lighthouse's rule.
    inline bool LocalViewerEye(glm::dvec3& out) {
        if (EnvironmentState::Get().FrameOverride()) return false;
        if (!Client::g_clientBlockAccess) return false;
        glm::dvec3 mn, mx;
        if (!Client::g_clientBlockAccess->GetLocalPlayerBox(mn, mx)) return false;
        out = glm::dvec3((mn.x + mx.x) * 0.5, mn.y + (mx.y - mn.y) * 0.9, (mn.z + mx.z) * 0.5);
        return true;
    }

    // ── GPU meshes ───────────────────────────────────────────────────────
    // Baked once at Initialize and destroyed only at Shutdown (renderer
    // teardown), never while a frame that drew them is in flight.
    struct GpuMesh {
        BufferHandle vb = INVALID_BUFFER;
        BufferHandle ib = INVALID_BUFFER;
        MeshHandle   mesh = INVALID_MESH;
        uint32_t     indexCount = 0;

        bool Valid() const { return mesh != INVALID_MESH && indexCount > 0; }
    };

    inline bool BuildMesh(GpuMesh& out, const std::vector<Vert>& verts,
                          const std::vector<uint32_t>& idx) {
        if (verts.empty() || idx.empty()) return false;
        out.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, verts.size() * sizeof(Vert), verts.data());
        out.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t), idx.data());
        if (out.vb == INVALID_BUFFER || out.ib == INVALID_BUFFER) return false;
        out.mesh = g_renderBackend->CreateMesh(out.vb, out.ib, GetBlockVertexLayout());
        out.indexCount = static_cast<uint32_t>(idx.size());
        return out.mesh != INVALID_MESH;
    }

    inline void DestroyMesh(GpuMesh& m) {
        if (!g_renderBackend) return;
        if (m.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m.mesh);  m.mesh = INVALID_MESH; }
        if (m.vb   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m.vb); m.vb = INVALID_BUFFER; }
        if (m.ib   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m.ib); m.ib = INVALID_BUFFER; }
        m.indexCount = 0;
    }

    inline void PushQuad(std::vector<uint32_t>& idx, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        for (uint32_t k : {a, b, c, a, c, d}) idx.push_back(k);
    }

    // A unit billboard quad in XY facing +Z (the renderers scale and orient
    // it from the view matrix).
    inline void AppendBillboard(std::vector<Vert>& verts, std::vector<uint32_t>& idx, Rgb c, double alpha) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const uint8_t a = Alpha8(alpha);
        verts.push_back({-0.5f, -0.5f, 0.0f, 0.0f, 1.0f, c.r, c.g, c.b, a});
        verts.push_back({ 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, c.r, c.g, c.b, a});
        verts.push_back({ 0.5f,  0.5f, 0.0f, 1.0f, 0.0f, c.r, c.g, c.b, a});
        verts.push_back({-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, c.r, c.g, c.b, a});
        PushQuad(idx, base, base + 1, base + 2, base + 3);
    }

    // A flat annulus in the XZ plane between radii r0 and r1, u running
    // across the band (0 inside, 1 outside) so a bell texture softens both
    // edges, v around it.
    inline void AppendAnnulus(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                              int segments, float r0, float r1, float y, Rgb c, double alpha) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const uint8_t a = Alpha8(alpha);
        for (int i = 0; i <= segments; ++i) {
            const double t = static_cast<double>(i) / segments;
            const float cs = static_cast<float>(std::cos(kTwoPi * t));
            const float sn = static_cast<float>(std::sin(kTwoPi * t));
            verts.push_back({r0 * cs, y, r0 * sn, 0.0f, static_cast<float>(t), c.r, c.g, c.b, a});
            verts.push_back({r1 * cs, y, r1 * sn, 1.0f, static_cast<float>(t), c.r, c.g, c.b, a});
        }
        for (int i = 0; i < segments; ++i) {
            const uint32_t k = base + 2u * static_cast<uint32_t>(i);
            PushQuad(idx, k, k + 2, k + 3, k + 1);
        }
    }

    // A short open cylinder (radius r, y from -h to +h) round the Y axis,
    // u running up the band — a ring's glow seen edge-on.
    inline void AppendBand(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                           int segments, float r, float h, Rgb c, double alpha) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const uint8_t a = Alpha8(alpha);
        for (int i = 0; i <= segments; ++i) {
            const double t = static_cast<double>(i) / segments;
            const float cs = static_cast<float>(std::cos(kTwoPi * t));
            const float sn = static_cast<float>(std::sin(kTwoPi * t));
            verts.push_back({r * cs, -h, r * sn, 0.0f, static_cast<float>(t), c.r, c.g, c.b, a});
            verts.push_back({r * cs,  h, r * sn, 1.0f, static_cast<float>(t), c.r, c.g, c.b, a});
        }
        for (int i = 0; i < segments; ++i) {
            const uint32_t k = base + 2u * static_cast<uint32_t>(i);
            PushQuad(idx, k, k + 2, k + 3, k + 1);
        }
    }

    // A fan of `planes` vertical quads through the +Y axis — a light column
    // from y0 to y1, each plane widening from w0 to w1; alpha fades in over
    // the first `fadeIn` blocks and falls off as (1 - t)^fall. Segments bunch
    // toward the base, where the fade and the colour change fastest.
    inline void AppendColumnFan(std::vector<Vert>& verts, std::vector<uint32_t>& idx,
                                int planes, double angleOffset, float y0, float y1,
                                float w0, float w1, double alpha0, double fadeIn, double fall,
                                Rgb nearCol, Rgb farCol, int segments) {
        for (int p = 0; p < planes; ++p) {
            const double ang = angleOffset + kPi * p / planes;
            const float cx = static_cast<float>(std::cos(ang));
            const float cz = static_cast<float>(std::sin(ang));
            const uint32_t base = static_cast<uint32_t>(verts.size());
            for (int i = 0; i <= segments; ++i) {
                const double t = std::pow(static_cast<double>(i) / segments, 1.6);
                const float y = static_cast<float>(y0 + (y1 - y0) * t);
                const float half = static_cast<float>(w0 + (w1 - w0) * t);
                const double fade = SmoothStep(y0, y0 + fadeIn, y);
                const uint8_t a8 = Alpha8(alpha0 * fade * std::pow(1.0 - t, fall));
                const Rgb c = Mix(nearCol, farCol, t);
                verts.push_back({-half * cx, y, -half * cz, 0.0f, static_cast<float>(t), c.r, c.g, c.b, a8});
                verts.push_back({ half * cx, y,  half * cz, 1.0f, static_cast<float>(t), c.r, c.g, c.b, a8});
            }
            for (int i = 0; i < segments; ++i) {
                const uint32_t a0 = base + 2u * static_cast<uint32_t>(i);
                PushQuad(idx, a0, a0 + 2, a0 + 3, a0 + 1);
            }
        }
    }

    // ── procedural textures (white; the vertices carry the colour) ───────
    // Bell: across u a soft sin² bell so no plane or band shows an edge, and
    // faint striations along v, like motes in the light.
    inline std::vector<uint8_t> BellPixels(int w, int h, uint64_t seed) {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const double u = (x + 0.5) / w;
                const double v = (y + 0.5) / h;
                const double bell = std::pow(std::sin(kPi * u), 2.0);
                const double streak = 0.72 + 0.28 * ValueNoise(seed, u * 9.0 + v * 1.5, 30);
                const size_t o = (static_cast<size_t>(y) * w + x) * 4;
                px[o + 0] = px[o + 1] = px[o + 2] = 255;
                px[o + 3] = Alpha8(bell * streak);
            }
        }
        return px;
    }

    // Glow: a soft radial falloff with a brighter heart.
    inline std::vector<uint8_t> GlowPixels(int size) {
        std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
        const double c = size * 0.5;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const double r = std::hypot(x + 0.5 - c, y + 0.5 - c) / c;
                const double halo = std::pow(std::max(0.0, 1.0 - r), 2.2);
                const double heart = std::pow(std::max(0.0, 1.0 - r * 3.0), 2.0);
                const size_t o = (static_cast<size_t>(y) * size + x) * 4;
                px[o + 0] = px[o + 1] = px[o + 2] = 255;
                px[o + 3] = Alpha8(0.8 * halo + 0.2 * heart);
            }
        }
        return px;
    }

    inline TextureHandle MakeTexture(int w, int h, const std::vector<uint8_t>& px, bool repeatV = false) {
        TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, px.data());
        if (tex != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
            g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge,
                                            repeatV ? TextureWrap::Repeat : TextureWrap::ClampToEdge);
        }
        return tex;
    }

    // ── pipeline states ──────────────────────────────────────────────────
    // Light: additive (the Hush is dark, so light adds; a draw fades by its
    // uBlockEntityLight as well as its vertex alpha), depth-tested with no
    // depth write, both faces — MC's beacon-beam translucent type with an
    // additive blend.
    inline PipelineState GlowPipeline() {
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = false;
        s.blendEnabled      = true;
        s.srcBlendFactor    = BlendFactor::SrcAlpha;
        s.dstBlendFactor    = BlendFactor::One;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        return s;
    }

    // Crystal: opaque and depth-writing. No face culling: the staves are
    // closed prisms, so their hidden faces lose the depth test anyway, and
    // the baked winding never decides what shows.
    inline PipelineState SolidPipeline() {
        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;
        return s;
    }

    // A billboard's model matrix: camera right/up out of the view matrix's
    // rotation rows, scaled by `size`, placed at render-space `at`.
    inline glm::mat4 BillboardModel(const glm::mat4& view, const glm::vec3& at, float size) {
        const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 up   (view[0][1], view[1][1], view[2][1]);
        const glm::vec3 back (view[0][2], view[1][2], view[2][2]);
        glm::mat4 model(1.0f);
        model[0] = glm::vec4(right * size, 0.0f);
        model[1] = glm::vec4(up * size, 0.0f);
        model[2] = glm::vec4(back, 0.0f);
        model[3] = glm::vec4(at, 1.0f);
        return model;
    }

} // namespace Render::Aurelith
