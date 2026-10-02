// File: src/client/renderer/entity/MobRenderer.hpp
//
// Draws every mob the client knows about.
//
// ── Why one draw per texture, not per mob or per part ─────────────────────
//
// ShulkerBoxRenderer draws one call per model PART, because it uses a static
// mesh and needs a fresh matrix each time. A mob is animated per entity per
// frame, so its geometry has to be rebuilt anyway — and once you are rebuilding
// it, applying the entity's world transform on the CPU at the same time costs
// nothing and collapses the whole scene into one draw per texture.
//
// That also sidesteps the Vulkan constraint that bit the block-entity
// renderers: only `uMVP` gets a matrix slot in the push-constant block, so a
// design that needed a separate model matrix per part would not fit.
//
// ── The MC transform chain ────────────────────────────────────────────────
//
// Model space is MC's: pixels, Y down, origin at the model root. Getting to
// world space is LivingEntityRenderer.submit's sequence, in order:
//
//   translate(entity position)
//   rotateY(180 - bodyRot)      MC's models face +Z at yaw 0
//   scale(-1, -1, 1)            flip to Y-up, and mirror X to match
//   scale(1/16)                 pixels -> blocks
//   translate(0, -1.501, 0)     EntityModel.MODEL_Y_OFFSET, in BLOCKS
//
// The 1.501 (not 1.5) is deliberate in MC: the extra thousandth lifts the model
// clear of the ground plane so a mob standing on a block does not z-fight it.
//
// ── Known visual deviations (deliberate, not oversights) ──────────────────
//
// * BLOB SHADOWS (MC EntityRenderDispatcher.renderBlockShadow) are not
//   rendered. MC projects a soft dark quad onto the top faces of the blocks
//   under the entity, per renderer shadowRadius (0.5 humanoid default,
//   slime size*0.25, ghast 1.5, ...), alpha fading with height. That needs
//   per-block ground shape queries from the render loop; skipped rather
//   than faked with a single floating quad.
//
// * WORLD LIGHTING: no per-cell light. The client keeps no light data for
//   entities (ClientLevelBridge reports a constant 15), so a mob takes the
//   one global knob the terrain takes (EntityEnvironment.hpp): the sky dim —
//   night, night vision, the Darkness pulse — per batch, full block light
//   for the entities MC lights at 15 (burning ones, blaze, magma cube,
//   wither, allay, vex, the fireballs), none for the EMISSIVE layers (eyes,
//   the warden's glow). Mobs fog exactly as the terrain does. A mob in a
//   torch-lit cave at night is as dark as the night; that waits on a client
//   light engine.
//
// * The warden's TENDRIL emissive layer is skipped: its alpha is
//   tendrilAnimation, which MC drives from vibration game events this port
//   does not run — the alpha is permanently 0, so MC would draw nothing
//   either. The other four warden emissive layers are rendered.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "client/renderer/entity/LeashRenderer.hpp"
#include "client/renderer/entity/FishingHookRenderer.hpp"
#include "client/renderer/entity/model/EntityModels.hpp"
#include "client/renderer/entity/model/HumanoidArmorModel.hpp"
#include "client/renderer/entity/model/PlayerModel.hpp"
#include "client/renderer/entity/ItemDisplayTransforms.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Morph.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Frustum;
namespace Game {
    class ArmorStand; class Mob; }
namespace Client { class ClientMobManager; struct ClientMob; }

namespace Render {

    // The per-world "Baby Models" look (Options → World Settings).
    //
    // MC 26.1 ("Tiny Takeover") gave nine mobs dedicated baby meshes and
    // textures — cat, chicken, cow (+ mooshroom), ocelot, pig, rabbit, sheep,
    // wolf — and remodeled the adult rabbit with them. `New` draws those
    // (the generator's `<slug>_baby_new` / `rabbit_new` rows on the
    // `*_baby` sheets); `Classic` draws the pre-26.1 BabyModelTransform
    // babies. VISUAL ONLY: hitboxes, eye heights and the rabbit's 15-tick
    // hop clock follow 26.1 either way (see GeneratedEntityTypes' baby
    // columns and Rabbit::SetupAnimationStates).
    enum class BabyModelLook : uint8_t { New = 0, Classic = 1 };
    void SetBabyModelLook(BabyModelLook look);
    BabyModelLook GetBabyModelLook();
    // Bumped by SetBabyModelLook; the renderer drops its model cache when it
    // sees a new value, so a change applies to the next frame.
    int  BabyModelLookGeneration();

    // MC HappyGhastModel over the generated happy ghast mesh `slug` (adult
    // or baby row): the compiled setupAnim plus the body squeeze a worn
    // harness applies (EntityRenderState::hasBodyItem). Null when the row is
    // missing. MountEquipmentLayers.cpp.
    std::unique_ptr<EntityModel> CreateHappyGhastModel(std::string_view slug);

    class MobRenderer {
    public:
        MobRenderer() = default;
        ~MobRenderer();

        bool Initialize();
        void Shutdown();

        // `frustum` is the one the chunk pass of THIS view was culled with
        // (the main frustum, or a portal recursion's) — MC
        // extractVisibleEntities' shouldRender AABB test and the visible-
        // section gate both key on it; see EntityCulling.hpp.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    const Client::ClientMobManager& mobs,
                    float partialTick);

        // MC's cull distance for mobs — clientTrackingRange in blocks, squared.
        // Beyond this the server has stopped sending updates anyway.
        static constexpr float kMaxRenderDistance = 160.0f;

        // ── Spectator (set per frame by the frame loop) ──────────────────
        // The mob the camera is inside (a spectator looking through it) is
        // not drawn — MC skips the camera entity in first person. -1 = none.
        void SetHiddenEntity(int32_t id) { m_hiddenEntityId = id; }
        // MC Entity.isInvisibleTo(player): a spectator sees INVISIBLE bodies,
        // translucent (LivingEntityRenderer's forceTransparent).
        void SetViewerSeesInvisible(bool sees) { m_viewerSeesInvisible = sees; }

