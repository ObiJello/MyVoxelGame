// File: src/client/renderer/viewmodel/HeldItemRenderer.hpp
// Vanilla-style first-person held-item rendering for BOTH hands (MC's
// ItemInHandRenderer.renderHandsWithItems). Shows the selected hotbar item
// in the lower-right and the offhand item mirrored in the lower-left, with:
//   • per-hand equip-swap animation (item slides down then the new one
//     slides up when the hand's item changes)
//   • swing animation (main hand only — forward arc when you click)
//   • hold-to-use poses (EAT/DRINK pull-to-mouth, BLOCK guard raise)
//   • view-bob (subtle drift driven by walked distance)
//   • per-item-type display transform (BLOCK items rendered as small
//     3D cubes, SPRITE items as voxelised extruded sprites)
//
// Sits alongside the portal-gun viewmodel; PlatformMain passes
// renderMainHand=false while the portal gun owns the main hand.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/entity/Item.hpp"  // Game::ItemID, Game::ItemStack

#include <glm/glm.hpp>

#include <optional>

namespace Render {

    class HeldItemRenderer {
    public:
        // One-time setup: shader, dummy white texture, scratch buffers.
        // Safe to call even if initialisation fails — Render() no-ops.
        bool Initialize();
        void Shutdown();

        // Per-game-tick (~20Hz). Drives all the animation timers. The
        // caller passes BOTH hands' current items (main = selected hotbar,
        // off = inventory slot 45) + whether the player started attacking
        // THIS tick (rising edge); the renderer handles per-hand equip
        // swaps and the main-hand swing itself.
        // `viewPitchDeg` / `viewYawDeg` are the live view angles in MINECRAFT's
        // convention (pitch positive looking DOWN). They drive the look-around
        // sway — see m_xBob below.
        // `mainHandSwapScale` is MC Player.getItemSwapScale(1.0F) — the raise
        // animation is paced by the HELD WEAPON's attack cooldown, not by a
        // fixed duration, which is why a netherite axe takes a full second to
        // come up and a bare hand a quarter of one.
        void Tick(Game::ItemID mainItem, Game::ItemID offhandItem,
                  bool attackPressedThisTick, float mainHandSwapScale,
                  float viewPitchDeg, float viewYawDeg);

        // The main arm's swing, 0..1 through it (0 at rest) — the local
        // player's LivingEntity.getAttackAnim(partialTick), which also poses
        // their own humanoid body in third person (a /morph Herobrine).
        // The server made the local player swing (/swing — its
        // ClientboundAnimatePacket on the player's own id): the next Tick
        // starts a swing as an attack press would.
        void RequestSwing() { m_swingRequested = true; }

        // MC LivingEntity.isSwinging for the local player.
        bool IsSwinging() const { return m_swingActive; }
        float AttackAnim(float partialTick) const {
            if (!m_swingActive) return 0.0f;
            return m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick;
        }

        // The hands' DataComponents.MAP_ID, set before each Tick. A hand
        // whose displayed stack carries one draws MC's map pose instead of
        // the item (FirstPersonHandsAndItemsRenderer: two-handed in the main
        // hand with the off hand empty, else one-handed), and a change of
        // map id is an equip swap like a change of item.
        void SetHandMapIds(std::optional<int32_t> mainMapId, std::optional<int32_t> offMapId) {
            m_hands[0].pendingMapId = mainMapId;
            m_hands[1].pendingMapId = offMapId;
        }

        // The hands' live stacks, set before each Tick. MC's hand keeps the
        // STACK, and a stack of the same item replaces the visible one at
        // once (shouldInstantlyReplaceVisibleItem) — how a crossbow shows its
        // load (crossbow_arrow / crossbow_firework and the CrossbowItem
        // .isCharged pose) and a firework star its colour, with no re-equip.
        // `mainSlot` / `offSlot`: the hands' inventory indices, the holder
        // slots their stacks are drawn as (ItemRegistry::SetRenderSlot) — so
        // only the hand actually in use shows a pulling bow or crossbow.
        void SetHandStacks(const Game::ItemStack& mainStack, const Game::ItemStack& offStack,
                           int mainSlot, int offSlot) {
            m_hands[0].pendingStack = mainStack;
            m_hands[1].pendingStack = offStack;
            m_handSlots[0] = mainSlot;
            m_handSlots[1] = offSlot;
        }

        // The camera's damage-tilt / death-spin matrix for this frame (MC
        // GameRenderer.bobHurt). Set it alongside Camera::viewTilt — vanilla
        // renders the hand inside the same bobbed pose as the level.
        void SetViewTilt(const glm::mat4& tilt) { m_viewTilt = tilt; }

