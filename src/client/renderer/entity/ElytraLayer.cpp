// File: src/client/renderer/entity/ElytraLayer.cpp
//
// MC WingsLayer + ElytraModel for this engine's bodies: the stick-figure
// players (MobRenderer::RenderElytras), the skinned players (AvatarRenderer,
// RenderPlayerSkins and the inventory preview), the humanoid /morph bodies
// and mobs that HumanoidMobRenderer gives the layer, and armor stands
// (ArmorStandRenderer) — the last two through AppendElytra into the mob
// pass's own batches.
//
//   ElytraModel.createLayer (64 × 32, CubeDeformation 1.0):
//     left_wing   texOffs(22, 0) box(-10, 0, 0, 10, 20, 2)
//                 offsetAndRotation(5, 0, 0,  π/12, 0, −π/12)
//     right_wing  texOffs(22, 0).mirror() box(0, 0, 0, 10, 20, 2)
//                 offsetAndRotation(−5, 0, 0, π/12, 0,  π/12)
//   setupAnim: both wings y = crouching ? 3 : 0, the left wing's angles from
//   ElytraAnimationState (x, y, z), the right mirrored (−y, −z).
//   WingsLayer.submit: translate(0, 0, 0.125) on the parent's pose stack;
//   ELYTRA_BABY is the mesh through ElytraModel.BABY_TRANSFORMER =
//   MeshTransformer.scaling(0.5): the root pose scaled 0.5 and moved
//   24.016 · (1 − 0.5) px down.
//   The texture: textures/entity/equipment/wings/elytra.png (the elytra
//   asset's WINGS layer), or a skinned player's cape (ElytraDraw::texture —
//   WingsLayer draws the cape's sheet when a cape is worn and shown).
//   An enchanted elytra draws MC's armor glint over it (RenderTypes.
//   armorCutoutNoCullGlint: glint², GlintAlpha 0.75, added unlit —
//   TextureTransform.ARMOR_ENTITY_GLINT_TEXTURING: scale 0.16), here as a
//   second additive pass at depth EQUAL.
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

#include <chrono>
#include <cmath>
#include <memory>

namespace Render {

    namespace {

        struct ElytraMesh {
            std::unique_ptr<ModelPart> root;
            ModelPart* left  = nullptr;
            ModelPart* right = nullptr;
        };

        ElytraMesh& Elytra() {
            static ElytraMesh mesh = [] {
                ElytraMesh e;
                e.root = std::make_unique<ModelPart>();
                e.left = e.root->AddChild("left_wing",
                    PartPose::OffsetAndRotation(5.0f, 0.0f, 0.0f, 0.2617994f, 0.0f, -0.2617994f));
                e.left->cubes.push_back(CubeDefinition{ -10.0f, 0.0f, 0.0f, 10.0f, 20.0f, 2.0f,
                                                        22.0f, 0.0f, 1.0f, 1.0f, 1.0f, false });
                e.right = e.root->AddChild("right_wing",
                    PartPose::OffsetAndRotation(-5.0f, 0.0f, 0.0f, 0.2617994f, 0.0f, 0.2617994f));
                e.right->cubes.push_back(CubeDefinition{ 0.0f, 0.0f, 0.0f, 10.0f, 20.0f, 2.0f,
                                                         22.0f, 0.0f, 1.0f, 1.0f, 1.0f, true });
                e.root->ResetPose();
                return e;
            }();
            return mesh;
        }

        // ElytraModel.setupAnim.
        void PoseElytra(ElytraMesh& e, const MobRenderer::ElytraDraw& d) {
            e.root->ResetPose();
            const float y = d.crouching ? 3.0f : 0.0f;
            e.left->y     = y;
            e.left->xRot  = d.rotX;
            e.left->zRot  = d.rotZ;
            e.left->yRot  = d.rotY;
            e.right->yRot = -e.left->yRot;
            e.right->y    = e.left->y;
            e.right->xRot = e.left->xRot;
            e.right->zRot = -e.left->zRot;
        }

        // TextureTransform.setupGlintTexturing(0.16) — translation(−l0, l1)
        // · rotateZ(π/18) · scale(0.16) over the model's 0..1 UVs, with MC's
        // default glint speed (0.5) × MAX_ENCHANTMENT_GLINT_SPEED_MILLIS (8).
        glm::vec2 GlintUv(float u, float v, float l0, float l1) {
            constexpr float kScale = 0.16f;
            constexpr float kRot   = 0.17453292f;
            const float x = u * kScale, y = v * kScale;
            const float c = std::cos(kRot), s = std::sin(kRot);
            return glm::vec2(x * c - y * s - l0, x * s + y * c + l1);
        }

        // EntityModel.MODEL_Y_OFFSET in pixels (−1.501 blocks) — the 24.016
        // MeshTransformer.scaling measures its baby offset against.
        constexpr float kModelYOffsetPx = -1.501f * 16.0f;
        // ElytraModel.BABY_TRANSFORMER's factor.
        constexpr float kBabyFactor = 0.5f;

    } // namespace