        // ── /morph ────────────────────────────────────────────────────────
        // A player drawn as a mob: the mob's model at the player's feet with
        // the player's own body yaw, head yaw, pitch and walk animation, so
        // it reads exactly like a mob to whoever is looking. The player's
        // own held items and armour ride it where the mob's renderer has
        // those layers (MorphPose::equipment). What a mob's own state drives
        // beyond that (wool colour, anger, charge, variants) has no
        // player-side source and is not drawn — the plain adult model with
        // its base texture, its layer where the model has one (sheep wool,
        // drowned/stray/bogged overlay).
        struct MorphPose {
            uint32_t   code = Game::Morph::kNone;   // Game::Morph: a mob or a block here
            // The morph's look beyond the code (Game::Morph::DefaultVariantOf:
            // a tropical fish's packed variant, a salmon's size).
            int32_t    variant = 0;
            glm::dvec3 position;      // feet, world space (double)
            float bodyYaw   = 0.0f;   // degrees
            float headYaw   = 0.0f;
            float pitch     = 0.0f;
            float walkPos   = 0.0f;   // WalkAnimationState::PositionAt / SpeedAt
            float walkSpeed = 0.0f;
            float ageTicks  = 0.0f;   // the models' idle clock
            float scale     = 1.0f;   // the player's /scale
            int   hurtTime  = 0;      // red flash
            int   deathTime = 0;      // topple
            float swell     = 0.0f;   // creeper: 0..1 (Creeper.getSwelling)
            float animTick  = 0.0f;   // sheep graze / skeleton draw ticks left (lerped)
            uint32_t seed   = 0;      // per-body phase (phantom flap, witch nose)
            bool  crouching = false;  // the player model's sneak pose
            // The player's arm (MC LivingEntity.getAttackAnim(partialTick),
            // 0..1 through a swing), its arm poses (AvatarRenderer.getArmPose,
            // ArmPose ordinals), the item-use clock and seat — what
            // HumanoidModel reads off the player's render state.
            float attackTime     = 0.0f;
            uint8_t rightArmPose = 0;
            uint8_t leftArmPose  = 0;
            bool  usingItem      = false;
            int   useItemHand    = 0;       // 0 main, 1 off
            float ticksUsingItem = 0.0f;
            float maxCrossbowCharge = 25.0f;   // HumanoidRenderState.maxCrossbowChargeDuration
            bool  passenger      = false;
            // MC LivingEntityRenderState.isAutoSpinAttack (a riptide: the
            // body spun along its look, the swirl layer) and
            // ticksSinceKineticHitFeedback (a charging spear's recoil).
            bool  autoSpinAttack = false;
            float ticksSinceKineticHitFeedback = 0.0f;
            // The player's worn elytra (ElytraAnimationState.hpp flags) and
            // its wings' angles (partial-tick lerped) — MC WingsLayer, on
            // the humanoid mobs HumanoidMobRenderer gives one.
            uint8_t elytraFlags = 0;
            float elytraRotX = 0.2617994f;
            float elytraRotY = 0.0f;
            float elytraRotZ = -0.2617994f;
            // The player's INVISIBILITY / GLOWING (MC: the body is not drawn
            // but its layers are; a glowing body is outlined) and burning
            // (full block light, MC getBlockLightLevel).
            bool  invisible = false;
            bool  glowing   = false;
            bool  onFire    = false;
            // A pitch of the whole body about its feet, after the body yaw —
            // the monster spawner's cage tilt (SpawnerRenderer: MC's
            // rotateX(-30) before the entity's own rotateY(180) is this +30
            // after it). 0 for a morph.
            float tiltDeg   = 0.0f;
            // MC ParrotOnShoulderLayer — NOT a morph: with shoulderParrot
            // >= 0 (a Parrot.Variant id) this pose is a PLAYER's (feet,
            // body/head yaw, pitch, walk, age, scale, crouch, death topple)
            // and only a parrot in the ON_SHOULDER pose is drawn, on the
            // left or right shoulder of the player model. `code` is ignored.
            int   shoulderParrot = -1;
            bool  shoulderLeft   = false;
            // An engine addition: the shoulder parrot dances (PARTY pose)
            // while its player is near a playing jukebox (ShoulderParrots).
            bool  shoulderParty  = false;
            // What the player holds and wears, by Game::EquipmentSlot
            // ordinal (MAINHAND = the selected hotbar stack): the morph body
            // draws them as the mob's renderer would its own — the held
            // items through ItemInHandLayer on every armed humanoid (the
            // Herobrine player model too), the armour and head item through
            // HumanoidArmorLayer / CustomHeadLayer only on a mob whose
            // renderer has an armour layer (zombies, skeletons, piglins).
            Game::ItemStack equipment[6];
        };
        // Same buffers, pipeline and per-batch draw as Render; appends
        // after it within the frame.
        void RenderMorphs(const glm::mat4& projection, const glm::mat4& view,
                          const glm::vec3& cameraPos, const Frustum& frustum,
                          const std::vector<MorphPose>& poses, float partialTick);

        // MC WingsLayer on a (stick-figure) player: the elytra model on the
        // player model's pose stack — AvatarRenderer's 0.9375 scale and
        // crouch offset, the body yaw, the −1.501 model offset — then
        // translate(0, 0, 0.125) and ElytraModel posed from the wings'
        // angles. The stick figure's own glide tip and death topple (both
        // about the feet) are applied the same way, so the wings stay on its
        // back. `glint` adds MC's armor_entity_glint pass.
        struct ElytraPose {
            glm::dvec3 position{0.0};   // feet, world space
            float bodyYaw   = 0.0f;
            float pitch     = 0.0f;     // the glide tip's (90 + xRot)
            float scale     = 1.0f;     // the player's /scale
            bool  crouching = false;
            float rotX = 0.2617994f, rotY = 0.0f, rotZ = -0.2617994f;
            bool  glint     = false;
            float deathFlipDeg = 0.0f;
            float fallFlyTicks = 0.0f;  // > 0 while gliding (with partial)
            bool  glowing   = false;
        };
        void RenderElytras(const glm::mat4& projection, const glm::mat4& view,
                           const glm::vec3& cameraPos, const Frustum& frustum,
                           const std::vector<ElytraPose>& poses);

        // MC SpinAttackEffectLayer (RiptideLayer.cpp): the swirl around a
        // riptiding stick-figure player, and — through DrawRiptideSwirls —
        // around a /morph player-model body.
        struct RiptidePose {
            glm::dvec3 position{0.0};   // feet, world space
            float bodyYaw    = 0.0f;
            float pitch      = 0.0f;    // xRot, degrees
            float ageInTicks = 0.0f;    // the spin's clock (partial included)
            float scale      = 1.0f;
            bool  glowing    = false;
        };
        void RenderRiptideSwirls(const glm::mat4& projection, const glm::mat4& view,
                                 const glm::vec3& cameraPos, const Frustum& frustum,
                                 const std::vector<RiptidePose>& poses);
        struct RiptideDraw {
            glm::mat4 rootPx{1.0f};     // the player model's root, pixel space
            float ageInTicks = 0.0f;
            int   packedLight = 0;
            bool  glowing = false;
        };
        void DrawRiptideSwirls(const glm::mat4& projection, const glm::mat4& view,
                               const glm::vec3& cameraPos, const std::vector<RiptideDraw>& draws);
        // The riptiding player model's root transform (pixel space).
        static glm::mat4 SpinningPlayerRoot(const glm::dvec3& feet, float bodyYaw, float pitch,
                                            float ageInTicks, float scale);

