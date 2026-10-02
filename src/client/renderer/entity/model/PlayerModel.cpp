// File: src/client/renderer/entity/model/PlayerModel.cpp
#include "client/renderer/entity/model/PlayerModel.hpp"

#include "common/entity/PlayerModelLayout.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Render {

    namespace {

        constexpr float kPi = 3.14159265358979323846f;

        // MC Mth.rotLerpRad: the short way round.
        float RotLerpRad(float a, float from, float to) {
            float diff = to - from;
            while (diff < -kPi) diff += 2.0f * kPi;
            while (diff >= kPi) diff -= 2.0f * kPi;
            return from + a * diff;
        }

        float Lerp(float a, float from, float to) { return from + a * (to - from); }

        // MC HumanoidModel.quadraticArmUpdate.
        float QuadraticArmUpdate(float x) { return -65.0f * x + x * x; }

        CubeDefinition CubeOf(const Game::PlayerLayout::Box& b) {
            CubeDefinition c{};
            c.originX = b.ox; c.originY = b.oy; c.originZ = b.oz;
            c.sizeX = b.sx;   c.sizeY = b.sy;   c.sizeZ = b.sz;
            c.texOffsX = static_cast<float>(b.texU);
            c.texOffsY = static_cast<float>(b.texV);
            c.growX = c.growY = c.growZ = b.grow;
            c.mirror = false;
            return c;
        }

        // MC ModelPart.rotateBy(q): the part's ZYX Euler rotation composed
        // with q (old · q) and read back as ZYX Euler angles
        // (Matrix3f.getEulerAnglesZYX).
        void RotateBy(ModelPart& part, const glm::mat4& q) {
            glm::mat4 old(1.0f);
            old = glm::rotate(old, part.zRot, glm::vec3(0.0f, 0.0f, 1.0f));
            old = glm::rotate(old, part.yRot, glm::vec3(0.0f, 1.0f, 0.0f));
            old = glm::rotate(old, part.xRot, glm::vec3(1.0f, 0.0f, 0.0f));
            const glm::mat3 m(old * q);
            // glm and JOML both index [column][row].
            part.xRot = std::atan2(m[1][2], m[2][2]);
            part.yRot = std::atan2(-m[0][2], std::sqrt(std::max(0.0f, 1.0f - m[0][2] * m[0][2])));
            part.zRot = std::atan2(m[0][1], m[0][0]);
        }

    } // namespace

    // ── PlayerModel ─────────────────────────────────────────────────────────

    PlayerModel::PlayerModel(bool slim)
        : HumanoidModel(/*slim=*/false), m_slim(slim) {
        // HumanoidModel built the parts and their pivots (MC
        // HumanoidModel.createMesh). PlayerModel.createMesh keeps the parts
        // and replaces the boxes: the player sheet's own left limbs, the
        // slim arms, and the second layer under every part.
        m_texWidth = 64.0f;
        m_texHeight = 64.0f;
        ModelPart* parts[static_cast<int>(Game::PlayerLayout::Part::Count)] = {
            m_head, m_body, m_rightArm, m_leftArm, m_rightLeg, m_leftLeg };
        for (ModelPart* p : parts) p->cubes.clear();
        m_hat->cubes.clear();

        const Game::PlayerLayout::Box* boxes =
            Game::PlayerLayout::Boxes(slim ? Game::SkinModel::Slim : Game::SkinModel::Classic);
        for (size_t i = 0; i < Game::PlayerLayout::kBoxCount; ++i) {
            const Game::PlayerLayout::Box& b = boxes[i];
            ModelPart* part = parts[static_cast<int>(b.part)];
            if (!b.outer) {
                part->cubes.push_back(CubeOf(b));
                continue;
            }
            // The hat already exists as the head's child; the rest are new.
            ModelPart* layer = (b.part == Game::PlayerLayout::Part::Head)
                ? m_hat : part->AddChild(b.name, PartPose::Zero());
            layer->cubes.push_back(CubeOf(b));
            switch (b.part) {
                case Game::PlayerLayout::Part::Body:     m_jacket = layer; break;
                case Game::PlayerLayout::Part::RightArm: m_rightSleeve = layer; break;
                case Game::PlayerLayout::Part::LeftArm:  m_leftSleeve = layer; break;
                case Game::PlayerLayout::Part::RightLeg: m_rightPants = layer; break;
                case Game::PlayerLayout::Part::LeftLeg:  m_leftPants = layer; break;
                default: break;
            }
        }
        m_root.ResetPose();
    }

    void PlayerModel::SetupAnim(const EntityRenderState& state) {
        // MC PlayerModel.setupAnim's toggles first; super.setupAnim's
        // resetPose leaves visibility alone.
        namespace MPB = Game::ModelPartBits;
        const bool showBody = !m_spectator;
        m_body->visible = showBody;
        m_rightArm->visible = showBody;
        m_leftArm->visible = showBody;
        m_rightLeg->visible = showBody;
        m_leftLeg->visible = showBody;
        m_head->visible = true;
        m_hat->visible = (m_modelParts & MPB::Hat) != 0;
        if (m_jacket)      m_jacket->visible      = (m_modelParts & MPB::Jacket) != 0;
        if (m_leftPants)   m_leftPants->visible   = (m_modelParts & MPB::LeftPants) != 0;
        if (m_rightPants)  m_rightPants->visible  = (m_modelParts & MPB::RightPants) != 0;
        if (m_leftSleeve)  m_leftSleeve->visible  = (m_modelParts & MPB::LeftSleeve) != 0;
        if (m_rightSleeve) m_rightSleeve->visible = (m_modelParts & MPB::RightSleeve) != 0;

        HumanoidModel::SetupAnim(state);

        // HumanoidModel.setupAnim's glide head: looking ahead along the
        // flight, whatever the pitch.
        if (state.isFallFlying) m_head->xRot = -0.7853982f;
        if (state.swimAmount > 0.0f) {
            SetupSwimAnimation(state, state.walkAnimationPos, state.swimAmount);
        }
    }

    void PlayerModel::SetupSwimAnimation(const EntityRenderState& state, float animationPos,
                                         float swimAmount) {
        if (!state.isFallFlying) m_head->xRot = RotLerpRad(swimAmount, m_head->xRot, -0.7853982f);

        const float swimPos = std::fmod(animationPos, 26.0f);
        // The arm mid-swing (MC currentSwing's arm) does not take the stroke;
        // nor does an arm in the SPEAR pose.
        const bool swinging = state.attackTime > 0.0f;
        const bool swingRight = swinging && state.mainArm >= 0.5f;
        const bool swingLeft  = swinging && state.mainArm < 0.5f;
        const float rightAmount = (state.rightArmPose != ArmPose::Spear && !swingRight) ? swimAmount : 0.0f;
        const float leftAmount  = (state.leftArmPose  != ArmPose::Spear && !swingLeft)  ? swimAmount : 0.0f;
        if (!state.isUsingItem) {
            if (swimPos < 14.0f) {
                m_leftArm->xRot  = RotLerpRad(leftAmount, m_leftArm->xRot, 0.0f);
                m_rightArm->xRot = Lerp(rightAmount, m_rightArm->xRot, 0.0f);
                m_leftArm->yRot  = RotLerpRad(leftAmount, m_leftArm->yRot, kPi);
                m_rightArm->yRot = Lerp(rightAmount, m_rightArm->yRot, kPi);
                m_leftArm->zRot  = RotLerpRad(leftAmount, m_leftArm->zRot,
                    kPi + 1.8707964f * QuadraticArmUpdate(swimPos) / QuadraticArmUpdate(14.0f));
                m_rightArm->zRot = Lerp(rightAmount, m_rightArm->zRot,
                    kPi - 1.8707964f * QuadraticArmUpdate(swimPos) / QuadraticArmUpdate(14.0f));
            } else if (swimPos < 22.0f) {
                const float t = (swimPos - 14.0f) / 8.0f;
                m_leftArm->xRot  = RotLerpRad(leftAmount, m_leftArm->xRot, 1.5707964f * t);
                m_rightArm->xRot = Lerp(rightAmount, m_rightArm->xRot, 1.5707964f * t);
                m_leftArm->yRot  = RotLerpRad(leftAmount, m_leftArm->yRot, kPi);
                m_rightArm->yRot = Lerp(rightAmount, m_rightArm->yRot, kPi);
                m_leftArm->zRot  = RotLerpRad(leftAmount, m_leftArm->zRot, 5.012389f - 1.8707964f * t);
                m_rightArm->zRot = Lerp(rightAmount, m_rightArm->zRot, 1.2707963f + 1.8707964f * t);
            } else {
                const float t = (swimPos - 22.0f) / 4.0f;
                m_leftArm->xRot  = RotLerpRad(leftAmount, m_leftArm->xRot, 1.5707964f - 1.5707964f * t);
                m_rightArm->xRot = Lerp(rightAmount, m_rightArm->xRot, 1.5707964f - 1.5707964f * t);
                m_leftArm->yRot  = RotLerpRad(leftAmount, m_leftArm->yRot, kPi);
                m_rightArm->yRot = Lerp(rightAmount, m_rightArm->yRot, kPi);
                m_leftArm->zRot  = RotLerpRad(leftAmount, m_leftArm->zRot, kPi);
                m_rightArm->zRot = Lerp(rightAmount, m_rightArm->zRot, kPi);
            }
        }
        m_leftLeg->xRot  = Lerp(swimAmount, m_leftLeg->xRot,
                                0.3f * std::cos(animationPos * 0.33333334f + kPi));
        m_rightLeg->xRot = Lerp(swimAmount, m_rightLeg->xRot,
                                0.3f * std::cos(animationPos * 0.33333334f));
    }

    bool PlayerModel::RightHandMatrix(glm::mat4& out) const {
        if (!m_rightArm) return false;
        const glm::vec3 offset(m_slim ? Game::PlayerLayout::kSlimHandOffset : 0.0f, 0.0f, 0.0f);
        out = m_root.LocalMatrix() * m_rightArm->LocalMatrix(offset);
        return true;
    }

    bool PlayerModel::LeftHandMatrix(glm::mat4& out) const {
        if (!m_leftArm) return false;
        const glm::vec3 offset(m_slim ? -Game::PlayerLayout::kSlimHandOffset : 0.0f, 0.0f, 0.0f);
        out = m_root.LocalMatrix() * m_leftArm->LocalMatrix(offset);
        return true;
    }

    ModelPart& PlayerModel::PrepareFirstPersonArm(bool right, bool sleeve) {
        ModelPart& arm = right ? *m_rightArm : *m_leftArm;
        arm.ResetPose();
        arm.visible = true;
        if (m_leftSleeve)  m_leftSleeve->visible = sleeve;
        if (m_rightSleeve) m_rightSleeve->visible = sleeve;
        m_leftArm->zRot  = -0.1f;
        m_rightArm->zRot =  0.1f;
        return arm;
    }

    // ── PlayerCapeModel ─────────────────────────────────────────────────────

    PlayerCapeModel::PlayerCapeModel() : PlayerModel(/*slim=*/false) {
        // MC createCapeLayer: the player mesh cleared of every box
        // (clearRecursively keeps the parts and their poses), then the cape
        // on the body. The 64x64 layer with texScale 1 x 0.5 samples the
        // cape's 64x32 sheet.
        std::vector<ModelPart*> stack{ &m_root };
        while (!stack.empty()) {
            ModelPart* p = stack.back();
            stack.pop_back();
            p->cubes.clear();
            for (auto& child : p->children) stack.push_back(child.get());
        }
        m_texWidth = 64.0f;
        m_texHeight = 64.0f;
        const Game::PlayerLayout::Box& b = Game::PlayerLayout::kCape;
        m_cape = m_body->AddChild("cape", PartPose::OffsetAndRotation(
            Game::PlayerLayout::kCapeOffset.x, Game::PlayerLayout::kCapeOffset.y,
            Game::PlayerLayout::kCapeOffset.z, 0.0f, Game::PlayerLayout::kCapeYRot, 0.0f));
        CubeDefinition cube = CubeOf(b);
        cube.texScaleU = 1.0f;
        cube.texScaleV = 0.5f;
        m_cape->cubes.push_back(cube);
        m_root.ResetPose();
    }

    void PlayerCapeModel::SetupAnim(const EntityRenderState& state) {
        PlayerModel::SetupAnim(state);
        // The cape layer draws whenever the layer runs; the parts' own
        // visibility (all box-less here) does not matter, but the body that
        // carries the cape must stay visible for a spectator-free body.
        m_body->visible = true;
        // PlayerCapeModel.setupAnim: rotateBy(rotateY(−π) · rotateX(6 +
        // lean/2 + flap) · rotateZ(lean2/2) · rotateY(180 − lean2/2)).
        glm::mat4 q(1.0f);
        q = glm::rotate(q, -kPi, glm::vec3(0.0f, 1.0f, 0.0f));
        q = glm::rotate(q, glm::radians(6.0f + m_capeLean / 2.0f + m_capeFlap), glm::vec3(1.0f, 0.0f, 0.0f));
        q = glm::rotate(q, glm::radians(m_capeLean2 / 2.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        q = glm::rotate(q, glm::radians(180.0f - m_capeLean2 / 2.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        RotateBy(*m_cape, q);
    }

} // namespace Render
