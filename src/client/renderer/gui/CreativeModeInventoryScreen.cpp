// File: src/client/renderer/gui/CreativeModeInventoryScreen.cpp
#include "CreativeModeInventoryScreen.hpp"
#include "InventoryScreen.hpp"          // GetSurvivalInventoryScreen (gamemode swap)
#include "GuiGraphics.hpp"
#include "items/PlayerInventoryPreview.hpp"
#include "screens/Screen.hpp"           // LoadStandaloneGuiTexture
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Game::Items::Compass etc.
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "client/entity/Player.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace {
    long long NowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    inline std::string ToLower(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) r.push_back((char)std::tolower((unsigned char)c));
        return r;
    }

    // Whether a block has an item form (= can appear in inventory). MC's
    // Items.java explicitly registers BlockItems for ~1000 blocks; the rest are
    // placement-only or technical blocks (wall_torch is auto-placed when you
    // right-click a torch on a wall, redstone_wire is the placed form of
    // redstone dust, etc.). Without this filter, the search tab shows
    // duplicates like "Wall Torch" alongside "Torch".
    //
    // TODO: replace this hardcoded denylist by extending tools/gen_items.py to
    // emit a `kBlockItemSlugs` allowlist parsed from Items.java's
    // `registerBlock(Blocks.X, ...)` calls. That's the MC-faithful approach.
    bool BlockHasItemForm(const std::string& slug) {
        // Substring patterns: anything matching is a wall/auto-placed variant.
        for (const char* needle : {
            "wall_torch", "wall_sign", "wall_hanging_sign",
            "wall_banner", "wall_skull", "wall_head", "wall_fan",
        }) {
            if (slug.find(needle) != std::string::npos) return false;
        }
        // Exact-match technical/placeholder blocks.
        switch (slug.size()) {
            default: break;
            case 3: if (slug == "air") return false; break;
            case 4: if (slug == "fire" || slug == "lava" || slug == "kelp" || slug == "wheat") return false; break;
            case 5: if (slug == "water" || slug == "cocoa") return false; break;
            case 7: if (slug == "carrots" || slug == "tripwire" || slug == "void_air"
                        || slug == "cave_air" || slug == "frosted_ice") return false; break;
            case 8: if (slug == "potatoes" || slug == "soul_fire") return false; break;
            case 9: if (slug == "beetroots" || slug == "kelp_plant") return false; break;
            case 11: if (slug == "piston_head" || slug == "pumpkin_stem"
                         || slug == "redstone_wire" || slug == "end_portal"
                         || slug == "melon_stem") return false; break;
            case 13: if (slug == "moving_piston" || slug == "nether_portal"
                         || slug == "end_gateway" || slug == "tall_seagrass"
                         || slug == "bubble_column") return false; break;
            case 16: if (slug == "sweet_berry_bush" || slug == "bamboo_sapling") return false; break;
            case 21: if (slug == "attached_melon_stem") return false; break;
            case 23: if (slug == "attached_pumpkin_stem") return false; break;
        }
        return true;
    }
}

namespace Render {

    CreativeModeInventoryScreen& GetCreativeInventoryScreen() {
        static CreativeModeInventoryScreen s;
        return s;
    }

    void CreativeModeInventoryScreen::OnOpen() {
        m_currentTab = Tab::Survival;
        m_searchText.clear();
        m_searchCursorPos = 0;
        m_searchFocused = false;
        m_scrollOffs = 0.0f;
        m_isScrolling = false;
        m_searchDirty = true;
        m_hoveredCreativeStack = Game::ItemStack{};
        // Pre-warm item textures the screen uses so the first render frame can
        // draw them. Without this the compass appeared one tab-switch late,
        // because the texture create + bind landed in the same frame as the draw.
        GuiGraphics::PreloadItem(Game::Items::Compass);
    }

