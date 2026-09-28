// File: src/client/renderer/gui/CreativeModeInventoryScreen.cpp
#include "CreativeModeInventoryScreen.hpp"
#include "CreativeHotbars.hpp"
#include "EffectsInInventory.hpp"
#include "FontRenderer.hpp"
#include "InventoryScreen.hpp"          // GetSurvivalInventoryScreen (gamemode swap)
#include "GuiGraphics.hpp"
#include "items/PlayerInventoryPreview.hpp"
#include "screens/Screen.hpp"           // LoadStandaloneGuiTexture
#include "screens/Widgets.hpp"          // WidgetDims (page button text colours)
#include "client/entity/Player.hpp"
#include "client/input/KeyMapping.hpp"
#include "client/sound/ClientSounds.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Game::Items::Compass etc.
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <set>

namespace {
    long long NowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // MC toLowerCase(Locale.ROOT) on the query and every indexed string.
    std::string ToLower(std::string_view s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return r;
    }

    std::string Trim(std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return std::string(s.substr(b, e - b));
    }

    bool Contains(const std::string& haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    // "#minecraft:logs" -> ("minecraft", "logs").
    std::pair<std::string, std::string> SplitTag(std::string_view tag) {
        if (!tag.empty() && tag.front() == '#') tag.remove_prefix(1);
        const size_t colon = tag.find(':');
        if (colon == std::string_view::npos) return {"minecraft", std::string(tag)};
        return {std::string(tag.substr(0, colon)), std::string(tag.substr(colon + 1))};
    }

    bool IsBoundKey(const Input::KeyMapping* mapping, int glfwKey) {
        return mapping && mapping->key == Input::BoundKey::Keyboard(glfwKey);
    }

    // The hotbar slot `glfwKey` selects (0..8), -1 for none.
    int HotbarKeyIndex(int glfwKey) {
        for (int i = 0; i < 9; ++i) {
            if (IsBoundKey(Input::Binds::Hotbar[i], glfwKey)) return i;
        }
        return -1;
    }

    constexpr uint32_t kTitleColor   = 0xFF404040u;   // MC -12566464
    constexpr uint32_t kBlue         = 0xFF5555FFu;   // ChatFormatting.BLUE
    constexpr uint32_t kDarkPurple   = 0xFFAA00AAu;   // ChatFormatting.DARK_PURPLE
}

namespace Render {

    CreativeModeInventoryScreen& GetCreativeInventoryScreen() {
        static CreativeModeInventoryScreen s;
        return s;
    }

    // ─── Tab selection ───────────────────────────────────────────
    const CreativeModeTab& CreativeModeInventoryScreen::SelectedTab() const {
        const int index = CreativeModeTabs::IndexOf(m_selectedTabKey);
        if (index < 0) return CreativeModeTabs::DefaultTab();
        return CreativeModeTabs::AllTabs()[static_cast<size_t>(index)];
    }

    void CreativeModeInventoryScreen::OnOpen() {
        // MC init: rebuild the tabs if their parameters moved, then re-select
        // the remembered tab from the default (so a search starts empty).
        CreativeModeTabs::TryRebuildTabContents(
            CreativeModeTabs::CurrentParameters(Player() && Player()->IsCreative()));
        m_searchIndexValid = false;
        const std::string remembered = m_selectedTabKey;
        m_selectedTabKey = CreativeModeTabs::DefaultTab().key;
        m_searchText.clear();
        m_searchCursorPos = 0;
        m_searchHighlightPos = 0;
        SelectTab(remembered);
        if (!SelectedTab().ShouldDisplay() || CreativeModeTabs::IndexOf(remembered) < 0) {
            SelectTab(CreativeModeTabs::DefaultTab().key);
        }
        // NeoForge: open on the first page that holds the selected tab.
        const CreativeModeTab& tab = SelectedTab();
        m_page = tab.onEveryPage ? 0 : tab.page;
        m_isScrolling = false;
        m_ignoreTextInput = false;
        m_hoveredCreativeStack = Game::ItemStack{};
        m_hoveredCreativeIndex = -1;
        // Pre-warm the tab icons' textures so the first render frame can draw
        // them. Without this the search tab's compass appeared one tab-switch
        // late, because the texture create + bind landed in the same frame as
        // the draw.
        for (const CreativeModeTab& t : CreativeModeTabs::AllTabs()) {
            if (!t.icon.IsEmpty()) GuiGraphics::PreloadItem(t.icon.itemId);
        }
    }

    void CreativeModeInventoryScreen::ContainerTick() {
        RefreshTabs();
        // The mirror of InventoryScreen::ContainerTick: a player who loses
        // infinite materials (a /gamemode survival while the picker is open)
        // gets handed the survival panel. Silent on both ends so the cursor
        // stack survives the swap.
        if (Player() && !Player()->IsCreative()) {
            CloseSilently();
            GetSurvivalInventoryScreen().OpenSilently();
        }
    }

    // MC tryRefreshInvalidatedTabs: a rule, permission or option change
    // rebuilt the tabs — refresh what the selected one shows, or fall back to
    // the default when it has nothing left.
    void CreativeModeInventoryScreen::RefreshTabs() {
        if (!CreativeModeTabs::TryRebuildTabContents(
                CreativeModeTabs::CurrentParameters(Player() && Player()->IsCreative()))) {
            return;
        }
        m_searchIndexValid = false;
        const CreativeModeTab& tab = SelectedTab();
        if (CreativeModeTabs::IndexOf(m_selectedTabKey) < 0 ||
            (tab.type == CreativeModeTab::Type::Category && tab.displayItems.empty())) {
            SelectTab(CreativeModeTabs::DefaultTab().key);
            m_page = 0;
        } else {
            RefreshCurrentTabContents();
        }
    }

    // MC refreshCurrentTabContents: new contents, same row.
    void CreativeModeInventoryScreen::RefreshCurrentTabContents() {
        const int oldRow = GetRowIndexForScroll(m_scrollOffs);
        const CreativeModeTab& tab = SelectedTab();
        m_hoveredCreativeStack = Game::ItemStack{};
        m_hoveredCreativeIndex = -1;
        m_items.clear();
        m_locked.clear();
        switch (tab.type) {
            case CreativeModeTab::Type::Search:    RefreshSearchResults(); break;
            case CreativeModeTab::Type::Hotbar:    FillHotbarTab(); break;
            case CreativeModeTab::Type::Category:  m_items = tab.displayItems; break;
            case CreativeModeTab::Type::Inventory: break;
        }
        m_locked.resize(m_items.size(), 0);
        m_scrollOffs = GetScrollForRowIndex(oldRow);
    }

