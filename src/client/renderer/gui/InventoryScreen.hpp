// File: src/client/renderer/gui/InventoryScreen.hpp
// MC-style Creative Mode inventory screen with two tabs:
//   - Survival Inventory (Type::Survival)
//   - Search Items       (Type::Search)
// Mirrors net.minecraft.client.gui.screens.inventory.CreativeModeInventoryScreen.
#pragma once

#include "common/entity/Inventory.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/network/PacketTypes.hpp"
#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace Game { class ClientPlayer; }

namespace Render {

    class GuiGraphics;

    class InventoryScreen {
    public:
        enum class Tab : uint8_t { Survival = 0, Search = 1 };

        // MC pixel constants (CreativeModeInventoryScreen.java)
        static constexpr int IMAGE_W       = 195;
        static constexpr int IMAGE_H       = 136;
        static constexpr int SLOT_SIZE     = 16;
        static constexpr int SLOT_STEP     = 18;
        static constexpr int TAB_W         = 26;
        static constexpr int TAB_H         = 32;
        static constexpr int TAB_SPACING   = 27;
        static constexpr int SEARCH_X      = 82;
        static constexpr int SEARCH_Y      = 6;
        static constexpr int SEARCH_W      = 80;
        static constexpr int SEARCH_H      = 9;
        static constexpr int SEARCH_MAX_LEN = 50;
        static constexpr int SCROLLBAR_X   = 175;
        static constexpr int SCROLLBAR_X2  = 189;
        static constexpr int SCROLLBAR_Y   = 18;
        static constexpr int SCROLLBAR_Y2  = 130;
        static constexpr int SCROLL_THUMB_W = 12;
        static constexpr int SCROLL_THUMB_H = 15;
        static constexpr int TRASH_X       = 173;
        static constexpr int TRASH_Y       = 112;
        static constexpr int DOUBLE_CLICK_MS = 250;

        // Special hit-test return values (separate from real slot indices 0..45)
        static constexpr int HIT_NONE          = -1;
        static constexpr int HIT_OUTSIDE       = -10;
        static constexpr int HIT_CREATIVE_GRID = -11;
        static constexpr int HIT_TRASH         = -12;
        static constexpr int HIT_TAB_SURVIVAL  = -20;
        static constexpr int HIT_TAB_SEARCH    = -21;
        static constexpr int HIT_SEARCH_BOX    = -30;
        static constexpr int HIT_SCROLLBAR     = -31;

        void Open();
        void Close();
        bool IsOpen() const { return m_open; }

        // Player pointer (provides access to local inventory mirror).
        void SetPlayer(Game::ClientPlayer* p) { m_player = p; }

        // Carried item state (cursor) — driven by server packets.
        void SetCarriedItem(Game::ItemID id, int count);

        // Per-frame input
        void OnCharInput(unsigned int codepoint);
        bool OnKeyDown(int glfwKey, int glfwMods);                // returns true if consumed
        void OnMouseButton(int glfwButton, int action, int mods); // GLFW_PRESS/RELEASE
        void OnMouseMove(double mouseX, double mouseY, int windowW, int windowH, int guiW, int guiH);
        void OnScroll(double dy);
        void Update(float deltaTime);
        void Render(GuiGraphics& graphics);

        // Drain queued clicks (PlatformMain forwards them to the server).
        bool ConsumePendingClick(Network::InventoryClickC2SPacket& out);

    private:
        // ─── State ───────────────────────────────────────────
        bool                m_open = false;
        Tab                 m_currentTab = Tab::Survival;
        Game::InventorySlot m_carriedItem{};
        int                 m_hoveredSlot = HIT_NONE;
        // The creative search shows multiple stacks of the same item-id when an
        // item supports per-stack variants (notably enchanted_book — one stack
        // per enchantment+level). So we track the FULL stack for hover, not just
        // the id, so the tooltip can read DataComponents like STORED_ENCHANTMENTS.
        Game::ItemStack     m_hoveredCreativeStack{};
        glm::vec2           m_mouseGui{0.0f, 0.0f};
        Game::ClientPlayer* m_player = nullptr;

        // Search
        std::string                m_searchText;
        int                        m_searchCursorPos = 0;
        long long                  m_searchFocusedAtMillis = 0;
        bool                       m_searchFocused = false;
        // Stores fully-formed stacks (with DataComponents) so per-stack variants
        // — e.g. enchanted_book at every (enchantment, level) — each get their own
        // grid cell with their own tooltip and (for the glint pass) per-cell
        // foil state.
        std::vector<Game::ItemStack>  m_filteredItems;
        bool                       m_searchDirty = true;  // refilter on next render
        void                       RefreshSearchResults();