        // One elytra to draw (ElytraLayer.cpp): the body model's ROOT
        // transform in pixel space (the matrix AppendMob returns — WingsLayer
        // sits on the parent model's pose stack), the baby model's 0.5 mesh
        // scale, the wings' angles, the glint, the body's packed light.
        struct ElytraDraw {
            glm::mat4 rootPx{1.0f};
            bool  baby = false;
            float rotX = 0.2617994f, rotY = 0.0f, rotZ = -0.2617994f;
            bool  crouching = false;
            bool  glint = false;
            int   packedLight = 0;
            bool  glowing = false;
            // MC WingsLayer's texture choice: the skin's elytra, else the
            // CAPE when one is worn and shown, else the elytra asset — a
            // player's cape texture here; INVALID = the elytra asset.
            TextureHandle texture = INVALID_TEXTURE;
        };
        void DrawElytras(const glm::mat4& projection, const glm::mat4& view,
                         const glm::vec3& cameraPos, const std::vector<ElytraDraw>& draws);

        // ── Players drawn with the Minecraft player model (skins) ──────────
        //
        // MC AvatarRenderer for a player whose look is a skin
        // (Game::PlayerAppearance, Client::PlayerSkins — PlayerSkinRenderer.cpp):
        // PlayerModel (classic or slim) in the player's own skin, with the
        // layers AvatarRenderer adds — HumanoidArmorLayer, PlayerItemInHand
        // Layer, CapeLayer (PlayerCapeModel swayed by the cape physics),
        // CustomHeadLayer, WingsLayer (the cape's texture on the elytra when a
        // cape is worn) and SpinAttackEffectLayer. The pose stack is
        // LivingEntityRenderer.submit's with AvatarRenderer.setupRotations:
        // the glide tip and its flying yaw, the swim tilt, the bed, the death
        // topple, the riptide spin. The stick figure stays PlayerRenderer's.
        struct SkinnedPlayerPose {
            uint32_t   playerId = 0;
            glm::dvec3 position{0.0};   // feet, world space
            float bodyYaw   = 0.0f;     // degrees
            float headYaw   = 0.0f;
            float pitch     = 0.0f;
            float walkPos   = 0.0f;     // WalkAnimationState::PositionAt / SpeedAt
            float walkSpeed = 0.0f;
            float ageTicks  = 0.0f;
            float scale     = 1.0f;     // the SCALE attribute (/scale)
            int   hurtTime  = 0;
            int   deathTime = 0;
            bool  crouching = false;
            bool  passenger = false;
            // The arms: the swing and the held-item poses (MorphPose's).
            float attackTime     = 0.0f;
            uint8_t rightArmPose = 0;
            uint8_t leftArmPose  = 0;
            bool  usingItem      = false;
            int   useItemHand    = 0;
            float ticksUsingItem = 0.0f;
            float maxCrossbowCharge = 25.0f;
            bool  autoSpinAttack = false;
            float ticksSinceKineticHitFeedback = 0.0f;
            // Flight (AvatarRenderer.extractFlightData): the glide's age in
            // ticks with the partial (0 when not gliding), the flying yaw
            // (radians) when the body moves off its look, and the limb-swing
            // damping (HumanoidMobRenderer's speedValue, ≥ 1).
            bool  fallFlying = false;
            float fallFlyTicks = 0.0f;
            bool  applyFlyingYRot = false;
            float flyingYRot = 0.0f;
            float speedValue = 1.0f;
            // Swimming (LivingEntity.getSwimAmount, isVisuallySwimming,
            // isInWater).
            float swimAmount = 0.0f;
            bool  visuallySwimming = false;
            bool  inWater = false;
            // In a bed: its FACING (Game::Direction ordinal), < 0 when not.
            int   bedFacing = -1;
            // The look.
            TextureHandle skin = INVALID_TEXTURE;
            bool    slim = false;
            uint8_t modelParts = 0x7F;   // Game::ModelPartBits
            TextureHandle cape = INVALID_TEXTURE;
            float capeFlap = 0.0f, capeLean = 0.0f, capeLean2 = 0.0f;
            // The elytra (ElytraAnimationState flags and angles).
            uint8_t elytraFlags = 0;
            float elytraRotX = 0.2617994f, elytraRotY = 0.0f, elytraRotZ = -0.2617994f;
            // Visibility: INVISIBILITY, GLOWING, burning, a spectator (the
            // translucent floating head).
            bool  invisible = false;
            bool  glowing   = false;
            bool  onFire    = false;
            bool  spectator = false;
            // What the player holds and wears, by Game::EquipmentSlot.
            Game::ItemStack equipment[6];
        };
        void RenderPlayerSkins(const glm::mat4& projection, const glm::mat4& view,
                               const glm::vec3& cameraPos, const Frustum& frustum,
                               const std::vector<SkinnedPlayerPose>& poses, float partialTick);

        // InventoryScreen.extractEntityInInventoryFollowsMouse overrides the
        // render state's rotations (bodyRot = 180 + xAngle·20, yRot = the
        // head's yaw off the body, xRot = −yAngle·20) and draws the entity
        // with every layer its renderer has (a horse's saddle, armour, a
        // llama's carpet). CaptureForGui runs the mob pass for that ONE
        // client mob with those rotations, partial tick 1 and no culling,
        // and hands back its geometry instead of drawing it: render-space
        // blocks with the entity's feet at the origin, one triangle list per
        // texture (hidden, depth-only and glint batches left out). The GUI
        // projects it (MountInventoryScreen).
        struct GuiEntityPose {
            float bodyRot    = 180.0f;   // degrees
            float headYawRel = 0.0f;     // LivingEntityRenderState.yRot
            float xRot       = 0.0f;
            // ENTITY_IN_UI's two lights, carried into render space by the
            // caller (the GUI box's rotation inverted) — the faces are shaded
            // with them instead of the level's pair.
            glm::vec3 light0{0.0f, 1.0f, 0.0f};
            glm::vec3 light1{0.0f, 1.0f, 0.0f};
        };
        struct GuiEntityBatch {
            TextureHandle texture = INVALID_TEXTURE;
            bool blend = false;
            std::vector<ModelVertex> triangles;   // 3 per triangle
        };
        bool CaptureForGui(const Client::ClientMobManager& mobs, int32_t entityId,
                           const GuiEntityPose& pose, std::vector<GuiEntityBatch>& out);
        // The same body for a GUI box (the inventory's player,
        // PlayerInventoryPreview), CaptureForGui's contract: render-space
        // blocks with the feet at the origin, one triangle list per texture,
        // lit with ENTITY_IN_UI's two lights (already carried into render
        // space by the caller). The pose's own bodyYaw / headYaw / pitch are
        // the GUI's (InventoryScreen's overrides); no culling, no cape
        // physics beyond what the pose carries.
        bool CapturePlayerSkinForGui(const SkinnedPlayerPose& pose,
                                     const glm::vec3& light0, const glm::vec3& light1,
                                     std::vector<GuiEntityBatch>& out);
        // The renderer the frame loop owns (set by Initialize, cleared by
        // Shutdown); null before the world renderer is up.
        static MobRenderer* Instance();