    // MC selectTab.
    void CreativeModeInventoryScreen::SelectTab(const std::string& key) {
        const std::string oldKey = m_selectedTabKey;
        m_selectedTabKey = key;
        m_hoveredCreativeStack = Game::ItemStack{};
        m_hoveredCreativeIndex = -1;
        const CreativeModeTab& tab = SelectedTab();
        m_items.clear();
        m_locked.clear();
        if (tab.type == CreativeModeTab::Type::Hotbar) {
            FillHotbarTab();
        } else if (tab.type == CreativeModeTab::Type::Category) {
            m_items = tab.displayItems;
        }
        m_locked.resize(m_items.size(), 0);

        if (tab.type == CreativeModeTab::Type::Search) {
            // setCanLoseFocus(false) + setFocused(true); a different tab
            // starts an empty query.
            m_searchFocused = true;
            m_searchFocusedAtMillis = NowMillis();
            if (oldKey != key) {
                m_searchText.clear();
                m_searchCursorPos = 0;
                m_searchHighlightPos = 0;
            }
            RefreshSearchResults();
        } else {
            m_searchFocused = false;
            m_searchText.clear();
            m_searchCursorPos = 0;
            m_searchHighlightPos = 0;
            m_visibleTags.clear();
        }
        m_scrollOffs = 0.0f;
        m_isScrolling = false;
    }

    // MC selectTab(HOTBAR): each saved hotbar as a row; an empty one shows a
    // locked paper in its own column naming the keys that save it.
    void CreativeModeInventoryScreen::FillHotbarTab() {
        m_items.clear();
        m_locked.clear();
        auto keyName = [](const Input::KeyMapping* m) { return m ? m->key.DisplayName() : std::string("?"); };
        for (int i = 0; i < CreativeHotbars::kHotbarCount; ++i) {
            if (CreativeHotbars::IsEmpty(i)) {
                for (int y = 0; y < CreativeHotbars::kSlots; ++y) {
                    if (y == i) {
                        Game::ItemStack placeholder(Game::Items::Paper, 1);
                        placeholder.components.set(
                            Game::DataComponents::ITEM_NAME,
                            Game::Text::GetString(Game::Text::Component::Translatable(
                                "inventory.hotbarInfo",
                                {Game::Text::Component::Literal(keyName(Input::Binds::SaveToolbarActivator)),
                                 Game::Text::Component::Literal(keyName(Input::Binds::Hotbar[i]))})));
                        m_items.push_back(std::move(placeholder));
                        m_locked.push_back(1);
                    } else {
                        m_items.emplace_back();
                        m_locked.push_back(0);
                    }
                }
            } else {
                for (const Game::ItemStack& stack : CreativeHotbars::Get(i)) {
                    m_items.push_back(stack);
                    m_locked.push_back(0);
                }
            }
        }
    }

    // ─── Slot layout ─────────────────────────────────────────────
    bool CreativeModeInventoryScreen::GetSlotPos(int menuIndex, int& outX, int& outY) const {
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return false;

        // MC CreativeModeInventoryScreen.selectTab re-wraps every
        // player-inventory slot at a position for THIS panel. Crafting result +
        // grid go to (-2000, -2000) — off-panel, i.e. not drawn or clickable.
        if (menuIndex < Game::Inventory::ARMOR_BEGIN) return false;

        // Every tab but the inventory replaces the panel's contents with the
        // item grid; only the hotbar stays visible beneath it (MC swaps the
        // menu's slot list back to the picker's own). Leaving the main rows
        // hittable here would route clicks on "empty" grid cells into hidden
        // storage slots.
        const bool inventoryTab = SelectedType() == CreativeModeTab::Type::Inventory;
        if (!inventoryTab && !Game::Inventory::IsHotbarSlot(menuIndex)) return false;

        if (Game::Inventory::IsArmorSlot(menuIndex)) {
            const int pos = menuIndex - Game::Inventory::ARMOR_BEGIN;
            outX = 54 + (pos / 2) * 54;
            outY = 6  + (pos % 2) * 27;
            return true;
        }
        if (Game::Inventory::IsOffhandSlot(menuIndex)) {
            outX = 35;
            outY = 20;
            return true;
        }

        // Main rows and hotbar: x = 9 + col*18; y = 54 + row*18, except the
        // hotbar which is pinned to 112 (ItemPickerMenu's addInventoryHotbarSlots
        // and selectTab(INVENTORY) alike).
        const int pos = menuIndex - Game::Inventory::MAIN_BEGIN;
        outX = 9 + (pos % 9) * SLOT_STEP;
        outY = Game::Inventory::IsHotbarSlot(menuIndex) ? 112
                                                        : 54 + (pos / 9) * SLOT_STEP;
        return true;
    }

    // ─── Search ──────────────────────────────────────────────────
    // MC SessionSearchTrees.updateCreativeTooltips / updateCreativeTags: every
    // stack of the search tab with its tooltip lines (TooltipFlag NORMAL,
    // formatting stripped, blank lines dropped), its item id, and its tags.
    void CreativeModeInventoryScreen::EnsureSearchIndex() {
        if (m_searchIndexValid) return;
        m_searchIndexValid = true;
        m_searchIndex.clear();
        const auto& stacks = CreativeModeTabs::SearchTab().displayItems;
        m_searchIndex.reserve(stacks.size());
        std::vector<TooltipLine> lines;
        for (const Game::ItemStack& stack : stacks) {
            SearchEntry entry;
            lines.clear();
            BuildTooltipLines(stack, /*advanced=*/false, lines);
            for (const TooltipLine& line : lines) {
                std::string text = Trim(line.text);
                if (!text.empty()) entry.lines.push_back(ToLower(text));
            }
            entry.idNamespace = CreativeModeTabs::NamespaceOf(stack.itemId);
            entry.idPath = CreativeModeTabs::NameOf(stack.itemId);
            for (const std::string& tag : Game::DataTags::TagsFor(Game::DataTags::Registry::Item, entry.idPath)) {
                entry.tags.push_back(SplitTag(tag));
            }
            m_searchIndex.push_back(std::move(entry));
        }
    }

