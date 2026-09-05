// File: src/client/renderer/entity/XpOrbRenderer.cpp
#include "client/resource/ResourcePacks.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "XpOrbRenderer.hpp"

#include "EntityCulling.hpp"
#include "../backend/RenderBackend.hpp"
#include "../environment/EnvironmentState.hpp"
#include "client/entity/XpOrbManager.hpp"
#include "common/entity/ExperienceOrb.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif

#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string>

namespace PlatformMain { std::string GetAssetPath(const std::string&); }

namespace Render {

    bool XpOrbRenderer::LoadTexture() {
        if (m_texture != INVALID_TEXTURE) { g_renderBackend->DestroyTexture(m_texture); m_texture = INVALID_TEXTURE; }
        const std::string path = PlatformMain::GetAssetPath("assets/textures/entity/experience_orb.png");
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[XpOrbRenderer] failed to load %s", path.c_str());
            return false;
        }
        m_texture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        if (m_texture != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(m_texture, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap(m_texture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        return m_texture != INVALID_TEXTURE;
    }

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
        if (!LoadTexture()) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
            return false;
        }

        // The static quad-pattern index buffer shared by both vertex sets.
        {
            std::vector<uint32_t> indices;
            indices.reserve(kMaxOrbs * 6);
            for (uint32_t k = 0; k < kMaxOrbs; ++k) {
                const uint32_t b = k * 4;
                indices.insert(indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
            }
            m_ib = g_renderBackend->CreateBuffer(
                BufferUsage::Index, indices.size() * sizeof(uint32_t),
                indices.data(), BufferAccess::Static);
        }
        for (FrameBuffers& fb : m_frames) {
            fb.vb = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kMaxOrbs * 4 * sizeof(ItemCubeVert),
                nullptr, BufferAccess::Streaming);
            fb.mesh = g_renderBackend->CreateMesh(fb.vb, m_ib, GetBlockVertexLayout());
        }
        m_verts.reserve(256 * 4);

        m_initialized = true;
        Log::Info("[XpOrbRenderer] initialized");
        return true;
    }

    void XpOrbRenderer::Shutdown() {
        if (!g_renderBackend) return;
        for (FrameBuffers& fb : m_frames) {
            if (fb.mesh != INVALID_MESH) { g_renderBackend->DestroyMesh(fb.mesh); fb.mesh = INVALID_MESH; }
            if (fb.vb != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(fb.vb); fb.vb = INVALID_BUFFER; }
        }
        if (m_ib   != INVALID_BUFFER)    { g_renderBackend->DestroyBuffer(m_ib); m_ib = INVALID_BUFFER; }
        if (m_texture != INVALID_TEXTURE){ g_renderBackend->DestroyTexture(m_texture); m_texture = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)  { g_renderBackend->DestroyShader(m_shader); m_shader = INVALID_SHADER; }
        m_initialized = false;
    }

    void XpOrbRenderer::Render(const glm::mat4& projection, const glm::mat4& view,
                               const glm::vec3& cameraPos, const Frustum& frustum,
                               float partialTick) {
        // Resource pack reload: the sheet is read again.
        if (m_initialized && g_renderBackend && Resources::CacheStale(m_packGeneration)) LoadTexture();
        PROFILE_ZONE_N("XpOrbRender");
        if (!m_initialized || !g_renderBackend) return;
        if (!Client::g_xpOrbManager) return;

        const auto& entities = Client::g_xpOrbManager->GetEntities();
        const auto& pickups  = Client::g_xpOrbManager->GetPickups();
        if (entities.empty() && pickups.empty()) return;

        // Which set this call writes, and where in it — see EntityFrame.hpp.
        if (m_frameCursor.Advance()) m_orbCursor = 0;
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH || m_orbCursor >= kMaxOrbs) return;
        const size_t orbRoom = kMaxOrbs - m_orbCursor;

        // MC shouldRenderAtSqrDistance: kMaxRenderDistance (0.5 x 64) times
        // the per-frame view scale, which is where the Entity Distance
        // option and the render-distance term come in.
        const float maxDist = kMaxRenderDistance * EntityCulling::GetViewScale();
        const float maxDistSq = maxDist * maxDist;

