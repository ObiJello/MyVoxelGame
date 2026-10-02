// File: src/client/renderer/entity/PlayerSkinRenderer.cpp
//
// MC AvatarRenderer for the players whose look is a Minecraft skin
// (Game::PlayerAppearance — docs/player-appearance.md): MobRenderer's
// RenderPlayerSkins / CapturePlayerSkinForGui.
//
//   LivingEntityRenderer.submit, as AvatarRenderer runs it:
//     translate(position + getRenderOffset)      crouch: scale · −2/16 down
//     [SLEEPING] translate(−bed.step · (eyeHeight − 0.1))   eyeHeight 0.2
//     scale(state.scale)
//     setupRotations:
//       rotateY(180 − bodyRot)                   (not while sleeping)
//       death: rotateZ(topple) | riptide: rotateX(−90 − xRot), rotateY(age · −75)
//         | sleeping: rotateY(bed angle), rotateZ(90), rotateY(270)
//       AvatarRenderer: gliding: rotateX(fallFlyingScale · (−90 − xRot))
//         (not riptiding), rotateY(flyingYRot) | swimming: rotateX(lerp(
//         swimAmount, 0, inWater ? −90 − xRot : −90)), translate(0, −1, 0.3)
//         while visually swimming
//     scale(−1, −1, 1), scale(0.9375), translate(0, −1.501, 0)
//   then PlayerModel (entityTranslucent: blended, no culling, the hurt
//   overlay) and the layers: HumanoidArmorLayer, PlayerItemInHandLayer,
//   CapeLayer, CustomHeadLayer, WingsLayer, SpinAttackEffectLayer — all
//   NO_OVERLAY, and none for a spectator (shouldRenderLayers).
#include "client/renderer/entity/MobRenderer.hpp"