        // Scroll (Search tab only)
        float m_scrollOffs   = 0.0f;
        bool  m_isScrolling  = false;
        int   GetRowCount() const;       // ceil(filtered/9) - 5
        int   GetRowIndex() const;       // floor(scrollOffs * rowCount + 0.5)
        bool  HasScrollBar() const;

        // Drag (QUICK_CRAFT)
        bool                 m_isDragging = false;
        uint8_t              m_dragType = 0;   // 0=split, 1=one-each, 2=clone
        std::vector<uint8_t> m_dragSlots;
        // Cursor count when drag began. The drag preview shows what each
        // touched slot WILL look like after the END phase commits server-side
        // (since the server doesn't actually update slot counts until END).
        // The cursor itself is rendered with `m_dragStartCarriedCount` minus
        // what's been distributed so the user sees their stack shrinking live.
        int                  m_dragStartCarriedCount = 0;

        // Double-click
        long long      m_lastClickTimeMs = 0;
        int            m_lastClickedSlot = HIT_NONE;
        Game::ItemID   m_lastClickedItem = Game::Items::Air;

        // Pending packets to send to server (drained by PlatformMain)
        std::vector<Network::InventoryClickC2SPacket> m_pendingClicks;

        // Backgrounds (lazy-loaded)
        TextureHandle m_inventoryBg = INVALID_TEXTURE;
        TextureHandle m_searchBg    = INVALID_TEXTURE;

        // ─── Layout helpers ──────────────────────────────────
        int LeftPos(int guiW) const { return (guiW - IMAGE_W) / 2; }
        int TopPos(int guiH)  const { return (guiH - IMAGE_H) / 2; }

        // Per-slot pixel position inside the panel (image-relative). Returns false if `slot`
        // doesn't have a position in the current tab (e.g., armor on Search tab).
        bool GetSlotImagePos(int slotIndex, int& outX, int& outY) const;

        // Determine what's under the mouse this frame.
        int  HitTest(int leftPos, int topPos);

        // ─── Rendering helpers ───────────────────────────────
        // MC layered tab rendering: unselected tabs drawn BEFORE panel BG (so the panel
        // covers their top 4px → "tucked under" look), selected tab drawn AFTER
        // (so its top 4px overlays the panel's bottom 4px → "merged" look).
        void RenderUnselectedTabs(GuiGraphics&, int leftPos, int topPos);
        void RenderSelectedTab   (GuiGraphics&, int leftPos, int topPos);
        void RenderSurvivalTab(GuiGraphics&, int leftPos, int topPos);
        void RenderSearchTab  (GuiGraphics&, int leftPos, int topPos);
        void RenderSlot       (GuiGraphics&, int x, int y, const Game::InventorySlot& s);
        void RenderHoverHighlight(GuiGraphics&, int x, int y);
        void RenderCarriedItem(GuiGraphics&);

        // Drag-distribute preview helpers — see InventoryScreen.cpp for the
        // full doc. DisplayedSlot returns the slot's CONTENT as the user
        // should currently SEE it (live drag preview overlaid on server state).
        // DragRemainingCarriedCount mirrors that for the cursor stack.
        int                  DragPerSlotCount() const;
        Game::InventorySlot  DisplayedSlot(int slotIndex,
                                           const Game::InventorySlot& base) const;
        int                  DragRemainingCarriedCount() const;
        // Reads STORED_ENCHANTMENTS (and future per-stack components) off the
        // stack to render multi-line tooltips matching MC's format. See
        // Enchantment::GetFullname for the per-line colour rules.
        void RenderTooltip    (GuiGraphics&, const Game::ItemStack& stack, int mx, int my);
        void RenderSearchBox  (GuiGraphics&, int leftPos, int topPos);
        void RenderScrollbar  (GuiGraphics&, int leftPos, int topPos);
        void DrawBackground   (GuiGraphics&, int leftPos, int topPos, TextureHandle bg);
        TextureHandle EnsureBackground(bool survival);

        // Click queue helper
        void QueueClick(Network::ContainerInput action, int16_t slotIndex,
                        uint8_t button, Game::ItemID creativeItem = Game::Items::Air);

        void SwitchTab(Tab t);
    };

    // Free-function singleton accessor (avoids extern hassle in PlatformMain).
    InventoryScreen& GetInventoryScreen();

} // namespace Render