    void CreativeModeInventoryScreen::ContainerTick() {
        // The mirror of InventoryScreen::ContainerTick: a player who loses
        // infinite materials (a /gamemode survival while the picker is open)
        // gets handed the survival panel. Silent on both ends so the cursor
        // stack survives the swap.
        if (Player() && !Player()->IsCreative()) {
            CloseSilently();
            GetSurvivalInventoryScreen().OpenSilently();
        }
    }

    void CreativeModeInventoryScreen::SwitchTab(Tab t) {
        if (m_currentTab == t) return;
        m_currentTab = t;
        m_scrollOffs = 0.0f;
        m_searchText.clear();
        m_searchCursorPos = 0;
        m_searchFocused = (t == Tab::Search);
        m_searchFocusedAtMillis = NowMillis();
        m_searchDirty = true;
    }

    // ─── Slot layout ─────────────────────────────────────────────
    bool CreativeModeInventoryScreen::GetSlotPos(int menuIndex, int& outX, int& outY) const {
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return false;

        // MC CreativeModeInventoryScreen.selectTab lines 524-552 re-wraps every
        // player-inventory slot at a position for THIS panel. Crafting result +
        // grid go to (-2000, -2000) — off-panel, i.e. not drawn or clickable.
        if (menuIndex < Game::Inventory::ARMOR_BEGIN) return false;

        // The Search tab replaces the panel's contents with the item grid; only
        // the hotbar stays visible beneath it. MC achieves this by swapping the
        // menu's slot list back to the picker's own. Leaving the main rows
        // hittable here would route clicks on "empty" grid cells into hidden
        // storage slots.
        if (m_currentTab != Tab::Survival && !Game::Inventory::IsHotbarSlot(menuIndex)) {
            return false;
        }

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
        // hotbar which is pinned to 112 (MC selectTab lines 542-549).
        const int pos = menuIndex - Game::Inventory::MAIN_BEGIN;
        outX = 9 + (pos % 9) * SLOT_STEP;
        outY = Game::Inventory::IsHotbarSlot(menuIndex) ? 112
                                                        : 54 + (pos / 9) * SLOT_STEP;
        return true;
    }

    // ─── Search ──────────────────────────────────────────────────
    void CreativeModeInventoryScreen::RefreshSearchResults() {
        if (!m_searchDirty) return;
        m_searchDirty = false;
        m_filteredItems.clear();
        const std::string needle = ToLower(m_searchText);

        // Iterate ALL items (block items + pure items) so the user can find
        // Compass etc. Block items have IDs 1..(BlockID::Count-1); pure items
        // live at PURE_ITEM_BASE+.
        const int blockItemCount = (int)Game::BlockID::Count;
        for (int i = 1; i < blockItemCount; ++i) {
            const auto& block = Game::BlockRegistry::Get((Game::BlockID)i);
            if (!BlockHasItemForm(block.modelName)) continue;
            const auto& it = Game::ItemRegistry::Get((Game::ItemID)i);
            if (needle.empty() || ToLower(it.name).find(needle) != std::string::npos) {
                m_filteredItems.emplace_back((Game::ItemID)i, 1);
            }
        }

        // Pure items — walk the registered map directly so CUSTOM items past
        // the kPureItemTable bounds (Portal Gun, future feature items) are
        // included automatically. Iterating the table directly used to MISS
        // anything registered after the MC-table loop in ItemRegistry::Initialize.
        Game::ItemRegistry::ForEachPureItem([&](Game::ItemID id, const Game::Item&) {
            // Special case: enchanted_book expands into one stack per
            // (enchantment, level) pair — mirrors MC's
            // CreativeModeTabs.generateEnchantmentBook TypesAllLevels at
            // CreativeModeTabs.java:1843-1844, which streams
            // IntStream.rangeClosed(minLevel, maxLevel) and calls
            // EnchantmentHelper.createBook(...) for each. The match needle
            // filters against the enchantment's display name so "sharpness"
            // finds Sharpness I-V.
            if (id == Game::Items::EnchantedBook) {
                // The item itself is "Enchanted Book" — also let users find
                // every variant by typing the item's own name (or a substring
                // like "enchant"). MC's search tab matches BOTH the item name
                // and per-variant tooltip lines this way.
                const auto& bookItem = Game::ItemRegistry::Get(id);
                const bool itemNameMatches =
                    !needle.empty()
                    && ToLower(bookItem.name).find(needle) != std::string::npos;
                const auto& all = Game::EnchantmentRegistry::All();
                for (size_t ei = 0; ei < all.size(); ++ei) {
                    const auto& ench = all[ei];
                    const Game::EnchantmentId enchId = static_cast<Game::EnchantmentId>(ei);
                    for (int level = ench.minLevel; level <= ench.maxLevel; ++level) {
                        if (needle.empty()
                            || itemNameMatches
                            || ToLower(ench.displayName).find(needle) != std::string::npos) {
                            m_filteredItems.push_back(
                                Game::EnchantmentHelper::CreateBook({enchId, level}));
                        }
                    }
                }
                return; // (lambda — equivalent of `continue` in the old for-loop)
            }
            const auto& it = Game::ItemRegistry::Get(id);
            if (needle.empty() || ToLower(it.name).find(needle) != std::string::npos) {
                m_filteredItems.emplace_back(id, 1);
            }
        });
        m_scrollOffs = 0.0f;
    }

