// File: src/client/renderer/entity/model/EntityModels.cpp
#include "client/renderer/entity/model/EntityModels.hpp"
#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>

#include <cmath>

namespace Render {

    namespace {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kDegToRad = kPi / 180.0f;

        // MC's limb-swing frequency. Appears in every setupAnim below.
        constexpr float kSwingFreq = 0.6662f;

        // Shorthand for one cube on a part. The hand-written meshes only ever
        // use MC's uniform CubeDeformation, so one grow fills all three axes.
        void AddBox(ModelPart* part, float texX, float texY,
                    float ox, float oy, float oz, float sx, float sy, float sz,
                    float grow = 0.0f, bool mirror = false) {
            part->cubes.push_back(CubeDefinition{ ox, oy, oz, sx, sy, sz,
                                                  texX, texY,
                                                  grow, grow, grow, mirror });
        }

        // MC client/model/BabyModelTransform.apply: each ROOT child's rest
        // pose is translated, then scaled — and PartPose.scaled multiplies
        // the offsets AND the pose scale (PartPose.java:30-32), which is how
        // the shrink composes down through LocalMatrix exactly like MC's
        // baked baby mesh. Parts below the root keep their own poses.
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

        // MC AnimationUtils.bobModelPart — the idle sway on raised zombie and
        // skeleton arms. Additive, so it layers on top of the attack pose.
        void BobModelPart(ModelPart* part, float ageInTicks, float scale) {
            part->zRot += scale * (std::cos(ageInTicks * 0.09f) * 0.05f + 0.05f);
            part->xRot += scale * std::sin(ageInTicks * 0.067f) * 0.05f;
        }
    }

    // ── HumanoidModel ──────────────────────────────────────────────────────

