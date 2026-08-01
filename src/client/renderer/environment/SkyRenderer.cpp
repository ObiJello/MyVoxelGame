// File: src/client/renderer/environment/SkyRenderer.cpp
//
// Geometry and constants are verbatim from the vendored MC decompile
// (minecraft_code/.../client/renderer/SkyRenderer.java):
//   SKY_DISC_RADIUS 512, disc fan of 10 verts at y=±16, sunrise fan of 18
//   verts (center (0,100,0), ring r=120 with z=-cos*40), sun 30×(0,100,0),
//   moon 20×(0,100,0), 1500 star attempts from RandomSource.create(10842L).
// Blend modes from RenderPipelines.java: SKY opaque, SUNRISE_SUNSET
// translucent, CELESTIAL/STARS "overlay" additive (SRC_ALPHA, ONE).
// Only the sky discs are fogged (sky.fsh: apply_fog with FogSkyEnd).
#include "SkyRenderer.hpp"
#include "EnvironmentState.hpp"
#include "JavaRandom.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "common/core/Log.hpp"
#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    SkyRenderer g_skyRenderer;

    // GL shader. VK uses shaders/sky_vk.{vert,frag} (portal pipeline layout:
    // push constants for uMVP/uColor, CommonUBO for uFogColor/uFogEnv).
    const char* SkyRenderer::vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;
out float vSph;
out float vCyl;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    // MC sky.vsh: fog distances from the RAW buffer position (the sky is
    // camera-centered, so model-space distance == camera distance).
    vSph = length(aPos);
    vCyl = max(length(aPos.xz), abs(aPos.y));
}
)";

    const char* SkyRenderer::fragmentShaderSource = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vSph;
