// File: src/client/renderer/gui/CreativeModeInventoryScreen.hpp
//
// The CREATIVE inventory — mirrors
// net.minecraft.client.gui.screens.inventory.CreativeModeInventoryScreen.
//
// A 195x136 panel with two tabs:
//   • Survival Inventory (Tab::Survival) — the player's own slots, re-positioned
//     for this panel, plus the destroy-item square.
//   • Search Items       (Tab::Search)   — every registered item in a scrolling
//     9x5 grid with a text filter, over the hotbar.
//
// MC's real creative screen has one tab per CreativeModeTab; ours has the two
// that matter without a hand-authored category table behind them.
//
// Slot POSITIONS here are this screen's own: Game::InventoryMenu carries MC's
// survival coordinates, and GetSlotPos re-maps them exactly as MC's
// selectTab(INVENTORY) does when it re-wraps the same slots in SlotWrappers.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>
#include <vector>

namespace Render {

    class CreativeModeInventoryScreen : public AbstractContainerScreen {
    public:
        enum class Tab : uint8_t { Survival = 0, Search = 1 };

        // MC pixel constants (CreativeModeInventoryScreen.java)
        static constexpr int IMAGE_W        = 195;
        static constexpr int IMAGE_H        = 136;
        static constexpr int TAB_W          = 26;
        static constexpr int TAB_H          = 32;
        static constexpr int TAB_SPACING    = 27;
        static constexpr int SEARCH_X       = 82;
        static constexpr int SEARCH_Y       = 6;
        static constexpr int SEARCH_W       = 80;
        static constexpr int SEARCH_H       = 9;
        static constexpr int SEARCH_MAX_LEN = 50;
        static constexpr int SCROLLBAR_X    = 175;
        static constexpr int SCROLLBAR_X2   = 189;
        static constexpr int SCROLLBAR_Y    = 18;
        static constexpr int SCROLLBAR_Y2   = 130;
        static constexpr int SCROLL_THUMB_W = 12;
        static constexpr int SCROLL_THUMB_H = 15;
        static constexpr int TRASH_X        = 173;
        static constexpr int TRASH_Y        = 112;
        // Search grid origin + extent, panel-relative.
        static constexpr int GRID_X    = 9;
        static constexpr int GRID_Y    = 18;
        static constexpr int GRID_COLS = 9;
        static constexpr int GRID_ROWS = 5;

        // Screen-specific hit-test results (base defines HIT_NONE / HIT_OUTSIDE).
        static constexpr int HIT_CREATIVE_GRID = -11;
        static constexpr int HIT_TRASH         = -12;
        static constexpr int HIT_TAB_SURVIVAL  = -20;
        static constexpr int HIT_TAB_SEARCH    = -21;
        static constexpr int HIT_SEARCH_BOX    = -30;
        static constexpr int HIT_SCROLLBAR     = -31;

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        // ── Slots ────────────────────────────────────────────────────────
        bool GetSlotPos(int menuIndex, int& outX, int& outY) const override;

        // ── Draw layers ──────────────────────────────────────────────────
        void RenderBehindBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtras(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtraHoverHighlight(GuiGraphics& g, int leftPos, int topPos) override;

        // ── Interaction ──────────────────────────────────────────────────
        int  HitTestExtras(int lx, int ly) override;
        bool HandleExtraClick(int hit, int glfwButton, bool shift) override;
        void HandleExtraRelease() override;
        bool HandleExtraKey(int glfwKey, int glfwMods) override;
        bool HandleExtraCharInput(unsigned int codepoint) override;
        bool HandleExtraScroll(double dy) override;
        void OnExtraMouseMove(int leftPos, int topPos) override;
        const Game::ItemStack* HoveredExtraStack() const override;

        void OnOpen() override;
        void ContainerTick() override;

    private:
        Tab m_currentTab = Tab::Survival;

        // ── Search ───────────────────────────────────────────────────────
        std::string m_searchText;
        int         m_searchCursorPos = 0;
        long long   m_searchFocusedAtMillis = 0;
        bool        m_searchFocused = false;
        bool        m_searchDirty = true;   // refilter on next render
        // Fully-formed stacks (with DataComponents), so per-stack variants —
        // e.g. enchanted_book at every (enchantment, level) — each get their
        // own grid cell, tooltip and foil state.
        std::vector<Game::ItemStack> m_filteredItems;
        void RefreshSearchResults();

        // The search grid shows several stacks of the same item id when an item
        // has per-stack variants, so hover tracks the FULL stack rather than an
        // id — the tooltip needs components like STORED_ENCHANTMENTS.
        Game::ItemStack m_hoveredCreativeStack{};

        // ── Scroll (Search tab only) ─────────────────────────────────────
        float m_scrollOffs  = 0.0f;
        bool  m_isScrolling = false;
        int   GetRowCount() const;       // ceil(filtered/9) - 5
        int   GetRowIndex() const;       // floor(scrollOffs * rowCount + 0.5)
        bool  HasScrollBar() const;

        // ── Backgrounds (lazy-loaded) ────────────────────────────────────
        TextureHandle m_inventoryBg = INVALID_TEXTURE;
        TextureHandle m_searchBg    = INVALID_TEXTURE;
        bool          m_inventoryBgTried = false;
        bool          m_searchBgTried    = false;
        TextureHandle EnsureBackground(bool survival);
        void DrawBackground(GuiGraphics& g, int leftPos, int topPos, TextureHandle bg);

        // MC's layered tab rendering: unselected tabs are drawn BEFORE the
        // panel (so it covers their bottom 4px → "tucked under"), the selected
        // one AFTER (its bottom 4px overlays the panel → "merged").
        void RenderUnselectedTabs(GuiGraphics& g, int leftPos, int topPos);
        void RenderSelectedTab   (GuiGraphics& g, int leftPos, int topPos);
        void RenderSearchBox     (GuiGraphics& g, int leftPos, int topPos);
        void RenderScrollbar     (GuiGraphics& g, int leftPos, int topPos);

        void SwitchTab(Tab t);
    };

    CreativeModeInventoryScreen& GetCreativeInventoryScreen();

} // namespace Render
