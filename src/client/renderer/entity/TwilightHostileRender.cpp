// File: src/client/renderer/entity/TwilightHostileRender.cpp
//
// Render::TwilightHostileRender (ModMobRender.hpp) — the Twilight Forest's
// landmark hostiles: TF's own model classes' setupAnim over the generated
// meshes (or the vanilla mesh a TF renderer reuses), and each renderer's
// texture, scale and layers.
//
// Per-mob render inputs ride EntityRenderState fields this module owns for
// its types (the header's per-mob fields are otherwise unused by them):
//   PinchBeetle        isRidden            PinchBeetleRenderState.isHoldingVictim
//   HelmetCrab         tailAngle           HelmetCrabRenderState.helmetRot (deg)
//                      headRollAngle       the death roll magnitude (MC's f,
//                                          signed in the model by entityId)
//   Troll              isHoldingItem       TrollRenderState.isHoldingRock
//   UpperGoblinKnight  attackTicksRemaining  spearTimer
//                      hasChest            hasArmor
//                      isHoldingItem       hasShield
//                      isScared            isShieldDisabled
//   LowerGoblinKnight  hasChest            hasArmor
//                      isRidden            hasUpperGoblin
#include "client/renderer/entity/ModMobRender.hpp"
#include "client/renderer/entity/ModModelHelpers.hpp"

#include "common/entity/mobs/TwilightHostiles.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>

namespace Render::TwilightHostileRender {

    namespace {

        // Static downcast by type id, the MobRenderer::MobAs contract: the
        // factory (Game::MakeTwilightHostile) builds exactly this class for
        // the listed ids.
        template <typename T, typename... Ids>
        const T* MobAs(const Game::Mob& mob, Game::EntityTypeId type, Ids... ids) {
            if (!((type == ids) || ...)) return nullptr;
            assert(dynamic_cast<const T*>(&mob) != nullptr);
            return static_cast<const T*>(&mob);
        }

        constexpr float kHalfPi = kModPi * 0.5f;

        // MC AnimationUtils.bobArms / bobModelPart.
        void BobArms(ModelPart* right, ModelPart* left, float ageInTicks) {
            if (!right || !left) return;
            const float z = std::cos(ageInTicks * 0.09f) * 0.05f + 0.05f;
            const float x = std::sin(ageInTicks * 0.067f) * 0.05f;
            right->zRot += z;
            left->zRot  -= z;
            right->xRot += x;
            left->xRot  -= x;
        }

        // The humanoid parts of a TF HumanoidModel mesh, and MC
        // HumanoidModel.setupAnim's walk (head look, arm/leg swing, idle
        // bob). The attack swing (setupAttackAnimation) is not posed — the
        // pass-one ModHumanoidModel's limit.
        struct HumanoidParts {
            ModelPart* head = nullptr;
            ModelPart* hat = nullptr;
            ModelPart* body = nullptr;
            ModelPart* rightArm = nullptr;
            ModelPart* leftArm = nullptr;
            ModelPart* rightLeg = nullptr;
            ModelPart* leftLeg = nullptr;

            void Resolve(ModelPart& root) {
                head = root.Find("head");
                hat = root.Find("hat");
                body = root.Find("body");
                rightArm = root.Find("right_arm");
                leftArm = root.Find("left_arm");
                rightLeg = root.Find("right_leg");
                leftLeg = root.Find("left_leg");
            }
            bool Complete() const { return head && rightArm && leftArm && rightLeg && leftLeg; }

            void HumanoidWalk(const EntityRenderState& s) const {
                if (!Complete()) return;
                head->xRot = s.xRot * kModDegToRad;
                head->yRot = s.yRot * kModDegToRad;
                const float pos = s.walkAnimationPos * 0.6662f;
                const float spd = s.walkAnimationSpeed;
                rightArm->xRot = std::cos(pos + kModPi) * 2.0f * spd * 0.5f;
                leftArm->xRot  = std::cos(pos) * 2.0f * spd * 0.5f;
                rightArm->zRot = leftArm->zRot = 0.0f;
                rightLeg->xRot = std::cos(pos) * 1.4f * spd;
                leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * spd;
                rightLeg->yRot = leftLeg->yRot = 0.0f;
                if (s.isPassenger) {
                    // HumanoidModel's riding pose.
                    rightArm->xRot += -kModPi / 5.0f;
                    leftArm->xRot  += -kModPi / 5.0f;
                    rightLeg->xRot = -1.4137167f;
                    rightLeg->yRot = kModPi / 10.0f;
                    rightLeg->zRot = 0.07853982f;
                    leftLeg->xRot  = -1.4137167f;
                    leftLeg->yRot  = -kModPi / 10.0f;
                    leftLeg->zRot  = -0.07853982f;
                }
                BobArms(rightArm, leftArm, s.ageInTicks);
            }
        };

