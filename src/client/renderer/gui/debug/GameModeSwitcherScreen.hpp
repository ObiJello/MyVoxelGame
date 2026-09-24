// File: src/client/renderer/gui/debug/GameModeSwitcherScreen.hpp
//
// F3+F4 — MC 26.3's GameModeSwitcherScreen. Holding F3, each F4 press
// moves the selection one slot right (Creative → Survival → Adventure →
// Spectator → …); releasing F3 switches to the hovered mode and closes.
// The mouse can pick a slot too. The default selection is the previous
// game mode, or the "other" of creative/survival.
#pragma once

#include "../screens/Screen.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Render {

    class GameModeSwitcherScreen : public Screen {
    public:
        // `currentMode` / `previousMode` are the 0..3 game-mode bytes
        // (survival, creative, adventure, spectator); previousMode -1 = none.
        GameModeSwitcherScreen(int currentMode, int previousMode, std::function<void(int)> switchTo);

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void RenderBackground(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool MouseClicked(double mx, double my, int button) override;
        bool MouseReleased(double mx, double my, int button) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;
        bool IsPauseScreen() const override { return false; }

        // Driven by DebugKeyHandler from the raw key stream.
        void SelectNext();
        void CommitAndClose();
        static int kIconsMode(int index);

    private:
        // MC GameModeIcon order: CREATIVE, SURVIVAL, ADVENTURE, SPECTATOR.
        struct Icon { const char* name; int mode; };
        static const Icon kIcons[4];
        static constexpr int SLOT_AREA = 26;
        static constexpr int SLOT_PADDING = 5;
        static constexpr int SLOT_AREA_PADDED = 31;
        static constexpr int ALL_SLOTS_WIDTH = 4 * 31 - 5;

        int  SlotAt(double mx, double my) const;
        void RenderIcon(GuiGraphics& g, int slot, int x, int y) const;

        int m_currentMode;
        int m_hovered;         // index into kIcons
        std::function<void(int)> m_switchTo;
        int m_firstMouseX = 0, m_firstMouseY = 0;
        bool m_setFirstMousePos = false;
        bool m_closed = false;
    };

} // namespace Render