        // MC LivingEntityRenderer.setupRotations:174-181 — how far a dying
        // entity has toppled, in degrees, from its death timer.
        //
        //   fall = sqrt(min((deathTime - 1) / 20 * 1.6, 1)) * 90
        //
        // The sqrt front-loads it: the body is most of the way over within the
        // first few ticks and eases into flat, rather than rotating linearly.
        // getFlipDegrees() defaults to 90; MC overrides it to 180 for spiders,
        // silverfish and endermites, and the caller passes that. Public
        // because the player renderer topples its stick figures with the same
        // curve.
        static float DeathFlipDegrees(int deathTime, float partialTick,
                                      float flipDegrees = 90.0f) {
            if (deathTime <= 0) return 0.0f;
            float fall = (static_cast<float>(deathTime) + partialTick - 1.0f)
                       / 20.0f * 1.6f;
            fall = std::sqrt(std::max(fall, 0.0f));
            if (fall > 1.0f) fall = 1.0f;
            return fall * flipDegrees;
        }

    private:
        struct ModelEntry {
            std::unique_ptr<EntityModel> model;
            TextureHandle texture = INVALID_TEXTURE;
            // Sheep carry a second, dyed layer over the base body.
            std::unique_ptr<EntityModel> overlayModel;
            // MC WolfArmorLayer's adultModel (ModelLayers.WOLF_ARMOR): the
            // mesh the BODY slot's armour is drawn on. Adults only, as MC.
            std::unique_ptr<EntityModel> bodyArmorModel;
            // MC EnergySwirlLayer's model: WitherArmorLayer's WITHER_ARMOR
            // (the wither mesh at CubeDeformation(0.5)), CreeperPowerLayer's
            // CREEPER_ARMOR (the creeper mesh at CubeDeformation(2.0)).
            std::unique_ptr<EntityModel> energySwirlModel;
            // MC AgeableMobRenderer's babyModel — the separate baby MESH
            // (big head, half body), not a shrunken adult. Built on the
            // first baby seen; null means MC has no baby mesh for this mob
            // and the uniform-shrink fallback applies.
            std::unique_ptr<EntityModel> babyModel;
            std::unique_ptr<EntityModel> babyOverlayModel;
            bool babyTried = false;
            // MC PufferfishRenderer's three puff stages — `model` is the
            // small (deflated) mesh; these are PUFFERFISH_MEDIUM/BIG,
            // swapped in per frame by the synced puff state.
            std::unique_ptr<EntityModel> pufferMid;
            std::unique_ptr<EntityModel> pufferBig;
            // MC TropicalFishRenderer's second body (TROPICAL_FISH_LARGE —
            // `model` is the small one) and TropicalFishPatternLayer's two
            // pattern meshes (TROPICAL_FISH_SMALL/LARGE_PATTERN), picked per
            // frame by the synced pattern's base.
            std::unique_ptr<EntityModel> tropicalLarge;
            std::unique_ptr<EntityModel> tropicalSmallPattern;
            std::unique_ptr<EntityModel> tropicalLargePattern;
            // MC SalmonRenderer's SALMON_SMALL / SALMON_LARGE (the SALMON
            // mesh through MeshTransformer.scaling(0.5 / 1.5)) — `model` is
            // the medium one; picked per frame by the synced size.
            std::unique_ptr<EntityModel> salmonSmall;
            std::unique_ptr<EntityModel> salmonLarge;
            // MC CowRenderer / PigRenderer / ChickenRenderer's per-ModelType
            // AdultAndBabyModelPair: [variant byte] → mesh, adult and baby,
            // built on first sight. Null where the variant draws with the
            // NORMAL mesh (warm pig, warm chicken).
            std::unique_ptr<EntityModel> variantModels[3];
            std::unique_ptr<EntityModel> variantBabyModels[3];
            bool variantTried[3] = {false, false, false};
            // MC BreezeWindLayer / BreezeEyesLayer — the breeze mesh on its
            // own sheets (the wind one is 128x128, so it is a separate
            // generated row, not a redraw of the body's vertices).
            std::unique_ptr<EntityModel> windModel;
            std::unique_ptr<EntityModel> eyesModel;
            bool layersTried = false;
            // MC SulfurCubeInnerLayer — the inner cube (SULFUR_CUBE_INNER /
            // SULFUR_CUBE_SMALL_INNER) drawn under the translucent shell
            // while the cube holds no block.
            std::unique_ptr<EntityModel> innerModel;
            std::unique_ptr<EntityModel> innerBabyModel;
            bool innerTried = false;
        };

        // CaptureForGui's request while it runs the mob pass (null otherwise).
        const GuiEntityPose* m_guiPose = nullptr;
        int32_t m_guiEntityId = 0;
        std::vector<GuiEntityBatch>* m_guiOut = nullptr;

        ModelEntry* GetModelFor(Game::EntityTypeId type);
        // The baby mesh (and its overlay mesh) for a type, built on the
        // first baby seen — MC AgeableMobRenderer's babyModel. Shared by the
        // mob pass and the morph pass; entry.babyModel stays null for a mob
        // MC never draws through a baby mesh (the caller then shrinks the
        // adult).
        static void EnsureBabyModels(ModelEntry& entry, Game::EntityTypeId type);
        // MC TropicalFishRenderer's look for a packed variant, shared by the
        // mob pass and the morph pass: the body mesh for the pattern's base
        // (`entry.model` / `entry.tropicalLarge`) and its sheet, and
        // TropicalFishPatternLayer — the *_PATTERN mesh appended over it,
        // tinted by the pattern colour. The pattern returns its sheet and
        // first index (INVALID_TEXTURE when nothing was appended).
        static EntityModel* TropicalFishBody(ModelEntry& entry, int32_t packedVariant);
        TextureHandle TropicalFishBodyTexture(int32_t packedVariant);
        TextureHandle AppendTropicalFishPattern(ModelEntry& entry, int32_t packedVariant,
                                                const EntityRenderState& state,
                                                const glm::dvec3& renderPos, float bodyRot,
                                                const glm::vec3& cameraPos,
                                                size_t& firstIndex, bool& cull);
        // `repeatWrap` samples the sheet with REPEAT instead of the clamp
        // entity sheets normally get — for a texture the shader scrolls
        // across its edge (the breeze's wind). Cached separately.
        TextureHandle LoadTexture(const std::string& relativePath, bool repeatWrap = false);
        // The armour trim layer's texture for a worn piece (its TRIM on the
        // layer — humanoid / humanoid_leggings — of `equipmentAsset`);
        // INVALID_TEXTURE without a trim.
        TextureHandle TrimTexture(const Game::ItemStack& piece, const std::string& equipmentAsset, bool leggings);

