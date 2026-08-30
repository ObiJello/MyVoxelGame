// File: src/client/renderer/entity/XpOrbRenderer.cpp
#include "XpOrbRenderer.hpp"

#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert (24-byte block layout)
#include "client/entity/XpOrbManager.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/core/Log.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    bool XpOrbRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Same block shader (and the same VK caveat) as ItemEntityRenderer —
        // single 2D texture, vertex-colour multiply, fog environment.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/block.vert", "shaders/block.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/block.vert", "shaders/block.frag");
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[XpOrbRenderer] failed to load block shader — "
                         "experience orbs will not render");
            return false;
        }

        // MC's textures/entity/experience_orb.png — a 64×64 sheet, 4×4 grid
        // of 16 px sprites.
        {
            const std::string path = PlatformMain::GetAssetPath(
                "assets/textures/entity/experience_orb.png");
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch,
                                              STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[XpOrbRenderer] failed to load %s", path.c_str());
                g_renderBackend->DestroyShader(m_shader);
                m_shader = INVALID_SHADER;
                return false;
            }
            m_texture = g_renderBackend->CreateTexture2D(
                w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (m_texture != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(m_texture,
                    TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(m_texture,
                    TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
        }

        // One quad, streamed per orb (UVs and the pulsing colour change every
        // draw; four vertices is nothing).
        m_vb = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, 4 * sizeof(ItemCubeVert),
            nullptr, BufferAccess::Streaming);
        const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        m_ib = g_renderBackend->CreateBuffer(
            BufferUsage::Index, sizeof(indices), indices, BufferAccess::Static);
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        m_initialized = true;
        Log::Info("[XpOrbRenderer] initialized");
        return true;
    }

    void XpOrbRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_mesh != INVALID_MESH)      { g_renderBackend->DestroyMesh(m_mesh); m_mesh = INVALID_MESH; }
        if (m_vb   != INVALID_BUFFER)    { g_renderBackend->DestroyBuffer(m_vb); m_vb = INVALID_BUFFER; }
        if (m_ib   != INVALID_BUFFER)    { g_renderBackend->DestroyBuffer(m_ib); m_ib = INVALID_BUFFER; }
        if (m_texture != INVALID_TEXTURE){ g_renderBackend->DestroyTexture(m_texture); m_texture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)  { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void XpOrbRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                               const glm::vec3& cameraPos, float partialTick) {
        if (!m_initialized || !g_renderBackend) return;
        if (!Client::g_xpOrbManager) return;

        const auto& entities = Client::g_xpOrbManager->GetEntities();
        const auto& pickups  = Client::g_xpOrbManager->GetPickups();
        if (entities.empty() && pickups.empty()) return;

        const glm::mat4 viewProj = projection * view;
        const float maxDistSq = kMaxRenderDistance * kMaxRenderDistance;

        // Camera-facing rotation — the inverse (= transpose, it's a pure
        // rotation) of the view matrix's rotation block. MC's
        // `poseStack.mulPose(camera.orientation)`.
        const glm::mat3 billboard = glm::transpose(glm::mat3(view));

        // ── Shared pipeline state / uniforms, set once ─────────────────────
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.blendEnabled      = true;
        state.srcBlendFactor    = BlendFactor::SrcAlpha;
        state.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        // The quad's winding flips with the billboard orientation — culling
        // would blank orbs for half the view directions.
        state.cullMode  = CullMode::None;
        state.frontFace = FrontFace::CounterClockwise;
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_texture, 0);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.01f);
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane",
            glm::vec4(0.0f));

        const auto& env = EnvironmentState::Get().Frame();
        g_renderBackend->SetUniformFloat(m_shader, "uSkyBrightness", env.skyBrightness);
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor",
            glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", cameraPos);

        // ── Orbs in the world ──────────────────────────────────────────────
        for (const auto& [id, ce] : entities) {
            const glm::vec3 pos = glm::vec3(
                glm::mix(ce.renderPrevPosition, ce.sim.pos,
                         static_cast<double>(partialTick)));

            const glm::vec3 d = pos - cameraPos;
            if (glm::dot(d, d) > maxDistSq) continue;

            DrawOrb(ce.sim.value, pos, ce.ageTicks + partialTick,
                    viewProj, billboard);
        }

        // ── Orbs flying into whoever absorbed them ─────────────────────────
        for (const auto& p : pickups) {
            // Same eased flight as the item pickup (t², the "snap").
            float t = (static_cast<float>(p.life) + partialTick)
                    / static_cast<float>(Client::XpOrbManager::kPickupLifeTicks);
            t = glm::clamp(t, 0.0f, 1.0f);
            t *= t;

            const glm::dvec3 target = p.targetSeeded
                ? glm::mix(p.targetPosOld, p.targetPos, static_cast<double>(partialTick))
                : p.startPos;
            const glm::vec3 pos =
                glm::vec3(glm::mix(p.startPos, target, static_cast<double>(t)));

            const glm::vec3 d = pos - cameraPos;
            if (glm::dot(d, d) > maxDistSq) continue;

            // Frozen colour phase, like the item's frozen spin.
            DrawOrb(p.value, pos, p.ageTicks, viewProj, billboard);
        }

        g_renderBackend->UnbindMesh();
    }

    void XpOrbRenderer::DrawOrb(int value, const glm::vec3& worldPos,
                                float ageTicks, const glm::mat4& viewProj,
                                const glm::mat3& billboard) {
        // Sprite cell from the value (ExperienceOrb.getIcon), on the 64×64
        // sheet's 4×4 grid.
        const int icon = Game::ExperienceOrb::GetIcon(value);
        const int col  = icon % 4;
        const int row  = icon / 4;
        const float u0 = static_cast<float>(col * 16)      / 64.0f;
        const float u1 = static_cast<float>(col * 16 + 16) / 64.0f;
        const float v0 = static_cast<float>(row * 16)      / 64.0f;
        const float v1 = static_cast<float>(row * 16 + 16) / 64.0f;

        // MC's colour pulse: red and blue ride phase-shifted sines on
        // ageInTicks/2 while green stays full — the yellow-green shimmer.
        const float phase = ageTicks / 2.0f;
        const auto channel = [](float s, float scale) {
            return static_cast<uint8_t>(
                glm::clamp((s + 1.0f) * scale * 255.0f, 0.0f, 255.0f));
        };
        const uint8_t r = channel(std::sin(phase), 0.5f);
        const uint8_t g = 255;
        const uint8_t b = channel(std::sin(phase + 4.1887903f), 0.1f);
        const uint8_t a = 128;   // MC renders orbs half-transparent

        // MC's quad, in the 0.3-scaled billboard frame, raised 0.1 off the
        // entity origin: x ∈ [-0.5, 0.5], y ∈ [-0.25, 0.75].
        const ItemCubeVert verts[4] = {
            { -0.5f, -0.25f, 0.0f, u0, v1, r, g, b, a },
            {  0.5f, -0.25f, 0.0f, u1, v1, r, g, b, a },
            {  0.5f,  0.75f, 0.0f, u1, v0, r, g, b, a },
            { -0.5f,  0.75f, 0.0f, u0, v0, r, g, b, a },
        };
        g_renderBackend->UpdateBuffer(m_vb, 0, sizeof(verts), verts);

        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                         worldPos + glm::vec3(0.0f, 0.1f, 0.0f));
        model = model * glm::mat4(billboard);
        model = glm::scale(model, glm::vec3(0.3f));

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProj * model);
        g_renderBackend->DrawIndexed(m_mesh, 6);
    }

} // namespace Render