#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/RenderOrigin.hpp"
#include "client/renderer/entity/EntityCulling.hpp"
#include "client/renderer/entity/EntityLighting.hpp"
#include "client/renderer/entity/EntityOutline.hpp"
#include "client/renderer/environment/EntityEnvironment.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/entity/ElytraAnimationState.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/PlayerAppearance.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/lighting/LightCoords.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace Render {

    namespace {

        // AppendSkinnedPlayer's batch kinds.
        constexpr int kPartBody  = 0;   // the skin: hurt overlay, blended, hidden when invisible
        constexpr int kPartLayer = 1;   // armour, held items, head item: NO_OVERLAY, kept when invisible
        constexpr int kPartCape  = 2;   // the cape (entitySolid): NO_OVERLAY, gone when invisible

        // LivingEntityRenderer.sleepDirectionToRotation.
        float SleepDirectionToRotation(Game::Direction d) {
            switch (d) {
                case Game::Direction::South: return 90.0f;
                case Game::Direction::West:  return 0.0f;
                case Game::Direction::North: return 270.0f;
                case Game::Direction::East:  return 180.0f;
                default:                     return 0.0f;
            }
        }

        bool Sleeping(const MobRenderer::SkinnedPlayerPose& pose) {
            // A body in a bed has the SLEEPING pose until it dies (DYING).
            return pose.bedFacing >= 0 && pose.deathTime <= 0;
        }

        // MC getLightProbePosition: the eye, by pose (Avatar POSES).
        double EyeHeight(const MobRenderer::SkinnedPlayerPose& pose) {
            double eye = 1.62;
            if (pose.crouching && !pose.passenger) eye = 1.27;
            if (pose.fallFlying || pose.visuallySwimming || pose.autoSpinAttack) eye = 0.4;
            if (Sleeping(pose)) eye = 0.2;
            return eye * static_cast<double>(pose.scale);
        }

        // The batch light: burning bodies take full block light (MC
        // getBlockLightLevel), everything else the cell's.
        glm::vec3 BatchLight(bool fullBlock, int packedLight) {
            namespace LC = Game::Lighting::LightCoords;
            return EntityEnvironment::LightColor(fullBlock ? LC::WithBlock(packedLight, 15) : packedLight);
        }

        // EntityModel.MODEL_Y_OFFSET, in blocks (the thousandth lifts the
        // feet clear of the ground plane).
        constexpr float kModelYOffset = -1.501f;

        // MC LivingEntityRenderer.submit's forceTransparent tint, 0x26 alpha.
        constexpr uint32_t kTranslucentAlpha = 38;

    } // namespace

    PlayerModel& MobRenderer::PlayerSkinModel(bool slim) {
        std::unique_ptr<PlayerModel>& slot = m_skinModels[slim ? 1 : 0];
        if (!slot) slot = std::make_unique<PlayerModel>(slim);
        return *slot;
    }

    EntityRenderState MobRenderer::SkinnedPlayerState(const SkinnedPlayerPose& pose, float partialTick) {
        EntityRenderState state;
        state.yRot = Game::Mth::WrapDegrees(pose.headYaw - pose.bodyYaw);
        state.xRot = pose.pitch;
        state.walkAnimationPos   = pose.walkPos;
        state.walkAnimationSpeed = pose.walkSpeed;
        state.ageInTicks = pose.ageTicks;
        state.scale = pose.scale;
        state.isCrouching = pose.crouching && !pose.passenger;
        state.isPassenger = pose.passenger;
        state.attackTime  = pose.attackTime;
        state.rightArmPose = static_cast<ArmPose>(std::min<uint8_t>(pose.rightArmPose, 10));
        state.leftArmPose  = static_cast<ArmPose>(std::min<uint8_t>(pose.leftArmPose, 10));
        state.isUsingItem  = pose.usingItem;
        state.useItemHand  = pose.useItemHand;
        state.ticksUsingItem = pose.ticksUsingItem;
        state.maxCrossbowChargeDuration = pose.maxCrossbowCharge;
        // A spear swings STAB (its SWING_ANIMATION) — the SPEAR arm pose says
        // the main hand holds one.
        state.swingAnimType = pose.rightArmPose == 10 ? 2.0f : 1.0f;
        state.rightArmItem = pose.equipment[static_cast<int>(Game::EquipmentSlot::MAINHAND)].itemId;
        state.leftArmItem  = pose.equipment[static_cast<int>(Game::EquipmentSlot::OFFHAND)].itemId;
        state.ticksSinceKineticHitFeedback = pose.ticksSinceKineticHitFeedback;
        state.isAutoSpinAttack = pose.autoSpinAttack && pose.deathTime <= 0;
        state.deathFlipDeg = DeathFlipDegrees(pose.deathTime, partialTick, 90.0f);
        state.isFallFlying = pose.fallFlying;
        state.speedValue = std::max(1.0f, pose.speedValue);
        state.swimAmount = pose.swimAmount;
        state.isInWater = pose.inWater;
        state.isSwimming = pose.visuallySwimming;
        state.isSprinting = false;
        state.entityId = static_cast<float>(pose.playerId);
        state.yHeadRotAbs = pose.headYaw;
        state.yBodyRotAbs = pose.bodyYaw;
        return state;
    }

    glm::mat4 MobRenderer::SkinnedPlayerRoot(const SkinnedPlayerPose& pose, const glm::vec3& renderFeet,
                                             float partialTick) {
        const bool sleeping = Sleeping(pose);
        glm::vec3 feet = renderFeet;
        // AvatarRenderer.getRenderOffset: a crouching player sits lower.
        if (pose.crouching && !pose.passenger) feet.y += pose.scale * -2.0f / 16.0f;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), feet);

        // LivingEntityRenderer.submit: the sleeper's head onto the pillow
        // (SLEEPING_DIMENSIONS' eye height 0.2, minus 0.1).
        const auto bed = static_cast<Game::Direction>(std::max(0, pose.bedFacing));
        if (sleeping) {
            constexpr float kHeadOffset = 0.2f - 0.1f;
            m = glm::translate(m, glm::vec3(-static_cast<float>(Game::StepX(bed)) * kHeadOffset, 0.0f,
                                            -static_cast<float>(Game::StepZ(bed)) * kHeadOffset));
        }
        m = glm::scale(m, glm::vec3(pose.scale));

        // LivingEntityRenderer.setupRotations.
        const glm::vec3 X(1.0f, 0.0f, 0.0f), Y(0.0f, 1.0f, 0.0f), Z(0.0f, 0.0f, 1.0f);
        if (!sleeping) m = glm::rotate(m, glm::radians(180.0f - pose.bodyYaw), Y);
        const bool spinning = pose.autoSpinAttack && pose.deathTime <= 0;
        if (pose.deathTime > 0) {
            m = glm::rotate(m, glm::radians(DeathFlipDegrees(pose.deathTime, partialTick, 90.0f)), Z);
        } else if (spinning) {
            m = glm::rotate(m, glm::radians(-90.0f - pose.pitch), X);
            m = glm::rotate(m, glm::radians(pose.ageTicks * -75.0f), Y);
        } else if (sleeping) {
            m = glm::rotate(m, glm::radians(SleepDirectionToRotation(bed)), Y);
            m = glm::rotate(m, glm::radians(90.0f), Z);
            m = glm::rotate(m, glm::radians(270.0f), Y);
        }
        // AvatarRenderer.setupRotations' own branches.
        if (pose.fallFlying) {
            const float s = std::clamp(pose.fallFlyTicks * pose.fallFlyTicks / 100.0f, 0.0f, 1.0f);
            if (!spinning) m = glm::rotate(m, glm::radians(s * (-90.0f - pose.pitch)), X);
            if (pose.applyFlyingYRot) m = glm::rotate(m, pose.flyingYRot, Y);
        } else if (pose.swimAmount > 0.0f) {
            const float target = pose.inWater ? -90.0f - pose.pitch : -90.0f;
            m = glm::rotate(m, glm::radians(pose.swimAmount * target), X);
            if (pose.visuallySwimming) m = glm::translate(m, glm::vec3(0.0f, -1.0f, 0.3f));
        }
        m = glm::scale(m, glm::vec3(-1.0f, -1.0f, 1.0f));
        m = glm::scale(m, glm::vec3(0.9375f));                     // AvatarRenderer.scale
        m = glm::translate(m, glm::vec3(0.0f, kModelYOffset, 0.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));                 // ModelPart pixels
        return m;
    }

    glm::mat4 MobRenderer::AppendSkinnedPlayer(const SkinnedPlayerPose& pose, const glm::vec3& renderFeet,
                                               float partialTick, const glm::vec3& cameraPos,
                                               const std::function<void(TextureHandle, size_t, bool, int)>& emit) {
        PlayerModel& model = PlayerSkinModel(pose.slim);
        model.SetParts(pose.modelParts, pose.spectator);
        const EntityRenderState state = SkinnedPlayerState(pose, partialTick);
        model.SetupAnim(state);
        const glm::mat4 root = SkinnedPlayerRoot(pose, renderFeet, partialTick);

        // The body (PlayerModel in the skin).
        const size_t bodyFirst = m_indices.size();
        model.Root().Build(root, model.TexWidth(), model.TexHeight(), m_verts, m_indices, false);
        if (m_indices.size() > bodyFirst) emit(pose.skin, bodyFirst, false, kPartBody);

        // AvatarRenderer.shouldRenderLayers: none for a spectator.
        if (pose.spectator) return root;

        // HumanoidArmorLayer (the player's armour mesh set is the humanoid
        // one: PlayerModel.createArmorMeshSet only adds empty sleeve and pants
        // parts), on the body's full pose stack.
        AppendHumanoidArmor(ArmorFamily::Humanoid, /*babyMesh=*/false, pose.equipment, model, state,
                            pose.position, pose.bodyYaw, cameraPos,
                            [&](TextureHandle tex, size_t first, bool cull) {
                                if (tex != INVALID_TEXTURE && m_indices.size() > first) emit(tex, first, cull, kPartLayer);
                            },
                            &root);

        // PlayerItemInHandLayer: the main hand in the right arm, the off hand
        // in the left (a player is right-handed); the use clock on the arm in
        // use. (The type only picks a grip: the humanoid's translateToHand.)
        {
            ItemUseState rightUse, leftUse;
            ArmUseStates(state, rightUse, leftUse);
            ItemUseState& inUse = pose.useItemHand == 1 ? leftUse : rightUse;
            inUse.usingItem = pose.usingItem;
            inUse.useTicks  = pose.usingItem ? pose.ticksUsingItem : 0.0f;
            AppendHeldItems(model, Game::EntityTypeId::Zombie, root,
                            pose.equipment[static_cast<int>(Game::EquipmentSlot::MAINHAND)],
                            pose.equipment[static_cast<int>(Game::EquipmentSlot::OFFHAND)],
                            /*babyGrip=*/false, rightUse, leftUse,
                            [&](TextureHandle tex, size_t first, bool cull) {
                                if (tex != INVALID_TEXTURE && m_indices.size() > first) emit(tex, first, cull, kPartLayer);
                            });
        }

        // CapeLayer: not on an invisible body, not under an elytra (the
        // WINGS layer takes the cape's texture instead); nudged out over a
        // chestplate (its HUMANOID layer).
        const Game::ItemStack& chest = pose.equipment[static_cast<int>(Game::EquipmentSlot::CHEST)];
        const bool wearsElytra = (pose.elytraFlags & Game::kElytraWorn) != 0;
        if (!pose.invisible && pose.cape != INVALID_TEXTURE &&
            (pose.modelParts & Game::ModelPartBits::Cape) && !wearsElytra) {
            if (!m_capeModel) m_capeModel = std::make_unique<PlayerCapeModel>();
            m_capeModel->SetParts(Game::ModelPartBits::All, false);
            m_capeModel->SetCape(pose.capeFlap, pose.capeLean, pose.capeLean2);
            m_capeModel->SetupAnim(state);
            glm::mat4 capeRoot = root;
            if (!chest.IsEmpty() && !EquipmentAssetOf(chest).empty()) {
                // poseStack.translate(0, −0.053125, 0.06875), in blocks; the
                // root is in pixels.
                capeRoot = glm::translate(capeRoot, glm::vec3(0.0f, -0.053125f, 0.06875f) * 16.0f);
            }
            const size_t capeFirst = m_indices.size();
            m_capeModel->Root().Build(capeRoot, m_capeModel->TexWidth(), m_capeModel->TexHeight(),
                                      m_verts, m_indices, /*culled=*/true);
            if (m_indices.size() > capeFirst) emit(pose.cape, capeFirst, true, kPartCape);
        }

        // CustomHeadLayer: a block, item or mob head in the head slot (the
        // armour layer drew any helmet).
        AppendCustomHead(model, root, pose.equipment[static_cast<int>(Game::EquipmentSlot::HEAD)],
                         HeadTransforms{}, state.walkAnimationPos,
                         [&](TextureHandle tex, size_t first, bool cull) {
                             if (tex != INVALID_TEXTURE && m_indices.size() > first) emit(tex, first, cull, kPartLayer);
                         });
        return root;
    }

    void MobRenderer::RenderPlayerSkins(const glm::mat4& projection, const glm::mat4& view,
                                        const glm::vec3& cameraPos, const Frustum& frustum,
                                        const std::vector<SkinnedPlayerPose>& poses, float partialTick) {
        PROFILE_ZONE_N("PlayerSkinRender");
        if (!m_initialized || !g_renderBackend || poses.empty()) return;

        if (m_frameCursor.Advance()) {
            m_vertCursor = 0;
            m_idxCursor  = 0;
        }
        FrameBuffers& fb = m_frames[m_frameCursor.parity];
        if (fb.mesh == INVALID_MESH) return;
        if (m_vertCursor + 4096 >= kMaxVertices || m_idxCursor + 8192 >= kMaxIndices) return;
        const size_t vertRoom = kMaxVertices - m_vertCursor;
        const size_t idxRoom  = kMaxIndices  - m_idxCursor;

        struct Batch {
            TextureHandle texture = INVALID_TEXTURE;
            glm::vec4 overlay{0.0f};
            size_t first = 0, count = 0;
            bool cull = false;
            bool blend = false;
            bool hidden = false;
            bool outline = false;
            bool fullBlock = false;
            int  packedLight = 0;
        };
        std::vector<Batch> batches;
        std::vector<ElytraDraw> elytras;
        std::vector<RiptideDraw> swirls;
        m_verts.clear();
        m_indices.clear();

        const float maxDistSq = kMaxRenderDistance * kMaxRenderDistance;
        const bool collectingOutline = EntityOutline::Get().Collecting();

        for (const SkinnedPlayerPose& pose : poses) {
            if (pose.skin == INVALID_TEXTURE) continue;
            // MC EntityRenderer.shouldRender for the standing 0.6 x 1.8 box
            // (a crouching / swimming box sits inside it).
            const float width  = 0.6f * pose.scale;
            const float height = 1.8f * pose.scale;
            const glm::vec3 delta(pose.position.x - cameraPos.x, pose.position.y - cameraPos.y,
                                  pose.position.z - cameraPos.z);
            const float distSq = glm::dot(delta, delta);
            if (distSq > maxDistSq) continue;
            if (EntityCulling::g_crossingFilter) {
                const glm::vec3 half(width * 0.5f, 0.0f, width * 0.5f);
                const glm::vec3 lo = glm::vec3(pose.position) - half;
                const glm::vec3 hi = glm::vec3(pose.position) + half + glm::vec3(0.0f, height, 0.0f);
                if (!EntityCulling::PassesCrossingFilter(lo, hi)) continue;
            }
            if (!EntityCulling::ShouldRenderAtSqrDistance(distSq, width, height)) continue;
            if (!EntityCulling::ShouldRender(frustum, glm::vec3(pose.position), width, height)) continue;
            ++EntityCulling::g_renderedThisFrame;

            const int packedLight = EntityEnvironment::PackedLightAt(
                pose.position + glm::dvec3(0.0, EyeHeight(pose), 0.0));
            const bool glowing = pose.glowing && collectingOutline;
            // MC LivingEntityRenderer.submit: an INVISIBLE body is not drawn —
            // unless the viewer is a spectator (forceTransparent) — but its
            // layers are; a spectator's own body is INVISIBLE too, drawn to a
            // spectator viewer as the translucent head.
            const bool bodyTranslucent = pose.spectator || (pose.invisible && m_viewerSeesInvisible);
            const bool bodyHidden = pose.invisible && !bodyTranslucent;
            glm::vec4 overlay(0.0f);
            if (pose.hurtTime > 0 || pose.deathTime > 0) {
                overlay = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f - 178.0f / 255.0f);   // OverlayTexture red row
            }

            const glm::mat4 root = AppendSkinnedPlayer(
                pose, Render::ToRender(pose.position), partialTick, cameraPos,
                [&](TextureHandle tex, size_t first, bool cull, int part) {
                    Batch b;
                    b.texture = tex;
                    b.first = first;
                    b.count = m_indices.size() - first;
                    b.cull = cull;
                    b.packedLight = packedLight;
                    b.fullBlock = pose.onFire;
                    b.outline = glowing;
                    if (part == kPartBody) {
                        b.overlay = overlay;
                        b.blend = true;   // entityTranslucent
                        b.hidden = bodyHidden;
                        if (bodyTranslucent) {
                            // The forceTransparent tint on every vertex the
                            // body's indices reach (one contiguous build).
                            uint32_t lo = UINT32_MAX, hi = 0;
                            for (size_t i = first; i < m_indices.size(); ++i) {
                                lo = std::min(lo, m_indices[i]);
                                hi = std::max(hi, m_indices[i]);
                            }
                            for (uint32_t v = lo; v <= hi && v < m_verts.size(); ++v) {
                                m_verts[v].a = static_cast<uint8_t>((m_verts[v].a * kTranslucentAlpha) / 255u);
                            }
                        }
                    }
                    batches.push_back(b);
                });

            // WingsLayer (a layer: kept on an invisible body; not for a
            // spectator, nor in a bed).
            if ((pose.elytraFlags & Game::kElytraWorn) && !pose.spectator && !Sleeping(pose)) {
                ElytraDraw d;
                d.rootPx    = root;
                d.rotX      = pose.elytraRotX;
                d.rotY      = pose.elytraRotY;
                d.rotZ      = pose.elytraRotZ;
                d.crouching = pose.crouching && !pose.passenger;
                d.glint     = (pose.elytraFlags & Game::kElytraGlint) != 0;
                d.packedLight = packedLight;
                d.glowing   = glowing;
                if (pose.cape != INVALID_TEXTURE && (pose.modelParts & Game::ModelPartBits::Cape)) {
                    d.texture = pose.cape;
                }
                elytras.push_back(d);
            }
            // SpinAttackEffectLayer.
            if (pose.autoSpinAttack && pose.deathTime <= 0 && !pose.spectator) {
                RiptideDraw d;
                d.rootPx = root;
                d.ageInTicks = pose.ageTicks;
                d.packedLight = EntityEnvironment::PackedLightAt(pose.position + glm::dvec3(0.0, 0.4 * pose.scale, 0.0));
                d.glowing = glowing;
                swirls.push_back(d);
            }
            if (m_verts.size() + 4096 > vertRoom || m_indices.size() + 8192 > idxRoom) break;
        }

        if (!m_indices.empty() && m_verts.size() <= vertRoom && m_indices.size() <= idxRoom) {
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

            PipelineState pipeline;
            pipeline.depthTestEnabled = true;
            pipeline.depthWriteEnabled = true;
            pipeline.blendEnabled = false;
            pipeline.srcBlendFactor = BlendFactor::SrcAlpha;
            pipeline.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
            pipeline.cullMode = CullMode::None;
            pipeline.frontFace = FrontFace::CounterClockwise;
            pipeline.primitiveType = PrimitiveType::Triangles;
            g_renderBackend->SetPipelineState(pipeline);
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", projection * view);
            g_renderBackend->SetUniformVec4(m_shader, "uEntityClipPlane",
                                            ::Render::ChunkRenderer::PortalEntityClipPlane());
            EntityEnvironment::ApplyWorld(m_shader, glm::dvec3(cameraPos));

            const glm::mat4 viewProj = projection * view;
            // Opaque first, then the blended skins (their translucent texels
            // over what is behind them).
            for (int pass = 0; pass < 2; ++pass) {
                for (const Batch& batch : batches) {
                    if (batch.blend != (pass == 1)) continue;
                    const auto first = static_cast<uint32_t>(firstIndexThisCall + batch.first);
                    const auto count = static_cast<uint32_t>(batch.count);
                    if (batch.outline) {
                        EntityOutline::Get().SubmitIndexed(fb.mesh, first, count, batch.texture, viewProj);
                    }
                    if (batch.hidden || count == 0) continue;
                    const CullMode cull = batch.cull ? CullMode::Back : CullMode::None;
                    if (pipeline.cullMode != cull || pipeline.blendEnabled != batch.blend) {
                        pipeline.cullMode = cull;
                        pipeline.blendEnabled = batch.blend;
                        g_renderBackend->SetPipelineState(pipeline);
                    }
                    g_renderBackend->BindTexture(batch.texture, 0);
                    g_renderBackend->SetUniformVec4(m_shader, "uColor", batch.overlay);
                    EntityEnvironment::SetEntityLight(m_shader, BatchLight(batch.fullBlock, batch.packedLight));
                    g_renderBackend->DrawIndexed(fb.mesh, count, first);
                }
            }
            g_renderBackend->UnbindMesh();
            pipeline.blendEnabled = false;
            pipeline.cullMode = CullMode::Back;
            g_renderBackend->SetPipelineState(pipeline);
        }

        if (!elytras.empty()) DrawElytras(projection, view, cameraPos, elytras);
        if (!swirls.empty()) DrawRiptideSwirls(projection, view, cameraPos, swirls);
    }

    bool MobRenderer::CapturePlayerSkinForGui(const SkinnedPlayerPose& pose,
                                              const glm::vec3& light0, const glm::vec3& light1,
                                              std::vector<GuiEntityBatch>& out) {
        out.clear();
        if (!m_initialized || pose.skin == INVALID_TEXTURE) return false;
        // Lighting.Entry.ENTITY_IN_UI for as long as the geometry is built.
        EntityLighting::LightOverride& ui = EntityLighting::UiLightOverride();
        const EntityLighting::LightOverride saved = ui;
        ui.active = true;
        ui.light0 = light0;
        ui.light1 = light1;

        m_verts.clear();
        m_indices.clear();
        struct Range { TextureHandle texture; size_t first, count; bool blend; };
        std::vector<Range> ranges;
        const glm::mat4 root = AppendSkinnedPlayer(
            pose, glm::vec3(0.0f), 1.0f, glm::vec3(0.0f),
            [&](TextureHandle tex, size_t first, bool, int part) {
                ranges.push_back({ tex, first, m_indices.size() - first, part == kPartBody });
            });
        (void)root;
        ui = saved;

        for (const Range& r : ranges) {
            if (r.count == 0 || r.texture == INVALID_TEXTURE) continue;
            GuiEntityBatch batch;
            batch.texture = r.texture;
            batch.blend = r.blend;
            batch.triangles.reserve(r.count);
            for (size_t i = r.first; i < r.first + r.count; ++i) {
                batch.triangles.push_back(m_verts[m_indices[i]]);
            }
            out.push_back(std::move(batch));
        }
        m_verts.clear();
        m_indices.clear();
        return !out.empty();
    }

} // namespace Render