    TextureHandle MobRenderer::AppendElytra(const ElytraDraw& d, std::vector<ModelVertex>& verts,
                                            std::vector<uint32_t>& idx) {
        const TextureHandle texture = d.texture != INVALID_TEXTURE
            ? d.texture : LoadTexture("assets/textures/entity/equipment/wings/elytra.png");
        if (texture == INVALID_TEXTURE) return INVALID_TEXTURE;
        ElytraMesh& mesh = Elytra();
        PoseElytra(mesh, d);
        glm::mat4 m = d.rootPx;
        if (d.rootScaledForBaby) {
            // The body's root carries the mini-adult fallback's 0.5 (a
            // uniform scale ahead of the −1.501 offset); MC's pose stack has
            // no such scale — the baby is in the mesh — so take it back out
            // before WingsLayer's translate, which is in unscaled blocks.
            m = glm::translate(m, glm::vec3(0.0f, -kModelYOffsetPx, 0.0f));
            m = glm::scale(m, glm::vec3(1.0f / kBabyFactor));
            m = glm::translate(m, glm::vec3(0.0f, kModelYOffsetPx, 0.0f));
        }
        // WingsLayer.submit: poseStack.translate(0, 0, 0.125).
        m = glm::translate(m, glm::vec3(0.0f, 0.0f, 0.125f * 16.0f));
        if (d.baby || d.rootScaledForBaby) {
            // ELYTRA_BABY's root pose: PartPose.scaled(0.5).translated(0,
            // 24.016 · (1 − 0.5), 0) — ModelPart.translateAndRotate's
            // translate, then its scale.
            m = glm::translate(m, glm::vec3(0.0f, -kModelYOffsetPx * (1.0f - kBabyFactor), 0.0f));
            m = glm::scale(m, glm::vec3(kBabyFactor));
        }
        mesh.root->Build(m, 64.0f, 32.0f, verts, idx, /*culled=*/false);
        return texture;
    }

    void MobRenderer::AppendArmorGlint(std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx,
                                       size_t vFirst, size_t vEnd, size_t iFirst, size_t iEnd,
                                       float l0, float l1) {
        // MC entity.fsh under GLINT: `color.rgb += glint.rgb²` with glint =
        // GlintAlpha · texture — GlintAlpha the glintStrength option, 0.75 by
        // default. The additive (SRC_COLOR, ONE) pass squares the fragment,
        // so the 0.75 rides the vertex colour (0xBF, as GuiGraphics' item
        // glint does).
        constexpr uint8_t kGlintAlpha = 191;
        const auto base = static_cast<uint32_t>(verts.size());
        for (size_t i = vFirst; i < vEnd; ++i) {
            ModelVertex v = verts[i];
            const glm::vec2 uv = GlintUv(v.u, v.v, l0, l1);
            v.u = uv.x;
            v.v = uv.y;
            v.r = v.g = v.b = v.a = kGlintAlpha;
            verts.push_back(v);
        }
        for (size_t i = iFirst; i < iEnd; ++i) {
            idx.push_back(base + (idx[i] - static_cast<uint32_t>(vFirst)));
        }
    }

