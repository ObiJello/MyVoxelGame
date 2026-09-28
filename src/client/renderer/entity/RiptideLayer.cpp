// File: src/client/renderer/entity/RiptideLayer.cpp
//
// MC SpinAttackEffectLayer + SpinAttackEffectModel: the swirl a riptiding
// player (LivingEntity.isAutoSpinAttack) wears — on the stick-figure players
// (MobRenderer::RenderRiptideSwirls) and on a /morph player-model body
// (RenderMorphs → DrawRiptideSwirls).
//
//   SpinAttackEffectModel.createLayer (64 × 64): box_i, i = 0, 1 —
//     texOffs(0, 0) box(-8, -16 + yOffset, -8, 16, 32, 16), yOffset =
//     -3.2 + 9.6 (i + 1), PartPose.ZERO scaled 0.75 (i + 1).
//   setupAnim: box_i.yRot = wrapDegrees(ageInTicks · -(45 + 5 (i + 1))).
//   The layer draws it on the player model's pose stack — which, spinning,
//   is laid along the look and turning (LivingEntityRenderer.setupRotations)
//   — with textures/entity/trident/trident_riptide.png, entityCutout (no
//   culling), the player's light, NO_OVERLAY.
#include "client/renderer/entity/MobRenderer.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/entity/EntityCulling.hpp"
#include "client/renderer/entity/EntityOutline.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>

namespace Render {

    namespace {

        struct SwirlMesh {
            std::unique_ptr<ModelPart> root;
            ModelPart* boxes[2] = { nullptr, nullptr };
        };

        SwirlMesh& Swirl() {
            static SwirlMesh mesh = [] {
                SwirlMesh s;
                s.root = std::make_unique<ModelPart>();
                for (int i = 0; i < 2; ++i) {
                    const float yOffset = -3.2f + 9.6f * static_cast<float>(i + 1);
                    const float scale = 0.75f * static_cast<float>(i + 1);
                    PartPose pose = PartPose::Zero();
                    pose.xScale = pose.yScale = pose.zScale = scale;
                    s.boxes[i] = s.root->AddChild(i == 0 ? "box0" : "box1", pose);
                    s.boxes[i]->cubes.push_back(CubeDefinition{ -8.0f, -16.0f + yOffset, -8.0f,
                                                                16.0f, 32.0f, 16.0f,
                                                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false });
                }
                s.root->ResetPose();
                return s;
            }();
            return mesh;
        }

        // SpinAttackEffectModel.setupAnim.
        void PoseSwirl(SwirlMesh& s, float ageInTicks) {
            s.root->ResetPose();
            for (int i = 0; i < 2; ++i) {
                const float angle = ageInTicks * static_cast<float>(-(45 + (i + 1) * 5));
                s.boxes[i]->yRot = Game::Mth::WrapDegrees(angle) * 0.017453292f;
            }
        }

    } // namespace

