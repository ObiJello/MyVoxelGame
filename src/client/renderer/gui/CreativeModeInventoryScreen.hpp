// File: src/client/renderer/gui/CreativeModeInventoryScreen.hpp
//
// The CREATIVE inventory — mirrors
// net.minecraft.client.gui.screens.inventory.CreativeModeInventoryScreen
// (26.3): a 195x136 panel with MC's fourteen tabs (CreativeModeTabs.hpp) —
// the item categories in a scrolling 9x5 picker, Saved Hotbars, Search Items
// with its text filter, Operator Utilities (op + the option) and the Survival
// Inventory — plus the mod tabs on a second page behind NeoForge-style page
// arrows.
//
// Slot POSITIONS here are this screen's own: Game::InventoryMenu carries MC's
// survival coordinates, and GetSlotPos re-maps them exactly as MC's
// selectTab(INVENTORY) does when it re-wraps the same slots in SlotWrappers.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "CreativeModeTabs.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Render {

    class CreativeModeInventoryScreen : public AbstractContainerScreen {
    public:
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
        // Item grid origin + extent, panel-relative (ItemPickerMenu).
        static constexpr int GRID_X    = 9;
        static constexpr int GRID_Y    = 18;
        static constexpr int GRID_COLS = 9;
        static constexpr int GRID_ROWS = 5;
        // NeoForge's page buttons: 20x20 at the panel's top corners, 50 px up.
        static constexpr int PAGE_BUTTON_SIZE = 20;
        static constexpr int PAGE_BUTTON_Y    = -50;
        static constexpr int PAGE_LABEL_Y     = -44;

        // Screen-specific hit-test results (base defines HIT_NONE / HIT_OUTSIDE).
        static constexpr int HIT_CREATIVE_GRID = -11;
        static constexpr int HIT_TRASH         = -12;
        static constexpr int HIT_SEARCH_BOX    = -30;
        static constexpr int HIT_SCROLLBAR     = -31;
        static constexpr int HIT_PAGE_PREV     = -40;
        static constexpr int HIT_PAGE_NEXT     = -41;
        // A tab: HIT_TAB_BASE - (index into CreativeModeTabs::AllTabs()).
        static constexpr int HIT_TAB_BASE      = -1000;

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        // ── Slots ────────────────────────────────────────────────────────
        bool GetSlotPos(int menuIndex, int& outX, int& outY) const override;

        // ── Draw layers ──────────────────────────────────────────────────
        void RenderBehindBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
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
        void DecorateItemTooltip(const Game::ItemStack& stack, std::vector<TooltipLine>& lines) override;

        void OnOpen() override;
        void ContainerTick() override;

    private:
        // MC's `static CreativeModeTab selectedTab`: kept across openings
        // (by key — the tab list is rebuilt when its parameters change).
        std::string m_selectedTabKey = "building_blocks";
        int         m_page = 0;   // NeoForge's current page
        const CreativeModeTab& SelectedTab() const;
        CreativeModeTab::Type  SelectedType() const { return SelectedTab().type; }
        // MC selectTab.
        void SelectTab(const std::string& key);
        void RefreshTabs();   // MC tryRefreshInvalidatedTabs
        void RefreshCurrentTabContents();

        // ── ItemPickerMenu.items ─────────────────────────────────────────
        // Fully-formed stacks (with DataComponents), so per-stack variants —
        // e.g. enchanted_book at every (enchantment, level) — each get their
        // own grid cell, tooltip and foil state.
        // A cell whose stack carries CREATIVE_SLOT_LOCK (the Saved Hotbars
        // tab's "Save hotbar with…" placeholder) is shown but never picked up.
        std::vector<Game::ItemStack> m_items;
        void FillHotbarTab();

        // ── Search ───────────────────────────────────────────────────────
        std::string m_searchText;
        int         m_searchCursorPos = 0;
        // MC EditBox.highlightPos — the selection ANCHOR. The selection is
        // [min(cursor, highlight), max(...)); equal = no selection. Picking a
        // creative slot sets cursor = end, highlight = 0 (CreativeMode-
        // InventoryScreen.slotClicked), so the next keystroke replaces the
        // whole query — type a new search straight after grabbing an item.
        int         m_searchHighlightPos = 0;
        // MC EditBox: moveCursorTo / insertText / deleteChars, selection-aware.
        void SearchMoveCursorTo(int pos, bool extendSelection);
        void SearchInsertText(const std::string& text);
        void SearchDeleteChars(int dir);
        void SearchSelectAll();
        long long   m_searchFocusedAtMillis = 0;
        bool        m_searchFocused = false;
        // MC ignoreTextInput: the key that switched to the search tab (the
        // chat key) or swapped a hotbar slot must not also type a character.
        bool        m_ignoreTextInput = false;
        void RefreshSearchResults();   // MC refreshSearchResults

        // MC SessionSearchTrees: the search tab's stacks indexed by their
        // tooltip text (creativeNameSearch) and item tags (creativeTagSearch),
        // built on first use after the tabs change.
        struct SearchEntry {
            std::vector<std::string> lines;   // lower-cased tooltip lines
            std::string idNamespace;          // "minecraft", "aether", …
            std::string idPath;               // "oak_log"
            std::vector<std::pair<std::string, std::string>> tags;   // (namespace, path)
        };
        std::vector<SearchEntry> m_searchIndex;
        bool m_searchIndexValid = false;
        void EnsureSearchIndex();
        // MC visibleTags: the tags a '#' query matched, listed in the tooltip.
        std::vector<std::string> m_visibleTags;

        // The grid shows several stacks of the same item id when an item has
        // per-stack variants, so hover tracks the FULL stack rather than an
        // id — the tooltip needs components like STORED_ENCHANTMENTS.
        Game::ItemStack m_hoveredCreativeStack{};
        int             m_hoveredCreativeIndex = -1;   // into m_items, -1 when empty

        // ── Scroll (ItemPickerMenu) ──────────────────────────────────────
        float m_scrollOffs  = 0.0f;
        bool  m_isScrolling = false;
        int   CalculateRowCount() const;                // ceil(items/9) - 5
        int   GetRowIndexForScroll(float scroll) const; // max((int)(scroll*rows + 0.5), 0)
        float GetScrollForRowIndex(int row) const;
        bool  CanScroll() const;                        // tab.canScroll && items > 45

        // ── Backgrounds (lazy-loaded per texture) ────────────────────────
        std::unordered_map<std::string, TextureHandle> m_backgrounds;
        TextureHandle EnsureBackground(const std::string& path);

        // ── Tabs ─────────────────────────────────────────────────────────
        bool TabVisibleOnPage(const CreativeModeTab& tab) const;
        int  TabX(const CreativeModeTab& tab) const;   // MC getTabX, panel-relative
        int  TabY(const CreativeModeTab& tab) const;   // MC getTabY
        void RenderTabButton(GuiGraphics& g, int leftPos, int topPos, const CreativeModeTab& tab, bool selected);
        void RenderSearchBox(GuiGraphics& g, int leftPos, int topPos);
        void RenderScrollbar(GuiGraphics& g, int leftPos, int topPos);
        void RenderPageControls(GuiGraphics& g, int leftPos, int topPos);
        void RenderTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my);
        void SetPage(int page);
    };

    CreativeModeInventoryScreen& GetCreativeInventoryScreen();

} // namespace Render