        // Camera-facing rotation — the inverse (= transpose, it's a pure
        // rotation) of the view matrix's rotation block. MC's
        // `poseStack.mulPose(camera.orientation)`.
        const glm::mat3 billboard = glm::transpose(glm::mat3(view));

        m_verts.clear();

        // ── Orbs in the world ──────────────────────────────────────────────
        for (const auto& [id, ce] : entities) {
            if (m_verts.size() / 4 >= orbRoom) break;
            const glm::vec3 pos = glm::vec3(
                glm::mix(ce.renderPrevPosition, ce.sim.pos,
                         static_cast<double>(partialTick)));

            const glm::vec3 d = pos - cameraPos;
            if (glm::dot(d, d) > maxDistSq) continue;

            // MC extractVisibleEntities: the inflated culling box against the
            // frustum, then the visible-section gate.
            if (!EntityCulling::ShouldRender(frustum, pos,
                                             Game::ExperienceOrb::kWidth,
                                             Game::ExperienceOrb::kHeight)) {
                continue;
            }

            AppendOrb(ce.sim.value, pos, ce.ageTicks + partialTick, billboard);
        }

        // ── Orbs flying into whoever absorbed them ─────────────────────────
        for (const auto& p : pickups) {
            if (m_verts.size() / 4 >= orbRoom) break;
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
            // The flight is short and aimed at a player; the frustum test is
            // enough — a section gate on something crossing sections every
            // frame would only flicker.
            if (!EntityCulling::BoxInFrustum(frustum, pos,
                                             Game::ExperienceOrb::kWidth,
                                             Game::ExperienceOrb::kHeight)) {
                continue;
            }
            {
                const glm::vec3 halfXZ(Game::ExperienceOrb::kWidth * 0.5f, 0.0f, Game::ExperienceOrb::kWidth * 0.5f);
                if (!EntityCulling::PassesCrossingFilter(pos - halfXZ,
                        pos + halfXZ + glm::vec3(0.0f, Game::ExperienceOrb::kHeight, 0.0f))) continue;
            }

            // Frozen colour phase, like the item's frozen spin.
            AppendOrb(p.value, pos, p.ageTicks, billboard);
        }

        if (m_verts.empty()) return;
        const size_t orbCount = m_verts.size() / 4;

        // One upload at this call's cursor, one draw over its index range.
        g_renderBackend->UpdateBuffer(fb.vb, m_orbCursor * 4 * sizeof(ItemCubeVert),
                                      m_verts.size() * sizeof(ItemCubeVert), m_verts.data());
        const size_t firstIndex = m_orbCursor * 6;
        m_orbCursor += orbCount;

        // ── Pipeline state / uniforms, set once for the batch ──────────────
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
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane", ::Render::ChunkRenderer::PortalEntityClipPlane());

        const auto& env = EnvironmentState::Get().Frame();
        g_renderBackend->SetUniformFloat(m_shader, "uSkyBrightness", env.skyBrightness);
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor",
            glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", cameraPos);

        // The quads are already in world space, so the MVP is the bare
        // view-projection — the same thing the per-orb model matrix used to
        // fold in, applied on the CPU instead.
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));   // world-space quads
        g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(orbCount * 6),
                                     static_cast<uint32_t>(firstIndex));

        g_renderBackend->UnbindMesh();
    }

    void XpOrbRenderer::AppendOrb(int value, const glm::vec3& worldPos,
                                  float ageTicks, const glm::mat3& billboard) {
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
        // entity origin: x ∈ [-0.5, 0.5], y ∈ [-0.25, 0.75]. The model chain
        // (translate, billboard rotate, scale 0.3) applied per corner here.
        const glm::vec3 origin = worldPos + glm::vec3(0.0f, 0.1f, 0.0f);
        const auto corner = [&](float x, float y, float u, float v) {
            const glm::vec3 p = origin + billboard * (glm::vec3(x, y, 0.0f) * 0.3f);
            m_verts.push_back({ p.x, p.y, p.z, u, v, r, g, b, a });
        };
        corner(-0.5f, -0.25f, u0, v1);
        corner( 0.5f, -0.25f, u1, v1);
        corner( 0.5f,  0.75f, u1, v0);
        corner(-0.5f,  0.75f, u0, v0);
    }

} // namespace Render