in float vCyl;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uColor;
uniform vec4 uFogColor;
uniform vec4 uFogEnv;   // (envStart, envEnd, rdStart, rdEnd); 1e9 = fog off

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 color = texture(uTexture, vUV) * vColor * uColor;
    if (color.a == 0.0) discard;   // MC position_tex.fsh texel discard
    float fogValue = max(linearFog(vSph, uFogEnv.x, uFogEnv.y),
                         linearFog(vCyl, uFogEnv.z, uFogEnv.w));
    FragColor = vec4(mix(color.rgb, uFogColor.rgb, fogValue * uFogColor.a), color.a);
}
)";

    namespace {

        constexpr float kSkyDiscRadius = 512.0f;
        constexpr float kSunSize = 30.0f;
        constexpr float kMoonSize = 20.0f;
        constexpr float kCelestialHeight = 100.0f;
        constexpr int kStarAttempts = 1500;
        constexpr int64_t kStarSeed = 10842;

        TextureHandle LoadEnvTexture(const std::string& relPath) {
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) {
                Log::Warning("SkyRenderer: missing texture %s", relPath.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                // Vanilla binds sun/moon with default (nearest) sampling.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }

        ShaderHandle CreateSkyShader(const char* vertSrc, const char* fragSrc) {
            // Vulkan needs the UBO-aware (portal) pipeline layout for
            // uFogColor/uFogEnv — same backend-cast pattern as PortalRenderer.
            if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
                auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
                return vk->CreateShaderFromFilesPortal("shaders/sky.vert", "shaders/sky.frag");
#else
                return INVALID_SHADER;
#endif
            }
            ShaderHandle s = g_renderBackend->CreateShaderFromFiles("shaders/sky.vert", "shaders/sky.frag");
            if (s == INVALID_SHADER) {
                s = g_renderBackend->CreateShader(vertSrc, fragSrc);
            }
            return s;
        }

        const glm::vec4 kFogOff{1e9f, 1e9f, 1e9f, 1e9f};

        // MC buildEndSky vertex color -14145496 = 0xFF282828.
        const glm::vec3 kEndSkyTint{0x28 / 255.0f, 0x28 / 255.0f, 0x28 / 255.0f};

        constexpr const char* kSkyboxDir = "assets/textures/environment/skyboxes/";
        constexpr const char* kPanoramaDir = "assets/textures/gui/title/background/";

        // Resolve a skybox set id to the directory holding panorama_0..5.png:
        // custom skyboxes first, then the title-screen panorama sets.
        std::string ResolveSkyboxDir(const std::string& id) {
            const std::string custom = PlatformMain::GetAssetPath(
                std::string(kSkyboxDir) + id + "/panorama_0.png");
            if (std::filesystem::exists(custom)) {
                return std::string(kSkyboxDir) + id + "/";
            }
            const std::string pano = PlatformMain::GetAssetPath(
                std::string(kPanoramaDir) + id + "/panorama_0.png");
            if (std::filesystem::exists(pano)) {
                return std::string(kPanoramaDir) + id + "/";
            }
            return {};
        }

        // Loads one skybox face; optionally accumulates the average color of
        // the texture's middle row (the horizon band on side faces) so the
        // fog color can match the skybox.
        TextureHandle LoadSkyboxFace(const std::string& relPath,
                                     glm::vec3* horizonAccum, int* horizonSamples) {
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) return INVALID_TEXTURE;
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            if (horizonAccum && w > 0 && h > 0) {
                const unsigned char* row = pixels + static_cast<size_t>(h / 2) * w * 4;
                for (int x = 0; x < w; ++x) {
                    horizonAccum->r += row[x * 4 + 0] / 255.0f;
                    horizonAccum->g += row[x * 4 + 1] / 255.0f;
                    horizonAccum->b += row[x * 4 + 2] / 255.0f;
                }
                *horizonSamples += w;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }

    } // namespace

    std::vector<SkyboxInfo> DiscoverSkyboxes() {
        std::vector<SkyboxInfo> out;
        out.push_back({"vanilla", "Vanilla"});
        if (std::filesystem::exists(
                PlatformMain::GetAssetPath("assets/textures/environment/end_sky.png"))) {
            out.push_back({"end", "The End"});
        }
        // Custom sets: assets/textures/environment/skyboxes/<name>/panorama_0..5.png
        const std::string customRoot = PlatformMain::GetAssetPath(kSkyboxDir);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(customRoot, ec)) {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (std::filesystem::exists(entry.path() / "panorama_0.png")) {
                out.push_back({name, name});
            }
        }
        // Title-screen panorama sets double as skyboxes.
        const std::string panoRoot = PlatformMain::GetAssetPath(kPanoramaDir);
        for (const auto& entry : std::filesystem::directory_iterator(panoRoot, ec)) {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (std::filesystem::exists(entry.path() / "panorama_0.png")) {
                out.push_back({name, "Panorama " + name});
            }
        }
        return out;
    }

    SkyRenderer::~SkyRenderer() {
        Shutdown();
    }

    SkyRenderer::Mesh SkyRenderer::CreateMesh(const void* verts, size_t vertBytes,
                                              const uint32_t* indices, size_t indexCount) {
        Mesh m;
        m.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, vertBytes, verts);
        m.ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
                                             indexCount * sizeof(uint32_t), indices);
        m.mesh = g_renderBackend->CreateMesh(m.vb, m.ib, GetBlockVertexLayout());
        m.indexCount = static_cast<uint32_t>(indexCount);
        return m;
    }

    void SkyRenderer::DestroyMesh(Mesh& m) {
        if (m.mesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(m.mesh);   m.mesh = INVALID_MESH; }
        if (m.vb != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m.vb);   m.vb = INVALID_BUFFER; }
        if (m.ib != INVALID_BUFFER)  { g_renderBackend->DestroyBuffer(m.ib);   m.ib = INVALID_BUFFER; }
        m.indexCount = 0;
    }

    void SkyRenderer::BuildSkyDiscs() {
        // buildSkyDisc(yy): TRIANGLE_FAN — center (0, yy, 0), then the ring
        // i = -180..180 step 45 at (signum(yy)*512*cos, yy, 512*sin).
        // signum flips the winding so both discs face the camera.
        auto buildDisc = [this](float yy) {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            const float x = (yy >= 0.0f ? 1.0f : -1.0f) * kSkyDiscRadius;
            verts.push_back({0.0f, yy, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255});
            for (int i = -180; i <= 180; i += 45) {
                const float rad = glm::radians(static_cast<float>(i));
                verts.push_back({x * std::cos(rad), yy, kSkyDiscRadius * std::sin(rad),
                                 0.0f, 0.0f, 255, 255, 255, 255});
            }
            for (uint32_t i = 1; i + 1 < verts.size(); ++i) {
                indices.insert(indices.end(), {0u, i, i + 1});
            }
            return CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                              indices.data(), indices.size());
        };
        m_topDisc = buildDisc(16.0f);
        m_bottomDisc = buildDisc(-16.0f);
    }

    void SkyRenderer::BuildSunriseFan() {
        // buildSunriseFan: center (0,100,0) white alpha 1; 17 ring verts at
        // (sin*120, cos*120, -cos*40) alpha 0. ColorModulator applies the
        // actual sunrise color at draw time.
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        verts.push_back({0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255});
        for (int i = 0; i <= 16; ++i) {
            const float angle = static_cast<float>(i) * glm::two_pi<float>() / 16.0f;
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            verts.push_back({s * 120.0f, c * 120.0f, -c * 40.0f, 0.0f, 0.0f, 255, 255, 255, 0});
        }
        for (uint32_t i = 1; i + 1 < verts.size(); ++i) {
            indices.insert(indices.end(), {0u, i, i + 1});
        }
        m_sunriseFan = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                                  indices.data(), indices.size());
    }

    void SkyRenderer::BuildCelestialQuads() {
        // Sun: unit quad, UV 0..1 (buildCelestialQuad).
        {
            const Vertex verts[4] = {
                {-1, 0, -1, 0.0f, 0.0f, 255, 255, 255, 255},
                { 1, 0, -1, 1.0f, 0.0f, 255, 255, 255, 255},
                { 1, 0,  1, 1.0f, 1.0f, 255, 255, 255, 255},
                {-1, 0,  1, 0.0f, 1.0f, 255, 255, 255, 255},
            };
            const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
            m_sunQuad = CreateMesh(verts, sizeof(verts), indices, 6);
        }
        // Moon: one quad per phase, UVs from the legacy 4×2 moon_phases.png
        // grid, with MC's flipped winding (u1/v1 first — buildMoonPhases).
        {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            for (int phase = 0; phase < 8; ++phase) {
                const int col = phase % 4;
                const int row = phase / 4;
                const float u0 = col / 4.0f, u1 = (col + 1) / 4.0f;
                const float v0 = row / 2.0f, v1 = (row + 1) / 2.0f;
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({-1, 0, -1, u1, v1, 255, 255, 255, 255});
                verts.push_back({ 1, 0, -1, u0, v1, 255, 255, 255, 255});
                verts.push_back({ 1, 0,  1, u0, v0, 255, 255, 255, 255});
                verts.push_back({-1, 0,  1, u1, v0, 255, 255, 255, 255});
                indices.insert(indices.end(),
                               {base, base + 1, base + 2, base, base + 2, base + 3});
            }
            m_moonQuads = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                                     indices.data(), indices.size());
        }
    }

    void SkyRenderer::BuildStars() {
        // buildStars: 1500 attempts; RNG consumed even for rejected stars.
        JavaRandom random(kStarSeed);
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        for (int i = 0; i < kStarAttempts; ++i) {
            const float x = random.NextFloat() * 2.0f - 1.0f;
            const float y = random.NextFloat() * 2.0f - 1.0f;
            const float z = random.NextFloat() * 2.0f - 1.0f;
            const float starSize = 0.15f + random.NextFloat() * 0.1f;
            const float lengthSq = x * x + y * y + z * z;
            const float zRot = static_cast<float>(random.NextDouble() * glm::two_pi<double>());
            if (lengthSq <= 0.010000001f || lengthSq >= 1.0f) continue;

            const glm::vec3 center = glm::normalize(glm::vec3(x, y, z)) * 100.0f;
            // JOML Matrix3f.rotateTowards(-center, up): columns [left, upn, ndir].
            const glm::vec3 ndir = glm::normalize(-center);
            glm::vec3 left = glm::cross(glm::vec3(0, 1, 0), ndir);
            const float leftLen = glm::length(left);
            left = leftLen > 1e-6f ? left / leftLen : glm::vec3(1, 0, 0);
            const glm::vec3 upn = glm::cross(ndir, left);
            const glm::mat3 rt(left, upn, ndir);
            const float c = std::cos(-zRot), s = std::sin(-zRot);
            const glm::mat3 rz(glm::vec3(c, s, 0), glm::vec3(-s, c, 0), glm::vec3(0, 0, 1));
            const glm::mat3 rot = rt * rz;

            const glm::vec3 corners[4] = {
                rot * glm::vec3( starSize, -starSize, 0) + center,
                rot * glm::vec3( starSize,  starSize, 0) + center,
                rot * glm::vec3(-starSize,  starSize, 0) + center,
                rot * glm::vec3(-starSize, -starSize, 0) + center,
            };
            const uint32_t base = static_cast<uint32_t>(verts.size());
            for (const auto& p : corners) {
                verts.push_back({p.x, p.y, p.z, 0.0f, 0.0f, 255, 255, 255, 255});
            }
            indices.insert(indices.end(),
                           {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        m_stars = CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                             indices.data(), indices.size());
        Log::Info("SkyRenderer: built %zu stars (seed %lld)",
                  verts.size() / 4, static_cast<long long>(kStarSeed));
    }

    void SkyRenderer::BuildSkyboxCubes() {
        // Inward-facing unit cube, one quad per face, face order matching the
        // panorama_N convention (same geometry as PanoramaRenderer):
        //   0 = front (-Z), 1 = right (+X), 2 = back (+Z), 3 = left (-X),
        //   4 = up, 5 = down.
        // uvScale 1 for panorama sets; 16 for the End sky (MC buildEndSky
        // tiles end_sky.png 16× per face with REPEAT wrap).
        auto buildCube = [this](float uvScale) {
            std::vector<Vertex> verts;
            std::vector<uint32_t> indices;
            const float uv = uvScale;
            auto addFace = [&](glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl) {
                const uint32_t base = static_cast<uint32_t>(verts.size());
                verts.push_back({tl.x, tl.y, tl.z, 0.0f, 0.0f, 255, 255, 255, 255});
                verts.push_back({tr.x, tr.y, tr.z, uv, 0.0f, 255, 255, 255, 255});
                verts.push_back({br.x, br.y, br.z, uv, uv, 255, 255, 255, 255});
                verts.push_back({bl.x, bl.y, bl.z, 0.0f, uv, 255, 255, 255, 255});
                indices.insert(indices.end(), {base, base + 1, base + 2,
                                               base, base + 2, base + 3});
            };
            const float s = 1.0f;
            addFace({-s,  s, -s}, { s,  s, -s}, { s, -s, -s}, {-s, -s, -s});  // 0 front
            addFace({ s,  s, -s}, { s,  s,  s}, { s, -s,  s}, { s, -s, -s});  // 1 right
            addFace({ s,  s,  s}, {-s,  s,  s}, {-s, -s,  s}, { s, -s,  s});  // 2 back
            addFace({-s,  s,  s}, {-s,  s, -s}, {-s, -s, -s}, {-s, -s,  s});  // 3 left
            addFace({-s,  s,  s}, { s,  s,  s}, { s,  s, -s}, {-s,  s, -s});  // 4 up
            addFace({-s, -s, -s}, { s, -s, -s}, { s, -s,  s}, {-s, -s,  s});  // 5 down
            return CreateMesh(verts.data(), verts.size() * sizeof(Vertex),
                              indices.data(), indices.size());
        };
        m_skyboxCube = buildCube(1.0f);
        m_endCube = buildCube(16.0f);
    }

    void SkyRenderer::DestroySkyboxTextures() {
        for (auto& face : m_skyboxFaces) {
            if (face != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(face); face = INVALID_TEXTURE; }
        }
        if (m_endTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_endTexture);
            m_endTexture = INVALID_TEXTURE;
        }
        m_skyboxValid = false;
        m_skyboxIsEnd = false;
    }

    bool SkyRenderer::LoadSkyboxTextures(const std::string& id) {
        DestroySkyboxTextures();

        if (id == "end") {
            m_endTexture = LoadEnvTexture("assets/textures/environment/end_sky.png");
            if (m_endTexture == INVALID_TEXTURE) return false;
            // MC binds end_sky with REPEAT (16× tiling) and default sampling.
            g_renderBackend->SetTextureWrap(m_endTexture, TextureWrap::Repeat, TextureWrap::Repeat);
            // End fog: the tinted average of the texture is ~the MC End haze.
            m_skyboxFogColor = kEndSkyTint * 0.35f;
            m_skyboxIsEnd = true;
            m_skyboxValid = true;
            return true;
        }

        const std::string dir = ResolveSkyboxDir(id);
        if (dir.empty()) return false;

        glm::vec3 horizonAccum{0.0f};
        int horizonSamples = 0;
        bool allValid = true;
        for (int i = 0; i < 6; ++i) {
            // Horizon color from the 4 side faces only (up/down don't touch
            // the horizon).
            const bool sideFace = i < 4;
            m_skyboxFaces[i] = LoadSkyboxFace(dir + "panorama_" + std::to_string(i) + ".png",
                                              sideFace ? &horizonAccum : nullptr,
                                              &horizonSamples);
            if (m_skyboxFaces[i] == INVALID_TEXTURE) allValid = false;
        }
        if (!allValid) {
            DestroySkyboxTextures();
            return false;
        }
        m_skyboxFogColor = horizonSamples > 0 ? horizonAccum / static_cast<float>(horizonSamples)
                                              : glm::vec3(0.5f);
        m_skyboxValid = true;
        return true;
    }

    void SkyRenderer::SetSkybox(const std::string& id, int mode) {
        m_skyboxMode = std::clamp(mode, 0, 2);
        m_skyboxId = id.empty() ? "vanilla" : id;

        if (!m_initialized || !g_renderBackend) {
            return;  // applied lazily if Initialize runs later
        }
        if (m_skyboxId == "vanilla") {
            DestroySkyboxTextures();
            EnvironmentState::Get().SetSkyboxOverride(false, glm::vec3(0.5f), m_skyboxMode);
            return;
        }
        if (!LoadSkyboxTextures(m_skyboxId)) {
            Log::Warning("SkyRenderer: skybox '%s' not found — using vanilla sky",
                         m_skyboxId.c_str());
            m_skyboxId = "vanilla";
            EnvironmentState::Get().SetSkyboxOverride(false, glm::vec3(0.5f), m_skyboxMode);
            return;
        }
        EnvironmentState::Get().SetSkyboxOverride(true, m_skyboxFogColor, m_skyboxMode);
        Log::Info("SkyRenderer: skybox '%s' (mode %d)", m_skyboxId.c_str(), m_skyboxMode);
    }

    bool SkyRenderer::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;

        m_shader = CreateSkyShader(vertexShaderSource, fragmentShaderSource);
        if (m_shader == INVALID_SHADER) {
            Log::Warning("SkyRenderer: failed to create shader — sky disabled");
            return false;
        }

        unsigned char white[] = {255, 255, 255, 255};
        m_whiteTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        m_sunTexture = LoadEnvTexture("assets/textures/environment/sun.png");
        m_moonTexture = LoadEnvTexture("assets/textures/environment/moon_phases.png");

        BuildSkyDiscs();
        BuildSunriseFan();
        BuildCelestialQuads();
        BuildStars();
        BuildSkyboxCubes();

        m_initialized = true;
        Log::Info("SkyRenderer initialized");

        // A skybox selected before init (session start races renderer init
        // only in pathological orders — handle it anyway).
        if (m_skyboxId != "vanilla") {
            SetSkybox(m_skyboxId, m_skyboxMode);
        }
        return true;
    }

    void SkyRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroySkyboxTextures();
        DestroyMesh(m_skyboxCube);
        DestroyMesh(m_endCube);
        DestroyMesh(m_topDisc);
        DestroyMesh(m_bottomDisc);
        DestroyMesh(m_sunriseFan);
        DestroyMesh(m_sunQuad);
        DestroyMesh(m_moonQuads);
        DestroyMesh(m_stars);
        if (m_whiteTexture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_whiteTexture); m_whiteTexture = INVALID_TEXTURE; }
        if (m_sunTexture != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_sunTexture);   m_sunTexture = INVALID_TEXTURE; }
        if (m_moonTexture != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_moonTexture);  m_moonTexture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)        { g_renderBackend->DestroyShader(m_shader);        m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void SkyRenderer::Render(const glm::mat4& proj, const glm::mat4& viewRotation) {
        if (!m_initialized || !g_renderBackend) return;

        const EnvironmentState& envState = EnvironmentState::Get();
        const EnvironmentFrame& env = envState.Frame();
        const glm::mat4 vp = proj * viewRotation;
        const float sunAngleRad = glm::radians(env.sunAngleDeg);

        PipelineState state;
        state.depthTestEnabled = false;
        state.depthWriteEnabled = false;
        state.blendEnabled = false;
        state.cullMode = CullMode::None;
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);

        auto draw = [&](const Mesh& m, const glm::mat4& model, const glm::vec4& color,
                        TextureHandle tex, const glm::vec4& fogEnv,
                        uint32_t indexCount = 0, uint32_t indexOffset = 0) {
            g_renderBackend->BindTexture(tex != INVALID_TEXTURE ? tex : m_whiteTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", vp * model);
            g_renderBackend->SetUniformVec4(m_shader, "uColor", color);
            g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
            g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", fogEnv);
            g_renderBackend->DrawIndexed(m.mesh, indexCount ? indexCount : m.indexCount, indexOffset);
        };

        // MC sky fog: apply_fog(..., 0, FogSkyEnd, FogSkyEnd, FogSkyEnd, FogColor)
        const glm::vec4 skyFog{0.0f, env.fogSkyEnd, env.fogSkyEnd, env.fogSkyEnd};

        // ── Cubemap skybox path ─────────────────────────────────────────────
        // Replaces the sky disc / sunrise fan / dark disc. Mode 0 (static)
        // and 1 (darken) stop here; mode 2 continues into the celestial pass
        // so the sun/moon/stars ride on top of the skybox.
        const bool skyboxActive = m_skyboxValid && m_skyboxId != "vanilla";
        if (skyboxActive) {
            const float brightness = (m_skyboxMode == 0) ? 1.0f : env.skyBrightness;
            RenderSkybox(vp, brightness);
            if (m_skyboxMode != 2) {
                g_renderBackend->UnbindMesh();
                PipelineState defaultState;
                defaultState.depthTestEnabled = true;
                defaultState.depthWriteEnabled = true;
                defaultState.blendEnabled = false;
                defaultState.cullMode = CullMode::Back;
                g_renderBackend->SetPipelineState(defaultState);
                return;
            }
        } else {
            // 1. Sky disc (opaque).
            draw(m_topDisc, glm::mat4(1.0f), glm::vec4(env.skyColor, 1.0f), m_whiteTexture, skyFog);

            // 2. Sunrise/sunset glow fan (translucent).
            if (env.sunriseColor.a > 0.001f) {
                state.blendEnabled = true;
                state.srcBlendFactor = BlendFactor::SrcAlpha;
                state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
                g_renderBackend->SetPipelineState(state);

                const float flip = std::sin(sunAngleRad) < 0.0f ? 180.0f : 0.0f;
                glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
                model = glm::rotate(model, glm::radians(flip + 90.0f), glm::vec3(0, 0, 1));
                model = glm::scale(model, glm::vec3(1.0f, 1.0f, env.sunriseColor.a));
                draw(m_sunriseFan, model, env.sunriseColor, m_whiteTexture, kFogOff);
            }
        }

        // 3. Celestial bodies — additive "overlay" blend (SRC_ALPHA, ONE).
        state.blendEnabled = true;
        state.srcBlendFactor = BlendFactor::SrcAlpha;
        state.dstBlendFactor = BlendFactor::One;
        g_renderBackend->SetPipelineState(state);

        const glm::mat4 celestialBase =
            glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 1, 0));

        // Sun.
        if (m_sunTexture != INVALID_TEXTURE) {
            glm::mat4 model = glm::rotate(celestialBase, sunAngleRad, glm::vec3(1, 0, 0));
            model = glm::translate(model, glm::vec3(0, kCelestialHeight, 0));
            model = glm::scale(model, glm::vec3(kSunSize, 1.0f, kSunSize));
            draw(m_sunQuad, model, glm::vec4(1.0f), m_sunTexture, kFogOff);
        }

        // Moon (phase quad selected by index offset).
        if (m_moonTexture != INVALID_TEXTURE) {
            glm::mat4 model =
                glm::rotate(celestialBase, glm::radians(env.moonAngleDeg), glm::vec3(1, 0, 0));
            model = glm::translate(model, glm::vec3(0, kCelestialHeight, 0));
            model = glm::scale(model, glm::vec3(kMoonSize, 1.0f, kMoonSize));
            draw(m_moonQuads, model, glm::vec4(1.0f), m_moonTexture, kFogOff,
                 6, static_cast<uint32_t>(env.moonPhase) * 6);
        }

        // Stars.
        if (env.starBrightness > 0.0f && m_stars.indexCount > 0) {
            const glm::mat4 model =
                glm::rotate(celestialBase, glm::radians(env.starAngleDeg), glm::vec3(1, 0, 0));
            const float b = env.starBrightness;
            draw(m_stars, model, glm::vec4(b, b, b, b), m_whiteTexture, kFogOff);
        }

        // 4. Dark disc below the horizon (renderDarkDisc: translate +12 y).
        //    Vanilla sky only — a skybox has its own bottom face.
        if (!skyboxActive && envState.ShouldRenderDarkDisc()) {
            state.blendEnabled = false;
            g_renderBackend->SetPipelineState(state);
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 12, 0));
            draw(m_bottomDisc, model, glm::vec4(0, 0, 0, 1), m_whiteTexture, skyFog);
        }

        g_renderBackend->UnbindMesh();

        // Restore default pipeline state for the terrain pass.
        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

    void SkyRenderer::RenderSkybox(const glm::mat4& viewProj, float brightness) {
        // Camera-centered inward cube (MC buildEndSky places faces at ±100;
        // the exact scale is irrelevant with depth write off).
        const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
        const glm::mat4 mvp = viewProj * model;
        const glm::vec3 tint = (m_skyboxIsEnd ? kEndSkyTint : glm::vec3(1.0f)) * brightness;

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformVec4(m_shader, "uColor", glm::vec4(tint, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", kFogOff);

        if (m_skyboxIsEnd) {
            g_renderBackend->BindTexture(m_endTexture, 0);
            g_renderBackend->DrawIndexed(m_endCube.mesh, m_endCube.indexCount);
        } else {
            // One draw per face so each binds its own 2D texture (same
            // trick as PanoramaRenderer — no cubemap sampler needed).
            for (int i = 0; i < 6; ++i) {
                g_renderBackend->BindTexture(m_skyboxFaces[i], 0);
                g_renderBackend->DrawIndexed(m_skyboxCube.mesh, 6, static_cast<uint32_t>(i) * 6);
            }
        }
    }

} // namespace Render