        // The entity's MC transform chain, mapping model-space PIXELS to
        // RENDER space (world minus the view's integer origin, see
        // RenderOrigin.hpp; `cameraPos` is no longer part of the
        // translation). Shared by the body and by anything
        // parented to it (the bow in a skeleton's hand), so the two can never
        // be composed from different matrices.
        // `scale` is MC LivingEntityRenderState.scale — the SCALE attribute,
        // NOT the baby shrink: MC babies render through a separate baby mesh
        // and this stays 1.0 for them. It carries kBabyScale only for the
        // fallback baby whose mesh could not be built.
        // `deathFlipDeg` is MC LivingEntityRenderer.setupRotations' death roll,
        // already in degrees (see DeathFlipDegrees) — 0 for a living entity.
        // `swimPitchDeg`/`swimPivotY` are DrownedRenderer.setupRotations' swim
        // tilt — a pitch about the mid-box pivot, applied after the death
        // flip, 0 for everything that is not a swimming drowned.
        // `upsideDown`/`boundingBoxHeight` are setupRotations' Dinnerbone roll
        // (EntityRenderState.isUpsideDown), after the yaw, before the swim tilt.
        static glm::mat4 EntityMatrix(const glm::dvec3& renderPos,
                                      const glm::vec3& cameraPos,
                                      float bodyRot, float scale,
                                      float deathFlipDeg = 0.0f,
                                      const glm::vec3& modelScale = glm::vec3(1.0f),
                                      float swimPitchDeg = 0.0f,
                                      float swimPivotY = 0.0f,
                                      const glm::vec3& modelOffset = glm::vec3(0.0f),
                                      bool upsideDown = false,
                                      float boundingBoxHeight = 0.0f,
                                      // A riptide (isAutoSpinAttack): the
                                      // head pitch and the age it spins by.
                                      bool autoSpinAttack = false,
                                      float spinXRot = 0.0f,
                                      float spinAgeInTicks = 0.0f,
                                      // The fish renderers' setupRotations
                                      // (EntityRenderState::fishYawDeg /
                                      // fishLandRoll / fishLandOffset).
                                      float fishYawDeg = 0.0f,
                                      bool fishLandRoll = false,
                                      const glm::vec3& fishLandOffset = glm::vec3(0.0f),
                                      // A renderer's own setupRotations roll
                                      // after the base ones (the iron
                                      // golem's sway): degrees about Z.
                                      float setupRollDeg = 0.0f);