        // ── Wraith ─────────────────────────────────────────────────────────
        // TF WraithModel.setupAnim: HumanoidModel's walk, then the zombie-
        // style reaching arms (the attack lunge in var8/var9) and the bob.
        // WraithRenderer tints the model 0.6 alpha (getModelTint), which the
        // engine can only apply to a layer: the body instance is built
        // HIDDEN and the visible instance is drawn as a blended layer.
        class WraithModel : public GeneratedModel {
        public:
            explicit WraithModel(bool hidden) : GeneratedModel("wraith"), m_hidden(hidden) {
                m_parts.Resolve(m_root);
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                m_root.visible = !m_hidden;
                m_parts.HumanoidWalk(s);
                ModelPart* ra = m_parts.rightArm;
                ModelPart* la = m_parts.leftArm;
                if (!ra || !la) return;
                const float var8 = std::sin(s.attackTime * kModPi);
                const float var9 = std::sin((1.0f - (1.0f - s.attackTime) * (1.0f - s.attackTime)) * kModPi);
                ra->zRot = la->zRot = 0.0f;
                ra->yRot = -(0.1f - var8 * 0.6f);
                la->yRot = 0.1f - var8 * 0.6f;
                ra->xRot = la->xRot = -kHalfPi;
                ra->xRot -= var8 * 1.2f - var9 * 0.4f;
                la->xRot -= var8 * 1.2f - var9 * 0.4f;
                BobArms(ra, la, s.ageInTicks);
            }

        private:
            bool m_hidden;
            HumanoidParts m_parts;
        };

        // ── Beetles ────────────────────────────────────────────────────────
        // TF FireBeetleModel / SlimeBeetleModel / PinchBeetleModel.setupAnim:
        // the head look and the six-leg scuttle (the pinch beetle's front and
        // rear leg splay mirrored), the slime beetle's tail wiggle, the
        // pinch beetle's jaws open while holding a victim.
        enum class BeetleKind { Fire, Slime, SlimeTail, Pinch };

        class BeetleModel : public GeneratedModel {
        public:
            BeetleModel(std::string_view slug, BeetleKind kind) : GeneratedModel(slug), m_kind(kind) {
                m_head = m_root.Find("head");
                static const char* kLegs[6] = { "left_leg_1", "right_leg_1", "left_leg_2",
                                                "right_leg_2", "left_leg_3", "right_leg_3" };
                for (int i = 0; i < 6; ++i) m_legs[i] = m_root.Find(kLegs[i]);
                m_tailBottom = m_root.Find("tail_bottom");
                m_tailTop = m_root.Find("tail_top");
                m_slimeCenter = m_root.Find("slime_center");
                m_slime = m_root.Find("slime");
                m_rightPincer = m_root.Find("right_pincher");
                m_leftPincer = m_root.Find("left_pincher");
            }

            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                if (m_kind == BeetleKind::Slime && m_slime) {
                    // SlimeBeetleModel(root, false): the slime cube is the
                    // translucent tail layer's, hidden on the body.
                    m_slime->visible = false;
                }
                if (m_kind == BeetleKind::SlimeTail) {
                    // SlimeBeetleModel(root, true).renderToBuffer draws only
                    // the tail_bottom subtree.
                    for (auto& child : m_root.children) child->visible = child.get() == m_tailBottom;
                }
                if (m_kind == BeetleKind::Pinch && m_rightPincer && m_leftPincer) {
                    if (s.isRidden) {   // open jaws
                        m_rightPincer->yRot = -170.0f * kModDegToRad;
                        m_leftPincer->yRot = 20.0f * kModDegToRad;
                    } else {            // close jaws
                        m_rightPincer->yRot = 135.0f * kModDegToRad;
                        m_leftPincer->yRot = 45.0f * kModDegToRad;
                    }
                }
                if (m_head) {
                    m_head->yRot = s.yRot * kModDegToRad;
                    m_head->xRot = s.xRot * kModDegToRad;
                }
                ModelPart* l1 = m_legs[0]; ModelPart* r1 = m_legs[1];
                ModelPart* l2 = m_legs[2]; ModelPart* r2 = m_legs[3];
                ModelPart* l3 = m_legs[4]; ModelPart* r3 = m_legs[5];
                if (l1 && r1 && l2 && r2 && l3 && r3) {
                    const float legZ = kModPi / 11.0f;
                    l1->zRot = legZ;          r1->zRot = -legZ;
                    l2->zRot = legZ * 0.74f;  r2->zRot = -legZ * 0.74f;
                    l3->zRot = legZ;          r3->zRot = -legZ;
                    constexpr float var10 = 0.3926991f;
                    const float front = m_kind == BeetleKind::Pinch ? -1.0f : 1.0f;
                    l1->yRot = front * var10 * 2.0f;   r1->yRot = -front * var10 * 2.0f;
                    l2->yRot = var10;                  r2->yRot = -var10;
                    l3->yRot = -front * var10 * 2.0f;  r3->yRot = front * var10 * 2.0f;

                    const float p = s.walkAnimationPos;
                    const float a = s.walkAnimationSpeed;
                    const float var11 = -(std::cos(p * 0.6662f * 2.0f) * 0.4f) * a;
                    const float var12 = -(std::cos(p * 0.6662f * 2.0f + kModPi) * 0.4f) * a;
                    const float var14 = -(std::cos(p * 0.6662f * 2.0f + kModPi * 1.5f) * 0.4f) * a;
                    const float var15 = std::abs(std::sin(p * 0.6662f) * 0.4f) * a;
                    const float var16 = std::abs(std::sin(p * 0.6662f + kModPi) * 0.4f) * a;
                    const float var18 = std::abs(std::sin(p * 0.6662f + kModPi * 1.5f) * 0.4f) * a;
                    l1->yRot += var11; r1->yRot -= var11;
                    l2->yRot += var12; r2->yRot -= var12;
                    l3->yRot += var14; r3->yRot -= var14;
                    l1->zRot += var15; r1->zRot -= var15;
                    l2->zRot += var16; r2->zRot -= var16;
                    l3->zRot += var18; r3->zRot -= var18;
                }
                if ((m_kind == BeetleKind::Slime || m_kind == BeetleKind::SlimeTail) &&
                    m_tailBottom && m_tailTop && m_slimeCenter) {
                    m_tailBottom->xRot = std::cos(s.ageInTicks * 0.3335f) * 0.15f;
                    m_tailTop->xRot = std::cos(s.ageInTicks * 0.4445f) * 0.20f;
                    m_slimeCenter->xRot = std::cos(s.ageInTicks * 0.5555f + 0.25f) * 0.25f;
                }
            }