    int CreativeModeInventoryScreen::GetRowCount() const {
        int rows = ((int)m_filteredItems.size() + GRID_COLS - 1) / GRID_COLS; // ceil(n/9)
        return std::max(0, rows - GRID_ROWS);
    }
    int CreativeModeInventoryScreen::GetRowIndex() const {
        const int rc = GetRowCount();
        if (rc <= 0) return 0;
        const int idx = (int)std::floor(m_scrollOffs * rc + 0.5f);
        return std::max(0, std::min(idx, rc));
    }
    bool CreativeModeInventoryScreen::HasScrollBar() const {
        return (int)m_filteredItems.size() > GRID_COLS * GRID_ROWS;
    }

    // ─── Hit testing ─────────────────────────────────────────────
    int CreativeModeInventoryScreen::HitTestExtras(int lx, int ly) {
        // Clear the search-grid hover cache up front. The grid branch below is
        // its only writer, and sets it back when the cursor IS over an occupied
        // cell. Without this clear the cache would hold stale data when:
        //   • the cursor moves off an item to an empty cell or off-grid,
        //   • search results change underneath the cursor (typing filters the
        //     grid, so the cell under the mouse may now be empty or different),
        //   • a click on the grid causes the server to clear the cursor — the
        //     tooltip path runs after the click and would still read the
        //     pre-click hover stack.
        // Resetting here ties the cache strictly to the current frame's hover.
        m_hoveredCreativeStack = Game::ItemStack{};

        // Tabs sit ABOVE the panel (MC Row.TOP): y = -28 .. -28 + TAB_H, of
        // which only the top 28px are visible — the panel covers the last 4.
        const int tabY = -28;
        if (ly >= tabY && ly < tabY + TAB_H - 4) {
            if (lx >= 0 && lx < TAB_W) return HIT_TAB_SURVIVAL;
            if (lx >= TAB_SPACING && lx < TAB_SPACING + TAB_W) return HIT_TAB_SEARCH;
        }

        if (m_currentTab == Tab::Search) {
            if (lx >= SEARCH_X && lx < SEARCH_X + SEARCH_W &&
                ly >= SEARCH_Y && ly < SEARCH_Y + SEARCH_H) {
                return HIT_SEARCH_BOX;
            }
            if (lx >= SCROLLBAR_X && lx < SCROLLBAR_X2 &&
                ly >= SCROLLBAR_Y && ly < SCROLLBAR_Y2) {
                return HIT_SCROLLBAR;
            }
            if (lx >= GRID_X && lx < GRID_X + GRID_COLS * SLOT_STEP &&
                ly >= GRID_Y && ly < GRID_Y + GRID_ROWS * SLOT_STEP) {
                const int col = (lx - GRID_X) / SLOT_STEP;
                const int row = (ly - GRID_Y) / SLOT_STEP;
                // Slots are 16x16 inside an 18-wide cell — reject the 2px gutter.
                if ((lx - GRID_X) - col * SLOT_STEP < SLOT_SIZE &&
                    (ly - GRID_Y) - row * SLOT_STEP < SLOT_SIZE) {
                    const int idx = (GetRowIndex() + row) * GRID_COLS + col;
                    // Always claim the cell — empty cells still need the hover
                    // highlight and still absorb clicks (dropping a carried
                    // stack on one is the search tab's delete gesture).
                    if (idx >= 0 && idx < (int)m_filteredItems.size()) {
                        m_hoveredCreativeStack = m_filteredItems[idx];
                    }
                    return HIT_CREATIVE_GRID;
                }
            }
        }

        // Trash slot — MC's selectTab(INVENTORY) line 556 adds destroyItemSlot
        // at (173, 112) ONLY on the Survival Inventory tab. The X icon is part
        // of tab_inventory.png; clicking it deletes whatever is on the cursor.
        if (m_currentTab == Tab::Survival &&
            lx >= TRASH_X && lx < TRASH_X + SLOT_SIZE &&
            ly >= TRASH_Y && ly < TRASH_Y + SLOT_SIZE) {
            return HIT_TRASH;
        }

        return HIT_NONE;
    }