    glm::mat4 MobRenderer::SpinningPlayerRoot(const glm::dvec3& feet, float bodyYaw, float pitch,
                                              float ageInTicks, float scale) {
        // AvatarRenderer / LivingEntityRenderer for a riptiding player:
        // translate(feet), the SCALE, rotateY(180 - bodyRot), then the spin
        // (rotateX(-90 - xRot), rotateY(ageInTicks * -75)), scale(-1, -1, 1),
        // AvatarRenderer.scale (0.9375), translate(0, -1.501, 0) — in pixels.
        glm::mat4 m = glm::translate(glm::mat4(1.0f), Render::ToRender(feet));
        m = glm::scale(m, glm::vec3(scale));
        m = glm::rotate(m, glm::radians(180.0f - bodyYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::rotate(m, glm::radians(-90.0f - pitch), glm::vec3(1.0f, 0.0f, 0.0f));
        m = glm::rotate(m, glm::radians(ageInTicks * -75.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::scale(m, glm::vec3(-0.9375f, -0.9375f, 0.9375f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));
        m = glm::translate(m, glm::vec3(0.0f, -1.501f * 16.0f, 0.0f));
        return m;
    }

    void MobRenderer::RenderRiptideSwirls(const glm::mat4& projection, const glm::mat4& view,
                                          const glm::vec3& cameraPos, const Frustum& frustum,
                                          const std::vector<RiptidePose>& poses) {
        if (poses.empty()) return;
        std::vector<RiptideDraw> draws;
        draws.reserve(poses.size());
        for (const RiptidePose& p : poses) {
            // Culled with the body (the spin pose's 0.6 x 0.6 box, generously:
            // the swirl reaches ~1.5 blocks).
            const float reach = 3.0f * p.scale;
            if (!EntityCulling::ShouldRender(frustum, glm::vec3(p.position) - glm::vec3(0.0f, reach * 0.5f, 0.0f),
                                             reach, reach)) {
                continue;
            }
            RiptideDraw d;
            d.rootPx = SpinningPlayerRoot(p.position, p.bodyYaw, p.pitch, p.ageInTicks, p.scale);
            d.ageInTicks = p.ageInTicks;
            d.packedLight = EntityEnvironment::PackedLightAt(p.position + glm::dvec3(0.0, 0.4 * p.scale, 0.0));
            d.glowing = p.glowing && EntityOutline::Get().Collecting();
            draws.push_back(d);
        }
        DrawRiptideSwirls(projection, view, cameraPos, draws);
    }

    void MobRenderer::DrawRiptideSwirls(const glm::mat4& projection, const glm::mat4& view,
                                        const glm::vec3& cameraPos, const std::vector<RiptideDraw>& draws) {
        PROFILE_ZONE_N("RiptideRender");
        if (!m_initialized || !g_renderBackend || draws.empty()) return;
        if (m_frameCursor.Advance()) {
            m_vertCursor = 0;
            m_idxCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH) return;
        if (m_vertCursor + 4096 >= kMaxVertices || m_idxCursor + 8192 >= kMaxIndices) return;
        const size_t vertRoom = kMaxVertices - m_vertCursor;
        const size_t idxRoom  = kMaxIndices  - m_idxCursor;

        const TextureHandle texture = LoadTexture("assets/textures/entity/trident_riptide.png");
        if (texture == INVALID_TEXTURE) return;

        struct Piece { size_t first = 0, count = 0; int packedLight = 0; bool glowing = false; };
        std::vector<Piece> pieces;
        m_verts.clear();
        m_indices.clear();
        SwirlMesh& mesh = Swirl();
        for (const RiptideDraw& d : draws) {
            PoseSwirl(mesh, d.ageInTicks);
            const size_t iFirst = m_indices.size();
            mesh.root->Build(d.rootPx, 64.0f, 64.0f, m_verts, m_indices, /*culled=*/false);
            if (m_indices.size() == iFirst) continue;
            pieces.push_back({ iFirst, m_indices.size() - iFirst, d.packedLight, d.glowing });
            if (m_verts.size() + 4096 > vertRoom || m_indices.size() + 8192 > idxRoom) break;
        }
        if (m_indices.empty() || pieces.empty()) return;
        if (m_verts.size() > vertRoom || m_indices.size() > idxRoom) return;

        if (m_vertCursor > 0) {
            const auto base = static_cast<uint32_t>(m_vertCursor);
            for (uint32_t& i : m_indices) i += base;
        }
        g_renderBackend->UpdateBuffer(fb.vb, m_vertCursor * sizeof(ModelVertex),
                                      m_verts.size() * sizeof(ModelVertex), m_verts.data());
        g_renderBackend->UpdateBuffer(fb.ib, m_idxCursor * sizeof(uint32_t),
                                      m_indices.size() * sizeof(uint32_t), m_indices.data());
        const size_t firstIndexThisCall = m_idxCursor;
        m_vertCursor += m_verts.size();
        m_idxCursor  += m_indices.size();

        // RenderTypes.entityCutout: no culling, depth written, the cutout in
        // the entity shader.
        PipelineState pipeline;
        pipeline.depthTestEnabled = true;
        pipeline.depthWriteEnabled = true;
        pipeline.blendEnabled = false;
        pipeline.cullMode = CullMode::None;
        pipeline.frontFace = FrontFace::CounterClockwise;
        pipeline.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(pipeline);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
        g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane",
                                        ::Render::ChunkRenderer::PortalEntityClipPlane());
        EntityEnvironment::ApplyWorld(m_shader, glm::dvec3(cameraPos));
        g_renderBackend->SetUniformVec4(m_shader, "uColor", glm::vec4(0.0f));   // NO_OVERLAY
        g_renderBackend->BindTexture(texture, 0);

        const glm::mat4 viewProj = projection * view;
        for (const Piece& piece : pieces) {
            if (piece.glowing) {
                EntityOutline::Get().SubmitIndexed(fb.mesh, static_cast<uint32_t>(firstIndexThisCall + piece.first),
                                                   static_cast<uint32_t>(piece.count), texture, viewProj);
            }
            EntityEnvironment::SetEntityLight(m_shader, EntityEnvironment::LightColor(piece.packedLight));
            g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(piece.count),
                                         static_cast<uint32_t>(firstIndexThisCall + piece.first));
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
