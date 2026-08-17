// File: src/client/renderer/entity/model/EntityModels.hpp
//
// The eight mob models, transcribed from
// minecraft_code/decompiled_net/minecraft/client/model/.
//
// Every cube origin, size and texOffs below is MC's, unrounded. They are not
// tunable: the texture atlas layout is derived from them, so changing a size
// by one pixel does not make a mob slightly different, it makes it sample the
// wrong texels.
//
// setupAnim is likewise verbatim. The recurring `cos(pos * 0.6662) * amp *
// speed` is MC's limb swing — 0.6662 is the frequency, and the amplitudes
// differ per limb type (1.4 legs, 2.0*0.5 arms, 0.4 spider legs).
#pragma once

#include "client/renderer/entity/model/GeneratedEntityModels.hpp"
#include "common/entity/AnimationState.hpp"
#include "client/renderer/entity/model/KeyframeAnimation.hpp"
#include "client/renderer/entity/model/SetupAnimRunner.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace Game { class Mob; }

namespace Render {

    // MC HumanoidModel.ArmPose. The ordinals matter — HumanoidModel.poseRightArm
    // switches on them — so the order is MC's even though only Empty, Item and
    // BowAndArrow are reachable here (nothing in this port holds a shield, a
    // crossbow or a spyglass yet).
    enum class ArmPose {
        Empty = 0,
        Item,
        Block,
        BowAndArrow,
        ThrowTrident,
        CrossbowCharge,
        CrossbowHold,
        Spyglass,
        TootHorn,
        Brush,
        Spear,
    };

    // MC ArmPose's second constructor flag. A two-handed pose is driven from
    // the MAIN arm's pose even when the item is in the off hand, and it also
    // writes the other arm — which is why poseLeftArm is skipped after a
    // BOW_AND_ARROW poseRightArm rather than being allowed to overwrite it.
    inline bool ArmPoseAffectsOffhand(ArmPose p) {
        switch (p) {
            case ArmPose::BowAndArrow:
            case ArmPose::ThrowTrident:
            case ArmPose::CrossbowCharge:
            case ArmPose::CrossbowHold:
                return true;
            default:
                return false;
        }
    }

    // What a model needs to pose itself. Assembled by the renderer from the
    // entity plus the partial tick — MC's LivingEntityRenderState, reduced to
    // the fields these eight actually read.
    struct EntityRenderState {
        float yRot = 0.0f;          // head yaw RELATIVE to the body, degrees
        float xRot = 0.0f;          // head pitch, degrees
        float walkAnimationPos = 0.0f;
        float walkAnimationSpeed = 0.0f;
        float ageInTicks = 0.0f;
        float attackTime = 0.0f;    // 0..1 swing progress
        bool  isAggressive = false;
        bool  isBaby = false;

        // MC LivingEntityRenderState.ageScale — the baby shrink, read by
        // setupAttackAnimation when it slides the arms around the body.
        float ageScale = 1.0f;

        // MC LivingEntityRenderer.setupRotations' death topple, in degrees.
        // Zero while alive; see MobRenderer::DeathFlipDegrees for the curve.
        float deathFlipDeg = 0.0f;

        // MC LivingEntityRenderer.scale(state, poseStack) — the per-renderer
        // hook a few mobs override to resize the whole model. Only the creeper
        // uses it here (CreeperRenderer.scale: the fuse swell and its wobble).
        glm::vec3 modelScale{1.0f};

        // MC HumanoidRenderState. Everything in this port is right-handed and
        // never uses an item, so the poses are the only humanoid state the
        // renderer actually varies.
        ArmPose rightArmPose = ArmPose::Empty;
        ArmPose leftArmPose  = ArmPose::Empty;

        // MC SkeletonRenderState.isHoldingBow. It gates the melee arm-raise:
        // a skeleton with a bow poses from the ArmPose instead.
        bool isHoldingBow = false;

        // Sheep grazing.
        float headEatPositionScale = 0.0f;
        float headEatAngleScale = 0.0f;

        // Chicken wing flap.
        float flap = 0.0f;
        float flapSpeed = 0.0f;

        // ── Read by the compiled setupAnim programs ────────────────────────
        //
        // These carry MC's value for a plain mob, which is the truthful answer
        // rather than a stand-in: HumanoidMobRenderer:62 sets speedValue to 1.0
        // and only changes it while fall-flying; nothing here rides, sits, uses
        // an item or swims, so MC would take exactly the branch these select.
        float speedValue = 1.0f;
        float swimAmount = 0.0f;
        float mainArm    = 1.0f;   // HumanoidArm ordinal: LEFT 0, RIGHT 1

        bool isCrouching  = false;
        bool isSprinting  = false;
        bool isInWater    = false;
        bool isOnGround   = true;
        bool isFallFlying = false;
        bool isPassenger  = false;
        bool isUsingItem  = false;
        bool isSitting    = false;