        // Build one mob's posed geometry into `verts`/`idx`, already in world
        // space. Returns the matrix it used.
        glm::mat4 AppendMob(EntityModel& model, const EntityRenderState& state,
                            const glm::dvec3& renderPos, float bodyRot,
                            const glm::vec3& cameraPos,
                            std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

        // One item model's display.thirdperson_righthand block — translation
        // in sixteenths of a block, rotation in degrees, uniform scale —
        // exactly as the item JSONs carry them.
        struct DisplaySpec {
            glm::vec3 translation;
            glm::vec3 rotationDeg;
            float     scale;
        };

        // MC ItemInHandLayer for an arbitrary flat item sprite: hand matrix
        // from the model, the layer's fixed grip, then the item's display
        // transform. Returns the sprite's texture (INVALID if unavailable).
        // The same, from a hand matrix the caller resolved (the armor
        // stand's off hand); `leftHand` mirrors the grip and the display
        // transform as MC ItemTransform.apply(leftHand) does.
        TextureHandle AppendHeldSpriteAt(const glm::mat4& entityMatrix,
                                         const glm::mat4& handMatrix, bool leftHand,
                                         const std::string& itemName,
                                         const DisplaySpec& spec,
                                         std::vector<ModelVertex>& verts,
                                         std::vector<uint32_t>& idx);
        // MC ArmorStandRenderer's layers: HumanoidArmorLayer (the four
        // armor pieces on the armor mesh, the leggings on the inner one),
        // ItemInHandLayer (both hands), CustomHeadLayer (a block or item on
        // the head). `emit(texture, firstIndex, cullBackFaces)` closes one
        // batch over everything appended since firstIndex.
        void AppendArmorStandLayers(const Game::ArmorStand& stand, const ArmorStandModel& model,
                                    const EntityRenderState& state, const glm::mat4& entityMatrix,
                                    const glm::dvec3& renderPos, float bodyRot,
                                    const glm::vec3& cameraPos,
                                    const std::function<void(TextureHandle, size_t, bool)>& emit);
        // The armor meshes for the stand, one per deformation (outer 1.0,
        // leggings 0.5) — MC ArmorModelSet.
        std::unique_ptr<ArmorStandArmorModel> m_armorStandArmorOuter;
        std::unique_ptr<ArmorStandArmorModel> m_armorStandArmorInner;
        // MC HumanoidArmorLayer for a mob whose renderer has one (the zombie
        // and skeleton families, the piglins — ArmorFamilyFor): its worn
        // pieces (Mob::GetEquipment) through AppendHumanoidArmor, the 26.x
        // baby mesh set under a 26.x baby body. Defined in
        // MobEquipmentLayers.cpp. `emit` as for the stand.
        void AppendMobArmorLayer(const Game::Mob& mob, EntityModel& model,
                                 const EntityRenderState& state,
                                 const glm::dvec3& renderPos, float bodyRot,
                                 const glm::vec3& cameraPos,
                                 const std::function<void(TextureHandle, size_t, bool)>& emit);

        // ── Equipment layers (MobEquipmentLayers.cpp) ─────────────────────
        //
        // MC's item-carrying render layers over the synced equipment
        // (Mob::GetEquipment): ItemInHandLayer on every armed renderer (both
        // hands, the left-handed mob's arms swapped, the baby grip),
        // CustomHeadLayer (a block, item or mob head on the head),
        // HumanoidArmorLayer (the renderer's armour mesh set — humanoid,
        // zombie-villager or piglin, adult or the 26.x baby mesh), and the
        // per-renderer holders (FoxHeldItemLayer, DolphinCarryingItemLayer,
        // PandaHoldsItemLayer, WitchItemLayer, CrossedArmsItemLayer). Every
        // item goes through AppendItem — MC ItemStackRenderState.submit with
        // the item model's own display transform.
    public:
        using EmitFn = std::function<void(TextureHandle, size_t, bool)>;
        // What an ItemModelResolver reads off the holder: the use clock that
        // picks the bow / crossbow pull stage, the trident's throwing model,
        // the shield's blocking model.
        struct ItemUseState {
            bool  usingItem = false;
            float useTicks  = 0.0f;
            // SpearAnimations' inputs for this arm (ItemInHandLayer
            // .submitArmWithItem): the swing progress (ArmedEntityRenderState
            // .swingAnimation), whether that swing is this arm's STAB, the
            // SPEAR pose (ArmPose.animateUseItem) and the kinetic hit clock.
            float swingAnimation = 0.0f;
            bool  stabbing = false;
            bool  spearPose = false;
            float ticksSinceKineticHitFeedback = 0.0f;
        };
        // The two arms' ItemUseStates from a humanoid render state (right
        // arm, left arm) — the use clock on the arm in use, the swing on the
        // main arm.
        static void ArmUseStates(const EntityRenderState& state, ItemUseState& right, ItemUseState& left);
        // MC ItemStackRenderState.submit: `pose` is the pose stack at submit,
        // in BLOCKS (render space); the context's display transform is
        // applied here. Each sprite layer / model closes its own batch
        // through `emit` (NO_OVERLAY — the caller's batch carries none).
        void AppendItem(const glm::mat4& pose, const Game::ItemStack& stack,
                        ItemDisplay::Context context, const ItemUseState& use,
                        const EmitFn& emit);
        // MC ItemInHandLayer.submitArmWithItem up to the item's submit: the
        // model's translateToHand for the arm (the vex's scaled grip, or
        // `handOverride` — the allay's), X -90, Y 180 and the grip offset
        // (the baby one for a baby). False when the model has no such arm.
        bool ArmItemPose(const EntityModel& model, Game::EntityTypeId type,
                         const glm::mat4& entityMatrix, bool leftArm, bool babyGrip,
                         glm::mat4& out, const glm::mat4* handOverride = nullptr) const;
        // MC ItemInHandLayer.submit: the right arm's item, then the left's.
        void AppendHeldItems(const EntityModel& model, Game::EntityTypeId type,
                             const glm::mat4& entityMatrix,
                             const Game::ItemStack& rightItem, const Game::ItemStack& leftItem,
                             bool babyGrip, const ItemUseState& rightUse,
                             const ItemUseState& leftUse, const EmitFn& emit);
        // MC CustomHeadLayer.Transforms.
        struct HeadTransforms {
            float yOffset = 0.0f, skullYOffset = 0.0f;
            float horizontalScale = 1.0f, verticalScale = 1.0f;
            // The model's own HeadedModel.translateToHead tail after the
            // head part's pose (blocks): CopperGolemModel translates
            // (0, 0.125, 0) and scales 1.0625; every humanoid head model
            // adds nothing.
            float modelHeadYOffset = 0.0f, modelHeadScale = 1.0f;
        };
        static HeadTransforms HeadTransformsFor(Game::EntityTypeId type);
        // MC CustomHeadLayer.submit for the head slot's stack: a mob head
        // (skull model) or a non-armour item / block. Armour drawn by the
        // armour layer is left alone (HumanoidArmorLayer.shouldRender).
        // `wornHeadAnimationPos` flaps a piglin head's ears (the wearer's
        // walk position).
        void AppendCustomHead(const EntityModel& model, const glm::mat4& entityMatrix,
                              const Game::ItemStack& head, const HeadTransforms& transforms,
                              float wornHeadAnimationPos, const EmitFn& emit);
        // Which armour mesh set a renderer's HumanoidArmorLayer uses; None
        // for a renderer without one (MC IllagerRenderer, VillagerRenderer…).
        enum class ArmorFamily : uint8_t { None, Humanoid, ZombieVillager, Piglin };
        static ArmorFamily ArmorFamilyFor(Game::EntityTypeId type);
        // MC HumanoidArmorLayer.submit: chest, legs, feet, head — each
        // EQUIPPABLE piece with an equipment asset in its own slot, on the
        // family's mesh (the 26.x baby mesh when `babyMesh`), posed from
        // `wearer`, in its asset's sheet (dyed leather under its overlay).
        // `equipment` is indexed by Game::EquipmentSlot ordinal (6).
        // `rootOverride`: build on that root (pixels → render space) instead
        // of the entity chain from `renderPos` / `bodyRot` — a player model
        // whose pose stack carries more than EntityMatrix knows (the glide,
        // the swim tilt, the bed).
        void AppendHumanoidArmor(ArmorFamily family, bool babyMesh,
                                 const Game::ItemStack* equipment, EntityModel& wearer,
                                 const EntityRenderState& state, const glm::dvec3& renderPos,
                                 float bodyRot, const glm::vec3& cameraPos, const EmitFn& emit,
                                 const glm::mat4* rootOverride = nullptr);
        // The layers above for one mob of the mob pass, gated per renderer
        // as MC's are (the vindicator's axe only while aggressive…); the
        // held and head layers only — the armour is AppendMobArmorLayer.
        void AppendMobItemLayers(const Game::Mob& mob, Game::EntityTypeId type, EntityModel& model,
                                 const EntityRenderState& state, const glm::mat4& entityMatrix,
                                 const EmitFn& emit);
        // The /morph pass's equipment layers: the player's held items in
        // the morph's hands (ItemInHandLayer — every armed humanoid, and the
        // Herobrine player model when `playerModel`), and on a mob whose
        // renderer has one, the armour layer and the head item. `mainHand`
        // is the right arm's stack (the player's, or the morph's own bow).
        void AppendMorphEquipment(const MorphPose& pose, Game::EntityTypeId type, bool playerModel,
                                  EntityModel& model, const EntityRenderState& state,
                                  const glm::mat4& entityMatrix, const glm::dvec3& renderPos,
                                  bool babyMesh, const Game::ItemStack& mainHand,
                                  const glm::vec3& cameraPos, const EmitFn& emit);
        // The item lookups MobRenderer.cpp keeps (EquipmentAssets, the
        // stack's sprite, the block a block item shows as).
        static const char* EquipmentAssetFor(Game::ItemID id);
        // The equipment asset a worn stack draws with: its EQUIPPABLE
        // asset_id's path (so a component patch re-skins the piece — MC
        // HumanoidArmorLayer reads equippable.assetId()), else the item's
        // own material; "" when it has none (nothing is drawn).
        static std::string EquipmentAssetOf(const Game::ItemStack& stack);
        static std::string ItemSpriteName(Game::ItemID id);
        static Game::BlockID ItemBlockShown(Game::ItemID id);
    private:
        // ── Mount equipment layers (MountEquipmentLayers.cpp) ─────────────
        //
        // MC SimpleEquipmentLayer on the mount renderers — saddles (horse,
        // donkey, mule, skeleton / zombie horse, pig, strider, camel, camel
        // husk, nautilus), horse and nautilus armour (leather dyed by
        // DYED_COLOR), the happy ghast's harness with its goggles — plus
        // LlamaDecorLayer (carpets, the trader llama's decor) and the happy
        // ghast's RopesLayer, over the synced SADDLE / BODY equipment.
        // Each layer mesh is the body mesh plus MC's layer parts, posed by
        // the body's own setupAnim. `emit` as for the armour layer.
        void AppendMountEquipmentLayers(const Game::Mob& mob, Game::EntityTypeId type,
                                        const EntityRenderState& state,
                                        const glm::dvec3& renderPos, float bodyRot,
                                        const glm::vec3& cameraPos, const EmitFn& emit);
        // The mount renderers' extractRenderState inputs the models read:
        // isRidden (entity.isVehicle() — the reins, the goggles, the
        // strider's bristles) and the happy ghast's worn body item.
        static void FillMountRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                                         EntityRenderState& state);
        enum class MountMesh : uint8_t;
        // The layer mesh for (mesh, type), built on first use; dropped when
        // the baby look changes. Null when the mob has no such mesh.
        EntityModel* MountModel(MountMesh mesh, Game::EntityTypeId type);
        std::unordered_map<uint32_t, std::unique_ptr<EntityModel>> m_mountModels;
        int m_mountModelsLookGeneration = -1;