        // Where the hand's light is read (MC renderHandsWithItems takes
        // EntityRenderDispatcher.getPackedLightCoords(player): the light at
        // the player's eye). Set each frame with the camera's world eye.
        void SetLightProbe(const glm::dvec3& eyeWorld) { m_lightProbe = eyeWorld; m_hasLightProbe = true; }
        // The lightmap colour the hand is drawn with this frame (the portal
        // gun viewmodel takes it too).
        glm::vec3 HandLight() const;

        // The view was REWRITTEN by the game, not turned by the player — a
        // portal crossing rotating the camera through the portal, a
        // teleport. Carry the lagging bob copies along by the same delta so
        // the sway stays whatever it was: without this the next tick reads
        // the whole rotation as one violent flick, and the hand flings
        // across the screen just as the world is meant to look continuous.
        void OnViewRewritten(float deltaPitchDeg, float deltaYawDeg);

        // Per-frame draw of both hands. partialTick is the 0..1 fraction
        // between the previous and next game tick (matches MC's
        // `partialTickTime`). walkDistance is the player's accumulated
        // walked distance in metres (bob phase). aspect is the framebuffer
        // w/h ratio. renderMainHand=false skips the main hand (portal gun
        // viewmodel owns it) but still draws the offhand.
        // `viewPitchDeg` / `viewYawDeg` in MC convention, as for Tick.
        void Render(float aspect, float partialTick, float walkDistance,
                    float viewPitchDeg, float viewYawDeg,
                    bool renderMainHand = true);

        // Hold-to-use pose state — fed per-frame from the local player's
        // predicted use (ClientPlayer.usingItem/usingHand/…). Drives the
        // EAT/DRINK pull-to-mouth and BLOCK raise on WHICHEVER hand is
        // using (mirrors ItemInHandRenderer.renderArmWithItem's
        // useAnimation switch keying on player.getUsedItemHand()).
        void SetUseState(bool usingItem, uint32_t hand,
                         Game::ItemUseAnimation anim,
                         int remainingTicks, int durationTicks) {
            m_useActive    = usingItem;
            m_useHand      = hand;
            m_useAnim      = anim;
            m_useRemaining = remainingTicks;
            m_useDuration  = durationTicks;
        }

        // The rest of AvatarRenderState the first-person hand reads: the
        // riptide in flight (isAutoSpinAttack — both hands take the spin
        // pose) and ticksSinceKineticHitFeedback (a charging spear's recoil
        // on a hit, SpearAnimations.hitFeedbackAmount). Per frame.
        void SetAvatarState(bool autoSpinAttack, float ticksSinceKineticHitFeedback) {
            m_autoSpinAttack = autoSpinAttack;
            m_ticksSinceKineticHitFeedback = ticksSinceKineticHitFeedback;
        }

        // The local player's skin for the arms (Client::PlayerSkins), per
        // frame: a skin look draws MC's bare arm in an empty main hand
        // (renderArmWithItem → renderPlayerArm) — classic or slim, the
        // sleeves as the skin's parts show them — and the map hands in it;
        // INVALID_TEXTURE (the stick figure) draws no bare arm and Steve's
        // map hands. `visible` false: INVISIBILITY (MC isInvisible) hides
        // the bare arm.
        void SetPlayerSkin(TextureHandle skin, bool slim, bool rightSleeve, bool leftSleeve, bool visible) {
            m_armSkin = skin;
            m_armSlim = slim;
            m_armRightSleeve = rightSleeve;
            m_armLeftSleeve = leftSleeve;
            m_armVisible = visible;
        }

    private:
        // ── Per-hand state (advanced by Tick) ───────────────────────
        // Index 0 = main hand (MC HumanoidArm.RIGHT, invert = +1),
        // index 1 = offhand (LEFT, invert = -1 → pure X mirror).
        struct HandState {
            // The item currently being DRAWN. Lags behind the hand's
            // actual item during an equip swap — the real item changes
            // instantly, but `displayed` only flips once the slide-down
            // phase completes (MC's mainHandItem/offHandItem pair).
            Game::ItemID displayed = 0;
            Game::ItemID pending   = 0;
            // The displayed / pending stack's map id (SetHandMapIds).
            std::optional<int32_t> mapId;
            std::optional<int32_t> pendingMapId;
            // The displayed / pending stack (SetHandStacks); `stack` follows
            // the live one while it is the displayed item.
            Game::ItemStack stack;
            Game::ItemStack pendingStack;
            float equipProgress     = 0.0f;   // 0=equipped, 1=fully off-screen
            float equipProgressPrev = 0.0f;
        };
        HandState m_hands[2];
        int       m_handSlots[2] = { -1, -1 };

