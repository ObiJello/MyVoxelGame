// File: src/client/renderer/gui/screens/PanoramaRenderer.cpp
#include "PanoramaRenderer.hpp"
#include "Widgets.hpp"   // ApplyAlpha
#include "../GuiGraphics.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "../../ext/stb_image/stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    PanoramaRenderer g_panoramaRenderer;

    // Vertex layout reuses the block layout (pos3 + uv2 + color4 ubyte) so we
    // can build the mesh with GetBlockVertexLayout like BlockBreakOverlay.
    const char* PanoramaRenderer::vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
)";

    const char* PanoramaRenderer::fragmentShaderSource = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
void main() {
    FragColor = vec4(texture(uTexture, vUV).rgb, 1.0);
}
)";

    namespace {
        // Every panorama Minecraft has shipped, oldest first. Folder must
        // exist under assets/textures/gui/title/background/ to be offered.
        const PanoramaRenderer::SetInfo kSets[] = {
            {"classic", "Classic"},   // Beta 1.8 – 1.12
            {"1.13",    "1.13"},      // Update Aquatic
            {"1.14",    "1.14"},      // Village & Pillage
            {"1.15",    "1.15"},      // Buzzy Bees
            {"1.16",    "1.16"},      // Nether Update
            {"1.17",    "1.17"},      // Caves & Cliffs I
            {"1.18",    "1.18"},      // Caves & Cliffs II
            {"1.19",    "1.19"},      // The Wild Update
            {"1.20",    "1.20"},      // Trails & Tales
            {"1.21",    "1.21"},      // Tricky Trials
            {"1.21.4",  "1.21.4"},    // The Garden Awakens
            {"1.21.5",  "1.21.5"},    // Spring to Life
            {"1.21.6",  "1.21.6"},    // Chase the Skies
            {"1.21.9",  "1.21.9"},    // Mounts of Mayhem
            {"1.21.11", "1.21.11"},
            {"26.1",    "26.1"},
            {"26.2",    "26.2"},
        };

        std::string SetDir(const std::string& slug) {
            return "assets/textures/gui/title/background/" + slug + "/";
        }

        // Loads one face texture. Returns INVALID_TEXTURE when the file is
        // missing OR is one of the 1×1 placeholder stubs in the asset dump.
        TextureHandle LoadFaceTexture(const std::string& relPath, bool& wasStub) {
            wasStub = false;
            const std::string full = PlatformMain::GetAssetPath(relPath);
            if (!std::filesystem::exists(full)) return INVALID_TEXTURE;

            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) return INVALID_TEXTURE;
            if (w <= 1 || h <= 1) {
                wasStub = true;
                stbi_image_free(pixels);
                return INVALID_TEXTURE;
            }
            TextureHandle t = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (t != INVALID_TEXTURE) {
                // Linear filtering: the skybox is heavily magnified.
                g_renderBackend->SetTextureFilter(t, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap(t, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return t;
        }
    } // namespace

    std::vector<PanoramaRenderer::SetInfo> PanoramaRenderer::AvailableSets() {
        std::vector<SetInfo> out;
        for (const auto& set : kSets) {
            const std::string probe =
                PlatformMain::GetAssetPath(SetDir(set.slug) + "panorama_0.png");
            if (std::filesystem::exists(probe)) out.push_back(set);
        }
        return out;
    }

    bool PanoramaRenderer::Initialize(const std::string& setSlug) {
        if (m_initialized) return true;
        if (!g_renderBackend) return false;

        // Try SPIR-V (Vulkan rewrites to panorama_vk.*.spv), then fall back
        // to GLSL source (OpenGL) — same pattern as BlockBreakOverlay.
        m_shader = g_renderBackend->CreateShaderFromFiles(
            "shaders/panorama.vert", "shaders/panorama.frag");
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("PanoramaRenderer: shader creation failed");
            return false;
        }

        // Inward-facing unit cube around the origin. One face per draw so
        // each can bind its own 2D texture (equivalent to MC's cubemap
        // sampler, no new backend capability needed).
        //
        // Face order matches the panorama_N convention:
        //   0 = front (-Z when yaw 0), 1 = right (+X), 2 = back (+Z),
        //   3 = left (-X), 4 = up, 5 = down.
        struct V { float x, y, z; float u, v; uint8_t r, g, b, a; };
        static_assert(sizeof(V) == 24, "vertex stride must match block layout");
        const uint8_t W = 255;
        std::vector<V> verts;
        std::vector<uint32_t> idx;
        verts.reserve(24);
        idx.reserve(36);

        // Adds one face as (top-left, top-right, bottom-right, bottom-left)
        // as seen from INSIDE the cube, with upright UVs.
        auto addFace = [&](glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl) {
            uint32_t base = static_cast<uint32_t>(verts.size());
            verts.push_back({tl.x, tl.y, tl.z, 0.0f, 0.0f, W, W, W, W});
            verts.push_back({tr.x, tr.y, tr.z, 1.0f, 0.0f, W, W, W, W});
            verts.push_back({br.x, br.y, br.z, 1.0f, 1.0f, W, W, W, W});
            verts.push_back({bl.x, bl.y, bl.z, 0.0f, 1.0f, W, W, W, W});
            idx.insert(idx.end(), {base + 0, base + 1, base + 2,
                                   base + 0, base + 2, base + 3});
        };

        const float s = 1.0f;
        // 0: front (-Z)
        addFace({-s,  s, -s}, { s,  s, -s}, { s, -s, -s}, {-s, -s, -s});
        // 1: right (+X)
        addFace({ s,  s, -s}, { s,  s,  s}, { s, -s,  s}, { s, -s, -s});
        // 2: back (+Z)
        addFace({ s,  s,  s}, {-s,  s,  s}, {-s, -s,  s}, { s, -s,  s});
        // 3: left (-X)
        addFace({-s,  s,  s}, {-s,  s, -s}, {-s, -s, -s}, {-s, -s,  s});
        // 4: up (+Y) — top edge continues from the front face
        addFace({-s,  s,  s}, { s,  s,  s}, { s,  s, -s}, {-s,  s, -s});
        // 5: down (-Y)
        addFace({-s, -s, -s}, { s, -s, -s}, { s, -s,  s}, {-s, -s,  s});

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            verts.size() * sizeof(V), verts.data());
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
            idx.size() * sizeof(uint32_t), idx.data());
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        m_initialized = true;
        LoadSet(setSlug);
        return true;
    }

    void PanoramaRenderer::DestroyFaceTextures() {
        for (auto& f : m_faces) {
            if (f != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(f); f = INVALID_TEXTURE; }
        }
        if (m_overlay != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_overlay); m_overlay = INVALID_TEXTURE; }
    }

    // Loads the six faces (+ optional overlay) of one set. All six must be
    // present and not 1×1 stubs; on failure the previous textures are gone
    // and m_texturesValid is false (gradient fallback).
    bool PanoramaRenderer::TryLoadSet(const std::string& slug) {
        DestroyFaceTextures();
        const std::string dir = SetDir(slug);
        bool allValid = true;
        for (int i = 0; i < 6; ++i) {
            bool stub = false;
            m_faces[i] = LoadFaceTexture(dir + "panorama_" + std::to_string(i) + ".png", stub);
            if (m_faces[i] == INVALID_TEXTURE) allValid = false;
        }
        if (!allValid) {
            DestroyFaceTextures();
            m_texturesValid = false;
            return false;
        }
        bool stub = false;
        m_overlay = LoadFaceTexture(dir + "panorama_overlay.png", stub);
        m_texturesValid = true;
        m_currentSet = slug;
        return true;
    }

    void PanoramaRenderer::LoadSet(const std::string& slug) {
        if (!m_initialized || !g_renderBackend) return;
        std::string want = slug.empty() ? kDefaultSet : slug;
        if (want == kRandomSet) {
            const auto sets = AvailableSets();
            if (!sets.empty()) {
                static std::mt19937 rng{std::random_device{}()};
                std::uniform_int_distribution<size_t> pick(0, sets.size() - 1);
                want = sets[pick(rng)].slug;
                Log::Info("PanoramaRenderer: random panorama -> '%s'", want.c_str());
            } else {
                want = kDefaultSet;
            }
        }
        if (TryLoadSet(want)) return;
        if (want != kDefaultSet && TryLoadSet(kDefaultSet)) {
            Log::Warning("PanoramaRenderer: set '%s' missing — fell back to '%s'",
                         want.c_str(), kDefaultSet);
            return;
        }
        Log::Info("PanoramaRenderer: panorama textures missing — using gradient fallback "
                  "(expected assets/textures/gui/title/background/%s/panorama_0..5.png)",
                  want.c_str());
    }

    void PanoramaRenderer::Shutdown() {
        if (!g_renderBackend) return;
        DestroyFaceTextures();
        if (m_mesh   != INVALID_MESH)   { g_renderBackend->DestroyMesh(m_mesh);     m_mesh = INVALID_MESH; }
        if (m_vb     != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_vb);     m_vb = INVALID_BUFFER; }
        if (m_ib     != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(m_ib);     m_ib = INVALID_BUFFER; }
        if (m_shader != INVALID_SHADER) { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
        m_texturesValid = false;
    }

    void PanoramaRenderer::Render(int fbWidth, int fbHeight, float deltaSeconds, float speed) {
        if (!g_renderBackend || fbWidth <= 0 || fbHeight <= 0) return;

        // MC PanoramaRenderer.render: spin += realtimeTicks * speed * 0.1 per
        // frame → 0.1°/tick = 2°/second at speed 1.0.
        m_spin += deltaSeconds * 20.0f * 0.1f * speed;
        while (m_spin >= 360.0f) m_spin -= 360.0f;

        if (!m_texturesValid || m_mesh == INVALID_MESH) return;

        PipelineState state;
        state.depthTestEnabled  = false;
        state.depthWriteEnabled = false;
        state.blendEnabled      = false;
        state.cullMode          = CullMode::None;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);

        // MC CubeMap.render: 85° perspective, fixed 10° downward pitch,
        // yaw = -spin.
        const float aspect = static_cast<float>(fbWidth) / static_cast<float>(fbHeight);
        glm::mat4 proj = glm::perspective(glm::radians(85.0f), aspect, 0.05f, 10.0f);
        glm::mat4 view(1.0f);
        view = glm::rotate(view, glm::radians(10.0f),  glm::vec3(1, 0, 0));
        view = glm::rotate(view, glm::radians(-m_spin), glm::vec3(0, 1, 0));
        glm::mat4 mvp = proj * view;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformInt(m_shader, "uTexture", 0);

        // One draw per face, 6 indices each, so each face binds its texture.
        // DrawIndexed's offset is in index ELEMENTS (GLBackend multiplies by
        // sizeof(uint32_t) itself).
        for (uint32_t face = 0; face < 6; ++face) {
            g_renderBackend->BindTexture(m_faces[face], 0);
            g_renderBackend->DrawIndexed(m_mesh, 6, face * 6);
        }
        g_renderBackend->UnbindMesh();

        // Restore defaults for the GUI pass.
        PipelineState def;
        def.depthTestEnabled  = true;
        def.depthWriteEnabled = true;
        def.blendEnabled      = false;
        def.cullMode          = CullMode::Back;
        g_renderBackend->SetPipelineState(def);
    }

    void PanoramaRenderer::RenderOverlay(GuiGraphics& g, int guiWidth, int guiHeight, float alpha) {
        if (!m_texturesValid) {
            // Gradient fallback: deep night-sky wash so logo/buttons read well.
            g.FillGradient(0, 0, guiWidth, guiHeight, 0xFF0B1026, 0xFF05070F);
            return;
        }
        if (m_overlay != INVALID_TEXTURE) {
            g.Blit(m_overlay, 0, 0, guiWidth, guiHeight, 0.0f, 0.0f, 1.0f, 1.0f,
                   ApplyAlpha(0xFFFFFFFF, alpha));
        }
    }

} // namespace Render