    // MC refreshSearchResults: the whole search tab for an empty query;
    // otherwise creativeNameSearch (plain text over the tooltip lines, or
    // `namespace:path` over the id) or, after '#', creativeTagSearch — each
    // result in the search tab's own order.
    void CreativeModeInventoryScreen::RefreshSearchResults() {
        m_hoveredCreativeStack = Game::ItemStack{};
        m_hoveredCreativeIndex = -1;
        m_items.clear();
        m_locked.clear();
        m_visibleTags.clear();
        const auto& stacks = CreativeModeTabs::SearchTab().displayItems;
        if (m_searchText.empty()) {
            m_items = stacks;
        } else {
            EnsureSearchIndex();
            std::string term = m_searchText;
            const bool tagSearch = !term.empty() && term.front() == '#';
            if (tagSearch) term.erase(0, 1);
            term = ToLower(term);
            const size_t colon = term.find(':');
            const std::string ns   = colon == std::string::npos ? std::string{} : Trim(term.substr(0, colon));
            const std::string path = colon == std::string::npos ? term : Trim(term.substr(colon + 1));

            if (tagSearch) {
                // updateVisibleTags: every item tag whose id matches.
                std::set<std::string> visible;
                for (const SearchEntry& e : m_searchIndex) {
                    for (const auto& [tagNs, tagPath] : e.tags) {
                        const bool match = colon == std::string::npos
                            ? Contains(tagPath, term)
                            : Contains(tagNs, ns) && Contains(tagPath, path);
                        if (match) visible.insert(tagNs + ":" + tagPath);
                    }
                }
                m_visibleTags.assign(visible.begin(), visible.end());
            }

            for (size_t i = 0; i < m_searchIndex.size() && i < stacks.size(); ++i) {
                const SearchEntry& e = m_searchIndex[i];
                bool match = false;
                if (tagSearch) {
                    // IdSearchTree over the tags: path alone, or namespace AND path.
                    for (const auto& [tagNs, tagPath] : e.tags) {
                        match = colon == std::string::npos
                            ? Contains(tagPath, term)
                            : Contains(tagNs, ns) && Contains(tagPath, path);
                        if (match) break;
                    }
                } else if (colon == std::string::npos) {
                    // FullTextSearchTree.searchPlainText: the tooltip lines.
                    for (const std::string& line : e.lines) {
                        if (Contains(line, term)) { match = true; break; }
                    }
                } else if (Contains(e.idNamespace, ns)) {
                    // searchIdentifier: the namespace, and the path either in
                    // the id or in the tooltip text.
                    match = Contains(e.idPath, path);
                    for (size_t l = 0; !match && l < e.lines.size(); ++l) match = Contains(e.lines[l], path);
                }
                if (match) m_items.push_back(stacks[i]);
            }
        }
        m_locked.assign(m_items.size(), 0);
        m_scrollOffs = 0.0f;
    }

    // ─── Scroll (ItemPickerMenu) ─────────────────────────────────
    int CreativeModeInventoryScreen::CalculateRowCount() const {
        return (static_cast<int>(m_items.size()) + GRID_COLS - 1) / GRID_COLS - GRID_ROWS;
    }
    int CreativeModeInventoryScreen::GetRowIndexForScroll(float scroll) const {
        return std::max(static_cast<int>(static_cast<double>(scroll * static_cast<float>(CalculateRowCount())) + 0.5), 0);
    }
    float CreativeModeInventoryScreen::GetScrollForRowIndex(int row) const {
        const int rows = CalculateRowCount();
        if (rows <= 0) return 0.0f;
        return std::clamp(static_cast<float>(row) / static_cast<float>(rows), 0.0f, 1.0f);
    }
    bool CreativeModeInventoryScreen::CanScroll() const {
        return SelectedTab().canScroll && static_cast<int>(m_items.size()) > GRID_COLS * GRID_ROWS;
    }

    // ─── Tabs ────────────────────────────────────────────────────
    bool CreativeModeInventoryScreen::TabVisibleOnPage(const CreativeModeTab& tab) const {
        return tab.ShouldDisplay() && (tab.onEveryPage || tab.page == m_page);
    }

    int CreativeModeInventoryScreen::TabX(const CreativeModeTab& tab) const {
        // MC getTabX.
        if (tab.alignedRight) return IMAGE_W - TAB_SPACING * (7 - tab.column) + 1;
        return TAB_SPACING * tab.column;
    }

    int CreativeModeInventoryScreen::TabY(const CreativeModeTab& tab) const {
        // MC getTabY.
        return tab.row == CreativeModeTab::Row::Top ? -32 : IMAGE_H;
    }

    void CreativeModeInventoryScreen::SetPage(int page) {
        m_page = std::clamp(page, 0, CreativeModeTabs::PageCount() - 1);
    }

