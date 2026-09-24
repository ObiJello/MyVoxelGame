// File: src/client/renderer/effects/VolumetricBeam.cpp
//
// See the header for the model. Layout of this file:
//   the uniform slots         (how a cone reaches both backends' shaders)
//   environment               (fog-driven density, the Darkness dim, fog culling)
//   geometry                  (the proxy frustum, the cube, the quad, the noise)
//   the terrain probe         (a DDA along the axis, cached per beam)
//   the depth snapshot        (OpenGL)
//   the draw
#include "VolumetricBeam.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/renderer/environment/MobEffectEnvironment.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Blocks.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace Render {

    namespace {

        // ── the uniform slots ─────────────────────────────────────────────
        // Every beam shader reads its per-draw parameters as seven vec4
        // "slots" P0..P6. On Vulkan a shader cannot declare uniforms of its
        // own: VKBackend routes each SetUniform* NAME into a fixed field of
        // the Common UBO (portal pipeline layout, set 1). The names below are
        // the portal renderer's; their UBO fields are read only by shaders
        // that set them on every draw (portal_vk, viewmodel_skinned_vk,
        // end_portal_vk's uTime), so borrowing them between draws is safe.
        // On OpenGL they are simply uniforms of those names.
        //
        //   slot  names (vec3 + float, or four floats)          cone meaning
        //   P0    uPortalColor + uPulse                          apex (render), length
        //   P1    uColorDark   + uOpenAmount                     axis, start radius
        //   P2    uColorHot    + uOpenAmountVS                   core colour, end radius
        //   P3    uKeyDir      + uKeyIntensity                   edge colour, intensity
        //   P4    uTime, uTimeVS, uStaticAmount, uColorScale     time, seed, mist, g
        //   P5    uPortalActive, uForceFarDepth, uOutlineMode,   haze, extinction,
        //         uFlashIntensity                                fade-in, half distance
        //   P6    uTint (vec4)                                   fog factor, fade-out start,
        //                                                        0, clip end
        //
        // The sprite shaders (glare, pool) reuse the slots — see their
        // setters — and take their model matrix as uMVP + uLocalToRender,
        // which on Vulkan are push constants. uLocalToRender's rows share the
        // push block with uColor / uScalars, which several of the names above
        // ALSO write, so it is always set last.
        //
        // OpenGL-only uniforms (Vulkan ignores unknown names): uSceneDepth,
        // uHasSceneDepth, uSceneViewProj, uPoolDeferred, uPoolRenderToBox,
        // uGlareProbe.

        constexpr double kPi = 3.14159265358979323846;

        // Must match shaders/beam_volume*.vert: the proxy is the cone's
        // radius × kProxyScale + kProxyPad (circumscribing the 16-gon, with
        // room for the soft rim), from s = −kProxyPad to clipEnd + kProxyPad.
        constexpr int   kProxySides = 16;
        constexpr float kProxyScale = 1.03f;
        constexpr float kProxyPad   = 0.08f;
        // How near the proxy the eye counts as inside it (no-depth path):
        // more than the near plane, so a front face never clips away.
        constexpr float kInsideMargin = 0.6f;

        // Where the cone ends past a terrain hit: with a depth snapshot the
        // scene ends every ray exactly and this only stops light leaking
        // behind the hill; without one it IS the end (the shader fades over
        // the last 1.75 blocks, kClipSoft in beam_volume.frag).
        constexpr float kClipPastHitDepth   = 2.0f;
        constexpr float kClipPastHitNoDepth = 0.5f;

        // The light pool: relight gain (dst · (1 + light · gain)), how far the
        // GL decal box reaches either side of the face that was hit, and the
        // no-depth quad's lift off it.
        constexpr float kPoolGain  = 2.5f;
        constexpr float kPoolDepth = 2.5f;
        constexpr float kPoolLift  = 0.02f;

        // A static beam's raycast is refreshed every this many views (world
        // edits); a moving one every view.
        constexpr uint32_t kProbeRefreshPasses = 8;
        constexpr uint32_t kProbeEvictPasses   = 512;

        // The mist's time wraps here. The drift velocities in the shader are
        // multiples of 1/16 block/s, so 16384 s of drift is a whole number of
        // the noise's 512- and 128-block tiles: the wrap is seamless.
        constexpr double kTimeWrap = 16384.0;

        constexpr int kNoiseSize = 64;

        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vert) == 24, "must match GetBlockVertexLayout");

        Vert V(float x, float y, float z) { return Vert{x, y, z, 0.0f, 0.0f, 255, 255, 255, 255}; }

        float SmoothStep(float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // ── environment ──────────────────────────────────────────────────
        // How much thicker than the open air the frame's atmosphere is. The
        // environmental fog end is the atmosphere (1024 in open air, 56 in
        // the Hush's caverns, ~560 in its stillness, ~100 underwater); the
        // render-distance fog is only the edge of the view and says nothing
        // about the air. Blindness and Darkness pull the fog in too, but
        // that is the eyes failing, not a thicker haze: no boost, and their
        // black fog then swallows the beam beyond a few blocks.
        float EnvironmentDensity(const EnvironmentFrame& env) {
            const MobEffectView& fx = GetMobEffectView();
            if (fx.blindness || fx.darkness) return 1.0f;
            const float visibility = std::clamp(env.fogEnvEnd - std::max(env.fogEnvStart, 0.0f), 16.0f, 1024.0f);
            return std::clamp(std::sqrt(1024.0f / visibility), 1.0f, 3.0f);
        }

        // The terrain's fog value at offset d from the eye — the shaders'
        // fogAt, for the CPU-side glare and culling.
        float FogAt(const EnvironmentFrame& env, const glm::vec3& d) {
            auto lin = [](float x, float s, float e) {
                if (x <= s) return 0.0f;
                if (x >= e) return 1.0f;
                return (x - s) / (e - s);
            };
            const float sph = glm::length(d);
            const float cyl = std::max(std::hypot(d.x, d.z), std::abs(d.y));
            return std::max(lin(sph, env.fogEnvStart, env.fogEnvEnd), lin(cyl, env.fogRdStart, env.fogRdEnd));
        }

        // Beyond this distance everything is fully fogged (cyl ≥ sph/√3).
        float FullyFoggedDistance(const EnvironmentFrame& env) {
            return std::min(env.fogEnvEnd, env.fogRdEnd * 1.7320508f);
        }

        // A light source is not lit by the lightmap (MC's beacon beam has
        // none), but Darkness's pulse takes light off every texel.
        float DarknessScale() {
            return std::max(0.0f, 1.0f - GetMobEffectView().darknessLightmap);
        }

        float MistTime() {
            using Clock = std::chrono::steady_clock;
            static const Clock::time_point start = Clock::now();
            const double sec = std::chrono::duration<double>(Clock::now() - start).count();
            return static_cast<float>(std::fmod(sec, kTimeWrap));
        }

        // The left/right/bottom/top planes of a render-space view-projection
        // (Gribb–Hartmann). Near and far are left out: the convention differs
        // per backend and the fog culls far cones anyway.
        struct Frustum {
            glm::vec4 planes[4];
            explicit Frustum(const glm::mat4& m) {
                const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
                const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
                const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
                planes[0] = r3 + r0; planes[1] = r3 - r0;
                planes[2] = r3 + r1; planes[3] = r3 - r1;
            }
            bool SphereVisible(const glm::vec3& c, float radius) const {
                for (const glm::vec4& p : planes) {
                    const float len = glm::length(glm::vec3(p));
                    if (glm::dot(glm::vec3(p), c) + p.w < -radius * len) return false;
                }
                return true;
            }
        };

        PipelineState BlendState(BlendFactor src, BlendFactor dst, bool depthTest, CullMode cull) {
            PipelineState s;
            s.depthTestEnabled  = depthTest;
            s.depthWriteEnabled = false;
            s.blendEnabled      = true;
            s.srcBlendFactor    = src;
            s.dstBlendFactor    = dst;
            s.cullMode          = cull;
            s.primitiveType     = PrimitiveType::Triangles;
            return s;
        }

        // ── geometry ─────────────────────────────────────────────────────
        // The proxy: a unit capped frustum. x is the axial fraction (0 at the
        // apex end, 1 at the far end), (y, z) the unit ring — the vertex
        // shader scales both. Wound counter-clockwise seen from OUTSIDE.
        void BuildProxy(std::vector<Vert>& v, std::vector<uint32_t>& idx) {
            for (int end = 0; end < 2; ++end) {
                for (int i = 0; i < kProxySides; ++i) {
                    const double a = 2.0 * kPi * i / kProxySides;
                    v.push_back(V(static_cast<float>(end), static_cast<float>(std::cos(a)),
                                  static_cast<float>(std::sin(a))));
                }
            }
            const uint32_t c0 = static_cast<uint32_t>(v.size()); v.push_back(V(0.0f, 0.0f, 0.0f));
            const uint32_t c1 = static_cast<uint32_t>(v.size()); v.push_back(V(1.0f, 0.0f, 0.0f));
            const uint32_t n = kProxySides;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t j = (i + 1) % n;
                // Side: (a0 b0 b1) (a0 b1 a1) — normal (0, cos, sin), outward.
                for (uint32_t k : {i, j, n + j, i, n + j, n + i}) idx.push_back(k);
                // Caps: outward −x at the apex end, +x at the far end.
                for (uint32_t k : {c0, j, i}) idx.push_back(k);
                for (uint32_t k : {c1, n + i, n + j}) idx.push_back(k);
            }
        }

        // [-1,1]³, outward counter-clockwise.
        void BuildCube(std::vector<Vert>& v, std::vector<uint32_t>& idx) {
            // Per face: outward normal axis/sign; four corners CCW seen from outside.
            const float s = 1.0f;
            const glm::vec3 faces[6][4] = {
                {{ s,-s,-s}, { s, s,-s}, { s, s, s}, { s,-s, s}},   // +X
                {{-s,-s,-s}, {-s,-s, s}, {-s, s, s}, {-s, s,-s}},   // -X
                {{-s, s,-s}, {-s, s, s}, { s, s, s}, { s, s,-s}},   // +Y
                {{-s,-s,-s}, { s,-s,-s}, { s,-s, s}, {-s,-s, s}},   // -Y
                {{-s,-s, s}, { s,-s, s}, { s, s, s}, {-s, s, s}},   // +Z
                {{-s,-s,-s}, {-s, s,-s}, { s, s,-s}, { s,-s,-s}},   // -Z
            };
            for (const auto& f : faces) {
                const uint32_t b = static_cast<uint32_t>(v.size());
                for (const glm::vec3& p : f) v.push_back(V(p.x, p.y, p.z));
                for (uint32_t k : {b, b + 1, b + 2, b, b + 2, b + 3}) idx.push_back(k);
            }
        }

        // The mist's noise: R is a hashed value per texel; G(x, y) =
        // R(x − 37, y − 17), so one bilinear fetch at slice z's offset
        // (37, 17)·z returns slice z in G and slice z+1 in R (the shader's
        // noise3 lerps them) — 3D value noise for one fetch. Tiles at 64.
        std::vector<uint8_t> NoisePixels() {
            auto hash = [](uint32_t x, uint32_t y) {
                uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ 0x5BD1E995u;
                h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
                return static_cast<uint8_t>(h >> 24);
            };
            std::vector<uint8_t> px(static_cast<size_t>(kNoiseSize) * kNoiseSize * 4);
            constexpr uint32_t mask = kNoiseSize - 1;
            for (uint32_t y = 0; y < static_cast<uint32_t>(kNoiseSize); ++y) {
                for (uint32_t x = 0; x < static_cast<uint32_t>(kNoiseSize); ++x) {
                    const size_t o = (static_cast<size_t>(y) * kNoiseSize + x) * 4;
                    px[o + 0] = hash(x, y);
                    px[o + 1] = hash((x - 37u) & mask, (y - 17u) & mask);
                    px[o + 2] = 0;
                    px[o + 3] = 255;
                }
            }
            return px;
        }

        // ── the terrain probe ────────────────────────────────────────────
        // What stops a beam: full opaque cubes. Glass (the lighthouse's own
        // lantern room), leaves, slabs and water let it through — the pool
        // then lands on the lake bed, seen through the water.
        bool StopsBeam(const Client::ClientBlockAccess& access, const glm::ivec3& c) {
            const Game::BlockState state = access.GetBlockState(c.x, c.y, c.z);
            const Game::BlockID id = state.Block();
            if (id == Game::BlockID::Air) return false;
            return Game::BlockRegistry::Get(id).opaque && Game::BlockRegistry::IsOcclusionFullCube(state);
        }

    } // namespace

    // ── lifetime ─────────────────────────────────────────────────────────

    VolumetricBeam& VolumetricBeam::Get() {
        // Never destroyed: its users are block-entity renderers owned by a
        // global dispatcher, whose destructors (static destruction, any
        // order) still call Release().
        static VolumetricBeam* instance = new VolumetricBeam();
        return *instance;
    }

    bool VolumetricBeam::Acquire() {
        VolumetricBeam& vb = Get();
        if (!vb.m_initialized && !vb.Initialize()) return false;
        ++vb.m_refs;
        return true;
    }

    void VolumetricBeam::Release() {
        VolumetricBeam& vb = Get();
        if (vb.m_refs <= 0) return;
        if (--vb.m_refs == 0) vb.Shutdown();
    }

    bool VolumetricBeam::BuildMesh(Mesh& out, const void* verts, size_t vertBytes,
                                   const uint32_t* indices, size_t indexCount) {
        out.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, vertBytes, verts);
        out.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, indexCount * sizeof(uint32_t), indices);
        if (out.vb == INVALID_BUFFER || out.ib == INVALID_BUFFER) return false;
        out.mesh = g_renderBackend->CreateMesh(out.vb, out.ib, GetBlockVertexLayout());
        out.indexCount = static_cast<uint32_t>(indexCount);
        return out.mesh != INVALID_MESH;
    }

    void VolumetricBeam::DestroyMesh(Mesh& m) {
        // Deferred: on Vulkan the last frame may still be drawing them.
        if (m.mesh != INVALID_MESH)   { g_renderBackend->DeferredDestroyMesh(m.mesh);  m.mesh = INVALID_MESH; }
        if (m.vb   != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m.vb); m.vb = INVALID_BUFFER; }
        if (m.ib   != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(m.ib); m.ib = INVALID_BUFFER; }
        m.indexCount = 0;
    }

    bool VolumetricBeam::Initialize() {
        if (!g_renderBackend) return false;
        if (m_initialized) return true;

        // All three read the frame's fog from the Common UBO on Vulkan and
        // take their MVP from the push constants (EntityEnvironment).
        m_volumeShader = EntityEnvironment::CreateShader("shaders/beam_volume.vert", "shaders/beam_volume.frag");
        m_glareShader  = EntityEnvironment::CreateShader("shaders/beam_sprite.vert", "shaders/beam_glare.frag");
        m_poolShader   = EntityEnvironment::CreateShader("shaders/beam_sprite.vert", "shaders/beam_pool.frag");
        if (m_volumeShader == INVALID_SHADER || m_glareShader == INVALID_SHADER ||
            m_poolShader == INVALID_SHADER) {
            Log::Error("[VolumetricBeam] shader compile failed");
            Shutdown();
            return false;
        }

        std::vector<Vert> verts;
        std::vector<uint32_t> idx;
        BuildProxy(verts, idx);
        const bool proxyOk = BuildMesh(m_proxy, verts.data(), verts.size() * sizeof(Vert), idx.data(), idx.size());
        verts.clear(); idx.clear();
        BuildCube(verts, idx);
        const bool cubeOk = BuildMesh(m_cube, verts.data(), verts.size() * sizeof(Vert), idx.data(), idx.size());
        const Vert quad[4] = {V(-1.0f, -1.0f, 0.0f), V(1.0f, -1.0f, 0.0f), V(1.0f, 1.0f, 0.0f), V(-1.0f, 1.0f, 0.0f)};
        const uint32_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
        const bool quadOk = BuildMesh(m_quad, quad, sizeof(quad), quadIdx, 6);
        if (!proxyOk || !cubeOk || !quadOk) {
            Log::Error("[VolumetricBeam] mesh creation failed");
            Shutdown();
            return false;
        }

        const std::vector<uint8_t> noise = NoisePixels();
        m_noiseTex = g_renderBackend->CreateTexture2D(kNoiseSize, kNoiseSize, TextureFormat::RGBA8, noise.data());
        if (m_noiseTex == INVALID_TEXTURE) {
            Log::Error("[VolumetricBeam] noise texture creation failed");
            Shutdown();
            return false;
        }
        g_renderBackend->SetTextureFilter(m_noiseTex, TextureFilter::Linear, TextureFilter::Linear);
        g_renderBackend->SetTextureWrap(m_noiseTex, TextureWrap::Repeat, TextureWrap::Repeat);

        const char* depthEnv = std::getenv("OBEY_BEAM_DEPTH");
        m_depthDisabled = depthEnv && std::string(depthEnv) == "0";
        if (m_depthDisabled) Log::Info("[VolumetricBeam] OBEY_BEAM_DEPTH=0: no scene-depth snapshot");

        m_initialized = true;
        return true;
    }

    void VolumetricBeam::Shutdown() {
        m_probes.clear();
        m_initialized = false;
        m_refs = 0;
        if (!g_renderBackend) return;
        DestroyMesh(m_proxy);
        DestroyMesh(m_cube);
        DestroyMesh(m_quad);
        if (m_noiseTex != INVALID_TEXTURE) { g_renderBackend->DeferredDestroyTexture(m_noiseTex); m_noiseTex = INVALID_TEXTURE; }
        if (m_depthTex != INVALID_TEXTURE) { g_renderBackend->DeferredDestroyTexture(m_depthTex); m_depthTex = INVALID_TEXTURE; }
        m_depthW = m_depthH = 0;
        m_depthValid = false;
        for (ShaderHandle* s : {&m_volumeShader, &m_glareShader, &m_poolShader}) {
            if (*s != INVALID_SHADER) { g_renderBackend->DestroyShader(*s); *s = INVALID_SHADER; }
        }
    }

    void VolumetricBeam::BeginView() {
        ++m_pass;
        if (m_pass == 0) m_pass = 1;   // 0 is "never" for the snapshot
        // Forget the probes of beams that stopped drawing (a lamp broken or
        // out of range) now and then.
        if ((m_pass & 255u) == 0 && !m_probes.empty()) {
            for (auto it = m_probes.begin(); it != m_probes.end();) {
                if (m_pass - it->second.pass > kProbeEvictPasses) it = m_probes.erase(it);
                else ++it;
            }
        }
    }

    // ── helpers ──────────────────────────────────────────────────────────

    glm::dvec3 VolumetricBeam::EyeFromView(const glm::mat4& view) {
        // The view is render space: the inverse's translation is the eye's
        // offset from the render origin, small and exact.
        return ToWorld(glm::vec3(glm::inverse(view)[3]));
    }

    float VolumetricBeam::Flare(const BeamCone& cone, const glm::dvec3& eye, float innerDeg, float outerDeg) {
        const glm::dvec3 to = eye - cone.apex;
        const double dist = glm::length(to);
        const double dirLen = glm::length(glm::dvec3(cone.direction));
        if (dist < 1e-3 || dirLen < 1e-6) return 0.0f;
        const double facing = glm::dot(glm::dvec3(cone.direction) / dirLen, to / dist);
        const double lo = std::cos(glm::radians(static_cast<double>(outerDeg)));
        const double hi = std::cos(glm::radians(static_cast<double>(innerDeg)));
        return SmoothStep(static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(facing));
    }

    // ── the terrain probe ────────────────────────────────────────────────

    VolumetricBeam::TerrainHit VolumetricBeam::ProbeTerrain(const BeamCone& cone, const glm::vec3& dir) {
        const float start = std::max(cone.terrainStart, 0.0f);
        if (cone.cacheKey != 0) {
            auto it = m_probes.find(cone.cacheKey);
            if (it != m_probes.end()) {
                const ProbeEntry& e = it->second;
                const bool still = glm::length(e.apex - cone.apex) < 1e-4 &&
                                   glm::dot(e.dir, dir) > 0.9999999f &&
                                   e.length == cone.length && e.start == start;
                if (still && m_pass - e.pass < kProbeRefreshPasses) return e.result;
            }
        }

        // Amanatides–Woo: visit every cell the axis passes through, from the
        // `start` point to the end of the beam, in order.
        TerrainHit out;
        const Client::ClientBlockAccess* access = Client::g_clientBlockAccess;
        const double span = static_cast<double>(cone.length) - start;
        if (access && span > 0.0) {
            PROFILE_ZONE_N("VolumetricBeam.Probe");
            const glm::dvec3 d(dir);
            const glm::dvec3 origin = cone.apex + d * static_cast<double>(start);
            glm::ivec3 cell(static_cast<int>(std::floor(origin.x)), static_cast<int>(std::floor(origin.y)),
                            static_cast<int>(std::floor(origin.z)));
            glm::ivec3 step(0);
            glm::dvec3 tMax(std::numeric_limits<double>::infinity());
            glm::dvec3 tDelta(std::numeric_limits<double>::infinity());
            for (int i = 0; i < 3; ++i) {
                if (d[i] > 1e-12) {
                    step[i] = 1;
                    tMax[i] = (cell[i] + 1.0 - origin[i]) / d[i];
                    tDelta[i] = 1.0 / d[i];
                } else if (d[i] < -1e-12) {
                    step[i] = -1;
                    tMax[i] = (origin[i] - cell[i]) / -d[i];
                    tDelta[i] = -1.0 / d[i];
                }
            }
            double t = 0.0;
            int enteredAxis = -1;
            for (int guard = 0; guard < 1024; ++guard) {
                if (StopsBeam(*access, cell)) {
                    out.hit = true;
                    out.s = static_cast<float>(start + t);
                    out.point = origin + d * t;
                    if (enteredAxis < 0) {
                        // Started inside a block: the face the axis mostly faces.
                        const glm::dvec3 a = glm::abs(d);
                        enteredAxis = (a.x > a.y && a.x > a.z) ? 0 : (a.y > a.z ? 1 : 2);
                    }
                    out.normal = glm::vec3(0.0f);
                    out.normal[enteredAxis] = d[enteredAxis] > 0.0 ? -1.0f : 1.0f;
                    break;
                }
                const int axis = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
                t = tMax[axis];
                if (t > span) break;
                cell[axis] += step[axis];
                tMax[axis] += tDelta[axis];
                enteredAxis = axis;
            }
        }

        if (cone.cacheKey != 0) {
            ProbeEntry& e = m_probes[cone.cacheKey];
            e.apex = cone.apex;
            e.dir = dir;
            e.length = cone.length;
            e.start = start;
            e.pass = m_pass;
            e.result = out;
        }
        return out;
    }

    // ── the depth snapshot ───────────────────────────────────────────────

    bool VolumetricBeam::EnsureSceneDepth(const glm::mat4& viewProj) {
        // One copy per view: the pass counter moves with BeginView, and a
        // caller that never calls it still gets a fresh copy whenever the
        // camera moves.
        if (m_depthPass == m_pass && m_depthViewProj == viewProj) return m_depthValid;
        m_depthPass = m_pass;
        m_depthViewProj = viewProj;
        m_depthValid = false;
        if (m_depthDisabled || g_renderBackend->GetType() != BackendType::OpenGL) return false;

        int w = 0, h = 0;
        glfwGetFramebufferSize(g_renderBackend->GetWindow(), &w, &h);
        if (w <= 0 || h <= 0) return false;
        if (m_depthTex == INVALID_TEXTURE || w != m_depthW || h != m_depthH) {
            if (m_depthTex != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(m_depthTex);
            m_depthTex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::Depth24Stencil8, nullptr);
            m_depthW = w;
            m_depthH = h;
            if (m_depthTex == INVALID_TEXTURE) return false;
            g_renderBackend->SetTextureFilter(m_depthTex, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap(m_depthTex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        PROFILE_ZONE_N("VolumetricBeam.DepthCopy");
        m_depthValid = g_renderBackend->CopyFramebufferDepthToTexture(m_depthTex);
        return m_depthValid;
    }

    // ── the draw ─────────────────────────────────────────────────────────

    namespace {

        // Per cone, what the draw needs past the caller's description.
        struct Prepared {
            const BeamCone* cone = nullptr;
            glm::vec3 dir{0.0f};
            glm::vec3 apexR{0.0f};      // render space
            float     r0 = 0.0f, r1 = 0.0f, length = 0.0f;
            float     clipEnd = 0.0f;   // where the cone stops (≤ length)
            float     hitS = 0.0f;
            bool      hit = false;
            glm::dvec3 hitPoint{0.0};
            glm::vec3  hitNormal{0.0f};
        };

        struct Globals {
            float envDensity = 1.0f;
            float lightScale = 1.0f;
            float time = 0.0f;
        };

        void SetConeParams(ShaderHandle sh, const Prepared& p, const Globals& g) {
            RenderBackend& b = *g_renderBackend;
            const BeamCone& c = *p.cone;
            b.SetUniformVec3(sh, "uPortalColor", p.apexR);
            b.SetUniformFloat(sh, "uPulse", p.length);
            b.SetUniformVec3(sh, "uColorDark", p.dir);
            b.SetUniformFloat(sh, "uOpenAmount", p.r0);
            b.SetUniformVec3(sh, "uColorHot", c.coreColor);
            b.SetUniformFloat(sh, "uOpenAmountVS", p.r1);
            b.SetUniformVec3(sh, "uKeyDir", c.edgeColor);
            b.SetUniformFloat(sh, "uKeyIntensity", std::max(c.intensity, 0.0f) * g.lightScale);
            b.SetUniformFloat(sh, "uTime", g.time);
            b.SetUniformFloat(sh, "uTimeVS", static_cast<float>(c.seed & 0xFFFFu) * (64.0f / 65536.0f));
            b.SetUniformFloat(sh, "uStaticAmount", std::clamp(c.mist, 0.0f, 1.0f));
            b.SetUniformFloat(sh, "uColorScale", std::clamp(c.anisotropy, -0.95f, 0.95f));
            b.SetUniformFloat(sh, "uPortalActive", std::max(c.haze, 0.0f) * g.envDensity);
            b.SetUniformFloat(sh, "uForceFarDepth", std::max(c.extinction, 0.0f) * g.envDensity);
            b.SetUniformFloat(sh, "uOutlineMode", std::max(c.fadeIn, 0.0f));
            b.SetUniformFloat(sh, "uFlashIntensity", std::max(c.halfIntensityDistance, 0.5f));
            b.SetUniformVec4(sh, "uTint", glm::vec4(std::clamp(c.fogFactor, 0.0f, 1.0f),
                                                    std::clamp(c.fadeOutStart, 0.0f, 0.999f),
                                                    0.0f, p.clipEnd));
        }

        // A sprite's model matrix: set LAST (see the slot table).
        void SetSpriteModel(ShaderHandle sh, const glm::mat4& viewProj, const glm::mat4& model) {
            g_renderBackend->SetUniformMat4(sh, "uMVP", viewProj * model);
            g_renderBackend->SetUniformMat4(sh, "uLocalToRender", model);
        }

        // The shader-wide state every beam shader needs: fog, samplers, the
        // GL depth snapshot.
        void BindCommon(ShaderHandle sh, const glm::dvec3& eye, const glm::mat4& viewProj,
                        TextureHandle noise, TextureHandle depth, bool hasDepth) {
            RenderBackend& b = *g_renderBackend;
            b.BindShader(sh);
            EntityEnvironment::ApplyWorld(sh, eye);
            b.SetUniformInt(sh, "uNoiseTex", 0);
            b.SetUniformInt(sh, "uSceneDepth", 1);
            b.SetUniformFloat(sh, "uHasSceneDepth", hasDepth ? 1.0f : 0.0f);
            b.SetUniformMat4(sh, "uSceneViewProj", viewProj);
            // Unit 1 only when there is a snapshot (OpenGL): on Vulkan a slot-1
            // texture would linger as the next portal-layout draw's set 2.
            if (hasDepth) b.BindTexture(depth, 1);
            b.BindTexture(noise, 0);   // last, so the active GL unit is 0
        }

    } // namespace

    void VolumetricBeam::Draw(const BeamCone* cones, size_t coneCount,
                              const BeamGlare* glares, size_t glareCount,
                              const glm::mat4& proj, const glm::mat4& view) {
        PROFILE_ZONE_N("VolumetricBeam");
        if (!m_initialized || !g_renderBackend) return;
        if ((!cones || coneCount == 0) && (!glares || glareCount == 0)) return;

        const glm::mat4 viewProj = proj * view;
        const glm::dvec3 eye = EyeFromView(view);
        const glm::vec3 eyeR = ToRender(eye);
        const EnvironmentFrame& env = EnvironmentState::Get().Frame();
        const Frustum frustum(viewProj);
        const float fullFog = FullyFoggedDistance(env);

        Globals globals;
        globals.envDensity = EnvironmentDensity(env);
        globals.lightScale = DarknessScale();
        globals.time = MistTime();
        if (globals.lightScale <= 0.0f) return;

        // ── cull and prepare the cones ────────────────────────────────────
        std::vector<Prepared> prepared;
        prepared.reserve(coneCount);
        for (size_t i = 0; cones && i < coneCount; ++i) {
            const BeamCone& c = cones[i];
            const float dirLen = glm::length(c.direction);
            if (dirLen < 1e-6f || c.length < 0.25f || c.intensity <= 0.0f) continue;
            Prepared p;
            p.cone = &c;
            p.dir = c.direction / dirLen;
            p.length = c.length;
            p.r0 = std::max(c.startRadius, 0.0f);
            p.r1 = std::max(c.endRadius, p.r0);
            p.apexR = ToRender(c.apex);
            p.clipEnd = c.length;

            // The bounding sphere of the whole cone (before any terrain clip).
            const float rMax = p.r1 * kProxyScale + kProxyPad;
            const glm::vec3 mid = p.apexR + p.dir * (0.5f * c.length);
            const float radius = std::hypot(0.5f * c.length, rMax);
            if (!frustum.SphereVisible(mid, radius)) continue;
            // Wholly past the fog's end: nothing of it can show.
            if (c.fogFactor >= 0.999f) {
                const glm::vec3 rel = eyeR - p.apexR;
                const float s = std::clamp(glm::dot(rel, p.dir), 0.0f, c.length);
                const float nearest = glm::length(rel - p.dir * s) - rMax;
                if (nearest >= fullFog) continue;
            }
            prepared.push_back(p);
        }

        bool hasDepth = false;
        if (!prepared.empty() || (glares && glareCount > 0)) hasDepth = EnsureSceneDepth(viewProj);

        for (Prepared& p : prepared) {
            const BeamCone& c = *p.cone;
            if (!c.terrainClip && !c.lightPool) continue;
            const TerrainHit hit = ProbeTerrain(c, p.dir);
            if (!hit.hit) continue;
            p.hit = true;
            p.hitS = hit.s;
            p.hitPoint = hit.point;
            p.hitNormal = hit.normal;
            if (c.terrainClip) {
                p.clipEnd = std::min(c.length, hit.s + (hasDepth ? kClipPastHitDepth : kClipPastHitNoDepth));
            }
        }

        RenderBackend& b = *g_renderBackend;

        // ── 1. the light pools (relight the terrain before the haze adds) ─
        bool poolBound = false;
        for (const Prepared& p : prepared) {
            const BeamCone& c = *p.cone;
            if (!p.hit || !c.lightPool || c.poolIntensity <= 0.0f) continue;
            if (p.hitS >= c.length) continue;
            if (!poolBound) {
                b.SetPipelineState(hasDepth
                    ? BlendState(BlendFactor::DstColor, BlendFactor::One, false, CullMode::Front)
                    : [] {
                          PipelineState s = BlendState(BlendFactor::DstColor, BlendFactor::One, true, CullMode::None);
                          s.depthBiasEnabled  = true;
                          s.depthBiasConstant = -2.0f;
                          s.depthBiasSlope    = -2.0f;
                          return s;
                      }());
                BindCommon(m_poolShader, eye, viewProj, m_noiseTex, m_depthTex, hasDepth);
                b.SetUniformFloat(m_poolShader, "uPoolDeferred", hasDepth ? 1.0f : 0.0f);
                poolBound = true;
            }

            // The splash's frame on the face that was hit: e1 along the
            // beam's run over the face, e2 across it. A grazing beam's pool
            // is long — r / cos(incidence) — so e1's reach grows with it.
            const glm::vec3 n = p.hitNormal;
            const float cosI = std::abs(glm::dot(p.dir, n));
            glm::vec3 e1 = p.dir - n * glm::dot(p.dir, n);
            if (glm::length(e1) < 1e-3f) e1 = std::abs(n.y) < 0.9f ? glm::cross(n, glm::vec3(0, 1, 0)) : glm::vec3(1, 0, 0);
            e1 = glm::normalize(e1);
            const glm::vec3 e2 = glm::cross(n, e1);
            const float k = (p.r1 - p.r0) / p.length;
            const float rHit = p.r0 + k * p.hitS;
            const float across = rHit * 1.15f + 0.4f;
            const float along  = std::min(rHit / std::max(cosI, 0.12f) * 1.3f + 0.4f, 24.0f);

            glm::mat4 model(1.0f);
            model[0] = glm::vec4(e1 * along, 0.0f);
            model[1] = glm::vec4(e2 * across, 0.0f);
            if (hasDepth) {
                model[2] = glm::vec4(n * kPoolDepth, 0.0f);
                model[3] = glm::vec4(ToRender(p.hitPoint), 1.0f);
                b.SetUniformMat4(m_poolShader, "uPoolRenderToBox", glm::inverse(model));
            } else {
                model[2] = glm::vec4(n, 0.0f);
                model[3] = glm::vec4(ToRender(p.hitPoint + glm::dvec3(n) * static_cast<double>(kPoolLift)), 1.0f);
            }

            SetConeParams(m_poolShader, p, globals);
            // P4 carries the face normal and the gain instead of the mist.
            b.SetUniformFloat(m_poolShader, "uTime", n.x);
            b.SetUniformFloat(m_poolShader, "uTimeVS", n.y);
            b.SetUniformFloat(m_poolShader, "uStaticAmount", n.z);
            b.SetUniformFloat(m_poolShader, "uColorScale", kPoolGain * c.poolIntensity);
            SetSpriteModel(m_poolShader, viewProj, model);
            const Mesh& mesh = hasDepth ? m_cube : m_quad;
            b.DrawIndexed(mesh.mesh, mesh.indexCount);
        }

        // ── 2. the cones ──────────────────────────────────────────────────
        if (!prepared.empty()) {
            BindCommon(m_volumeShader, eye, viewProj, m_noiseTex, m_depthTex, hasDepth);
            b.SetUniformMat4(m_volumeShader, "uMVP", viewProj);
            int lastMode = -1;
            for (const Prepared& p : prepared) {
                // With depth: back faces, untested — the shader ends every ray
                // at the scene. Without: front faces depth-tested from
                // outside; from inside (no front faces left) the back faces,
                // untested, the axis probe standing in for the scene.
                int mode = 0;
                if (!hasDepth) {
                    const glm::dvec3 q = eye - p.cone->apex;
                    const double s = glm::dot(q, glm::dvec3(p.dir));
                    bool inside = s > -(kProxyPad + kInsideMargin) && s < p.clipEnd + kProxyPad + kInsideMargin;
                    if (inside) {
                        const double sc = std::clamp(s, 0.0, static_cast<double>(p.length));
                        const double r = (p.r0 + (p.r1 - p.r0) * sc / p.length) * kProxyScale + kProxyPad + kInsideMargin;
                        inside = glm::dot(q, q) - s * s <= r * r;
                    }
                    mode = inside ? 0 : 1;
                }
                if (mode != lastMode) {
                    b.SetPipelineState(mode == 0
                        ? BlendState(BlendFactor::One, BlendFactor::One, false, CullMode::Front)
                        : BlendState(BlendFactor::One, BlendFactor::One, true, CullMode::Back));
                    lastMode = mode;
                }
                SetConeParams(m_volumeShader, p, globals);
                b.DrawIndexed(m_proxy.mesh, m_proxy.indexCount);
            }
        }

        // ── 3. the glares ─────────────────────────────────────────────────
        if (glares && glareCount > 0) {
            bool glareBound = false;
            // Camera right / up out of the view matrix's rotation rows.
            const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
            const glm::vec3 up   (view[0][1], view[1][1], view[2][1]);
            const glm::vec3 back (view[0][2], view[1][2], view[2][2]);
            for (size_t i = 0; i < glareCount; ++i) {
                const BeamGlare& g = glares[i];
                if (g.intensity <= 0.0f || g.size <= 0.0f) continue;
                const glm::vec3 pos = ToRender(g.position);
                const glm::vec3 rel = pos - eyeR;
                if (glm::length(rel) < 0.3f) continue;
                const float T = 1.0f - FogAt(env, rel) * std::clamp(g.fogFactor, 0.0f, 1.0f);
                const float strength = g.intensity * T * globals.lightScale;
                if (strength <= 0.003f) continue;

                const float aspect = g.streak > 0.0f ? std::max(1.0f, g.streakLength) : 1.0f;
                if (!frustum.SphereVisible(pos, g.size * aspect)) continue;

                if (!glareBound) {
                    b.SetPipelineState(BlendState(BlendFactor::One, BlendFactor::One, !hasDepth, CullMode::None));
                    BindCommon(m_glareShader, eye, viewProj, m_noiseTex, m_depthTex, hasDepth);
                    glareBound = true;
                }

                // The occlusion probe (OpenGL): the source's window position
                // and depth, tested against the snapshot in the shader.
                glm::vec4 probe(0.0f);
                if (hasDepth) {
                    const glm::vec4 clip = viewProj * glm::vec4(pos, 1.0f);
                    if (clip.w <= 1e-4f) continue;          // behind the eye
                    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    probe = glm::vec4(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f, ndc.z * 0.5f + 0.5f, 1.0f);
                }
                b.SetUniformVec4(m_glareShader, "uGlareProbe", probe);
                b.SetUniformVec3(m_glareShader, "uColorHot", g.color);
                b.SetUniformFloat(m_glareShader, "uOpenAmountVS", aspect);
                b.SetUniformFloat(m_glareShader, "uKeyIntensity", strength);
                b.SetUniformFloat(m_glareShader, "uTimeVS", std::max(g.streak, 0.0f));

                glm::mat4 model(1.0f);
                model[0] = glm::vec4(right * (g.size * aspect), 0.0f);
                model[1] = glm::vec4(up * g.size, 0.0f);
                model[2] = glm::vec4(back, 0.0f);
                model[3] = glm::vec4(pos, 1.0f);
                SetSpriteModel(m_glareShader, viewProj, model);
                b.DrawIndexed(m_quad.mesh, m_quad.indexCount);
            }
        }

        b.UnbindMesh();
    }

} // namespace Render