    const Game::ItemStack* CreativeModeInventoryScreen::HoveredExtraStack() const {
        return &m_hoveredCreativeStack;   // empty unless a grid cell is hovered
    }

    // ─── Input ───────────────────────────────────────────────────
    // Left/right/middle make no difference to any of this screen's own zones —
    // shift alone decides stack vs. single — so the button is unused here.
    bool CreativeModeInventoryScreen::HandleExtraClick(int hit, int /*glfwButton*/, bool shift) {
        if (hit == HIT_TAB_SURVIVAL) { SwitchTab(Tab::Survival); return true; }
        if (hit == HIT_TAB_SEARCH)   { SwitchTab(Tab::Search);   return true; }

        // Runs on EVERY press, so clicking anywhere else drops search focus.
        m_searchFocused = (hit == HIT_SEARCH_BOX);
        if (m_searchFocused) {
            m_searchFocusedAtMillis = NowMillis();
            return true;
        }

        if (hit == HIT_SCROLLBAR) {
            m_isScrolling = true;
            return true;
        }

        if (hit == HIT_CREATIVE_GRID) {
            // Empty cell + held cursor → delete the held item. The search grid
            // doubles as a trash zone (the user-facing model is "drop it
            // anywhere on the grid to delete it"). Routes through THROW with
            // the OUTSIDE sentinel — the same path as click-outside-the-panel
            // and shift+trash, both of which already discard the cursor.
            if (m_hoveredCreativeStack.IsEmpty()) {
                if (!Carried().IsEmpty()) {
                    QueueClick(Network::ContainerInput::THROW,
                               Network::InventorySlotSentinel::OUTSIDE,
                               0 /*button 0 = whole stack (MC PRIMARY)*/);
                }
                return true;
            }
            // Server-side button semantics in HandleCreativePickup:
            //   button=0 → cursor = full stack of this item
            //   button=1 → cursor = 1 of this item (or +1 if same; clear if different)
            //
            // Mapping (shift = stack, no-shift = single, regardless of L/R):
            //   plain left   → 1 on cursor
            //   plain right  → 1 on cursor (or +1 if same item already held)
            //   shift+left   → full stack on cursor
            //   shift+right  → full stack on cursor
            const uint8_t btn = shift ? 0 : 1;
            QueueClick(Network::ContainerInput::PICKUP,
                       Network::InventorySlotSentinel::CREATIVE_GRID, btn,
                       m_hoveredCreativeStack.itemId,
                       &m_hoveredCreativeStack);
            return true;
        }

        // Trash slot:
        //   • Shift+click → clear ALL inventory slots (MC's
        //     CreativeModeInventoryScreen line 189-193:
        //     `if (slot == this.destroyItemSlot && quickKey)`).
        //   • Plain click with a carried stack → discard the cursor.
        if (hit == HIT_TRASH) {
            if (shift) {
                QueueClick(Network::ContainerInput::CREATIVE_DESTROY_ALL,
                           Network::InventorySlotSentinel::OUTSIDE, 0);
            } else if (!Carried().IsEmpty()) {
                QueueClick(Network::ContainerInput::THROW,
                           Network::InventorySlotSentinel::OUTSIDE,
                           0 /*button 0 = whole stack (MC PRIMARY)*/);
            }
            return true;
        }

        return false;   // let the base handle real slots and outside-clicks
    }