        // ── Clip guards (GenClip::guard) ───────────────────────────────────
        //
        // The four booleans MC's animated models branch on. `isInWater` above
        // doubles as FrogRenderState.isSwimming, which FrogRenderer sets from
        // `entity.isInWater()` — so the frog's swim/walk choice needs no field
        // of its own.
        //
        // The defaults are the state of a mob whose behaviour this port does
        // not run yet: a sniffer never searches, a creaking can always move, a
        // copper golem carries nothing. Those are the branches MC takes for a
        // mob doing nothing in particular, which is what these are.
        bool isSearching    = false;
        // MC ArmadilloRenderState.isHidingInShell — the whole rolled-up look
        // (body skipped, legs and tail hidden, the shell cube shown) is one
        // branch of ArmadilloModel.setupAnim keyed on this.
        bool isHidingInShell = false;
        bool canMove        = true;
        bool isResting      = false;
        bool isHoldingItem  = false;

        // ── Animation state timers (Game::AnimationState) ──────────────────
        //
        // One start tick per Game::MobAnim slot, in the mob's own tick space,
        // plus a bit saying whether the slot is running at all. Packed rather
        // than passed as a pointer to the mob so the render state stays a plain
        // value the renderer can build for a mob it does not own.
        uint64_t animStarted = 0;
        float    animStartTick[Game::kMobAnimCount] = {};

        bool AnimRunning(int slot) const {
            return (animStarted & (uint64_t(1) << slot)) != 0;
        }
        // MC AnimationState.getTimeInMillis / KeyframeAnimation.getElapsedSeconds,
        // folded: millis = (ageInTicks - startTick) * 50, seconds = millis/1000.
        float AnimSeconds(int slot) const {
            return (ageInTicks - animStartTick[slot]) * 0.05f;
        }
    };

    class EntityModel {
    public:
        virtual ~EntityModel() = default;

        // Pose the parts for this frame. Implementations call Root().ResetPose()
        // first — MC's loadPose — so that an animation which only writes some
        // parts does not inherit last frame's values on the rest.
        virtual void SetupAnim(const EntityRenderState& state) = 0;

        ModelPart& Root() { return m_root; }
        const ModelPart& Root() const { return m_root; }

        float TexWidth()  const { return m_texWidth; }
        float TexHeight() const { return m_texHeight; }

        // MC's baby transform: head at 1/(headScale) and the body at half size,
        // offset down. Applied by the renderer, not baked in, because the same
        // model instance serves adults and babies.
        virtual bool HasBabyTransform() const { return false; }

        // MC ArmedModel.translateToHand — the transform of the MAIN (right)
        // hand, in this model's own pixel space, valid only after SetupAnim.
        // Returns false for models that hold nothing, which is all of them
        // except the skeleton.
        virtual bool RightHandMatrix(glm::mat4& out) const { (void)out; return false; }

    protected:
        ModelPart m_root;
        float m_texWidth = 64.0f;
        float m_texHeight = 64.0f;
    };

    // MC HumanoidModel — zombie and skeleton.
    class HumanoidModel : public EntityModel {
    public:
        // `slim` gives the skeleton's 2-pixel-wide limbs instead of 4.
        explicit HumanoidModel(bool slim);
        void SetupAnim(const EntityRenderState& state) override;
        bool HasBabyTransform() const override { return true; }

    protected:
        // MC HumanoidModel.poseRightArm / poseLeftArm / setupAttackAnimation.
        // Split out exactly as MC has them because the CALL ORDER matters: the
        // arm poses run first and setupAttackAnimation then displaces the arms
        // around the twisting body, so swapping them loses the twist.
        void PoseRightArm(const EntityRenderState& state);
        void PoseLeftArm(const EntityRenderState& state);
        void SetupAttackAnimation(const EntityRenderState& state);

        ModelPart* m_head = nullptr;
        ModelPart* m_hat = nullptr;
        ModelPart* m_body = nullptr;
        ModelPart* m_rightArm = nullptr;
        ModelPart* m_leftArm = nullptr;
        ModelPart* m_rightLeg = nullptr;
        ModelPart* m_leftLeg = nullptr;
    };

    // MC AbstractZombieModel — HumanoidModel plus the arms-out pose.
    class ZombieModel : public HumanoidModel {
    public:
        ZombieModel() : HumanoidModel(false) {}
        void SetupAnim(const EntityRenderState& state) override;
    };

    class SkeletonModel : public HumanoidModel {
    public:
        SkeletonModel() : HumanoidModel(true) {}
        void SetupAnim(const EntityRenderState& state) override;

        // MC SkeletonModel.translateToHand — note it OVERRIDES HumanoidModel's,
        // shoving the arm one pixel outward before composing its matrix. That
        // pixel is what seats the bow in the fist instead of inside the bone.
        bool RightHandMatrix(glm::mat4& out) const override;
    };