        // Swing animation (main hand only): 0=idle, ramps to 1 over
        // kSwingTicks ticks when the player starts an attack.
        float m_swingProgress     = 0.0f;
        float m_swingProgressPrev = 0.0f;
        bool  m_swingActive       = false;
        bool  m_swingRequested    = false;   // RequestSwing, consumed by Tick
        // The swing's SwingAnimation (ItemStack.getAttackAnimation of the
        // main hand when it began): a spear STABs for its attack duration,
        // everything else WHACKs for 6 ticks.
        int   m_swingDuration     = 6;
        bool  m_swingStab         = false;

        bool  m_autoSpinAttack = false;
        float m_ticksSinceKineticHitFeedback = 0.0f;
        // entity/trident.png, for the TridentModel drawn in the hand.
        TextureHandle m_tridentTexture = INVALID_TEXTURE;
        bool          m_tridentTextureTried = false;
        // The trident's first-person draw (TridentSpecialRenderer): its model
        // through `pose` (block units, the item's display already applied).
        void RenderTridentModel(int hand, const glm::mat4& pose, float aspect);
        // ShieldSpecialRenderer in the hand (ShieldModel, the stack's sheet).
        void RenderShieldModel(int hand, const glm::mat4& pose, float aspect, const Game::ItemStack& stack);

        // Hold-to-use pose (eat wiggle / shield block). See SetUseState.
        bool                   m_useActive    = false;
        uint32_t               m_useHand      = 0;
        Game::ItemUseAnimation m_useAnim      = Game::ItemUseAnimation::NONE;
        int                    m_useRemaining = 0;
        int                    m_useDuration  = 0;

        // MC GameRenderer.bobHurt's tilt, in view space. The hand shares the
        // level's bobbed pose in vanilla, so it leans and spins with the world
        // rather than staying pinned to the screen. Identity when unhurt.
        glm::mat4 m_viewTilt {1.0f};
        glm::dvec3 m_lightProbe {0.0};
        bool       m_hasLightProbe = false;

        bool m_firstTick = true;

        // ── Look-around sway (MC LocalPlayer.xBob / yBob) ────────────────
        //
        // A lagging copy of the view angles. LocalPlayer.applyInput advances
        // it once per tick by half the remaining gap:
        //     xBob += (getXRot() - xBob) * 0.5F
        // and ItemInHandRenderer rotates the whole hand rig by a tenth of
        // however far the view has outrun it:
        //     mulPose(XP.rotationDegrees((viewXRot - xBob) * 0.1F))
        //     mulPose(YP.rotationDegrees((viewYRot - yBob) * 0.1F))
        //
        // So the item trails the camera on a fast flick and settles back to
        // centre when you stop — the whole effect is those two lines plus the
        // 0.5 follow. Angles are MC-convention degrees.
        float m_xBob = 0.0f, m_xBobPrev = 0.0f;
        float m_yBob = 0.0f, m_yBobPrev = 0.0f;

        // Sway rotation for the current frame, in degrees. Recomputed once per
        // Render() and applied by both the BEWLR and cube/sprite paths, which
        // build their model matrices separately.
        float m_swayPitchDeg = 0.0f;
        float m_swayYawDeg   = 0.0f;

        // Draw one hand. `hand` indexes m_hands; invert = +1 / -1.
        void RenderHand(int hand, float aspect, float partialTick,
                        float walkDistance);
        // A hand holding a map: MC renderTwoHandedMap / renderOneHandedMap
        // with the player's arms (renderMapHand / renderPlayerArm) and
        // renderMap (the paper, the map texture, its decorations).
        void RenderMapHand(int hand, float aspect, float partialTick, float walkDistance);
        // This frame's view pitch (MC xRot) — the two-handed map's tilt.
        float m_viewPitchDeg = 0.0f;
        TextureHandle m_skinTexture = INVALID_TEXTURE;
        bool m_skinTried = false;
        // SetPlayerSkin's (owned by Client::PlayerSkins, not destroyed here).
        TextureHandle m_armSkin = INVALID_TEXTURE;
        bool m_armSlim = false;
        bool m_armRightSleeve = true;
        bool m_armLeftSleeve = true;
        bool m_armVisible = true;
        // The arms' sheet: the player's skin, else Steve (m_skinTexture).
        TextureHandle ArmSkin();
        // MC renderArmWithItem for an empty main hand: renderPlayerArm.
        void RenderEmptyArm(float aspect, float partialTick, float walkDistance);

        bool m_initialized = false;
        ShaderHandle  m_shader        = INVALID_SHADER;
        TextureHandle m_dummyTexture  = INVALID_TEXTURE;
    };

    // Global instance accessed from PlatformMain (mirrors how the
    // portal-gun viewmodel is exposed). Declared in HeldItemRenderer.cpp.
    extern HeldItemRenderer g_heldItemRenderer;

} // namespace Render