        private:
            BeetleKind m_kind;
            ModelPart* m_head = nullptr;
            ModelPart* m_legs[6] = {};
            ModelPart* m_tailBottom = nullptr;
            ModelPart* m_tailTop = nullptr;
            ModelPart* m_slimeCenter = nullptr;
            ModelPart* m_slime = nullptr;
            ModelPart* m_rightPincer = nullptr;
            ModelPart* m_leftPincer = nullptr;
        };

        // ── Helmet crab ────────────────────────────────────────────────────
        // TF HelmetCrabModel.setupAnim (see the header note for the fields).
        class HelmetCrabModel : public GeneratedModel {
        public:
            HelmetCrabModel() : GeneratedModel("helmet_crab") {
                m_helmet = m_root.Find("helmet_base");
                m_crab = m_root.Find("crab");
                m_body = m_root.Find("body");
                m_rightClaw = m_root.Find("right_claw");
                m_leftClaw = m_root.Find("left_claw");
                m_rightLeg1 = m_root.Find("right_leg_1");
                m_rightLeg2 = m_root.Find("right_leg_2");
                m_leftLeg1 = m_root.Find("left_leg_1");
                m_leftLeg2 = m_root.Find("left_leg_2");
            }

            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                if (!m_helmet || !m_crab || !m_body || !m_rightClaw || !m_leftClaw ||
                    !m_rightLeg1 || !m_rightLeg2 || !m_leftLeg1 || !m_leftLeg2) {
                    return;
                }
                m_helmet->yRot = s.tailAngle * kModDegToRad;
                // The death roll: f (from extraction) signed by id % 4.
                const int id = static_cast<int>(s.entityId);
                const float roll = s.headRollAngle * (id % 4 >= 2 ? 1.0f : -1.0f);
                m_crab->zRot = roll * 90.0f * kModDegToRad;
                m_crab->yRot = s.yRot * kModDegToRad;
                m_body->xRot = s.xRot * kModDegToRad;

                const float f6 = kModPi / 4.0f;
                m_rightLeg1->zRot = -f6 * 0.74f;
                m_leftLeg1->zRot = f6 * 0.74f;
                m_rightLeg2->zRot = -f6 * 0.74f;
                m_leftLeg2->zRot = f6 * 0.74f;
                constexpr float f8 = 0.3926991f;
                m_rightLeg1->yRot = f8;
                m_leftLeg1->yRot = -f8;
                m_rightLeg2->yRot = -f8;
                m_leftLeg2->yRot = f8;
                const float p = s.walkAnimationPos;
                const float a = s.walkAnimationSpeed;
                const float f10 = -(std::cos(p * 0.6662f * 2.0f + kModPi) * 0.4f) * a;
                const float f11 = -(std::cos(p * 0.6662f * 2.0f + kHalfPi) * 0.4f) * a;
                const float f12 = -(std::cos(p * 0.6662f * 2.0f + kModPi * 1.5f) * 0.4f) * a;
                const float f14 = std::abs(std::sin(p * 0.6662f + kModPi) * 0.4f) * a;
                const float f15 = std::abs(std::sin(p * 0.6662f + kHalfPi) * 0.4f) * a;
                const float f16 = std::abs(std::sin(p * 0.6662f + kModPi * 1.5f) * 0.4f) * a;
                m_rightLeg1->yRot += f10; m_leftLeg1->yRot -= f10;
                m_rightLeg2->yRot += f11; m_leftLeg2->yRot -= f11;
                m_rightLeg1->zRot += f14; m_leftLeg1->zRot -= f14;
                m_rightLeg2->zRot += f15; m_leftLeg2->zRot -= f15;
                // The right claw swings like an arm, the left like a leg.
                m_rightClaw->yRot = -1.319531f + std::cos(p * 0.6662f + kModPi) * 2.0f * a * 0.5f;
                m_leftClaw->zRot = f6 - f16;
                m_leftClaw->yRot = f8 * 2.0f - f12;
            }