    HumanoidModel::HumanoidModel(bool slim) {
        m_texWidth = 64.0f;
        m_texHeight = slim ? 32.0f : 64.0f;

        // MC HumanoidModel.createMesh(CubeDeformation.NONE, 0.0F).
        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 0.0f, 0.0f));
        AddBox(m_head, 0, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f);

        m_hat = m_head->AddChild("hat", PartPose::Zero());
        AddBox(m_hat, 32, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f, 0.5f);

        m_body = m_root.AddChild("body", PartPose::Offset(0.0f, 0.0f, 0.0f));
        AddBox(m_body, 16, 16, -4.0f, 0.0f, -2.0f, 8.0f, 12.0f, 4.0f);

        if (slim) {
            // MC SkeletonModel.createDefaultSkeletonMesh — 2-wide limbs, which
            // is what makes a skeleton read as bones rather than a thin zombie.
            m_rightArm = m_root.AddChild("right_arm", PartPose::Offset(-5.0f, 2.0f, 0.0f));
            AddBox(m_rightArm, 40, 16, -1.0f, -2.0f, -1.0f, 2.0f, 12.0f, 2.0f);

            m_leftArm = m_root.AddChild("left_arm", PartPose::Offset(5.0f, 2.0f, 0.0f));
            AddBox(m_leftArm, 40, 16, -1.0f, -2.0f, -1.0f, 2.0f, 12.0f, 2.0f, 0.0f, true);

            m_rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-2.0f, 12.0f, 0.0f));
            AddBox(m_rightLeg, 0, 16, -1.0f, 0.0f, -1.0f, 2.0f, 12.0f, 2.0f);

            m_leftLeg = m_root.AddChild("left_leg", PartPose::Offset(2.0f, 12.0f, 0.0f));
            AddBox(m_leftLeg, 0, 16, -1.0f, 0.0f, -1.0f, 2.0f, 12.0f, 2.0f, 0.0f, true);
        } else {
            m_rightArm = m_root.AddChild("right_arm", PartPose::Offset(-5.0f, 2.0f, 0.0f));
            AddBox(m_rightArm, 40, 16, -3.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f);

            m_leftArm = m_root.AddChild("left_arm", PartPose::Offset(5.0f, 2.0f, 0.0f));
            AddBox(m_leftArm, 40, 16, -1.0f, -2.0f, -2.0f, 4.0f, 12.0f, 4.0f, 0.0f, true);

            // -1.9 rather than -2.0: MC offsets the legs a tenth of a pixel
            // inward so they do not z-fight where they meet the body.
            m_rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-1.9f, 12.0f, 0.0f));
            AddBox(m_rightLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f);

            m_leftLeg = m_root.AddChild("left_leg", PartPose::Offset(1.9f, 12.0f, 0.0f));
            AddBox(m_leftLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, 0.0f, true);
        }

        m_root.ResetPose();
    }

    bool HumanoidModel::BecomeBaby() {
        // HumanoidModel.BABY_TRANSFORMER (HumanoidModel.java:27). The hat is
        // a child of the head in MC too (createMesh:67), so the transform
        // never touches it — only root children move.
        ApplyBabyTransform(m_root, true, 16.0f, 0.0f, 2.0f, 2.0f, 24.0f, {"head"});
        return true;
    }

    void HumanoidModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();

        m_head->xRot = state.xRot * kDegToRad;
        m_head->yRot = state.yRot * kDegToRad;

        const float pos = state.walkAnimationPos;
        const float speed = state.walkAnimationSpeed;

        // Arms swing at twice the amplitude of legs but at half weight, which
        // nets out to a slightly smaller arc — MC's `2.0F * speed * 0.5F`.
        m_rightArm->xRot = std::cos(pos * kSwingFreq + kPi) * 2.0f * speed * 0.5f;
        m_leftArm->xRot  = std::cos(pos * kSwingFreq) * 2.0f * speed * 0.5f;
        m_rightLeg->xRot = std::cos(pos * kSwingFreq) * 1.4f * speed;
        m_leftLeg->xRot  = std::cos(pos * kSwingFreq + kPi) * 1.4f * speed;

        // The hundredth-radian splay stops the legs intersecting exactly when
        // they cross at the top of the swing.
        m_rightLeg->yRot =  0.005f;
        m_leftLeg->yRot  = -0.005f;
        m_rightLeg->zRot =  0.005f;
        m_leftLeg->zRot  = -0.005f;

        // MC branches here on which hand is using an item and on whether the
        // pose is two-handed. Nothing in this port is left-handed or uses an
        // item, so both branches collapse to "right arm first, then the left
        // unless the right's pose already wrote it".
        PoseRightArm(state);
        if (!ArmPoseAffectsOffhand(state.rightArmPose)) {
            PoseLeftArm(state);
        }

        SetupAttackAnimation(state);

        // Idle sway, applied unconditionally in MC (the SPYGLASS exclusion is
        // the only exception and nothing here can hold one). It is ADDITIVE, so
        // it layers over whatever the pose above wrote.
        BobModelPart(m_rightArm, state.ageInTicks, 1.0f);
        BobModelPart(m_leftArm, state.ageInTicks, -1.0f);
    }

    void HumanoidModel::PoseRightArm(const EntityRenderState& state) {
        switch (state.rightArmPose) {
            case ArmPose::Empty:
                m_rightArm->yRot = 0.0f;
                break;
            case ArmPose::Item:
                m_rightArm->xRot = m_rightArm->xRot * 0.5f - kPi / 10.0f;
                m_rightArm->yRot = 0.0f;
                break;
            case ArmPose::BowAndArrow:
                // Both arms, and both keyed off the HEAD — which is what makes
                // a drawn bow track where the skeleton is looking rather than
                // where its body faces.
                m_rightArm->yRot = -0.1f + m_head->yRot;
                m_leftArm->yRot  =  0.1f + m_head->yRot + 0.4f;
                m_rightArm->xRot = -kPi / 2.0f + m_head->xRot;
                m_leftArm->xRot  = -kPi / 2.0f + m_head->xRot;
                break;
            default:
                // The remaining MC poses (shield, crossbow, spyglass, spear…)
                // need items this port does not have. Leaving the walk swing
                // untouched is what MC's default branch does too.
                break;
        }
    }

    void HumanoidModel::PoseLeftArm(const EntityRenderState& state) {
        switch (state.leftArmPose) {
            case ArmPose::Empty:
                m_leftArm->yRot = 0.0f;
                break;
            case ArmPose::Item:
                m_leftArm->xRot = m_leftArm->xRot * 0.5f - kPi / 10.0f;
                m_leftArm->yRot = 0.0f;
                break;
            case ArmPose::BowAndArrow:
                m_rightArm->yRot = -0.1f + m_head->yRot - 0.4f;
                m_leftArm->yRot  =  0.1f + m_head->yRot;
                m_rightArm->xRot = -kPi / 2.0f + m_head->xRot;
                m_leftArm->xRot  = -kPi / 2.0f + m_head->xRot;
                break;
            default:
                break;
        }
    }

    void HumanoidModel::SetupAttackAnimation(const EntityRenderState& state) {
        const float attackTime = state.attackTime;
        if (attackTime <= 0.0f) return;

        // The body twists on a sine of the SQUARE ROOT of progress, so the
        // wind-up is fast and the follow-through drags — the shape of MC's
        // swing. Everything below hangs off this one angle.
        m_body->yRot = std::sin(std::sqrt(attackTime) * kPi * 2.0f) * 0.2f;

        // MC flips the sign for a left-handed attack; every mob here is
        // right-handed, so the flip is unreachable.
        const float ageScale = state.ageScale;
        m_rightArm->z =  std::sin(m_body->yRot) * 5.0f * ageScale;
        m_rightArm->x = -std::cos(m_body->yRot) * 5.0f * ageScale;
        m_leftArm->z  = -std::sin(m_body->yRot) * 5.0f * ageScale;
        m_leftArm->x  =  std::cos(m_body->yRot) * 5.0f * ageScale;
        m_rightArm->yRot += m_body->yRot;
        m_leftArm->yRot  += m_body->yRot;
        m_leftArm->xRot  += m_body->yRot;

        // MC SwingAnimationType.WHACK — the default for any item without a
        // SWING_ANIMATION component, which is everything this port has.
        // Ease.outQuart(x) = 1 - (1-x)^4.
        const float e = 1.0f - (1.0f - attackTime) * (1.0f - attackTime)
                             * (1.0f - attackTime) * (1.0f - attackTime);
        const float aa = std::sin(e * kPi);
        const float bb = std::sin(attackTime * kPi) * -(m_head->xRot - 0.7f) * 0.75f;

        m_rightArm->xRot -= aa * 1.2f + bb;
        m_rightArm->yRot += m_body->yRot * 2.0f;
        m_rightArm->zRot += std::sin(attackTime * kPi) * -0.4f;
    }

    // ── ZombieModel ────────────────────────────────────────────────────────

    void ZombieModel::SetupAnim(const EntityRenderState& state) {
        HumanoidModel::SetupAnim(state);

        // MC AnimationUtils.animateZombieArms. The arms drop to -PI/1.5 when
        // aggressive and -PI/2.25 otherwise — that difference is the whole
        // "zombie raises its arms when chasing you" tell.
        const float armDrop = -kPi / (state.isAggressive ? 1.5f : 2.25f);
        const float t = state.attackTime;
        const float attackY = std::sin(t * kPi);
        const float attackX = std::sin((1.0f - (1.0f - t) * (1.0f - t)) * kPi);

        m_rightArm->zRot = 0.0f;
        m_rightArm->yRot = -(0.1f - attackY * 0.6f);
        m_rightArm->xRot = armDrop + attackY * 1.2f - attackX * 0.4f;

        m_leftArm->zRot = 0.0f;
        m_leftArm->yRot = 0.1f - attackY * 0.6f;
        m_leftArm->xRot = armDrop + attackY * 1.2f - attackX * 0.4f;

        BobModelPart(m_rightArm, state.ageInTicks, 1.0f);
        BobModelPart(m_leftArm, state.ageInTicks, -1.0f);
    }

    // ── SkeletonModel ──────────────────────────────────────────────────────

    void SkeletonModel::SetupAnim(const EntityRenderState& state) {
        HumanoidModel::SetupAnim(state);

        // MC raises a skeleton's arms into the two-handed melee pose only when
        // it is aggressive AND NOT holding a bow. A bow-carrying skeleton is
        // posed by ArmPose::BowAndArrow in the base class instead, so running
        // this as well would overwrite the aim and leave the bow floating.
        if (!state.isAggressive || state.isHoldingBow) return;

        const float t = state.attackTime;
        const float attack2 = std::sin(t * kPi);
        const float attack = std::sin((1.0f - (1.0f - t) * (1.0f - t)) * kPi);

        m_rightArm->zRot = 0.0f;
        m_leftArm->zRot = 0.0f;
        m_rightArm->yRot = -(0.1f - attack2 * 0.6f);
        m_leftArm->yRot = 0.1f - attack2 * 0.6f;
        m_rightArm->xRot = -kPi / 2.0f - (attack2 * 1.2f - attack * 0.4f);
        m_leftArm->xRot  = -kPi / 2.0f - (attack2 * 1.2f - attack * 0.4f);

        BobModelPart(m_rightArm, state.ageInTicks, 1.0f);
        BobModelPart(m_leftArm, state.ageInTicks, -1.0f);
    }

    bool SkeletonModel::RightHandMatrix(glm::mat4& out) const {
        if (!m_rightArm) return false;

        // MC SkeletonModel.translateToHand: root, then the arm shifted one
        // pixel outward (`part.x += offset` with offset +1 for the right arm,
        // undone immediately after).
        out = m_root.LocalMatrix() * m_rightArm->LocalMatrix(glm::vec3(1.0f, 0.0f, 0.0f));
        return true;
    }

    // ── QuadrupedModel ─────────────────────────────────────────────────────

    void QuadrupedModel::BuildBodyMesh(int legSize, bool mirrorLeftLeg, bool mirrorRightLeg,
                                       float grow) {
        // MC QuadrupedModel.createBodyMesh. The head and body are replaced by
        // most subclasses; the legs are shared.
        m_head = m_root.AddChild("head",
            PartPose::Offset(0.0f, static_cast<float>(18 - legSize), -6.0f));
        AddBox(m_head, 0, 0, -4.0f, -4.0f, -8.0f, 8.0f, 8.0f, 8.0f, grow);

        m_body = m_root.AddChild("body",
            PartPose::OffsetAndRotation(0.0f, static_cast<float>(17 - legSize), 2.0f,
                                        kPi / 2.0f, 0.0f, 0.0f));
        AddBox(m_body, 28, 8, -5.0f, -10.0f, -7.0f, 10.0f, 16.0f, 8.0f, grow);

        const float legY = static_cast<float>(24 - legSize);
        const float legH = static_cast<float>(legSize);

        m_rightHindLeg = m_root.AddChild("right_hind_leg", PartPose::Offset(-3.0f, legY, 7.0f));
        AddBox(m_rightHindLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, legH, 4.0f, grow, mirrorRightLeg);

        m_leftHindLeg = m_root.AddChild("left_hind_leg", PartPose::Offset(3.0f, legY, 7.0f));
        AddBox(m_leftHindLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, legH, 4.0f, grow, mirrorLeftLeg);

        m_rightFrontLeg = m_root.AddChild("right_front_leg", PartPose::Offset(-3.0f, legY, -5.0f));
        AddBox(m_rightFrontLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, legH, 4.0f, grow, mirrorRightLeg);

        m_leftFrontLeg = m_root.AddChild("left_front_leg", PartPose::Offset(3.0f, legY, -5.0f));
        AddBox(m_leftFrontLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, legH, 4.0f, grow, mirrorLeftLeg);
    }

    void QuadrupedModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();

        m_head->xRot = state.xRot * kDegToRad;
        m_head->yRot = state.yRot * kDegToRad;

        const float pos = state.walkAnimationPos;
        const float speed = state.walkAnimationSpeed;

        // Diagonal gait: each hind leg is in phase with the OPPOSITE front leg.
        m_rightHindLeg->xRot  = std::cos(pos * kSwingFreq) * 1.4f * speed;
        m_leftHindLeg->xRot   = std::cos(pos * kSwingFreq + kPi) * 1.4f * speed;
        m_rightFrontLeg->xRot = std::cos(pos * kSwingFreq + kPi) * 1.4f * speed;
        m_leftFrontLeg->xRot  = std::cos(pos * kSwingFreq) * 1.4f * speed;
    }

    // ── CowModel ───────────────────────────────────────────────────────────

    CowModel::CowModel() {
        m_texWidth = 64.0f;
        m_texHeight = 64.0f;

        // MC CowModel.createBaseCowModel builds its own parts rather than
        // reusing createBodyMesh — the cow is wider than the generic quadruped.
        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 4.0f, -8.0f));
        AddBox(m_head, 0, 0, -4.0f, -4.0f, -6.0f, 8.0f, 8.0f, 6.0f);
        AddBox(m_head, 1, 33, -3.0f, 1.0f, -7.0f, 6.0f, 3.0f, 1.0f);       // muzzle
        AddBox(m_head, 22, 0, -5.0f, -5.0f, -5.0f, 1.0f, 3.0f, 1.0f);      // right horn
        AddBox(m_head, 22, 0, 4.0f, -5.0f, -5.0f, 1.0f, 3.0f, 1.0f);       // left horn

        m_body = m_root.AddChild("body",
            PartPose::OffsetAndRotation(0.0f, 5.0f, 2.0f, kPi / 2.0f, 0.0f, 0.0f));
        AddBox(m_body, 18, 4, -6.0f, -10.0f, -7.0f, 12.0f, 18.0f, 10.0f);
        AddBox(m_body, 52, 0, -2.0f, 2.0f, -8.0f, 4.0f, 6.0f, 1.0f);       // udder

        m_rightHindLeg = m_root.AddChild("right_hind_leg", PartPose::Offset(-4.0f, 12.0f, 7.0f));
        AddBox(m_rightHindLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f);

        m_leftHindLeg = m_root.AddChild("left_hind_leg", PartPose::Offset(4.0f, 12.0f, 7.0f));
        AddBox(m_leftHindLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, 0.0f, true);

        m_rightFrontLeg = m_root.AddChild("right_front_leg", PartPose::Offset(-4.0f, 12.0f, -5.0f));
        AddBox(m_rightFrontLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f);

        m_leftFrontLeg = m_root.AddChild("left_front_leg", PartPose::Offset(4.0f, 12.0f, -5.0f));
        AddBox(m_leftFrontLeg, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 12.0f, 4.0f, 0.0f, true);

        m_root.ResetPose();
    }

    bool CowModel::BecomeBaby() {
        // CowModel.BABY_TRANSFORMER (CowModel.java:16) — the 4-arg
        // BabyModelTransform ctor, so head/body scales default to 2 and the
        // body drop to 24 (BabyModelTransform.java:16-18).
        ApplyBabyTransform(m_root, false, 8.0f, 6.0f, 2.0f, 2.0f, 24.0f, {"head"});
        return true;
    }

    // ── PigModel ───────────────────────────────────────────────────────────

    PigModel::PigModel() {
        m_texWidth = 64.0f;
        m_texHeight = 64.0f;

        // Legs and body from the generic quadruped at legSize 6, then the head
        // is replaced to add the snout.
        BuildBodyMesh(6, true, false, 0.0f);

        m_head->pose = PartPose::Offset(0.0f, 12.0f, -6.0f);
        m_head->cubes.clear();
        AddBox(m_head, 0, 0, -4.0f, -4.0f, -8.0f, 8.0f, 8.0f, 8.0f);
        AddBox(m_head, 16, 16, -2.0f, 0.0f, -9.0f, 4.0f, 3.0f, 1.0f);      // snout

        m_root.ResetPose();
    }

    bool PigModel::BecomeBaby() {
        // PigModel.BABY_TRANSFORMER (PigModel.java:17).
        ApplyBabyTransform(m_root, false, 4.0f, 4.0f, 2.0f, 2.0f, 24.0f, {"head"});
        return true;
    }

    // ── SheepModel ─────────────────────────────────────────────────────────

    SheepModel::SheepModel(bool fur) : m_fur(fur) {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        if (fur) {
            // MC SheepFurModel.createFurLayer — the same skeleton, inflated.
            // The three different grow values are what give the wool its
            // characteristic bulk: a lot on the body, less on the head and legs.
            m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 6.0f, -8.0f));
            AddBox(m_head, 0, 0, -3.0f, -4.0f, -4.0f, 6.0f, 6.0f, 6.0f, 0.6f);

            m_body = m_root.AddChild("body",
                PartPose::OffsetAndRotation(0.0f, 5.0f, 2.0f, kPi / 2.0f, 0.0f, 0.0f));
            AddBox(m_body, 28, 8, -4.0f, -10.0f, -7.0f, 8.0f, 16.0f, 6.0f, 1.75f);

            const auto leg = [&](const char* name, float x, float z) {
                ModelPart* p = m_root.AddChild(name, PartPose::Offset(x, 12.0f, z));
                AddBox(p, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 6.0f, 4.0f, 0.5f);
                return p;
            };
            m_rightHindLeg  = leg("right_hind_leg", -3.0f, 7.0f);
            m_leftHindLeg   = leg("left_hind_leg",   3.0f, 7.0f);
            m_rightFrontLeg = leg("right_front_leg", -3.0f, -5.0f);
            m_leftFrontLeg  = leg("left_front_leg",   3.0f, -5.0f);
        } else {
            BuildBodyMesh(12, false, true, 0.0f);

            m_head->pose = PartPose::Offset(0.0f, 6.0f, -8.0f);
            m_head->cubes.clear();
            AddBox(m_head, 0, 0, -3.0f, -4.0f, -6.0f, 6.0f, 6.0f, 8.0f);

            m_body->pose = PartPose::OffsetAndRotation(0.0f, 5.0f, 2.0f, kPi / 2.0f, 0.0f, 0.0f);
            m_body->cubes.clear();
            AddBox(m_body, 28, 8, -4.0f, -10.0f, -7.0f, 8.0f, 16.0f, 6.0f);
        }

        m_root.ResetPose();
    }

    bool SheepModel::BecomeBaby() {
        // SheepModel.BABY_TRANSFORMER (SheepModel.java:17). MC applies the
        // same transform to the wool layer (LayerDefinitions'
        // SHEEP_BABY_WOOL), so this serves the fur instance unchanged.
        ApplyBabyTransform(m_root, false, 8.0f, 4.0f, 2.0f, 2.0f, 24.0f, {"head"});
        return true;
    }

    void SheepModel::SetupAnim(const EntityRenderState& state) {
        QuadrupedModel::SetupAnim(state);

        // MC SheepModel.setupAnim: the head drops toward the ground while
        // grazing. 9 pixels is a little over half a block, which puts the
        // muzzle on the grass.
        // MC scales the drop by ageScale, so a lamb's head does not sink
        // through its own body.
        m_head->y += state.headEatPositionScale * 9.0f * state.ageScale;
        m_head->xRot = state.headEatAngleScale;
    }

    // ── CreeperModel ───────────────────────────────────────────────────────

    CreeperModel::CreeperModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        // MC CreeperModel: upright body, but four short legs animated exactly
        // like a quadruped's — which is why it inherits QuadrupedModel here.
        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 6.0f, 0.0f));
        AddBox(m_head, 0, 0, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f);

        m_body = m_root.AddChild("body", PartPose::Offset(0.0f, 6.0f, 0.0f));
        AddBox(m_body, 16, 16, -4.0f, 0.0f, -2.0f, 8.0f, 12.0f, 4.0f);

        const auto leg = [&](const char* name, float x, float z) {
            ModelPart* p = m_root.AddChild(name, PartPose::Offset(x, 18.0f, z));
            AddBox(p, 0, 16, -2.0f, 0.0f, -2.0f, 4.0f, 6.0f, 4.0f);
            return p;
        };
        m_rightHindLeg  = leg("right_hind_leg", -2.0f, 4.0f);
        m_leftHindLeg   = leg("left_hind_leg",   2.0f, 4.0f);
        m_rightFrontLeg = leg("right_front_leg", -2.0f, -4.0f);
        m_leftFrontLeg  = leg("left_front_leg",   2.0f, -4.0f);

        m_root.ResetPose();
    }

    // ── SpiderModel ────────────────────────────────────────────────────────

    SpiderModel::SpiderModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 15.0f, -3.0f));
        AddBox(m_head, 32, 4, -4.0f, -4.0f, -8.0f, 8.0f, 8.0f, 8.0f);

        ModelPart* body0 = m_root.AddChild("body0", PartPose::Offset(0.0f, 15.0f, 0.0f));
        AddBox(body0, 0, 0, -3.0f, -3.0f, -3.0f, 6.0f, 6.0f, 6.0f);

        ModelPart* body1 = m_root.AddChild("body1", PartPose::Offset(0.0f, 15.0f, 9.0f));
        AddBox(body1, 0, 12, -5.0f, -4.0f, -6.0f, 10.0f, 8.0f, 12.0f);

        // The eight legs. Right legs extend in -X from their anchor and left
        // legs in +X, which is why their box origins differ rather than being
        // mirrored positions of the same box.
        const float kQuarter = kPi / 4.0f;
        const float kEighth  = kPi / 8.0f;
        const float kMid     = 0.58119464f;   // MC's literal

        struct LegDef { const char* name; float x, z, yRot, zRot; bool left; };
        const LegDef legs[8] = {
            { "right_hind_leg",         -4.0f,  2.0f,  kQuarter, -kQuarter, false },
            { "left_hind_leg",           4.0f,  2.0f, -kQuarter,  kQuarter, true  },
            { "right_middle_hind_leg",  -4.0f,  1.0f,  kEighth,  -kMid,     false },
            { "left_middle_hind_leg",    4.0f,  1.0f, -kEighth,   kMid,     true  },
            { "right_middle_front_leg", -4.0f,  0.0f, -kEighth,  -kMid,     false },
            { "left_middle_front_leg",   4.0f,  0.0f,  kEighth,   kMid,     true  },
            { "right_front_leg",        -4.0f, -1.0f, -kQuarter, -kQuarter, false },
            { "left_front_leg",          4.0f, -1.0f,  kQuarter,  kQuarter, true  },
        };

        for (int i = 0; i < 8; ++i) {
            const LegDef& d = legs[i];
            m_legs[i] = m_root.AddChild(d.name,
                PartPose::OffsetAndRotation(d.x, 15.0f, d.z, 0.0f, d.yRot, d.zRot));
            AddBox(m_legs[i], 18, 0,
                   d.left ? -1.0f : -15.0f, -1.0f, -1.0f, 16.0f, 2.0f, 2.0f,
                   0.0f, d.left);
        }

        m_root.ResetPose();
    }

    void SpiderModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();

        m_head->xRot = state.xRot * kDegToRad;
        m_head->yRot = state.yRot * kDegToRad;

        const float pos = state.walkAnimationPos * kSwingFreq;
        const float speed = state.walkAnimationSpeed;

        // Four phase groups, a quarter cycle apart, so the eight legs move in
        // the alternating tetrapod gait MC uses. `swing` sweeps the leg
        // forwards and back; `step` lifts it — the abs() is what makes the lift
        // one-directional rather than dipping below the body.
        const float phase[4] = { 0.0f, kPi, kPi / 2.0f, kPi * 1.5f };
        float swing[4], step[4];
        for (int i = 0; i < 4; ++i) {
            swing[i] = -(std::cos(pos * 2.0f + phase[i]) * 0.4f) * speed;
            step[i]  = std::abs(std::sin(pos + phase[i]) * 0.4f) * speed;
        }

        // Leg index -> phase group: hind, middle-hind, middle-front, front.
        const int group[8] = { 0, 0, 1, 1, 2, 2, 3, 3 };
        for (int i = 0; i < 8; ++i) {
            const bool left = (i % 2) == 1;
            const float sign = left ? -1.0f : 1.0f;
            m_legs[i]->yRot += sign * swing[group[i]];
            m_legs[i]->zRot += sign * step[group[i]];
        }
    }

    // ── ChickenModel ───────────────────────────────────────────────────────

    ChickenModel::ChickenModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 15.0f, -4.0f));
        AddBox(m_head, 0, 0, -2.0f, -6.0f, -2.0f, 4.0f, 6.0f, 3.0f);

        ModelPart* beak = m_head->AddChild("beak", PartPose::Zero());
        AddBox(beak, 14, 0, -2.0f, -4.0f, -4.0f, 4.0f, 2.0f, 2.0f);

        ModelPart* wattle = m_head->AddChild("red_thing", PartPose::Zero());
        AddBox(wattle, 14, 4, -1.0f, -2.0f, -3.0f, 2.0f, 2.0f, 2.0f);

        ModelPart* body = m_root.AddChild("body",
            PartPose::OffsetAndRotation(0.0f, 16.0f, 0.0f, kPi / 2.0f, 0.0f, 0.0f));
        AddBox(body, 0, 9, -3.0f, -4.0f, -3.0f, 6.0f, 8.0f, 6.0f);

        m_rightLeg = m_root.AddChild("right_leg", PartPose::Offset(-2.0f, 19.0f, 1.0f));
        AddBox(m_rightLeg, 26, 0, -1.0f, 0.0f, -3.0f, 3.0f, 5.0f, 3.0f);

        m_leftLeg = m_root.AddChild("left_leg", PartPose::Offset(1.0f, 19.0f, 1.0f));
        AddBox(m_leftLeg, 26, 0, -1.0f, 0.0f, -3.0f, 3.0f, 5.0f, 3.0f);

        m_rightWing = m_root.AddChild("right_wing", PartPose::Offset(-4.0f, 13.0f, 0.0f));
        AddBox(m_rightWing, 24, 13, 0.0f, 0.0f, -3.0f, 1.0f, 4.0f, 6.0f);

        m_leftWing = m_root.AddChild("left_wing", PartPose::Offset(4.0f, 13.0f, 0.0f));
        AddBox(m_leftWing, 24, 13, -1.0f, 0.0f, -3.0f, 1.0f, 4.0f, 6.0f);

        m_root.ResetPose();
    }

    bool ChickenModel::BecomeBaby() {
        // ChickenModel.BABY_TRANSFORMER (ChickenModel.java:19). Its headParts
        // set also names beak/red_thing, but MC nests both under the head
        // (createBodyLayer:44-45) and the transform walks root children only,
        // so those two entries are no-ops there and here alike.
        ApplyBabyTransform(m_root, false, 5.0f, 2.0f, 2.0f, 1.99f, 24.0f, {"head"});
        return true;
    }

    void ChickenModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();

        m_head->xRot = state.xRot * kDegToRad;
        m_head->yRot = state.yRot * kDegToRad;

        const float pos = state.walkAnimationPos;
        const float speed = state.walkAnimationSpeed;

        m_rightLeg->xRot = std::cos(pos * kSwingFreq) * 1.4f * speed;
        m_leftLeg->xRot  = std::cos(pos * kSwingFreq + kPi) * 1.4f * speed;

        // `flap` accumulates without bound (it is a phase, not an angle), so
        // the sine wraps it; +1 keeps the wings from folding through the body.
        const float flapAngle = (std::sin(state.flap) + 1.0f) * state.flapSpeed;
        m_rightWing->zRot =  flapAngle;
        m_leftWing->zRot  = -flapAngle;
    }

    // ── GeneratedModel ─────────────────────────────────────────────────────

    GeneratedModel::GeneratedModel(std::string_view slug,
                                   std::string_view animSlugOverride) {
        const GenModel* gm = FindGenModel(slug);
        if (!gm) {
            Log::Warning("[GeneratedModel] no mesh for '%s'", std::string(slug).c_str());
            return;
        }

        m_texWidth  = gm->texWidth;
        m_texHeight = gm->texHeight;

        // Parts are emitted parent-before-child, so one pass suffices and a
        // child's parent pointer is always already built.
        std::vector<ModelPart*> built(static_cast<size_t>(gm->partCount), nullptr);
        for (int i = 0; i < gm->partCount; ++i) {
            const GenPart& gp = kGenParts[gm->firstPart + i];
            ModelPart* parent = (gp.parent >= 0 && gp.parent < i)
                ? built[static_cast<size_t>(gp.parent)] : &m_root;
            if (!parent) parent = &m_root;

            PartPose pose =
                PartPose::OffsetAndRotation(gp.x, gp.y, gp.z, gp.xRot, gp.yRot, gp.zRot);
            // The LayerDefinitions mesh scale rides the root part's pose,
            // exactly as MC's MeshTransformer.scaling leaves it.
            pose.xScale = gp.xScale;
            pose.yScale = gp.yScale;
            pose.zScale = gp.zScale;
            ModelPart* p = parent->AddChild(std::string(gp.name), pose);
            // MC's setupAnim hides some parts in the default state — see
            // HIDDEN_PARTS in the generator. Nothing here ever shows them
            // again, because the state that would (rolled up, carrying a
            // chest, gravid, croaking) is not modelled.
            p->visible = gp.visible;
            for (int c = 0; c < gp.cubeCount; ++c) {
                const GenCube& gc = kGenCubes[gp.firstCube + c];
                p->cubes.push_back(CubeDefinition{
                    gc.ox, gc.oy, gc.oz, gc.sx, gc.sy, gc.sz,
                    gc.tu, gc.tv, gc.growX, gc.growY, gc.growZ, gc.mirror });
            }
            built[static_cast<size_t>(i)] = p;
        }

        // A baby mesh is animated by the ADULT's compiled program — MC's
        // AgeableMobRenderer swaps meshes, never setupAnim, and the
        // BabyModelTransform keeps every part name. The generated row already
        // carries the adult's clips; only the program lookup needs the base
        // slug.
        std::string_view animSlug = animSlugOverride.empty() ? slug
                                                             : animSlugOverride;
        constexpr std::string_view kBabySuffix = "_baby";
        if (animSlugOverride.empty() &&
            animSlug.size() > kBabySuffix.size() &&
            animSlug.substr(animSlug.size() - kBabySuffix.size()) == kBabySuffix) {
            animSlug.remove_suffix(kBabySuffix.size());
        }

        // MC's setupAnim turns a head only in SOME models, and the part is
        // not always called "head". Both facts come from the generator, read
        // straight out of the model class rather than assumed from the name —
        // a frog and a breeze keep their heads rigid in MC, and turning one
        // shears it off the body. The bat turns its head only while resting,
        // which is what the guard carries.
        if (!gm->headPart.empty()) {
            m_head = (gm->headPart == "root") ? &m_root
                                              : m_root.Find(std::string(gm->headPart));
            m_headGuard = gm->headGuard;
            m_headGuardNegate = gm->headGuardNegate;
        }

        // MC's own setupAnim. Covers the head turn (with MC's clamps), the limb
        // swing, and everything else the model writes by hand.
        if (const AnimProgram* prog = FindAnimProgram(animSlug)) {
            m_setup = SetupAnimProgram::Bake(m_root, *prog);
            // Only drop the plain head turn when the program actually poses the
            // head. A program that compiled some statements but not the head
            // one — the bat's, whose head turn is behind an unsupported
            // condition — still needs it.
            if (m_setup.Valid() && prog->writesHead) m_head = nullptr;
        }

        // MC's KeyframeAnimation clips, in setupAnim's own order.
        for (int i = 0; i < gm->clipCount; ++i) {
            const GenClip& gc = kGenClips[gm->firstClip + i];
            const GenAnim* anim = FindGenAnim(gc.anim);
            if (!anim) {
                Log::Warning("[GeneratedModel] '%s': no animation data for '%s'",
                             std::string(slug).c_str(), std::string(gc.anim).c_str());
                continue;
            }
            m_clips.push_back(Clip{ KeyframeAnimation::Bake(m_root, *anim), &gc });
            if (gc.isWalk) m_hasWalkClip = true;
        }

        for (int i = 0; i < gm->visCount; ++i) {
            const GenClipVisibility& gv = kGenClipVis[gm->firstVis + i];
            if (ModelPart* p = m_root.Find(std::string(gv.part))) {
                m_visRules.push_back(VisRule{ p, gv.animSlot });
            }
        }

        // Root-to-"right_arm" chain for RightHandMatrix — resolved once so a
        // held item is a couple of matrix multiplies per frame. The DFS is
        // depth-first exactly like ModelPart::Find, so both agree on WHICH
        // right_arm when a mesh nests one (none do today).
        {
            std::vector<const ModelPart*> chain;
            const std::function<bool(const ModelPart&)> dfs =
                [&](const ModelPart& part) -> bool {
                    chain.push_back(&part);
                    if (part.name == "right_arm") return true;
                    for (const auto& child : part.children) {
                        if (dfs(*child)) return true;
                    }
                    chain.pop_back();
                    return false;
                };
            if (dfs(m_root)) m_rightArmChain = std::move(chain);

            // MC SkeletonModel.translateToHand shoves the right arm one pixel
            // outward; every other armed model uses the plain chain.
            if (slug == "stray" || slug == "bogged" || slug == "parched"
                || slug == "wither_skeleton") {
                m_handOffset = glm::vec3(1.0f, 0.0f, 0.0f);
            }
        }

        // The shared limb swing is the fallback for a model that has neither a
        // compiled program nor a walk clip — i.e. one whose animation this port
        // could not read at all.
        if (m_setup.Valid() || m_hasWalkClip) return;

        for (int i = 0; i < gm->partCount; ++i) {
            const std::string name(kGenParts[gm->firstPart + i].name);
            ModelPart* p = built[static_cast<size_t>(i)];
            if (!p || p == m_head) continue;

            const auto has = [&](const char* s) {
                return name.find(s) != std::string::npos;
            };

            if (has("head")) continue;

            // MC swings a limb by cos(pos * 0.6662 + phase). The phase is PI
            // for the diagonal pair, which is what makes a quadruped trot
            // rather than hop.
            const bool right = has("right");
            const bool front = has("front") || has("fore");
            float amp = 0.0f;
            if (has("leg") || has("haunch") || has("foot") || has("arm")) {
                amp = has("arm") ? 1.0f : 1.4f;
            } else if (has("wing") || has("tail") || has("fin")) {
                amp = 0.6f;
            }
            if (amp == 0.0f) continue;

            const bool flip = right ^ front;
            m_animated.push_back({ p, flip ? kPi : 0.0f, amp });
        }
    }

    namespace {
        // A GenClip / GenModel guard against the render state.
        bool GuardHolds(AnimGuard g, bool negate, const EntityRenderState& s) {
            bool v = true;
            switch (g) {
                case AnimGuard::None:          return true;
                case AnimGuard::IsInWater:     v = s.isInWater; break;
                case AnimGuard::IsSearching:   v = s.isSearching; break;
                case AnimGuard::CanMove:       v = s.canMove; break;
                case AnimGuard::IsResting:     v = s.isResting; break;
                case AnimGuard::IsHoldingItem: v = s.isHoldingItem; break;
            }
            return negate ? !v : v;
        }
    } // namespace

    bool GeneratedModel::RightHandMatrix(glm::mat4& out) const {
        if (m_rightArmChain.empty()) return false;
        // MC ArmedModel.translateToHand: root.translateAndRotate, then every
        // part down to the arm. The shove (skeleton family's ±1 pixel) is
        // folded into the ARM's own matrix, exactly like the hand-written
        // SkeletonModel::RightHandMatrix.
        out = glm::mat4(1.0f);
        for (size_t i = 0; i < m_rightArmChain.size(); ++i) {
            const bool last = (i + 1 == m_rightArmChain.size());
            out *= m_rightArmChain[i]->LocalMatrix(last ? m_handOffset
                                                        : glm::vec3(0.0f));
        }
        return true;
    }

    void GeneratedModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();

        // Head first: MC's keyframe channels ADD to whatever the pose holds, so
        // a mob that both turns its head and plays an animation composes the
        // two, exactly as ArmadilloModel.setupAnim does.
        if (m_head && GuardHolds(m_headGuard, m_headGuardNegate, state)) {
            // ADD to the rest pose, don't replace it. Most models rest at zero
            // so this is identical to MC's assignment, but AbstractEquineModel
            // rests its head_parts at PI/6 and writes `PI/6 + headRotXRad` —
            // overwriting would snap every horse, donkey and mule's head flat.
            m_head->xRot = m_head->pose.xRot + state.xRot * kDegToRad;
            m_head->yRot = m_head->pose.yRot + state.yRot * kDegToRad;
        }

        if (m_setup.Valid()) m_setup.Run(state);

        for (const Clip& c : m_clips) {
            if (!GuardHolds(c.def->guard, c.def->guardNegate, state)) continue;

            if (c.def->isWalk) {
                // NautilusModel is the only model that does not pass the walk
                // values straight through; posAgeScale and speedBias are zero
                // for every other clip, so this is MC's call for all of them.
                c.anim.ApplyWalk(
                    state.walkAnimationPos + state.ageInTicks * c.def->posAgeScale,
                    state.walkAnimationSpeed + c.def->speedBias,
                    c.def->speedFactor, c.def->scaleFactor);
                continue;
            }

            // MC KeyframeAnimation.apply(AnimationState, ageInTicks) is
            // `state.ifStarted(...)` — a stopped timer plays nothing at all,
            // which is what leaves the mob in its rest pose between clips.
            const int slot = c.def->animSlot;
            if (!state.AnimRunning(slot)) continue;
            c.anim.Apply(state.AnimSeconds(slot), 1.0f);
        }

        for (const VisRule& v : m_visRules) {
            v.part->visible = state.AnimRunning(v.slot);
        }

        for (const Animated& a : m_animated) {
            a.part->xRot += std::cos(state.walkAnimationPos * kSwingFreq + a.phase) *
                            a.amplitude * state.walkAnimationSpeed;
        }
    }

    // ── PufferfishModel (mid/big puff stages) ──────────────────────────────

    PufferfishModel::PufferfishModel(std::string_view slug)
        // Pass the SMALL puffer's program as animSlug: it targets
        // right_fin/left_fin, which these meshes do not have, so it binds to
        // nothing — but a VALID program suppresses the generic limb-swing
        // fallback, which would otherwise wag every "*_fin" part on the walk
        // clock (MC's mid/big fins flap on ageInTicks, not on distance).
        : GeneratedModel(slug, "pufferfish") {
        m_leftBlueFin = m_root.Find("left_blue_fin");
        m_rightBlueFin = m_root.Find("right_blue_fin");
    }

    void PufferfishModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        // MC PufferfishMidModel.setupAnim / PufferfishBigModel.setupAnim —
        // identical two statements in both classes.
        if (m_rightBlueFin) {
            m_rightBlueFin->zRot =
                -0.2f + 0.4f * std::sin(state.ageInTicks * 0.2f);
        }
        if (m_leftBlueFin) {
            m_leftBlueFin->zRot =
                0.2f - 0.4f * std::sin(state.ageInTicks * 0.2f);
        }
    }

    // ── ArrowModel ─────────────────────────────────────────────────────────

    ArrowModel::ArrowModel() {
        m_texWidth = 32.0f;
        m_texHeight = 32.0f;

        // MC arrow units are 0.05625 blocks; the model pipeline is 1/16
        // blocks per pixel, so one arrow unit is 0.9 model pixels.
        constexpr float u = 0.9f;

        // Two nested parts because ModelPart's rotation order is Z-Y-X with X
        // innermost: the pitch (X rotation, set per frame) must wrap the fixed
        // 90-degree yaw that turns the +X shaft into the renderer's -Z
        // "forward", or it would roll the shaft instead of tilting it.
        m_pivot = m_root.AddChild("pivot", PartPose::Offset(0.0f, 22.0f, 0.0f));
        ModelPart* yaw = m_pivot->AddChild("yaw",
            PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f,
                                        0.0f, kPi * 0.5f, 0.0f));

        // Each flat plane needs a flipped twin: a zero-thickness box maps only
        // one face onto the artwork, and the twin (rotated a half-turn) shows
        // the same texels from the other side.
        const auto flatPair = [&](const char* name, float rotAxisX,
                                  float texX, float texY,
                                  float ox, float oy, float oz,
                                  float sx, float sy, float sz) {
            ModelPart* front = yaw->AddChild(name, PartPose::Zero());
            AddBox(front, texX, texY, ox, oy, oz, sx, sy, sz);
            ModelPart* back = yaw->AddChild(std::string(name) + "_back",
                PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f,
                                            rotAxisX ? kPi : 0.0f, 0.0f,
                                            rotAxisX ? 0.0f : kPi));
            AddBox(back, texX, texY, ox, oy, oz, sx, sy, sz);
        };

        // Horizontal shaft plane: 16x5 units in XZ. texOffs (-4.5, 5) lands
        // the box's TOP face on the shaft strip at (0,5)-(16,10).
        flatPair("shaft_h", 1.0f, -4.5f, 5.0f,
                 -7.0f * u, 0.0f, -2.5f * u, 16.0f * u, 0.0f, 5.0f * u);

        // Vertical shaft plane: 16x5 units in XY. texOffs (0,5) lands the
        // NORTH face on the same strip.
        flatPair("shaft_v", 0.0f, 0.0f, 5.0f,
                 -7.0f * u, -2.5f * u, 0.0f, 16.0f * u, 5.0f * u, 0.0f);

        // Tail cross (the fletching): a 5x5-unit YZ plane at the tail. A
        // zero-WIDTH box's east/west faces are the D x H side pair; texOffs
        // (0, -4.5) puts the west face on the 5x5 fletching art at (0,0).
        flatPair("back", 0.0f, 0.0f, -4.5f,
                 -7.0f * u, -2.5f * u, -2.5f * u, 0.0f, 5.0f * u, 5.0f * u);

        m_root.ResetPose();
    }

    void ArrowModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        // The arrow's xRot is atan2(vy, horizontal) — positive climbing.
        // Model pitch is positive nose-down, hence the sign flip.
        m_pivot->zRot = 0.0f;
        m_pivot->xRot = -state.xRot * kDegToRad;
    }

    // ── EvokerFangsModel ───────────────────────────────────────────────────

    EvokerFangsModel::EvokerFangsModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        m_base = m_root.AddChild("base", PartPose::Offset(-5.0f, 24.0f, -5.0f));
        AddBox(m_base, 0, 0, 0.0f, 0.0f, 0.0f, 10.0f, 12.0f, 10.0f);

        // Both jaws share one 4x14x8 plate; the lower one is yawed a half
        // turn so the two blades face each other.
        m_upperJaw = m_base->AddChild("upper_jaw",
            PartPose::OffsetAndRotation(6.5f, 0.0f, 1.0f, 0.0f, 0.0f, 2.042035f));
        AddBox(m_upperJaw, 40, 0, 0.0f, 0.0f, 0.0f, 4.0f, 14.0f, 8.0f);
        m_lowerJaw = m_base->AddChild("lower_jaw",
            PartPose::OffsetAndRotation(3.5f, 0.0f, 9.0f, 0.0f, kPi, 4.2411504f));
        AddBox(m_lowerJaw, 40, 0, 0.0f, 0.0f, 0.0f, 4.0f, 14.0f, 8.0f);
    }

    void EvokerFangsModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        const float biteProgress = state.biteProgress;
        float biteAmount = std::min(biteProgress * 2.0f, 1.0f);
        biteAmount = 1.0f - biteAmount * biteAmount * biteAmount;
        m_upperJaw->zRot = kPi - biteAmount * 0.35f * kPi;
        m_lowerJaw->zRot = kPi + biteAmount * 0.35f * kPi;
        m_base->y -= (biteProgress + std::sin(biteProgress * 2.7f)) * 7.2f;

        float preScale = 1.0f;
        if (biteProgress > 0.9f) {
            preScale *= (1.0f - biteProgress) / 0.1f;
        }
        m_root.y = 24.0f - 20.0f * preScale;
        m_root.xScale = preScale;
        m_root.yScale = preScale;
        m_root.zScale = preScale;
    }

    // ── TridentModel ───────────────────────────────────────────────────────

    TridentModel::TridentModel() {
        m_texWidth = 32.0f;
        m_texHeight = 32.0f;

        // Same nesting as ArrowModel: the pivot carries the per-frame pitch,
        // the yaw child turns +X into the renderer's forward, and the orient
        // child rolls the vertical trident (spikes at -Y) point-first onto
        // +X. The trident geometry spans y -4..27, so the parts sit at -11.5
        // to centre it on the pivot; the pivot itself sits at the entity's
        // half-height (0.5 blocks tall -> centre 4 px above ground = y 20).
        m_pivot = m_root.AddChild("pivot", PartPose::Offset(0.0f, 20.0f, 0.0f));
        ModelPart* yaw = m_pivot->AddChild("yaw",
            PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f,
                                        0.0f, kPi * 0.5f, 0.0f));
        ModelPart* orient = yaw->AddChild("orient",
            PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f,
                                        0.0f, 0.0f, kPi * 0.5f));
        ModelPart* pole = orient->AddChild("pole",
                                           PartPose::Offset(0.0f, -11.5f, 0.0f));

        // MC TridentModel.createLayer, box for box.
        AddBox(pole, 0.0f, 6.0f, -0.5f, 2.0f, -0.5f, 1.0f, 25.0f, 1.0f);
        AddBox(pole, 4.0f, 0.0f, -1.5f, 0.0f, -0.5f, 3.0f, 2.0f, 1.0f);
        AddBox(pole, 4.0f, 3.0f, -2.5f, -3.0f, -0.5f, 1.0f, 4.0f, 1.0f);
        AddBox(pole, 0.0f, 0.0f, -0.5f, -4.0f, -0.5f, 1.0f, 4.0f, 1.0f);
        AddBox(pole, 4.0f, 3.0f, 1.5f, -3.0f, -0.5f, 1.0f, 4.0f, 1.0f,
               0.0f, /*mirror=*/true);

        m_root.ResetPose();
    }

    void TridentModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        m_pivot->xRot = -state.xRot * kDegToRad;
    }

    // ── WitherSkullModel ───────────────────────────────────────────────────

    WitherSkullModel::WitherSkullModel() {
        m_texWidth = 64.0f;
        m_texHeight = 64.0f;

        // The 0.3125-block skull's centre sits 2.5 px above ground; the box
        // spans y -8..0, so the part sits 4 px lower still.
        m_head = m_root.AddChild("head", PartPose::Offset(0.0f, 25.5f, 0.0f));
        AddBox(m_head, 0.0f, 35.0f, -4.0f, -8.0f, -4.0f, 8.0f, 8.0f, 8.0f);
        m_root.ResetPose();
    }

    void WitherSkullModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        // The renderer's body yaw already turns the skull; only the pitch is
        // the model's (MC SkullModel.setupAnim xRot).
        m_head->xRot = -state.xRot * kDegToRad;
    }

    // ── ShulkerBulletModel ─────────────────────────────────────────────────

    ShulkerBulletModel::ShulkerBulletModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        m_main = m_root.AddChild("main", PartPose::Offset(0.0f, 21.5f, 0.0f));
        AddBox(m_main,  0.0f,  0.0f, -4.0f, -4.0f, -1.0f, 8.0f, 8.0f, 2.0f);
        AddBox(m_main,  0.0f, 10.0f, -1.0f, -4.0f, -4.0f, 2.0f, 8.0f, 8.0f);
        AddBox(m_main, 20.0f,  0.0f, -4.0f, -1.0f, -4.0f, 8.0f, 2.0f, 8.0f);
        m_root.ResetPose();
    }

    void ShulkerBulletModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        // MC ShulkerBulletRenderer's tumble: yaw sin(t*0.1)*180, pitch
        // cos(t*0.1)*180, roll sin(t*0.15)*360, at a net 0.75 scale
        // (scale(-0.5,-0.5,0.5) * 1.5 — the flip is the renderer's usual -1).
        const float t = state.ageInTicks;
        m_main->yRot = std::sin(t * 0.1f) * kPi;
        m_main->xRot = std::cos(t * 0.1f) * kPi;
        m_main->zRot = std::sin(t * 0.15f) * 2.0f * kPi;
        m_main->xScale = m_main->yScale = m_main->zScale = 0.75f;
    }

    // ── LlamaSpitModel ─────────────────────────────────────────────────────

    LlamaSpitModel::LlamaSpitModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        // The seven 2x2x2 cubes centre on (1, 1, 1); the inner offset recentres
        // them on the pivot at the 0.25-block entity's half height (y 22).
        m_pivot = m_root.AddChild("pivot", PartPose::Offset(0.0f, 22.0f, 0.0f));
        ModelPart* main = m_pivot->AddChild("main",
                                            PartPose::Offset(-1.0f, -1.0f, -1.0f));
        AddBox(main, 0.0f, 0.0f, -4.0f,  0.0f,  0.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  0.0f, -4.0f,  0.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  0.0f,  0.0f, -4.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  2.0f,  0.0f,  0.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  0.0f,  2.0f,  0.0f, 2.0f, 2.0f, 2.0f);
        AddBox(main, 0.0f, 0.0f,  0.0f,  0.0f,  2.0f, 2.0f, 2.0f, 2.0f);
        m_root.ResetPose();
    }

    void LlamaSpitModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        m_pivot->xRot = -state.xRot * kDegToRad;
    }

    // ── WindChargeModel ────────────────────────────────────────────────────

    WindChargeModel::WindChargeModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        ModelPart* bone = m_root.AddChild("bone",
                                          PartPose::Offset(0.0f, 21.5f, 0.0f));
        m_wind = bone->AddChild("wind",
            PartPose::OffsetAndRotation(0.0f, 0.0f, 0.0f, 0.0f, -0.7854f, 0.0f));
        AddBox(m_wind, 15.0f, 20.0f, -4.0f, -1.0f, -4.0f, 8.0f, 2.0f, 8.0f);
        AddBox(m_wind,  0.0f,  9.0f, -3.0f, -2.0f, -3.0f, 6.0f, 4.0f, 6.0f);
        m_windCharge = bone->AddChild("wind_charge", PartPose::Zero());
        AddBox(m_windCharge, 0.0f, 0.0f, -2.0f, -2.0f, -2.0f, 4.0f, 4.0f, 4.0f);
        m_root.ResetPose();
    }

    void WindChargeModel::SetupAnim(const EntityRenderState& state) {
        m_root.ResetPose();
        // MC WindChargeModel.setupAnim — the core and the shroud counter-spin
        // at 16 degrees per tick. The assignments REPLACE the rest yaw, as
        // MC's do.
        m_windCharge->yRot = -state.ageInTicks * 16.0f * kDegToRad;
        m_wind->yRot = state.ageInTicks * 16.0f * kDegToRad;
    }

    // ── DragonModel ────────────────────────────────────────────────────────

    DragonModel::DragonModel() : GeneratedModel("ender_dragon") {
        m_head = m_root.Find("head");
        m_jaw  = m_head ? m_head->Find("jaw") : nullptr;
        m_body = m_root.Find("body");
        for (int i = 0; i < 5; ++i) {
            m_neck[i] = m_root.Find("neck" + std::to_string(i));
        }
        for (int i = 0; i < 12; ++i) {
            m_tail[i] = m_root.Find("tail" + std::to_string(i));
        }
        if (m_body) {
            m_leftWing     = m_body->Find("left_wing");
            m_leftWingTip  = m_leftWing ? m_leftWing->Find("left_wing_tip") : nullptr;
            m_rightWing    = m_body->Find("right_wing");
            m_rightWingTip = m_rightWing ? m_rightWing->Find("right_wing_tip") : nullptr;

            const char* legs[12] = {
                "left_front_leg", "left_front_leg_tip", "left_front_foot",
                "left_hind_leg",  "left_hind_leg_tip",  "left_hind_foot",
                "right_front_leg", "right_front_leg_tip", "right_front_foot",
                "right_hind_leg",  "right_hind_leg_tip",  "right_hind_foot",
            };
            for (int i = 0; i < 12; ++i) m_leg[i] = m_body->Find(legs[i]);
        }
    }

    void DragonModel::PoseLimbs(float bounce, ModelPart* frontLeg, ModelPart* frontTip,
                                ModelPart* frontFoot, ModelPart* rearLeg,
                                ModelPart* rearTip, ModelPart* rearFoot) {
        if (rearLeg)   rearLeg->xRot   = 1.0f + bounce * 0.1f;
        if (rearTip)   rearTip->xRot   = 0.5f + bounce * 0.1f;
        if (rearFoot)  rearFoot->xRot  = 0.75f + bounce * 0.1f;
        if (frontLeg)  frontLeg->xRot  = 1.3f + bounce * 0.1f;
        if (frontTip)  frontTip->xRot  = -0.5f - bounce * 0.1f;
        if (frontFoot) frontFoot->xRot = 0.75f + bounce * 0.1f;
    }

    namespace {
        // MC Mth.wrapDegrees, local so the dragon math below reads like the
        // Java (Game::Mth is not included here).
        float DragonWrapDegrees(float deg) {
            float r = std::fmod(deg, 360.0f);
            if (r >= 180.0f) r -= 360.0f;
            if (r < -180.0f) r += 360.0f;
            return r;
        }

        // MC EnderDragonRenderState.getHeadPartYOffset.
        float DragonHeadPartYOffset(const EntityRenderState& state, int part,
                                    double bodyY, double partY) {
            double result;
            if (state.dragonIsLandingOrTakingOff) {
                result = static_cast<double>(part) /
                         std::max(state.dragonDistanceToEgg / 4.0, 1.0);
            } else if (state.dragonIsSitting) {
                result = static_cast<double>(part);
            } else if (part == 6) {
                result = 0.0;
            } else {
                result = partY - bodyY;
            }
            return static_cast<float>(result);
        }
    } // namespace

    void DragonModel::SetupAnim(const EntityRenderState& state) {
        // Deliberately NOT GeneratedModel::SetupAnim — the compiled program
        // holds only the fragments that survived the compiler, and half a
        // dragon pose is worse than this whole one.
        m_root.ResetPose();
        if (!m_head || !m_body || !m_neck[0] || !m_tail[0]) return;

        // MC EnderDragonModel.setupAnim: state.flapTime * 2π. Without dragon
        // history (a state built for no real dragon) the hover clock — MC's
        // sitting rate of 0.1/tick — stands in.
        const bool live = state.hasDragonHistory;
        const float flapTime =
            (live ? state.dragonFlapTime : state.ageInTicks * 0.1f) * 2.0f * kPi;

        if (m_jaw) m_jaw->xRot = (std::sin(flapTime) + 1.0f) * 0.2f;

        float bounce = std::sin(flapTime - 1.0f) + 1.0f;
        bounce = (bounce * bounce + bounce * 2.0f) * 0.05f;

        // The root shift is the reason an unposed dragon sat three blocks off
        // its hitbox: MC recentres the whole model every frame.
        m_root.y = (bounce - 2.0f) * 16.0f;
        m_root.z = -48.0f;
        m_root.xRot = bounce * 2.0f * kDegToRad;

        // The history reads. The fallback is a constant history — every yaw
        // and height delta zero — which is MC's own pose for a hover.
        const auto sampleY = [&](int d) { return live ? state.dragonY[d] : 0.0; };
        const auto sampleYRot = [&](int d) {
            return live ? state.dragonYRot[d] : 0.0f;
        };

        // ── Neck chain (MC setupAnim's first loop) ─────────────────────────
        const double startY = sampleY(6);
        const float  startYRot = sampleYRot(6);
        const float rot2 = DragonWrapDegrees(sampleYRot(5) - sampleYRot(10));
        const float rot = DragonWrapDegrees(sampleYRot(5) + rot2 / 2.0f);

        float xx = m_neck[0]->x;
        float yy = m_neck[0]->y;
        float zz = m_neck[0]->z;
        for (int i = 0; i < 5; ++i) {
            ModelPart* neck = m_neck[i];
            if (!neck) continue;
            const double pointY = sampleY(5 - i);
            const float  pointYRot = sampleYRot(5 - i);
            const float neckXRot =
                std::cos(static_cast<float>(i) * 0.45f + flapTime) * 0.15f;
            neck->yRot = DragonWrapDegrees(pointYRot - startYRot) * kDegToRad * 1.5f;
            neck->xRot = neckXRot +
                         DragonHeadPartYOffset(state, i, startY, pointY) *
                             kDegToRad * 1.5f * 5.0f;
            neck->zRot = -DragonWrapDegrees(pointYRot - rot) * kDegToRad * 1.5f;
            neck->x = xx;
            neck->y = yy;
            neck->z = zz;
            xx -= std::sin(neck->yRot) * std::cos(neck->xRot) * 10.0f;
            yy += std::sin(neck->xRot) * 10.0f;
            zz -= std::cos(neck->yRot) * std::cos(neck->xRot) * 10.0f;
        }

        // ── Head (MC: historical sample 0) ─────────────────────────────────
        m_head->x = xx;
        m_head->y = yy;
        m_head->z = zz;
        const double currentY = sampleY(0);
        const float  currentYRot = sampleYRot(0);
        m_head->yRot = DragonWrapDegrees(currentYRot - startYRot) * kDegToRad;
        m_head->xRot =
            DragonWrapDegrees(
                DragonHeadPartYOffset(state, 6, startY, currentY)) *
            kDegToRad * 1.5f * 5.0f;
        m_head->zRot = -DragonWrapDegrees(currentYRot - rot) * kDegToRad;

        // ── Body bank ──────────────────────────────────────────────────────
        m_body->zRot = -rot2 * 1.5f * kDegToRad;

        // ── Wings (exact MC waveform) ──────────────────────────────────────
        if (m_leftWing) {
            m_leftWing->xRot = 0.125f - std::cos(flapTime) * 0.2f;
            m_leftWing->yRot = -0.25f;
            m_leftWing->zRot = -(std::sin(flapTime) + 0.125f) * 0.8f;
        }
        if (m_leftWingTip) {
            m_leftWingTip->zRot = (std::sin(flapTime + 2.0f) + 0.5f) * 0.75f;
        }
        if (m_rightWing && m_leftWing) {
            m_rightWing->xRot = m_leftWing->xRot;
            m_rightWing->yRot = -m_leftWing->yRot;
            m_rightWing->zRot = -m_leftWing->zRot;
        }
        if (m_rightWingTip && m_leftWingTip) {
            m_rightWingTip->zRot = -m_leftWingTip->zRot;
        }

        PoseLimbs(bounce, m_leg[0], m_leg[1], m_leg[2], m_leg[3], m_leg[4], m_leg[5]);
        PoseLimbs(bounce, m_leg[6], m_leg[7], m_leg[8], m_leg[9], m_leg[10], m_leg[11]);

        // ── Tail chain (MC setupAnim's second loop, samples 12..23 against
        //    the sample-11 anchor) ───────────────────────────────────────────
        const double tailStartY = sampleY(11);
        const float  tailStartYRot = sampleYRot(11);
        float tailXRot = 0.0f;
        yy = m_tail[0]->y;
        zz = m_tail[0]->z;
        xx = m_tail[0]->x;
        for (int i = 0; i < 12; ++i) {
            ModelPart* tail = m_tail[i];
            if (!tail) continue;
            const double pointY = sampleY(12 + i);
            const float  pointYRot = sampleYRot(12 + i);
            tailXRot += std::sin(static_cast<float>(i) * 0.45f + flapTime) * 0.05f;
            tail->yRot =
                (DragonWrapDegrees(pointYRot - tailStartYRot) * 1.5f + 180.0f) *
                kDegToRad;
            tail->xRot = tailXRot + static_cast<float>(pointY - tailStartY) *
                                        kDegToRad * 1.5f * 5.0f;
            tail->zRot =
                DragonWrapDegrees(pointYRot - rot) * kDegToRad * 1.5f;
            tail->x = xx;
            tail->y = yy;
            tail->z = zz;
            yy += std::sin(tail->xRot) * 10.0f;
            zz -= std::cos(tail->yRot) * std::cos(tail->xRot) * 10.0f;
            xx -= std::sin(tail->yRot) * std::cos(tail->xRot) * 10.0f;
        }
    }

    // ── EndCrystalModel ────────────────────────────────────────────────────

    float EndCrystalModel::GetY(float ageInTicks) {
        // MC EndCrystalRenderer.getY, verbatim.
        float hh = std::sin(ageInTicks * 0.2f) / 2.0f + 0.5f;
        hh = (hh * hh + hh) * 0.4f;
        return hh - 1.4f;
    }

    EndCrystalModel::EndCrystalModel() {
        m_texWidth = 64.0f;
        m_texHeight = 32.0f;

        // MC EndCrystalModel.createBodyLayer. Each of MC's quaternion
        // rotations — a Y spin composed with the fixed 60° tilt about the
        // (1,0,1)/√2 axis — is expressed as a chain of helper parts, using
        // Ry(45°)·Rx(60°)·Ry(−45°) for the axis-angle. (The tilt's SENSE may
        // mirror MC's; a fixed diagonal tilt spinning at the same rate is
        // visually identical either way.)
        constexpr float kTilt = 60.0f * kDegToRad;
        constexpr float kQuarter = 45.0f * kDegToRad;

        // outer_glass: PartPose.offset(0, 24, 0), spun by the anim.
        m_outerSpin = m_root.AddChild("og_spin", PartPose::Offset(0.0f, 24.0f, 0.0f));
        ModelPart* ogTilt = m_outerSpin->AddChild(
            "og_tilt", PartPose::OffsetAndRotation(0, 0, 0, kTilt, 0, 0));
        ModelPart* ogPost = ogTilt->AddChild(
            "og_post", PartPose::OffsetAndRotation(0, 0, 0, 0, -kQuarter, 0));
        AddBox(ogPost, 0, 0, -4.0f, -4.0f, -4.0f, 8.0f, 8.0f, 8.0f);

        // inner_glass: child of outer, pose scale 0.875, its own spin.
        PartPose innerPose = PartPose::OffsetAndRotation(0, 0, 0, 0, kQuarter, 0);
        innerPose.xScale = innerPose.yScale = innerPose.zScale = 0.875f;
        ModelPart* igPre = ogPost->AddChild("ig_pre", innerPose);
        ModelPart* igTilt = igPre->AddChild(
            "ig_tilt", PartPose::OffsetAndRotation(0, 0, 0, kTilt, 0, 0));
        m_innerSpin = igTilt->AddChild("ig_spin", PartPose::Zero());
        AddBox(m_innerSpin, 0, 0, -4.0f, -4.0f, -4.0f, 8.0f, 8.0f, 8.0f);

        // cube (the core): child of inner, pose scale 0.765625, texOffs(32,0).
        PartPose cubePose = PartPose::OffsetAndRotation(0, 0, 0, 0, kQuarter, 0);
        cubePose.xScale = cubePose.yScale = cubePose.zScale = 0.765625f;
        ModelPart* cPre = m_innerSpin->AddChild("c_pre", cubePose);
        ModelPart* cTilt = cPre->AddChild(
            "c_tilt", PartPose::OffsetAndRotation(0, 0, 0, kTilt, 0, 0));
        m_cubeSpin = cTilt->AddChild("c_spin", PartPose::Zero());
        AddBox(m_cubeSpin, 32, 0, -4.0f, -4.0f, -4.0f, 8.0f, 8.0f, 8.0f);

        // base: the bedrock slab, texOffs(0, 16).
        m_base = m_root.AddChild("base", PartPose::Zero());
        AddBox(m_base, 0, 16, -6.0f, 0.0f, -6.0f, 12.0f, 4.0f, 12.0f);

        m_root.ResetPose();
    }

    void EndCrystalModel::SetupAnim(const EntityRenderState& state) {
        // MC EndCrystalModel.setupAnim: the shells spin at 3°/tick — the
        // outer with the Y bob, the inner and core with their own quaternion
        // phase (the −45° here is the euler chain's closing rotation folded
        // into the spin).
        m_root.ResetPose();
        m_base->visible = state.crystalShowsBottom;

        constexpr float kQuarter = 45.0f * kDegToRad;
        const float spin = state.ageInTicks * 3.0f * kDegToRad;
        const float crystalY = GetY(state.ageInTicks) * 16.0f;

        m_outerSpin->y += crystalY / 2.0f;
        m_outerSpin->yRot = spin + kQuarter;
        m_innerSpin->yRot = spin - kQuarter;
        m_cubeSpin->yRot = spin - kQuarter;
    }

} // namespace Render