        // The armour meshes by set: humanoid outer 1.0 / inner 0.5, piglin
        // outer 1.02, zombie villager outer / inner, humanoid baby outer /
        // inner, piglin baby — built on first use.
        std::unique_ptr<HumanoidArmorModel> m_armorModels[8];
        HumanoidArmorModel& ArmorModel(int index);
        // The mob-head models (MC SkullModel's mob head / humanoid head and
        // PiglinHeadModel), by SkullBlock.Type ordinal — built on first use.
        std::unique_ptr<ModelPart> m_skullModels[6];
        std::unique_ptr<ModelPart> m_shieldModel;
        // MC BannerModel (standing: pole + bar) and BannerFlagModel (the
        // flag, patterned) for a banner worn or held — the illager captain's
        // ominous banner. Built on first use.
        std::unique_ptr<ModelPart> m_bannerModel;
        std::unique_ptr<ModelPart> m_bannerFlagModel;
        TextureHandle AppendHeldSprite(const EntityModel& model,
                                       const glm::mat4& entityMatrix,
                                       const std::string& itemName,
                                       const DisplaySpec& spec,
                                       std::vector<ModelVertex>& verts,
                                       std::vector<uint32_t>& idx);


        ShaderHandle m_shader = INVALID_SHADER;

        // Two streaming sets alternated per FRAME, each call within a frame
        // appending at a cursor — the scheme EntityCulling.hpp's frame-serial
        // note lays out. One set rewritten every call was a Vulkan hazard
        // twice over: the previous frame's commands could still be reading
        // it, and the portal pass's second call overwrote the main pass's
        // geometry before either had drawn.
        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            BufferHandle ib   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[2];
        EntityFrame::Cursor m_frameCursor;
        size_t m_vertCursor = 0;   // vertices already written this frame
        size_t m_idxCursor  = 0;   // indices already written this frame

        // Streaming capacity PER SET, shared by every call in a frame. A mob
        // is ~1.5k vertices posed; this holds a few hundred of them, well
        // past what the tracking range can deliver.
        static constexpr size_t kMaxVertices = 262144;
        static constexpr size_t kMaxIndices  = 393216;

        std::unordered_map<uint16_t, ModelEntry> m_models;
        // Morph::Kind::Player: the wide player model (HumanoidModel, 64×64
        // skin), built on first use.
        std::unique_ptr<EntityModel> m_playerMorphModel;
        // Skinned players (PlayerSkinRenderer.cpp): the classic and slim
        // PlayerModel and the cape's PlayerCapeModel, built on first use.
        std::unique_ptr<PlayerModel> m_skinModels[2];
        std::unique_ptr<PlayerCapeModel> m_capeModel;
        PlayerModel& PlayerSkinModel(bool slim);
        // LivingEntityRenderer.submit + AvatarRenderer.setupRotations for a
        // skinned player: the model root, pixels → render space.
        // `renderFeet`: the feet in render space (the origin for a GUI box).
        static glm::mat4 SkinnedPlayerRoot(const SkinnedPlayerPose& pose, const glm::vec3& renderFeet,
                                           float partialTick);
        // One skinned body and its layers onto m_verts / m_indices; `emit`
        // closes a batch (texture, first index, cull, part: 0 body,
        // 1 layer, 2 cape). Returns the root for the elytra and swirl.
        glm::mat4 AppendSkinnedPlayer(const SkinnedPlayerPose& pose, const glm::vec3& renderFeet,
                                      float partialTick, const glm::vec3& cameraPos,
                                      const std::function<void(TextureHandle, size_t, bool, int)>& emit);
        // The pose's EntityRenderState (what PlayerModel.setupAnim reads).
        static EntityRenderState SkinnedPlayerState(const SkinnedPlayerPose& pose, float partialTick);
        // A chicken morph's wing state, MC Chicken.aiStep's flap fields
        // stepped per tick from "airborne" (the morph's anim byte), one per
        // morphed player.
        struct MorphFlap {
            float flap = 0.0f, oFlap = 0.0f;
            float flapSpeed = 0.0f, oFlapSpeed = 0.0f;
            float flapping = 1.0f;
            int   lastTick = -1;
        };
        std::unordered_map<uint32_t, MorphFlap> m_morphFlaps;
        // An armadillo morph's shell: MC Armadillo.ArmadilloState run on the
        // morph's flag (Left Alt) — IDLE → ROLLING (10 ticks) → SCARED, and
        // UNROLLING (30) → IDLE — with the clips the mob's own
        // setupAnimationStates starts in each state, one per morphed player.
        struct MorphShell {
            int state = 0;          // Game::Armadillo::State
            int enteredTick = -1;
        };
        std::unordered_map<uint32_t, MorphShell> m_morphShells;
        std::unordered_map<std::string, TextureHandle> m_textureCache;
        int m_textureCacheGeneration = -1;   // Resources::CacheStale
        int m_babyLookGeneration = -1;       // BabyModelLookGeneration

