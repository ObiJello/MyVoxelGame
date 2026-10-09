// File: src/client/renderer/environment/WeatherEffectRenderer.cpp
//
// MC WeatherEffectRenderer — see the header.
#include "WeatherEffectRenderer.hpp"
#include "client/renderer/core/DevRenderSkip.hpp"

#include "EntityEnvironment.hpp"
#include "EnvironmentState.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/LateDepthBands.hpp"
#include "../core/RenderOrigin.hpp"
#include "../mesh/ChunkRenderer.hpp"   // PortalClipPlane
#include "../particle/MobParticleSystem.hpp"
#include "../post/ImprovedTransparency.hpp"
#include "client/renderer/core/WorldFramebuffer.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientWeather.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "platform/GameDirectory.hpp"
#include "stb_image.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    WeatherEffectRenderer g_weatherEffectRenderer;

    namespace {

        // OpenGL twin of shaders/weather_half_vk.frag (see there): the
        // particle fragment shader with the world's depth test against the
        // farthest of the four full-resolution texels and premultiplied
        // output. Runs after MobParticleSystem's vertex shader.
        const char* const kHalfFragSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vRenderPos;
out vec4 FragColor;

uniform sampler2D uSprite;
uniform sampler2D uSceneDepth;   // FBO 0's depth, copied (texture slot 1)
uniform vec3 uEntityLight;
uniform vec3 uCameraPos;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 c = texture(uSprite, vUV) * vColor;
    if (c.a < 0.1) discard;
    ivec2 full = ivec2(gl_FragCoord.xy) * 2;
    ivec2 last = textureSize(uSceneDepth, 0) - 1;
    float scene = max(max(texelFetch(uSceneDepth, min(full, last), 0).r,
                          texelFetch(uSceneDepth, min(full + ivec2(1, 0), last), 0).r),
                      max(texelFetch(uSceneDepth, min(full + ivec2(0, 1), last), 0).r,
                          texelFetch(uSceneDepth, min(full + ivec2(1, 1), last), 0).r));
    if (gl_FragCoord.z > scene) discard;
    c.rgb *= uEntityLight;
    vec3 fogDelta = vRenderPos - uCameraPos;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, uFogEnv.x, uFogEnv.y),
                         linearFog(cyl, uFogEnv.z, uFogEnv.w));
    c.rgb = mix(c.rgb, uFogColor.rgb, fogValue * uFogColor.a);
    FragColor = vec4(c.rgb * c.a, c.a);
}
)";

        // Java int arithmetic: wraps on overflow (the column hashes rely on it).
        int32_t Mul(int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
        }
        int32_t Add(int32_t a, int32_t b) {
            return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
        }

        // The decoded RGBA of a weather texture (stbi_image_free it), or null.
        unsigned char* DecodeWeatherTexture(const char* relPath, int& w, int& h) {
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) {
                Log::Warning("WeatherEffectRenderer: missing texture %s", full.c_str());
                return nullptr;
            }
            int ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) Log::Warning("WeatherEffectRenderer: cannot decode %s", full.c_str());
            return pixels;
        }

        TextureHandle MakeWeatherTexture(const unsigned char* pixels, int w, int h) {
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            if (t != INVALID_TEXTURE) {
                // The V coordinate runs to y / 4 blocks and scrolls without
                // bound, and snow's U drifts: the texture repeats, nearest.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::Repeat, TextureWrap::Repeat);
            }
            return t;
        }

        // OpenGL twin of shaders/weather_streak_vk.vert (see there for the
        // contract); the fragment shaders are the quads'.
        const char* const kStreakVertSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;      // u0, v0, copy
layout(location = 1) in vec2 aUV;       // u1, v1
layout(location = 2) in vec4 aColor;    // r = corner / 255
layout(location = 3) in vec4 aColumn;   // cx, cz, hx, hz
layout(location = 4) in vec4 aSpan;     // bottomY, topY, uOffset, vOffset
layout(location = 5) in vec4 aTint;
layout(location = 6) in vec4 aRepeat;   // R

uniform mat4 uMVP;
uniform vec4 uEntityClipPlane;

out vec2 vUV;
out vec4 vColor;
out vec3 vRenderPos;

