// File: src/client/renderer/gui/SpectatorGui.hpp
//
// MC SpectatorGui + SpectatorMenu + its categories (RootSpectatorMenuCategory,
// TeleportToPlayerMenuCategory, TeleportToTeamMenuCategory, PlayerMenuItem):
// the hotbar a spectator has instead of an inventory.
//
// Closed until a number key, the mouse wheel's "select on hotbar" key
// (middle click) or a hotbar key opens it; then it shows nine slots — the
// current category's items (six a page after the first), a scroll-left
// arrow on later pages, scroll-right in slot 7 and "close" in slot 8. Pressing
// a slot's key selects it; pressing it again uses it. The root category holds
// "Teleport to Player" (every listed non-spectator player, a face each) and
// "Teleport to Team Member" (scoreboard teams — this port has none, so it is
// always the disabled icon, as it is in a vanilla world without teams). The
// hotbar fades 5 s after the last selection and the menu closes when it has.
//
// Choosing a player sends TeleportToEntityC2S (MC
// ServerboundTeleportToEntityPacket). Client main thread only.
#pragma once

#include "GuiGraphics.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Render {

    // A player's face icon: the stick figure's head — the player's colour
    // with its two eyes and smile — as MC's PlayerFaceExtractor draws the
    // skin's face. `size` pixels square at (x, y).
    void DrawPlayerFace(GuiGraphics& g, int x, int y, int size, uint8_t colorId,
                        float alpha = 1.0f, float brightness = 1.0f);

    class SpectatorGui {
    public:
        using TeleportFn = std::function<void(uint32_t playerId)>;
        void SetTeleportCallback(TeleportFn fn) { m_teleport = std::move(fn); }

        // MC SpectatorGui.onHotbarSelected: a hotbar key. Opens the menu the
        // first time, selects (then uses) slots after.
        void OnHotbarSelected(int slot);
        // MC onMouseScrolled: steps the selection over enabled slots.
        void OnMouseScrolled(int wheel);
        // MC onHotbarActionKeyPressed (key.spectatorHotbar, middle click):
        // opens the menu, or uses the selected slot.
        void OnHotbarActionKeyPressed();
        bool IsMenuActive() const { return m_open; }
        // Leaving spectator, or the world: no menu.
        void Reset();

        // MC extractHotbar / extractAction.
        void RenderHotbar(GuiGraphics& g);
        void RenderAction(GuiGraphics& g);

    private:
        enum class ItemKind : uint8_t {
            Empty, Close, ScrollLeft, ScrollRightEnabled, ScrollRightDisabled,
            TeleportToPlayer, TeleportToTeam, Player,
        };
        struct Item {
            ItemKind    kind = ItemKind::Empty;
            uint32_t    playerId = 0;
            std::string name;
            uint8_t     colorId = 0;
        };
        enum class Category : uint8_t { Root, TeleportToPlayer, TeleportToTeam };

        Item        GetItem(int slot) const;           // SpectatorMenu.getItem
        bool        IsEnabled(const Item& item) const;
        std::string NameOf(const Item& item) const;
        std::string Prompt() const;                    // category.getPrompt
        void        SelectSlot(int slot);              // SpectatorMenu.selectSlot
        void        UseItem(const Item& item);         // SpectatorMenuItem.selectItem
        void        SelectCategory(Category category); // SpectatorMenu.selectCategory
        void        OpenMenu();                        // new SpectatorMenu(this)
        void        CloseMenu();                       // onSpectatorMenuClosed
        std::vector<Item> ListedPlayers() const;       // TeleportToPlayerMenuCategory's list
        float       HotbarAlpha() const;
        void        RenderSlot(GuiGraphics& g, int slot, int x, int y, float alpha, const Item& item);

        bool              m_open = false;
        Category          m_category = Category::Root;
        std::vector<Item> m_items;         // the category's items, snapshotted when entered
        int               m_selectedSlot = -1;
        int               m_page = 0;
        int64_t           m_lastSelectionMs = 0;
        TeleportFn        m_teleport;
    };

    SpectatorGui& GetSpectatorGui();

} // namespace Render
