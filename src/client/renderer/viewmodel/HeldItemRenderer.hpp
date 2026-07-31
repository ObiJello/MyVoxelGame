// File: src/client/renderer/viewmodel/HeldItemRenderer.hpp
// Vanilla-style first-person held-item rendering. Shows whatever item
// is in the player's currently-selected hotbar slot in the lower-right
// of the screen, with:
//   • equip-swap animation (item slides down then new one slides up
//     when you change slots)
//   • swing animation (forward arc when you click)
//   • view-bob (subtle drift driven by walked distance)
//   • per-item-type display transform (BLOCK items rendered as small
//     3D cubes, SPRITE items as voxelised extruded sprites)
//
// Sits alongside the portal-gun viewmodel and is rendered only when
// the selected item is NOT the portal gun (PlatformMain branches).
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
        // caller passes the player's CURRENT selected itemId + whether
        // the player started attacking THIS tick (rising edge); the
        // renderer handles equip-swap and swing onset itself.
        void Tick(Game::ItemID selectedItem, bool attackPressedThisTick);

        // Per-frame draw. partialTick is the 0..1 fraction between the
        // previous and next game tick (matches MC's `partialTickTime`)
        // — used to interpolate animation progress so motion is smooth
        // at any framerate. walkDistance is the player's accumulated
        // walked distance in metres (used by the bob animation). aspect
        // is the framebuffer w/h ratio for the viewmodel projection.
        void Render(float aspect, float partialTick, float walkDistance);

    private:
        // ── State (advanced by Tick) ────────────────────────────────
        // The item currently being DRAWN. Lags behind the player's
        // selection during an equip swap — selectedItem changes
        // instantly, but `m_displayedItem` only flips once the slide-
        // down phase completes.
        Game::ItemID m_displayedItem = 0;
        // The player's actual current selection. When this differs from
        // m_displayedItem, the equip animation drives m_equipProgress
        // from 0 → 1 (slide down), swaps, then 1 → 0 (slide up).
        Game::ItemID m_pendingItem   = 0;

        float m_equipProgress     = 0.0f;   // 0=equipped, 1=fully off-screen
        float m_equipProgressPrev = 0.0f;

        // Swing animation: 0=idle, ramps to 1 over kSwingTicks ticks
        // when the player starts an attack, then snaps back to 0.
        float m_swingProgress     = 0.0f;
        float m_swingProgressPrev = 0.0f;
        bool  m_swingActive       = false;

        bool m_initialized = false;
        ShaderHandle  m_shader        = INVALID_SHADER;
        TextureHandle m_dummyTexture  = INVALID_TEXTURE;
    };

    // Global instance accessed from PlatformMain (mirrors how the
    // portal-gun viewmodel is exposed). Declared in HeldItemRenderer.cpp.
    extern HeldItemRenderer g_heldItemRenderer;

} // namespace Render
