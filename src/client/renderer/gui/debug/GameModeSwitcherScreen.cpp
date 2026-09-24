// File: src/client/renderer/gui/debug/GameModeSwitcherScreen.cpp
#include "GameModeSwitcherScreen.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "client/input/KeyMapping.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/Blocks.hpp"

#include <GLFW/glfw3.h>

namespace Render {

    const GameModeSwitcherScreen::Icon GameModeSwitcherScreen::kIcons[4] = {
        { "Creative Mode",  1 },
        { "Survival Mode",  0 },
        { "Adventure Mode", 2 },
        { "Spectator Mode", 3 },
    };

    namespace {
        int IconIndexOf(int mode) {
            for (int i = 0; i < 4; ++i) if (GameModeSwitcherScreen::kIconsMode(i) == mode) return i;
            return 0;
        }
        TextureHandle s_sheet = INVALID_TEXTURE;
        int s_sheetW = 0, s_sheetH = 0;
    }

    int GameModeSwitcherScreen::kIconsMode(int i) { return kIcons[i].mode; }

    GameModeSwitcherScreen::GameModeSwitcherScreen(int currentMode, int previousMode, std::function<void(int)> switchTo)
        : Screen(""), m_currentMode(currentMode), m_switchTo(std::move(switchTo)) {
        // MC getDefaultSelected: the previous mode, else the other of creative/survival.
        int def = previousMode;
        if (def < 0) def = currentMode == 1 ? 0 : 1;
        m_hovered = IconIndexOf(def);
    }

    void GameModeSwitcherScreen::Init() {
        if (s_sheet == INVALID_TEXTURE) {
            s_sheet = LoadStandaloneGuiTexture("assets/textures/gui/container/gamemode_switcher.png", s_sheetW, s_sheetH);
        }
    }

    int GameModeSwitcherScreen::SlotAt(double mx, double my) const {
        const int y = m_height / 2 - 31;
        for (int i = 0; i < 4; ++i) {
            const int x = m_width / 2 - ALL_SLOTS_WIDTH / 2 + i * SLOT_AREA_PADDED;
            if (mx >= x && mx < x + SLOT_AREA && my >= y && my < y + SLOT_AREA) return i;
        }
        return -1;
    }

    void GameModeSwitcherScreen::RenderIcon(GuiGraphics& g, int slot, int x, int y) const {
        // GameModeIcon.renderStack: grass block, iron sword, buried treasure map, ender eye.
        switch (slot) {
            case 0: g.RenderItem(Game::ItemStack(Game::BlockID::Grass, 1), x, y); break;
            case 1: g.RenderItem(Game::ItemStack(Game::Items::IronSword, 1), x, y); break;
            case 2: g.RenderItem(Game::ItemStack(Game::Items::BuriedTreasureMap, 1), x, y); break;
            default: g.RenderItem(Game::ItemStack(Game::Items::EnderEye, 1), x, y); break;
        }
    }

    void GameModeSwitcherScreen::RenderBackground(GuiGraphics& g, int, int, float) {
        // No gradient: MC draws only the switcher sheet (125x75 of a 128x128 texture).
        if (s_sheet == INVALID_TEXTURE) return;
        const int xo = m_width / 2 - 62;
        const int yo = m_height / 2 - 31 - 27;
        g.Blit(s_sheet, xo, yo, xo + 125, yo + 75, 0.0f, 0.0f, 125.0f / 128.0f, 75.0f / 128.0f);
    }

    void GameModeSwitcherScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(kIcons[m_hovered].name, m_width / 2, m_height / 2 - 31 - 20, 0xFFFFFFFF);
        const std::string key = Input::Binds::DebugSwitchGameMode ? Input::Binds::DebugSwitchGameMode->key.DisplayName() : "F4";
        g.DrawCenteredString("\xC2\xA7" "b[ " + key + " ]\xC2\xA7r Next", m_width / 2, m_height / 2 + 5, 0xFFFFFFFF);

        if (!m_setFirstMousePos) {
            m_firstMouseX = mouseX;
            m_firstMouseY = mouseY;
            m_setFirstMousePos = true;
        }
        const bool sameAsFirst = m_firstMouseX == mouseX && m_firstMouseY == mouseY;
        const int y = m_height / 2 - 31;
        for (int i = 0; i < 4; ++i) {
            const int x = m_width / 2 - ALL_SLOTS_WIDTH / 2 + i * SLOT_AREA_PADDED;
            g.BlitSprite("gamemode_switcher/slot", x, y, SLOT_AREA, SLOT_AREA);
            if (m_hovered == i) g.BlitSprite("gamemode_switcher/selection", x, y, SLOT_AREA, SLOT_AREA);
            RenderIcon(g, i, x + 5, y + 5);
            if (!sameAsFirst && SlotAt(mouseX, mouseY) == i) m_hovered = i;
        }
    }

    bool GameModeSwitcherScreen::MouseClicked(double mx, double my, int button) {
        if (button != GLFW_MOUSE_BUTTON_LEFT) return false;
        const int slot = SlotAt(mx, my);
        if (slot >= 0) { m_hovered = slot; return true; }
        return false;
    }

    bool GameModeSwitcherScreen::MouseReleased(double, double, int) { return false; }

    bool GameModeSwitcherScreen::KeyPressed(int glfwKey, int glfwMods) {
        // F4 reaches this screen twice — once through the UI key queue and
        // once through the raw stream DebugKeyHandler drives SelectNext
        // from. Swallow the queued copy here so a press advances once.
        if (Input::Binds::DebugSwitchGameMode && Input::Binds::DebugSwitchGameMode->key == Input::BoundKey::Keyboard(glfwKey)) {
            return true;
        }
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    void GameModeSwitcherScreen::SelectNext() {
        m_setFirstMousePos = false;
        m_hovered = (m_hovered + 1) % 4;
    }

    void GameModeSwitcherScreen::CommitAndClose() {
        if (m_closed) return;
        m_closed = true;
        const int target = kIcons[m_hovered].mode;
        if (target != m_currentMode && m_switchTo) m_switchTo(target);
        OnClose();
    }

} // namespace Render
