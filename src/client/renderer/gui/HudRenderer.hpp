// File: src/client/renderer/gui/HudRenderer.hpp
// Game HUD rendering matching MC's Gui.java.
// Draws hotbar, health, food, armor, XP bar, selected item name, effects.
#pragma once

#include "GuiGraphics.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Inventory.hpp"  // brings in InventorySlot alias + Inventory class
#include "common/core/JavaRandom.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include <cstdint>
#include <string>
#include <vector>

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
        // The rest of what MC's Hud reads off the player for the status
        // bars (SetHealthS2C's appended fields, ClientPlayer's copies).
        void SetAbsorption(float amount)   { m_absorption = amount; }
        void SetHudFlags(uint8_t flags)    { m_hudFlags = flags; }
        void SetDamageCooldown(int ticks)  { m_damageCooldownTime = ticks; }
        void SetAir(int air, int maxAir, bool eyesUnderWater) {
            m_air = air; m_maxAir = maxAir; m_isUnderWater = eyesUnderWater;
        }
        // MC Gui.tick: the HUD's own 20 Hz clock. The heart blink, the
        // low-health jitter, the regeneration bob, the hunger shake and the
        // empty-bubble wobble all count in ticks, not frames.
        void Tick() { ++m_tickCount; }
        void SetExperience(float progress, int level) {
            m_experience      = progress;
            m_experienceLevel = level;
        }
        // Creative/spectator hide the survival stat block (hearts, food,
        // armor, air, XP) — MC Gui gates those on gameMode.canHurtPlayer().
        void SetStatsHidden(bool hidden)   { m_statsHidden = hidden; }

        // MAX_HEALTH with HEALTH_BOOST (ClientPlayer::GetMaxHealth) — the
        // heart containers MC's Hud draws from player.getMaxHealth().
        void SetMaxHealth(int maxHealth)   { m_maxHealth = maxHealth; }

        // ── Status effect icons (MC Hud.extractEffects) ───────────────────
        // The local player's effects (a copy of ClientPlayer::activeEffects,
        // taken each frame — a handful of entries). Drawn top-right — beneficial in the first row, the rest
        // in the second — unless an inventory screen shows them itself
        // (Screen.showsActiveEffects).
        void SetActiveEffects(const std::vector<Game::MobEffectInstance>& effects) {
            m_activeEffects = effects;
        }

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

        // ── Underwater overlay (MC ScreenEffectRenderer.submitWater) ───────
        // While the player's eyes are in water: textures/misc/underwater.png
        // tiled four times across the screen, scrolled by the view angles,
        // at alpha 0.1 times the light at the eye. `yaw`/`pitch` are the
        // player's MC rotations, `brightness` the lightmap brightness (0..1).
        void SetWaterOverlay(bool active, float brightness, float yaw, float pitch) {
            m_waterOverlay           = active;
            m_waterOverlayBrightness = brightness;
            m_waterOverlayU          = -yaw / 64.0f;
            m_waterOverlayV          =  pitch / 64.0f;
        }

        // ── Sleep fade (MC Gui.renderSleepOverlay) ────────────────────────
        // The local player's Player.getSleepTimer(): the screen darkens over
        // the first 100 ticks in bed and clears over the 10 after getting up.
        void SetSleepTimer(int ticks) { m_sleepTimer = ticks; }

        // ── Action bar (MC Gui.setOverlayMessage / renderOverlayMessage) ──
        // The line above the hotbar: bed problems, "N/M players sleeping".
        // Shown for 60 ticks, fading over the last 20.
        void SetOverlayMessage(const std::string& text) {
            m_overlayMessage     = text;
            m_overlayMessageTime = 60.0f / 20.0f;   // seconds; ticked at render rate
        }

        void RenderAttackIndicator(GuiGraphics& graphics);

        // MC BossHealthOverlay.render — the bar(s) across the top of the
        // screen. State arrives via BossEventS2C into Client::g_bossBarState.
        void RenderBossBar(GuiGraphics& graphics);

    private:
        // MC's Gui.java HUD element methods
        void RenderItemHotbar(GuiGraphics& graphics, const Game::Inventory& inventory);

        float m_attackStrengthScale = 1.0f;
        float m_attackStrengthDelay = 5.0f;
        bool  m_crosshairTarget = false;
        void RenderSlot(GuiGraphics& graphics, int x, int y,
                       const Game::InventorySlot& slot);
        void RenderSelectedItemName(GuiGraphics& graphics, const Game::Inventory& inventory);
        // MC Hud.extractPlayerHealth: the whole survival stat block — armor
        // row, hearts, food, air — laid out from one health-row count.
        void RenderPlayerHealth(GuiGraphics& graphics);
        void RenderArmor(GuiGraphics& graphics, int yLineBase, int numHealthRows,
                         int healthRowHeight, int xLeft);
        // MC Hud.HeartType: which sheet a heart draws from.
        enum class HeartType { Container, Normal, Poisoned, Withered, Absorbing, Frozen };
        static std::string HeartSprite(HeartType type, bool hardcore, bool half, bool blink);
        void RenderHearts(GuiGraphics& graphics, int xLeft, int yLineBase, int healthRowHeight,
                          int heartOffsetIndex, float maxHealth, int currentHealth, int oldHealth,
                          int absorption, bool blink);
        void RenderFood(GuiGraphics& graphics, int yLineBase, int xRight);
        void RenderAir(GuiGraphics& graphics, int yLineAir, int xRight);
        void RenderExperienceBar(GuiGraphics& graphics);
        void RenderExperienceLevel(GuiGraphics& graphics);
        void RenderSleepOverlay(GuiGraphics& graphics);
        void RenderOverlayMessage(GuiGraphics& graphics);
        void RenderEffects(GuiGraphics& graphics);
        std::vector<Game::MobEffectInstance> m_activeEffects;

        int         m_sleepTimer = 0;
        std::string m_overlayMessage;
        float       m_overlayMessageTime = 0.0f;

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
        // MC Gui.tick compares the hover NAME too (selected.getHoverName()
        // .equals(lastToolHighlight.getHoverName())), so switching between
        // two potions of the same item re-shows the name.
        std::string  m_lastSelectedName;
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
        float   m_absorption = 0.0f;
        uint8_t m_hudFlags = 0;   // SetHealthS2CPacket::kFlag*
        int     m_damageCooldownTime = 0;
        // MC Gui's heart-blink bookkeeping: lastHealth / displayHealth /
        // lastHealthTime / healthBlinkTime, and its tickCount + random.
        int     m_tickCount = 0;
        int     m_lastHealth = 0;
        int     m_displayHealth = 0;
        int64_t m_lastHealthTime = 0;
        int64_t m_healthBlinkTime = 0;
        Game::JavaRandom m_random{0};
        bool m_statsHidden = false; // True in creative/spectator (SetStatsHidden)
        float m_experience = 0.0f; // XP bar progress (0.0-1.0)
        int m_experienceLevel = 0;
        bool m_isUnderWater = false;

        bool  m_waterOverlay = false;
        float m_waterOverlayBrightness = 1.0f;
        float m_waterOverlayU = 0.0f;
        float m_waterOverlayV = 0.0f;
        void RenderWaterOverlay(GuiGraphics& graphics);
    };

} // namespace Render
