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

        // MC LivingEntityRenderState.ageScale (LivingEntity.getAgeScale) —
        // 0.5 for a baby. Read by setupAttackAnimation when it slides the
        // arms around the body and by the sheep's graze drop; it does NOT
        // shrink the model — MC babies are drawn through a separate baby
        // mesh, not a uniform scale.
        float ageScale = 1.0f;

        // MC LivingEntityRenderState.scale — the SCALE attribute, and the
        // ONLY uniform factor LivingEntityRenderer.submit applies (always
        // 1.0 in this port). MobRenderer drops it to kBabyScale solely as
        // the fallback for a baby whose mesh could not be built.
        float scale = 1.0f;

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

        // MC DrownedRenderer.setupRotations' swim tilt — a whole-body pitch
        // applied in the pose stack (after the death flip, before the Y
        // flip), rotating about a pivot swimPivotY blocks up from the feet
        // (state.boundingBoxHeight / 2). Carried on the render state so the
        // outer-layer overlay tilts with the body from the same numbers.
        float swimPitchDeg = 0.0f;
        float swimPivotY = 0.0f;

        bool isCrouching  = false;
        bool isSprinting  = false;
        bool isInWater    = false;
        bool isOnGround   = true;
        bool isFallFlying = false;
        bool isPassenger  = false;
        bool isUsingItem  = false;
        bool isSitting    = false;

        // ── Per-mob render-state inputs for the compiled programs ──────────
        //
        // Each mirrors one field of an MC RenderState subclass. The defaults
        // are the value MC's own extractRenderState produces for a mob whose
        // behaviour this port does not run yet (a horse that never rears has
        // standAnimation 0, a bee that has not stung keeps its stinger) — so
        // an unpopulated field is still the vanilla answer, not a stand-in.
        // MobRenderer fills the ones the game genuinely tracks.
        float squish = 0.0f;             // slime family squash spring
        float flapTime = 0.0f;           // phantom wing clock
        float tentacleAngle = 0.0f;      // squid
        float eatAnimation = 0.0f;       // equines
        float standAnimation = 0.0f;
        float feedingAnimation = 0.0f;
        float playingDeadFactor = 0.0f;  // axolotl
        float inWaterFactor = 0.0f;
        float onGroundFactor = 1.0f;
        float movingFactor = 0.0f;
        float standScale = 0.0f;         // polar bear rear-up
        float rammingXHeadRot = 0.0f;    // goat
        float attackTicksRemaining = 0.0f;         // iron golem, ravager
        float attackAnimationRemainingTicks = 0.0f;// hoglin/zoglin headbutt
        float stunnedTicksRemaining = 0.0f;        // ravager
        float jumpCompletion = 0.0f;     // rabbit
        float holdingAnimationProgress = 0.0f;     // allay
        float tendrilAnimation = 0.0f;   // warden
        float spikesAnimation = 0.0f;    // guardian
        float tailAnimation = 0.0f;      // guardian swim tail
        float entityId = 0.0f;           // witch nose wiggle seed
        float jumpCooldown = 0.0f;       // camel
        float headRollAngle = 0.0f;      // wolf
        float tailAngle = 0.0f;          // wolf
        float lieDownAmount = 0.0f;      // cat/ocelot
        float lieDownAmountTail = 0.0f;
        float relaxStateOneAmount = 0.0f;
        float sitAmount = 0.0f;          // panda
        float lieOnBackAmount = 0.0f;
        float rollAmount = 0.0f;         // panda roll / bee hover roll
        float sneezeTime = 0.0f;         // panda
        float crouchAmount = 0.0f;       // fox pounce crouch
        float peekAmount = 0.0f;         // shulker
        float spinningProgress = 0.0f;   // allay
        float offerFlowerTick = 0.0f;    // iron golem
        float roarAnimation = 0.0f;      // ravager
        float yHeadRotAbs = 0.0f;        // shulker: ABSOLUTE head yaw
        float yBodyRotAbs = 0.0f;        // shulker: ABSOLUTE body yaw
        float biteProgress = 0.0f;       // evoker fangs 0..1 lifetime
        // Enum ordinals, in MC declaration order. mobArmPose is
        // IllagerArmPose or PiglinArmPose depending on the mob; mobPose is
        // ParrotModel.Pose. swingAnimType is SwingAnimationType — WHACK (1)
        // is ArmedEntityRenderState's default, the empty-hand swing.
        float mobArmPose = 0.0f;
        float mobPose = 0.0f;
        float swingAnimType = 1.0f;

        bool isMoving = false;           // dolphin tail gate
        bool animateTail = false;        // equines
        bool hasChest = false;           // donkey/mule/llama
        bool hasLeftHorn = true;         // goat spawns with both horns
        bool hasRightHorn = true;
        bool hasEgg = false;             // turtle
        bool isOnLand = false;
        bool isLayingEgg = false;
        bool isAngry = false;            // bee/wolf
        bool hasStinger = true;          // bee keeps it until it stings
        bool isSheared = false;          // bogged
        bool isUnhappy = false;          // villager head shake
        bool isCharging = false;         // vex
        bool isRidden = false;           // strider
        bool isCreepy = false;           // enderman
        bool isDancing = false;          // allay/piglin
        bool isFaceplanted = false;      // fox
        bool isSwimming = false;         // drowned
        bool isSleeping = false;         // fox/cat
        bool isSpinning = false;         // allay
        bool isSneezing = false;         // panda
        bool isEating = false;
        bool isScared = false;
        // Whether the mob's MAIN hand holds an item — IllagerModel's
        // ATTACKING branch picks swingWeaponDown (armed) vs animateZombieArms
        // (empty) on it. Only the vindicator carries a weapon here.
        bool hasMainHandItem = false;

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

        // MC EndCrystalRenderState.showsBottom — the bedrock base slab.
        bool crystalShowsBottom = true;

        // ── Ender dragon flight inputs (MC EnderDragonRenderState) ─────────
        //
        // The model's neck/tail kinematics read the flight history at fixed
        // delays 0..23 (necks 0..6, body 5/7/10, tails 11..23); MobRenderer
        // pre-lerps each sample by the partial tick (MC getHistoricalPos), so
        // the model just indexes. hasDragonHistory false keeps the old
        // constant-history hover pose — the fallback for a state built
        // without a dragon behind it.
        static constexpr int kDragonHistorySamples = 24;
        bool   hasDragonHistory = false;
        double dragonY[kDragonHistorySamples] = {};
        float  dragonYRot[kDragonHistorySamples] = {};
        // MC EnderDragonRenderState.flapTime — lerp(partialTick, oFlapTime,
        // flapTime), in MC's revolutions (the model multiplies by 2π).
        float  dragonFlapTime = 0.0f;
        // MC getHeadPartYOffset's three inputs.
        bool   dragonIsSitting = false;
        bool   dragonIsLandingOrTakingOff = false;
        double dragonDistanceToEgg = 0.0;   // SQUARED, as MC stores it

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

        // MC BabyModelTransform.apply, run over THIS instance's rest poses:
        // rewrites each root child (head parts translated up and enlarged,
        // everything else offset down and halved) so the instance becomes the
        // class's baby mesh — the renderer builds a second instance per type
        // and calls this once, mirroring AgeableMobRenderer's babyModel.
        // False for models MC never draws as a baby; generated mobs use the
        // '<slug>_baby' mesh instead of this.
        virtual bool BecomeBaby() { return false; }

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
        bool BecomeBaby() override;

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
        bool BecomeBaby() override;
    };

    class PigModel : public QuadrupedModel {
    public:
        PigModel();
        bool BecomeBaby() override;
    };

    class SheepModel : public QuadrupedModel {
    public:
        // `fur` builds the woolly overlay layer instead of the body.
        explicit SheepModel(bool fur);
        void SetupAnim(const EntityRenderState& state) override;
        // One transform serves both layers — MC's SHEEP_BABY_WOOL is the wool
        // layer run through the same SheepModel.BABY_TRANSFORMER.
        bool BecomeBaby() override;

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
        // `slug` must name a row in kGenModels; use FindGenModel to check
        // first. A "<slug>_baby" row (the generator's BabyModelTransform
        // output) runs the ADULT slug's setupAnim program — MC's
        // AgeableMobRenderer swaps meshes, never animation, and the baby
        // mesh keeps every part name.
        //
        // `animSlug` names the compiled setupAnim program to run when it is
        // not simply `slug` — the clothing/decor LAYER meshes (drowned_outer,
        // stray_clothes, ...) keep the body's part names and MC animates them
        // with the body's model class, so they take the base mob's program.
        // Empty = derive from `slug` (stripping a "_baby" suffix).
        explicit GeneratedModel(std::string_view slug,
                                std::string_view animSlug = {});
        void SetupAnim(const EntityRenderState& state) override;

        // MC ArmedModel.translateToHand generalized: composes the transform
        // chain from the root to the mesh's "right_arm" part, resolved at
        // construction. The skeleton-family models (SkeletonModel in MC —
        // stray, bogged, parched, wither skeleton) shove the arm one pixel
        // outward first, exactly like the hand-written SkeletonModel does;
        // IllagerModel and the zombie family use the plain chain.
        bool RightHandMatrix(glm::mat4& out) const override;

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

        // Root-to-right_arm part chain (empty when the mesh has no right_arm)
        // plus the per-model translateToHand arm shove — see RightHandMatrix.
        std::vector<const ModelPart*> m_rightArmChain;
        glm::vec3 m_handOffset{0.0f};
    };

    // MC PufferfishMidModel / PufferfishBigModel over the generated
    // "pufferfish_mid" / "pufferfish_big" meshes (LayerDefinitions
    // PUFFERFISH_MEDIUM / PUFFERFISH_BIG). The compiled setupAnim table only
    // carries the SMALL puffer's program (slug "pufferfish", parts
    // right_fin/left_fin); the mid/big classes write the same flap onto
    // right_blue_fin/left_blue_fin, so it is hand-posed here — the two
    // statements of PufferfishMidModel.setupAnim, verbatim.
    class PufferfishModel : public GeneratedModel {
    public:
        explicit PufferfishModel(std::string_view slug);
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_leftBlueFin = nullptr;
        ModelPart* m_rightBlueFin = nullptr;
    };

    class ChickenModel : public EntityModel {
    public:
        ChickenModel();
        void SetupAnim(const EntityRenderState& state) override;
        bool BecomeBaby() override;

    private:
        ModelPart* m_head = nullptr;
        ModelPart* m_rightLeg = nullptr;
        ModelPart* m_leftLeg = nullptr;
        ModelPart* m_rightWing = nullptr;
        ModelPart* m_leftWing = nullptr;
    };

    // MC ArrowRenderer's quad geometry, rebuilt through the cube pipeline:
    // two crossed 16x5-unit shaft planes plus the 5x5 tail cross, at MC's
    // 0.05625 world scale (0.9 model pixels per arrow unit). Each plane is a
    // zero-thickness box paired with a flipped twin so both sides draw — the
    // cube UV layout maps only one face of a flat box onto the artwork.
    //
    // SetupAnim pitches the whole assembly by the entity xRot: an arrow's
    // orientation IS its rotation pair, there is nothing else to animate.
    class ArrowModel : public EntityModel {
    public:
        ArrowModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_pivot = nullptr;
    };

    // ── Projectile models ──────────────────────────────────────────────────
    //
    // Hand-written rather than generated: each needs a real setupAnim (the
    // pitch-to-flight-direction pose the arrow already has, the wind charge's
    // counter-spin, the shulker bullet's tumble), which the compiled pipeline
    // does not express for non-mob entities. Meshes are MC's model classes
    // (client/model/object/projectile/*) transcribed number for number.

    // MC TridentModel (ThrownTridentRenderer rotates it point-first along the
    // flight path; here the arrow's pivot/yaw scheme plus a fixed roll does
    // the same mapping). Texture entity/trident.png, 32x32.
    class TridentModel : public EntityModel {
    public:
        TridentModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_pivot = nullptr;
    };

    // MC WitherSkullRenderer.createSkullLayer — the 8x8x8 SkullModel head cube
    // at texOffs(0, 35) on the wither sheet (64x64). The dangerous (blue)
    // variant swaps the texture in MobRenderer.
    class WitherSkullModel : public EntityModel {
    public:
        WitherSkullModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_head = nullptr;
    };

    // MC ShulkerBulletModel — three crossed slabs, tumbling exactly as
    // ShulkerBulletRenderer spins them (sin/cos of the age) at its net 0.75
    // scale. Texture entity/shulker/spark.png, 64x32.
    class ShulkerBulletModel : public EntityModel {
    public:
        ShulkerBulletModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_main = nullptr;
    };

    // MC LlamaSpitModel — the seven-cube 3D plus. Texture
    // entity/llama/spit.png, 64x32.
    class LlamaSpitModel : public EntityModel {
    public:
        LlamaSpitModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_pivot = nullptr;
    };

    // MC WindChargeModel — the core cube and the wind shroud counter-rotating
    // at 16 deg/tick. (MC draws the shroud translucent; the entity pipeline
    // here is cutout-only, so it reads solid.) Texture
    // entity/projectiles/wind_charge.png, 64x32.
    class WindChargeModel : public EntityModel {
    public:
        WindChargeModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_wind = nullptr;
        ModelPart* m_windCharge = nullptr;
    };

    // MC EvokerFangsModel — the fang trap the evoker's attack spell raises.
    // Two mirrored jaw plates on a base cube; biteProgress (the entity's
    // lifetime fraction) drives the snap: jaws swing 0.35π shut through an
    // eased 1-(2t)³ curve, the base surges up 7.2·(t+sin(2.7t)), and past
    // t=0.9 the whole model scales away to nothing.
    class EvokerFangsModel : public EntityModel {
    public:
        EvokerFangsModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        ModelPart* m_base = nullptr;
        ModelPart* m_upperJaw = nullptr;
        ModelPart* m_lowerJaw = nullptr;
    };

    // MC EnderDragonModel's setupAnim over the GENERATED dragon mesh. The
    // mesh (37 parts: head+jaw, 5 neck and 12 tail spine segments, body,
    // two-bone wings, three-bone legs) is exact from the tables; what the
    // compiled pipeline cannot express is the POSING — the neck and tail are
    // kinematic chains laid out segment by segment in for-loops, positions
    // accumulated through each segment's own rotations, and the whole root
    // shifts (-2 blocks up-bounce, -3 blocks forward) every frame.
    //
    // The flight-history terms (yaw/height deltas across the dragon's last
    // second of movement) come from EntityRenderState's dragon inputs —
    // MobRenderer pre-lerps the EnderDragon's real DragonFlightHistory into
    // them, so the neck cranes into turns and the tail trails the flight
    // path exactly as MC's does. When hasDragonHistory is false (a state
    // built without a dragon behind it) the old constant-history hover pose
    // stands in, which is MC's own pose for a hovering dragon.
    // MC EndCrystalModel — the two spinning glass shells and the core cube
    // over the bedrock base. MC composes each shell's rotation as a
    // quaternion (Y-spin × the fixed 60° tilt about the (1,0,1)/√2 axis);
    // ModelPart carries euler angles only, so each compound rotation is a
    // CHAIN of helper parts — Ry(45°)·Rx(60°)·Ry(-45°) is exactly that
    // axis-angle, and nesting the chains reproduces MC's outer→inner→cube
    // parenting (the nested pose scales multiply the same way). Texture
    // entity/end_crystal/end_crystal.png, 64x32.
    class EndCrystalModel : public EntityModel {
    public:
        EndCrystalModel();
        void SetupAnim(const EntityRenderState& state) override;

        // MC EndCrystalRenderer.getY — the vertical bob, shared with the
        // beam anchors (a beam ends where the glass is, not where the entity
        // stands).
        static float GetY(float ageInTicks);

    private:
        ModelPart* m_base = nullptr;
        ModelPart* m_outerSpin = nullptr;   // Ry(age·3° + 45°) + the bob
        ModelPart* m_innerSpin = nullptr;   // Ry(age·3° − 45°)
        ModelPart* m_cubeSpin = nullptr;    // Ry(age·3° − 45°)
    };

    class DragonModel : public GeneratedModel {
    public:
        DragonModel();
        void SetupAnim(const EntityRenderState& state) override;

    private:
        void PoseLimbs(float bounce, ModelPart* frontLeg, ModelPart* frontTip,
                       ModelPart* frontFoot, ModelPart* rearLeg, ModelPart* rearTip,
                       ModelPart* rearFoot);

        ModelPart* m_head = nullptr;
        ModelPart* m_jaw = nullptr;
        ModelPart* m_body = nullptr;
        ModelPart* m_neck[5] = {};
        ModelPart* m_tail[12] = {};
        ModelPart* m_leftWing = nullptr;
        ModelPart* m_leftWingTip = nullptr;
        ModelPart* m_rightWing = nullptr;
        ModelPart* m_rightWingTip = nullptr;
        ModelPart* m_leg[12] = {};   // L/R x front/rear x leg/tip/foot
    };

} // namespace Render