    // MC QuadrupedModel — cow, pig, sheep (and the creeper, which reuses the
    // same leg animation despite being upright).
    class QuadrupedModel : public EntityModel {
    public:
        void SetupAnim(const EntityRenderState& state) override;
        bool HasBabyTransform() const override { return true; }

    protected:
        // MC QuadrupedModel.createBodyMesh(legSize, mirrorLeft, mirrorRight, g).
        void BuildBodyMesh(int legSize, bool mirrorLeftLeg, bool mirrorRightLeg, float grow);

        ModelPart* m_head = nullptr;
        ModelPart* m_body = nullptr;
        ModelPart* m_rightHindLeg = nullptr;
        ModelPart* m_leftHindLeg = nullptr;
        ModelPart* m_rightFrontLeg = nullptr;
        ModelPart* m_leftFrontLeg = nullptr;
    };

    class CowModel : public QuadrupedModel {
    public:
        CowModel();
    };

    class PigModel : public QuadrupedModel {
    public:
        PigModel();
    };

    class SheepModel : public QuadrupedModel {
    public:
        // `fur` builds the woolly overlay layer instead of the body.
        explicit SheepModel(bool fur);
        void SetupAnim(const EntityRenderState& state) override;

    private:
        bool m_fur;
    };

    class CreeperModel : public QuadrupedModel {
    public:
        CreeperModel();
    };

    class SpiderModel : public EntityModel {
    public:
        SpiderModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_head = nullptr;
        ModelPart* m_legs[8] = {};   // right/left pairs, hind to front
    };

    // Built from GeneratedEntityModels — the mesh for any mob without a
    // hand-written class.
    //
    // Animation comes from four sources, applied in MC's own order:
    //
    //  1. Head tracking, but only for the models whose setupAnim actually turns
    //     a head, on the part it actually turns, and only when the compiled
    //     program does not already pose it with MC's own clamps.
    //  2. MC's setupAnim itself, compiled to data by tools/gen_setup_anim.py.
    //     This is the real thing — the same constants, the same call order —
    //     for every statement whose inputs this port carries.
    //  3. MC's KeyframeAnimation clips (tools/gen_entity_models.py). Two kinds:
    //     the distance-driven walk cycle, and the episodic clips played from a
    //     Game::AnimationState — a frog's croak, a camel's idle sway, an
    //     armadillo rolling up, a warden's roar. MC applies these AFTER
    //     setupAnim's own writes, and every channel ADDS to the pose, so the
    //     order here is not cosmetic.
    //  4. The shared limb swing, only for a model with no compiled program and
    //     no walk clip. It is what QuadrupedModel does anyway.
    class GeneratedModel : public EntityModel {
    public:
        // `slug` must name a row in kGenModels; use FindGenModel to check first.
        explicit GeneratedModel(std::string_view slug);
        void SetupAnim(const EntityRenderState& state) override;
        bool HasBabyTransform() const override { return true; }

    private:
        // Resolved once at construction so SetupAnim is a walk over pointers
        // rather than a string compare per part per frame.
        struct Animated {
            ModelPart* part;
            float      phase;      // radians added to the swing
            float      amplitude;
        };
        std::vector<Animated> m_animated;

        // A baked clip plus the generated row that describes how to play it.
        struct Clip {
            KeyframeAnimation anim;
            const GenClip*    def;
        };
        std::vector<Clip> m_clips;

        // MC `part.visible = state.<X>AnimationState.isStarted()` — the frog's
        // croaking throat sac is the only one, and it is the difference between
        // a frog that visibly croaks and one that plays the motion with nothing
        // to inflate.
        struct VisRule { ModelPart* part; int slot; };
        std::vector<VisRule> m_visRules;

        // The part MC turns with the look direction, or null when MC turns
        // none. Read from the model class, not guessed from the part name.
        ModelPart* m_head = nullptr;
        AnimGuard  m_headGuard = AnimGuard::None;
        bool       m_headGuardNegate = false;

        // MC's own setupAnim, compiled.
        SetupAnimProgram m_setup;

        // A model with a walk clip writes no limb rotations of its own, so the
        // heuristic swing must not fill in for it.
        bool m_hasWalkClip = false;
    };

    class ChickenModel : public EntityModel {
    public:
        ChickenModel();
        void SetupAnim(const EntityRenderState& state) override;
        bool HasBabyTransform() const override { return true; }

    private:
        ModelPart* m_head = nullptr;
        ModelPart* m_rightLeg = nullptr;
        ModelPart* m_leftLeg = nullptr;
        ModelPart* m_rightWing = nullptr;
        ModelPart* m_leftWing = nullptr;
    };

} // namespace Render
