// File: src/client/renderer/gui/AbstractContainerScreen.cpp
#include "AbstractContainerScreen.hpp"
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include "common/data/DataComponents.hpp"
#include "client/entity/Player.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace Render {

    namespace {
        long long NowMillis() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // QuickCraft mask helpers — match MC AbstractContainerMenu lines 749/753/757.
        inline uint8_t QuickcraftMask(int header, int type) {
            return (uint8_t)(((header & 3) << 2) | (type & 3));
        }

        // MC: `player.inventoryMenu`. One per local player; see the header for
        // why every container screen shares it.
        std::unique_ptr<Game::InventoryMenu> s_playerMenu;

        // MC: `player.containerMenu` when a BLOCK container is open (a crafting
        // table). Owns the table's grid, which exists only while the menu does.
        std::unique_ptr<Game::AbstractContainerMenu> s_openContainerMenu;
        Game::MenuType s_openMenuType = Game::MenuType::Inventory;

        // Somewhere harmless for cursor reads/writes to land before a player is
        // attached (the very first frames after launch, and the title screen).
        Game::ItemStack s_noMenuCarried{};

        // Whichever menu the cursor and the sync ids currently live on.
        Game::AbstractContainerMenu* CurrentMenu() {
            if (s_openContainerMenu) return s_openContainerMenu.get();
            return s_playerMenu.get();
        }

        // MC AbstractContainerScreen.SLOT_HIGHLIGHT_{BACK,FRONT}_SPRITE, blitted
        // as 24x24 at (slot.x - 4, slot.y - 4): back under the item, front over it.
        constexpr const char* kSlotHighlightBack  = "container/slot_highlight_back";
        constexpr const char* kSlotHighlightFront = "container/slot_highlight_front";
    }

    Game::InventoryMenu*         PlayerInventoryMenu() { return s_playerMenu.get(); }
    Game::AbstractContainerMenu* PlayerContainerMenu() { return CurrentMenu(); }

    void RebuildPlayerInventoryMenu(Game::ClientPlayer* player) {
        s_openContainerMenu.reset();
        s_openMenuType = Game::MenuType::Inventory;
        s_playerMenu = player ? std::make_unique<Game::InventoryMenu>(&player->inventory)
                              : nullptr;
        s_noMenuCarried.Clear();
    }

    // ─── Server-driven container state ───────────────────────────
    // Called from ClientPacketHandler.cpp through file-scope extern
    // declarations, so the network layer never has to include a GUI header.
    // All three write the CURRENT menu, which is what every screen reads — so
    // they land correctly no matter which screen (if any) is open.

    void SetInventoryScreenCarriedItem(const Game::ItemStack& stack) {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        Game::ItemStack& carried = menu ? menu->getCarried() : s_noMenuCarried;
        carried = stack;
        if (carried.IsEmpty()) carried.Clear();
    }

    void SetInventoryScreenStateId(uint32_t id) {
        if (Game::AbstractContainerMenu* menu = CurrentMenu()) menu->stateId = id;
    }

    void SetInventoryScreenContainerId(uint32_t id) {
        if (Game::AbstractContainerMenu* menu = CurrentMenu()) menu->containerId = id;
    }

    void ApplyContainerSlot(int menuIndex, const Game::ItemStack& stack) {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return;
        menu->GetSlot(menuIndex).Set(stack);
    }

    void ApplyContainerData(uint32_t containerId, uint16_t index, int32_t value) {
        Game::AbstractContainerMenu* menu = PlayerContainerMenu();
        if (!menu || menu->containerId != containerId) return;
        menu->SetData(static_cast<int>(index), value);
    }

    void ApplyContainerSlots(const std::vector<Game::ItemStack>& slots) {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        if (!menu) return;
        const int count = std::min(static_cast<int>(slots.size()), menu->SlotCount());
        for (int i = 0; i < count; ++i) menu->GetSlot(i).Set(slots[i]);
    }

    Game::MenuType ClientContainerMenuType() { return s_openMenuType; }

    void SetClientContainerMenu(std::unique_ptr<Game::AbstractContainerMenu> menu,
                                Game::MenuType type) {
        // MC AbstractContainerMenu.transferState — the cursor belongs to the
        // player, so it follows whichever menu is on top rather than being
        // stranded on the one going away.
        Game::ItemStack carried{};
        if (Game::AbstractContainerMenu* previous = CurrentMenu()) {
            carried = previous->getCarried();
            previous->setCarried(Game::ItemStack{});
        }

        s_openContainerMenu = std::move(menu);
        s_openMenuType = s_openContainerMenu ? type : Game::MenuType::Inventory;

        if (Game::AbstractContainerMenu* current = CurrentMenu()) {
            current->setCarried(carried);
        }
    }

    // ─── Cursor ──────────────────────────────────────────────────
    Game::InventorySlot& AbstractContainerScreen::Carried() {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        return menu ? menu->getCarried() : s_noMenuCarried;
    }
    const Game::InventorySlot& AbstractContainerScreen::Carried() const {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        return menu ? menu->getCarried() : s_noMenuCarried;
    }

    // ─── Open / close ────────────────────────────────────────────
    void AbstractContainerScreen::Open() {
        OpenSilently();
    }

    void AbstractContainerScreen::OpenSilently() {
        m_open = true;
        m_freshlyOpened = true;
        m_isDragging = false;
        m_dragSlots.clear();
        m_dragStartCarriedCount = 0;
        m_hoveredSlot = HIT_NONE;
        m_lastClickTimeMs = 0;
        m_lastClickedSlot = HIT_NONE;
        m_pendingClicks.clear();
        OnOpen();
    }

    void AbstractContainerScreen::Close() {
        if (!m_open) return;
        CloseSilently();

        // Tell the server, so it can put the cursor stack back and invalidate
        // the menu we were clicking against. PlatformMain turns the 0xFF action
        // into an InventoryCloseC2S — the click queue is the only channel this
        // screen has to the network, so the close rides it as a sentinel.
        Network::InventoryClickC2SPacket close{};
        close.action = 0xFF;
        m_pendingClicks.push_back(close);
    }

    void AbstractContainerScreen::CloseSilently() {
        if (!m_open) return;
        m_open = false;
        m_isDragging = false;
        m_dragSlots.clear();
        m_dragStartCarriedCount = 0;
        OnClose();
    }

    // ─── Click queue ─────────────────────────────────────────────
    bool AbstractContainerScreen::ConsumePendingClick(Network::InventoryClickC2SPacket& out) {
        if (m_pendingClicks.empty()) return false;
        out = m_pendingClicks.front();
        m_pendingClicks.erase(m_pendingClicks.begin());
        return true;
    }

    void AbstractContainerScreen::QueueClick(Network::ContainerInput action, int16_t slotIndex,
                                             uint8_t button, Game::ItemID creativeItem,
                                             const Game::ItemStack* creativeStack) {
        Game::AbstractContainerMenu* menu = Menu();

        Network::InventoryClickC2SPacket p{};
        p.slotIndex      = slotIndex;
        p.button         = button;
        p.action         = (uint8_t)action;
        p.flags          = 0;
        p.creativeItemId = creativeItem;
        p.stateId        = menu ? menu->stateId     : 0;
        p.containerId    = menu ? menu->containerId : 0;
        if (creativeStack) p.creativeStack = *creativeStack;

        // Predict BEFORE queueing, so the packet can carry the outcome.
        //
        // MC runs AbstractContainerMenu.doClick on the client for instant
        // feedback and reports the resulting slots back in
        // ServerboundContainerClickPacket. The server adopts those as its model
        // of what we believe and corrects only where that model disagrees with
        // the truth — so a correct prediction costs zero packets, and a slot we
        // wrongly wrote still gets fixed because we told the server we wrote it.
        if (menu) {
            menu->creative = m_player && m_player->IsCreative();

            const auto result = menu->DoClick(p);

            p.hasPrediction = true;
            p.predictedSlots.reserve(result.changedSlots.size());
            for (uint8_t slot : result.changedSlots) {
                if (menu->IsValidSlotIndex(slot)) {
                    p.predictedSlots.emplace_back(slot, menu->GetSlot(slot).GetItem());
                }
            }
            p.predictedCarried = Carried();
        }

        m_pendingClicks.push_back(p);
    }

    // ─── Slots ───────────────────────────────────────────────────
    bool AbstractContainerScreen::GetSlotPos(int menuIndex, int& outX, int& outY) const {
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return false;

        const Game::Slot& slot = menu->GetSlot(menuIndex);
        if (!slot.IsActive()) return false;
        outX = slot.x;
        outY = slot.y;
        return true;
    }

    const char* AbstractContainerScreen::GetNoItemIcon(int menuIndex) const {
        // MC AbstractContainerScreen.renderSlot: `slot.getNoItemIcon()`. The
        // menu decides, not the screen — the same index is a leggings slot in
        // one menu and a crafting cell in another.
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return nullptr;
        return menu->GetSlot(menuIndex).noItemIcon;
    }

    // ─── Hit testing ─────────────────────────────────────────────
    int AbstractContainerScreen::HitTest(int leftPos, int topPos) {
        const int lx = (int)std::floor(m_mouseGui.x) - leftPos;
        const int ly = (int)std::floor(m_mouseGui.y) - topPos;

        // Screen-owned zones first — some of them (creative's tabs) sit outside
        // the panel and would otherwise be swallowed by the bounds check below.
        const int extra = HitTestExtras(lx, ly);
        if (extra != HIT_NONE) return extra;

        if (lx < 0 || lx >= ImageWidth() || ly < 0 || ly >= ImageHeight()) {
            return HIT_OUTSIDE;
        }

        Game::AbstractContainerMenu* menu = Menu();
        if (menu) {
            for (int i = 0; i < menu->SlotCount(); ++i) {
                int sx, sy;
                if (!GetSlotPos(i, sx, sy)) continue;
                if (lx >= sx && lx < sx + SLOT_SIZE && ly >= sy && ly < sy + SLOT_SIZE) {
                    return i;
                }
            }
        }
        return HIT_NONE;
    }

    // ─── Input ───────────────────────────────────────────────────
    void AbstractContainerScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        HandleExtraCharInput(codepoint);
    }

    bool AbstractContainerScreen::OnKeyDown(int glfwKey, int glfwMods) {
        if (!m_open) return false;

        // ESC always closes, even mid-search.
        if (glfwKey == GLFW_KEY_ESCAPE) {
            Close();
            return true;
        }

        // The subclass gets first refusal: the creative search box needs the
        // letter keys, including the E that would otherwise close the screen.
        if (HandleExtraKey(glfwKey, glfwMods)) return true;

        if (glfwKey == GLFW_KEY_E) {
            Close();
            return true;
        }

        // Number keys: SWAP with hotbar slot (button = key - GLFW_KEY_1).
        if (m_hoveredSlot >= 0 && glfwKey >= GLFW_KEY_1 && glfwKey <= GLFW_KEY_9) {
            uint8_t button = (uint8_t)(glfwKey - GLFW_KEY_1);
            QueueClick(Network::ContainerInput::SWAP, (int16_t)m_hoveredSlot, button);
            return true;
        }

        // F: swap the hovered slot with the offhand. MC encodes the offhand as
        // button 40 in the SWAP action's player-inventory index space
        // (AbstractContainerMenu.doClick: `buttonNum < 9 || buttonNum == 40`).
        if (m_hoveredSlot >= 0 && glfwKey == GLFW_KEY_F) {
            QueueClick(Network::ContainerInput::SWAP, (int16_t)m_hoveredSlot, 40);
            return true;
        }

        // Q: drop. Ctrl+Q drops the whole stack (MC button 1).
        if (m_hoveredSlot >= 0 && glfwKey == GLFW_KEY_Q) {
            uint8_t button = (glfwMods & GLFW_MOD_CONTROL) ? 1 : 0;
            QueueClick(Network::ContainerInput::THROW, (int16_t)m_hoveredSlot, button);
            return true;
        }

        return true; // consume everything else while the screen is open
    }

    void AbstractContainerScreen::OnMouseButton(int glfwButton, int action, int mods) {
        if (!m_open) return;
        const bool press = (action == GLFW_PRESS);

        if (!press) {
            HandleExtraRelease();
            if (m_isDragging) {
                // The distribution is NOT committed by hand here: QueueClick
                // runs the same AbstractContainerMenu::DoClick the server will,
                // whose quick-craft END phase performs the exact distribution —
                // so a hand-rolled commit would double-apply it.
                QueueClick(Network::ContainerInput::QUICK_CRAFT, -1,
                           QuickcraftMask(2, m_dragType));
                m_isDragging = false;
                m_dragSlots.clear();
                m_dragStartCarriedCount = 0;
            }
            return;
        }

        const int  hit   = m_hoveredSlot;
        const bool shift = (mods & GLFW_MOD_SHIFT) != 0;

        // Tabs, search box, scrollbar, creative grid, trash — all subclass turf.
        // Called for every press (not just when `hit` is one of its zones) so a
        // screen can also react to a click landing elsewhere, e.g. dropping
        // search-box focus.
        if (HandleExtraClick(hit, glfwButton, shift)) return;

        // Outside-panel click drops the cursor. MC: left = whole stack
        // (button 0 / PRIMARY), right = one (button 1 / SECONDARY).
        if (hit == HIT_OUTSIDE) {
            if (!Carried().IsEmpty()) {
                uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
                QueueClick(Network::ContainerInput::THROW,
                           Network::InventorySlotSentinel::OUTSIDE, btn);
            }
            return;
        }

        if (hit < 0) return;

        Game::AbstractContainerMenu* menu = Menu();
        if (!menu) return;

        // Middle click → CLONE (creative).
        if (glfwButton == GLFW_MOUSE_BUTTON_MIDDLE) {
            QueueClick(Network::ContainerInput::CLONE, (int16_t)hit, 0);
            return;
        }

        // Shift+click → QUICK_MOVE.
        if (shift) {
            uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
            QueueClick(Network::ContainerInput::QUICK_MOVE, (int16_t)hit, btn);
            return;
        }

        const long long now  = NowMillis();
        const auto&     slot = menu->GetSlot(hit).GetItem();

        // Double-click → PICKUP_ALL. MC's precondition is `!slot.hasItem()`:
        // the canonical double-click picks the stack up on click 1 (leaving the
        // slot empty) and vacuums matching stacks on click 2.
        const bool sameSlot      = (m_lastClickedSlot == hit);
        const bool fresh         = (now - m_lastClickTimeMs) <= DOUBLE_CLICK_MS;
        const bool cursorMatches = !Carried().IsEmpty() && slot.IsEmpty();
        if (sameSlot && fresh && cursorMatches && glfwButton == GLFW_MOUSE_BUTTON_LEFT) {
            QueueClick(Network::ContainerInput::PICKUP_ALL, (int16_t)hit, 0);
            m_lastClickTimeMs = 0;
            m_lastClickedSlot = HIT_NONE;
            return;
        }
        m_lastClickTimeMs = now;
        m_lastClickedSlot = hit;

        // Cursor non-empty → either drag-distribute (same item / empty slot) or
        // a straight swap (different item).
        if (!Carried().IsEmpty()) {
            // Same Slot::MayPlace filter the server enforces — refusing locally
            // means no drag starts and no ghost item can appear.
            if (!menu->GetSlot(hit).MayPlace(Carried())) return;

            const bool slotCompatible = slot.IsEmpty() || slot.itemId == Carried().itemId;
            if (!slotCompatible) {
                // HandlePickup's "both non-empty + different items" branch does
                // std::swap on slot ↔ carried, which is what we want here.
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
            m_dragStartCarriedCount = Carried().count;
            QueueClick(Network::ContainerInput::QUICK_CRAFT, -1, QuickcraftMask(0, type));
            QueueClick(Network::ContainerInput::QUICK_CRAFT, (int16_t)hit, QuickcraftMask(1, type));
            return;
        }

        // Otherwise plain PICKUP.
        uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
        QueueClick(Network::ContainerInput::PICKUP, (int16_t)hit, btn);
    }

    void AbstractContainerScreen::OnMouseMove(double mouseX, double mouseY,
                                              int windowW, int windowH,
                                              int guiW, int guiH) {
        if (!m_open) return;
        // Convert window pixels → GUI virtual coords using the same scale
        // GuiGraphics uses.
        const float sx = (windowW > 0) ? ((float)guiW / (float)windowW) : 1.0f;
        const float sy = (windowH > 0) ? ((float)guiH / (float)windowH) : 1.0f;
        m_mouseGui.x = (float)mouseX * sx;
        m_mouseGui.y = (float)mouseY * sy;

        const int leftPos = LeftPos(guiW);
        const int topPos  = TopPos(guiH);
        m_hoveredSlot = HitTest(leftPos, topPos);

        OnExtraMouseMove(leftPos, topPos);

        // Drag (QUICK_CRAFT) accumulator.
        if (m_isDragging && m_hoveredSlot >= 0) {
            Game::AbstractContainerMenu* menu = Menu();
            const uint8_t s = (uint8_t)m_hoveredSlot;
            if (menu && menu->GetSlot(m_hoveredSlot).MayPlace(Carried()) &&
                std::find(m_dragSlots.begin(), m_dragSlots.end(), s) == m_dragSlots.end()) {
                m_dragSlots.push_back(s);
                QueueClick(Network::ContainerInput::QUICK_CRAFT, (int16_t)s,
                           QuickcraftMask(1, m_dragType));
            }
        }
    }

    void AbstractContainerScreen::OnScroll(double dy) {
        if (!m_open) return;
        HandleExtraScroll(dy);
    }

    void AbstractContainerScreen::Update(float /*dt*/) {
        if (!m_open) return;
        ContainerTick();
    }

    // ─── Drag-preview helpers ────────────────────────────────────
    int AbstractContainerScreen::DragPerSlotCount() const {
        if (!m_isDragging || Carried().IsEmpty() || m_dragSlots.empty()) return 0;
        if (m_dragType == 0) {
            // Left-drag: split as evenly as possible across touched slots.
            return m_dragStartCarriedCount / static_cast<int>(m_dragSlots.size());
        }
        if (m_dragType == 1) {
            // Right-drag: 1 per slot.
            return 1;
        }
        // Middle (creative-clone): full stack per slot.
        return Game::ItemRegistry::Get(Carried().itemId).maxStackSize;
    }

    Game::InventorySlot AbstractContainerScreen::DisplayedSlot(
            int slotIndex, const Game::InventorySlot& base) const {
        if (!m_isDragging || Carried().IsEmpty()) return base;
        if (std::find(m_dragSlots.begin(), m_dragSlots.end(),
                      static_cast<uint8_t>(slotIndex)) == m_dragSlots.end()) {
            return base;
        }
        const int per = DragPerSlotCount();
        if (per <= 0) return base;

        Game::AbstractContainerMenu* menu = Menu();
        // Cap at the SLOT's limit, matching the authoritative end-phase commit.
        const int maxStack = menu ? menu->GetSlot(slotIndex).GetMaxStackSize(Carried())
                                  : Game::ItemRegistry::Get(Carried().itemId).maxStackSize;
        if (base.IsEmpty()) {
            // Copy the cursor stack so the preview shows the real item —
            // constructing from a bare id drops components, previewing a plain
            // book where an enchanted one will land.
            Game::InventorySlot preview = Carried();
            preview.count = std::min(per, maxStack);
            return preview;
        }
        // Same item AND components, matching CanItemQuickReplace — a
        // differently enchanted stack does not merge, so it must not preview
        // as merging.
        if (Game::IsSameItemSameComponents(base, Carried())) {
            Game::InventorySlot preview = base;
            preview.count = std::min(maxStack, base.count + per);
            return preview;
        }
        // Different item in the slot — the drag never overwrites those.
        return base;
    }

    int AbstractContainerScreen::DragRemainingCarriedCount() const {
        if (!m_isDragging || Carried().IsEmpty()) return Carried().count;
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu) return Carried().count;

        const int per = DragPerSlotCount();
        // Only count slots that will actually accept items in the preview
        // (empty or same-item not-yet-full). Different-item slots receive 0.
        int distributed = 0;
        for (uint8_t s : m_dragSlots) {
            if (!menu->IsValidSlotIndex(s)) continue;
            const auto& base = menu->GetSlot(s).GetItem();
            if (base.IsEmpty() || Game::IsSameItemSameComponents(base, Carried())) {
                distributed += per;
            }
        }
        return std::max(0, m_dragStartCarriedCount - distributed);
    }

    // ─── Rendering ───────────────────────────────────────────────
    void AbstractContainerScreen::RenderSlots(GuiGraphics& g, int leftPos, int topPos) {
        Game::AbstractContainerMenu* menu = Menu();
        if (!menu) return;

        for (int i = 0; i < menu->SlotCount(); ++i) {
            int sx, sy;
            if (!GetSlotPos(i, sx, sy)) continue;
            const int x = leftPos + sx;
            const int y = topPos  + sy;

            // DisplayedSlot overlays the live drag preview on the server state.
            const Game::InventorySlot shown = DisplayedSlot(i, menu->GetSlot(i).GetItem());
            if (shown.IsEmpty()) {
                if (const char* icon = GetNoItemIcon(i)) {
                    g.BlitSprite(icon, x, y, SLOT_SIZE, SLOT_SIZE);
                }
                continue;
            }
            g.RenderItem(shown, x, y);
            g.NextStratum();
            g.RenderItemDecorations(shown, x, y);
        }
    }

    void AbstractContainerScreen::RenderHoverHighlight(GuiGraphics& g, int x, int y, bool front) {
        // MC blits the 24x24 nine-sliced highlight sprite centred on the 16x16
        // slot: the back half under the item, the front half over it.
        g.BlitSprite(front ? kSlotHighlightFront : kSlotHighlightBack, x - 4, y - 4, 24, 24);
    }

    void AbstractContainerScreen::RenderSlotHighlight(GuiGraphics& g, int leftPos, int topPos,
                                                      bool front) {
        if (m_hoveredSlot >= 0) {
            int sx, sy;
            if (GetSlotPos(m_hoveredSlot, sx, sy)) {
                RenderHoverHighlight(g, leftPos + sx, topPos + sy, front);
            }
            return;
        }
        // Non-slot cells (creative grid / trash) only get the front pass —
        // there is no menu slot to draw a back highlight under.
        if (front) RenderExtraHoverHighlight(g, leftPos, topPos);
    }

    void AbstractContainerScreen::RenderCarriedItem(GuiGraphics& g) {
        if (Carried().IsEmpty()) return;
        const int x = (int)m_mouseGui.x - 8;
        const int y = (int)m_mouseGui.y - 8;
        // While dragging, show the PROJECTED remaining count so the stack
        // visibly shrinks; the server's SetCarried syncs the real value at END.
        Game::InventorySlot displayed = Carried();
        if (m_isDragging) displayed.count = DragRemainingCarriedCount();
        if (displayed.IsEmpty()) return;
        g.RenderItem(displayed, x, y);
        g.NextStratum();
        g.RenderItemDecorations(displayed, x, y);
    }

    void AbstractContainerScreen::RenderTooltip(GuiGraphics& g, const Game::ItemStack& stack,
                                                int mx, int my) {
        if (stack.IsEmpty()) return;

        // Name line — mirrors ItemStack.getStyledHoverName:
        //   CUSTOM_NAME (anvil rename; MC renders italic — our font can't)
        //   → ITEM_NAME (data-driven base name)
        //   → registry display name,
        // coloured by the RARITY component (Rarity.color(), WHITE default).
        std::string name;
        if (auto custom = stack.get(Game::DataComponents::CUSTOM_NAME)) {
            name = *custom;
        } else if (auto itemName = stack.get(Game::DataComponents::ITEM_NAME)) {
            name = *itemName;
        } else {
            name = Game::ItemRegistry::Get(stack.itemId).name;
        }
        if (name.empty()) return;
        const uint32_t nameColor = Game::RarityColorARGB(
            stack.get(Game::DataComponents::RARITY).value_or(Game::Rarity::COMMON));

        // Build the line list: name first, then per-component annotations.
        // Mirrors MC's ItemStack.appendHoverText / DataComponentTooltips chain —
        // each component's TooltipProvider appends its lines. Order: name →
        // enchantments → lore (matching MC's addDetailsToTooltip).
        struct Line { std::string text; uint32_t color; };
        std::vector<Line> lines;
        lines.push_back({name, nameColor});

        if (auto stored = stack.get(Game::DataComponents::STORED_ENCHANTMENTS)) {
            std::vector<Game::Enchantment::FormattedLine> ench;
            stored->AddToTooltip(ench);
            for (auto& l : ench) lines.push_back({std::move(l.text), l.colorARGB});
        }

        // LORE lines — MC ItemLore.LORE_STYLE = DARK_PURPLE + italic (no italics
        // in our font; colour carries the style).
        if (auto lore = stack.get(Game::DataComponents::LORE)) {
            for (const auto& loreLine : lore->lines) {
                lines.push_back({loreLine, 0xFFAA00AAu});   // DARK_PURPLE
            }
        }

        // Bundle contents — MC renders a slot grid (BundleTooltip); listed as
        // "Name xN" lines here until a grid tooltip exists. Gray, newest first.
        if (auto bundle = stack.get(Game::DataComponents::BUNDLE_CONTENTS)) {
            for (const auto& inner : bundle->items) {
                if (inner.IsEmpty()) continue;
                lines.push_back({Game::ItemRegistry::Get(inner.itemId).name
                                     + " x" + std::to_string(inner.count),
                                 0xFFAAAAAAu});   // GRAY
            }
        }

        // Layout: 10-px line spacing matches MC's GuiGraphics tooltip spacing.
        const int LINE_H = 10;
        int textW = 0;
        for (const auto& l : lines) textW = std::max(textW, g.GetStringWidth(l.text));
        const int totalH = static_cast<int>(lines.size()) * LINE_H - 2; // no trailing gap

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
            g.DrawString(lines[i].text, x, y + static_cast<int>(i) * LINE_H,
                         lines[i].color, true);
        }
    }

    void AbstractContainerScreen::Render(GuiGraphics& g) {
        if (!m_open) return;
        const int guiW    = g.GuiWidth();
        const int guiH    = g.GuiHeight();
        const int leftPos = LeftPos(guiW);
        const int topPos  = TopPos(guiH);

        // Bump the stratum FIRST so the dark overlay lands above the HUD the
        // host already submitted (hotbar, hearts, hunger). Without this the
        // overlay shares the HUD's stratum and the HUD blits draw on top of it,
        // leaving the HUD un-dimmed while the world behind goes dark — not what
        // MC's Screen.renderBackground does.
        g.NextStratum();
        g.Fill(0, 0, guiW, guiH, 0xA0101010);

        // Every layer gets its own stratum: the renderer sorts by stratum and
        // within one it draws blits before fills, so without explicit bumps a
        // panel blit would cover a highlight fill submitted after it.
        g.NextStratum();
        RenderBehindBg(g, leftPos, topPos);

        g.NextStratum();
        RenderBg(g, leftPos, topPos);

        g.NextStratum();
        RenderSlotHighlight(g, leftPos, topPos, /*front=*/false);

        g.NextStratum();
        RenderSlots(g, leftPos, topPos);
        RenderExtraSlots(g, leftPos, topPos);

        g.NextStratum();
        RenderSlotHighlight(g, leftPos, topPos, /*front=*/true);

        g.NextStratum();
        RenderLabels(g, leftPos, topPos);
        RenderExtras(g, leftPos, topPos);

        // Carried stack follows the mouse, on top of everything but the tooltip.
        g.NextStratum();
        RenderCarriedItem(g);

        // Tooltip — only when not carrying.
        if (!Carried().IsEmpty()) return;
        if (m_hoveredSlot >= 0) {
            Game::AbstractContainerMenu* menu = Menu();
            if (menu && menu->IsValidSlotIndex(m_hoveredSlot)) {
                const auto& s = menu->GetSlot(m_hoveredSlot).GetItem();
                if (!s.IsEmpty()) RenderTooltip(g, s, (int)m_mouseGui.x, (int)m_mouseGui.y);
            }
        } else if (const Game::ItemStack* extra = HoveredExtraStack()) {
            if (!extra->IsEmpty()) {
                RenderTooltip(g, *extra, (int)m_mouseGui.x, (int)m_mouseGui.y);
            }
        }
    }

} // namespace Render
