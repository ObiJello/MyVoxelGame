// File: src/client/renderer/entity/TwilightCreatureRender.cpp
//
// Render::TwilightCreatureRender (ModMobRender.hpp) — the Twilight Forest's
// animals and biome-spawner creatures: TF's own model classes' setupAnim over
// the generated meshes (or the vanilla mesh a TF renderer reuses), and each
// renderer's texture, scale and layers. Sheets are the mod's own (CC BY-NC-SA,
// assets/ATTRIBUTION.md, copied by tools/copy_twilight_assets.py).
//
// Render-state slots used beyond their vanilla meaning (documented once here):
//   flap / flapSpeed  — TF BirdRenderState.flap / flapSpeed (every TF bird);
//   isOnGround        — FlyingBird's landed flag (tiny bird, raven);
//   isAngry           — HostileWolf/WolfRenderState.isAngry (target != null);
//   tailAngle         — WolfRenderState.tailAngle (HostileWolf.getTailAngle);
//   isHoldingItem     — YetiRenderState.isHoldingEntity (isVehicle).
#include "client/renderer/entity/ModMobRender.hpp"
#include "client/renderer/entity/ModModelHelpers.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/mobs/TwilightCreatures.hpp"
#include "common/entity/mobs/TwilightMobs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace Render::TwilightCreatureRender {

    namespace {

        // Static downcast by type id, the MobRenderer::MobAs contract: the
        // factory (Game::MakeTwilightMob) builds exactly this class for the
        // listed ids.
        template <typename T, typename... Ids>
        const T* MobAs(const Game::Mob& mob, Game::EntityTypeId type, Ids... ids) {
            if (!((type == ids) || ...)) return nullptr;
            assert(dynamic_cast<const T*>(&mob) != nullptr);
            return static_cast<const T*>(&mob);
        }

        // MC client/model/BabyModelTransform.apply over a built model's rest
        // poses (the same transform EntityModels.cpp applies to the
        // hand-written babies): each ROOT child translated, then scaled.
        void ApplyBabyTransform(ModelPart& root, bool scaleHead,
                                float babyYHeadOffset, float babyZHeadOffset,
                                float babyHeadScale, float babyBodyScale,
                                float bodyYOffset,
                                std::initializer_list<std::string_view> headParts) {
            const float headScale = scaleHead ? 1.5f / babyHeadScale : 1.0f;
            const float bodyScale = 1.0f / babyBodyScale;
            for (auto& child : root.children) {
                const bool isHead =
                    std::find(headParts.begin(), headParts.end(),
                              std::string_view(child->name)) != headParts.end();
                const float s = isHead ? headScale : bodyScale;
                PartPose& p = child->pose;
                p.y += isHead ? babyYHeadOffset : bodyYOffset;
                p.z += isHead ? babyZHeadOffset : 0.0f;
                p.x *= s; p.y *= s; p.z *= s;
                p.xScale *= s; p.yScale *= s; p.zScale *= s;
            }
            root.ResetPose();
        }

        // MC AnimationUtils.bobModelPart.
        void BobModelPart(ModelPart* part, float ageInTicks, float scale) {
            part->zRot += scale * (std::cos(ageInTicks * 0.09f) * 0.05f + 0.05f);
            part->xRot += scale * std::sin(ageInTicks * 0.067f) * 0.05f;
        }

        // ── TF bird models ─────────────────────────────────────────────────

        // TF TinyBirdModel.setupAnim. `isOnGround` carries the landed flag.
        class TinyBirdGenModel : public GeneratedModel {
        public:
            TinyBirdGenModel() : GeneratedModel("tiny_bird") {
                m_head = m_root.Find("head");
                m_rightFoot = m_root.Find("right_foot");
                m_leftFoot = m_root.Find("left_foot");
                m_rightWing = m_root.Find("right_wing");
                m_leftWing = m_root.Find("left_wing");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                const float f = (std::sin(state.flap) + 1.0f) * state.flapSpeed;
                if (m_head) {
                    m_head->xRot = state.xRot * kModDegToRad;
                    m_head->yRot = state.yRot * kModDegToRad;
                }
                const float pos = state.walkAnimationPos * 0.6662f;
                if (m_rightFoot) m_rightFoot->xRot = std::cos(pos) * 1.4f * state.walkAnimationSpeed;
                if (m_leftFoot)  m_leftFoot->xRot  = std::cos(pos + kModPi) * 1.4f * state.walkAnimationSpeed;
                if (m_rightWing) m_rightWing->zRot = f;
                if (m_leftWing)  m_leftWing->zRot  = -f;
                if (!state.isOnGround) {
                    if (m_rightFoot) m_rightFoot->y -= 0.5f;
                    if (m_leftFoot)  m_leftFoot->y  -= 0.5f;
                }
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_rightFoot = nullptr;
            ModelPart* m_leftFoot = nullptr;
            ModelPart* m_rightWing = nullptr;
            ModelPart* m_leftWing = nullptr;
        };

        // TF RavenModel.setupAnim: the tiny bird's shape plus the head cock
        // (zRot -15° when the look turns past 5°) and the legs dropping a
        // pixel on the ground (y 21 landed, 20 flying).
        class RavenGenModel : public GeneratedModel {
        public:
            RavenGenModel() : GeneratedModel("raven") {
                m_head = m_root.Find("head");
                m_rightLeg = m_root.Find("right_leg");
                m_leftLeg = m_root.Find("left_leg");
                m_rightWing = m_root.Find("right_wing");
                m_leftWing = m_root.Find("left_wing");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (m_head) {
                    m_head->xRot = state.xRot * kModDegToRad;
                    m_head->yRot = state.yRot * kModDegToRad;
                    m_head->zRot = state.yRot > 5.0f ? -0.2617994f : 0.0f;
                }
                const float pos = state.walkAnimationPos * 0.6662f;
                if (m_rightLeg) m_rightLeg->xRot = std::cos(pos) * 1.4f * state.walkAnimationSpeed;
                if (m_leftLeg)  m_leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * state.walkAnimationSpeed;
                const float flapAngle = (std::sin(state.flap) + 1.0f) * state.flapSpeed;
                if (m_rightWing) m_rightWing->zRot = flapAngle;
                if (m_leftWing)  m_leftWing->zRot  = -flapAngle;
                const float legY = state.isOnGround ? 21.0f : 20.0f;
                if (m_rightLeg) m_rightLeg->y = legY;
                if (m_leftLeg)  m_leftLeg->y  = legY;
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_rightLeg = nullptr;
            ModelPart* m_leftLeg = nullptr;
            ModelPart* m_rightWing = nullptr;
            ModelPart* m_leftWing = nullptr;
        };

        // TF PenguinModel.setupAnim (the feet swing at the raw walk position,
        // no 0.6662, amplitude 0.7).
        class PenguinGenModel : public GeneratedModel {
        public:
            PenguinGenModel() : GeneratedModel("penguin") {
                m_head = m_root.Find("head");
                m_rightFoot = m_root.Find("right_foot");
                m_leftFoot = m_root.Find("left_foot");
                m_rightWing = m_root.Find("right_wing");
                m_leftWing = m_root.Find("left_wing");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                const float f = (std::sin(state.flap) + 1.0f) * state.flapSpeed;
                if (m_head) {
                    m_head->xRot = state.xRot * kModDegToRad;
                    m_head->yRot = state.yRot * kModDegToRad;
                }
                const float pos = state.walkAnimationPos;
                if (m_rightFoot) m_rightFoot->xRot = std::cos(pos) * 0.7f * state.walkAnimationSpeed;
                if (m_leftFoot)  m_leftFoot->xRot  = std::cos(pos + kModPi) * 0.7f * state.walkAnimationSpeed;
                if (m_rightWing) m_rightWing->zRot = f;
                if (m_leftWing)  m_leftWing->zRot  = -f;
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_rightFoot = nullptr;
            ModelPart* m_leftFoot = nullptr;
            ModelPart* m_rightWing = nullptr;
            ModelPart* m_leftWing = nullptr;
        };

        // ── TF SquirrelModel.setupAnim ─────────────────────────────────────

        class SquirrelGenModel : public GeneratedModel {
        public:
            SquirrelGenModel() : GeneratedModel("squirrel") {
                m_head = m_root.Find("head");
                m_rightHind = m_root.Find("right_hind_leg");
                m_leftHind = m_root.Find("left_hind_leg");
                m_rightFront = m_root.Find("right_front_leg");
                m_leftFront = m_root.Find("left_front_leg");
                m_tail = m_root.Find("tail");
                m_tail1 = m_root.Find("tail_1");
                m_tail2 = m_root.Find("tail_2");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (m_head) {
                    m_head->xRot = state.xRot * kModDegToRad;
                    m_head->yRot = state.yRot * kModDegToRad;
                }
                const float pos = state.walkAnimationPos * 0.6662f;
                const float amt = 1.4f * state.walkAnimationSpeed;
                if (m_rightHind)  m_rightHind->xRot  = std::cos(pos) * amt;
                if (m_leftHind)   m_leftHind->xRot   = std::cos(pos + kModPi) * amt;
                if (m_rightFront) m_rightFront->xRot = std::cos(pos + kModPi) * amt;
                if (m_leftFront)  m_leftFront->xRot  = std::cos(pos) * amt;
                if (!m_tail || !m_tail1 || !m_tail2) return;
                const float age = state.ageInTicks;
                if (state.walkAnimationSpeed > 0.2f) {
                    const float wiggle = std::min(state.walkAnimationSpeed, 0.6f);
                    m_tail->xRot  = 0.2f + (std::cos(age * 0.6662f) - kModPi / 3.0f) * wiggle;
                    m_tail1->xRot = std::cos(age * 0.7774f) * 1.2f * wiggle;
                    m_tail2->xRot = std::cos(age * 0.8886f + kModPi / 2.0f) * 1.4f * wiggle;
                } else {
                    m_tail->xRot  = 0.2f + std::cos(age * 0.3335f) * 0.15f;
                    m_tail1->xRot = 0.1f + std::cos(age * 0.4445f) * 0.20f;
                    m_tail2->xRot = 0.1f + std::cos(age * 0.5555f) * 0.25f;
                }
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_rightHind = nullptr;
            ModelPart* m_leftHind = nullptr;
            ModelPart* m_rightFront = nullptr;
            ModelPart* m_leftFront = nullptr;
            ModelPart* m_tail = nullptr;
            ModelPart* m_tail1 = nullptr;
            ModelPart* m_tail2 = nullptr;
        };

        // ── TF HostileWolfModel.setupAnim, over the vanilla wolf mesh ──────
        // (TFModelLayers.HOSTILE_WOLF = AdultWolfModel.createBodyLayer(NONE),
        // 64x32 — MC's own WOLF row, the generated "wolf" mesh).

        class HostileWolfGenModel : public GeneratedModel {
        public:
            HostileWolfGenModel() : GeneratedModel("wolf") {
                m_head = m_root.Find("head");
                m_body = m_root.Find("body");
                m_upperBody = m_root.Find("upper_body");
                m_rightHind = m_root.Find("right_hind_leg");
                m_leftHind = m_root.Find("left_hind_leg");
                m_rightFront = m_root.Find("right_front_leg");
                m_leftFront = m_root.Find("left_front_leg");
                m_tail = m_root.Find("tail");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_head || !m_body || !m_upperBody || !m_rightHind || !m_leftHind ||
                    !m_rightFront || !m_leftFront || !m_tail) {
                    return;
                }
                const float pos = state.walkAnimationPos * 0.6662f;
                const float spd = state.walkAnimationSpeed;
                m_tail->yRot = state.isAngry ? 0.0f : std::cos(pos) * 1.4f * spd;
                SetPos(m_body, 0.0f, 14.0f, 2.0f);
                m_body->xRot = kModPi / 2.0f;
                SetPos(m_upperBody, -1.0f, 14.0f, -3.0f);
                m_upperBody->xRot = m_body->xRot;
                SetPos(m_tail, -1.0f, 12.0f, 8.0f);
                SetPos(m_rightHind, -2.5f, 16.0f, 7.0f);
                SetPos(m_leftHind, 0.5f, 16.0f, 7.0f);
                SetPos(m_rightFront, -2.5f, 16.0f, -4.0f);
                SetPos(m_leftFront, 0.5f, 16.0f, -4.0f);
                m_rightHind->xRot  = std::cos(pos) * 1.4f * spd;
                m_leftHind->xRot   = std::cos(pos + kModPi) * 1.4f * spd;
                m_rightFront->xRot = std::cos(pos + kModPi) * 1.4f * spd;
                m_leftFront->xRot  = std::cos(pos) * 1.4f * spd;
                m_head->xRot = state.xRot * kModDegToRad;
                m_head->yRot = state.yRot * kModDegToRad;
                m_tail->xRot = state.tailAngle;
            }

        private:
            static void SetPos(ModelPart* p, float x, float y, float z) { p->x = x; p->y = y; p->z = z; }

            ModelPart* m_head = nullptr;
            ModelPart* m_body = nullptr;
            ModelPart* m_upperBody = nullptr;
            ModelPart* m_rightHind = nullptr;
            ModelPart* m_leftHind = nullptr;
            ModelPart* m_rightFront = nullptr;
            ModelPart* m_leftFront = nullptr;
            ModelPart* m_tail = nullptr;
        };

        // ── TF YetiModel.setupAnim ─────────────────────────────────────────
        // YetiRenderer never fills YetiRenderState.isAngry (only
        // isHoldingEntity), so in the mod the calm eyes always show and the
        // zombie-arms branch never runs; `isAngry` stays false here too.

        class YetiGenModel : public GeneratedModel {
        public:
            YetiGenModel() : GeneratedModel("yeti") {
                m_head = m_root.Find("head");
                m_hat = m_root.Find("hat");
                m_rightEye = m_root.Find("right_eye");
                m_leftEye = m_root.Find("left_eye");
                m_angryRightEye = m_root.Find("angry_right_eye");
                m_angryLeftEye = m_root.Find("angry_left_eye");
                m_rightArm = m_root.Find("right_arm");
                m_leftArm = m_root.Find("left_arm");
                m_rightLeg = m_root.Find("right_leg");
                m_leftLeg = m_root.Find("left_leg");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_head || !m_hat || !m_rightArm || !m_leftArm || !m_rightLeg || !m_leftLeg) return;
                const bool angry = state.isAngry;
                const bool holding = state.isHoldingItem;
                if (m_rightEye) m_rightEye->visible = !angry;
                if (m_leftEye) m_leftEye->visible = !angry;
                if (m_angryRightEye) m_angryRightEye->visible = angry;
                if (m_angryLeftEye) m_angryLeftEye->visible = angry;

                m_head->yRot = state.yRot * kModDegToRad;
                m_head->xRot = state.xRot * kModDegToRad;
                m_hat->yRot = m_head->yRot;
                m_hat->xRot = m_head->xRot;
                const float pos = state.walkAnimationPos * 0.6662f;
                const float spd = state.walkAnimationSpeed;
                m_rightArm->xRot = std::cos(pos + kModPi) * 2.0f * spd * 0.5f;
                m_leftArm->xRot  = std::cos(pos) * 2.0f * spd * 0.5f;
                m_rightArm->zRot = 0.0f;
                m_leftArm->zRot = 0.0f;
                m_rightLeg->xRot = std::cos(pos) * 1.4f * spd;
                m_leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * spd;
                m_rightLeg->yRot = 0.0f;
                m_leftLeg->yRot = 0.0f;
                if (holding) {
                    // "arms up!"
                    m_rightArm->xRot += kModPi;
                    m_leftArm->xRot += kModPi;
                }
                m_rightArm->yRot = 0.0f;
                m_leftArm->yRot = 0.0f;
                // AnimationUtils.bobArms.
                BobModelPart(m_rightArm, state.ageInTicks, 1.0f);
                BobModelPart(m_leftArm, state.ageInTicks, -1.0f);
                if (angry) {
                    const float f6 = std::sin(state.attackTime * kModPi);
                    const float f7 = std::sin((1.0f - (1.0f - state.attackTime) *
                                                          (1.0f - state.attackTime)) * kModPi);
                    m_rightArm->zRot = 0.0f;
                    m_leftArm->zRot = 0.0f;
                    m_rightArm->yRot = -(0.1f - f6 * 0.6f);
                    m_leftArm->yRot = 0.1f - f6 * 0.6f;
                    m_rightArm->xRot = -(kModPi / 2.0f);
                    m_leftArm->xRot = -(kModPi / 2.0f);
                    m_rightArm->xRot -= f6 * 1.2f - f7 * 0.4f;
                    m_leftArm->xRot -= f6 * 1.2f - f7 * 0.4f;
                    if (holding) {
                        m_rightArm->xRot -= kModPi / 2.0f;
                        m_leftArm->xRot -= kModPi / 2.0f;
                    }
                }
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_hat = nullptr;
            ModelPart* m_rightEye = nullptr;
            ModelPart* m_leftEye = nullptr;
            ModelPart* m_angryRightEye = nullptr;
            ModelPart* m_angryLeftEye = nullptr;
            ModelPart* m_rightArm = nullptr;
            ModelPart* m_leftArm = nullptr;
            ModelPart* m_rightLeg = nullptr;
            ModelPart* m_leftLeg = nullptr;
        };

        // ── TF MosquitoSwarmModel ──────────────────────────────────────────
        //
        // The mod builds the mesh from a RandomSource at layer-bake time (its
        // own TODO notes swarms were meant to differ per mob): a core cube,
        // six groups (one per Direction, the group cube 11 px out along it),
        // each with sixteen 1-px bugs scattered ±16 px and turned about Y by
        // i * 22.5°. The generator cannot evaluate a random mesh, so it is
        // built here with the mod's exact draw order off a fixed seed —
        // stable across sessions, like one baked RandomSource would be.
        class MosquitoSwarmModel : public EntityModel {
        public:
            MosquitoSwarmModel() {
                m_texWidth = 64.0f;
                m_texHeight = 64.0f;
                Game::JavaRandom rand(0x4D6F7371);   // "Mosq"
                const auto box = [](ModelPart* part, float u, float v, float x, float y, float z) {
                    CubeDefinition c{};
                    c.originX = x; c.originY = y; c.originZ = z;
                    c.sizeX = c.sizeY = c.sizeZ = 1.0f;
                    c.texOffsX = u; c.texOffsY = v;
                    part->cubes.push_back(c);
                };
                m_core = m_root.AddChild("core", PartPose::Offset(0.0f, -4.0f, 0.0f));
                {
                    const float u = static_cast<float>(rand.NextInt(28));
                    const float v = static_cast<float>(rand.NextInt(28));
                    box(m_core, u, v, -0.5f, 2.0f, -0.5f);
                }
                // Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST.
                static constexpr int kSteps[6][3] = {
                    { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 },
                };
                constexpr int kBugs = 16;
                for (int dir = 0; dir < 6; ++dir) {
                    ModelPart* group = m_core->AddChild("group_" + std::to_string(dir + 1),
                                                        PartPose::Offset(-0.5f, -2.0f, -0.5f));
                    const float gu = static_cast<float>(rand.NextInt(28));
                    const float gv = static_cast<float>(rand.NextInt(28));
                    box(group, gu, gv, kSteps[dir][0] * 11.0f, kSteps[dir][1] * 11.0f,
                        kSteps[dir][2] * 11.0f);
                    m_groups[dir] = group;
                    for (int i = 0; i < kBugs; ++i) {
                        const float yRot = ((static_cast<float>(i) * (360.0f / kBugs)) * kModPi) / 180.0f;
                        const float bx = (rand.NextFloat() - rand.NextFloat()) * 16.0f;
                        const float by = (rand.NextFloat() - rand.NextFloat()) * 16.0f;
                        const float bz = (rand.NextFloat() - rand.NextFloat()) * 16.0f;
                        const float u = static_cast<float>(rand.NextInt(28));
                        const float v = static_cast<float>(rand.NextInt(28));
                        // The Vector3f rotated about X is the zero vector:
                        // every bug sits at its group's origin, turned by yRot.
                        ModelPart* bug = group->AddChild(
                            "bug_" + std::to_string(dir * kBugs + i),
                            PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f, 0.0f, yRot, 0.0f));
                        box(bug, u, v, bx, by, bz);
                    }
                }
                m_root.ResetPose();
            }

            void SetupAnim(const EntityRenderState& state) override {
                // MosquitoSwarmModel.setupAnim, verbatim.
                m_root.ResetPose();
                const float a = state.ageInTicks;
                m_core->yRot = a / 5.0f;
                m_core->xRot = std::sin(a / 5.0f) / 4.0f;
                m_core->zRot = std::cos(a / 5.0f) / 4.0f;
                ModelPart** g = m_groups;
                g[0]->yRot = a / 2.0f;
                g[0]->xRot = std::sin(a / 6.0f) / 2.0f;
                g[0]->zRot = std::cos(a / 5.0f) / 4.0f;
                g[1]->yRot = std::sin(a / 2.0f) / 3.0f;
                g[1]->xRot = a / 5.0f;
                g[1]->zRot = std::cos(a / 5.0f) / 4.0f;
                g[2]->yRot = std::sin(a / 7.0f) / 3.0f;
                g[2]->xRot = std::cos(a / 4.0f) / 2.0f;
                g[2]->zRot = a / 5.0f;
                g[3]->xRot = a / 2.0f;
                g[3]->zRot = std::sin(a / 6.0f) / 2.0f;
                g[3]->yRot = std::sin(a / 5.0f) / 4.0f;
                g[4]->zRot = a / 2.0f;
                g[4]->yRot = std::cos(a / 5.0f) / 4.0f;
                g[4]->xRot = std::cos(a / 5.0f) / 4.0f;
                g[5]->zRot = std::cos(a / 7.0f) / 3.0f;
                g[5]->xRot = std::cos(a / 4.0f) / 2.0f;
                g[5]->yRot = a / 5.0f;
            }

        private:
            ModelPart* m_core = nullptr;
            ModelPart* m_groups[6] = {};
        };

        // ── Layer models (built on first use, posed per mob by AppendMob) ──

        std::unique_ptr<EntityModel> s_sheepWool;       // MC SHEEP_WOOL (SheepFurModel)
        std::unique_ptr<EntityModel> s_sheepWoolBaby;   // BIGHORN_SHEEP_BABY_WOOL

    } // namespace

    std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type) {
        // Each mod mesh under its mod setupAnim, with the nearest vanilla
        // model as the fallback should its mesh be missing.
        switch (type) {
            case Game::EntityTypeId::Deer:
                if (FindGenModel("deer")) {
                    return std::make_unique<ModQuadrupedModel>(
                        "deer", std::initializer_list<const char*>{ "left_antler", "right_antler" },
                        false);
                }
                return std::make_unique<CowModel>();
            case Game::EntityTypeId::Boar:
                if (FindGenModel("boar")) {
                    return std::make_unique<ModQuadrupedModel>(
                        "boar", std::initializer_list<const char*>{}, false);
                }
                return std::make_unique<PigModel>();
            case Game::EntityTypeId::BighornSheep:
                if (FindGenModel("bighorn_sheep")) {
                    return std::make_unique<ModQuadrupedModel>(
                        "bighorn_sheep", std::initializer_list<const char*>{ "left_horn", "right_horn" },
                        true);
                }
                return std::make_unique<SheepModel>(false);
            case Game::EntityTypeId::TinyBird:
                if (FindGenModel("tiny_bird")) return std::make_unique<TinyBirdGenModel>();
                if (FindGenModel("parrot")) return std::make_unique<GeneratedModel>("parrot");
                return nullptr;
            case Game::EntityTypeId::Raven:
                if (FindGenModel("raven")) return std::make_unique<RavenGenModel>();
                if (FindGenModel("parrot")) return std::make_unique<GeneratedModel>("parrot");
                return nullptr;
            case Game::EntityTypeId::Penguin:
                if (FindGenModel("penguin")) return std::make_unique<PenguinGenModel>();
                return nullptr;
            case Game::EntityTypeId::Kobold:
                if (FindGenModel("kobold")) return std::make_unique<ModHumanoidModel>("kobold", true);
                if (FindGenModel("husk")) return std::make_unique<GeneratedModel>("husk");
                return nullptr;
            case Game::EntityTypeId::Redcap:
                if (FindGenModel("redcap")) return std::make_unique<ModHumanoidModel>("redcap", false);
                if (FindGenModel("husk")) return std::make_unique<GeneratedModel>("husk");
                return nullptr;
            // SquirrelRenderer: SquirrelModel.
            case Game::EntityTypeId::Squirrel:
                if (FindGenModel("squirrel")) return std::make_unique<SquirrelGenModel>();
                return nullptr;
            // BunnyRenderer: BunnyModel extends QuadrupedModel and keeps its
            // setupAnim (head look + leg swing).
            case Game::EntityTypeId::DwarfRabbit:
                if (FindGenModel("dwarf_rabbit")) {
                    return std::make_unique<ModQuadrupedModel>(
                        "dwarf_rabbit", std::initializer_list<const char*>{}, false);
                }
                return nullptr;
            // HostileWolfRenderer / MistWolfRenderer / WinterWolfRenderer.
            case Game::EntityTypeId::HostileWolf:
            case Game::EntityTypeId::MistWolf:
            case Game::EntityTypeId::WinterWolf:
                if (FindGenModel("wolf")) return std::make_unique<HostileWolfGenModel>();
                return nullptr;
            // TFSpiderRenderer extends SpiderRenderer: MC's SpiderModel.
            case Game::EntityTypeId::KingSpider:
                return std::make_unique<SpiderModel>();
            case Game::EntityTypeId::MosquitoSwarm:
                return std::make_unique<MosquitoSwarmModel>();
            // SkeletonDruidModel extends SkeletonModel: its mesh under MC's
            // SkeletonModel.setupAnim, the stray's compiled program (same
            // class, same part names).
            case Game::EntityTypeId::SkeletonDruid:
                if (FindGenModel("skeleton_druid")) {
                    return std::make_unique<GeneratedModel>("skeleton_druid", "stray");
                }
                return std::make_unique<SkeletonModel>();
            case Game::EntityTypeId::Yeti:
                if (FindGenModel("yeti")) return std::make_unique<YetiGenModel>();
                return nullptr;
            default:
                return nullptr;
        }
    }

    std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type) {
        // The TF renderers' babyModel rows (ClientRegistrationEvents'
        // *_BABY layer definitions), each the adult mesh through its model
        // class's BABY_TRANSFORMER.
        std::unique_ptr<EntityModel> model;
        switch (type) {
            case Game::EntityTypeId::BighornSheep:
                // BighornModel.BABY_TRANSFORMER = BabyModelTransform(false,
                // 8, 4, {"head"}) — the sheep's.
                model = CreateModel(type);
                if (model && FindGenModel("bighorn_sheep")) {
                    ApplyBabyTransform(model->Root(), false, 8.0f, 4.0f, 2.0f, 2.0f, 24.0f, {"head"});
                    return model;
                }
                return nullptr;
            case Game::EntityTypeId::DwarfRabbit:
                // BunnyModel.BABY_TRANSFORMER = (true, 8.5, 0, {"head"}).
                model = CreateModel(type);
                if (!model) return nullptr;
                ApplyBabyTransform(model->Root(), true, 8.5f, 0.0f, 2.0f, 2.0f, 24.0f, {"head"});
                return model;
            case Game::EntityTypeId::Penguin:
                // PenguinModel.BABY_TRANSFORMER = BabyModelTransform(Set.of())
                // — defaults (false, 5, 2), no head parts.
                model = CreateModel(type);
                if (!model) return nullptr;
                ApplyBabyTransform(model->Root(), false, 5.0f, 2.0f, 2.0f, 2.0f, 24.0f, {});
                return model;
            case Game::EntityTypeId::SkeletonDruid:
                // SKELETON_DRUID_BABY = HumanoidModel.BABY_TRANSFORMER
                // (true, 16, 0, {"head"}).
                model = CreateModel(type);
                if (!model || !FindGenModel("skeleton_druid")) return nullptr;
                ApplyBabyTransform(model->Root(), true, 16.0f, 0.0f, 2.0f, 2.0f, 24.0f, {"head"});
                return model;
            default:
                return nullptr;
        }
    }

    std::string_view TexturePath(Game::EntityTypeId type) {
        switch (type) {
            case Game::EntityTypeId::Deer:
                return "assets/textures/entity/twilightforest/deer.png";
            case Game::EntityTypeId::Boar:
                return "assets/textures/entity/twilightforest/boar.png";
            case Game::EntityTypeId::BighornSheep:
                return "assets/textures/entity/twilightforest/bighorn_sheep.png";
            // The per-instance variant picks the sheet (InstanceTexturePath);
            // these are the synced defaults (TinyBird RED, DwarfRabbit BROWN).
            case Game::EntityTypeId::TinyBird:
                return "assets/textures/entity/twilightforest/tiny_bird_red.png";
            case Game::EntityTypeId::DwarfRabbit:
                return "assets/textures/entity/twilightforest/dwarf_rabbit_brown.png";
            case Game::EntityTypeId::Kobold:
                return "assets/textures/entity/twilightforest/kobold.png";
            case Game::EntityTypeId::Redcap:
                return "assets/textures/entity/twilightforest/redcap.png";
            case Game::EntityTypeId::Squirrel:
                return "assets/textures/entity/twilightforest/squirrel.png";
            case Game::EntityTypeId::Raven:
                return "assets/textures/entity/twilightforest/raven.png";
            case Game::EntityTypeId::Penguin:
                return "assets/textures/entity/twilightforest/penguin.png";
            // HostileWolf.getTexture: the PALE variant's wild / angry sheet.
            case Game::EntityTypeId::HostileWolf:
                return "assets/textures/entity/wolf/wolf.png";
            case Game::EntityTypeId::MistWolf:
                return "assets/textures/entity/twilightforest/mist_wolf.png";
            case Game::EntityTypeId::WinterWolf:
                return "assets/textures/entity/twilightforest/winter_wolf.png";
            case Game::EntityTypeId::KingSpider:
                return "assets/textures/entity/twilightforest/king_spider.png";
            case Game::EntityTypeId::MosquitoSwarm:
                return "assets/textures/entity/twilightforest/mosquito_swarm.png";
            case Game::EntityTypeId::SkeletonDruid:
                return "assets/textures/entity/twilightforest/skeleton_druid.png";
            case Game::EntityTypeId::Yeti:
                return "assets/textures/entity/twilightforest/yeti.png";
            default:
                return {};
        }
    }

    bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                            float partialTick, EntityRenderState& state) {
        // MC Sheep's two head scales, from the same event-10 countdown.
        if (const auto* grazer = MobAs<Game::GrazingAnimal>(
                mob, type, Game::EntityTypeId::BighornSheep)) {
            state.headEatPositionScale = grazer->GetHeadEatPositionScale(partialTick);
            state.headEatAngleScale = grazer->GetHeadEatAngleScale(partialTick);
            return true;
        }
        // TF BirdRenderer / TinyBirdRenderer.extractRenderState: flap and
        // flapSpeed; the flying birds' landed flag.
        if (const auto* flier = MobAs<Game::FlyingBird>(
                mob, type, Game::EntityTypeId::TinyBird, Game::EntityTypeId::Raven)) {
            state.flap = flier->GetFlap(partialTick);
            state.flapSpeed = flier->GetFlapIntensity(partialTick);
            state.isOnGround = flier->IsBirdLanded();
            return true;
        }
        if (const auto* penguin = MobAs<Game::TFBird>(mob, type, Game::EntityTypeId::Penguin)) {
            state.flap = penguin->GetFlap(partialTick);
            state.flapSpeed = penguin->GetFlapIntensity(partialTick);
            return true;
        }
        // HostileWolfRenderer.extractRenderState: isAngry = target != null,
        // tailAngle. MistWolfRenderer / WinterWolfRenderer.scale: 1.9.
        if (const auto* wolf = MobAs<Game::HostileWolf>(
                mob, type, Game::EntityTypeId::HostileWolf, Game::EntityTypeId::MistWolf,
                Game::EntityTypeId::WinterWolf)) {
            state.isAngry = wolf->HasTargetClient();
            state.tailAngle = wolf->GetTailAngle();
            if (type != Game::EntityTypeId::HostileWolf) state.modelScale = glm::vec3(1.9f);
            return true;
        }
        // TFSpiderRenderer.scale: the king spider's 1.9.
        if (type == Game::EntityTypeId::KingSpider) {
            state.modelScale = glm::vec3(1.9f);
            return true;
        }
        // SkeletonDruidRenderer: HumanoidMobRenderer's ITEM arm pose for the
        // held hoe/stick; SkeletonModel's aggressive melee raise (not a bow).
        if (type == Game::EntityTypeId::SkeletonDruid) {
            state.rightArmPose = ArmPose::Item;
            state.isHoldingBow = false;
            state.hasMainHandItem = true;
            return true;
        }
        // YetiRenderer.extractRenderState: isHoldingEntity = isVehicle.
        if (const auto* yeti = MobAs<Game::Yeti>(mob, type, Game::EntityTypeId::Yeti)) {
            state.isHoldingItem = yeti->IsHoldingEntityClient() || mob.IsVehicle();
            state.isAngry = false;   // never set by YetiRenderer (see YetiGenModel)
            return true;
        }
        switch (type) {
            case Game::EntityTypeId::Deer:
            case Game::EntityTypeId::Boar:
            case Game::EntityTypeId::Kobold:
            case Game::EntityTypeId::Redcap:
            case Game::EntityTypeId::Squirrel:
            case Game::EntityTypeId::DwarfRabbit:
            case Game::EntityTypeId::MosquitoSwarm:
                return true;
            default:
                return false;
        }
    }

    std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,
                                         const EntityRenderState& state) {
        switch (type) {
            // TinyBirdRenderer: the variant's texture (blue, brown, gold, red).
            case Game::EntityTypeId::TinyBird: {
                static constexpr const char* kSheets[Game::TinyBird::VariantCount] = {
                    "assets/textures/entity/twilightforest/tiny_bird.png",
                    "assets/textures/entity/twilightforest/tiny_bird_brown.png",
                    "assets/textures/entity/twilightforest/tiny_bird_gold.png",
                    "assets/textures/entity/twilightforest/tiny_bird_red.png",
                };
                const uint8_t v = mob.GetVariantByte();
                return v < Game::TinyBird::VariantCount ? kSheets[v] : std::string_view{};
            }
            // BunnyRenderer: the variant's texture (brown, dutch, white).
            case Game::EntityTypeId::DwarfRabbit: {
                static constexpr const char* kSheets[Game::DwarfRabbit::VariantCount] = {
                    "assets/textures/entity/twilightforest/dwarf_rabbit_brown.png",
                    "assets/textures/entity/twilightforest/dwarf_rabbit_dutch.png",
                    "assets/textures/entity/twilightforest/dwarf_rabbit_white.png",
                };
                const uint8_t v = mob.GetVariantByte();
                return v < Game::DwarfRabbit::VariantCount ? kSheets[v] : std::string_view{};
            }
            // HostileWolf.getTexture: the angry sheet while aggressive.
            case Game::EntityTypeId::HostileWolf:
                return state.isAggressive ? "assets/textures/entity/wolf/wolf_angry.png"
                                          : std::string_view{};
            default:
                return {};
        }
    }

    int Layers(const Game::Mob& mob, Game::EntityTypeId type,
               const EntityRenderState& state, ModLayer* out) {
        switch (type) {
            // BighornWoolLayer: MC's SHEEP_WOOL mesh (BIGHORN_SHEEP_BABY_WOOL
            // for a lamb — the fur through the bighorn's baby transform) on
            // sheep_wool.png, tinted by the fleece colour; skipped when
            // sheared.
            case Game::EntityTypeId::BighornSheep: {
                const auto* bighorn = MobAs<Game::Bighorn>(mob, type, Game::EntityTypeId::BighornSheep);
                if (!bighorn || bighorn->IsSheared()) return 0;
                std::unique_ptr<EntityModel>& slot = state.isBaby ? s_sheepWoolBaby : s_sheepWool;
                if (!slot) {
                    auto fur = std::make_unique<SheepModel>(true);
                    if (state.isBaby) fur->BecomeBaby();
                    slot = std::move(fur);
                }
                const SheepWoolColor tint = kSheepWoolColors[bighorn->GetColor() & 0x0F];
                out[0].model = slot.get();
                out[0].texture = "assets/textures/entity/sheep/sheep_wool.png";
                out[0].r = tint.r;
                out[0].g = tint.g;
                out[0].b = tint.b;
                return 1;
            }
            // SpiderRenderer's SpiderEyesLayer (TFSpiderRenderer inherits it):
            // the body again on spider_eyes.png, NO_OVERLAY.
            case Game::EntityTypeId::KingSpider:
                out[0].texture = "assets/textures/entity/spider_eyes.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // RenderTypes.eyes
                return 1;
            default:
                return 0;
        }
    }

    bool BodyTranslucent(Game::EntityTypeId type) {
        // MistWolfModel: RenderTypes::entityTranslucent. (Its per-mob tint,
        // MistWolfRenderer.getModelTint — alpha 0.6..1 and grey 0.25..1 by
        // brightness — needs a body-tint hook the render interface lacks.)
        return type == Game::EntityTypeId::MistWolf;
    }

    float FlipDegrees(Game::EntityTypeId type) {
        switch (type) {
            // SpiderRenderer.getFlipDegrees: 180.
            case Game::EntityTypeId::KingSpider:    return 180.0f;
            // MosquitoSwarmRenderer.getFlipDegrees: 0 — the swarm does not
            // topple. A tiny positive angle stands in for 0 (0 means "not
            // mine" in this interface); it is invisible.
            case Game::EntityTypeId::MosquitoSwarm: return 0.001f;
            default:                                return 0.0f;
        }
    }

    const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,
                         const EntityRenderState& state) {
        (void)state;
        // SkeletonDruid.populateDefaultEquipmentSlots: the golden hoe, a
        // stick for a baby.
        if (type == Game::EntityTypeId::SkeletonDruid) {
            return mob.IsBaby() ? "stick" : "golden_hoe";
        }
        return nullptr;
    }

} // namespace Render::TwilightCreatureRender