    void CreativeModeInventoryScreen::HandleExtraRelease() {
        m_isScrolling = false;
    }

    bool CreativeModeInventoryScreen::HandleExtraKey(int glfwKey, int /*glfwMods*/) {
        if (m_currentTab != Tab::Search || !m_searchFocused) return false;

        // E must produce the letter rather than close the screen while typing;
        // consuming it here lets OnCharInput append the actual character.
        if (glfwKey == GLFW_KEY_E) return true;

        if (glfwKey == GLFW_KEY_BACKSPACE) {
            if (m_searchCursorPos > 0) {
                m_searchText.erase(m_searchText.begin() + (m_searchCursorPos - 1));
                m_searchCursorPos--;
                m_searchDirty = true;
                RefreshSearchResults();
                m_searchFocusedAtMillis = NowMillis();
            }
            return true;
        }
        if (glfwKey == GLFW_KEY_DELETE) {
            if (m_searchCursorPos < (int)m_searchText.size()) {
                m_searchText.erase(m_searchText.begin() + m_searchCursorPos);
                m_searchDirty = true;
                RefreshSearchResults();
                m_searchFocusedAtMillis = NowMillis();
            }
            return true;
        }
        if (glfwKey == GLFW_KEY_LEFT) {
            if (m_searchCursorPos > 0) m_searchCursorPos--;
            m_searchFocusedAtMillis = NowMillis();
            return true;
        }
        if (glfwKey == GLFW_KEY_RIGHT) {
            if (m_searchCursorPos < (int)m_searchText.size()) m_searchCursorPos++;
            m_searchFocusedAtMillis = NowMillis();
            return true;
        }
        if (glfwKey == GLFW_KEY_HOME) { m_searchCursorPos = 0; m_searchFocusedAtMillis = NowMillis(); return true; }
        if (glfwKey == GLFW_KEY_END)  { m_searchCursorPos = (int)m_searchText.size(); m_searchFocusedAtMillis = NowMillis(); return true; }

        return false;
    }

    bool CreativeModeInventoryScreen::HandleExtraCharInput(unsigned int codepoint) {
        if (m_currentTab != Tab::Search || !m_searchFocused) return false;
        if ((int)m_searchText.size() >= SEARCH_MAX_LEN) return true;
        if (codepoint < 32 || codepoint >= 127) return true;
        m_searchText.insert(m_searchText.begin() + m_searchCursorPos, (char)codepoint);
        m_searchCursorPos++;
        m_searchFocusedAtMillis = NowMillis();
        m_searchDirty = true;
        RefreshSearchResults();
        return true;
    }

    bool CreativeModeInventoryScreen::HandleExtraScroll(double dy) {
        if (m_currentTab != Tab::Search) return false;
        if (!HasScrollBar()) return false;
        const int rc = GetRowCount();
        if (rc <= 0) return false;
        m_scrollOffs = std::max(0.0f, std::min(1.0f, m_scrollOffs - (float)dy / (float)rc));
        return true;
    }

    void CreativeModeInventoryScreen::OnExtraMouseMove(int /*leftPos*/, int topPos) {
        if (!m_isScrolling) return;
        const float top    = (float)(topPos + SCROLLBAR_Y);
        const float trackH = (float)((SCROLLBAR_Y2 - SCROLLBAR_Y) - SCROLL_THUMB_H);
        const float t      = (MouseGui().y - top - 7.5f) / trackH;
        m_scrollOffs = std::max(0.0f, std::min(1.0f, t));
    }

