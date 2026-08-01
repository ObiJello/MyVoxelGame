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
        void Tick(Game::ItemID mainItem, Game::ItemID offhandItem,
                  bool attackPressedThisTick);

        // Per-frame draw of both hands. partialTick is the 0..1 fraction
        // between the previous and next game tick (matches MC's
        // `partialTickTime`). walkDistance is the player's accumulated
        // walked distance in metres (bob phase). aspect is the framebuffer
        // w/h ratio. renderMainHand=false skips the main hand (portal gun
        // viewmodel owns it) but still draws the offhand.
        void Render(float aspect, float partialTick, float walkDistance,
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
            float equipProgress     = 0.0f;   // 0=equipped, 1=fully off-screen
            float equipProgressPrev = 0.0f;
        };
        HandState m_hands[2];

        // Swing animation (main hand only): 0=idle, ramps to 1 over
        // kSwingTicks ticks when the player starts an attack.
        float m_swingProgress     = 0.0f;
        float m_swingProgressPrev = 0.0f;
        bool  m_swingActive       = false;

        // Hold-to-use pose (eat wiggle / shield block). See SetUseState.
        bool                   m_useActive    = false;
        uint32_t               m_useHand      = 0;
        Game::ItemUseAnimation m_useAnim      = Game::ItemUseAnimation::NONE;
        int                    m_useRemaining = 0;
        int                    m_useDuration  = 0;

        bool m_firstTick = true;

        // Draw one hand. `hand` indexes m_hands; invert = +1 / -1.
        void RenderHand(int hand, float aspect, float partialTick,
                        float walkDistance);

        bool m_initialized = false;
        ShaderHandle  m_shader        = INVALID_SHADER;
        TextureHandle m_dummyTexture  = INVALID_TEXTURE;
    };

    // Global instance accessed from PlatformMain (mirrors how the
    // portal-gun viewmodel is exposed). Declared in HeldItemRenderer.cpp.
    extern HeldItemRenderer g_heldItemRenderer;

} // namespace Render