        private:
            ModelPart* m_helmet = nullptr;
            ModelPart* m_crab = nullptr;
            ModelPart* m_body = nullptr;
            ModelPart* m_rightClaw = nullptr;
            ModelPart* m_leftClaw = nullptr;
            ModelPart* m_rightLeg1 = nullptr;
            ModelPart* m_rightLeg2 = nullptr;
            ModelPart* m_leftLeg1 = nullptr;
            ModelPart* m_leftLeg2 = nullptr;
        };

        // ── Troll ──────────────────────────────────────────────────────────
        // TF TrollModel.setupAnim — arms overhead holding a rock, the
        // two-handed club swing on attackTime otherwise.
        class TrollModel : public GeneratedModel {
        public:
            TrollModel() : GeneratedModel("troll") { m_parts.Resolve(m_root); }

            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const HumanoidParts& p = m_parts;
                if (!p.Complete()) return;
                p.HumanoidWalk(s);
                p.head->yRot = s.yRot * kModDegToRad;
                p.head->xRot = s.xRot * kModDegToRad;
                const float pos = s.walkAnimationPos * 0.6662f;
                const float spd = s.walkAnimationSpeed;
                p.rightLeg->xRot = std::cos(pos) * 1.4f * spd;
                p.leftLeg->xRot = std::cos(pos + kModPi) * 1.4f * spd;
                p.rightLeg->yRot = p.leftLeg->yRot = 0.0f;
                if (s.isHoldingItem) {
                    p.rightArm->xRot = p.leftArm->xRot = kModPi;   // arms up!
                } else {
                    p.rightArm->xRot = std::cos(pos + kModPi) * 2.0f * spd * 0.5f;
                    p.leftArm->xRot = std::cos(pos) * 2.0f * spd * 0.5f;
                }
                p.rightArm->zRot = p.leftArm->zRot = 0.0f;
                if (s.attackTime > 0.0f) {
                    const float swing = 1.0f - s.attackTime;
                    p.rightArm->xRot -= kModPi * swing;
                    p.leftArm->xRot -= kModPi * swing;
                }
                p.rightArm->yRot = p.leftArm->yRot = 0.0f;
                if (!s.isHoldingItem) BobArms(p.rightArm, p.leftArm, s.ageInTicks);
            }

        private:
            HumanoidParts m_parts;
        };

        // ── Block-and-chain goblin ─────────────────────────────────────────
        // TF BlockChainGoblinModel.setupAnim: HumanoidModel's walk with both
        // arms raised (the chain is held overhead).
        class BlockChainGoblinModel : public GeneratedModel {
        public:
            BlockChainGoblinModel() : GeneratedModel("block_and_chain_goblin") {
                m_parts.Resolve(m_root);
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                if (!m_parts.Complete()) return;
                m_parts.HumanoidWalk(s);
                m_parts.rightArm->xRot += kModPi;
                m_parts.leftArm->xRot += kModPi;
            }

        private:
            HumanoidParts m_parts;
        };

        // A mesh drawn as a layer at an arbitrary world offset from its
        // owner — BlockChainGoblinRenderer's spike ball and chain link. The
        // layer is built through the owner's entity matrix, so Place()
        // converts the world transform into the root part's pose (see
        // PlaceInWorld).
        class PlacedModel : public GeneratedModel {
        public:
            explicit PlacedModel(std::string_view slug) : GeneratedModel(slug) {}

            void Place(const glm::vec3& offsetPx, float yRot, float xRot) {
                m_offset = offsetPx;
                m_yRot = yRot;
                m_xRot = xRot;
            }
            void SetupAnim(const EntityRenderState&) override {
                m_root.ResetPose();
                m_root.x = m_offset.x;
                m_root.y = m_offset.y;
                m_root.z = m_offset.z;
                m_root.yRot = m_yRot;
                m_root.xRot = m_xRot;
            }

        private:
            glm::vec3 m_offset{0.0f};
            float m_yRot = 0.0f;
            float m_xRot = 0.0f;
        };

        // The owner's entity matrix is T(pos) * RotY(180 - bodyRot) *
        // S(-1,-1,1)/16 * T(0, -24.016, 0) (MobRenderer::EntityMatrix). A
        // model the TF renderer draws at T(pos + offset) * W * S(-1,-1,1)/16
        // is therefore the root pose T(p) * conj(RotY(bodyRot - 180) * W),
        // p = (0, 24.016, 0) + 16 * S * RotY(bodyRot - 180) * offset.
        glm::vec3 RootOffsetFor(const glm::dvec3& worldOffset, float bodyRotDeg) {
            const glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(bodyRotDeg - 180.0f),
                                            glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 v = glm::vec3(r * glm::vec4(glm::vec3(worldOffset), 0.0f));
            return glm::vec3(-16.0f * v.x, -16.0f * v.y + 24.016f, 16.0f * v.z);
        }

        // ── Goblin knights ─────────────────────────────────────────────────
        // TF UpperGoblinKnightModel.setupAnim (see the header note).
        class UpperGoblinKnightModel : public GeneratedModel {
        public:
            UpperGoblinKnightModel() : GeneratedModel("upper_goblin_knight") {
                m_parts.Resolve(m_root);
                m_breastplate = m_root.Find("breastplate");
                m_shield = m_root.Find("shield");
            }

            // UpperGoblinKnightRenderState.getArmRotationDuringSwing.
            static float ArmRotationDuringSwing(float spearTimer) {
                const float t = 60.0f - spearTimer;
                if (t <= 10.0f) return t;
                if (t <= 30.0f) return 10.0f;
                if (t <= 33.0f) return (t - 30.0f) * -8.0f + 10.0f;
                if (t <= 50.0f) return -15.0f;
                if (t <= 60.0f) return (10.0f - (t - 50.0f)) * -1.5f;
                return 0.0f;
            }

            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const HumanoidParts& p = m_parts;
                if (!p.Complete()) return;
                p.head->yRot = s.yRot * kModDegToRad;
                p.head->xRot = s.xRot * kModDegToRad;
                p.head->zRot = 0.0f;
                const float pos = s.walkAnimationPos * 0.6662f;
                const float spd = s.walkAnimationSpeed;
                p.rightArm->xRot = std::cos(pos + kModPi) * 2.0f * spd * 0.5f;
                const float leftConstraint = s.isHoldingItem ? 0.2f : spd;
                p.leftArm->zRot = s.isScared
                    ? (std::cos(s.ageInTicks * 3.25f) * kModPi * 0.4f) * kModDegToRad - 0.4f
                    : 0.0f;
                p.leftArm->xRot = std::cos(pos) * 2.0f * leftConstraint * 0.5f;
                p.rightArm->zRot = 0.0f;
                p.rightLeg->xRot = std::cos(pos) * 1.4f * spd;
                p.leftLeg->xRot = std::cos(pos + kModPi) * 1.4f * spd;
                p.rightLeg->yRot = p.leftLeg->yRot = 0.0f;
                if (s.isPassenger) {
                    p.rightArm->xRot -= kModPi / 5.0f;
                    p.leftArm->xRot -= kModPi / 5.0f;
                    p.rightLeg->xRot = 0.0f;
                    p.leftLeg->xRot = 0.0f;
                }
                p.rightArm->xRot = p.rightArm->xRot * 0.5f - kModPi / 10.0f;
                p.rightArm->xRot -= kModPi * 0.66f;
                if (s.attackTicksRemaining > 0.0f) {
                    p.rightArm->xRot -= ArmRotationDuringSwing(s.attackTicksRemaining) * kModDegToRad;
                }
                p.rightArm->yRot = p.leftArm->yRot = 0.0f;
                BobArms(p.rightArm, p.leftArm, s.ageInTicks);
                p.leftArm->zRot = -p.leftArm->zRot;   // shield arm points inward
                if (m_shield) {
                    m_shield->xRot = 2.0f * kModPi - p.leftArm->xRot;
                    m_shield->visible = s.isHoldingItem;
                }
                if (m_breastplate) m_breastplate->visible = s.hasChest;
            }

        private:
            HumanoidParts m_parts;
            ModelPart* m_breastplate = nullptr;
            ModelPart* m_shield = nullptr;
        };

        // UpperGoblinKnightRenderState.getPitchForAttack — the body tilt
        // UpperGoblinKnightRenderer.setupRotations applies during the slam.
        float PitchForAttack(float spearTimer) {
            const float t = 60.0f - spearTimer;
            if (t <= 10.0f) return t * 3.0f;
            if (t <= 30.0f) return 30.0f;
            if (t <= 33.0f) return (t - 30.0f) * -25.0f + 30.0f;
            if (t <= 50.0f) return -45.0f;
            if (t <= 60.0f) return (10.0f - (t - 50.0f)) * -4.5f;
            return 0.0f;
        }

        // TF LowerGoblinKnightModel.setupAnim.
        class LowerGoblinKnightModel : public GeneratedModel {
        public:
            LowerGoblinKnightModel() : GeneratedModel("lower_goblin_knight") {
                m_parts.Resolve(m_root);
                m_tunic = m_root.Find("tunic");
            }

            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const HumanoidParts& p = m_parts;
                if (!p.Complete()) return;
                const bool hasUpper = s.isRidden;
                if (hasUpper) {
                    p.head->yRot = 0.0f;
                    p.head->xRot = 0.0f;
                } else {
                    p.head->yRot = s.yRot * kModDegToRad;
                    p.head->xRot = s.xRot * kModDegToRad;
                }
                const float pos = s.walkAnimationPos * 0.6662f;
                const float spd = s.walkAnimationSpeed;
                const bool swingArms = !s.hasChest && !hasUpper;
                p.rightArm->xRot = swingArms ? std::cos(pos + kModPi) * 2.0f * spd * 0.5f : 0.0f;
                p.leftArm->xRot = swingArms ? std::cos(pos) * 2.0f * spd * 0.5f : 0.0f;
                p.rightArm->zRot = p.leftArm->zRot = 0.0f;
                p.rightLeg->xRot = std::cos(pos) * 1.4f * spd;
                p.leftLeg->xRot = std::cos(pos + kModPi) * 1.4f * spd;
                p.rightLeg->yRot = p.leftLeg->yRot = 0.0f;
                if (swingArms) BobArms(p.rightArm, p.leftArm, s.ageInTicks);
                if (m_tunic) m_tunic->visible = s.hasChest;
            }

        private:
            HumanoidParts m_parts;
            ModelPart* m_tunic = nullptr;
        };

        // Layer models, one per mesh (per link for the chain), built on first
        // use and posed from each mob's render state as it is drawn.
        std::unique_ptr<EntityModel> s_wraithLayer;
        std::unique_ptr<EntityModel> s_slimeBeetleTail;
        std::unique_ptr<PlacedModel> s_spikeBlock;
        std::unique_ptr<PlacedModel> s_chainLink;

        // The partial tick of the mob being drawn — ExtractRenderState runs
        // before Layers for the same mob; the chain goblin's ball lerps on it.
        float s_partialTick = 0.0f;

        constexpr const char* kSpiderEyes = "assets/textures/entity/spider_eyes.png";

    } // namespace

    std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type) {
        switch (type) {
            // TFSpiderRenderer extends SpiderRenderer: MC's SpiderModel.
            case Game::EntityTypeId::HedgeSpider:
            case Game::EntityTypeId::SwarmSpider:
                return std::make_unique<SpiderModel>();
            case Game::EntityTypeId::Wraith:
                if (FindGenModel("wraith")) return std::make_unique<WraithModel>(true);
                return nullptr;
            case Game::EntityTypeId::FireBeetle:
                if (FindGenModel("fire_beetle")) {
                    return std::make_unique<BeetleModel>("fire_beetle", BeetleKind::Fire);
                }
                return nullptr;
            case Game::EntityTypeId::SlimeBeetle:
                if (FindGenModel("slime_beetle")) {
                    return std::make_unique<BeetleModel>("slime_beetle", BeetleKind::Slime);
                }
                return nullptr;
            case Game::EntityTypeId::PinchBeetle:
                if (FindGenModel("pinch_beetle")) {
                    return std::make_unique<BeetleModel>("pinch_beetle", BeetleKind::Pinch);
                }
                return nullptr;
            case Game::EntityTypeId::HelmetCrab:
                if (FindGenModel("helmet_crab")) return std::make_unique<HelmetCrabModel>();
                return nullptr;
            case Game::EntityTypeId::Troll:
                if (FindGenModel("troll")) return std::make_unique<TrollModel>();
                return nullptr;
            // TowerwoodBorerRenderer: MC's SilverfishModel (its generated
            // mesh and compiled setupAnim).
            case Game::EntityTypeId::TowerwoodBorer:
                if (FindGenModel("silverfish")) return std::make_unique<GeneratedModel>("silverfish");
                return nullptr;
            // MazeSlimeRenderer: SlimeModel's inner body (the translucent
            // outer shell is not in the generated set, as for MC's slime).
            case Game::EntityTypeId::MazeSlime:
                if (FindGenModel("slime")) return std::make_unique<GeneratedModel>("slime");
                return nullptr;
            case Game::EntityTypeId::Minotaur:
                if (FindGenModel("minotaur")) return std::make_unique<ModHumanoidModel>("minotaur", false);
                return nullptr;
            // RedcapSapperRenderer extends RedcapRenderer: the redcap mesh.
            case Game::EntityTypeId::RedcapSapper:
                if (FindGenModel("redcap")) return std::make_unique<ModHumanoidModel>("redcap", false);
                return nullptr;
            case Game::EntityTypeId::BlockAndChainGoblin:
                if (FindGenModel("block_and_chain_goblin")) {
                    return std::make_unique<BlockChainGoblinModel>();
                }
                return nullptr;
            case Game::EntityTypeId::UpperGoblinKnight:
                if (FindGenModel("upper_goblin_knight")) return std::make_unique<UpperGoblinKnightModel>();
                return nullptr;
            case Game::EntityTypeId::LowerGoblinKnight:
                if (FindGenModel("lower_goblin_knight")) return std::make_unique<LowerGoblinKnightModel>();
                return nullptr;
            default:
                return nullptr;
        }
    }

    std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type) {
        (void)type;
        return nullptr;
    }

    std::string_view TexturePath(Game::EntityTypeId type) {
        // The mod's own sheets (CC BY-NC-SA, assets/ATTRIBUTION.md, copied by
        // tools/copy_twilight_assets.py), named per each renderer's
        // getModelTexture.
        switch (type) {
            case Game::EntityTypeId::HedgeSpider:
                return "assets/textures/entity/twilightforest/hedge_spider.png";
            case Game::EntityTypeId::SwarmSpider:
                return "assets/textures/entity/twilightforest/swarm_spider.png";
            case Game::EntityTypeId::Wraith:
                return "assets/textures/entity/twilightforest/wraith.png";
            case Game::EntityTypeId::FireBeetle:
                return "assets/textures/entity/twilightforest/fire_beetle.png";
            case Game::EntityTypeId::SlimeBeetle:
                return "assets/textures/entity/twilightforest/slime_beetle.png";
            case Game::EntityTypeId::PinchBeetle:
                return "assets/textures/entity/twilightforest/pinch_beetle.png";
            case Game::EntityTypeId::HelmetCrab:
                return "assets/textures/entity/twilightforest/helmet_crab.png";
            case Game::EntityTypeId::Troll:
                return "assets/textures/entity/twilightforest/troll.png";
            case Game::EntityTypeId::TowerwoodBorer:
                return "assets/textures/entity/twilightforest/towerwood_borer.png";
            case Game::EntityTypeId::MazeSlime:
                return "assets/textures/entity/twilightforest/maze_slime.png";
            case Game::EntityTypeId::Minotaur:
                return "assets/textures/entity/twilightforest/minotaur.png";
            case Game::EntityTypeId::RedcapSapper:
                return "assets/textures/entity/twilightforest/redcap_sapper.png";
            case Game::EntityTypeId::BlockAndChainGoblin:
                return "assets/textures/entity/twilightforest/block_and_chain_goblin.png";
            case Game::EntityTypeId::UpperGoblinKnight:
            case Game::EntityTypeId::LowerGoblinKnight:
                return "assets/textures/entity/twilightforest/goblin_knight.png";
            default:
                return {};
        }
    }

    bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                            float partialTick, EntityRenderState& state) {
        switch (type) {
            // TFSpiderRenderer.scale: 1.0 (hedge) / 0.5 (swarm).
            case Game::EntityTypeId::HedgeSpider:
                return true;
            case Game::EntityTypeId::SwarmSpider:
                state.modelScale = glm::vec3(0.5f);
                return true;
            case Game::EntityTypeId::PinchBeetle:
                if (const auto* beetle = MobAs<Game::PinchBeetle>(mob, type, type)) {
                    state.isRidden = beetle->IsHoldingVictim();
                }
                return true;
            case Game::EntityTypeId::HelmetCrab:
                if (const auto* crab = MobAs<Game::HelmetCrab>(mob, type, type)) {
                    state.tailAngle = crab->GetHelmetRotation(partialTick);
                    float f = 0.0f;
                    if (crab->deathTime > 0) {
                        f = std::sqrt(std::max(0.0f, (static_cast<float>(crab->deathTime) +
                                                      partialTick - 1.0f) / 20.0f * 1.6f));
                        f = std::min(f, 1.0f);
                    }
                    state.headRollAngle = f;
                }
                return true;
            case Game::EntityTypeId::Troll:
                if (const auto* troll = MobAs<Game::Troll>(mob, type, type)) {
                    state.isHoldingItem = troll->HasRock();
                }
                return true;
            case Game::EntityTypeId::MazeSlime:
                // MazeSlimeRenderer.scale: 0.999, then the squish stretch
                // around the size (the 0.001 lift is dropped).
                if (const auto* slime = MobAs<Game::Slime>(mob, type, type)) {
                    const float size = static_cast<float>(slime->GetSize());
                    const float squish = slime->GetSquish(partialTick) / (size * 0.5f + 1.0f);
                    const float scaled = 1.0f / (squish + 1.0f);
                    state.squish = slime->GetSquish(partialTick);
                    state.modelScale = glm::vec3(0.999f * scaled * size,
                                                 0.999f / scaled * size,
                                                 0.999f * scaled * size);
                }
                return true;
            case Game::EntityTypeId::UpperGoblinKnight:
                if (const auto* knight = MobAs<Game::UpperGoblinKnight>(mob, type, type)) {
                    const float timer = static_cast<float>(knight->GetHeavySpearTimer());
                    state.attackTicksRemaining = timer;
                    state.hasChest = knight->HasArmor();
                    state.isHoldingItem = knight->HasShield();
                    state.isScared = knight->IsShieldDisabled();
                    state.isPassenger = knight->IsPassenger();
                    // setupRotations: the whole body pitches through the slam
                    // (about the feet — the swim-tilt slot, pivot 0).
                    if (timer > 0.0f) {
                        state.swimPitchDeg = PitchForAttack(timer);
                        state.swimPivotY = 0.0f;
                    }
                }
                return true;
            case Game::EntityTypeId::LowerGoblinKnight:
                if (const auto* knight = MobAs<Game::LowerGoblinKnight>(mob, type, type)) {
                    state.hasChest = knight->HasArmor();
                    state.isRidden = knight->HasUpperGoblinClient() || knight->IsVehicle();
                }
                return true;
            case Game::EntityTypeId::BlockAndChainGoblin:
                s_partialTick = partialTick;
                return true;
            case Game::EntityTypeId::Wraith:
            case Game::EntityTypeId::FireBeetle:
            case Game::EntityTypeId::SlimeBeetle:
            case Game::EntityTypeId::TowerwoodBorer:
            case Game::EntityTypeId::Minotaur:
            case Game::EntityTypeId::RedcapSapper:
                return true;
            default:
                return false;
        }
    }

    std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,
                                         const EntityRenderState& state) {
        (void)state;
        // HelmetCrabRenderer.getTextureLocation: the blue shell.
        if (const auto* crab = MobAs<Game::HelmetCrab>(mob, type, Game::EntityTypeId::HelmetCrab)) {
            if (crab->IsBlue()) return "assets/textures/entity/twilightforest/helmet_crab_blue.png";
        }
        return {};
    }

    int Layers(const Game::Mob& mob, Game::EntityTypeId type,
               const EntityRenderState& state, ModLayer* out) {
        switch (type) {
            // SpiderRenderer's SpiderEyesLayer (TFSpiderRenderer inherits it).
            case Game::EntityTypeId::HedgeSpider:
            case Game::EntityTypeId::SwarmSpider:
                out[0].texture = kSpiderEyes;
                out[0].noOverlay = true;
                out[0].emissive = true;   // RenderTypes.eyes
                return 1;
            // WraithRenderer.getModelTint: the visible wraith at 0.6 alpha.
            case Game::EntityTypeId::Wraith:
                if (!FindGenModel("wraith")) return 0;
                if (!s_wraithLayer) s_wraithLayer = std::make_unique<WraithModel>(false);
                out[0].model = s_wraithLayer.get();
                out[0].texture = TexturePath(type);
                out[0].a = static_cast<uint8_t>(0.6f * 255.0f);
                out[0].blend = true;
                return 1;
            // SlimeBeetleRenderer.OuterTailLayer — entityTranslucent.
            case Game::EntityTypeId::SlimeBeetle:
                if (!FindGenModel("slime_beetle")) return 0;
                if (!s_slimeBeetleTail) {
                    s_slimeBeetleTail = std::make_unique<BeetleModel>("slime_beetle", BeetleKind::SlimeTail);
                }
                out[0].model = s_slimeBeetleTail.get();
                out[0].texture = TexturePath(type);
                out[0].blend = true;
                return 1;
            // BlockChainGoblinRenderer.submit: the spike ball at the block's
            // position (rotated 180 - yRot, then xRot; NO_OVERLAY) and — while
            // alive — the chain link from the eye toward the ball.
            case Game::EntityTypeId::BlockAndChainGoblin: {
                const auto* goblin = MobAs<Game::BlockChainGoblin>(mob, type, type);
                if (!goblin || !FindGenModel("tf_spike_block")) return 0;
                if (!s_spikeBlock) s_spikeBlock = std::make_unique<PlacedModel>("tf_spike_block");
                const float bodyRot = state.yBodyRotAbs;
                const glm::dvec3 ball = goblin->GetBallOffset(s_partialTick);
                s_spikeBlock->Place(RootOffsetFor(ball, bodyRot),
                                    (state.yRot - bodyRot) * kModDegToRad,
                                    -state.xRot * kModDegToRad);
                int n = 0;
                out[n].model = s_spikeBlock.get();
                out[n].texture = "assets/textures/entity/twilightforest/block_and_chain.png";
                out[n].noOverlay = true;
                ++n;
                if (goblin->deathTime <= 0 && FindGenModel("tf_chain")) {
                    if (!s_chainLink) s_chainLink = std::make_unique<PlacedModel>("tf_chain");
                    // chainStartPos = (ball eye - goblin eye) * (1, 0.5, 1),
                    // drawn from the goblin's eye height.
                    const double ballEye = Game::BlockChainGoblin::kBallSize * 0.85;
                    const double eye = goblin->GetEyeHeight();
                    const glm::dvec3 link(ball.x, eye + (ball.y + ballEye - eye) * 0.5, ball.z);
                    s_chainLink->Place(RootOffsetFor(link, bodyRot),
                                       (180.0f - bodyRot) * kModDegToRad, 0.0f);
                    out[n].model = s_chainLink.get();
                    out[n].texture = "assets/textures/entity/twilightforest/block_and_chain.png";
                    out[n].noOverlay = true;
                    ++n;
                }
                return n;
            }
            default:
                return 0;
        }
    }

    bool BodyTranslucent(Game::EntityTypeId type) {
        (void)type;
        return false;
    }

    float FlipDegrees(Game::EntityTypeId type) {
        switch (type) {
            // SpiderRenderer.getFlipDegrees: 180 — onto their backs.
            case Game::EntityTypeId::HedgeSpider:
            case Game::EntityTypeId::SwarmSpider:
                return 180.0f;
            // HelmetCrabRenderer.getFlipDegrees: 0 — the model rolls itself.
            // The hook reads 0 as "not mine", so the smallest non-zero angle
            // stands for it.
            case Game::EntityTypeId::HelmetCrab:
                return 1.0e-4f;
            default:
                return 0.0f;
        }
    }

    const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,
                         const EntityRenderState& state) {
        (void)mob; (void)state;
        switch (type) {
            // Minotaur.populateDefaultEquipmentSlots: a golden axe (TF's
            // golden minotaur axe 1 time in 10/(difficulty+1), drawn as the
            // golden axe).
            case Game::EntityTypeId::Minotaur:     return "golden_axe";
            // RedcapSapper: an ironwood pickaxe — drawn as the iron one.
            case Game::EntityTypeId::RedcapSapper: return "iron_pickaxe";
            default:                               return nullptr;
        }
    }

} // namespace Render::TwilightHostileRender
