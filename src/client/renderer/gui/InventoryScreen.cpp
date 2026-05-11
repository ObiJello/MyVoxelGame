// File: src/client/renderer/gui/InventoryScreen.cpp
#include "InventoryScreen.hpp"
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include "items/PlayerInventoryPreview.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/GeneratedItemList.hpp"   // for Game::Items::Compass etc.
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include "common/data/DataComponents.hpp"
#include "common/core/Log.hpp"
#include "client/entity/Player.hpp"

#include "stb_image.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>

// Provided by src/platform/PlatformMain.cpp — handles macOS app-bundle resource path
// resolution and falls back to a CWD-relative path during dev builds.
namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

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

    // QuickCraft mask helpers — match MC AbstractContainerMenu lines 749/753/757.
    inline uint8_t QuickcraftMask(int header, int type) {
        return (uint8_t)(((header & 3) << 2) | (type & 3));
    }

    // Whether a block has an item form (= can appear in inventory). MC's Items.java
    // explicitly registers BlockItems for ~1000 blocks; the rest are
    // placement-only or technical blocks (wall_torch is auto-placed when you
    // right-click a torch on a wall, redstone_wire is the placed form of redstone
    // dust, etc.). Without this filter, the search tab shows duplicates like
    // "Wall Torch" alongside "Torch".
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

    InventoryScreen& GetInventoryScreen() {
        static InventoryScreen s;
        return s;
    }

    // Exposed via extern declaration in ClientPacketHandler.cpp.
    void SetInventoryScreenCarriedItem(Game::ItemID id, int count) {
        GetInventoryScreen().SetCarriedItem(id, count);
    }

    void InventoryScreen::Open() {
        m_open = true;
        m_currentTab = Tab::Survival;
        m_searchText.clear();
        m_searchCursorPos = 0;
        m_searchFocused = false;
        m_scrollOffs = 0.0f;
        m_isScrolling = false;
        m_isDragging = false;
        m_dragSlots.clear();
        m_pendingClicks.clear();
        m_searchDirty = true;
        // Pre-warm item textures used by the screen so the first render frame can use
        // them immediately. Without this the compass appeared one tab-switch late
        // because the texture create + bind landed in the same frame as the draw.
        GuiGraphics::PreloadItem(Game::Items::Compass);
    }

    void InventoryScreen::Close() {
        if (!m_open) return;
        m_open = false;
        m_isDragging = false;
        m_isScrolling = false;
        m_dragSlots.clear();
        // Tell the server (so it can drop carried). The server replies with a SetCarried(empty).
        Network::InventoryClickC2SPacket close{};
        close.action = (uint8_t)Network::ContainerInput::PICKUP; // sentinel — unused
        // We use a separate close packet type (InventoryCloseC2S). Push that instead via a
        // special marker — PlatformMain will intercept this and send InventoryCloseC2S.
        // Simpler: enqueue a marker using ContainerInput=255 reserved sentinel.
        close.action = 0xFF;
        m_pendingClicks.push_back(close);
    }

    void InventoryScreen::SetCarriedItem(Game::ItemID id, int count) {
        m_carriedItem.itemId = id;
        m_carriedItem.count  = count;
        if (id == Game::Items::Air || count <= 0) m_carriedItem.Clear();
    }

    bool InventoryScreen::ConsumePendingClick(Network::InventoryClickC2SPacket& out) {
        if (m_pendingClicks.empty()) return false;
        out = m_pendingClicks.front();
        m_pendingClicks.erase(m_pendingClicks.begin());
        return true;
    }

    void InventoryScreen::QueueClick(Network::ContainerInput action, int16_t slotIndex,
                                     uint8_t button, Game::ItemID creativeItem) {
        Network::InventoryClickC2SPacket p{};
        p.slotIndex      = slotIndex;
        p.button         = button;
        p.action         = (uint8_t)action;
        p.flags          = 0;
        p.creativeItemId = creativeItem;
        m_pendingClicks.push_back(p);
    }

    void InventoryScreen::SwitchTab(Tab t) {
        if (m_currentTab == t) return;
        m_currentTab = t;
        m_scrollOffs = 0.0f;
        m_searchText.clear();
        m_searchCursorPos = 0;
        m_searchFocused = (t == Tab::Search);
        m_searchFocusedAtMillis = NowMillis();
        m_searchDirty = true;
    }

    // ─── Search refresh ───────────────────────────────────────
    void InventoryScreen::RefreshSearchResults() {
        if (!m_searchDirty) return;
        m_searchDirty = false;
        m_filteredItems.clear();
        const std::string needle = ToLower(m_searchText);
        // Iterate ALL items (block items + pure items) so the user can find Compass etc.
        // Block items have IDs 1..(BlockID::Count-1); pure items live at PURE_ITEM_BASE+.
        const int blockItemCount = (int)Game::BlockID::Count;
        for (int i = 1; i < blockItemCount; ++i) {
            const auto& block = Game::BlockRegistry::Get((Game::BlockID)i);
            if (!BlockHasItemForm(block.modelName)) continue;
            const auto& it = Game::ItemRegistry::Get((Game::ItemID)i);
            if (needle.empty() || ToLower(it.name).find(needle) != std::string::npos) {
                m_filteredItems.emplace_back((Game::ItemID)i, 1);
            }
        }
        // Pure items — walk the generated table; IDs are PURE_ITEM_BASE + table index.
        for (size_t k = 0; k < Game::kPureItemTableSize; ++k) {
            const Game::ItemID id = Game::PURE_ITEM_BASE + (Game::ItemID)k;
            // Special case: enchanted_book expands into one stack per (enchantment,
            // level) pair — mirrors MC's CreativeModeTabs.generateEnchantmentBook
            // TypesAllLevels at CreativeModeTabs.java:1843–1844, which streams
            // IntStream.rangeClosed(minLevel, maxLevel) and calls EnchantmentHelper
            // .createBook(...) for each. The match needle filters against the
            // enchantment's display name so "sharpness" finds Sharpness I–V.
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
                        // Match search needle against bare display name so
                        // "sharpness" finds Sharpness I–V, "protect" finds all
                        // protection variants, etc. Also match against the
                        // item's own name ("enchanted book") so a search of
                        // "enchant" returns the full set.
                        if (needle.empty()
                            || itemNameMatches
                            || ToLower(ench.displayName).find(needle) != std::string::npos) {
                            m_filteredItems.push_back(
                                Game::EnchantmentHelper::CreateBook({enchId, level}));
                        }
                    }
                }
                continue; // skip the default single-stack push for enchanted_book
            }
            const auto& it = Game::ItemRegistry::Get(id);
            if (needle.empty() || ToLower(it.name).find(needle) != std::string::npos) {
                m_filteredItems.emplace_back(id, 1);
            }
        }
        m_scrollOffs = 0.0f;
    }

    int InventoryScreen::GetRowCount() const {
        int rows = ((int)m_filteredItems.size() + 8) / 9; // ceil(n/9)
        return std::max(0, rows - 5);
    }
    int InventoryScreen::GetRowIndex() const {
        int rc = GetRowCount();
        if (rc <= 0) return 0;
        int idx = (int)std::floor(m_scrollOffs * rc + 0.5f);
        return std::max(0, std::min(idx, rc));
    }
    bool InventoryScreen::HasScrollBar() const {
        return (int)m_filteredItems.size() > 45;
    }

    // ─── Slot positions ───────────────────────────────────────
    bool InventoryScreen::GetSlotImagePos(int slotIndex, int& outX, int& outY) const {
        // Common to both tabs: main inv + hotbar
        if (Game::Inventory::IsMainSlot(slotIndex)) {
            int local = slotIndex - Game::Inventory::MAIN_BEGIN;
            outX = 9 + (local % 9) * SLOT_STEP;
            outY = 54 + (local / 9) * SLOT_STEP; // y=54,72,90
            return true;
        }
        if (Game::Inventory::IsHotbarSlot(slotIndex)) {
            int local = slotIndex - Game::Inventory::HOTBAR_BEGIN;
            outX = 9 + local * SLOT_STEP;
            outY = 112;
            return true;
        }
        // Survival-tab-only: armor + offhand. (Crafting result + grid are off-screen in
        // MC creative survival — we never lay them out here, so HitTest can't hit them.)
        if (m_currentTab == Tab::Survival) {
            if (Game::Inventory::IsArmorSlot(slotIndex)) {
                int local = slotIndex - Game::Inventory::ARMOR_BEGIN;
                int col = local / 2;
                int row = local % 2;
                outX = 54 + col * 54;
                outY = 6 + row * 27;
                return true;
            }
            if (Game::Inventory::IsOffhandSlot(slotIndex)) {
                // MC: CreativeModeInventoryScreen.selectTab(INVENTORY) line 537-539 → (35, 20)
                outX = 35; outY = 20;
                return true;
            }
        }
        return false;
    }

    // ─── Hit testing ──────────────────────────────────────────
    int InventoryScreen::HitTest(int leftPos, int topPos) {
        const int mx = (int)std::floor(m_mouseGui.x);
        const int my = (int)std::floor(m_mouseGui.y);
        const int lx = mx - leftPos;
        const int ly = my - topPos;

        // Tabs are above the panel (Row.TOP): y = -28 .. -28 + TAB_H, but only the top 28px
        // are visible (the bottom 4px is covered by the panel).
        const int tabY = -28;
        if (ly >= tabY && ly < tabY + TAB_H - 4) {
            if (lx >= 0 && lx < TAB_W) return HIT_TAB_SURVIVAL;
            if (lx >= TAB_SPACING && lx < TAB_SPACING + TAB_W) return HIT_TAB_SEARCH;
        }

        // Outside the panel? (and outside tabs)
        if (lx < 0 || lx >= IMAGE_W || ly < 0 || ly >= IMAGE_H) {
            return HIT_OUTSIDE;
        }

        // Search box (Search tab only)
        if (m_currentTab == Tab::Search) {
            if (lx >= SEARCH_X && lx < SEARCH_X + SEARCH_W &&
                ly >= SEARCH_Y && ly < SEARCH_Y + SEARCH_H) {
                return HIT_SEARCH_BOX;
            }
            // Scrollbar zone
            if (lx >= SCROLLBAR_X && lx < SCROLLBAR_X2 &&
                ly >= SCROLLBAR_Y && ly < SCROLLBAR_Y2) {
                return HIT_SCROLLBAR;
            }
            // Search grid 9 cols × 5 rows starting at (9, 18)
            if (lx >= 9 && lx < 9 + 9 * SLOT_STEP &&
                ly >= 18 && ly < 18 + 5 * SLOT_STEP) {
                int col = (lx - 9) / SLOT_STEP;
                int row = (ly - 18) / SLOT_STEP;
                int colInSlot = (lx - 9) - col * SLOT_STEP;
                int rowInSlot = (ly - 18) - row * SLOT_STEP;
                // Slots are 16x16 inside an 18-wide cell — reject the 2px gutter
                if (colInSlot < SLOT_SIZE && rowInSlot < SLOT_SIZE) {
                    int rowIndex = GetRowIndex();
                    int idx = (rowIndex + row) * 9 + col;
                    if (idx >= 0 && idx < (int)m_filteredItems.size()) {
                        m_hoveredCreativeStack = m_filteredItems[idx];
                        return HIT_CREATIVE_GRID;
                    }
                }
            }
        }

        // Trash slot — MC's CreativeModeInventoryScreen.selectTab(INVENTORY) line 556 adds
        // destroyItemSlot at (173, 112) ONLY on the Survival Inventory tab. The X icon is
        // drawn on tab_inventory.png; clicking it deletes whatever is in the cursor.
        if (m_currentTab == Tab::Survival &&
            lx >= TRASH_X && lx < TRASH_X + SLOT_SIZE &&
            ly >= TRASH_Y && ly < TRASH_Y + SLOT_SIZE) {
            return HIT_TRASH;
        }

        // Hover-eligible slots: main + hotbar + armor + offhand (shield).
        // Crafting (result + 2x2 grid) is positioned off-screen in MC creative survival,
        // so we don't include those — that prevents spurious highlights near armor where
        // craft slots used to land.
        auto hitsSlot = [&](int i) -> bool {
            int sx, sy;
            if (!GetSlotImagePos(i, sx, sy)) return false;
            return (lx >= sx && lx < sx + SLOT_SIZE && ly >= sy && ly < sy + SLOT_SIZE);
        };

        // Main + hotbar
        for (int i = Game::Inventory::MAIN_BEGIN;
             i < Game::Inventory::HOTBAR_BEGIN + Game::Inventory::HOTBAR_SIZE; ++i) {
            if (hitsSlot(i)) return i;
        }
        // Armor (interactive — placement is server-validated)
        for (int i = Game::Inventory::ARMOR_BEGIN;
             i < Game::Inventory::ARMOR_BEGIN + Game::Inventory::ARMOR_SIZE; ++i) {
            if (hitsSlot(i)) return i;
        }
        // Offhand (shield)
        if (hitsSlot(Game::Inventory::OFFHAND_BEGIN)) return Game::Inventory::OFFHAND_BEGIN;

        return HIT_NONE;
    }

    // ─── Input ────────────────────────────────────────────────
    void InventoryScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        if (m_currentTab != Tab::Search || !m_searchFocused) return;
        if ((int)m_searchText.size() >= SEARCH_MAX_LEN) return;
        if (codepoint < 32 || codepoint >= 127) return;
        m_searchText.insert(m_searchText.begin() + m_searchCursorPos, (char)codepoint);
        m_searchCursorPos++;
        m_searchFocusedAtMillis = NowMillis();
        m_searchDirty = true;
        RefreshSearchResults();
    }

    bool InventoryScreen::OnKeyDown(int glfwKey, int glfwMods) {
        if (!m_open) return false;

        // ESC always closes. E closes too — UNLESS the user is typing in the
        // search box, in which case E must produce the letter (handled later by
        // OnCharInput). We let GLFW_KEY_E fall through to the search-edit block
        // below, which returns true to mark the key consumed; OnCharInput then
        // appends the actual character.
        if (glfwKey == GLFW_KEY_ESCAPE) {
            Close();
            return true;
        }
        const bool searchTyping = (m_currentTab == Tab::Search && m_searchFocused);
        if (glfwKey == GLFW_KEY_E && !searchTyping) {
            Close();
            return true;
        }

        // Search box editing
        if (m_currentTab == Tab::Search && m_searchFocused) {
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
            if (glfwKey == GLFW_KEY_LEFT)  { if (m_searchCursorPos > 0) m_searchCursorPos--; m_searchFocusedAtMillis = NowMillis(); return true; }
            if (glfwKey == GLFW_KEY_RIGHT) { if (m_searchCursorPos < (int)m_searchText.size()) m_searchCursorPos++; m_searchFocusedAtMillis = NowMillis(); return true; }
            if (glfwKey == GLFW_KEY_HOME)  { m_searchCursorPos = 0; m_searchFocusedAtMillis = NowMillis(); return true; }
            if (glfwKey == GLFW_KEY_END)   { m_searchCursorPos = (int)m_searchText.size(); m_searchFocusedAtMillis = NowMillis(); return true; }
        }

        // Number keys: SWAP with hotbar slot (button = key - GLFW_KEY_1).
        if (m_hoveredSlot >= 0 && glfwKey >= GLFW_KEY_1 && glfwKey <= GLFW_KEY_9) {
            uint8_t button = (uint8_t)(glfwKey - GLFW_KEY_1);
            QueueClick(Network::ContainerInput::SWAP, (int16_t)m_hoveredSlot, button);
            return true;
        }

        // Q: drop. Hovered slot must be a real player slot.
        if (m_hoveredSlot >= 0 && glfwKey == GLFW_KEY_Q) {
            uint8_t button = (glfwMods & GLFW_MOD_CONTROL) ? 1 : 0;
            QueueClick(Network::ContainerInput::THROW, (int16_t)m_hoveredSlot, button);
            return true;
        }

        return true; // consume everything else while inventory is open
    }

    void InventoryScreen::OnMouseButton(int glfwButton, int action, int mods) {
        if (!m_open) return;
        const bool press = (action == GLFW_PRESS);

        if (!press) {
            // Release: end scroll-drag and quick-craft.
            if (m_isScrolling) m_isScrolling = false;
            if (m_isDragging) {
                // Commit the drag preview to the client's local inventory and
                // cursor state IMMEDIATELY, before sending the END packet. The
                // server's SetSlot/SetCarried response will arrive a few frames
                // later with the same values; without this local commit the UI
                // would briefly snap back to the pre-drag state during those
                // frames, producing a visible flicker.
                if (m_player && !m_carriedItem.IsEmpty()) {
                    const int per = DragPerSlotCount();
                    const int maxStack = Game::ItemRegistry::Get(m_carriedItem.itemId).maxStackSize;
                    int distributed = 0;
                    for (uint8_t s : m_dragSlots) {
                        const auto& base = m_player->inventory.GetSlot(s);
                        if (base.IsEmpty()) {
                            int give = std::min(per, maxStack);
                            m_player->inventory.SetSlot(s, m_carriedItem.itemId, give);
                            distributed += give;
                        } else if (base.itemId == m_carriedItem.itemId) {
                            int give = std::min(per, maxStack - base.count);
                            if (give > 0) {
                                m_player->inventory.SetSlot(s, base.itemId, base.count + give);
                                distributed += give;
                            }
                        }
                        // Different-item slots are skipped (preview also skips).
                    }
                    int remaining = std::max(0, m_dragStartCarriedCount - distributed);
                    m_carriedItem.count = remaining;
                    if (remaining <= 0) m_carriedItem.Clear();
                }
                // End phase — server commits its own copy and replies with
                // matching SetSlot/SetCarried packets that just confirm what we
                // already showed locally.
                QueueClick(Network::ContainerInput::QUICK_CRAFT, -1, QuickcraftMask(2, m_dragType));
                m_isDragging = false;
                m_dragSlots.clear();
                m_dragStartCarriedCount = 0;
            }
            return;
        }

        // PRESS handling
        const int hit = m_hoveredSlot;
        const bool shift = (mods & GLFW_MOD_SHIFT) != 0;

        // Tab clicks
        if (hit == HIT_TAB_SURVIVAL)  { SwitchTab(Tab::Survival); return; }
        if (hit == HIT_TAB_SEARCH)    { SwitchTab(Tab::Search); return; }

        // Search box focus toggle
        m_searchFocused = (hit == HIT_SEARCH_BOX);
        if (m_searchFocused) m_searchFocusedAtMillis = NowMillis();

        // Scrollbar drag (Search tab)
        if (hit == HIT_SCROLLBAR) {
            m_isScrolling = true;
            return;
        }

        // Search-grid clicks (creative source)
        if (hit == HIT_CREATIVE_GRID) {
            // Server-side button semantics in HandleCreativePickup:
            //   button=0 → cursor = full stack of this item
            //   button=1 → cursor = 1 of this item (or +1 if same; clear if different)
            //
            // Mapping (shift = stack, no-shift = single, regardless of L/R):
            //   plain left   → 1 on cursor
            //   plain right  → 1 on cursor (or +1 if same item already held)
            //   shift+left   → full stack on cursor
            //   shift+right  → full stack on cursor
            uint8_t btn = shift ? 0 : 1;
            QueueClick(Network::ContainerInput::PICKUP,
                       Network::InventorySlotSentinel::CREATIVE_GRID, btn,
                       m_hoveredCreativeStack.itemId);
            return;
        }

        // Trash slot:
        //   - Shift+click → clear ALL inventory slots (MC's CreativeModeInventoryScreen
        //     line 189-193: `if (slot == this.destroyItemSlot && quickKey)`).
        //   - Plain click with carried item → drop the cursor (visually disappears).
        if (hit == HIT_TRASH) {
            if (shift) {
                QueueClick(Network::ContainerInput::CREATIVE_DESTROY_ALL,
                           Network::InventorySlotSentinel::OUTSIDE, 0);
            } else if (!m_carriedItem.IsEmpty()) {
                QueueClick(Network::ContainerInput::THROW,
                           Network::InventorySlotSentinel::OUTSIDE, 1 /*full stack*/);
            }
            return;
        }

        // Outside-panel click (drop carried)
        if (hit == HIT_OUTSIDE) {
            if (!m_carriedItem.IsEmpty()) {
                uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 0 : 1;
                QueueClick(Network::ContainerInput::THROW,
                           Network::InventorySlotSentinel::OUTSIDE, btn);
            }
            return;
        }

        // Real slot click (hit >= 0)
        if (hit >= 0) {
            // Middle click → CLONE (creative)
            if (glfwButton == GLFW_MOUSE_BUTTON_MIDDLE) {
                QueueClick(Network::ContainerInput::CLONE, (int16_t)hit, 0);
                return;
            }

            // Shift+click → QUICK_MOVE
            if (shift) {
                uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
                QueueClick(Network::ContainerInput::QUICK_MOVE, (int16_t)hit, btn);
                return;
            }

            // Double-click detection (PICKUP_ALL)
            const long long now = NowMillis();
            const auto& slot = m_player ? m_player->inventory.GetSlot(hit) : Game::InventorySlot{};
            const bool sameSlot = (m_lastClickedSlot == hit);
            const bool fresh    = (now - m_lastClickTimeMs) <= DOUBLE_CLICK_MS;
            const bool cursorMatches = !m_carriedItem.IsEmpty() &&
                                       (slot.IsEmpty() || slot.itemId == m_carriedItem.itemId);

            if (sameSlot && fresh && cursorMatches && glfwButton == GLFW_MOUSE_BUTTON_LEFT) {
                QueueClick(Network::ContainerInput::PICKUP_ALL, (int16_t)hit, 0);
                m_lastClickTimeMs = 0;
                m_lastClickedSlot = HIT_NONE;
                return;
            }
            m_lastClickTimeMs  = now;
            m_lastClickedSlot  = hit;
            m_lastClickedItem  = slot.itemId;

            // Cursor non-empty → either drag (same item) or swap (different item).
            // Drag-distribute only makes sense when the slot is empty or holds
            // the SAME item as the cursor — otherwise the user expects the
            // cursor and the slot to swap contents (MC's standard behaviour).
            if (!m_carriedItem.IsEmpty()) {
                const bool slotCompatible = slot.IsEmpty()
                                         || slot.itemId == m_carriedItem.itemId;
                if (!slotCompatible) {
                    // Different item in slot → swap via PICKUP. HandlePickup's
                    // "both non-empty + different items" branch does std::swap
                    // on slot ↔ carried, which is exactly what we want here.
                    uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
                    QueueClick(Network::ContainerInput::PICKUP, (int16_t)hit, btn);
                    return;
                }
                int type = 0;
                if (glfwButton == GLFW_MOUSE_BUTTON_RIGHT)  type = 1;
                if (glfwButton == GLFW_MOUSE_BUTTON_MIDDLE) type = 2;
                m_dragType = (uint8_t)type;
                m_dragSlots.clear();
                m_dragSlots.push_back((uint8_t)hit);
                m_isDragging = true;
                // Snapshot the cursor count at drag start. The drag preview
                // (in RenderSlot / RenderCarriedItem) uses this to compute
                // what each touched slot WILL look like after END commits.
                m_dragStartCarriedCount = m_carriedItem.count;
                // Start phase
                QueueClick(Network::ContainerInput::QUICK_CRAFT, -1, QuickcraftMask(0, type));
                // Add this first slot
                QueueClick(Network::ContainerInput::QUICK_CRAFT, (int16_t)hit, QuickcraftMask(1, type));
                return;
            }

            // Otherwise plain PICKUP
            uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
            QueueClick(Network::ContainerInput::PICKUP, (int16_t)hit, btn);
        }
    }

    void InventoryScreen::OnMouseMove(double mouseX, double mouseY,
                                      int windowW, int windowH, int guiW, int guiH) {
        if (!m_open) return;
        // Convert window pixels → GUI virtual coords using same scale GuiGraphics uses.
        const float sx = (windowW > 0) ? ((float)guiW / (float)windowW) : 1.0f;
        const float sy = (windowH > 0) ? ((float)guiH / (float)windowH) : 1.0f;
        m_mouseGui.x = (float)mouseX * sx;
        m_mouseGui.y = (float)mouseY * sy;

        // Recompute hover for next frame's click decisions
        const int leftPos = LeftPos(guiW);
        const int topPos  = TopPos(guiH);
        m_hoveredSlot = HitTest(leftPos, topPos);

        // Scrollbar drag
        if (m_isScrolling) {
            float top = (float)(topPos + SCROLLBAR_Y);
            float trackH = (float)((SCROLLBAR_Y2 - SCROLLBAR_Y) - SCROLL_THUMB_H);
            float t = (m_mouseGui.y - top - 7.5f) / trackH;
            m_scrollOffs = std::max(0.0f, std::min(1.0f, t));
        }

        // Drag (QUICK_CRAFT) accumulator
        if (m_isDragging && m_hoveredSlot >= 0) {
            const uint8_t s = (uint8_t)m_hoveredSlot;
            if (std::find(m_dragSlots.begin(), m_dragSlots.end(), s) == m_dragSlots.end()) {
                m_dragSlots.push_back(s);
                QueueClick(Network::ContainerInput::QUICK_CRAFT, (int16_t)s, QuickcraftMask(1, m_dragType));
            }
        }
    }

    void InventoryScreen::OnScroll(double dy) {
        if (!m_open || m_currentTab != Tab::Search) return;
        if (!HasScrollBar()) return;
        const int rc = GetRowCount();
        if (rc <= 0) return;
        m_scrollOffs = std::max(0.0f, std::min(1.0f, m_scrollOffs - (float)dy / (float)rc));
    }

    void InventoryScreen::Update(float /*dt*/) {
        // Blink-driven by wall clock — no per-frame state to update.
    }

    // ─── Background loading ───────────────────────────────────
    TextureHandle InventoryScreen::EnsureBackground(bool survival) {
        TextureHandle& cache = survival ? m_inventoryBg : m_searchBg;
        if (cache != INVALID_TEXTURE) return cache;
        if (!g_renderBackend) return INVALID_TEXTURE;

        const std::string relPath = survival
            ? "assets/textures/gui/container/creative_inventory/tab_inventory.png"
            : "assets/textures/gui/container/creative_inventory/tab_item_search.png";

        // PlatformMain::GetAssetPath handles macOS app bundles and dev-build CWD fallback.
        const std::string full = PlatformMain::GetAssetPath(relPath);
        if (!std::filesystem::exists(full)) {
            Log::Warning("[InventoryScreen] Background not found: %s", full.c_str());
            cache = INVALID_TEXTURE;
            return INVALID_TEXTURE;
        }

        int w = 0, h = 0, ch = 0;
        // GUI ortho projection has Y top-to-bottom, so textures are loaded UN-flipped
        // (matches GuiAtlas.cpp:101). Flipping makes the image render upside-down.
        stbi_set_flip_vertically_on_load(0);
        unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!pixels) {
            Log::Warning("[InventoryScreen] stbi_load failed: %s", stbi_failure_reason());
            return INVALID_TEXTURE;
        }
        cache = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
        stbi_image_free(pixels);
        if (cache != INVALID_TEXTURE) {
            g_renderBackend->SetTextureFilter(cache, TextureFilter::Nearest, TextureFilter::Nearest);
            g_renderBackend->SetTextureWrap(cache, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        }
        return cache;
    }

    void InventoryScreen::DrawBackground(GuiGraphics& g, int leftPos, int topPos, TextureHandle bg) {
        if (bg == INVALID_TEXTURE) {
            // Fallback: solid dark grey panel
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
            return;
        }
        // The textures are 256×256 PNGs; the actual content is the top-left 195×136.
        const float u0 = 0.0f, v0 = 0.0f;
        const float u1 = (float)IMAGE_W / 256.0f;
        const float v1 = (float)IMAGE_H / 256.0f;
        g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, u0, v0, u1, v1);
    }

    // ─── Drag-preview helpers ────────────────────────────────
    // The server only commits drag distribution at QUICK_CRAFT END (mouse up).
    // While the user is still holding the button, the inventory would otherwise
    // show no feedback. Mirror MC's behaviour by computing locally what each
    // touched slot AND the cursor will look like after the drag commits, and
    // rendering THAT instead of the raw server state.
    int InventoryScreen::DragPerSlotCount() const {
        if (!m_isDragging || m_carriedItem.IsEmpty() || m_dragSlots.empty()) return 0;
        if (m_dragType == 0) {
            // Left-drag: split as evenly as possible across touched slots.
            return m_dragStartCarriedCount / static_cast<int>(m_dragSlots.size());
        }
        if (m_dragType == 1) {
            // Right-drag: 1 per slot.
            return 1;
        }
        // Middle (creative-clone): full stack per slot.
        return Game::ItemRegistry::Get(m_carriedItem.itemId).maxStackSize;
    }

    Game::InventorySlot InventoryScreen::DisplayedSlot(int slotIndex,
                                                       const Game::InventorySlot& base) const {
        if (!m_isDragging || m_carriedItem.IsEmpty()) return base;
        if (std::find(m_dragSlots.begin(), m_dragSlots.end(),
                      static_cast<uint8_t>(slotIndex)) == m_dragSlots.end()) {
            return base;
        }
        const int per = DragPerSlotCount();
        if (per <= 0) return base;
        const int maxStack = Game::ItemRegistry::Get(m_carriedItem.itemId).maxStackSize;
        if (base.IsEmpty()) {
            return Game::InventorySlot{m_carriedItem.itemId, std::min(per, maxStack)};
        }
        if (base.itemId == m_carriedItem.itemId) {
            // Top up an existing same-item stack (cap at max).
            int newCount = std::min(maxStack, base.count + per);
            return Game::InventorySlot{base.itemId, newCount};
        }
        // Different item in the slot — drag never overwrites these (server's
        // QUICK_CRAFT_addPlayerSlot also rejects them); show as-is.
        return base;
    }

    int InventoryScreen::DragRemainingCarriedCount() const {
        if (!m_isDragging || m_carriedItem.IsEmpty()) return m_carriedItem.count;
        const int per = DragPerSlotCount();
        // Only count slots that will actually accept items in the preview
        // (empty or same-item not-yet-full). Different-item slots receive 0.
        int distributed = 0;
        for (uint8_t s : m_dragSlots) {
            if (!m_player) break;
            const auto& base = m_player->inventory.GetSlot(s);
            if (base.IsEmpty() || base.itemId == m_carriedItem.itemId) {
                distributed += per;
            }
        }
        return std::max(0, m_dragStartCarriedCount - distributed);
    }

    // ─── Rendering ───────────────────────────────────────────
    void InventoryScreen::RenderSlot(GuiGraphics& g, int x, int y, const Game::InventorySlot& s) {
        if (s.IsEmpty()) return;
        g.RenderItem(s, x, y);
        g.NextStratum();
        g.RenderItemDecorations(s, x, y);
    }

    void InventoryScreen::RenderHoverHighlight(GuiGraphics& g, int x, int y) {
        // MC: 0x80FFFFFF translucent white over the 16×16 slot
        g.Fill(x, y, x + SLOT_SIZE, y + SLOT_SIZE, 0x80FFFFFF);
    }

    // Tab icon helpers — MC: CreativeModeTabs.java line 1814 (INVENTORY → Blocks.CHEST)
    // and line 1253 (SEARCH → Items.COMPASS). One unified RenderItem(ItemStack, x, y) call
    // dispatches on the item's renderType — block items render 3D, sprite items render flat.
    static void DrawSurvivalIcon(GuiGraphics& g, int x, int y) {
        Game::ItemStack icon{Game::ItemRegistry::FromBlock(Game::BlockID::Chest), 1};
        g.RenderItem(icon, x, y);
    }
    static void DrawSearchIcon(GuiGraphics& g, int x, int y) {
        Game::ItemStack icon{Game::Items::Compass, 1};
        g.RenderItem(icon, x, y);
    }

    void InventoryScreen::RenderUnselectedTabs(GuiGraphics& g, int leftPos, int topPos) {
        // Top-row tabs: y = topPos - 28. The bottom 4px of each tab overlaps the panel's top.
        const int tabY = topPos - 28;
        const bool surSel = (m_currentTab == Tab::Survival);
        const bool srcSel = (m_currentTab == Tab::Search);

        // Two-phase render so tab icons are FORCED on top of tab backgrounds.
        // Within a single stratum the GUI renderer doesn't always honour submission order
        // for blits (texture batching can reorder), and the compass icon was getting hidden
        // under the unselected Search tab background. Bumping stratum between BG and icon
        // is the explicit fix.
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

    void InventoryScreen::RenderSelectedTab(GuiGraphics& g, int leftPos, int topPos) {
        const int tabY = topPos - 28;
        const bool surSel = (m_currentTab == Tab::Survival);
        // Two-phase render (BG → bump stratum → icon) so the icon is guaranteed on top
        // of the tab background. See RenderUnselectedTabs for why this is needed.
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

    void InventoryScreen::RenderSurvivalTab(GuiGraphics& g, int leftPos, int topPos) {
        DrawBackground(g, leftPos, topPos, EnsureBackground(true));
        if (!m_player) return;
        const auto& inv = m_player->inventory;

        // Player preview — MC CreativeModeInventoryScreen.java:702 calls
        // InventoryScreen.renderEntityInInventoryFollowsMouse(graphics, leftPos+73,
        // topPos+6, leftPos+105, topPos+49, 20, 0.0625F, xm, ym, this.minecraft.player).
        // We render our stick figure into the same rect, with the same cursor-tracking
        // math. Bump stratum so the figure sits above the panel BG and below the slot
        // icons rendered after.
        {
            g.NextStratum();
            Render::StickFigurePose pose;
            pose.bodyYawDeg   = m_player->visualYaw;
            pose.headYawDeg   = m_player->visualYaw;
            pose.headPitchDeg = m_player->visualPitch;
            pose.isCrouching  = false;
            Render::RenderStickFigureInInventory(
                g,
                leftPos + 73, topPos + 6, leftPos + 105, topPos + 49,
                20, 0.0625f,
                m_mouseGui.x, m_mouseGui.y,
                pose,
                m_player->color);
            g.NextStratum();
        }

        // Armor placeholders. MC's AbstractContainerScreen.renderSlot (line 266) blits the
        // empty-armor sprite via slot.getNoItemIcon(). Sprite IDs come from
        // InventoryMenu.EMPTY_ARMOR_SLOT_* → "container/slot/{helmet,chestplate,leggings,boots}".
        // ARMOR_BEGIN+0=helmet, +1=chestplate, +2=leggings, +3=boots.
        static const char* kArmorSprites[4] = {
            "container/slot/helmet",
            "container/slot/chestplate",
            "container/slot/leggings",
            "container/slot/boots",
        };
        for (int i = 0; i < Game::Inventory::ARMOR_SIZE; ++i) {
            int unified = Game::Inventory::ARMOR_BEGIN + i;
            int sx, sy; if (!GetSlotImagePos(unified, sx, sy)) continue;
            const auto& s = inv.GetSlot(unified);
            if (s.IsEmpty()) {
                g.BlitSprite(kArmorSprites[i], leftPos + sx, topPos + sy, SLOT_SIZE, SLOT_SIZE);
            } else {
                RenderSlot(g, leftPos + sx, topPos + sy, s);
            }
        }

        // Offhand (shield) slot — MC InventoryMenu.EMPTY_ARMOR_SLOT_SHIELD = "container/slot/shield"
        {
            int sx, sy;
            if (GetSlotImagePos(Game::Inventory::OFFHAND_BEGIN, sx, sy)) {
                const auto& s = inv.GetSlot(Game::Inventory::OFFHAND_BEGIN);
                if (s.IsEmpty()) {
                    g.BlitSprite("container/slot/shield",
                                 leftPos + sx, topPos + sy, SLOT_SIZE, SLOT_SIZE);
                } else {
                    RenderSlot(g, leftPos + sx, topPos + sy, s);
                }
            }
        }

        // Crafting grid + result are HIDDEN in MC's creative survival tab
        // (CreativeModeInventoryScreen.java line 534-536: x=-2000, y=-2000). Don't render.

        // Main inventory + hotbar — DisplayedSlot overlays the live drag preview
        // (so the user sees their stack distribute as they drag) on top of the
        // raw server state.
        for (int i = Game::Inventory::MAIN_BEGIN; i < Game::Inventory::MAIN_BEGIN + Game::Inventory::MAIN_SIZE; ++i) {
            int sx, sy; if (!GetSlotImagePos(i, sx, sy)) continue;
            RenderSlot(g, leftPos + sx, topPos + sy, DisplayedSlot(i, inv.GetSlot(i)));
        }
        for (int i = Game::Inventory::HOTBAR_BEGIN; i < Game::Inventory::HOTBAR_BEGIN + Game::Inventory::HOTBAR_SIZE; ++i) {
            int sx, sy; if (!GetSlotImagePos(i, sx, sy)) continue;
            RenderSlot(g, leftPos + sx, topPos + sy, DisplayedSlot(i, inv.GetSlot(i)));
        }

        // Hover highlight — bump stratum first so it lands above the panel background
        // and slot icons regardless of how many slots actually contained items. (Without
        // this, an empty inventory leaves the stratum counter where DrawBackground left
        // it — RenderSlot only bumps for non-empty stacks — and within a stratum blits
        // draw on top of fill quads, hiding the highlight.)
        g.NextStratum();
        if (m_hoveredSlot >= 0) {
            int sx, sy;
            if (GetSlotImagePos(m_hoveredSlot, sx, sy)) {
                RenderHoverHighlight(g, leftPos + sx, topPos + sy);
            }
        } else if (m_hoveredSlot == HIT_TRASH) {
            RenderHoverHighlight(g, leftPos + TRASH_X, topPos + TRASH_Y);
        }
    }

    void InventoryScreen::RenderSearchTab(GuiGraphics& g, int leftPos, int topPos) {
        DrawBackground(g, leftPos, topPos, EnsureBackground(false));
        if (!m_player) return;

        // Hotbar always visible at bottom of search tab — DisplayedSlot adds
        // the live drag-distribute preview if a drag is in progress.
        const auto& inv = m_player->inventory;
        for (int i = Game::Inventory::HOTBAR_BEGIN; i < Game::Inventory::HOTBAR_BEGIN + Game::Inventory::HOTBAR_SIZE; ++i) {
            int sx, sy; if (!GetSlotImagePos(i, sx, sy)) continue;
            RenderSlot(g, leftPos + sx, topPos + sy, DisplayedSlot(i, inv.GetSlot(i)));
        }

        // Creative search grid (5×9) — paint each visible block as a 1-count slot.
        RefreshSearchResults();
        const int rowIndex = GetRowIndex();
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 9; ++col) {
                int idx = (rowIndex + row) * 9 + col;
                if (idx >= (int)m_filteredItems.size()) continue;
                // Render the pre-built ItemStack directly — it carries the
                // correct DataComponents (e.g. STORED_ENCHANTMENTS for an
                // enchanted_book variant), which RenderItem / its glint pass
                // need to read for foil detection.
                RenderSlot(g, leftPos + 9 + col * SLOT_STEP, topPos + 18 + row * SLOT_STEP,
                           m_filteredItems[idx]);
            }
        }

        // Search box + caret
        RenderSearchBox(g, leftPos, topPos);

        // Scrollbar
        RenderScrollbar(g, leftPos, topPos);

        // Hover highlight (over search grid OR hotbar) — see RenderSurvivalTab for why
        // we bump the stratum first.
        g.NextStratum();
        if (m_hoveredSlot >= 0) {
            int sx, sy;
            if (GetSlotImagePos(m_hoveredSlot, sx, sy)) {
                RenderHoverHighlight(g, leftPos + sx, topPos + sy);
            }
        } else if (m_hoveredSlot == HIT_CREATIVE_GRID && !m_hoveredCreativeStack.IsEmpty()) {
            // Find the cell again (cheap re-derive)
            const int mx = (int)std::floor(m_mouseGui.x) - leftPos;
            const int my = (int)std::floor(m_mouseGui.y) - topPos;
            int col = (mx - 9) / SLOT_STEP;
            int row = (my - 18) / SLOT_STEP;
            RenderHoverHighlight(g, leftPos + 9 + col * SLOT_STEP, topPos + 18 + row * SLOT_STEP);
        }
    }

    void InventoryScreen::RenderSearchBox(GuiGraphics& g, int leftPos, int topPos) {
        const int x = leftPos + SEARCH_X;
        const int y = topPos  + SEARCH_Y;
        // Background is already painted by tab_item_search.png — just draw text on top.
        if (m_searchText.empty()) {
            // MC shows a placeholder "Search..." in grey when empty + unfocused. Keep simple here.
        } else {
            g.DrawString(m_searchText, x, y, 0xFFFFFFFF, true);
        }
        // Caret blink (300ms on/off, MC EditBox.java line 408)
        if (m_searchFocused) {
            long long elapsed = NowMillis() - m_searchFocusedAtMillis;
            if (elapsed < 0) elapsed = 0;
            bool show = ((elapsed / 300LL) % 2LL) == 0LL;
            if (show) {
                std::string before = m_searchText.substr(0, m_searchCursorPos);
                int beforeW = g.GetStringWidth(before);
                if (m_searchCursorPos >= (int)m_searchText.size()) {
                    g.DrawString("_", x + beforeW + 1, y, 0xFFFFFFFF, true);
                } else {
                    g.Fill(x + beforeW, y - 1, x + beforeW + 1, y + 1 + 9, 0xFFFFFFFF);
                }
            }
        }
    }

    void InventoryScreen::RenderScrollbar(GuiGraphics& g, int leftPos, int topPos) {
        const int x = leftPos + SCROLLBAR_X;
        const int trackH = (SCROLLBAR_Y2 - SCROLLBAR_Y) - SCROLL_THUMB_H;
        const int y = topPos + SCROLLBAR_Y + (int)((float)trackH * m_scrollOffs);
        const char* sprite = HasScrollBar()
            ? "container/creative_inventory/scroller"
            : "container/creative_inventory/scroller_disabled";
        g.BlitSprite(sprite, x, y, SCROLL_THUMB_W, SCROLL_THUMB_H);
    }

    void InventoryScreen::RenderCarriedItem(GuiGraphics& g) {
        if (m_carriedItem.IsEmpty()) return;
        int x = (int)m_mouseGui.x - 8;
        int y = (int)m_mouseGui.y - 8;
        // While dragging, render the cursor with the PROJECTED remaining count
        // (start count minus what's been distributed so far) so the user sees
        // their stack shrink live as they drag across slots. After drag ends,
        // the server's SetCarried packet syncs the real value.
        Game::InventorySlot displayed = m_carriedItem;
        if (m_isDragging) displayed.count = DragRemainingCarriedCount();
        if (displayed.IsEmpty()) return;
        g.RenderItem(displayed, x, y);
        g.NextStratum();
        g.RenderItemDecorations(displayed, x, y);
    }

    void InventoryScreen::RenderTooltip(GuiGraphics& g, const Game::ItemStack& stack, int mx, int my) {
        if (stack.IsEmpty()) return;
        const std::string name = Game::ItemRegistry::Get(stack.itemId).name;
        if (name.empty()) return;

        // Build the line list: name first (white), then any per-stack annotations.
        // Mirrors MC's ItemStack.appendHoverText / DataComponentTooltips chain —
        // each component's TooltipProvider implementation appends its lines to
        // the consumer. Today only STORED_ENCHANTMENTS contributes; future
        // components (CUSTOM_NAME, LORE, durability bar, etc.) plug in here.
        struct Line { std::string text; uint32_t color; };
        std::vector<Line> lines;
        lines.push_back({name, 0xFFFFFFFFu});

        if (auto stored = stack.get(Game::DataComponents::STORED_ENCHANTMENTS)) {
            std::vector<Game::Enchantment::FormattedLine> ench;
            stored->AddToTooltip(ench);
            for (auto& l : ench) lines.push_back({std::move(l.text), l.colorARGB});
        }

        // Layout: 10-px line spacing matches MC's GuiGraphics tooltip spacing.
        const int LINE_H = 10;
        int textW = 0;
        for (const auto& l : lines) textW = std::max(textW, g.GetStringWidth(l.text));
        const int totalH = static_cast<int>(lines.size()) * LINE_H - 2; // last line has no trailing gap

        int x = mx + 12;
        int y = my - 12;
        // MC tooltip background colours (Screen.renderTooltip)
        const uint32_t bg     = 0xF0100010;
        const uint32_t border = 0x505000FF;
        g.Fill(x - 3, y - 4,           x + textW + 3, y - 3,           bg);
        g.Fill(x - 3, y + totalH + 3,  x + textW + 3, y + totalH + 4,  bg);
        g.Fill(x - 3, y - 3,           x + textW + 3, y + totalH + 3,  bg);
        g.Fill(x - 4, y - 3,           x - 3,         y + totalH + 3,  bg);
        g.Fill(x + textW + 3, y - 3,   x + textW + 4, y + totalH + 3,  bg);
        // Border (left + right)
        g.Fill(x - 3,         y - 3 + 1, x - 3 + 1,     y + totalH + 3 - 1, border);
        g.Fill(x + textW + 2, y - 3 + 1, x + textW + 3, y + totalH + 3 - 1, border);

        for (size_t i = 0; i < lines.size(); ++i) {
            g.DrawString(lines[i].text, x, y + static_cast<int>(i) * LINE_H, lines[i].color, true);
        }
    }

    void InventoryScreen::Render(GuiGraphics& g) {
        if (!m_open) return;
        const int guiW = g.GuiWidth();
        const int guiH = g.GuiHeight();
        const int leftPos = LeftPos(guiW);
        const int topPos  = TopPos(guiH);

        // Bump stratum FIRST so the dark overlay lands above the HUD that the host
        // already submitted (hotbar, hearts, hunger, etc.). Without this the overlay
        // is at the same stratum as the HUD and the HUD blits draw on top of it,
        // leaving the HUD looking un-dimmed while the world behind goes dark — which
        // doesn't match MC's Screen.renderBackground behaviour.
        g.NextStratum();
        g.Fill(0, 0, guiW, guiH, 0xA0101010);

        // Z-order: unselected tabs BEHIND the panel (panel covers their bottom 4px);
        // selected tab IN FRONT of the panel (its bottom 4px overlaps the panel top so
        // the tab visually merges with the panel). The renderer sorts by zOrder and within
        // a stratum draws blits before quads, so we need explicit NextStratum() between
        // each layer or quad icons would float above blits in the same stratum.
        g.NextStratum();
        RenderUnselectedTabs(g, leftPos, topPos);
        g.NextStratum();

        if (m_currentTab == Tab::Survival) RenderSurvivalTab(g, leftPos, topPos);
        else                                RenderSearchTab  (g, leftPos, topPos);
        g.NextStratum();

        RenderSelectedTab(g, leftPos, topPos);

        // Carried item follows the mouse, on top of everything except tooltip.
        RenderCarriedItem(g);

        // Tooltip — only when not carrying
        if (m_carriedItem.IsEmpty()) {
            if (m_hoveredSlot >= 0 && m_player) {
                const auto& s = m_player->inventory.GetSlot(m_hoveredSlot);
                if (!s.IsEmpty()) {
                    RenderTooltip(g, s, (int)m_mouseGui.x, (int)m_mouseGui.y);
                }
            } else if (m_hoveredSlot == HIT_CREATIVE_GRID && !m_hoveredCreativeStack.IsEmpty()) {
                RenderTooltip(g, m_hoveredCreativeStack, (int)m_mouseGui.x, (int)m_mouseGui.y);
            }
        }
    }

} // namespace Render