void main() {
    int corner = int(aColor.r * 255.0 + 0.5);
    float u0 = aPos.x, v0 = aPos.y, u1 = aUV.x, v1 = aUV.y;
    float uOffset = aSpan.z, vOffset = aSpan.w;
    float w = fract(u0 - uOffset);
    float dw = u1 - u0;
    float fx0, fx1, tu0;
    if (aPos.z < 0.5) { fx0 = w; fx1 = min(w + dw, 1.0); tu0 = u0; }
    else { fx0 = 0.0; fx1 = max(w + dw - 1.0, 0.0); tu0 = u0 + (1.0 - w); }
    float sum = aSpan.x + aSpan.y;
    float yHi = clamp(sum - 4.0 * (aRepeat.x + v0 - vOffset), aSpan.x, aSpan.y);
    float yLo = clamp(sum - 4.0 * (aRepeat.x + v1 - vOffset), aSpan.x, aSpan.y);
    bool top = corner == 0 || corner == 1;
    bool right = corner == 1 || corner == 2;
    float fx = right ? fx1 : fx0;
    float y = top ? yHi : yLo;
    float across = 2.0 * fx - 1.0;
    vec3 pos = vec3(aColumn.x + across * aColumn.z, y, aColumn.y + across * aColumn.w);
    gl_Position = uMVP * vec4(pos, 1.0);
    vUV = vec2(tu0 + (fx - fx0), (sum - y) * 0.25 + vOffset);
    vColor = aTint;
    vRenderPos = pos;
    gl_ClipDistance[0] = (any(notEqual(uEntityClipPlane.xyz, vec3(0.0))))
        ? dot(uEntityClipPlane.xyz, pos) + uEntityClipPlane.w
        : 1.0;
}
)";

    } // namespace

    WeatherEffectRenderer::WeatherEffectRenderer() {
        // MC's constructor: for each of the 32 x 32 cells around the camera
        // column, the unit vector perpendicular to the direction to it —
        // (-dz, dx) / |d|. The camera's own cell divides 0 by 0 (NaN, as in
        // Java); Render skips that column, which draws nothing either way.
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 32; ++x) {
                const float deltaX = static_cast<float>(x - 16);
                const float deltaZ = static_cast<float>(z - 16);
                const float distance = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
                m_columnSizeX[static_cast<size_t>(z * 32 + x)] = -deltaZ / distance;
                m_columnSizeZ[static_cast<size_t>(z * 32 + x)] = deltaX / distance;
            }
        }
    }

    WeatherEffectRenderer::~WeatherEffectRenderer() = default;

    bool WeatherEffectRenderer::HalfResolutionAvailable() {
        // Metal: Full is the faster path (see the header) — the setting is
        // ignored and the control hidden; the other backends keep it.
        // OBEY_RAIN_HALF=1 lets Metal take the setting again, for the A/B.
        static const bool s_forceAvailable = std::getenv("OBEY_RAIN_HALF") != nullptr;
        return s_forceAvailable || !g_renderBackend || g_renderBackend->GetType() != BackendType::Metal;
    }

    bool WeatherEffectRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // MC's WEATHER pipeline runs the particle shader (PARTICLE_SNIPPET).
        if (g_renderBackend->GetType() == BackendType::OpenGL) {
            m_shader = g_renderBackend->CreateShader(MobParticleSystem::VertexSource(),
                                                     MobParticleSystem::FragmentSource());
        } else {
            m_shader = EntityEnvironment::CreateShader("shaders/mob_particle.vert", "shaders/mob_particle.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[WeatherEffectRenderer] failed to create the particle shader for %s",
                         g_renderBackend->GetName());
            return false;
        }
        if (const char* streaks = std::getenv("OBEY_RAIN_STREAKS")) m_streaksWanted = std::strcmp(streaks, "0") != 0;
        if (m_streaksWanted && !CreateStreakShaders()) m_streaksWanted = false;
        LoadTextures();
        return true;
    }

    bool WeatherEffectRenderer::CreateStreakShaders() {
        RenderBackend& b = *g_renderBackend;
        // Locations 3..6: four vec4 per instance (StreakInstance).
        m_streakInstanceLayout = VertexLayout{};
        m_streakInstanceLayout.stride = sizeof(StreakInstance);
        for (uint32_t k = 0; k < 4; ++k) {
            VertexAttribute attr;
            attr.location        = 3 + k;
            attr.componentCount  = 4;
            attr.offset          = k * 16;
            attr.type            = AttribType::Float;
            attr.instanceDivisor = 1;
            m_streakInstanceLayout.attributes.push_back(attr);
        }
        if (b.GetType() == BackendType::OpenGL) {
            m_streakShader = b.CreateShader(kStreakVertSource, MobParticleSystem::FragmentSource());
        } else {
            m_streakShader = EntityEnvironment::CreateShader("shaders/weather_streak.vert", "shaders/mob_particle.frag");
            if (m_streakShader != INVALID_SHADER) {
                b.RegisterShaderVertexLayout(m_streakShader, GetBlockVertexLayout());
                b.RegisterShaderInstanceLayout(m_streakShader, m_streakInstanceLayout);
            }
        }
        if (m_streakShader == INVALID_SHADER) {
            Log::Warning("[WeatherEffectRenderer] streak shader unavailable - drawing MC's column quads");
            return false;
        }
        return true;
    }

    void WeatherEffectRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroySlots();
        DestroyTextures(/*deferred=*/false);
        DestroyStreakSet(m_rainStreaks, /*deferred=*/false);
        DestroyStreakSet(m_snowStreaks, /*deferred=*/false);
        m_instances.clear();
        if (m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        if (m_streakShader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_streakShader);
            m_streakShader = INVALID_SHADER;
        }
        ReleaseHalfResources();
        g_renderBackend->SetFrameDepthPreserved(false, RenderBackend::FrameDepthUser::Weather);
        g_renderBackend->SetFrameDepthCapable(false);
        g_renderBackend->SetDepthHandoff(false);
        m_aheadFrame = 0;
        m_viewFrame = 0;
        m_rainColumns.clear();
        m_snowColumns.clear();
        m_vertices.clear();
    }

    void WeatherEffectRenderer::ReloadTextures() {
        if (!g_renderBackend || m_shader == INVALID_SHADER) return;
        LoadTextures();
    }

    void WeatherEffectRenderer::LoadTextures() {
        DestroyTextures(/*deferred=*/true);
        DestroyStreakSet(m_rainStreaks, /*deferred=*/true);
        DestroyStreakSet(m_snowStreaks, /*deferred=*/true);
        ++m_streakGeneration;   // the slots' instanced meshes point at the old templates
        struct Load { const char* path; TextureHandle* texture; StreakSet* streaks; int copies; };
        // Rain's u offset is always 0 (one copy of every rect); snow's u
        // drifts per column, so a rect may wrap the column's edge (two).
        const Load loads[] = {
            {"assets/textures/environment/rain.png", &m_rainTexture, &m_rainStreaks, 1},
            {"assets/textures/environment/snow.png", &m_snowTexture, &m_snowStreaks, 2},
        };
        for (const Load& load : loads) {
            int w = 0, h = 0;
            unsigned char* pixels = DecodeWeatherTexture(load.path, w, h);
            if (!pixels) continue;
            *load.texture = MakeWeatherTexture(pixels, w, h);
            if (m_streaksWanted) BuildStreakSet(*load.streaks, pixels, w, h, load.copies);
            stbi_image_free(pixels);
        }
        if (m_streaksWanted) {
            const auto area = [](const StreakSet& set) {
                size_t a = 0;
                for (const StreakRect& r : set.rects) a += static_cast<size_t>(r.w) * r.h;
                return a;
            };
            Log::Info("[WeatherEffectRenderer] streaks: rain %zu rects (%zu texels)%s, snow %zu rects (%zu texels)%s",
                      m_rainStreaks.rects.size(), area(m_rainStreaks), m_rainStreaks.usable ? "" : " (quads)",
                      m_snowStreaks.rects.size(), area(m_snowStreaks), m_snowStreaks.usable ? "" : " (quads)");
        }
    }

    void WeatherEffectRenderer::BuildStreakSet(StreakSet& set, const unsigned char* rgba, int width, int height,
                                               int copies) {
        set = StreakSet{};
        set.copies = copies;
        if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return;
        // Greedy rectangles over alpha > 0 (the shader's cutout is on
        // texture alpha x vertex alpha, so an alpha-0 texel never draws and
        // every other one may): a run down from each unclaimed texel, then
        // widened while whole runs match.
        const auto alpha = [&](int x, int y) { return rgba[(static_cast<size_t>(y) * width + x) * 4 + 3]; };
        std::vector<uint8_t> used(static_cast<size_t>(width) * height, 0);
        size_t covered = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (alpha(x, y) == 0 || used[static_cast<size_t>(y) * width + x]) continue;
                int y1 = y;
                while (y1 + 1 < height && alpha(x, y1 + 1) != 0 && !used[static_cast<size_t>(y1 + 1) * width + x]) ++y1;
                int x1 = x;
                for (;;) {
                    if (x1 + 1 >= width) break;
                    bool whole = true;
                    for (int yy = y; yy <= y1; ++yy) {
                        if (alpha(x1 + 1, yy) == 0 || used[static_cast<size_t>(yy) * width + x1 + 1]) { whole = false; break; }
                    }
                    if (!whole) break;
                    ++x1;
                }
                for (int yy = y; yy <= y1; ++yy) {
                    for (int xx = x; xx <= x1; ++xx) used[static_cast<size_t>(yy) * width + xx] = 1;
                }
                set.rects.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                                     static_cast<uint16_t>(x1 - x + 1), static_cast<uint16_t>(y1 - y + 1)});
                covered += static_cast<size_t>(x1 - x + 1) * static_cast<size_t>(y1 - y + 1);
            }
        }
        if (set.rects.empty() || covered * 4 > static_cast<size_t>(width) * height || set.rects.size() > 4096) {
            return;   // nothing to draw, or dense: the quads are the cheaper draw
        }

        // Fewer, looser rects. On a tile GPU the rects' vertex work is on
        // the serial vertex chain of the frame (the rain target's vertex
        // stage runs before the frame's own) while their fragments overlap
        // it, so a rect costs more than the transparent texels it would
        // enclose: pairs are merged into their bounding box, least added
        // area first, while the total stays within kSlack times the opaque
        // area. A merge whose box would overlap another rect is refused —
        // the rects stay disjoint, so no pixel is drawn twice.
        // kSlack: the merged area may grow to this many times the lit area.
        // 3 was chosen while the rain drew into its own half-res target
        // (its vertex stage on the frame's serial chain); OBEY_RAIN_SLACK
        // overrides for the A/B.
        static const float s_slack = [] {
            const char* v = std::getenv("OBEY_RAIN_SLACK");
            return v ? static_cast<float>(std::atof(v)) : 3.0f;
        }();
        const size_t maxArea = static_cast<size_t>(static_cast<float>(covered) * s_slack);
        size_t total = covered;
        const auto area = [](const StreakRect& r) { return static_cast<size_t>(r.w) * r.h; };
        const auto overlaps = [](const StreakRect& a, const StreakRect& b) {
            return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
        };
        for (;;) {
            size_t bestCost = SIZE_MAX, bi = 0, bj = 0;
            StreakRect bestBox{};
            for (size_t i = 0; i < set.rects.size(); ++i) {
                for (size_t j = i + 1; j < set.rects.size(); ++j) {
                    const StreakRect& a = set.rects[i];
                    const StreakRect& b = set.rects[j];
                    const uint16_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
                    const uint16_t x1 = std::max<uint16_t>(a.x + a.w, b.x + b.w);
                    const uint16_t y1 = std::max<uint16_t>(a.y + a.h, b.y + b.h);
                    const StreakRect box{x0, y0, static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0)};
                    const size_t cost = area(box) - area(a) - area(b);
                    if (cost >= bestCost) continue;
                    bool clear = true;
                    for (size_t k = 0; k < set.rects.size() && clear; ++k) {
                        if (k != i && k != j && overlaps(box, set.rects[k])) clear = false;
                    }
                    if (clear) { bestCost = cost; bi = i; bj = j; bestBox = box; }
                }
            }
            if (bestCost == SIZE_MAX || total + bestCost > maxArea) break;
            total += bestCost;
            set.rects[bi] = bestBox;
            set.rects.erase(set.rects.begin() + static_cast<std::ptrdiff_t>(bj));
        }

        // The template: per rect and copy, four block-layout vertices
        // (u0, v0, copy | u1, v1 | corner) and two triangles.
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        verts.reserve(set.rects.size() * copies * 4);
        indices.reserve(set.rects.size() * copies * 6);
        const float sx = 1.0f / static_cast<float>(width), sy = 1.0f / static_cast<float>(height);
        for (const StreakRect& r : set.rects) {
            const float u0 = r.x * sx, v0 = r.y * sy, u1 = (r.x + r.w) * sx, v1 = (r.y + r.h) * sy;
            for (int c = 0; c < copies; ++c) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                for (uint8_t corner = 0; corner < 4; ++corner) {
                    verts.push_back(Vertex{u0, v0, static_cast<float>(c), u1, v1, corner, 0, 0, 255});
                }
                for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + i);
            }
        }
        RenderBackend& b = *g_renderBackend;
        set.vb = b.CreateBuffer(BufferUsage::Vertex, verts.size() * sizeof(Vertex), verts.data(), BufferAccess::Static);
        set.ib = b.CreateBuffer(BufferUsage::Index, indices.size() * sizeof(uint32_t), indices.data(), BufferAccess::Static);
        if (set.vb == INVALID_BUFFER || set.ib == INVALID_BUFFER) {
            DestroyStreakSet(set, /*deferred=*/false);
            set.copies = copies;
            return;
        }
        b.SetDebugLabel(DebugLabelKind::Buffer, set.vb, copies == 1 ? "Rain streak template" : "Snow streak template");
        b.SetDebugLabel(DebugLabelKind::Buffer, set.ib, copies == 1 ? "Rain streak indices" : "Snow streak indices");
        set.indexCount = static_cast<uint32_t>(indices.size());
        set.usable = true;
    }

    void WeatherEffectRenderer::DestroyStreakSet(StreakSet& set, bool deferred) {
        for (BufferHandle* buffer : {&set.vb, &set.ib}) {
            if (*buffer == INVALID_BUFFER) continue;
            if (deferred) g_renderBackend->DeferredDestroyBuffer(*buffer);
            else          g_renderBackend->DestroyBuffer(*buffer);
            *buffer = INVALID_BUFFER;
        }
        set.rects.clear();
        set.indexCount = 0;
        set.usable = false;
    }

    void WeatherEffectRenderer::DestroyTextures(bool deferred) {
        // Deferred on a reload: a frame still in flight may be sampling the
        // old texture. Immediate at shutdown, with the device going away.
        for (TextureHandle* t : {&m_rainTexture, &m_snowTexture}) {
            if (*t == INVALID_TEXTURE) continue;
            if (deferred) g_renderBackend->DeferredDestroyTexture(*t);
            else          g_renderBackend->DestroyTexture(*t);
            *t = INVALID_TEXTURE;
        }
    }

    WeatherEffectRenderer::StreamSlot& WeatherEffectRenderer::AcquireSlot(size_t bytesNeeded, bool streaks) {
        StreamSlot& slot = m_slots[m_slotCursor];
        m_slotCursor = (m_slotCursor + 1) % kStreamSlots;
        RenderBackend& b = *g_renderBackend;
        if (slot.vb == INVALID_BUFFER || slot.capacityBytes < bytesNeeded) {
            // 21 x 21 columns x 6 vertices at the widest radius, or the same
            // columns x ~6 repeats as 64-byte instances: the larger.
            size_t newCap = std::max<size_t>(slot.capacityBytes, 2646 * 64);
            while (newCap < bytesNeeded) newCap *= 2;
            for (MeshHandle* mesh : {&slot.mesh, &slot.rainMesh, &slot.snowMesh}) {
                if (*mesh != INVALID_MESH) b.DeferredDestroyMesh(*mesh);
                *mesh = INVALID_MESH;
            }
            if (slot.vb != INVALID_BUFFER) b.DeferredDestroyBuffer(slot.vb);
            slot.vb = b.CreateBuffer(BufferUsage::Vertex, newCap, nullptr, BufferAccess::Streaming);
            b.SetDebugLabel(DebugLabelKind::Buffer, slot.vb, "Weather stream");
            slot.capacityBytes = newCap;
        }
        if (!streaks) {
            if (slot.mesh == INVALID_MESH) slot.mesh = b.CreateMesh(slot.vb, INVALID_BUFFER, GetBlockVertexLayout());
        } else if (slot.rainMesh == INVALID_MESH || slot.snowMesh == INVALID_MESH ||
                   slot.meshGeneration != m_streakGeneration) {
            // The instanced meshes: the templates (this generation's) over
            // this buffer's instances.
            for (MeshHandle* mesh : {&slot.rainMesh, &slot.snowMesh}) {
                if (*mesh != INVALID_MESH) b.DeferredDestroyMesh(*mesh);
                *mesh = INVALID_MESH;
            }
            slot.rainMesh = b.CreateInstancedMesh(m_rainStreaks.vb, m_rainStreaks.ib, slot.vb,
                                                  GetBlockVertexLayout(), m_streakInstanceLayout);
            slot.snowMesh = b.CreateInstancedMesh(m_snowStreaks.vb, m_snowStreaks.ib, slot.vb,
                                                  GetBlockVertexLayout(), m_streakInstanceLayout);
            slot.meshGeneration = m_streakGeneration;
        }
        return slot;
    }

    void WeatherEffectRenderer::DestroySlots() {
        for (StreamSlot& slot : m_slots) {
            for (MeshHandle* mesh : {&slot.mesh, &slot.rainMesh, &slot.snowMesh}) {
                if (*mesh != INVALID_MESH) g_renderBackend->DestroyMesh(*mesh);
                *mesh = INVALID_MESH;
            }
            if (slot.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(slot.vb); slot.vb = INVALID_BUFFER; }
            slot.capacityBytes = 0;
            slot.meshGeneration = 0;
        }
        m_slotCursor = 0;
    }

    void WeatherEffectRenderer::PrepareInstances(std::vector<Vertex>& out,
                                                 const std::vector<ColumnInstance>& columns,
                                                 const glm::dvec3& cameraPos, float maxAlpha, int radius,
                                                 float intensity) const {
        // MC prepareInstances.
        const float radiusSq = static_cast<float>(radius * radius);
        const int camBlockX = static_cast<int>(std::floor(cameraPos.x));
        const int camBlockZ = static_cast<int>(std::floor(cameraPos.z));
        for (const ColumnInstance& column : columns) {
            const float relativeX = static_cast<float>(static_cast<double>(column.x) + 0.5 - cameraPos.x);
            const float relativeZ = static_cast<float>(static_cast<double>(column.z) + 0.5 - cameraPos.z);
            const float distanceSq = relativeX * relativeX + relativeZ * relativeZ;
            const float lerpT = std::min(distanceSq / radiusSq, 1.0f);
            const float alpha = (maxAlpha + lerpT * (0.5f - maxAlpha)) * intensity;
            const int index = (column.z - camBlockZ + 16) * 32 + column.x - camBlockX + 16;
            if (index < 0 || index >= 1024) continue;
            const float halfSizeX = m_columnSizeX[static_cast<size_t>(index)] / 2.0f;
            const float halfSizeZ = m_columnSizeZ[static_cast<size_t>(index)] / 2.0f;
            // The camera's own column: no direction to face (0 / 0).
            if (std::isnan(halfSizeX) || std::isnan(halfSizeZ)) continue;

            // ARGB.white(alpha): the alpha to 8 bits (floor); the lightmap
            // colour of the column's light in the RGB (the shader's own
            // light is 1 — see the header).
            const int a8 = std::clamp(static_cast<int>(std::floor(alpha * 255.0f)), 0, 255);
            const glm::vec3 light = EntityEnvironment::LightColor(column.lightCoords);
            const uint8_t r = static_cast<uint8_t>(std::clamp(light.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(light.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(light.b, 0.0f, 1.0f) * 255.0f);
            const uint8_t a = static_cast<uint8_t>(a8);

            // World corners in double, then narrowed through the view's
            // render origin (MC builds them camera-relative in float).
            const double cx = static_cast<double>(column.x) + 0.5;
            const double cz = static_cast<double>(column.z) + 0.5;
            const glm::vec3 p0 = ToRender(glm::dvec3(cx - halfSizeX, static_cast<double>(column.topY),    cz - halfSizeZ));
            const glm::vec3 p1 = ToRender(glm::dvec3(cx + halfSizeX, static_cast<double>(column.topY),    cz + halfSizeZ));
            const glm::vec3 p2 = ToRender(glm::dvec3(cx + halfSizeX, static_cast<double>(column.bottomY), cz + halfSizeZ));
            const glm::vec3 p3 = ToRender(glm::dvec3(cx - halfSizeX, static_cast<double>(column.bottomY), cz - halfSizeZ));
            const float u0 = column.uOffset + 0.0f;
            const float u1 = column.uOffset + 1.0f;
            const float v0 = static_cast<float>(column.bottomY) * 0.25f + column.vOffset;
            const float v1 = static_cast<float>(column.topY) * 0.25f + column.vOffset;
            // MC's quad (x0,y1,z0) (x1,y1,z1) (x1,y0,z1) (x0,y0,z0) with
            // UVs (u0,v0) (u1,v0) (u1,v1) (u0,v1), as two triangles.
            const Vertex q0{p0.x, p0.y, p0.z, u0, v0, r, g, b, a};
            const Vertex q1{p1.x, p1.y, p1.z, u1, v0, r, g, b, a};
            const Vertex q2{p2.x, p2.y, p2.z, u1, v1, r, g, b, a};
            const Vertex q3{p3.x, p3.y, p3.z, u0, v1, r, g, b, a};
            out.push_back(q0); out.push_back(q1); out.push_back(q2);
            out.push_back(q0); out.push_back(q2); out.push_back(q3);
        }
    }

    void WeatherEffectRenderer::PrepareStreakInstances(std::vector<StreakInstance>& out,
                                                       const std::vector<ColumnInstance>& columns,
                                                       const glm::dvec3& cameraPos, float maxAlpha, int radius,
                                                       float intensity) const {
        // prepareInstances, as (column, texture repeat) instances. Per column
        // the quad MC would draw (PrepareInstances: the same alpha, size,
        // light and corners); per repeat the shader lays the rects.
        const float radiusSq = static_cast<float>(radius * radius);
        const int camBlockX = static_cast<int>(std::floor(cameraPos.x));
        const int camBlockZ = static_cast<int>(std::floor(cameraPos.z));
        // v = (bottomY + topY - y) / 4 + vOffset in WORLD y; in render space
        // (y - origin.y) that gains origin.y / 4, of which only the
        // fractional part matters to the texture (it repeats every 1) — and
        // the integer render origin makes it one of 0, .25, .5, .75: exact.
        const long long originY = static_cast<long long>(std::floor(RenderOrigin().y));
        const float originTerm = static_cast<float>(((originY % 4) + 4) % 4) * 0.25f;
        for (const ColumnInstance& column : columns) {
            const float relativeX = static_cast<float>(static_cast<double>(column.x) + 0.5 - cameraPos.x);
            const float relativeZ = static_cast<float>(static_cast<double>(column.z) + 0.5 - cameraPos.z);
            const float distanceSq = relativeX * relativeX + relativeZ * relativeZ;
            const float lerpT = std::min(distanceSq / radiusSq, 1.0f);
            const float alpha = (maxAlpha + lerpT * (0.5f - maxAlpha)) * intensity;
            const int index = (column.z - camBlockZ + 16) * 32 + column.x - camBlockX + 16;
            if (index < 0 || index >= 1024) continue;
            const float halfSizeX = m_columnSizeX[static_cast<size_t>(index)] / 2.0f;
            const float halfSizeZ = m_columnSizeZ[static_cast<size_t>(index)] / 2.0f;
            if (std::isnan(halfSizeX) || std::isnan(halfSizeZ)) continue;

            // The quad's RGBA8 as the shader sees it (x / 255, the UNORM
            // conversion the vertex colour went through).
            const int a8 = std::clamp(static_cast<int>(std::floor(alpha * 255.0f)), 0, 255);
            const glm::vec3 light = EntityEnvironment::LightColor(column.lightCoords);
            const float r = static_cast<float>(static_cast<uint8_t>(std::clamp(light.r, 0.0f, 1.0f) * 255.0f)) / 255.0f;
            const float g = static_cast<float>(static_cast<uint8_t>(std::clamp(light.g, 0.0f, 1.0f) * 255.0f)) / 255.0f;
            const float bl = static_cast<float>(static_cast<uint8_t>(std::clamp(light.b, 0.0f, 1.0f) * 255.0f)) / 255.0f;
            const float a = static_cast<float>(a8) / 255.0f;

            const double cx = static_cast<double>(column.x) + 0.5;
            const double cz = static_cast<double>(column.z) + 0.5;
            const glm::vec3 bottom = ToRender(glm::dvec3(cx, static_cast<double>(column.bottomY), cz));
            const glm::vec3 top = ToRender(glm::dvec3(cx, static_cast<double>(column.topY), cz));
            const float vOffset = column.vOffset + originTerm;
            // The texture repeats this column spans: v from bottomY / 4 +
            // vOffset (at the top corner) to topY / 4 + vOffset (bottom).
            const float vLo = bottom.y * 0.25f + vOffset, vHi = top.y * 0.25f + vOffset;
            const int repeatFirst = static_cast<int>(std::floor(vLo));
            const int repeatLast = static_cast<int>(std::floor(vHi));
            for (int repeat = repeatFirst; repeat <= repeatLast; ++repeat) {
                StreakInstance inst;
                inst.column[0] = bottom.x; inst.column[1] = bottom.z; inst.column[2] = halfSizeX; inst.column[3] = halfSizeZ;
                inst.span[0] = bottom.y; inst.span[1] = top.y; inst.span[2] = column.uOffset; inst.span[3] = vOffset;
                inst.tint[0] = r; inst.tint[1] = g; inst.tint[2] = bl; inst.tint[3] = a;
                inst.repeat[0] = static_cast<float>(repeat); inst.repeat[1] = inst.repeat[2] = inst.repeat[3] = 0.0f;
                out.push_back(inst);
            }
        }
    }

    void WeatherEffectRenderer::Render(const glm::mat4& proj, const glm::mat4& view,
                                       const glm::dvec3& cameraPos, float partialTick, bool allowHalfResolution) {
        Render(proj, view, cameraPos, partialTick, Client::ClientLevels::BoundDimension(),
               ChunkRenderer::PortalClipPlane(), allowHalfResolution);
    }

    void WeatherEffectRenderer::Render(const glm::mat4& proj, const glm::mat4& view,
                                       const glm::dvec3& cameraPos, float partialTick,
                                       Game::DimensionId dimension, const glm::vec4& clipPlane,
                                       bool allowHalfResolution) {
        PROFILE_ZONE_N("WeatherEffects.Render");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        if (!Client::g_clientBlockAccess) return;
        RenderBackend& b = *g_renderBackend;
        // Vulkan / Metal: the half-res rain is drawn ahead of the frame.
        const bool vulkan = b.UsesVkShaders();

        // Vulkan, the main view: drawn ahead of the frame's pass this frame
        // (RenderAhead) — only the composite is left.
        if (allowHalfResolution && vulkan && m_aheadFrame != 0 && m_aheadFrame == b.FrameNumber()) {
            int width = 0, height = 0;
            if (!WorldFramebuffer::Get(width, height)) glfwGetFramebufferSize(b.GetWindow(), &width, &height);
            CompositeHalf(m_halfTargets[b.FrameSlot() % m_halfTargets.size()], width, height);
            RestoreDefaultState();
            return;
        }

        // Rain Resolution: Half for the main view, unless Improved
        // Transparency draws the weather (OBEY_SKIP=rainhalf: full in the
        // skip phases, for A/B). On Vulkan it is drawn ahead of the frame
        // (RenderAhead) or, without the previous frame's depth, at full
        // resolution here; OpenGL splits the frame for it.
        const bool halfWanted = allowHalfResolution && !vulkan && clipPlane == glm::vec4(0.0f) &&
                                HalfResolutionAvailable() && Platform::g_gameSettings.GetRainHalfResolution() &&
                                !m_halfFailed && !ImprovedTransparency::Get().Active() && !DevSkip("rainhalf");

        Batch batch;
        if (!BuildColumns(dimension, cameraPos, partialTick, batch)) return;

        // ── render (RenderPipelines.WEATHER) ────────────────────────────
        const glm::mat4 mvp = proj * view;
        if (!(halfWanted && DrawHalfResolution(batch, mvp, cameraPos))) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthWriteEnabled = false;
            s.colorWriteEnabled = true;
            s.blendEnabled      = true;
            s.srcBlendFactor    = BlendFactor::SrcAlpha;
            s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            DrawColumns(/*half=*/false, batch, mvp, cameraPos, clipPlane, s);
        }
        RestoreDefaultState();
    }

    void WeatherEffectRenderer::RenderAhead(const glm::mat4& proj, const glm::mat4& view,
                                            const glm::dvec3& cameraPos, float partialTick) {
        PROFILE_ZONE_N("WeatherEffects.RenderAhead");
        if (m_shader == INVALID_SHADER || !g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        if (!b.UsesVkShaders()) return;
        GpuDebugGroup gpuGroup("Rain (half-res, ahead of the frame)");
        const uint64_t frame = b.FrameNumber();

        // The depth images can always be preserved while the option is on
        // (built once, not when it starts raining), so the gate below may
        // follow the rain frame by frame: turning preservation on or off is
        // then a store op, never a device idle.
        const bool halfOption = HalfResolutionAvailable() && Platform::g_gameSettings.GetRainHalfResolution() &&
                                !m_halfFailed;
        b.SetFrameDepthCapable(halfOption);

        // The gate: Half, the main view, precipitation this frame, and the
        // weather not drawn by Improved Transparency. Off, nothing below
        // runs and the frame draws as it always has (OBEY_SKIP=rainhalf:
        // off in the skip phases, for A/B).
        const Game::DimensionId dimension = Client::ClientLevels::BoundDimension();
        const bool canRain = Game::DimensionCanHaveWeather(dimension);
        const float intensity = canRain ? Client::ClientWeather::RainLevelIn(dimension, partialTick) : 0.0f;
        const bool gate = halfOption && Client::g_clientBlockAccess && intensity > 0.0f &&
                          !ImprovedTransparency::Get().Active() && !DevSkip("rainhalf") && !DevSkip("weather");
        // This frame keeps its depth (store op), hands it to the next one,
        // and its hand, portal gun and GUI items draw into depth bands
        // instead of clearing it — the next frame's rain reads it.
        b.SetFrameDepthPreserved(gate, RenderBackend::FrameDepthUser::Weather);
        b.SetDepthHandoff(gate);
        // The bands every frame on Metal, rain or not: the three full-screen
        // depth clears they stand in for cost 0.22 ms serial (4 % of a 5.4 ms
        // frame) and −9..19 % of the live GPU frame (Xcode + ABBA on the M4,
        // 2026-10-07); a band is a viewport change. Not on Vulkan: MoltenVK's
        // vkCmdClearAttachments measured equal or slightly better than the
        // bands there (ABBA 178 vs 175 fps, within noise), so it keeps MC's
        // clears outside the rain. OBEY_DEPTH_CLEARS=1 restores the clears on
        // Metal (the A/B).
        static const bool s_bandsAlways = std::getenv("OBEY_DEPTH_CLEARS") == nullptr &&
                                          b.GetType() == BackendType::Metal;
        // OBEY_SKIP=bands: the skip phases go back to the clears (the phase A/B).
        LateDepthBands::MarkFrame(gate || (s_bandsAlways && !DevSkip("bands")), b.FrameDepthIsFloat());

        // This view, for the next frame's reprojection; the previous one
        // only if it is the frame just before.
        const glm::mat4 mvp = proj * view;
        const glm::dvec3 origin = RenderOrigin();
        const bool havePrevious = m_viewFrame != 0 && m_viewFrame + 1 == frame;
        const glm::mat4 previousMvp = m_viewMvp;
        const glm::dvec3 previousOrigin = m_viewOrigin;
        m_viewMvp = mvp;
        m_viewOrigin = origin;
        m_viewFrame = gate ? frame : 0;
        if (!gate || !havePrevious) return;

        // The previous frame's depth, if it was handed off and this frame's
        // pass has not opened (nothing drawn yet). Otherwise the rain is
        // drawn at full resolution in the frame (Render).
        const TextureHandle previousDepth = b.PreviousFrameDepthTexture();
        if (previousDepth == INVALID_TEXTURE) return;

        Batch batch;
        if (!BuildColumns(dimension, cameraPos, partialTick, batch)) return;

        // That frame's view-projection over this frame's render space: the
        // origin moved by (origin - previousOrigin) between them.
        const glm::mat4 reprojection =
            previousMvp * glm::translate(glm::mat4(1.0f), glm::vec3(origin - previousOrigin));
        if (DrawAhead(batch, mvp, reprojection, cameraPos, previousDepth)) m_aheadFrame = frame;
        RestoreDefaultState();
    }

    bool WeatherEffectRenderer::BuildColumns(Game::DimensionId dimension, const glm::dvec3& cameraPos,
                                             float partialTick, Batch& out) {
        // ── extractRenderState ──────────────────────────────────────────
        // The view's level's weather: a portal into the Nether shows none.
        const bool canRain = Game::DimensionCanHaveWeather(dimension);
        const float intensity = canRain ? Client::ClientWeather::RainLevelIn(dimension, partialTick) : 0.0f;
        m_rainColumns.clear();
        m_snowColumns.clear();
        if (!canRain || !(intensity > 0.0f)) return false;

        int radius = Platform::g_gameSettings.GetWeatherRadius();
        // OBEY_SKIP=weatherradius: radius 5 (MC's fast-graphics radius) in
        // the skip phases — the Weather Radius option's cost, measured.
        if (DevSkip("weatherradius")) radius = std::min(radius, 5);
        const int cameraBlockX = static_cast<int>(std::floor(cameraPos.x));
        const int cameraBlockY = static_cast<int>(std::floor(cameraPos.y));
        const int cameraBlockZ = static_cast<int>(std::floor(cameraPos.z));
        const int64_t gameTime = EnvironmentState::Get().GameTime();
        const Client::ClientBlockAccess& blocks = *Client::g_clientBlockAccess;
        Game::JavaRandom random(0);

        {
            PROFILE_ZONE_N("WeatherEffects.Extract");
            for (int z = cameraBlockZ - radius; z <= cameraBlockZ + radius; ++z) {
                for (int x = cameraBlockX - radius; x <= cameraBlockX + radius; ++x) {
                    const int terrainHeight = Client::ClientWeather::MotionBlockingHeight(x, z);
                    const int y0 = std::max(cameraBlockY - radius, terrainHeight);
                    const int y1 = std::max(cameraBlockY + radius, terrainHeight);
                    if (y1 - y0 == 0) continue;
                    const int precipitation =
                        Client::ClientWeather::BiomePrecipitationAt(blocks, glm::ivec3(x, cameraBlockY, z));
                    if (precipitation == static_cast<int>(Game::BiomeRegistry::Precipitation::None)) continue;

                    // int seed = x * x * 3121 + x * 45238971 ^ z * z * 418711 + z * 13761;
                    const int32_t seed = Add(Mul(Mul(x, x), 3121), Mul(x, 45238971)) ^
                                         Add(Mul(Mul(z, z), 418711), Mul(z, 13761));
                    random.SetSeed(static_cast<int64_t>(seed));
                    const int lightSampleY = std::max(cameraBlockY, terrainHeight);
                    const int lightCoords = EntityEnvironment::LevelLightCoordsAt(glm::ivec3(x, lightSampleY, z));

                    if (precipitation == static_cast<int>(Game::BiomeRegistry::Precipitation::Rain)) {
                        // createRainColumnInstance.
                        const int wrappedTicks = static_cast<int>(gameTime & 131071LL);
                        const int tickOffset = Add(Add(Add(Mul(Mul(x, x), 3121), Mul(x, 45238971)),
                                                       Mul(Mul(z, z), 418711)), Mul(z, 13761)) & 255;
                        const float blockPosRainSpeed = 3.0f + random.NextFloat();
                        const float textureOffset =
                            -(static_cast<float>(wrappedTicks + tickOffset) + partialTick) / 32.0f * blockPosRainSpeed;
                        const float wrappedTextureOffset = std::fmod(textureOffset, 32.0f);
                        m_rainColumns.push_back({x, z, y0, y1, 0.0f, wrappedTextureOffset, lightCoords});
                    } else {
                        // createSnowColumnInstance.
                        const int wrappedTicks = static_cast<int>(gameTime & 131071LL);
                        const float time = static_cast<float>(wrappedTicks) + partialTick;
                        const double ud = random.NextDouble();
                        const float ug = static_cast<float>(random.NextGaussian());
                        const float u = static_cast<float>(ud + static_cast<double>(time * 0.01f * ug));
                        const double vd = random.NextDouble();
                        const float vg = static_cast<float>(random.NextGaussian());
                        const float v = static_cast<float>(vd + static_cast<double>(time * vg * 0.001f));
                        const float vOffset = -(static_cast<float>(gameTime & 511LL) + partialTick) / 512.0f;
                        namespace LC = Game::Lighting::LightCoords;
                        const int brightened = LC::Pack((LC::Block(lightCoords) * 3 + 15) / 4,
                                                        (LC::Sky(lightCoords) * 3 + 15) / 4);
                        m_snowColumns.push_back({x, z, y0, y1, u, vOffset + v, brightened});
                    }
                }
            }
        }
        if (m_rainColumns.empty() && m_snowColumns.empty()) return false;

        // ── prepare ─────────────────────────────────────────────────────
        out = Batch{};
        if (StreaksActive() && !DevSkip("streaks")) {   // OBEY_SKIP=streaks: MC's quads in the skip phases
            m_instances.clear();
            PrepareStreakInstances(m_instances, m_rainColumns, cameraPos, 1.0f, radius, intensity);
            out.rainCount = m_instances.size();
            PrepareStreakInstances(m_instances, m_snowColumns, cameraPos, 0.8f, radius, intensity);
            out.snowCount = m_instances.size() - out.rainCount;
            if (m_instances.empty()) return false;
            StreamSlot& slot = AcquireSlot(m_instances.size() * sizeof(StreakInstance), /*streaks=*/true);
            if (slot.rainMesh == INVALID_MESH || slot.snowMesh == INVALID_MESH) return false;
            g_renderBackend->UpdateBufferStreaming(slot.vb, 0, m_instances.size() * sizeof(StreakInstance),
                                                   m_instances.data());
            out.slot = &slot;
            out.streaks = true;
            return true;
        }
        m_vertices.clear();
        PrepareInstances(m_vertices, m_rainColumns, cameraPos, 1.0f, radius, intensity);
        out.rainCount = m_vertices.size();
        PrepareInstances(m_vertices, m_snowColumns, cameraPos, 0.8f, radius, intensity);
        out.snowCount = m_vertices.size() - out.rainCount;
        if (m_vertices.empty()) return false;

        StreamSlot& slot = AcquireSlot(m_vertices.size() * sizeof(Vertex), /*streaks=*/false);
        if (slot.mesh == INVALID_MESH) return false;
        g_renderBackend->UpdateBufferStreaming(slot.vb, 0, m_vertices.size() * sizeof(Vertex), m_vertices.data());
        out.slot = &slot;
        return true;
    }

    void WeatherEffectRenderer::RestoreDefaultState() {
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

    void WeatherEffectRenderer::DrawColumns(bool half, const Batch& batch,
                                            const glm::mat4& mvp, const glm::dvec3& cameraPos,
                                            const glm::vec4& clipPlane, const PipelineState& state,
                                            TextureHandle sceneDepth, const glm::mat4* reprojection) {
        if (!batch.slot) return;
        RenderBackend& b = *g_renderBackend;
        const StreamSlot& slot = *batch.slot;
        const ShaderHandle shader = batch.streaks ? (half ? m_streakHalfShader : m_streakShader)
                                                  : (half ? m_halfShader : m_shader);
        if (shader == INVALID_SHADER) return;
        b.BindShader(shader);
        b.SetUniformMat4(shader, "uMVP", mvp);
        b.SetUniformInt(shader, "uSprite", 0);
        EntityEnvironment::ApplyWorld(shader, cameraPos);
        EntityEnvironment::SetEntityLight(shader, glm::vec3(1.0f));
        // The view's clip plane (a portal view's far side only); set every
        // draw — on Vulkan it rides a push-constant slot others write.
        b.SetUniformVec4(shader, "uEntityClipPlane", clipPlane);
        if (sceneDepth != INVALID_TEXTURE) {
            b.BindTexture(sceneDepth, 1);
            b.SetUniformInt(shader, "uSceneDepth", 1);
        }
        if (reprojection) {
            // The previous frame's depth (weather_half_vk.frag): its
            // view-projection, and the depth below which it holds a band.
            b.SetUniformMat4(shader, "uModel", *reprojection);
            b.SetUniformFloat(shader, "uAlphaTest", LateDepthBands::WorldNear());
        }
        b.SetPipelineState(state);

        if (batch.streaks) {
            // Every rect of the template per instance; snow's instances
            // follow rain's in the slot's buffer.
            if (batch.rainCount > 0 && m_rainTexture != INVALID_TEXTURE) {
                b.BindTexture(m_rainTexture, 0);
                b.DrawIndexedInstanced(slot.rainMesh, m_rainStreaks.indexCount, 0,
                                       static_cast<uint32_t>(batch.rainCount), 0);
            }
            if (batch.snowCount > 0 && m_snowTexture != INVALID_TEXTURE) {
                b.BindTexture(m_snowTexture, 0);
                b.DrawIndexedInstanced(slot.snowMesh, m_snowStreaks.indexCount, 0,
                                       static_cast<uint32_t>(batch.snowCount),
                                       static_cast<uint32_t>(batch.rainCount * sizeof(StreakInstance)));
            }
        } else {
            if (batch.rainCount > 0 && m_rainTexture != INVALID_TEXTURE) {
                b.BindTexture(m_rainTexture, 0);
                b.DrawArrays(slot.mesh, static_cast<uint32_t>(batch.rainCount), 0);
            }
            if (batch.snowCount > 0 && m_snowTexture != INVALID_TEXTURE) {
                b.BindTexture(m_snowTexture, 0);
                b.DrawArrays(slot.mesh, static_cast<uint32_t>(batch.snowCount), static_cast<uint32_t>(batch.rainCount));
            }
        }
        b.UnbindMesh();
    }

    bool WeatherEffectRenderer::DrawHalfResolution(const Batch& batch, const glm::mat4& mvp,
                                                   const glm::dvec3& cameraPos) {
        // OpenGL: the frame is split — FBO 0's depth copied while it is
        // still bound, the columns drawn into the target against it, the
        // target laid over the frame.
        RenderBackend& b = *g_renderBackend;
        // The level's size: the scaled scene's under Render Resolution.
        int width = 0, height = 0;
        if (!WorldFramebuffer::Get(width, height)) glfwGetFramebufferSize(b.GetWindow(), &width, &height);
        if (width <= 0 || height <= 0) return false;
        const int halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
        if (!EnsureHalfResources(halfWidth, halfHeight)) return false;

        if (m_sceneDepthCopy == INVALID_TEXTURE || m_sceneDepthWidth != width || m_sceneDepthHeight != height) {
            if (m_sceneDepthCopy != INVALID_TEXTURE) b.DeferredDestroyTexture(m_sceneDepthCopy);
            m_sceneDepthCopy = b.CreateTexture2D(width, height, TextureFormat::Depth24Stencil8, nullptr);
            m_sceneDepthWidth = width;
            m_sceneDepthHeight = height;
            if (m_sceneDepthCopy == INVALID_TEXTURE) return false;
            b.SetTextureFilter(m_sceneDepthCopy, TextureFilter::Nearest, TextureFilter::Nearest);
            b.SetTextureWrap(m_sceneDepthCopy, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        if (!b.CopyFramebufferDepthToTexture(m_sceneDepthCopy)) return false;

        const RenderTargetHandle target = m_halfTargets[b.FrameSlot() % m_halfTargets.size()];
        b.BindRenderTarget(target);
        DrawIntoHalfTarget(batch, mvp, cameraPos, m_sceneDepthCopy, nullptr, halfWidth, halfHeight);
        b.BindRenderTarget(INVALID_RENDER_TARGET);
        CompositeHalf(target, width, height);
        return true;
    }

    bool WeatherEffectRenderer::DrawAhead(const Batch& batch, const glm::mat4& mvp, const glm::mat4& reprojection,
                                          const glm::dvec3& cameraPos, TextureHandle previousDepth) {
        // Vulkan: the frame's pass has not opened, so the target's pass runs
        // first in the frame's command buffer and the return leaves the
        // frame's pass to open, clearing, on its first draw — no split.
        RenderBackend& b = *g_renderBackend;
        int width = 0, height = 0;
        if (!WorldFramebuffer::Get(width, height)) glfwGetFramebufferSize(b.GetWindow(), &width, &height);
        if (width <= 0 || height <= 0) return false;
        const int halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
        if (!EnsureHalfResources(halfWidth, halfHeight)) return false;

        const RenderTargetHandle target = m_halfTargets[b.FrameSlot() % m_halfTargets.size()];
        b.BindRenderTarget(target);
        DrawIntoHalfTarget(batch, mvp, cameraPos, previousDepth, &reprojection, halfWidth, halfHeight);
        b.BindRenderTarget(INVALID_RENDER_TARGET);
        b.SetViewport(0, 0, width, height);
        return true;
    }

    void WeatherEffectRenderer::DrawIntoHalfTarget(const Batch& batch, const glm::mat4& mvp,
                                                   const glm::dvec3& cameraPos, TextureHandle sceneDepth,
                                                   const glm::mat4* reprojection, int halfWidth, int halfHeight) {
        RenderBackend& b = *g_renderBackend;
        b.SetViewport(0, 0, halfWidth, halfHeight);
        // The state first: GL's clear honours the colour write mask the last
        // draw left.
        PipelineState flat;
        flat.depthTestEnabled  = false;
        flat.depthWriteEnabled = false;
        flat.blendEnabled      = false;
        flat.cullMode          = CullMode::None;
        b.SetPipelineState(flat);
        b.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        b.Clear(true, false, false);

        // The columns, premultiplied "over" into the cleared target; the
        // world's depth test is the shader's (the target's own depth is not
        // the world's).
        // Alpha accumulates "over" too (Vulkan otherwise keeps only the last
        // layer's): the composite darkens the frame by it, and a too-small
        // alpha under the summed colour reads as light-blue / white streaks.
        PipelineState columns = flat;
        columns.blendEnabled   = true;
        columns.srcBlendFactor = BlendFactor::One;
        columns.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        columns.blendAlphaLikeColor = true;
        DrawColumns(/*half=*/true, batch, mvp, cameraPos, glm::vec4(0.0f), columns, sceneDepth, reprojection);

        // The frame's own clear colour (the fog), as the frame had it.
        const glm::vec3 fog = EnvironmentState::Get().Frame().fogColor;
        b.SetClearColor(fog.r, fog.g, fog.b, 1.0f);
    }

    void WeatherEffectRenderer::CompositeHalf(RenderTargetHandle target, int width, int height) {
        // Over the frame in one draw, bilinear, premultiplied "over".
        RenderBackend& b = *g_renderBackend;
        b.SetViewport(0, 0, width, height);
        PipelineState over;
        over.depthTestEnabled  = false;
        over.depthWriteEnabled = false;
        over.blendEnabled      = true;
        over.srcBlendFactor    = BlendFactor::One;
        over.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        over.cullMode          = CullMode::None;
        b.SetPipelineState(over);
        b.BindShader(m_compositeShader);
        b.BindTexture(b.GetRenderTargetColorTexture(target), 0);
        b.SetUniformInt(m_compositeShader, "uInSampler", 0);
        b.DrawArrays(m_quadMesh, 6, 0);
        b.UnbindMesh();
    }

    bool WeatherEffectRenderer::EnsureHalfResources(int width, int height) {
        RenderBackend& b = *g_renderBackend;
        if (m_halfShader == INVALID_SHADER) {
            m_halfShader = b.GetType() == BackendType::OpenGL
                ? b.CreateShader(MobParticleSystem::VertexSource(), kHalfFragSource)
                : EntityEnvironment::CreateShader("shaders/mob_particle.vert", "shaders/weather_half.frag",
                                                  /*readsCommonMatrices=*/true);   // the reprojection
            // The streaks' twin over the same fragment shader.
            if (m_streaksWanted && m_streakHalfShader == INVALID_SHADER) {
                if (b.GetType() == BackendType::OpenGL) {
                    m_streakHalfShader = b.CreateShader(kStreakVertSource, kHalfFragSource);
                } else {
                    m_streakHalfShader = EntityEnvironment::CreateShader("shaders/weather_streak.vert",
                                                                         "shaders/weather_half.frag",
                                                                         /*readsCommonMatrices=*/true);
                    if (m_streakHalfShader != INVALID_SHADER) {
                        b.RegisterShaderVertexLayout(m_streakHalfShader, GetBlockVertexLayout());
                        b.RegisterShaderInstanceLayout(m_streakHalfShader, m_streakInstanceLayout);
                    }
                }
                if (m_streakHalfShader == INVALID_SHADER) {
                    Log::Warning("[WeatherEffectRenderer] half-resolution streak shader unavailable - quads");
                    m_streaksWanted = false;
                }
            }
            // The plain textured full-screen blit (entity_outline_post.vert
            // mirrors v for Vulkan's flipped viewport).
            m_compositeShader = b.CreateShaderFromFiles("shaders/entity_outline_post.vert",
                                                        "shaders/entity_outline_blit.frag");
            struct QuadVert { float x, y, z, u, v; uint8_t r, g, b, a; };
            const QuadVert quad[6] = {
                {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255},
            };
            m_quadVB = b.CreateBuffer(BufferUsage::Vertex, sizeof(quad), quad, BufferAccess::Static);
            if (m_quadVB != INVALID_BUFFER) m_quadMesh = b.CreateMesh(m_quadVB, INVALID_BUFFER, GetBlockVertexLayout());
            if (m_halfShader == INVALID_SHADER || m_compositeShader == INVALID_SHADER || m_quadMesh == INVALID_MESH) {
                Log::Warning("[WeatherEffectRenderer] half-resolution rain unavailable (shaders or quad) - drawn at full resolution");
                ReleaseHalfResources();
                m_halfFailed = true;
                return false;
            }
        }
        if (m_halfWidth != width || m_halfHeight != height) {
            for (RenderTargetHandle& t : m_halfTargets) {
                if (t == INVALID_RENDER_TARGET) {
                    RenderTargetDesc desc;
                    desc.width = width;
                    desc.height = height;
                    desc.colorFormat = TextureFormat::RGBA8;
                    desc.depth = false;   // the world's depth test is the shader's
                    t = b.CreateRenderTarget(desc);
                    b.SetDebugLabel(DebugLabelKind::RenderTarget, t, "Rain half-res");
                } else {
                    b.ResizeRenderTarget(t, width, height);
                }
                if (t == INVALID_RENDER_TARGET) {
                    Log::Warning("[WeatherEffectRenderer] half-resolution rain target unavailable - drawn at full resolution");
                    ReleaseHalfResources();
                    m_halfFailed = true;
                    return false;
                }
                const TextureHandle tex = b.GetRenderTargetColorTexture(t);
                b.SetTextureFilter(tex, TextureFilter::Linear, TextureFilter::Linear);
                b.SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            m_halfWidth = width;
            m_halfHeight = height;
        }
        return true;
    }

    void WeatherEffectRenderer::ReleaseHalfResources() {
        if (!g_renderBackend) return;
        RenderBackend& b = *g_renderBackend;
        for (RenderTargetHandle& t : m_halfTargets) {
            if (t != INVALID_RENDER_TARGET) b.DestroyRenderTarget(t);
            t = INVALID_RENDER_TARGET;
        }
        if (m_quadMesh != INVALID_MESH) { b.DestroyMesh(m_quadMesh); m_quadMesh = INVALID_MESH; }
        if (m_quadVB != INVALID_BUFFER) { b.DestroyBuffer(m_quadVB); m_quadVB = INVALID_BUFFER; }
        if (m_halfShader != INVALID_SHADER) { b.DestroyShader(m_halfShader); m_halfShader = INVALID_SHADER; }
        if (m_streakHalfShader != INVALID_SHADER) { b.DestroyShader(m_streakHalfShader); m_streakHalfShader = INVALID_SHADER; }
        if (m_compositeShader != INVALID_SHADER) { b.DestroyShader(m_compositeShader); m_compositeShader = INVALID_SHADER; }
        if (m_sceneDepthCopy != INVALID_TEXTURE) { b.DestroyTexture(m_sceneDepthCopy); m_sceneDepthCopy = INVALID_TEXTURE; }
        m_halfWidth = m_halfHeight = 0;
        m_sceneDepthWidth = m_sceneDepthHeight = 0;
    }

} // namespace Render