        enum class AssetState : uint8_t { Unloaded, Ready, Failed };
        // A TridentModel instance reused for every in-hand trident (the
        // geometry under its "orient" wrapper is the raw vertical trident).
        std::unique_ptr<EntityModel> m_heldTridentModel;

        // MC ThrownItemRenderer — snowball, egg, splash potion and the
        // fireballs render as a camera-facing extruded item sprite at MC's
        // GROUND display scale (0.5). One cached mesh per item sprite.
        struct SpriteEntry {
            AssetState state = AssetState::Unloaded;
            std::vector<ModelVertex> verts;
            std::vector<uint32_t>    indices;
            TextureHandle            texture = INVALID_TEXTURE;
        };
        std::unordered_map<std::string, SpriteEntry> m_spriteEntries;

        SpriteEntry* EnsureSpriteGeometry(const std::string& itemName);
        // Appends the billboarded sprite; returns the texture to batch with,
        // or INVALID_TEXTURE when nothing was appended. `edgeOn`: MC
        // FireworkEntityRenderer's shot-at-angle turn (Z 180, Y 180, X 90
        // after the camera orientation — the sprite laid edge-on to the
        // view, a streak rather than a card).
        TextureHandle AppendSpriteProjectile(const std::string& itemName,
                                             const glm::dvec3& renderPos,
                                             float halfHeight,
                                             const glm::vec3& cameraPos,
                                             std::vector<ModelVertex>& verts,
                                             std::vector<uint32_t>& idx,
                                             bool edgeOn = false);

        // ── End dragon fight geometry ─────────────────────────────────────
        //
        // MC EnderDragonRenderer.submitCrystalBeams — the 8-segment textured
        // tube between a crystal and its target (also the dragon-healing
        // beam), in world space, batched on end_crystal_beam.png (wrapped
        // REPEAT for the scroll). AppendDragonRays is the death cinematic's
        // fan of magenta rays, batched blended on a 1x1 white texture.
        TextureHandle BeamTexture();
        void AppendCrystalBeam(const glm::dvec3& baseWorld,
                               const glm::dvec3& tipWorld, float ageInTicks,
                               const glm::vec3& cameraPos,
                               std::vector<ModelVertex>& verts,
                               std::vector<uint32_t>& idx);
        void AppendDragonRays(const glm::dvec3& centerWorld, float deathTime01,
                              const glm::vec3& cameraPos,
                              std::vector<ModelVertex>& verts,
                              std::vector<uint32_t>& idx);
        // MC PaintingRenderer: the canvas (front, on the variant's texture)
        // and the frame (back and edges, on painting/back.png), each quad
        // lit by the light of the block cell it covers — MC's
        // lightCoordsPerBlock — baked into the vertex colour with the face's
        // diffuse shade. Appends two batches' worth of geometry: the front
        // indices first, then the frame's; returns the split.
        struct PaintingGeometry {
            TextureHandle front = INVALID_TEXTURE;
            TextureHandle back  = INVALID_TEXTURE;
            size_t frontFirst = 0, frontCount = 0;
            size_t backFirst = 0,  backCount = 0;
        };
        PaintingGeometry AppendPainting(const Game::Mob& painting, const glm::dvec3& centerWorld,
                                        std::vector<ModelVertex>& verts,
                                        std::vector<uint32_t>& idx);
        // MC ItemFrameRenderer: the frame (block/item_frame or
        // block/glow_item_frame, on the blocks atlas — hidden for an
        // invisible frame) and the framed item turned by its rotation, at
        // the FIXED display transform (a block's model, or a flat item's
        // extruded sprite). Two ranges, as for the painting.
        struct ItemFrameGeometry {
            TextureHandle frameTex = INVALID_TEXTURE;
            size_t frameFirst = 0, frameCount = 0;
            TextureHandle itemTex = INVALID_TEXTURE;
            size_t itemFirst = 0, itemCount = 0;
            // A framed map (MapRenderer.render, showOnlyFrame): the map's own
            // texture, then its frame-visible decorations on the
            // map_decorations atlas.
            TextureHandle mapTex = INVALID_TEXTURE;
            size_t mapFirst = 0, mapCount = 0;
            TextureHandle decorationTex = INVALID_TEXTURE;
            size_t decorationFirst = 0, decorationCount = 0;
        };
        ItemFrameGeometry AppendItemFrame(const Game::Mob& frame, const glm::dvec3& centerWorld,
                                          std::vector<ModelVertex>& verts,
                                          std::vector<uint32_t>& idx);

        // Boats, rafts and minecarts (VehicleRenderer.cpp) — MC
        // AbstractBoatRenderer (BoatRenderer / RaftRenderer, the chest
        // variants, the water patch) and AbstractMinecartRenderer (the cart
        // on its rails, the displayed block, the TNT cart's swell and flash).
        // Appends the geometry to m_verts / m_indices and one piece per draw.
        struct VehiclePiece {
            TextureHandle texture = INVALID_TEXTURE;
            size_t first = 0, count = 0;
            // The boat's water patch: RenderTypes.waterMask — depth only, so
            // the water surface is not drawn inside the hull.
            bool   depthOnly = false;
            // A lit TNT minecart's block flashes white (OverlayTexture u(1)).
            bool   whiteFlash = false;
            int    packedLight = 0;
        };
        void AppendVehicle(const Client::ClientMob& entry, const glm::dvec3& renderPos, float partialTick,
                           std::vector<VehiclePiece>& out);
        TextureHandle m_whiteTexture = INVALID_TEXTURE;
        TextureHandle m_beamTexture = INVALID_TEXTURE;
        bool m_beamTextureTried = false;

        // Reused across frames so the per-frame rebuild does not allocate.
        std::vector<ModelVertex> m_verts;
        int32_t m_hiddenEntityId      = -1;
        bool    m_viewerSeesInvisible = false;
        std::vector<uint32_t>    m_indices;

        // Leads and fence knots (LeashRenderer.hpp) — drawn from Render, so
        // every pass that draws the mobs draws their ropes.
        LeashRenderer m_leashRenderer;
        // Fishing bobbers and their lines (FishingHookRenderer.hpp) — the
        // same arrangement as the leads.
        FishingHookRenderer m_fishingHookRenderer;

        bool m_initialized = false;
    };

} // namespace Render