    void MobRenderer::RenderElytras(const glm::mat4& projection, const glm::mat4& view,
                                    const glm::vec3& cameraPos, const Frustum& frustum,
                                    const std::vector<ElytraPose>& poses) {
        if (poses.empty()) return;
        std::vector<ElytraDraw> draws;
        draws.reserve(poses.size());
        for (const ElytraPose& p : poses) {
            const float width = 0.6f * p.scale, height = 1.8f * p.scale;
            if (!EntityCulling::ShouldRender(frustum, glm::vec3(p.position), width, height)) continue;

            // The stick figure's own transforms, about its feet, in its order
            // (PlayerRenderer: glide tip, death topple, body scale).
            const glm::vec3 feet = Render::ToRender(p.position);
            glm::mat4 about(1.0f);
            about = glm::scale(about, glm::vec3(p.scale));
            if (p.deathFlipDeg != 0.0f) {
                about = glm::rotate(glm::mat4(1.0f), glm::radians(p.deathFlipDeg),
                                    Game::Mth::HorizontalViewVector(p.bodyYaw)) * about;
                // (scale commutes with the rotations about the feet)
            }
            glm::mat4 glide(1.0f);
            if (p.fallFlyTicks > 0.0f) {
                const float ease = std::min(1.0f, std::max(0.0f, p.fallFlyTicks * p.fallFlyTicks / 100.0f));
                const float angle = ease * (90.0f + p.pitch);
                if (angle != 0.0f) {
                    const glm::vec3 forward = Game::Mth::HorizontalViewVector(p.bodyYaw);
                    const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward));
                    glide = glm::rotate(glm::mat4(1.0f), glm::radians(angle), axis);
                }
            }
            glm::mat4 m = glm::translate(glm::mat4(1.0f), feet) * about * glide;
            // AvatarRenderer.getRenderOffset: a crouching player sits 2/16 lower.
            if (p.crouching) m = glm::translate(m, glm::vec3(0.0f, -2.0f / 16.0f, 0.0f));
            // LivingEntityRenderer.submit's chain for the player model:
            // rotate(180 − yBodyRot), scale(−1, −1, 1), AvatarRenderer.scale
            // (0.9375), translate(0, −1.501, 0) — in pixels.
            m = glm::rotate(m, glm::radians(180.0f - p.bodyYaw), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::scale(m, glm::vec3(-0.9375f, -0.9375f, 0.9375f));
            m = glm::scale(m, glm::vec3(1.0f / 16.0f));
            m = glm::translate(m, glm::vec3(0.0f, -1.501f * 16.0f, 0.0f));

            ElytraDraw d;
            d.rootPx    = m;
            d.rotX      = p.rotX;
            d.rotY      = p.rotY;
            d.rotZ      = p.rotZ;
            d.crouching = p.crouching;
            d.glint     = p.glint;
            d.glowing   = p.glowing && EntityOutline::Get().Collecting();
            const double eye = (p.crouching ? 1.27 : 1.62) * p.scale;
            d.packedLight = EntityEnvironment::PackedLightAt(p.position + glm::dvec3(0.0, eye, 0.0));
            draws.push_back(d);
        }
        DrawElytras(projection, view, cameraPos, draws);
    }

    void MobRenderer::DrawElytras(const glm::mat4& projection, const glm::mat4& view,
                                  const glm::vec3& cameraPos, const std::vector<ElytraDraw>& draws) {
        PROFILE_ZONE_N("ElytraRender");
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

        TextureHandle glintTex = INVALID_TEXTURE;

        // The glint's scroll, once for the frame.
        const double ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count());
        const long long millis = static_cast<long long>(ms * 0.5 * 8.0);
        const float l0 = static_cast<float>(millis % 110000LL) / 110000.0f;
        const float l1 = static_cast<float>(millis % 30000LL) / 30000.0f;

        struct Piece {
            size_t first = 0, count = 0;
            bool glint = false;
            int packedLight = 0;
            bool glowing = false;
            TextureHandle texture = INVALID_TEXTURE;
        };
        std::vector<Piece> pieces;
        m_verts.clear();
        m_indices.clear();
        for (const ElytraDraw& d : draws) {
            const size_t vFirst = m_verts.size();
            const size_t iFirst = m_indices.size();
            const TextureHandle texture = AppendElytra(d, m_verts, m_indices);
            if (texture == INVALID_TEXTURE) continue;
            const size_t vEnd = m_verts.size();
            const size_t iEnd = m_indices.size();
            if (iEnd == iFirst) continue;
            pieces.push_back({ iFirst, iEnd - iFirst, false, d.packedLight, d.glowing, texture });
            if (d.glint) {
                if (glintTex == INVALID_TEXTURE) {
                    glintTex = LoadTexture("assets/textures/misc/enchanted_glint_armor.png", /*repeatWrap=*/true);
                }
                if (glintTex != INVALID_TEXTURE) {
                    const size_t gFirst = m_indices.size();
                    AppendArmorGlint(m_verts, m_indices, vFirst, vEnd, iFirst, iEnd, l0, l1);
                    pieces.push_back({ gFirst, m_indices.size() - gFirst, true, d.packedLight, false, glintTex });
                }
            }
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

        // RenderTypes.armorCutoutNoCull: no culling, depth written.
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

        const glm::mat4 viewProj = projection * view;
        for (const Piece& piece : pieces) {
            if (piece.glint) continue;
            if (piece.glowing) {
                EntityOutline::Get().SubmitIndexed(fb.mesh, static_cast<uint32_t>(firstIndexThisCall + piece.first),
                                                   static_cast<uint32_t>(piece.count), piece.texture, viewProj);
            }
            g_renderBackend->BindTexture(piece.texture, 0);
            EntityEnvironment::SetEntityLight(m_shader, EntityEnvironment::LightColor(piece.packedLight));
            g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(piece.count),
                                         static_cast<uint32_t>(firstIndexThisCall + piece.first));
        }
        // The glint over what was just drawn: RenderPipelines.GLINT — blend
        // (SRC_COLOR, ONE), depth EQUAL, no depth write — at full light.
        bool glintState = false;
        for (const Piece& piece : pieces) {
            if (!piece.glint) continue;
            if (!glintState) {
                PipelineState glint = pipeline;
                glint.depthWriteEnabled = false;
                glint.depthCompareOp = CompareOp::Equal;
                glint.blendEnabled = true;
                glint.srcBlendFactor = BlendFactor::SrcColor;
                glint.dstBlendFactor = BlendFactor::One;
                g_renderBackend->SetPipelineState(glint);
                g_renderBackend->BindTexture(glintTex, 0);
                EntityEnvironment::SetEntityLight(m_shader, glm::vec3(EntityEnvironment::kEmissive));
                glintState = true;
            }
            g_renderBackend->DrawIndexed(fb.mesh, static_cast<uint32_t>(piece.count),
                                         static_cast<uint32_t>(firstIndexThisCall + piece.first));
        }
        if (glintState) g_renderBackend->SetPipelineState(pipeline);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
