// File: src/client/renderer/entity/model/EntityModels.cpp
#include "client/renderer/entity/model/EntityModels.hpp"
#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <string>

#include <cmath>

namespace Render {

    namespace {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kDegToRad = kPi / 180.0f;

        // MC's limb-swing frequency. Appears in every setupAnim below.
        constexpr float kSwingFreq = 0.6662f;

        // Shorthand for one cube on a part.
        void AddBox(ModelPart* part, float texX, float texY,
                    float ox, float oy, float oz, float sx, float sy, float sz,
                    float grow = 0.0f, bool mirror = false) {
            part->cubes.push_back(CubeDefinition{ ox, oy, oz, sx, sy, sz,
                                                  texX, texY, grow, mirror });
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

    GeneratedModel::GeneratedModel(std::string_view slug) {
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

            ModelPart* p = parent->AddChild(
                std::string(gp.name),
                PartPose::OffsetAndRotation(gp.x, gp.y, gp.z, gp.xRot, gp.yRot, gp.zRot));
            // MC's setupAnim hides some parts in the default state — see
            // HIDDEN_PARTS in the generator. Nothing here ever shows them
            // again, because the state that would (rolled up, carrying a
            // chest, gravid, croaking) is not modelled.
            p->visible = gp.visible;
            for (int c = 0; c < gp.cubeCount; ++c) {
                const GenCube& gc = kGenCubes[gp.firstCube + c];
                p->cubes.push_back(CubeDefinition{
                    gc.ox, gc.oy, gc.oz, gc.sx, gc.sy, gc.sz,
                    gc.tu, gc.tv, gc.grow, gc.mirror });
            }
            built[static_cast<size_t>(i)] = p;
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
        if (const AnimProgram* prog = FindAnimProgram(slug)) {
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

} // namespace Render