    // ─── Hit testing ─────────────────────────────────────────────
    int CreativeModeInventoryScreen::HitTestExtras(int lx, int ly) {
        // The grid hover is recomputed from scratch every frame, so it can
        // never outlive the cell, the results or the click that emptied it.
        m_hoveredCreativeStack = Game::ItemStack{};
        m_hoveredCreativeIndex = -1;

        // MC checkTabClicked (inclusive bounds), for the tabs on this page.
        const auto& tabs = CreativeModeTabs::AllTabs();
        for (size_t i = 0; i < tabs.size(); ++i) {
            const CreativeModeTab& tab = tabs[i];
            if (!TabVisibleOnPage(tab)) continue;
            const int x = TabX(tab);
            const int y = TabY(tab);
            if (lx >= x && lx <= x + TAB_W && ly >= y && ly <= y + TAB_H) {
                return HIT_TAB_BASE - static_cast<int>(i);
            }
        }

        if (CreativeModeTabs::PageCount() > 1 &&
            ly >= PAGE_BUTTON_Y && ly < PAGE_BUTTON_Y + PAGE_BUTTON_SIZE) {
            if (lx >= 0 && lx < PAGE_BUTTON_SIZE) return HIT_PAGE_PREV;
            if (lx >= IMAGE_W - PAGE_BUTTON_SIZE && lx < IMAGE_W) return HIT_PAGE_NEXT;
        }

        const CreativeModeTab::Type type = SelectedType();
        if (type == CreativeModeTab::Type::Search &&
            lx >= SEARCH_X && lx < SEARCH_X + SEARCH_W &&
            ly >= SEARCH_Y && ly < SEARCH_Y + SEARCH_H) {
            return HIT_SEARCH_BOX;
        }
        // MC insideScrollbar (only a scrolling tab has one).
        if (SelectedTab().canScroll &&
            lx >= SCROLLBAR_X && lx < SCROLLBAR_X2 && ly >= SCROLLBAR_Y && ly < SCROLLBAR_Y2) {
            return HIT_SCROLLBAR;
        }
        if (type != CreativeModeTab::Type::Inventory &&
            lx >= GRID_X - 1 && lx < GRID_X - 1 + GRID_COLS * SLOT_STEP &&
            ly >= GRID_Y - 1 && ly < GRID_Y - 1 + GRID_ROWS * SLOT_STEP) {
            // MC isHovering: each 16x16 slot grown by 1 px on every side, so
            // the 18 px cells tile the grid with no dead gutter.
            const int col = (lx - (GRID_X - 1)) / SLOT_STEP;
            const int row = (ly - (GRID_Y - 1)) / SLOT_STEP;
            {
                const int idx = (GetRowIndexForScroll(m_scrollOffs) + row) * GRID_COLS + col;
                // Always claim the cell — empty cells still need the hover
                // highlight and still absorb clicks (dropping a carried
                // stack on one is the picker's delete gesture).
                if (idx >= 0 && idx < static_cast<int>(m_items.size()) && !m_items[idx].IsEmpty()) {
                    m_hoveredCreativeStack = m_items[idx];
                    m_hoveredCreativeIndex = idx;
                }
                return HIT_CREATIVE_GRID;
            }
        }

        // The destroy-item slot — MC selectTab(INVENTORY) adds it at
        // (173, 112) on the Survival Inventory tab only; the X icon is part
        // of tab_inventory.png.
        if (type == CreativeModeTab::Type::Inventory &&
            lx >= TRASH_X - 1 && lx < TRASH_X + SLOT_SIZE + 1 &&
            ly >= TRASH_Y - 1 && ly < TRASH_Y + SLOT_SIZE + 1) {
            return HIT_TRASH;
        }
        return HIT_NONE;
    }

    const Game::ItemStack* CreativeModeInventoryScreen::HoveredExtraStack() const {
        return &m_hoveredCreativeStack;   // empty unless a grid cell is hovered
    }

