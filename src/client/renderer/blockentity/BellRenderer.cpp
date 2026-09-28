// File: src/client/renderer/blockentity/BellRenderer.cpp
#include "BellRenderer.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../core/RenderOrigin.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/block/entity/BellBlockEntity.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace Render {

    BellRenderer::~BellRenderer() { Shutdown(); }

    bool BellRenderer::Initialize() {
        if (!g_renderBackend) return false;
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[BellRenderer] shader compile failed");
            return false;
        }
        using namespace BEModel;
        // BellModel.createBodyLayer (32x32): bell_body at (8,12,8), its
        // bell_base child at (-8,-12,-8) from it. The mesh is in bell_body's
        // frame (block units); the swing rotates that frame.
        const glm::mat4 bodyCube = CubeSpace(glm::mat4(1.0f));
        const glm::mat4 baseCube = CubeSpace(PartPose(glm::mat4(1.0f), {-8.0f, -12.0f, -8.0f}));
        std::vector<Vert> verts;
        std::vector<uint32_t> idx;
        for (int set = 0; set < 2; ++set) {
            const auto lightSet = set ? EntityLighting::LightSet::Nether : EntityLighting::LightSet::Default;
            const glm::mat3 n(1.0f);   // no model transform: world-aligned
            AddCube(verts, idx, {-3, -6, -3}, {3, 1, 3}, 0, 0, 6, 7, 6, 32, 32, kAll, bodyCube, n, lightSet);
            AddCube(verts, idx, {4, 4, 4}, {12, 6, 12}, 0, 13, 8, 2, 8, 32, 32, kAll, baseCube, n, lightSet);
        }
        m_body = Upload(verts, idx, 2);
        return m_body.Valid();
    }

    void BellRenderer::Shutdown() {
        BEModel::Destroy(m_body);
        m_textures.Clear();
        if (g_renderBackend && m_shader != INVALID_SHADER) g_renderBackend->DestroyShader(m_shader);
        m_shader = INVALID_SHADER;
    }

    void BellRenderer::Render(const Game::BlockEntity& be,
                              float partialTick,
                              const glm::mat4& proj,
                              const glm::mat4& view,
                              const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Bell");
        if (!m_body.Valid() || !g_renderBackend) return;
        const auto* bell = dynamic_cast<const Game::BellBlockEntity*>(&be);
        if (!bell) return;
        const TextureHandle tex = m_textures.Get("bell/bell_body");
        if (tex == INVALID_TEXTURE) return;

        // BellModel.setupAnim.
        float xRot = 0.0f, zRot = 0.0f;
        if (bell->Shaking()) {
            const float ticks = static_cast<float>(bell->Ticks()) + partialTick;
            const float baseRot = std::sin(ticks / 3.1415927f) / (4.0f + ticks / 3.0f);
            switch (bell->ClickDirection()) {
                case 2: xRot = -baseRot; break;   // NORTH
                case 3: xRot =  baseRot; break;   // SOUTH
                case 5: zRot = -baseRot; break;   // EAST
                case 4: zRot =  baseRot; break;   // WEST
                default: break;
            }
        }
        const glm::ivec3 p = be.GetWorldPos();
        const glm::mat4 model = BEModel::PartPose(
            glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(p))),
            {8.0f, 12.0f, 8.0f}, {xRot, 0.0f, zRot});

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::Back;
        s.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(s);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * view * model);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, p);
        BEModel::Draw(m_body, EntityLighting::Current() == EntityLighting::LightSet::Nether ? 1u : 0u);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
