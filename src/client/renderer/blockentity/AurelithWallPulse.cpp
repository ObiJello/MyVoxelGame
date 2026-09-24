// File: src/client/renderer/blockentity/AurelithWallPulse.cpp
//
// See the header for the design. Layout of this file:
//   the band        (the Stave band's exposed faces, found once from the
//                    wall's octagon — the same for every city)
//   the pulse       (where the swell is: a pure function of game time)
//   the stream      (a ring of per-call vertex buffers — Vulkan records an
//                    upload at once and draws at submit)
//   the draw
#include "AurelithWallPulse.hpp"
#include "AurelithRenderCommon.hpp"
#include "BlockEntityShader.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/level/AurelithQuest.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../environment/EntityEnvironment.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Render {

    using namespace Aurelith;

    namespace {

        namespace A = Game::Aurelith;

        // ── the band ─────────────────────────────────────────────────────
        // One exposed side face of a band cell, in design offsets from the
        // Heart block's minimum corner (x east, z south; the octagon is
        // symmetric under the city's rotations, so the table is rotation-
        // free): the face's two bottom corners, already pushed kLift proud of
        // the stone, and the bearing from the Heart's centre of each corner
        // and of the face's middle.
        struct Face {
            glm::vec2 a, b;          // x/z of the two edges
            double    bearingA, bearingB, bearing;
            double    radius;        // distance of the face's middle from the Heart
        };

        constexpr float  kLift          = 0.025f;   // proud of the stone
        constexpr int    kGateHalfWidth = 17;       // the gate towers' outer faces, design u
        constexpr int    kGateNear      = 88;       // radial start of a gate complex

        int OctagonCells(int dx, int dz) {
            // gen_aurelith.py's octagon() on the integer design grid, compared
            // against WALL_IN / WALL_OUT exactly as the generator's masks are.
            const double r = A::OctagonRadius(static_cast<double>(dx), static_cast<double>(dz));
            if (r < A::kWallIn) return -1;          // inside the city
            if (r < A::kWallOut) return 0;          // the wall
            return 1;                               // outside
        }

        bool InGateComplex(int dx, int dz) {
            return (std::abs(dx) <= kGateHalfWidth && std::abs(dz) >= kGateNear) ||
                   (std::abs(dz) <= kGateHalfWidth && std::abs(dx) >= kGateNear);
        }

        double BearingOf(double x, double z) {
            // From the Heart block's centre (0.5, 0.5); the sweep convention
            // atan2(dz, dx) of the lighthouse and the Heart.
            return std::atan2(z - 0.5, x - 0.5);
        }

        const std::vector<Face>& BandFaces() {
            static const std::vector<Face> faces = [] {
                std::vector<Face> out;
                const int reach = static_cast<int>(A::kWallOut) + 2;
                for (int dx = -reach; dx <= reach; ++dx) {
                    for (int dz = -reach; dz <= reach; ++dz) {
                        if (OctagonCells(dx, dz) != 0 || InGateComplex(dx, dz)) continue;
                        // Each horizontal side whose neighbour is off the wall
                        // (the city side or the outside) shows the band.
                        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : dirs) {
                            if (OctagonCells(dx + d[0], dz + d[1]) == 0) continue;
                            Face f{};
                            const float x0 = static_cast<float>(dx), z0 = static_cast<float>(dz);
                            if (d[0] != 0) {
                                const float x = d[0] > 0 ? x0 + 1.0f + kLift : x0 - kLift;
                                f.a = {x, z0};
                                f.b = {x, z0 + 1.0f};
                            } else {
                                const float z = d[1] > 0 ? z0 + 1.0f + kLift : z0 - kLift;
                                f.a = {x0, z};
                                f.b = {x0 + 1.0f, z};
                            }
                            f.bearingA = BearingOf(f.a.x, f.a.y);
                            f.bearingB = BearingOf(f.b.x, f.b.y);
                            const glm::vec2 mid = (f.a + f.b) * 0.5f;
                            f.bearing = BearingOf(mid.x, mid.y);
                            f.radius = std::hypot(mid.x - 0.5, mid.y - 0.5);
                            out.push_back(f);
                        }
                    }
                }
                return out;
            }();
            return faces;
        }

        // ── the pulse ────────────────────────────────────────────────────
        // A swell travels the circuit counter-clockwise seen from above (+
        // bearing), once every kCircuitTicks: a bright head that eases up
        // over kHeadBlocks ahead of it and a long tail of kTailBlocks
        // fading behind. The awakened city carries a second one opposite.
        constexpr double kCircuitTicks = 1800.0;    // 90 s: ~7 blocks a second along the wall
        constexpr double kHeadBlocks   = 2.5;
        constexpr double kTailBlocks   = 18.0;
        constexpr double kCutoff       = 0.015;     // below this a face is not drawn

        double WrapPi(double a) { return a - kTwoPi * std::floor((a + kPi) / kTwoPi); }

        // The swell's strength at `bearing` for a head at `head`, at the
        // wall's radius `radius` (so head and tail are measured in blocks).
        double Swell(double bearing, double head, double radius) {
            const double d = WrapPi(bearing - head) * radius;   // + ahead of the head
            if (d >= 0.0) return std::exp(-(d / kHeadBlocks) * (d / kHeadBlocks));
            return std::exp(d / kTailBlocks);
        }

        // The Stave's five lines across the band (v), a faint glyph rhythm
        // along it (u): the pulse lights the carving, not a flat sheet.
        std::vector<uint8_t> BandPixels(int w, int h) {
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const double v = (y + 0.5) / h;
                    const double u = (x + 0.5) / w;
                    // Five lines at the stave_stone texture's rows 3..11 of 16.
                    double line = 0.0;
                    for (int k = 0; k < 5; ++k) {
                        const double lv = (3.5 + 2.0 * k) / 16.0;
                        line = std::max(line, std::exp(-std::pow((v - lv) * 22.0, 2.0)));
                    }
                    // Periodic in u (the texture repeats along the wall).
                    const double glyph = 0.5 + 0.5 * std::sin(kTwoPi * 3.0 * u)
                                                   * std::sin(kTwoPi * 5.0 * u + 1.3);
                    // Soft at the band's top and bottom edges.
                    const double edge = std::pow(std::sin(kPi * v), 0.6);
                    const double a = edge * (0.30 + 0.55 * line + 0.15 * glyph * line);
                    const size_t o = (static_cast<size_t>(y) * w + x) * 4;
                    px[o + 0] = px[o + 1] = px[o + 2] = 255;
                    px[o + 3] = Alpha8(a);
                }
            }
            return px;
        }

        constexpr Rgb kHeadColour{230, 255, 255};
        constexpr Rgb kTailColour{ 95, 243, 255};   // the Chord's cyan
        constexpr Rgb kFarColour { 28, 150, 170};

        constexpr size_t kStreamSlots = 64;         // per-call buffers, reused round-robin

    } // namespace

    struct AurelithWallPulse::Impl {
        ShaderHandle  shader = INVALID_SHADER;
        TextureHandle texture = INVALID_TEXTURE;
        struct Slot {
            BufferHandle vb = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t       capacity = 0;              // vertices
        };
        std::array<Slot, kStreamSlots> slots{};
        size_t cursor = 0;
        std::vector<Vert> scratch;

        Slot& Acquire(size_t verts) {
            Slot& slot = slots[cursor];
            cursor = (cursor + 1) % kStreamSlots;
            if (slot.vb == INVALID_BUFFER || slot.capacity < verts) {
                size_t cap = std::max<size_t>(slot.capacity, 768);
                while (cap < verts) cap *= 2;
                // Deferred: a recorded draw may still read the old buffer.
                if (slot.mesh != INVALID_MESH) g_renderBackend->DeferredDestroyMesh(slot.mesh);
                if (slot.vb != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.vb);
                slot.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, cap * sizeof(Vert), nullptr,
                                                        BufferAccess::Streaming);
                slot.mesh = slot.vb != INVALID_BUFFER
                    ? g_renderBackend->CreateMesh(slot.vb, INVALID_BUFFER, GetBlockVertexLayout())
                    : INVALID_MESH;
                slot.capacity = slot.mesh != INVALID_MESH ? cap : 0;
            }
            return slot;
        }
    };

    AurelithWallPulse::~AurelithWallPulse() { Shutdown(); }

    bool AurelithWallPulse::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;
        m_impl = new Impl();
        m_impl->shader = BlockEntityShader::Create();
        m_impl->texture = MakeTexture(32, 16, BandPixels(32, 16));
        if (m_impl->texture != INVALID_TEXTURE) {
            // u runs along the wall (repeating glyph rhythm), v across the band.
            g_renderBackend->SetTextureWrap(m_impl->texture, TextureWrap::Repeat, TextureWrap::ClampToEdge);
        }
        if (m_impl->shader == INVALID_SHADER || m_impl->texture == INVALID_TEXTURE) {
            Log::Error("[AurelithWallPulse] shader or texture creation failed");
            Shutdown();
            return false;
        }
        (void)BandFaces();                           // build the table now, not on the first draw
        m_initialized = true;
        return true;
    }

    void AurelithWallPulse::Shutdown() {
        if (!m_impl) { m_initialized = false; return; }
        if (g_renderBackend) {
            // Renderer teardown: nothing is in flight any more, but the
            // deferred path is correct either way.
            for (auto& slot : m_impl->slots) {
                if (slot.mesh != INVALID_MESH) g_renderBackend->DeferredDestroyMesh(slot.mesh);
                if (slot.vb != INVALID_BUFFER) g_renderBackend->DeferredDestroyBuffer(slot.vb);
            }
            if (m_impl->texture != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(m_impl->texture);
            if (m_impl->shader != INVALID_SHADER) g_renderBackend->DestroyShader(m_impl->shader);
        }
        delete m_impl;
        m_impl = nullptr;
        m_initialized = false;
    }

    void AurelithWallPulse::DrawQuadrant(const glm::ivec3& heart, int worldDir, double ticks,
                                         double lightLevel, double voice,
                                         const glm::mat4& proj, const glm::mat4& view,
                                         const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.AurelithWallPulse");
        if (!m_initialized || !m_impl || !g_renderBackend) return;

        // The quadrant: bearings within ±45° of the gate's direction from the
        // Heart (north is -z: bearing -90°), half-open so the four tile.
        static constexpr double kDirBearing[4] = {-0.5 * kPi, 0.0, 0.5 * kPi, kPi};
        const double centre = kDirBearing[worldDir & 3];

        // The heads: one travelling swell, and a second opposite it as the
        // city wakes. The phase is seeded from the Heart, so no two cities
        // pulse in step.
        const uint64_t seed = Mix64(PositionSeed(heart) ^ 0x57414C4C50554C53ull);
        const double head0 = kTwoPi * Draw(seed, 0, 1) + kTwoPi * (ticks / kCircuitTicks);
        const double second = std::clamp(lightLevel, 0.0, 1.0);
        // Brightness: a soft swell while the city sleeps, full once it is
        // lit, with a slow shimmer as the Chord is sung.
        const double shimmer = 1.0 + 0.12 * voice * std::sin(ticks * 0.21 + 3.0 * Draw(seed, 0, 2));
        const double strength = (0.45 + 0.55 * second) * shimmer;

        auto swellAt = [&](double bearing, double radius) {
            double s = Swell(bearing, head0, radius);
            if (second > 0.0) s = std::max(s, second * Swell(bearing, head0 + kPi, radius));
            return s;
        };

        std::vector<Vert>& verts = m_impl->scratch;
        verts.clear();
        const float y0 = static_cast<float>(A::kStaveBandAboveHeart);
        const float y1 = y0 + 1.0f;
        for (const Face& f : BandFaces()) {
            const double off = WrapPi(f.bearing - centre);
            if (off < -0.25 * kPi || off >= 0.25 * kPi) continue;
            const double mid = swellAt(f.bearing, f.radius);
            const double sa = swellAt(f.bearingA, f.radius);
            const double sb = swellAt(f.bearingB, f.radius);
            if (std::max(mid, std::max(sa, sb)) < kCutoff) continue;
            auto colour = [&](double s) {
                return s > 0.5 ? Mix(kTailColour, kHeadColour, (s - 0.5) * 2.0)
                               : Mix(kFarColour, kTailColour, s * 2.0);
            };
            const Rgb ca = colour(sa), cb = colour(sb);
            const uint8_t aa = Alpha8(sa), ab = Alpha8(sb);
            // u along the face (the glyph rhythm), v up the band.
            // (The face straddling the bearing's wrap, due west, keeps its
            // second u beside its first.)
            const double uA = f.bearingA * f.radius * 0.25;
            double uB = f.bearingB * f.radius * 0.25;
            if (std::abs(uB - uA) > 1.0) uB = uA + (uB > uA ? -0.25 : 0.25);
            const float ua = static_cast<float>(uA);
            const float ub = static_cast<float>(uB);
            const Vert v0{f.a.x, y0, f.a.y, ua, 1.0f, ca.r, ca.g, ca.b, aa};
            const Vert v1{f.b.x, y0, f.b.y, ub, 1.0f, cb.r, cb.g, cb.b, ab};
            const Vert v2{f.b.x, y1, f.b.y, ub, 0.0f, cb.r, cb.g, cb.b, ab};
            const Vert v3{f.a.x, y1, f.a.y, ua, 0.0f, ca.r, ca.g, ca.b, aa};
            verts.insert(verts.end(), {v0, v1, v2, v0, v2, v3});
        }
        if (verts.empty()) return;

        Impl::Slot& slot = m_impl->Acquire(verts.size());
        if (slot.mesh == INVALID_MESH) return;
        g_renderBackend->UpdateBuffer(slot.vb, 0, verts.size() * sizeof(Vert), verts.data());

        // The faces are Heart-relative; the model matrix puts the Heart's
        // block corner in render space (RenderOrigin.hpp), so every vertex
        // stays a small float.
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), ToRender(glm::dvec3(heart)));
        g_renderBackend->SetPipelineState(GlowPipeline());
        g_renderBackend->BindShader(m_impl->shader);
        g_renderBackend->SetUniformFloat(m_impl->shader, "uAlphaTest", 0.0f);
        g_renderBackend->BindTexture(m_impl->texture, 0);
        g_renderBackend->SetUniformMat4(m_impl->shader, "uMVP", proj * view * model);
        BlockEntityShader::ApplyWorld(m_impl->shader, model, cameraPos);
        BlockEntityShader::SetLight(m_impl->shader,
                                    static_cast<float>(EntityEnvironment::kEmissive * strength));
        g_renderBackend->DrawArrays(slot.mesh, static_cast<uint32_t>(verts.size()), 0);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