    // MC getTooltipFromContainerItem: in a category tab a picker slot's
    // tooltip is the item's own; anywhere else each displayed tab holding the
    // stack is named under the item name in blue, and in the search tab the
    // matched '#' tags the stack carries in dark purple.
    void CreativeModeInventoryScreen::DecorateItemTooltip(const Game::ItemStack& stack,
                                                          std::vector<TooltipLine>& lines) {
        if (lines.empty()) return;
        const bool creativeSlot = HoveredSlot() == HIT_CREATIVE_GRID;
        const CreativeModeTab::Type type = SelectedType();
        if (type == CreativeModeTab::Type::Category && creativeSlot) return;
        if (type == CreativeModeTab::Type::Search && creativeSlot && !m_visibleTags.empty()) {
            const std::string path = CreativeModeTabs::NameOf(stack.itemId);
            for (const std::string& tag : m_visibleTags) {
                if (Game::DataTags::HasTag(Game::DataTags::Registry::Item, path, tag)) {
                    lines.insert(lines.begin() + 1, TooltipLine{"#" + tag, kDarkPurple});
                }
            }
        }
        size_t at = 1;
        for (const CreativeModeTab* tab : CreativeModeTabs::Tabs()) {
            if (tab->type != CreativeModeTab::Type::Search && tab->Contains(stack)) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at++),
                             TooltipLine{tab->DisplayName(), kBlue});
            }
        }
    }

    // ─── Input ───────────────────────────────────────────────────
    bool CreativeModeInventoryScreen::HandleExtraClick(int hit, int glfwButton, bool shift) {
        if (hit <= HIT_TAB_BASE) {
            const auto& tabs = CreativeModeTabs::AllTabs();
            const size_t index = static_cast<size_t>(HIT_TAB_BASE - hit);
            if (glfwButton == GLFW_MOUSE_BUTTON_LEFT && index < tabs.size()) SelectTab(tabs[index].key);
            return true;
        }
        if (hit == HIT_PAGE_PREV || hit == HIT_PAGE_NEXT) {
            const int target = m_page + (hit == HIT_PAGE_NEXT ? 1 : -1);
            if (glfwButton == GLFW_MOUSE_BUTTON_LEFT && target >= 0 && target < CreativeModeTabs::PageCount()) {
                Client::Sounds::PlayButtonClick();
                SetPage(target);
            }
            return true;
        }

        // A click IN the box collapses any selection onto the cursor
        // (EditBox.onClick → moveCursorTo(…, shift)); the box never loses
        // focus on the search tab (setCanLoseFocus(false)).
        if (hit == HIT_SEARCH_BOX) {
            m_searchFocused = true;
            SearchMoveCursorTo(m_searchCursorPos, shift);
            return true;
        }

        if (hit == HIT_SCROLLBAR) {
            m_isScrolling = CanScroll();
            return true;
        }

        if (hit == HIT_CREATIVE_GRID) {
            // MC slotClicked on a creative slot: moveCursorToEnd(false) +
            // setHighlightPos(0) — the query is selected, so backspace or a
            // letter replaces it.
            if (SelectedType() == CreativeModeTab::Type::Search) SearchSelectAll();
            const int idx = m_hoveredCreativeIndex < static_cast<int>(m_items.size()) ? m_hoveredCreativeIndex : -1;
            // CustomCreativeSlot.mayPickup: a locked placeholder does nothing.
            if (idx >= 0 && idx < static_cast<int>(m_locked.size()) && m_locked[idx]) return true;

            const Game::ItemStack& carried = Carried();
            const Game::ItemStack  clicked = idx >= 0 ? m_items[idx] : Game::ItemStack{};
            auto pick = [&](uint8_t button) {
                QueueClick(Network::ContainerInput::PICKUP, Network::InventorySlotSentinel::CREATIVE_GRID,
                           button, clicked.itemId, &clicked);
            };
            auto shrinkOrClear = [&](uint8_t button) {   // 0 = clear the cursor, 1 = one fewer
                QueueClick(Network::ContainerInput::CREATIVE_DELETE_CARRIED,
                           Network::InventorySlotSentinel::OUTSIDE, button);
            };

            // CLONE (middle click): a full stack onto an empty cursor.
            if (glfwButton == GLFW_MOUSE_BUTTON_MIDDLE) {
                if (carried.IsEmpty() && !clicked.IsEmpty()) pick(0);
                return true;
            }
            const int button = glfwButton == GLFW_MOUSE_BUTTON_RIGHT ? 1 : 0;
            // MC's four cases, expressed in the creative-grid actions:
            //   PICKUP 0 = cursor becomes a full stack of the cell,
            //   PICKUP 1 = one of it (or one more when it is the same stack),
            //   PICKUP 2 = the cell's stack as shown (a saved hotbar's count),
            //   CREATIVE_DELETE_CARRIED 0/1 = clear the cursor / one fewer.
            if (!carried.IsEmpty() && !clicked.IsEmpty() && Game::IsSameItemSameComponents(carried, clicked)) {
                if (button == 0) {
                    const int maxStack = Game::ItemRegistry::Get(carried.itemId).maxStackSize;
                    if (shift) pick(0);
                    else if (carried.count < maxStack) pick(1);
                } else {
                    shrinkOrClear(1);
                }
            } else if (!clicked.IsEmpty() && carried.IsEmpty()) {
                if (shift) pick(0);
                else pick(clicked.count > 1 ? 2 : 1);
            } else if (button == 0) {
                if (!carried.IsEmpty()) shrinkOrClear(0);
            } else if (!carried.IsEmpty()) {
                shrinkOrClear(1);
            }
            return true;
        }

        // Destroy-item slot:
        //   • Shift+click → clear ALL inventory slots (MC slotClicked:
        //     `if (slot == this.destroyItemSlot && quickKey)`).
        //   • Plain click with a carried stack → DELETE the cursor
        //     (`menu.setCarried(ItemStack.EMPTY)` — no drop, either button).
        if (hit == HIT_TRASH) {
            if (shift) {
                QueueClick(Network::ContainerInput::CREATIVE_DESTROY_ALL,
                           Network::InventorySlotSentinel::OUTSIDE, 0);
            } else if (!Carried().IsEmpty()) {
                QueueClick(Network::ContainerInput::CREATIVE_DELETE_CARRIED,
                           Network::InventorySlotSentinel::OUTSIDE,
                           0 /*whole cursor*/);
            }
            return true;
        }

        return false;   // let the base handle real slots and outside-clicks
    }

    void CreativeModeInventoryScreen::HandleExtraRelease() {
        m_isScrolling = false;
    }

    // ── Search box editing — MC EditBox, selection included ──────────────

    void CreativeModeInventoryScreen::SearchMoveCursorTo(int pos, bool extendSelection) {
        m_searchCursorPos = std::clamp(pos, 0, (int)m_searchText.size());
        if (!extendSelection) m_searchHighlightPos = m_searchCursorPos;
        m_searchFocusedAtMillis = NowMillis();
    }

    void CreativeModeInventoryScreen::SearchSelectAll() {
        // moveCursorToEnd(false) then setHighlightPos(0).
        SearchMoveCursorTo((int)m_searchText.size(), false);
        m_searchHighlightPos = 0;
    }

    void CreativeModeInventoryScreen::SearchInsertText(const std::string& input) {
        // MC insertText: the selection (or the empty span at the cursor) is
        // replaced, the cursor lands after the text, the selection collapses.
        const int start = std::min(m_searchCursorPos, m_searchHighlightPos);
        const int end   = std::max(m_searchCursorPos, m_searchHighlightPos);
        const int room  = SEARCH_MAX_LEN - (int)m_searchText.size() + (end - start);
        if (room <= 0) return;
        const std::string text = input.substr(0, (size_t)room);
        const std::string old = m_searchText;
        m_searchText.replace((size_t)start, (size_t)(end - start), text);
        SearchMoveCursorTo(start + (int)text.size(), false);
        // MC charTyped / keyPressed: refresh only when the value changed.
        if (m_searchText != old) RefreshSearchResults();
    }

    void CreativeModeInventoryScreen::SearchDeleteChars(int dir) {
        // MC deleteChars → deleteCharsToPos: a selection is deleted whole,
        // otherwise one character in `dir`.
        if (m_searchText.empty()) return;
        if (m_searchHighlightPos != m_searchCursorPos) {
            SearchInsertText("");
            return;
        }
        const int pos   = std::clamp(m_searchCursorPos + dir, 0, (int)m_searchText.size());
        const int start = std::min(pos, m_searchCursorPos);
        const int end   = std::max(pos, m_searchCursorPos);
        if (start == end) return;
        m_searchText.erase((size_t)start, (size_t)(end - start));
        SearchMoveCursorTo(start, false);
        RefreshSearchResults();
    }

    bool CreativeModeInventoryScreen::HandleExtraKey(int glfwKey, int glfwMods) {
        // MC keyPressed: every press starts by clearing ignoreTextInput.
        m_ignoreTextInput = false;
        const CreativeModeTab::Type type = SelectedType();

        // AbstractContainerScreen.checkHotbarKeyPressed, run first: a hotbar
        // key over a hovered slot with an empty cursor swaps. Over a picker
        // cell that is MC's creative SWAP — the hotbar slot takes a full
        // stack of the cell (a creative slot write, CREATIVE_FILL_SLOT 0).
        auto checkHotbarKeyPressed = [&]() -> bool {
            const int hotbar = HotbarKeyIndex(glfwKey);
            if (hotbar < 0 || !Carried().IsEmpty()) return false;
            if (HoveredSlot() == HIT_CREATIVE_GRID) {
                if (m_hoveredCreativeIndex < 0 || m_hoveredCreativeIndex >= static_cast<int>(m_items.size())) {
                    return false;
                }
                if (!m_locked[static_cast<size_t>(m_hoveredCreativeIndex)]) {
                    const Game::ItemStack cell = m_items[static_cast<size_t>(m_hoveredCreativeIndex)];
                    QueueClick(Network::ContainerInput::CREATIVE_FILL_SLOT,
                               static_cast<int16_t>(Game::Inventory::HotbarToIndex(hotbar)), 0,
                               cell.itemId, &cell);
                }
                return true;
            }
            if (HoveredSlot() >= 0) {
                QueueClick(Network::ContainerInput::SWAP, static_cast<int16_t>(HoveredSlot()),
                           static_cast<uint8_t>(hotbar));
                return true;
            }
            return false;
        };

        if (type != CreativeModeTab::Type::Search) {
            // The chat key jumps to the search tab (and types nothing).
            if (IsBoundKey(Input::Binds::Chat, glfwKey)) {
                m_ignoreTextInput = true;
                SelectTab("search");
                if (!SelectedTab().onEveryPage) m_page = SelectedTab().page;
                return true;
            }
            return checkHotbarKeyPressed();
        }

        // Search tab: a hotbar key swaps unless the cursor rests on an empty
        // picker cell (then the digit is typed).
        const bool doQuickSwap = HoveredSlot() != HIT_CREATIVE_GRID || m_hoveredCreativeIndex >= 0;
        if (doQuickSwap && checkHotbarKeyPressed()) {
            m_ignoreTextInput = true;
            return true;
        }

        const bool shift = (glfwMods & GLFW_MOD_SHIFT) != 0;
        // MC isSelectAll: Ctrl+A (Cmd+A on macOS).
        if (glfwKey == GLFW_KEY_A && (glfwMods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0) {
            SearchSelectAll();
            return true;
        }
        if (glfwKey == GLFW_KEY_BACKSPACE) { SearchDeleteChars(-1); return true; }
        if (glfwKey == GLFW_KEY_DELETE)    { SearchDeleteChars(1);  return true; }
        if (glfwKey == GLFW_KEY_LEFT)  { SearchMoveCursorTo(m_searchCursorPos - 1, shift); return true; }
        if (glfwKey == GLFW_KEY_RIGHT) { SearchMoveCursorTo(m_searchCursorPos + 1, shift); return true; }
        if (glfwKey == GLFW_KEY_HOME)  { SearchMoveCursorTo(0, shift); return true; }
        if (glfwKey == GLFW_KEY_END)   { SearchMoveCursorTo((int)m_searchText.size(), shift); return true; }

        // MC: `searchBox.capturesInput() && !event.isEscape()` — the focused
        // box keeps every other key (ESC was handled before this), so the
        // letter that is also the inventory, drop or offhand key types
        // instead of acting; OnCharInput appends it.
        return true;
    }

    bool CreativeModeInventoryScreen::HandleExtraCharInput(unsigned int codepoint) {
        if (SelectedType() != CreativeModeTab::Type::Search) return false;
        if (m_ignoreTextInput) {
            m_ignoreTextInput = false;
            return true;
        }
        if (codepoint < 32 || codepoint >= 127) return true;
        SearchInsertText(std::string(1, (char)codepoint));
        return true;
    }

    bool CreativeModeInventoryScreen::HandleExtraScroll(double dy) {
        // MC mouseScrolled → ItemPickerMenu.subtractInputFromScroll.
        if (!CanScroll()) return false;
        const int rows = CalculateRowCount();
        if (rows <= 0) return false;
        m_scrollOffs = std::clamp(m_scrollOffs - static_cast<float>(dy / static_cast<double>(rows)), 0.0f, 1.0f);
        return true;
    }

    void CreativeModeInventoryScreen::OnExtraMouseMove(int /*leftPos*/, int topPos) {
        if (!m_isScrolling) return;
        // MC mouseDragged.
        const float top    = (float)(topPos + SCROLLBAR_Y);
        const float bottom = top + (float)(SCROLLBAR_Y2 - SCROLLBAR_Y);
        m_scrollOffs = std::clamp((MouseGui().y - top - 7.5f) / ((bottom - top) - 15.0f), 0.0f, 1.0f);
    }

    // ─── Background ──────────────────────────────────────────────
    TextureHandle CreativeModeInventoryScreen::EnsureBackground(const std::string& path) {
        auto it = m_backgrounds.find(path);
        if (it != m_backgrounds.end()) return it->second;
        int w = 0, h = 0;
        const TextureHandle handle = LoadStandaloneGuiTexture(path.c_str(), w, h);
        m_backgrounds.emplace(path, handle);
        return handle;
    }

    // ─── Tab chrome ──────────────────────────────────────────────
    // MC extractTabButton: tab_{top,bottom}_{selected,unselected}_<column+1>.
    void CreativeModeInventoryScreen::RenderTabButton(GuiGraphics& g, int leftPos, int topPos,
                                                      const CreativeModeTab& tab, bool selected) {
        const bool isTop = tab.row == CreativeModeTab::Row::Top;
        const int  x = leftPos + TabX(tab);
        const int  y = isTop ? topPos - 28 : topPos + IMAGE_H - 4;
        const int  sprite = std::clamp(tab.column, 0, 6) + 1;
        const std::string name = std::string("container/creative_inventory/tab_") + (isTop ? "top_" : "bottom_") +
                                 (selected ? "selected_" : "unselected_") + std::to_string(sprite);
        g.BlitSprite(name, x, y, TAB_W, TAB_H);
    }

    // The icon centred on the tab and nudged one pixel towards the panel
    // (MC: x + 13 - 8, y + 16 - 8 + (isTop ? 1 : -1)).
    static void RenderTabIcon(GuiGraphics& g, const CreativeModeTab& tab, int tabX, int tabY) {
        const bool isTop = tab.row == CreativeModeTab::Row::Top;
        g.RenderItem(tab.icon, tabX + 5, tabY + 8 + (isTop ? 1 : -1));
    }

    // ─── Draw layers ─────────────────────────────────────────────
    void CreativeModeInventoryScreen::RenderBehindBg(GuiGraphics& g, int leftPos, int topPos) {
        // MC extractBackground: every displayed tab but the selected one
        // under the panel (it covers their last 4 px). Icons go one stratum
        // up — within a stratum blits may batch out of submission order, and
        // an icon under its own tab background disappears.
        const std::string& selectedKey = SelectedTab().key;
        std::vector<const CreativeModeTab*> drawn;
        for (const CreativeModeTab& tab : CreativeModeTabs::AllTabs()) {
            if (tab.key == selectedKey || !TabVisibleOnPage(tab)) continue;
            RenderTabButton(g, leftPos, topPos, tab, false);
            drawn.push_back(&tab);
        }
        g.NextStratum();
        for (const CreativeModeTab* tab : drawn) {
            const bool isTop = tab->row == CreativeModeTab::Row::Top;
            RenderTabIcon(g, *tab, leftPos + TabX(*tab), isTop ? topPos - 28 : topPos + IMAGE_H - 4);
        }
    }

    void CreativeModeInventoryScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const CreativeModeTab& tab = SelectedTab();
        const TextureHandle bg = EnsureBackground(tab.backgroundTexture);
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            // 256x256 PNGs; the panel is the top-left 195x136.
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, (float)IMAGE_W / 256.0f, (float)IMAGE_H / 256.0f);
        }
        if (tab.type != CreativeModeTab::Type::Inventory) return;

        Game::ClientPlayer* player = Player();
        if (!player) return;

        // Player preview — MC extractEntityInInventoryFollowsMouse(graphics,
        // leftPos+73, topPos+6, leftPos+105, topPos+49, 20, 0.0625F, xm, ym,
        // player). Same rect, same cursor-tracking math.
        g.NextStratum();
        StickFigurePose pose;
        pose.bodyYawDeg   = player->visualYaw;
        pose.headYawDeg   = player->visualYaw;
        pose.headPitchDeg = player->visualPitch;
        pose.isCrouching  = false;
        pose.isSitting    = player->IsPassenger();
        RenderStickFigureInInventory(
            g,
            leftPos + 73, topPos + 6, leftPos + 105, topPos + 49,
            20, 0.0625f,
            MouseGui().x, MouseGui().y,
            pose,
            player->color);
    }

    void CreativeModeInventoryScreen::RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) {
        if (SelectedType() == CreativeModeTab::Type::Inventory) return;
        const int rowIndex = GetRowIndexForScroll(m_scrollOffs);
        for (int row = 0; row < GRID_ROWS; ++row) {
            for (int col = 0; col < GRID_COLS; ++col) {
                const int idx = (rowIndex + row) * GRID_COLS + col;
                if (idx < 0 || idx >= (int)m_items.size() || m_items[idx].IsEmpty()) continue;
                // The pre-built stack carries its DataComponents (an enchanted
                // book's STORED_ENCHANTMENTS), which RenderItem and its glint
                // pass read for foil.
                const auto& stack = m_items[idx];
                const int x = leftPos + GRID_X + col * SLOT_STEP;
                const int y = topPos  + GRID_Y + row * SLOT_STEP;
                g.RenderItem(stack, x, y);
                g.NextStratum();
                g.RenderItemDecorations(stack, x, y);
            }
        }
    }

    void CreativeModeInventoryScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // MC extractLabels: the tab's title at (8, 6), dark grey, no shadow.
        const CreativeModeTab& tab = SelectedTab();
        if (tab.showTitle) g.DrawString(tab.DisplayName(), leftPos + 8, topPos + 6, kTitleColor, false);
    }

    void CreativeModeInventoryScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        const CreativeModeTab& selected = SelectedTab();
        if (selected.type == CreativeModeTab::Type::Search) RenderSearchBox(g, leftPos, topPos);
        if (selected.canScroll) RenderScrollbar(g, leftPos, topPos);

        // The selected tab over the panel (its bottom 4 px merge into it) —
        // drawn only on a page that shows it.
        if (TabVisibleOnPage(selected)) {
            g.NextStratum();
            RenderTabButton(g, leftPos, topPos, selected, true);
            g.NextStratum();
            const bool isTop = selected.row == CreativeModeTab::Row::Top;
            RenderTabIcon(g, selected, leftPos + TabX(selected), isTop ? topPos - 28 : topPos + IMAGE_H - 4);
        }

        RenderPageControls(g, leftPos, topPos);

        // MC CreativeModeInventoryScreen's EffectsInInventory column.
        if (Player()) {
            g.NextStratum();
            EffectsInInventory::Render(g, Player()->activeEffects, g.GuiWidth(), leftPos, topPos,
                                       IMAGE_W, static_cast<int>(MouseGui().x),
                                       static_cast<int>(MouseGui().y));
        }

        // MC extractRenderState: the hovered tab's name (checkTabHovering's
        // isHovering(x + 3, y + 3, 21, 27), which widens the box by one pixel
        // each side), or "Destroy Item" over the bin.
        const int mx = static_cast<int>(std::floor(MouseGui().x));
        const int my = static_cast<int>(std::floor(MouseGui().y));
        for (const CreativeModeTab& tab : CreativeModeTabs::AllTabs()) {
            if (!TabVisibleOnPage(tab)) continue;
            const int x = leftPos + TabX(tab) + 3;
            const int y = topPos + TabY(tab) + 3;
            if (mx >= x - 1 && mx < x + 21 + 1 && my >= y - 1 && my < y + 27 + 1) {
                g.NextStratum();
                RenderTextTooltip(g, tab.DisplayName(), mx, my);
                return;
            }
        }
        if (HoveredSlot() == HIT_TRASH) {
            g.NextStratum();
            RenderTextTooltip(g, Game::Language::GetOrDefault("inventory.binSlot", "Destroy Item"), mx, my);
        }
    }

    // NeoForge CreativeModeInventoryScreen: "<" / ">" buttons at the panel's
    // top corners and the "page / pages" label between them.
    void CreativeModeInventoryScreen::RenderPageControls(GuiGraphics& g, int leftPos, int topPos) {
        const int pages = CreativeModeTabs::PageCount();
        if (pages <= 1) return;
        g.NextStratum();
        const int mx = static_cast<int>(std::floor(MouseGui().x));
        const int my = static_cast<int>(std::floor(MouseGui().y));
        const int y = topPos + PAGE_BUTTON_Y;
        auto button = [&](int x, const char* label, bool active) {
            const bool hovered = mx >= x && mx < x + PAGE_BUTTON_SIZE && my >= y && my < y + PAGE_BUTTON_SIZE;
            const char* sprite = !active ? "widget/button_disabled"
                               : hovered ? "widget/button_highlighted" : "widget/button";
            g.BlitSprite(sprite, x, y, PAGE_BUTTON_SIZE, PAGE_BUTTON_SIZE);
            g.DrawCenteredString(label, x + PAGE_BUTTON_SIZE / 2,
                                 y + (PAGE_BUTTON_SIZE - FontRenderer::LINE_HEIGHT) / 2 + 1,
                                 static_cast<uint32_t>(active ? WidgetDims::TEXT_COLOR_ACTIVE
                                                              : WidgetDims::TEXT_COLOR_INACTIVE));
        };
        button(leftPos, "<", m_page > 0);
        button(leftPos + IMAGE_W - PAGE_BUTTON_SIZE, ">", m_page < pages - 1);
        const std::string label = std::to_string(m_page + 1) + " / " + std::to_string(pages);
        g.DrawString(label, leftPos + IMAGE_W / 2 - g.GetStringWidth(label) / 2, topPos + PAGE_LABEL_Y,
                     0xFFFFFFFFu, true);
    }

    // A one-line tooltip in MC's tooltip frame (the item tooltip's style).
    void CreativeModeInventoryScreen::RenderTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my) {
        if (text.empty()) return;
        const int textW = g.GetStringWidth(text);
        const int totalH = 8;
        int x = mx + 12;
        const int y = my - 12;
        if (x + textW + 4 > g.GuiWidth()) x = std::max(4, mx - 16 - textW);
        const uint32_t bg     = 0xF0100010;
        const uint32_t border = 0x505000FF;
        g.Fill(x - 3, y - 4,           x + textW + 3, y - 3,           bg);
        g.Fill(x - 3, y + totalH + 3,  x + textW + 3, y + totalH + 4,  bg);
        g.Fill(x - 3, y - 3,           x + textW + 3, y + totalH + 3,  bg);
        g.Fill(x - 4, y - 3,           x - 3,         y + totalH + 3,  bg);
        g.Fill(x + textW + 3, y - 3,   x + textW + 4, y + totalH + 3,  bg);
        g.Fill(x - 3,         y - 3 + 1, x - 3 + 1,     y + totalH + 3 - 1, border);
        g.Fill(x + textW + 2, y - 3 + 1, x + textW + 3, y + totalH + 3 - 1, border);
        g.Fill(x - 3,         y - 3,     x + textW + 3, y - 3 + 1,          border);
        g.Fill(x - 3,         y + totalH + 2, x + textW + 3, y + totalH + 3, border);
        g.NextStratum();
        g.DrawString(text, x, y, 0xFFFFFFFFu, true);
    }

    void CreativeModeInventoryScreen::RenderExtraHoverHighlight(GuiGraphics& g,
                                                                int leftPos, int topPos) {
        if (HoveredSlot() == HIT_TRASH) {
            RenderHoverHighlight(g, leftPos + TRASH_X, topPos + TRASH_Y, /*front=*/true);
            return;
        }
        if (HoveredSlot() != HIT_CREATIVE_GRID) return;

        // Every hovered picker cell, occupied or empty — an empty one takes
        // clicks too (a carried stack dropped on it is deleted).
        const int mx  = (int)std::floor(MouseGui().x) - leftPos;
        const int my  = (int)std::floor(MouseGui().y) - topPos;
        const int col = (mx - GRID_X) / SLOT_STEP;
        const int row = (my - GRID_Y) / SLOT_STEP;
        RenderHoverHighlight(g,
                             leftPos + GRID_X + col * SLOT_STEP,
                             topPos  + GRID_Y + row * SLOT_STEP,
                             /*front=*/true);
    }

    void CreativeModeInventoryScreen::RenderSearchBox(GuiGraphics& g, int leftPos, int topPos) {
        const int x = leftPos + SEARCH_X;
        const int y = topPos  + SEARCH_Y;
        // The box itself is already painted by tab_item_search.png — only the
        // text and caret go on top.
        if (!m_searchText.empty()) {
            g.DrawString(m_searchText, x, y, 0xFFFFFFFF, true);
        }
        // Caret blink (300ms on/off, MC EditBox.java line 408).
        // MC EditBox.renderWidget → graphics.textHighlight(x0, y, x1, y + 9,
        // invert): the creative box sets invertHighlightedTextColor(false),
        // so it is only the GUI_TEXT_HIGHLIGHT quad — pure blue (0xFF0000FF)
        // blended ADDITIVE over what is already drawn. The field's grey
        // (139,139,139 in tab_item_search.png) becomes (139,139,255), white
        // glyphs stay white, their dark shadow turns dark blue. No additive
        // blend in GuiGraphics, so those results are painted directly.
        if (m_searchFocused && m_searchHighlightPos != m_searchCursorPos) {
            const int s0 = std::min(m_searchCursorPos, m_searchHighlightPos);
            const int s1 = std::max(m_searchCursorPos, m_searchHighlightPos);
            const int x0 = x + g.GetStringWidth(m_searchText.substr(0, (size_t)s0));
            const int x1 = x + g.GetStringWidth(m_searchText.substr(0, (size_t)s1));
            const std::string selected = m_searchText.substr((size_t)s0, (size_t)(s1 - s0));
            g.Fill(x0, y - 1, x1, y + 9, 0xFF8B8BFF);
            g.DrawString(selected, x0 + 1, y + 1, 0xFF3E3EFF, false);   // the shadow, +blue
            g.DrawString(selected, x0, y, 0xFFFFFFFF, false);
        }
        if (!m_searchFocused) return;
        long long elapsed = NowMillis() - m_searchFocusedAtMillis;
        if (elapsed < 0) elapsed = 0;
        if (((elapsed / 300LL) % 2LL) != 0LL) return;

        const int beforeW = g.GetStringWidth(m_searchText.substr(0, m_searchCursorPos));
        if (m_searchCursorPos >= (int)m_searchText.size()) {
            g.DrawString("_", x + beforeW + 1, y, 0xFFFFFFFF, true);
        } else {
            g.Fill(x + beforeW, y - 1, x + beforeW + 1, y + 1 + 9, 0xFFFFFFFF);
        }
    }

    void CreativeModeInventoryScreen::RenderScrollbar(GuiGraphics& g, int leftPos, int topPos) {
        // MC: blitSprite(scroller, xscr, yscr + (int)((yscr2 - yscr - 17) * scrollOffs), 12, 15).
        const int x = leftPos + SCROLLBAR_X;
        const int y = topPos + SCROLLBAR_Y +
                      (int)((float)(SCROLLBAR_Y2 - SCROLLBAR_Y - 17) * m_scrollOffs);
        const char* sprite = CanScroll()
            ? "container/creative_inventory/scroller"
            : "container/creative_inventory/scroller_disabled";
        g.BlitSprite(sprite, x, y, SCROLL_THUMB_W, SCROLL_THUMB_H);
    }

} // namespace Render
