// File: src/client/renderer/gui/HudRenderer.hpp
// Game HUD rendering matching MC's Gui.java.
// Draws hotbar, health, food, armor, XP bar, selected item name, effects.
#pragma once

#include "GuiGraphics.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Inventory.hpp"  // brings in InventorySlot alias + Inventory class
#include <string>

namespace Game {
    struct PlayerPhysics;
}

namespace Render {

    class HudRenderer {
    public:
        HudRenderer() = default;

        // Called each frame
        void Render(GuiGraphics& graphics, const Game::Inventory& inventory, float deltaTime);

        // Legacy hook (kept for any caller that still passes a BlockID).
        // The Render() path also auto-detects ItemID changes so no caller
        // is required to invoke this — but it's harmless to call manually.
        void OnSelectedSlotChanged(Game::BlockID blockId);

        // Server-synced stats — fed each frame from ClientPlayer (which is
        // written by SetHealthS2C). Replaces the old fixed placeholders.
        void SetHealth(int health)         { m_health = health; }
        void SetFood(int food)             { m_food = food; }
        void SetSaturation(float sat)      { m_saturation = sat; }
        void SetArmor(int armorPoints)     { m_armor = armorPoints; }
        // Creative/spectator hide the survival stat block (hearts, food,
        // armor, air, XP) — MC Gui gates those on gameMode.canHurtPlayer().
        void SetStatsHidden(bool hidden)   { m_statsHidden = hidden; }

        // ── Attack indicator (MC Gui.renderCrosshair) ─────────────────────
        //
        // `scale` is Player.getAttackStrengthScale(0.0) — note the ZERO, not
        // the 0.5 the damage calculation uses. MC deliberately shows the bar
        // slightly behind the damage it would deal, so a bar that looks full
        // always IS full.
        void SetAttackStrength(float scale, float delayTicks) {
            m_attackStrengthScale = scale;
            m_attackStrengthDelay = delayTicks;
        }
        // True when the crosshair is on a living target — MC only shows the
        // "full" burst when there is something to hit.
        void SetCrosshairOnLivingTarget(bool v) { m_crosshairTarget = v; }

        void RenderAttackIndicator(GuiGraphics& graphics);

    private:
        // MC's Gui.java HUD element methods
        void RenderItemHotbar(GuiGraphics& graphics, const Game::Inventory& inventory);

        float m_attackStrengthScale = 1.0f;
        float m_attackStrengthDelay = 5.0f;
        bool  m_crosshairTarget = false;
        void RenderSlot(GuiGraphics& graphics, int x, int y,
                       const Game::InventorySlot& slot);
        void RenderSelectedItemName(GuiGraphics& graphics, const Game::Inventory& inventory);
        void RenderPlayerHealth(GuiGraphics& graphics);
        void RenderArmor(GuiGraphics& graphics);
        void RenderFood(GuiGraphics& graphics);
        void RenderAir(GuiGraphics& graphics);
        void RenderExperienceBar(GuiGraphics& graphics);
        void RenderExperienceLevel(GuiGraphics& graphics);

        // State — MC's Gui.toolHighlightTimer + lastToolHighlight.
        // Frames remaining for the item-name overlay (MC uses 40 ticks ×
        // notificationDisplayTime, default 2 = 80 ticks; we count down at
        // the render rate to keep things framerate-aware via deltaTime).
        float m_toolHighlightTimer  = 0.0f;
        // Item currently being displayed in the overlay (mirrors MC's
        // `lastToolHighlight`). Set when the player's selected ItemID
        // changes to a non-air item; cleared when the timer expires.
        Game::ItemID m_displayedItem = 0;
        // The selected ItemID we observed last frame. Used to detect
        // "what's in my hand changed" without needing the rest of the
        // game to notify us explicitly.
        Game::ItemID m_lastSelected  = 0;
        bool         m_firstObserve  = true;

        // Gameplay values. health/food/saturation/armor are server-synced via
        // the setters above; the rest stay placeholders until their systems
        // exist (air, XP).
        int m_health = 20;        // Half-hearts (20 = full)
        int m_maxHealth = 20;
        int m_food = 20;          // Half-shanks (20 = full)
        float m_saturation = 5.0f;// Drives MC's saturation heart-jitter (unused yet)
        int m_armor = 0;          // Armor points (0-20)
        int m_air = 300;          // Air supply (300 = full, ticks)
        int m_maxAir = 300;
        bool m_statsHidden = false; // True in creative/spectator (SetStatsHidden)
        float m_experience = 0.0f; // XP bar progress (0.0-1.0)
        int m_experienceLevel = 0;
        bool m_isUnderWater = false;
    };

} // namespace Render
