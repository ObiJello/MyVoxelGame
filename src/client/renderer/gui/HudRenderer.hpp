// File: src/client/renderer/gui/HudRenderer.hpp
// Game HUD rendering matching MC's Gui.java.
// Draws hotbar, health, food, armor, XP bar, selected item name, effects.
#pragma once

#include "GuiGraphics.hpp"
#include "common/world/block/Blocks.hpp"
#include <string>

namespace Game {
    struct PlayerPhysics;
    class Inventory;
}

namespace Render {

    class HudRenderer {
    public:
        HudRenderer() = default;

        // Called each frame
        void Render(GuiGraphics& graphics, const Game::Inventory& inventory, float deltaTime);

        // Update selected item tooltip timer (call when selected slot changes)
        void OnSelectedSlotChanged(Game::BlockID blockId);

    private:
        // MC's Gui.java HUD element methods
        void RenderItemHotbar(GuiGraphics& graphics, const Game::Inventory& inventory);
        void RenderSlot(GuiGraphics& graphics, int x, int y,
                       const Game::InventorySlot& slot);
        void RenderSelectedItemName(GuiGraphics& graphics, const Game::Inventory& inventory);
        void RenderPlayerHealth(GuiGraphics& graphics);
        void RenderArmor(GuiGraphics& graphics);
        void RenderFood(GuiGraphics& graphics);
        void RenderAir(GuiGraphics& graphics);
        void RenderExperienceBar(GuiGraphics& graphics);
        void RenderExperienceLevel(GuiGraphics& graphics);

        // State
        int m_toolHighlightTimer = 0;          // Ticks remaining for item name display
        Game::BlockID m_lastHighlightedBlock = Game::BlockID::Air;

        // Placeholder gameplay values (until real systems exist)
        int m_health = 20;        // Half-hearts (20 = full)
        int m_maxHealth = 20;
        int m_food = 20;          // Half-shanks (20 = full)
        int m_armor = 0;          // Armor points (0-20)
        int m_air = 300;          // Air supply (300 = full, ticks)
        int m_maxAir = 300;
        float m_experience = 0.0f; // XP bar progress (0.0-1.0)
        int m_experienceLevel = 0;
        bool m_isUnderWater = false;
    };

} // namespace Render