    // ─── Background ──────────────────────────────────────────────
    TextureHandle CreativeModeInventoryScreen::EnsureBackground(bool survival) {
        TextureHandle& cache = survival ? m_inventoryBg : m_searchBg;
        bool&          tried = survival ? m_inventoryBgTried : m_searchBgTried;
        if (tried) return cache;
        tried = true;
        int w = 0, h = 0;
        cache = LoadStandaloneGuiTexture(
            survival ? "assets/textures/gui/container/creative_inventory/tab_inventory.png"
                     : "assets/textures/gui/container/creative_inventory/tab_item_search.png",
            w, h);
        return cache;
    }

    void CreativeModeInventoryScreen::DrawBackground(GuiGraphics& g, int leftPos, int topPos,
                                                     TextureHandle bg) {
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
            return;
        }
        // The textures are 256x256 PNGs; the content is the top-left 195x136.
        g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
               0.0f, 0.0f, (float)IMAGE_W / 256.0f, (float)IMAGE_H / 256.0f);
    }

    // ─── Tab chrome ──────────────────────────────────────────────
    // MC: CreativeModeTabs.java line 1814 (INVENTORY → Blocks.CHEST) and line
    // 1253 (SEARCH → Items.COMPASS). One RenderItem(ItemStack, x, y) dispatches
    // on renderType — block items render 3D, sprite items flat.
    static void DrawSurvivalIcon(GuiGraphics& g, int x, int y) {
        Game::ItemStack icon{Game::ItemRegistry::FromBlock(Game::BlockID::Chest), 1};
        g.RenderItem(icon, x, y);
    }
    static void DrawSearchIcon(GuiGraphics& g, int x, int y) {
        Game::ItemStack icon{Game::Items::Compass, 1};
        g.RenderItem(icon, x, y);
    }

    void CreativeModeInventoryScreen::RenderUnselectedTabs(GuiGraphics& g, int leftPos, int topPos) {
        const int  tabY   = topPos - 28;
        const bool surSel = (m_currentTab == Tab::Survival);
        const bool srcSel = (m_currentTab == Tab::Search);

        // Two-phase render so tab icons are FORCED on top of tab backgrounds.
        // Within a single stratum the GUI renderer doesn't always honour
        // submission order for blits (texture batching can reorder), and the
        // compass icon was getting hidden under the unselected Search tab
        // background. Bumping the stratum between BG and icon is the explicit fix.
        if (!surSel) {
            g.BlitSprite("container/creative_inventory/tab_top_unselected_1",
                         leftPos, tabY, TAB_W, TAB_H);
        }
        if (!srcSel) {
            g.BlitSprite("container/creative_inventory/tab_top_unselected_2",
                         leftPos + TAB_SPACING, tabY, TAB_W, TAB_H);
        }
        g.NextStratum();
        if (!surSel) DrawSurvivalIcon(g, leftPos + 5, tabY + 9);
        if (!srcSel) DrawSearchIcon  (g, leftPos + TAB_SPACING + 5, tabY + 9);
    }

    void CreativeModeInventoryScreen::RenderSelectedTab(GuiGraphics& g, int leftPos, int topPos) {
        const int  tabY   = topPos - 28;
        const bool surSel = (m_currentTab == Tab::Survival);
        if (surSel) {
            g.BlitSprite("container/creative_inventory/tab_top_selected_1",
                         leftPos, tabY, TAB_W, TAB_H);
        } else {
            g.BlitSprite("container/creative_inventory/tab_top_selected_2",
                         leftPos + TAB_SPACING, tabY, TAB_W, TAB_H);
        }
        g.NextStratum();
        if (surSel) DrawSurvivalIcon(g, leftPos + 5, tabY + 9);
        else        DrawSearchIcon  (g, leftPos + TAB_SPACING + 5, tabY + 9);
    }

    // ─── Draw layers ─────────────────────────────────────────────
    void CreativeModeInventoryScreen::RenderBehindBg(GuiGraphics& g, int leftPos, int topPos) {
        RenderUnselectedTabs(g, leftPos, topPos);
    }

    void CreativeModeInventoryScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const bool survival = (m_currentTab == Tab::Survival);
        DrawBackground(g, leftPos, topPos, EnsureBackground(survival));
        if (!survival) return;

        Game::ClientPlayer* player = Player();
        if (!player) return;

        // Player preview — MC CreativeModeInventoryScreen.java:702 calls
        // InventoryScreen.renderEntityInInventoryFollowsMouse(graphics,
        // leftPos+73, topPos+6, leftPos+105, topPos+49, 20, 0.0625F, xm, ym,
        // this.minecraft.player). Same rect, same cursor-tracking math.
        g.NextStratum();
        StickFigurePose pose;
        pose.bodyYawDeg   = player->visualYaw;
        pose.headYawDeg   = player->visualYaw;
        pose.headPitchDeg = player->visualPitch;
        pose.isCrouching  = false;
        RenderStickFigureInInventory(
            g,
            leftPos + 73, topPos + 6, leftPos + 105, topPos + 49,
            20, 0.0625f,
            MouseGui().x, MouseGui().y,
            pose,
            player->color);
    }

    void CreativeModeInventoryScreen::RenderExtraSlots(GuiGraphics& g, int leftPos, int topPos) {
        if (m_currentTab != Tab::Search) return;

        RefreshSearchResults();
        const int rowIndex = GetRowIndex();
        for (int row = 0; row < GRID_ROWS; ++row) {
            for (int col = 0; col < GRID_COLS; ++col) {
                const int idx = (rowIndex + row) * GRID_COLS + col;
                if (idx >= (int)m_filteredItems.size()) continue;
                // Render the pre-built ItemStack directly — it carries the
                // correct DataComponents (e.g. STORED_ENCHANTMENTS for an
                // enchanted_book variant), which RenderItem and its glint pass
                // read for foil detection.
                const auto& stack = m_filteredItems[idx];
                const int x = leftPos + GRID_X + col * SLOT_STEP;
                const int y = topPos  + GRID_Y + row * SLOT_STEP;
                g.RenderItem(stack, x, y);
                g.NextStratum();
                g.RenderItemDecorations(stack, x, y);
            }
        }
    }

    void CreativeModeInventoryScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        if (m_currentTab == Tab::Search) {
            RenderSearchBox(g, leftPos, topPos);
            RenderScrollbar(g, leftPos, topPos);
        }
        g.NextStratum();
        RenderSelectedTab(g, leftPos, topPos);
    }

    void CreativeModeInventoryScreen::RenderExtraHoverHighlight(GuiGraphics& g,
                                                                int leftPos, int topPos) {
        if (HoveredSlot() == HIT_TRASH) {
            RenderHoverHighlight(g, leftPos + TRASH_X, topPos + TRASH_Y, /*front=*/true);
            return;
        }
        if (HoveredSlot() != HIT_CREATIVE_GRID) return;

        // Highlight ANY hovered grid cell (occupied OR empty) — empty cells are
        // interactive too: clicking one with a held cursor deletes the cursor,
        // so it needs the same feedback as an occupied cell.
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
        const int x      = leftPos + SCROLLBAR_X;
        const int trackH = (SCROLLBAR_Y2 - SCROLLBAR_Y) - SCROLL_THUMB_H;
        const int y      = topPos + SCROLLBAR_Y + (int)((float)trackH * m_scrollOffs);
        const char* sprite = HasScrollBar()
            ? "container/creative_inventory/scroller"
            : "container/creative_inventory/scroller_disabled";
        g.BlitSprite(sprite, x, y, SCROLL_THUMB_W, SCROLL_THUMB_H);
    }

} // namespace Render
