// File: src/client/renderer/gui/HudRenderer.cpp
#include "HudRenderer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    // Sprite IDs matching MC's Gui.java constants
    static const char* HOTBAR_SPRITE = "hud/hotbar";
    static const char* HOTBAR_SELECTION_SPRITE = "hud/hotbar_selection";
    static const char* ARMOR_EMPTY_SPRITE = "hud/armor_empty";
    static const char* ARMOR_HALF_SPRITE = "hud/armor_half";
    static const char* ARMOR_FULL_SPRITE = "hud/armor_full";
    static const char* FOOD_EMPTY_SPRITE = "hud/food_empty";
    static const char* FOOD_HALF_SPRITE = "hud/food_half";
    static const char* FOOD_FULL_SPRITE = "hud/food_full";
    static const char* FOOD_EMPTY_HUNGER_SPRITE = "hud/food_empty_hunger";
    static const char* FOOD_HALF_HUNGER_SPRITE = "hud/food_half_hunger";
    static const char* FOOD_FULL_HUNGER_SPRITE = "hud/food_full_hunger";
    static const char* AIR_SPRITE = "hud/air";
    static const char* AIR_POPPING_SPRITE = "hud/air_bursting";
    static const char* AIR_EMPTY_SPRITE = "hud/air_empty";
    static const char* XP_BAR_BG_SPRITE = "hud/experience_bar_background";
    static const char* XP_BAR_PROGRESS_SPRITE = "hud/experience_bar_progress";

    void HudRenderer::Render(GuiGraphics& graphics, const Game::Inventory& inventory, float deltaTime) {
        // Update tooltip timer
        if (m_toolHighlightTimer > 0) {
            m_toolHighlightTimer--;
        }

        // MC render order: crosshair (separate) → hotbar → health/food/armor → XP bar
        RenderItemHotbar(graphics, inventory);

        graphics.NextStratum();

        // Status bars (MC: renderHotbarAndDecorations calls these)
        RenderPlayerHealth(graphics);
        RenderFood(graphics);
        RenderArmor(graphics);

        if (m_isUnderWater || m_air < m_maxAir) {
            RenderAir(graphics);
        }

        RenderExperienceBar(graphics);
        RenderExperienceLevel(graphics);

        // Selected item name tooltip
        RenderSelectedItemName(graphics, inventory);
    }

    void HudRenderer::OnSelectedSlotChanged(Game::BlockID blockId) {
        if (blockId != Game::BlockID::Air) {
            m_toolHighlightTimer = 40 * 2; // 40 ticks * notificationDisplayTime (2 default)
            m_lastHighlightedBlock = blockId;
        }
    }

    // ========================================================================
    // Hotbar (MC: Gui.renderItemHotbar)
    // ========================================================================

    void HudRenderer::RenderItemHotbar(GuiGraphics& graphics, const Game::Inventory& inventory) {
        int screenCenter = graphics.GuiWidth() / 2;
        int bottomY = graphics.GuiHeight();

        // Hotbar background: 182x22, centered at bottom
        graphics.BlitSprite(HOTBAR_SPRITE, screenCenter - 91, bottomY - 22, 182, 22);

        // Selection highlight: 24x23, positioned at selected slot
        int selectedSlot = inventory.GetSelectedSlot();
        graphics.BlitSprite(HOTBAR_SELECTION_SPRITE,
                           screenCenter - 91 - 1 + selectedSlot * 20,
                           bottomY - 22 - 1, 24, 23);

        // Render items in 9 slots
        for (int i = 0; i < 9; i++) {
            int x = screenCenter - 90 + i * 20 + 2;
            int y = bottomY - 16 - 3;
            RenderSlot(graphics, x, y, inventory.GetSlot(Game::Inventory::HotbarToIndex(i)));
        }
    }

    void HudRenderer::RenderSlot(GuiGraphics& graphics, int x, int y,
                                  const Game::InventorySlot& slot) {
        if (slot.IsEmpty()) return;

        // Render item icon first, then decorations (count text) on top
        graphics.RenderItem(slot, x, y);
        graphics.NextStratum(); // Text draws on top of item icon
        graphics.RenderItemDecorations(slot, x, y);
        graphics.NextStratum();
    }

    // ========================================================================
    // Selected Item Name (MC: Gui.renderSelectedItemName)
    // ========================================================================

    void HudRenderer::RenderSelectedItemName(GuiGraphics& graphics, const Game::Inventory& inventory) {
        if (m_toolHighlightTimer <= 0) return;
        if (m_lastHighlightedBlock == Game::BlockID::Air) return;

        // Get block display name from registry (e.g., "Stone", "Grass Block")
        const auto& block = Game::BlockRegistry::Get(m_lastHighlightedBlock);
        std::string name = block.name;
        if (name.empty()) return;

        int strWidth = graphics.GetStringWidth(name);
        int x = (graphics.GuiWidth() - strWidth) / 2;
        int y = graphics.GuiHeight() - 59;

        // Fade out (MC: alpha = timer * 256 / 10, capped at 255)
        int alpha = std::min(255, m_toolHighlightTimer * 256 / 10);
        if (alpha <= 0) return;

        uint32_t color = (static_cast<uint32_t>(alpha) << 24) | 0x00FFFFFF;
        graphics.DrawStringWithBackdrop(name, x, y, strWidth, color);
    }

    // ========================================================================
    // Health Hearts (MC: Gui.renderPlayerHealth)
    // ========================================================================

    void HudRenderer::RenderPlayerHealth(GuiGraphics& graphics) {
        int xLeft = graphics.GuiWidth() / 2 - 91;
        int yBase = graphics.GuiHeight() - 39;

        // MC: 10 heart containers, 9x9 each, 8px horizontal separation
        // Render container (black border background) FIRST for every slot,
        // then overlay the heart on top.
        int heartContainers = (m_maxHealth + 1) / 2; // 10 for 20 HP
        for (int i = 0; i < heartContainers && i < 10; i++) {
            int x = xLeft + i * 8;
            int y = yBase;

            // Always draw container background (has the black outline)
            graphics.BlitSprite("hud/heart/container", x, y, 9, 9);

            // Overlay heart on top
            int halfHearts = i * 2;
            if (halfHearts + 1 < m_health) {
                graphics.BlitSprite("hud/heart/full", x, y, 9, 9);
            } else if (halfHearts + 1 == m_health) {
                graphics.BlitSprite("hud/heart/half", x, y, 9, 9);
            }
        }
    }

    // ========================================================================
    // Armor (MC: Gui.renderArmor)
    // ========================================================================

    void HudRenderer::RenderArmor(GuiGraphics& graphics) {
        if (m_armor <= 0) return;

        int xLeft = graphics.GuiWidth() / 2 - 91;
        int yBase = graphics.GuiHeight() - 49; // Above health

        for (int i = 0; i < 10; i++) {
            int x = xLeft + i * 8;
            int halfPoints = i * 2;

            if (halfPoints + 1 < m_armor) {
                graphics.BlitSprite(ARMOR_FULL_SPRITE, x, yBase, 9, 9);
            } else if (halfPoints + 1 == m_armor) {
                graphics.BlitSprite(ARMOR_HALF_SPRITE, x, yBase, 9, 9);
            } else {
                graphics.BlitSprite(ARMOR_EMPTY_SPRITE, x, yBase, 9, 9);
            }
        }
    }

    // ========================================================================
    // Food (MC: Gui.renderFood — right side, opposite health)
    // ========================================================================

    void HudRenderer::RenderFood(GuiGraphics& graphics) {
        int xRight = graphics.GuiWidth() / 2 + 91;
        int yBase = graphics.GuiHeight() - 39;

        for (int i = 0; i < 10; i++) {
            int x = xRight - i * 8 - 9;

            // Empty background
            graphics.BlitSprite(FOOD_EMPTY_SPRITE, x, yBase, 9, 9);

            // Overlay full/half
            int halfShanks = i * 2;
            if (halfShanks + 1 < m_food) {
                graphics.BlitSprite(FOOD_FULL_SPRITE, x, yBase, 9, 9);
            } else if (halfShanks + 1 == m_food) {
                graphics.BlitSprite(FOOD_HALF_SPRITE, x, yBase, 9, 9);
            }
        }
    }

    // ========================================================================
    // Air Bubbles (MC: above food when underwater)
    // ========================================================================

    void HudRenderer::RenderAir(GuiGraphics& graphics) {
        int xRight = graphics.GuiWidth() / 2 + 91;
        int yBase = graphics.GuiHeight() - 49; // Above food

        int airBubbles = std::min(m_air / 30, 10); // 30 ticks per bubble, max 10

        for (int i = 1; i <= 10; i++) {
            int x = xRight - (i - 1) * 8 - 9;

            if (i <= airBubbles) {
                graphics.BlitSprite(AIR_SPRITE, x, yBase, 9, 9);
            } else {
                graphics.BlitSprite(AIR_EMPTY_SPRITE, x, yBase, 9, 9);
            }
        }
    }

    // ========================================================================
    // Experience Bar (MC: above hotbar, 182x5)
    // ========================================================================

    void HudRenderer::RenderExperienceBar(GuiGraphics& graphics) {
        int screenCenter = graphics.GuiWidth() / 2;
        int y = graphics.GuiHeight() - 32 + 3; // MC positions XP bar here

        // Background (full width)
        graphics.BlitSprite(XP_BAR_BG_SPRITE, screenCenter - 91, y, 182, 5);

        // Progress fill
        if (m_experience > 0.0f) {
            int progressWidth = static_cast<int>(m_experience * 182.0f);
            if (progressWidth > 0) {
                graphics.BlitSprite(XP_BAR_PROGRESS_SPRITE, 182, 5,
                                   0, 0, screenCenter - 91, y, progressWidth, 5);
            }
        }
    }

    // ========================================================================
    // Experience Level Number (MC: centered above XP bar)
    // ========================================================================

    void HudRenderer::RenderExperienceLevel(GuiGraphics& graphics) {
        if (m_experienceLevel <= 0) return;

        std::string levelStr = std::to_string(m_experienceLevel);
        int x = graphics.GuiWidth() / 2;
        int y = graphics.GuiHeight() - 31 - 4;

        // MC renders XP level in green (0xFF80FF20) with black shadow
        graphics.DrawCenteredString(levelStr, x, y, 0xFF80FF20);
    }

} // namespace Render
