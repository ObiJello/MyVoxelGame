// File: src/client/renderer/entity/AetherCreatureRender.cpp
//
// Render::AetherCreatureRender (ModMobRender.hpp) — every Aether mob: the
// Aether's model classes' setupAnim over the generated meshes, each
// renderer's scale and layers, on in-house sheets
// (tools/gen_aether_textures.py, tools/gen_aether_entity_textures.py — the
// Aether's own art is all rights reserved and is never used).
//
// Render-state slots carrying mod values (each noted at its model):
//   flap        — the WingedBird flap (getBob), the zephyr's tail, the
//                 aechor plant's sinage
//   squish      — the aerbunny's puffiness / 20
//   isSitting   — the moa's sitting flag
//   isOnGround  — the mods' synced "entity on ground" flag
//   isCharging  — the aechor plant's hurtTime > 0
//   isAngry     — the aechor plant's targetingEntity
#include "client/renderer/entity/ModMobRender.hpp"
#include "client/renderer/entity/ModModelHelpers.hpp"

#include "common/entity/mobs/AetherCreatures.hpp"
#include "common/entity/mobs/AetherMobs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace Render::AetherCreatureRender {

    namespace {

        // Static downcast by type id, the MobRenderer::MobAs contract: the
        // factory (Game::MakeAetherMob) builds exactly this class for the
        // listed ids.
        template <typename T, typename... Ids>
        const T* MobAs(const Game::Mob& mob, Game::EntityTypeId type, Ids... ids) {
            if (!((type == ids) || ...)) return nullptr;
            assert(dynamic_cast<const T*>(&mob) != nullptr);
            return static_cast<const T*>(&mob);
        }

        // Every cube of a part tree grown by `by` — a CubeDeformation on a
        // copy of a mesh (the saddle layers' inflated bodies).
        void InflateTree(ModelPart& part, float by) {
            for (CubeDefinition& c : part.cubes) {
                c.growX += by; c.growY += by; c.growZ += by;
            }
            for (auto& child : part.children) InflateTree(*child, by);
        }

        // A generated mesh with no setupAnim at all (AerwhaleModel.setupAnim
        // is empty) — the rest pose every frame.
        class StaticGenModel : public GeneratedModel {
        public:
            explicit StaticGenModel(std::string_view slug) : GeneratedModel(slug) {}
            void SetupAnim(const EntityRenderState& state) override {
                (void)state;
                m_root.ResetPose();
            }
        };

        // MC SlimeModel.createOuterBodyLayer — one 8x8x8 cube at texOffs
        // (0, 0) on a 64x32 sheet: the swets' translucent shell and the
        // sentry's whole body.
        class SlimeOuterModel : public EntityModel {
        public:
            SlimeOuterModel() {
                m_texWidth = 64.0f;
                m_texHeight = 32.0f;
                ModelPart* cube = m_root.AddChild("cube", PartPose::Zero());
                CubeDefinition c{};
                c.originX = -4.0f; c.originY = 16.0f; c.originZ = -4.0f;
                c.sizeX = 8.0f; c.sizeY = 8.0f; c.sizeZ = 8.0f;
                c.texOffsX = 0.0f; c.texOffsY = 0.0f;
                cube->cubes.push_back(c);
                m_root.ResetPose();
            }
            void SetupAnim(const EntityRenderState& state) override {
                (void)state;
                m_root.ResetPose();
            }
        };

        // Aether BipedBirdModel.setupAnim — the cockatrice's and the moa's.
        // CockatriceModel keeps the jaw open (0.35); MoaModel closes it while
        // sitting and hides the legs while sitting on the ground (renderLegs).
        // `state.flap` is the renderer's getBob (setupWingsAnimation),
        // `isOnGround` the synced flag, `isSitting` the moa's.
        class BipedBirdGenModel : public GeneratedModel {
        public:
            BipedBirdGenModel(std::string_view slug, bool moa, float inflate = 0.0f)
                : GeneratedModel(slug), m_moa(moa) {
                if (inflate != 0.0f) InflateTree(m_root, inflate);
                m_head = m_root.Find("head");
                m_jaw = m_root.Find("jaw");
                m_neck = m_root.Find("neck");
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
                    if (m_neck) m_neck->xRot = -m_head->xRot;
                }
                if (!m_rightWing || !m_leftWing || !m_rightLeg || !m_leftLeg) return;
                if (!state.isOnGround) {
                    m_rightWing->x = -3.001f; m_rightWing->y = 0.0f; m_rightWing->z = 4.0f;
                    m_leftWing->x  =  3.001f; m_leftWing->y  = 0.0f; m_leftWing->z  = 4.0f;
                    m_rightWing->xRot = m_leftWing->xRot = -kModPi / 2.0f;
                    m_rightLeg->xRot = m_leftLeg->xRot = 0.6f;
                    m_rightWing->yRot = state.flap;
                } else {
                    // The rest pose already sits at (+-3.001, -3, 3), xRot 0.
                    const float pos = state.walkAnimationPos * 0.6662f;
                    m_rightLeg->xRot = std::cos(pos) * 1.4f * state.walkAnimationSpeed;
                    m_leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * state.walkAnimationSpeed;
                    m_rightWing->yRot = 0.0f;
                }
                m_leftWing->yRot = -m_rightWing->yRot;
                if (m_jaw) m_jaw->xRot = (m_moa && state.isSitting) ? 0.0f : 0.35f;
                if (m_moa) {
                    // MoaModel.prepareMobModel: renderLegs =
                    // !sitting || (!onGround && sitting).
                    const bool legs = !state.isSitting || !state.isOnGround;
                    m_rightLeg->visible = legs;
                    m_leftLeg->visible = legs;
                }
            }

        private:
            bool m_moa;
            ModelPart* m_head = nullptr;
            ModelPart* m_jaw = nullptr;
            ModelPart* m_neck = nullptr;
            ModelPart* m_rightLeg = nullptr;
            ModelPart* m_leftLeg = nullptr;
            ModelPart* m_rightWing = nullptr;
            ModelPart* m_leftWing = nullptr;
        };

        // Aether ZephyrModel.setupAnim. `state.flap` is ZephyrRenderer.getBob
        // (the tail rotation), MC's ageInTicks argument there.
        class ZephyrGenModel : public GeneratedModel {
        public:
            ZephyrGenModel() : GeneratedModel("zephyr") {
                m_rightFace = m_root.Find("right_face");
                m_leftFace = m_root.Find("left_face");
                m_rightFront = m_root.Find("body_right_side_front");
                m_rightBack = m_root.Find("body_right_side_back");
                m_leftFront = m_root.Find("body_left_side_front");
                m_leftBack = m_root.Find("body_left_side_back");
                m_tailBase = m_root.Find("tail_base");
                m_tailMiddle = m_root.Find("tail_middle");
                m_tailEnd = m_root.Find("tail_end");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_rightFace || !m_leftFace || !m_rightFront || !m_rightBack ||
                    !m_leftFront || !m_leftBack || !m_tailBase || !m_tailMiddle || !m_tailEnd) {
                    return;
                }
                constexpr float kRadToDeg = 57.29578f;   // Mth.RAD_TO_DEG
                const float swing = state.walkAnimationPos;
                const float amount = state.walkAnimationSpeed;
                const float motion = std::sin((swing * 20.0f) / kRadToDeg) * amount * 0.5f;

                m_rightFace->y = 8.0f - motion;
                m_rightFace->x = -motion * 0.5f;
                m_leftFace->y = motion + 8.0f;
                m_leftFace->x = motion * 0.5f;

                m_rightFront->y = 8.0f - motion * 0.5f;
                m_rightBack->y = 9.0f + motion * 0.5f;
                m_leftFront->y = m_rightFront->y;
                m_leftBack->y = m_rightBack->y;

                m_tailBase->x = std::sin((swing * 20.0f) / kRadToDeg) * amount * 0.75f;
                m_tailBase->y = 8.0f - motion;
                m_tailBase->yRot = std::sin(state.flap * 0.5f) * amount * 0.75f;

                m_tailMiddle->x = std::sin((swing * 15.0f) / kRadToDeg) * amount * 0.85f;
                m_tailMiddle->y = motion * 1.25f;
                m_tailMiddle->yRot = m_tailBase->yRot + 0.25f;

                m_tailEnd->x = std::sin((swing * 10.0f) / kRadToDeg) * amount * 0.95f;
                m_tailEnd->y = -motion;
                m_tailEnd->yRot = m_tailMiddle->yRot + 0.35f;
            }

        private:
            ModelPart* m_rightFace = nullptr;
            ModelPart* m_leftFace = nullptr;
            ModelPart* m_rightFront = nullptr;
            ModelPart* m_rightBack = nullptr;
            ModelPart* m_leftFront = nullptr;
            ModelPart* m_leftBack = nullptr;
            ModelPart* m_tailBase = nullptr;
            ModelPart* m_tailMiddle = nullptr;
            ModelPart* m_tailEnd = nullptr;
        };

        // Aether QuadrupedWingsModel — the phyg's and flying cow's wing
        // layer (QuadrupedWingsLayer). MC eases wingFold toward 0.1 on the
        // ground / 1.0 aloft by 1/37.5 per frame on the ENTITY; the model is
        // shared per type here, so the fold sits at its target (the eased
        // value converges on it within a couple of seconds). `baby` applies
        // the layer's scale(0.5) + translate(0, 1.5, 0) to every root part.
        class QuadrupedWingsGenModel : public GeneratedModel {
        public:
            QuadrupedWingsGenModel(std::string_view slug, bool baby) : GeneratedModel(slug) {
                m_leftInner = m_root.Find("left_wing_inner");
                m_leftOuter = m_root.Find("left_wing_outer");
                m_rightInner = m_root.Find("right_wing_inner");
                m_rightOuter = m_root.Find("right_wing_outer");
                if (baby) {
                    // S(0.5) * T(0, 24px): offset' = (offset + 24) * 0.5,
                    // pose scale halved (the generator's apply_baby_poses).
                    for (auto& child : m_root.children) {
                        PartPose& p = child->pose;
                        p.x *= 0.5f;
                        p.y = (p.y + 24.0f) * 0.5f;
                        p.z *= 0.5f;
                        p.xScale *= 0.5f;
                        p.yScale *= 0.5f;
                        p.zScale *= 0.5f;
                    }
                    m_root.ResetPose();
                }
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_leftInner || !m_leftOuter || !m_rightInner || !m_rightOuter) return;
                const float fold = state.isOnGround ? 0.1f : 1.0f;
                const float angle = fold * std::sin(state.ageInTicks / 15.9f);
                const float bend = -std::acos(fold);
                m_leftInner->zRot = -(angle + bend + kModPi / 2.0f);
                m_leftOuter->zRot = -(angle - bend + kModPi / 2.0f) - m_leftInner->zRot;
                m_rightInner->zRot = -m_leftInner->zRot;
                m_rightOuter->zRot = -m_leftOuter->zRot;
            }

        private:
            ModelPart* m_leftInner = nullptr;
            ModelPart* m_leftOuter = nullptr;
            ModelPart* m_rightInner = nullptr;
            ModelPart* m_rightOuter = nullptr;
        };

        // Aether AerbunnyModel.setupAnim + renderToBuffer's puff: the head
        // looks, the legs swing against the body's quarter turn, and the puff
        // cube is drawn at scale 1 + puffiness * 0.5 about a point one block
        // down (translate(0, 1, 0) before the scale — the part's y of 16 px).
        // `state.squish` carries the lerped puffiness / 20.
        class AerbunnyGenModel : public GeneratedModel {
        public:
            AerbunnyGenModel() : GeneratedModel("aerbunny") {
                m_head = m_root.Find("head");
                m_body = m_root.Find("body");
                m_puff = m_root.Find("puff");
                m_rightFront = m_root.Find("right_front_leg");
                m_leftFront = m_root.Find("left_front_leg");
                m_rightBack = m_root.Find("right_back_leg");
                m_leftBack = m_root.Find("left_back_leg");
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (m_head) {
                    m_head->xRot = state.xRot * kModDegToRad;
                    m_head->yRot = state.yRot * kModDegToRad;
                }
                const float bodyX = m_body ? m_body->xRot : 0.0f;
                const float pos = state.walkAnimationPos * 0.6662f;
                const float amt = state.walkAnimationSpeed;
                if (m_rightFront) m_rightFront->xRot = std::cos(pos) * 1.0f * amt - bodyX;
                if (m_leftFront)  m_leftFront->xRot  = std::cos(pos) * 1.0f * amt - bodyX;
                if (m_rightBack)  m_rightBack->xRot  = std::cos(pos + kModPi) * 1.2f * amt - bodyX;
                if (m_leftBack)   m_leftBack->xRot   = std::cos(pos + kModPi) * 1.2f * amt - bodyX;
                if (m_puff) {
                    const float a = 1.0f + state.squish * 0.5f;
                    m_puff->y = 16.0f;
                    m_puff->xScale = m_puff->yScale = m_puff->zScale = a;
                }
            }

        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_body = nullptr;
            ModelPart* m_puff = nullptr;
            ModelPart* m_rightFront = nullptr;
            ModelPart* m_leftFront = nullptr;
            ModelPart* m_rightBack = nullptr;
            ModelPart* m_leftBack = nullptr;
        };

        // Aether AechorPlantModel. The mesh builds leaf_1..10 and upper/
        // lower_petal_1..5 in loops (`"leaf_" + i`); the generator reads the
        // loop once as "leaf_" / "upper_petal_" / "lower_petal_", so the parts
        // are cloned here into the full set before posing. setupAnim runs off
        // the renderer's getBob — the sinage (`state.flap`) — with the hurt
        // (`isCharging`) and targeting (`isAngry`) branches.
        class AechorPlantGenModel : public GeneratedModel {
        public:
            AechorPlantGenModel() : GeneratedModel("aechor_plant") {
                m_head = m_root.Find("head");
                m_stem = m_root.Find("stem");
                if (m_stem) {
                    Expand("leaf_", 10, m_leaves);
                    std::vector<ModelPart*> upper, lower;
                    Expand("upper_petal_", 5, upper);
                    Expand("lower_petal_", 5, lower);
                    // petalParts(): upper1, lower1, upper2, lower2, ...
                    for (size_t i = 0; i < upper.size() && i < lower.size(); ++i) {
                        m_petals.push_back(upper[i]);
                        m_petals.push_back(lower[i]);
                    }
                    for (const char* n : { "stamen_stem_1", "stamen_stem_2", "stamen_stem_3" }) {
                        if (ModelPart* p = m_stem->Find(n)) m_stamens.push_back(p);
                    }
                }
                m_root.ResetPose();
            }

            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_head || !m_stem) return;
                const float age = state.flap;
                float sinage1 = std::sin(age);
                float sinage2;
                if (state.isCharging) {
                    sinage1 *= 0.45f;
                    sinage1 -= 0.125f;
                    sinage2 = 1.75f + std::sin(age + 2.0f) * 1.5f;
                } else if (state.isAngry) {
                    sinage1 *= 0.25f;
                    sinage2 = 1.75f + std::sin(age + 2.0f) * 1.5f;
                } else {
                    sinage1 *= 0.125f;
                    sinage2 = 1.75f;
                }
                // head.yRot = headPitch / 57.29578 (the mod's own, verbatim).
                m_head->yRot = state.xRot / 57.29578f;
                m_stem->yRot = m_head->yRot;
                m_stem->y = sinage2 * 0.5f;
                for (size_t i = 0; i < m_stamens.size(); ++i) {
                    ModelPart* p = m_stamens[i];
                    p->xRot = 0.2f + static_cast<float>(i) / 15.0f + sinage1 * 0.4f;
                    p->yRot = m_head->yRot + 0.1f + (2.0f * kModPi / 3.0f) * static_cast<float>(i);
                    p->y = sinage2 + sinage1 * 2.0f;
                }
                for (size_t i = 0; i < m_leaves.size(); ++i) {
                    ModelPart* p = m_leaves[i];
                    p->xRot = ((i % 2 == 0) ? 0.1f : 0.2f) + sinage1 * 0.75f;
                    p->yRot = m_head->yRot + (2.0f * kModPi / 10.0f / 2.0f) +
                              (2.0f * kModPi / 10.0f) * static_cast<float>(i);
                    p->y = sinage2;
                }
                for (size_t i = 0; i < m_petals.size(); ++i) {
                    ModelPart* p = m_petals[i];
                    p->xRot = ((i % 2 == 0) ? -0.25f : -0.4125f) + sinage1;
                    p->yRot = m_head->yRot + (2.0f * kModPi / 10.0f) * static_cast<float>(i);
                    p->y = sinage2;
                }
                m_head->y = sinage2 + sinage1 * 2.0f;
            }

        private:
            // `base` (the generator's single part) becomes base1..baseN, all
            // children of the stem with the same pose and cubes.
            void Expand(const std::string& base, int count, std::vector<ModelPart*>& out) {
                ModelPart* proto = nullptr;
                for (auto& c : m_stem->children) {
                    if (c->name == base) { proto = c.get(); break; }
                }
                if (!proto) {
                    // Already unrolled by a newer generator: collect by name.
                    for (int i = 1; i <= count; ++i) {
                        if (ModelPart* p = m_stem->Find(base + std::to_string(i))) out.push_back(p);
                    }
                    return;
                }
                proto->name = base + "1";
                out.push_back(proto);
                for (int i = 2; i <= count; ++i) {
                    ModelPart* p = m_stem->AddChild(base + std::to_string(i), proto->pose);
                    p->cubes = proto->cubes;
                    out.push_back(p);
                }
            }

            ModelPart* m_head = nullptr;
            ModelPart* m_stem = nullptr;
            std::vector<ModelPart*> m_stamens, m_leaves, m_petals;
        };

        // Aether MimicModel.setupAnim: the lid gapes on a 20-tick cycle, the
        // legs swing.
        class MimicGenModel : public GeneratedModel {
        public:
            MimicGenModel() : GeneratedModel("mimic") {
                m_upper = m_root.Find("upper_body");
                m_rightLeg = m_root.Find("right_leg");
                m_leftLeg = m_root.Find("left_leg");
            }
            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (m_upper) {
                    m_upper->xRot = kModPi - 0.6f * (1.0f + std::cos(state.ageInTicks / 10.0f * kModPi));
                }
                const float pos = state.walkAnimationPos * 0.6662f;
                if (m_rightLeg) m_rightLeg->xRot = std::cos(pos) * 1.4f * state.walkAnimationSpeed;
                if (m_leftLeg)  m_leftLeg->xRot  = std::cos(pos + kModPi) * 1.4f * state.walkAnimationSpeed;
            }
        private:
            ModelPart* m_upper = nullptr;
            ModelPart* m_rightLeg = nullptr;
            ModelPart* m_leftLeg = nullptr;
        };

        // The valkyrie's sheet: ValkyrieModel and ValkyrieWingsModel both
        // declare 64x32, but the wings' texOffs (24, 31) run to row 40 — past
        // a 32-row sheet (MC's repeat wrap hides it; this engine clamps). The
        // in-house sheet is 64x64 with the body in the top 32 rows, and both
        // models read it at height 64.
        constexpr float kValkyrieSheetHeight = 64.0f;

        // Aether ValkyrieModel.setupAnim (no super call): head look, arms
        // re-seated at x -4 / 5, the walk swing, then HumanoidModel's
        // setupAttackAnimation (right-handed).
        class ValkyrieGenModel : public GeneratedModel {
        public:
            ValkyrieGenModel() : GeneratedModel("valkyrie") {
                m_texHeight = kValkyrieSheetHeight;
                m_head = m_root.Find("head");
                m_body = m_root.Find("body");
                m_rightArm = m_root.Find("right_arm");
                m_leftArm = m_root.Find("left_arm");
                m_rightLeg = m_root.Find("right_leg");
                m_leftLeg = m_root.Find("left_leg");
            }
            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_head || !m_rightArm || !m_leftArm || !m_rightLeg || !m_leftLeg) return;
                m_head->yRot = state.yRot * kModDegToRad;
                m_head->xRot = state.xRot * kModDegToRad;
                m_rightArm->x = -4.0f; m_rightArm->z = 0.0f;
                m_leftArm->x = 5.0f;   m_leftArm->z = 0.0f;
                const float pos = state.walkAnimationPos * 0.6662f;
                const float amt = state.walkAnimationSpeed;
                m_rightArm->xRot = std::cos(pos + kModPi) * 2.0f * amt * 0.5f;
                m_leftArm->xRot = std::cos(pos) * 2.0f * amt * 0.5f;
                m_rightArm->zRot = m_leftArm->zRot = 0.0f;
                m_rightLeg->xRot = std::cos(pos) * 1.4f * amt;
                m_leftLeg->xRot = std::cos(pos + kModPi) * 1.4f * amt;
                m_rightArm->yRot = m_leftArm->yRot = 0.0f;
                // HumanoidModel.setupAttackAnimation.
                const float attack = state.attackTime;
                if (attack <= 0.0f || !m_body) return;
                m_body->yRot = std::sin(std::sqrt(attack) * 2.0f * kModPi) * 0.2f;
                m_rightArm->z = std::sin(m_body->yRot) * 5.0f;
                m_rightArm->x = -std::cos(m_body->yRot) * 5.0f;
                m_leftArm->z = -std::sin(m_body->yRot) * 5.0f;
                m_leftArm->x = std::cos(m_body->yRot) * 5.0f;
                m_rightArm->yRot += m_body->yRot;
                m_leftArm->yRot += m_body->yRot;
                m_leftArm->xRot += m_body->yRot;
                float f = 1.0f - attack;
                f *= f;
                f *= f;
                f = 1.0f - f;
                const float f1 = std::sin(f * kModPi);
                const float f2 = std::sin(attack * kModPi) * -(m_head->xRot - 0.7f) * 0.75f;
                m_rightArm->xRot -= f1 * 1.2f + f2;
                m_rightArm->yRot += m_body->yRot * 2.0f;
                m_rightArm->zRot += std::sin(attack * kModPi) * -0.4f;
            }
        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_body = nullptr;
            ModelPart* m_rightArm = nullptr;
            ModelPart* m_leftArm = nullptr;
            ModelPart* m_rightLeg = nullptr;
            ModelPart* m_leftLeg = nullptr;
        };

        // Aether ValkyrieWingsLayer.setupWingRotation over ValkyrieWingsModel
        // (createMainLayer(4.5, 2.5)): the flap slows to 0.15x on the ground
        // (0.75x aloft) and folds tighter there.
        class ValkyrieWingsGenModel : public GeneratedModel {
        public:
            ValkyrieWingsGenModel() : GeneratedModel("valkyrie_wings") {
                m_texHeight = kValkyrieSheetHeight;
                m_rightWing = m_root.Find("right_wing");
                m_leftWing = m_root.Find("left_wing");
            }
            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (!m_rightWing || !m_leftWing) return;
                const float sinage = state.ageInTicks * (state.isOnGround ? 0.15f : 0.75f);
                const float yRot = std::sin(sinage) / 6.0f - 0.2f;
                const float zRot = std::cos(sinage) / (state.isOnGround ? 8.0f : 3.0f) - 0.125f;
                m_leftWing->yRot = yRot;
                m_leftWing->zRot = zRot;
                m_rightWing->yRot = -yRot;
                m_rightWing->zRot = -zRot;
            }
        private:
            ModelPart* m_rightWing = nullptr;
            ModelPart* m_leftWing = nullptr;
        };

        // Aether SunSpiritModel.setupAnim (FireMinionModel inherits it): the
        // head looks, the arms bob. The layer is 64x64 (SunSpiritModel.
        // createBodyLayer's LayerDefinition.create(mesh, 64, 64)); the
        // generated row states 64x32, so the sheet height is corrected here.
        class FireMinionGenModel : public GeneratedModel {
        public:
            FireMinionGenModel() : GeneratedModel("fire_minion") {
                m_texWidth = 64.0f;
                m_texHeight = 64.0f;
                m_head = m_root.Find("head");
                m_rightArm = m_root.Find("right_arm");
                m_leftArm = m_root.Find("left_arm");
            }
            void SetupAnim(const EntityRenderState& state) override {
                m_root.ResetPose();
                if (m_head) {
                    m_head->xRot = state.xRot * 0.017453292f;
                    m_head->yRot = state.yRot * 0.017453292f;
                }
                if (!m_rightArm || !m_leftArm) return;
                m_rightArm->xRot = -(std::sin(state.ageInTicks * 0.067f) * 0.05f);
                m_rightArm->yRot = 0.0f;
                m_rightArm->zRot = -(std::cos(state.ageInTicks * 0.09f) * 0.05f - 0.05f);
                m_leftArm->xRot = -m_rightArm->xRot;
                m_leftArm->yRot = m_rightArm->yRot;
                m_leftArm->zRot = -m_rightArm->zRot;
            }
        private:
            ModelPart* m_head = nullptr;
            ModelPart* m_rightArm = nullptr;
            ModelPart* m_leftArm = nullptr;
        };

        // SheepuffWoolModel.createFurLayer(new CubeDeformation(3.75), 2.0) —
        // SHEEPUFF_WOOL_PUFFED: the fur row's body pushed 2 px down and grown
        // 3.75 instead of 1.75 (head and legs keep their fixed deformations).
        std::unique_ptr<ModQuadrupedModel> MakePuffedWool() {
            auto wool = std::make_unique<ModQuadrupedModel>(
                "sheepuff_wool", std::initializer_list<const char*>{}, true);
            if (ModelPart* body = wool->Root().Find("body")) {
                for (CubeDefinition& c : body->cubes) {
                    c.originY += 2.0f;
                    c.growX = c.growY = c.growZ = 3.75f;
                }
            }
            return wool;
        }

        // Texture paths (in-house sheets under entity/aether/).
        constexpr const char* kMoaTextures[Game::Moa::kMoaTypeCount] = {
            "assets/textures/entity/aether/moa_blue.png",
            "assets/textures/entity/aether/moa_white.png",
            "assets/textures/entity/aether/moa_black.png",
        };
        // MoaType.saddleTexture: blue and white share moa_saddle.
        constexpr const char* kMoaSaddles[Game::Moa::kMoaTypeCount] = {
            "assets/textures/entity/aether/moa_saddle.png",
            "assets/textures/entity/aether/moa_saddle.png",
            "assets/textures/entity/aether/black_moa_saddle.png",
        };

        // Layer models, one per mesh, built on first use and posed from each
        // mob's render state as it is drawn (AppendMob runs SetupAnim).
        std::unique_ptr<EntityModel> s_phygWings, s_phygWingsBaby;
        std::unique_ptr<EntityModel> s_cowWings, s_cowWingsBaby;
        std::unique_ptr<EntityModel> s_phygSaddle;
        std::unique_ptr<EntityModel> s_moaSaddle;
        std::unique_ptr<EntityModel> s_sheepuffWool, s_sheepuffWoolPuffed;
        std::unique_ptr<EntityModel> s_slimeOuter;
        std::unique_ptr<EntityModel> s_valkyrieWings;

    } // namespace

    std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type) {
        switch (type) {
            case Game::EntityTypeId::Sheepuff:
                if (FindGenModel("sheepuff")) {
                    return std::make_unique<ModQuadrupedModel>(
                        "sheepuff", std::initializer_list<const char*>{}, true);
                }
                return std::make_unique<SheepModel>(false);
            case Game::EntityTypeId::Cockatrice:
                if (FindGenModel("cockatrice")) return std::make_unique<BipedBirdGenModel>("cockatrice", false);
                return std::make_unique<ChickenModel>();
            case Game::EntityTypeId::Moa:
                if (FindGenModel("moa")) return std::make_unique<BipedBirdGenModel>("moa", true);
                return std::make_unique<ChickenModel>();
            case Game::EntityTypeId::Zephyr:
                if (FindGenModel("zephyr")) return std::make_unique<ZephyrGenModel>();
                if (FindGenModel("ghast")) return std::make_unique<GeneratedModel>("ghast");
                return nullptr;
            // PhygRenderer / FlyingCowRenderer: MC's own PigModel and
            // CowModel bodies (their wings are a layer).
            case Game::EntityTypeId::Phyg:      return std::make_unique<PigModel>();
            case Game::EntityTypeId::FlyingCow: return std::make_unique<CowModel>();
            case Game::EntityTypeId::Aerbunny:
                if (FindGenModel("aerbunny")) return std::make_unique<AerbunnyGenModel>();
                return nullptr;
            case Game::EntityTypeId::Aerwhale:
                if (FindGenModel("aerwhale")) return std::make_unique<StaticGenModel>("aerwhale");
                return nullptr;
            // SwetRenderer: SlimeModel (SWET = createInnerBodyLayer) — the
            // engine's generated "slime" row; the shell is a layer.
            case Game::EntityTypeId::BlueSwet:
            case Game::EntityTypeId::GoldenSwet:
                if (FindGenModel("slime")) return std::make_unique<StaticGenModel>("slime");
                return std::make_unique<SlimeOuterModel>();
            // SentryRenderer: SlimeModel over SENTRY = createOuterBodyLayer.
            case Game::EntityTypeId::Sentry:
                return std::make_unique<SlimeOuterModel>();
            case Game::EntityTypeId::AechorPlant:
                if (FindGenModel("aechor_plant")) return std::make_unique<AechorPlantGenModel>();
                return nullptr;
            case Game::EntityTypeId::Mimic:
                if (FindGenModel("mimic")) return std::make_unique<MimicGenModel>();
                return nullptr;
            case Game::EntityTypeId::Valkyrie:
                if (FindGenModel("valkyrie")) return std::make_unique<ValkyrieGenModel>();
                return nullptr;
            case Game::EntityTypeId::FireMinion:
                if (FindGenModel("fire_minion")) return std::make_unique<FireMinionGenModel>();
                return nullptr;
            // WhirlwindRenderer draws nothing — particles only.
            default:
                return nullptr;
        }
    }

    std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type) {
        // MoaRenderer / AerbunnyRenderer draw a baby through the ADULT model,
        // resized in scale() (see ExtractRenderState) — so the baby "mesh" is
        // simply another adult instance and the uniform shrink never applies.
        switch (type) {
            case Game::EntityTypeId::Moa:
                if (FindGenModel("moa")) return std::make_unique<BipedBirdGenModel>("moa", true);
                return nullptr;
            case Game::EntityTypeId::Aerbunny:
                if (FindGenModel("aerbunny")) return std::make_unique<AerbunnyGenModel>();
                return nullptr;
            default:
                return nullptr;
        }
    }

    std::string_view TexturePath(Game::EntityTypeId type) {
        // In-house sheets. Phyg and flying cow are in the vanilla 64x64 pig /
        // cow layout (their renderers draw MC's PigModel / CowModel).
        switch (type) {
            case Game::EntityTypeId::Phyg:        return "assets/textures/entity/aether/phyg.png";
            case Game::EntityTypeId::FlyingCow:   return "assets/textures/entity/aether/flying_cow.png";
            case Game::EntityTypeId::Sheepuff:    return "assets/textures/entity/aether/sheepuff.png";
            case Game::EntityTypeId::Cockatrice:  return "assets/textures/entity/aether/cockatrice.png";
            case Game::EntityTypeId::Zephyr:      return "assets/textures/entity/aether/zephyr.png";
            case Game::EntityTypeId::Moa:         return kMoaTextures[Game::Moa::kBlue];
            case Game::EntityTypeId::Aerbunny:    return "assets/textures/entity/aether/aerbunny.png";
            case Game::EntityTypeId::Aerwhale:    return "assets/textures/entity/aether/aerwhale.png";
            case Game::EntityTypeId::BlueSwet:    return "assets/textures/entity/aether/swet_blue.png";
            case Game::EntityTypeId::GoldenSwet:  return "assets/textures/entity/aether/swet_golden.png";
            case Game::EntityTypeId::AechorPlant: return "assets/textures/entity/aether/aechor_plant.png";
            case Game::EntityTypeId::Mimic:       return "assets/textures/entity/aether/mimic.png";
            case Game::EntityTypeId::Sentry:      return "assets/textures/entity/aether/sentry.png";
            case Game::EntityTypeId::Valkyrie:    return "assets/textures/entity/aether/valkyrie.png";
            case Game::EntityTypeId::FireMinion:  return "assets/textures/entity/aether/fire_minion.png";
            default:                              return {};
        }
    }

    bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                            float partialTick, EntityRenderState& state) {
        // Sheepuff: MC Sheep's two head scales, from the same event-10
        // countdown.
        if (const auto* sheepuff = MobAs<Game::Sheepuff>(mob, type, Game::EntityTypeId::Sheepuff)) {
            state.headEatPositionScale = sheepuff->GetHeadEatPositionScale(partialTick);
            state.headEatAngleScale = sheepuff->GetHeadEatAngleScale(partialTick);
            return true;
        }
        // QuadrupedWingsModel keys the wing fold on the synced flag.
        if (const auto* winged = MobAs<Game::WingedAetherAnimal>(
                mob, type, Game::EntityTypeId::Phyg, Game::EntityTypeId::FlyingCow)) {
            state.isOnGround = winged->IsEntityOnGround();
            return true;
        }
        // CockatriceRenderer: scale 1.8, getBob = the wing flap, the synced
        // on-ground flag.
        if (const auto* cockatrice =
                MobAs<Game::Cockatrice>(mob, type, Game::EntityTypeId::Cockatrice)) {
            state.isOnGround = cockatrice->IsEntityOnGround();
            state.flap = cockatrice->GetWingsAnimation(partialTick);
            state.modelScale = glm::vec3(1.8f);
            return true;
        }
        // MoaRenderer.scale: 1.8 grown, 1.0 as a baby, then translate
        // (0, 0.5, 0) while sitting; getBob = the wing flap.
        if (const auto* moa = MobAs<Game::Moa>(mob, type, Game::EntityTypeId::Moa)) {
            state.isOnGround = moa->IsEntityOnGround();
            state.isSitting = moa->IsSitting();
            state.flap = moa->GetWingsAnimation(partialTick);
            state.modelScale = glm::vec3(state.isBaby ? 1.0f : 1.8f);
            if (moa->IsSitting()) state.modelOffset = glm::vec3(0.0f, 0.5f, 0.0f);
            return true;
        }
        // ZephyrRenderer.scale — the charge puff (squash-and-stretch around
        // 4.5) then translate(0, 0.5, 0) — and getBob, the tail rotation.
        if (const auto* zephyr = MobAs<Game::Zephyr>(mob, type, Game::EntityTypeId::Zephyr)) {
            const float f = std::min(zephyr->GetCloudScale(partialTick), 38.0f);
            float f1 = std::max(f / 38.0f, 0.0f);
            f1 = 1.0f / (std::pow(f1, 5.0f) * 2.0f + 1.0f);
            const float f2 = (8.0f + f1) / 2.0f;
            const float f3 = (8.0f + 1.0f / f1) / 2.0f;
            state.modelScale = glm::vec3(f3, f2, f3);
            state.modelOffset = glm::vec3(0.0f, 0.5f, 0.0f);
            state.flap = zephyr->GetTailRot(partialTick);
            return true;
        }
        // AerbunnyRenderer: scale 0.5 as a baby, translate (0, 0.2, 0); the
        // mid-air pitch (setupRotations: XN by 15 / -15 / vy * 30); the
        // model's puffiness lerp.
        if (const auto* bunny = MobAs<Game::Aerbunny>(mob, type, Game::EntityTypeId::Aerbunny)) {
            state.modelScale = glm::vec3(state.isBaby ? 0.5f : 1.0f);
            state.modelOffset = glm::vec3(0.0f, 0.2f, 0.0f);
            if (!mob.onGround) {
                const double vy = mob.velocity.y;
                const float a = vy > 0.5 ? 15.0f : (vy < -0.5 ? -15.0f : static_cast<float>(vy * 30.0));
                state.swimPitchDeg = -a;   // Axis.XN
                state.swimPivotY = 0.0f;
            }
            const float p = static_cast<float>(bunny->GetPuffiness());
            const float next = p - static_cast<float>(bunny->GetPuffSubtract());
            state.squish = (p + (next - p) * partialTick) / 20.0f;
            return true;
        }
        // AerwhaleRenderer: translate(0, -0.5, 0) then scale 2 (-0.25 in the
        // scaled frame), and setupRotations' XP pitch by the lerped xRot.
        if (type == Game::EntityTypeId::Aerwhale) {
            state.modelScale = glm::vec3(2.0f);
            state.modelOffset = glm::vec3(0.0f, -0.25f, 0.0f);
            state.swimPitchDeg = state.xRot;
            state.swimPivotY = 0.0f;
            return true;
        }
        // SwetRenderer.scale: 1.5 x (the client's squash-and-stretch) x the
        // water-damage shrink.
        if (const auto* swet = MobAs<Game::Swet>(mob, type, Game::EntityTypeId::BlueSwet,
                                                 Game::EntityTypeId::GoldenSwet)) {
            const float w = swet->GetSwetWidth(partialTick);
            const float h = swet->GetSwetHeight(partialTick);
            const float s = 1.5f * (1.0f - swet->GetWaterDamageScale());
            state.modelScale = glm::vec3(w * s, h * s, w * s);
            return true;
        }
        // AechorPlantRenderer.scale: 0.625 + size / 6, then translate
        // (0, 1.2, 0); getBob = the sinage.
        if (const auto* plant = MobAs<Game::AechorPlant>(mob, type, Game::EntityTypeId::AechorPlant)) {
            const float f2 = 0.625f + static_cast<float>(plant->GetSize()) / 6.0f;
            state.modelScale = glm::vec3(f2);
            state.modelOffset = glm::vec3(0.0f, 1.2f, 0.0f);
            state.flap = plant->GetSinage(partialTick);
            state.isCharging = mob.hurtTime > 0;
            state.isAngry = plant->IsTargetingEntity();
            return true;
        }
        // SentryRenderer.scale: 0.879 x (size + 1) with size 1, unsquished.
        if (type == Game::EntityTypeId::Sentry) {
            state.modelScale = glm::vec3(0.879f * 2.0f);
            return true;
        }
        if (const auto* valkyrie = MobAs<Game::Valkyrie>(mob, type, Game::EntityTypeId::Valkyrie)) {
            state.isOnGround = valkyrie->IsEntityOnGround();
            return true;
        }
        // FireMinionRenderer.scale: translate(0, 0.35, 0).
        if (type == Game::EntityTypeId::FireMinion) {
            state.modelOffset = glm::vec3(0.0f, 0.35f, 0.0f);
            return true;
        }
        return type == Game::EntityTypeId::Mimic || type == Game::EntityTypeId::Whirlwind ||
               type == Game::EntityTypeId::EvilWhirlwind;
    }

    std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,
                                         const EntityRenderState& state) {
        (void)state;
        // MoaRenderer.getTexture: the MoaType's texture.
        if (const auto* moa = MobAs<Game::Moa>(mob, type, Game::EntityTypeId::Moa)) {
            return kMoaTextures[std::min<uint8_t>(moa->GetMoaType(), Game::Moa::kMoaTypeCount - 1)];
        }
        // SentryRenderer.getTextureLocation: lit while awake.
        if (const auto* sentry = MobAs<Game::Sentry>(mob, type, Game::EntityTypeId::Sentry)) {
            if (sentry->IsAwake()) return "assets/textures/entity/aether/sentry_lit.png";
        }
        return {};
    }

    int Layers(const Game::Mob& mob, Game::EntityTypeId type,
               const EntityRenderState& state, ModLayer* out) {
        switch (type) {
            // QuadrupedWingsLayer (PHYG_WINGS / FLYING_COW_WINGS) — untinted,
            // always drawn; on a baby the same wings scaled 0.5 and dropped
            // 1.5 blocks (the layer's own baby transform). Then SaddleLayer:
            // the phyg's PHYG_SADDLE = PigModel inflated 0.5 on vanilla
            // pig_saddle; the flying cow's FLYING_COW_SADDLE = the plain
            // CowModel mesh (the body again) on the in-house saddle sheet.
            case Game::EntityTypeId::Phyg:
            case Game::EntityTypeId::FlyingCow: {
                int n = 0;
                const bool phyg = type == Game::EntityTypeId::Phyg;
                const char* slug = phyg ? "phyg_wings" : "flying_cow_wings";
                if (FindGenModel(slug)) {
                    std::unique_ptr<EntityModel>& slot =
                        phyg ? (state.isBaby ? s_phygWingsBaby : s_phygWings)
                             : (state.isBaby ? s_cowWingsBaby : s_cowWings);
                    if (!slot) slot = std::make_unique<QuadrupedWingsGenModel>(slug, state.isBaby);
                    out[n].model = slot.get();
                    out[n].texture = phyg ? "assets/textures/entity/aether/phyg_wings.png"
                                          : "assets/textures/entity/aether/flying_cow_wings.png";
                    ++n;
                }
                const auto* winged = MobAs<Game::WingedAetherAnimal>(
                    mob, type, Game::EntityTypeId::Phyg, Game::EntityTypeId::FlyingCow);
                if (winged && winged->IsSaddled() && !state.isBaby) {
                    if (phyg) {
                        if (!s_phygSaddle) {
                            auto pig = std::make_unique<PigModel>();
                            InflateTree(pig->Root(), 0.5f);
                            s_phygSaddle = std::move(pig);
                        }
                        out[n].model = s_phygSaddle.get();
                        out[n].texture = "assets/textures/entity/equipment/pig_saddle/saddle.png";
                    } else {
                        out[n].model = nullptr;   // the CowModel body again
                        out[n].texture = "assets/textures/entity/aether/flying_cow_saddle.png";
                    }
                    ++n;
                }
                return n;
            }
            // MoaSaddleLayer: MOA_SADDLE = MoaModel inflated 0.27, the
            // MoaType's saddle sheet, NO_OVERLAY.
            case Game::EntityTypeId::Moa: {
                const auto* moa = MobAs<Game::Moa>(mob, type, Game::EntityTypeId::Moa);
                if (!moa || !moa->IsSaddled() || !FindGenModel("moa")) return 0;
                if (!s_moaSaddle) s_moaSaddle = std::make_unique<BipedBirdGenModel>("moa", true, 0.27f);
                out[0].model = s_moaSaddle.get();
                out[0].texture = kMoaSaddles[std::min<uint8_t>(moa->GetMoaType(), Game::Moa::kMoaTypeCount - 1)];
                out[0].noOverlay = true;
                return 1;
            }
            // SheepuffWoolLayer — hidden when shorn, the puffed mesh while
            // puffed, tinted by Sheepuff.getColor (MC Sheep's floored table).
            case Game::EntityTypeId::Sheepuff: {
                const auto* sheepuff = MobAs<Game::Sheepuff>(mob, type, Game::EntityTypeId::Sheepuff);
                if (!sheepuff || sheepuff->IsSheared() || !FindGenModel("sheepuff_wool")) return 0;
                std::unique_ptr<EntityModel>& slot =
                    sheepuff->IsPuffed() ? s_sheepuffWoolPuffed : s_sheepuffWool;
                if (!slot) {
                    if (sheepuff->IsPuffed()) {
                        slot = MakePuffedWool();
                    } else {
                        slot = std::make_unique<ModQuadrupedModel>(
                            "sheepuff_wool", std::initializer_list<const char*>{}, true);
                    }
                }
                const SheepWoolColor& tint = kSheepWoolColors[sheepuff->GetColor() & 0x0F];
                out[0].model = slot.get();
                out[0].texture = "assets/textures/entity/aether/sheepuff_wool.png";
                out[0].r = tint.r;
                out[0].g = tint.g;
                out[0].b = tint.b;
                return 1;
            }
            // SwetOuterLayer: SWET_OUTER (SlimeModel's outer shell) on the
            // same sheet, entityTranslucent.
            case Game::EntityTypeId::BlueSwet:
            case Game::EntityTypeId::GoldenSwet: {
                if (!s_slimeOuter) s_slimeOuter = std::make_unique<SlimeOuterModel>();
                out[0].model = s_slimeOuter.get();
                out[0].texture = type == Game::EntityTypeId::BlueSwet
                    ? "assets/textures/entity/aether/swet_blue.png"
                    : "assets/textures/entity/aether/swet_golden.png";
                out[0].blend = true;
                return 1;
            }
            // SentryGlowLayer (an EyesLayer): the eye sheet over the body
            // while awake, NO_OVERLAY.
            case Game::EntityTypeId::Sentry: {
                const auto* sentry = MobAs<Game::Sentry>(mob, type, Game::EntityTypeId::Sentry);
                if (!sentry || !sentry->IsAwake()) return 0;
                out[0].model = nullptr;
                out[0].texture = "assets/textures/entity/aether/sentry_eye.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // an EyesLayer
                return 1;
            }
            // ValkyrieWingsLayer: VALKYRIE_WINGS on the valkyrie's own sheet.
            case Game::EntityTypeId::Valkyrie: {
                if (!FindGenModel("valkyrie_wings")) return 0;
                if (!s_valkyrieWings) s_valkyrieWings = std::make_unique<ValkyrieWingsGenModel>();
                out[0].model = s_valkyrieWings.get();
                out[0].texture = "assets/textures/entity/aether/valkyrie.png";
                return 1;
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
        (void)type;
        return 0.0f;
    }

    const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,
                         const EntityRenderState& state) {
        (void)mob; (void)type; (void)state;
        return nullptr;
    }

} // namespace Render::AetherCreatureRender
