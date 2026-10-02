// File: src/client/renderer/gui/AbstractContainerScreen.cpp
#include "AbstractContainerScreen.hpp"
#include "platform/GameDirectory.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "GuiGraphics.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "FontRenderer.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Instruments.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"
#include "common/data/components/ComponentTooltips.hpp"
#include "client/entity/Player.hpp"
#include "common/entity/Item.hpp"        // IsSameItemSameComponents (Ctrl+Shift+Q)
#include "common/entity/decoration/PaintingVariants.hpp"
#include "common/entity/mobs/TropicalFishVariant.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Items::Painting
#include "client/input/Input.hpp"        // IsGlfwKeyDown (shift-drag)
#include "client/input/KeyMapping.hpp"
#include "client/map/ClientMaps.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <memory>

namespace Render {

    namespace {
        long long NowMillis() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // MC KeyMapping.matches(KeyEvent): the press is this action's key,
        // whatever the player bound it to.
        bool IsBoundKey(const Input::KeyMapping* mapping, int glfwKey) {
            return mapping && mapping->key == Input::BoundKey::Keyboard(glfwKey);
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

    namespace {
        // Bumped whenever the server writes into the open menu's slots —
        // what a screen that is not a slot grid (the lectern's book view)
        // polls to learn its contents changed (MC ContainerListener.slotChanged).
        uint32_t s_containerContentRevision = 0;
    }

    uint32_t ContainerContentRevision() { return s_containerContentRevision; }

    void ApplyContainerSlot(int menuIndex, const Game::ItemStack& stack) {
        Game::AbstractContainerMenu* menu = CurrentMenu();
        if (!menu || !menu->IsValidSlotIndex(menuIndex)) return;
        menu->GetSlot(menuIndex).Set(stack);
        ++s_containerContentRevision;
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
        ++s_containerContentRevision;
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
        EndShiftDrag();
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
        EndShiftDrag();
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
        // A spectator looks and never takes: the server refuses every click
        // (MC handleContainerClick's `!player.isSpectator()`), so none is
        // predicted or sent — the screen is read-only.
        if (m_player && m_player->IsSpectator()) return;

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
                // MC AbstractContainerScreen.isHovering(slot): the 16x16
                // item square grown by 1 px on every side, so neighbouring
                // slots (18 px apart) meet with no dead gap between them.
                if (lx >= sx - 1 && lx < sx + SLOT_SIZE + 1 && ly >= sy - 1 && ly < sy + SLOT_SIZE + 1) {
                    return i;
                }
            }
        }
        return HIT_NONE;
    }

    int AbstractContainerScreen::SlotAtGuiPoint(glm::vec2 gui, int leftPos, int topPos) const {
        const int lx = (int)std::floor(gui.x) - leftPos;
        const int ly = (int)std::floor(gui.y) - topPos;
        if (lx < 0 || lx >= ImageWidth() || ly < 0 || ly >= ImageHeight()) return HIT_NONE;

        Game::AbstractContainerMenu* menu = Menu();
        if (!menu) return HIT_NONE;
        for (int i = 0; i < menu->SlotCount(); ++i) {
            int sx, sy;
            if (!GetSlotPos(i, sx, sy)) continue;
            if (lx >= sx - 1 && lx < sx + SLOT_SIZE + 1 && ly >= sy - 1 && ly < sy + SLOT_SIZE + 1) return i;
        }
        return HIT_NONE;
    }

    // ─── Shift-drag quick move ───────────────────────────────────
    void AbstractContainerScreen::ShiftDragQuickMove(int slot) {
        if (!m_isShiftDragging || slot < 0 || static_cast<size_t>(slot) >= m_shiftDragDone.size()) return;
        if (m_shiftDragDone[static_cast<size_t>(slot)]) return;
        m_shiftDragDone[static_cast<size_t>(slot)] = true;

        Game::AbstractContainerMenu* menu = Menu();
        if (!menu || !menu->IsValidSlotIndex(slot)) return;
        // An empty slot has nothing to move (MC quickMoveStack returns EMPTY
        // straight away); skipping it just saves the packet. It stays spent
        // either way — "each slot once per drag" is about slots crossed.
        if (menu->GetSlot(slot).GetItem().IsEmpty()) return;

        const size_t queuedBefore = m_pendingClicks.size();
        QueueClick(Network::ContainerInput::QUICK_MOVE, static_cast<int16_t>(slot), 0);
        // Every slot the predicted move wrote — the source and each stack it
        // landed on — is spent too, so the drag never carries a stack it just
        // delivered back out when the cursor crosses where it landed (a sweep
        // from the hotbar up into the main inventory, say).
        if (m_pendingClicks.size() > queuedBefore) {
            for (const auto& written : m_pendingClicks.back().predictedSlots) {
                const size_t index = written.first;
                if (index < m_shiftDragDone.size()) m_shiftDragDone[index] = true;
            }
        }
    }

    void AbstractContainerScreen::EndShiftDrag() {
        m_isShiftDragging = false;
        m_shiftDragDone.clear();
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

        // MC AbstractContainerScreen.keyPressed: options.keyInventory.
        if (IsBoundKey(Input::Binds::Inventory, glfwKey)) {
            Close();
            return true;
        }

        // Hotbar keys: SWAP with that hotbar slot (button = slot index;
        // MC checkHotbarKeyPressed: options.keyHotbarSlots[i]).
        if (m_hoveredSlot >= 0) {
            for (int i = 0; i < 9; ++i) {
                if (!IsBoundKey(Input::Binds::Hotbar[i], glfwKey)) continue;
                QueueClick(Network::ContainerInput::SWAP, (int16_t)m_hoveredSlot, (uint8_t)i);
                return true;
            }
        }

        // Swap-offhand key (F): swap the hovered slot with the offhand. MC encodes the offhand as
        // button 40 in the SWAP action's player-inventory index space
        // (AbstractContainerMenu.doClick: `buttonNum < 9 || buttonNum == 40`).
        if (m_hoveredSlot >= 0 && IsBoundKey(Input::Binds::SwapOffhand, glfwKey)) {
            QueueClick(Network::ContainerInput::SWAP, (int16_t)m_hoveredSlot, 40);
            return true;
        }

        // Drop key (Q): drop. Ctrl+Q drops the whole stack (MC button 1). Ctrl+Shift+Q
        // (no vanilla counterpart) drops every stack of the hovered item
        // from the player's inventory: one stack-THROW per matching slot,
        // queued together so they predict and send as one batch. Only the
        // player's own slots take part — a chest's contents stay put — and
        // "the same item" means id and components, so an enchanted pickaxe
        // does not take the plain ones with it.
        if (m_hoveredSlot >= 0 && IsBoundKey(Input::Binds::Drop, glfwKey)) {
            const bool ctrl  = (glfwMods & GLFW_MOD_CONTROL) != 0;
            const bool shift = (glfwMods & GLFW_MOD_SHIFT) != 0;
            if (ctrl && shift) {
                Game::AbstractContainerMenu* menu = Menu();
                // THROW needs an empty cursor (HandleThrow refuses otherwise),
                // so with something carried nothing would happen anyway.
                if (menu && Carried().IsEmpty()) {
                    // A copy: the hovered slot empties as its own click predicts.
                    const Game::ItemStack kind = menu->GetSlot(m_hoveredSlot).GetItem();
                    const Game::IContainer* playerInventory = &menu->getInventory();
                    if (!kind.IsEmpty()) {
                        for (int i = 0; i < menu->SlotCount(); ++i) {
                            const Game::Slot& s = menu->GetSlot(i);
                            if (s.container != playerInventory) continue;
                            if (!Game::IsSameItemSameComponents(s.GetItem(), kind)) continue;
                            QueueClick(Network::ContainerInput::THROW, (int16_t)i, 1);
                        }
                    }
                }
                return true;
            }
            QueueClick(Network::ContainerInput::THROW, (int16_t)m_hoveredSlot, ctrl ? 1 : 0);
            return true;
        }

        return true; // consume everything else while the screen is open
    }

    void AbstractContainerScreen::OnMouseButton(int glfwButton, int action, int mods) {
        if (!m_open) return;
        const bool press = (action == GLFW_PRESS);

        if (!press) {
            HandleExtraRelease();
            if (glfwButton == GLFW_MOUSE_BUTTON_LEFT) EndShiftDrag();
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

        // Ctrl+Shift+left click → QUICK_MOVE of every stack of the hovered
        // item on the hovered slot's side (MC's shift+double-click: same
        // container, same item and components), the hovered one first.
        // Works both ways — inventory into a chest, or a chest's stacks
        // out — as one predicted batch.
        const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
        if (shift && ctrl && glfwButton == GLFW_MOUSE_BUTTON_LEFT) {
            const Game::ItemStack kind = menu->GetSlot(hit).GetItem();   // a copy: the slot empties as it predicts
            if (!kind.IsEmpty()) {
                const Game::IContainer* side = menu->GetSlot(hit).container;
                QueueClick(Network::ContainerInput::QUICK_MOVE, (int16_t)hit, 0);
                for (int i = 0; i < menu->SlotCount(); ++i) {
                    if (i == hit) continue;
                    const Game::Slot& s = menu->GetSlot(i);
                    if (s.container != side) continue;
                    if (!Game::IsSameItemSameComponents(s.GetItem(), kind)) continue;
                    QueueClick(Network::ContainerInput::QUICK_MOVE, (int16_t)i, 0);
                }
            }
            return;
        }

        // Shift+click → QUICK_MOVE.
        //
        // A shift + LEFT press on an empty cursor also starts a shift-drag:
        // while both stay held, every further slot the cursor enters gets the
        // same QUICK_MOVE, once (OnMouseMove → ShiftDragQuickMove). Vanilla
        // has no such gesture — its nearest relative is shift + double-click,
        // which quick-moves every stack of the clicked item on that side
        // (our Ctrl+Shift+click above) — so this follows Mouse Tweaks' shift
        // + LMB drag, the de-facto standard. Each step is an ordinary
        // QUICK_MOVE click, so it obeys the open menu's quickMoveStack rules
        // (chest ⇄ inventory, hotbar ⇄ main inventory / armour / offhand in
        // the player's own menu, a furnace's fuel/input routing, …) and the
        // server validates it like any shift-click. With a stack on the
        // cursor the drag never starts: that button belongs to MC's
        // quick-craft drag-splitting.
        if (shift) {
            uint8_t btn = (glfwButton == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 0;
            const bool startDrag = glfwButton == GLFW_MOUSE_BUTTON_LEFT && Carried().IsEmpty();
            if (startDrag) {
                m_isShiftDragging = true;
                m_shiftDragDone.assign(static_cast<size_t>(menu->SlotCount()), false);
                m_shiftDragLastGui = m_mouseGui;
                ShiftDragQuickMove(hit);
            } else {
                QueueClick(Network::ContainerInput::QUICK_MOVE, (int16_t)hit, btn);
            }
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

        // Shift-drag quick move. Letting go of shift ends the gesture (the
        // left button's release ends it too, in OnMouseButton). The cursor is
        // polled once a frame, so a fast sweep can jump several slots between
        // two samples: walk the segment from the last position in steps well
        // under the 2-pixel gutter between slots, so every slot on the path
        // is entered, in path order.
        if (m_isShiftDragging) {
            const bool shiftHeld = Input::IsGlfwKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                   Input::IsGlfwKeyDown(GLFW_KEY_RIGHT_SHIFT);
            Game::AbstractContainerMenu* menu = Menu();
            if (!shiftHeld || !menu ||
                static_cast<size_t>(menu->SlotCount()) != m_shiftDragDone.size()) {
                // A menu swap under the drag (the server closed the chest)
                // invalidates every index it recorded.
                EndShiftDrag();
            } else {
                const glm::vec2 from  = m_shiftDragLastGui;
                const glm::vec2 delta = m_mouseGui - from;
                const float     dist  = std::max(std::fabs(delta.x), std::fabs(delta.y));
                const int       steps = std::max(1, static_cast<int>(std::ceil(dist)));
                for (int i = 1; i <= steps && m_isShiftDragging; ++i) {
                    const glm::vec2 p = from + delta * (static_cast<float>(i) / static_cast<float>(steps));
                    const int s = (i == steps) ? m_hoveredSlot : SlotAtGuiPoint(p, leftPos, topPos);
                    if (s >= 0) ShiftDragQuickMove(s);
                }
                m_shiftDragLastGui = m_mouseGui;
            }
        }

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
        return Game::GetMaxStackSize(Carried());
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
                                  : Game::GetMaxStackSize(Carried());
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

    namespace {

        constexpr uint32_t kTooltipWhite      = 0xFFFFFFFFu;
        constexpr uint32_t kTooltipGray       = 0xFFAAAAAAu;
        constexpr uint32_t kTooltipDarkGray   = 0xFF555555u;
        constexpr uint32_t kTooltipBlue       = 0xFF5555FFu;
        constexpr uint32_t kTooltipRed        = 0xFFFF5555u;
        constexpr uint32_t kTooltipDarkGreen  = 0xFF00AA00u;

        // Component.translatable(key, args…) as a plain string.
        std::string Translated(const char* key, std::initializer_list<std::string> args) {
            std::vector<Game::Text::Component> with;
            for (const std::string& a : args) with.push_back(Game::Text::Component::Literal(a));
            return Game::Text::GetString(Game::Text::Component::Translatable(key, with));
        }

        // ItemAttributeModifiers.ATTRIBUTE_MODIFIER_FORMAT: DecimalFormat("#.##").
        std::string FormatModifierAmount(double v) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            std::string t = buf;
            while (!t.empty() && t.back() == '0') t.pop_back();
            if (!t.empty() && t.back() == '.') t.pop_back();
            if (t == "-0") t = "0";
            return t;
        }

        // Attribute.getStyle(valueIncrease) over its Sentiment.
        uint32_t AttributeStyle(Game::Attribute attribute, bool increase) {
            switch (Game::GetAttributeDef(attribute).sentiment) {
                case Game::AttributeSentiment::Positive: return increase ? kTooltipBlue : kTooltipRed;
                case Game::AttributeSentiment::Negative: return increase ? kTooltipRed : kTooltipBlue;
                case Game::AttributeSentiment::Neutral:  return kTooltipGray;
            }
            return kTooltipBlue;
        }

        // Attribute.getDescriptionId, translated (a pack without the key
        // shows the attribute's name in title case).
        std::string AttributeDisplayName(Game::Attribute attribute) {
            std::string fallback(Game::AttributeName(attribute));
            bool capitalize = true;
            for (char& c : fallback) {
                if (c == '_') { c = ' '; capitalize = true; continue; }
                if (capitalize) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalize = false;
            }
            return Game::Language::GetOrDefault(Game::AttributeDescriptionId(attribute), fallback);
        }

        int OperationId(Game::AttributeOperation op) { return static_cast<int>(op); }

    } // namespace

    // MC ItemStack.getTooltipLines — the lines only, no drawing. RenderTooltip
    // draws them; the creative screen also indexes them for its search (MC
    // SessionSearchTrees.updateCreativeTooltips searches the tooltip text).
    //
    // The order is ItemStack.addDetailsToTooltip's, one step per
    // ComponentTooltips::Slot; every step asks TOOLTIP_DISPLAY first
    // (TooltipDisplay.shows), and the components whose lines are produced in
    // common code are the providers registered against their slot.
    void AbstractContainerScreen::BuildTooltipLines(const Game::ItemStack& stack, bool advanced,
                                                    std::vector<TooltipLine>& lines) {
        namespace CT = Game::ComponentTooltips;
        namespace DC = Game::DataComponents;
        if (stack.IsEmpty()) return;

        const bool creative = m_player && m_player->IsCreative();
        const Game::TooltipDisplay display = Game::GetTooltipDisplay(stack);
        // `!tooltipFlag.isCreative() && display.hideTooltip()`: no tooltip.
        if (!creative && display.hideTooltip) return;
        const auto shows = [&display](const Game::DataComponentTypeBase& type) { return display.Shows(type); };
        const CT::Context context{advanced, creative};

        const auto push = [&lines](std::string text, uint32_t color) {
            lines.push_back(TooltipLine{std::move(text), color, std::nullopt});
        };
        // Every provider registered for a slot (ComponentTooltips).
        const auto providers = [&](CT::Slot slot) {
            for (const CT::Registration& reg : CT::ForSlot(slot)) {
                if (reg.type && (!stack.has(*reg.type) || !display.Shows(*reg.type))) continue;
                std::vector<CT::Line> out;
                reg.provider(stack, context, out);
                for (CT::Line& l : out) lines.push_back(TooltipLine{std::move(l.text), l.argb, std::move(l.rich)});
            }
        };

        // ── getStyledHoverName: the hover name in the rarity's colour,
        // italic when renamed, with the name's own styles.
        {
            Game::Text::Component name = Game::GetStyledHoverName(stack);
            std::string plain = Game::Text::GetString(name);
            if (plain.empty()) return;
            lines.push_back(TooltipLine{std::move(plain),
                                        Game::RarityColorARGB(static_cast<Game::Rarity>(Game::GetStackRarity(stack))),
                                        std::move(name)});
        }

        // ── Item.appendHoverText ─────────────────────────────────────────
        // DiscFragmentItem: "<descriptionId>.desc" in grey ("Music Disc - 5").
        if (stack.itemId == Game::Items::DiscFragment5) {
            push(Game::Language::Get("item.minecraft.disc_fragment_5.desc"), kTooltipGray);
        }
        // SmithingTemplateItem: "Smithing Template", then "Applies to:" /
        // "Ingredients:" (grey) over their blue descriptions.
        {
            const std::string_view slug = Game::ItemRegistry::Slug(stack.itemId);
            constexpr std::string_view kSuffix = "_smithing_template";
            if (slug.size() > kSuffix.size() && slug.substr(slug.size() - kSuffix.size()) == kSuffix) {
                const bool netherite = slug == "netherite_upgrade_smithing_template";
                const std::string kind = netherite ? "netherite_upgrade" : "armor_trim";
                push(Game::Language::Get("item.minecraft.smithing_template"), kTooltipGray);
                push("", kTooltipWhite);
                push(Game::Language::Get("item.minecraft.smithing_template.applies_to"), kTooltipGray);
                push(" " + Game::Language::Get("item.minecraft.smithing_template." + kind + ".applies_to"), kTooltipBlue);
                push(Game::Language::Get("item.minecraft.smithing_template.ingredients"), kTooltipGray);
                push(" " + Game::Language::Get("item.minecraft.smithing_template." + kind + ".ingredients"), kTooltipBlue);
            }
        }
        // HangingEntityItem (the painting): the canvas's title and author
        // (each in its variant JSON's colour), then "painting.dimensions"; a
        // plain painting says "Random variant", in grey, to a creative player
        // only (tooltipFlag.isCreative()).
        if (stack.itemId == Game::Items::Painting && shows(DC::PAINTING_VARIANT)) {
            const Game::PaintingVariant* variant = nullptr;
            if (auto id = stack.get(DC::PAINTING_VARIANT)) {
                variant = Game::PaintingVariants::Get(Game::PaintingVariants::IndexOf(*id));
            }
            const auto colorOf = [](const std::string& name) {
                const auto parsed = Game::Text::TextColor::Parse(name);
                return parsed ? (0xFF000000u | parsed->rgb) : kTooltipWhite;
            };
            if (variant) {
                for (const auto* line : { &variant->title, &variant->author }) {
                    if (!*line) continue;
                    push(Game::Language::GetOrDefault((*line)->translate, (*line)->translate), colorOf((*line)->color));
                }
                std::string dims = Game::Language::GetOrDefault("painting.dimensions", "%sx%s");
                for (int value : { variant->width, variant->height }) {
                    const size_t at = dims.find("%s");
                    if (at != std::string::npos) dims.replace(at, 2, std::to_string(value));
                }
                push(dims, kTooltipWhite);
            } else if (creative) {
                push(Game::Language::GetOrDefault("painting.random", "Random variant"), kTooltipGray);
            }
        }
        providers(CT::Slot::AppendHoverText);

        // ── TROPICAL_FISH_PATTERN — TropicalFish.Pattern.addToTooltip: the
        // named fish ("Clownfish"), else the pattern and its colours, ITALIC
        // GRAY (grey here — the font has no italics).
        if (auto pattern = stack.get(DC::TROPICAL_FISH_PATTERN); pattern && shows(DC::TROPICAL_FISH_PATTERN)) {
            namespace TFV = Game::TropicalFishVariants;
            TFV::Variant variant = TFV::kDefaultVariant;
            if (*pattern >= 0 && *pattern < TFV::kPatternCount) variant.pattern = static_cast<TFV::Pattern>(*pattern);
            if (auto c = stack.get(DC::TROPICAL_FISH_BASE_COLOR)) variant.baseColor = TFV::DyeById(*c);
            if (auto c = stack.get(DC::TROPICAL_FISH_PATTERN_COLOR)) variant.patternColor = TFV::DyeById(*c);
            for (std::string& line : TFV::TooltipLines(variant)) push(std::move(line), kTooltipGray);
        }
        providers(CT::Slot::TropicalFishPattern);

        // ── INSTRUMENT — InstrumentComponent.addToTooltip: the description.
        if (auto instrument = stack.get(DC::INSTRUMENT); instrument && shows(DC::INSTRUMENT)) {
            const std::string description = Game::Instruments::DescriptionOf(*instrument);
            if (!description.empty()) push(description, kTooltipGray);
        }
        providers(CT::Slot::Instrument);

        // ── MAP_ID — MapId.addToTooltip: "Unknown Map" with no data; else
        // "ID #n" (unless renamed or about to be locked / scaled), "Locked",
        // and with advanced tooltips the scale and zoom level.
        if (auto mapId = stack.get(DC::MAP_ID); mapId && shows(DC::MAP_ID)) {
            const auto data = Client::Maps::GetMapData(*mapId);
            if (!data) {
                push(Game::Language::Get("filled_map.unknown"), kTooltipGray);
            } else {
                const auto post = stack.get(DC::MAP_POST_PROCESSING);
                if (!stack.get(DC::CUSTOM_NAME) && !post) {
                    push(Translated("filled_map.id", {std::to_string(*mapId)}), kTooltipGray);
                }
                if (data->locked || (post && *post == Game::Maps::MapPostProcessing::Lock)) {
                    push(Game::Language::Get("filled_map.locked"), kTooltipGray);
                }
                if (advanced) {
                    const int scaleToAdd = post && *post == Game::Maps::MapPostProcessing::Scale ? 1 : 0;
                    const int scale = std::min(data->scale + scaleToAdd, Game::Maps::kMaxScale);
                    push(Translated("filled_map.scale", {std::to_string(1 << scale)}), kTooltipGray);
                    push(Translated("filled_map.level", {std::to_string(scale), "4"}), kTooltipGray);
                }
            }
        }
        providers(CT::Slot::MapId);
        providers(CT::Slot::Bees);
        providers(CT::Slot::ContainerLoot);

        // ── CONTAINER — ItemContainerContents.addToTooltip: the first five
        // stacks as "<name> x<count>", then "and N more...".
        if (auto contents = stack.get(DC::CONTAINER); contents && shows(DC::CONTAINER)) {
            int lineCount = 0, itemCount = 0;
            for (const Game::ItemStack& item : contents->items) {
                if (item.IsEmpty()) continue;
                ++itemCount;
                if (lineCount <= 4) {
                    ++lineCount;
                    push(Translated("container.shulkerBox.itemCount",
                                    {Game::GetItemStackHoverName(item), std::to_string(item.count)}),
                         kTooltipWhite);
                }
            }
            if (itemCount - lineCount > 0) {
                push(Translated("container.shulkerBox.more", {std::to_string(itemCount - lineCount)}), kTooltipWhite);
            }
        }
        providers(CT::Slot::Container);
        providers(CT::Slot::BannerPatterns);

        // ── POT_DECORATIONS — PotDecorations.addToTooltip: unless EMPTY, a
        // blank line and each side's item name in grey, front, left, right,
        // back.
        if (auto decorations = stack.get(DC::POT_DECORATIONS);
            decorations && !decorations->IsEmpty() && shows(DC::POT_DECORATIONS)) {
            push("", kTooltipWhite);
            for (int side : { 3, 1, 2, 0 }) {
                const Game::ItemID id = decorations->sides[static_cast<size_t>(side)];
                if (id == Game::Items::Air) continue;
                push(Game::GetItemStackHoverName(Game::ItemStack(id, 1)), kTooltipGray);
            }
        }
        providers(CT::Slot::PotDecorations);

        // ── WRITTEN_BOOK_CONTENT — "by <author>" when the author is not
        // blank, then the generation, both grey.
        if (auto book = stack.get(DC::WRITTEN_BOOK_CONTENT); book && shows(DC::WRITTEN_BOOK_CONTENT)) {
            const bool blankAuthor = book->author.find_first_not_of(" \t\r\n") == std::string::npos;
            if (!blankAuthor) push(Translated("book.byAuthor", {book->author}), kTooltipGray);
            push(Game::Language::Get("book.generation." + std::to_string(book->generation)), kTooltipGray);
        }
        providers(CT::Slot::WrittenBookContent);

        // ── CHARGED_PROJECTILES, FIREWORKS, FIREWORK_EXPLOSION ────────────
        {
            // The lines an item-stack template adds of its own (its
            // addDetailsToTooltip), for a loaded crossbow's projectiles.
            const auto projectileDetails = [](const Game::ItemStack& projectile,
                                              std::vector<Game::FireworkItems::TooltipLine>& out) {
                if (auto fw = projectile.get(DC::FIREWORKS)) Game::FireworkItems::AddFireworksTooltip(*fw, out);
                if (auto fe = projectile.get(DC::FIREWORK_EXPLOSION)) Game::FireworkItems::AddExplosionTooltip(*fe, out);
                if (auto potion = projectile.get(DC::POTION_CONTENTS)) {
                    std::vector<Game::PotionTooltipLine> potionLines;
                    Game::AddPotionTooltip(potion->GetAllEffects(), potionLines,
                                           projectile.get(DC::POTION_DURATION_SCALE).value_or(1.0f));
                    for (auto& l : potionLines) out.push_back({std::move(l.text), l.colorARGB});
                }
            };
            // ChargedProjectiles.addToTooltip: runs of matching stacks,
            // "Projectile: [name]" / "Projectile: N x [name]", each followed
            // by its details indented in grey.
            if (auto charged = stack.get(DC::CHARGED_PROJECTILES);
                charged && !charged->IsEmpty() && shows(DC::CHARGED_PROJECTILES)) {
                const auto addRun = [&](const Game::ItemStack& projectile, int count) {
                    if (count == 1) {
                        push(Translated("item.minecraft.crossbow.projectile.single",
                                        {"[" + Game::GetItemStackHoverName(projectile) + "]"}),
                             kTooltipWhite);
                    } else {
                        push(Translated("item.minecraft.crossbow.projectile.multiple",
                                        {std::to_string(count), Game::GetItemStackHoverName(projectile)}),
                             kTooltipWhite);
                    }
                    std::vector<Game::FireworkItems::TooltipLine> details;
                    projectileDetails(projectile, details);
                    for (auto& d : details) push("  " + d.text, kTooltipGray);
                };
                const Game::ItemStack* current = nullptr;
                int count = 0;
                for (const Game::ItemStack& projectile : charged->items) {
                    if (!current) {
                        current = &projectile;
                        count = 1;
                    } else if (Game::ItemStacksMatch(*current, projectile)) {
                        ++count;
                    } else {
                        addRun(*current, count);
                        current = &projectile;
                        count = 1;
                    }
                }
                if (current) addRun(*current, count);
            }
            providers(CT::Slot::ChargedProjectiles);
            std::vector<Game::FireworkItems::TooltipLine> fireworkLines;
            if (auto fw = stack.get(DC::FIREWORKS); fw && shows(DC::FIREWORKS)) {
                Game::FireworkItems::AddFireworksTooltip(*fw, fireworkLines);
            }
            for (auto& l : fireworkLines) push(std::move(l.text), l.argb);
            providers(CT::Slot::Fireworks);
            fireworkLines.clear();
            if (auto fe = stack.get(DC::FIREWORK_EXPLOSION); fe && shows(DC::FIREWORK_EXPLOSION)) {
                Game::FireworkItems::AddExplosionTooltip(*fe, fireworkLines);
            }
            for (auto& l : fireworkLines) push(std::move(l.text), l.argb);
            providers(CT::Slot::FireworkExplosion);
        }

        // ── POTION_CONTENTS — every effect with its potency and (scaled)
        // duration, "No Effects" for none, and the "When Applied:" lines.
        if (auto potion = stack.get(DC::POTION_CONTENTS); potion && shows(DC::POTION_CONTENTS)) {
            std::vector<Game::PotionTooltipLine> potionLines;
            Game::AddPotionTooltip(potion->GetAllEffects(), potionLines,
                                   stack.get(DC::POTION_DURATION_SCALE).value_or(1.0f));
            for (auto& l : potionLines) push(std::move(l.text), l.colorARGB);
        }
        providers(CT::Slot::PotionContents);

        // ── JUKEBOX_PLAYABLE — the song's description in grey. (A stack
        // whose JUKEBOX_PLAYABLE provider is registered answers there.)
        if (CT::ForSlot(CT::Slot::JukeboxPlayable).empty()) {
            if (const std::string& song = Game::ItemRegistry::Get(stack.itemId).jukeboxSongDescription; !song.empty()) {
                push(song, kTooltipGray);
            }
        }
        providers(CT::Slot::JukeboxPlayable);
        providers(CT::Slot::Trim);

        // ── STORED_ENCHANTMENTS, ENCHANTMENTS — ItemEnchantments.addToTooltip.
        if (auto stored = stack.get(DC::STORED_ENCHANTMENTS); stored && shows(DC::STORED_ENCHANTMENTS)) {
            std::vector<Game::Enchantment::FormattedLine> ench;
            stored->AddToTooltip(ench);
            for (auto& l : ench) push(std::move(l.text), l.colorARGB);
        }
        providers(CT::Slot::StoredEnchantments);
        if (auto enchantments = stack.get(DC::ENCHANTMENTS); enchantments && shows(DC::ENCHANTMENTS)) {
            std::vector<Game::Enchantment::FormattedLine> ench;
            enchantments->AddToTooltip(ench);
            for (auto& l : ench) push(std::move(l.text), l.colorARGB);
        }
        providers(CT::Slot::Enchantments);

        // ── DYED_COLOR — DyedItemColor.addToTooltip: "Color: #RRGGBB" with
        // advanced tooltips, else "Dyed" (grey, italic).
        if (auto dyed = stack.get(DC::DYED_COLOR); dyed && shows(DC::DYED_COLOR)) {
            if (advanced) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "#%06X", static_cast<unsigned>(*dyed) & 0xFFFFFFu);
                push(Translated("item.color", {hex}), kTooltipGray);
            } else {
                push(Game::Language::GetOrDefault("item.dyed", "Dyed"), kTooltipGray);
            }
        }
        providers(CT::Slot::DyedColor);
        providers(CT::Slot::Profile);
        // ── LORE (PresentationComponents registers ItemLore's provider).
        providers(CT::Slot::Lore);

        // ── SULFUR_CUBE_CONTENT — "Contains: <block>" (grey italic).
        if (auto sulfur = stack.get(DC::SULFUR_CUBE_CONTENT);
            sulfur && !sulfur->absorbed.IsEmpty() && shows(DC::SULFUR_CUBE_CONTENT)) {
            push(Translated("entity.minecraft.sulfur_cube.content", {Game::GetItemStackHoverName(sulfur->absorbed)}),
                 kTooltipGray);
        }
        providers(CT::Slot::SulfurCubeContent);

        // ── ItemStack.addAttributeTooltips: per EquipmentSlotGroup (MC's
        // declaration order), the stack's forEachModifier(group) — its
        // ATTRIBUTE_MODIFIERS entries, then its enchantments' — under a
        // blank line and "When in Main Hand:" (item.modifiers.<group>). A
        // Display.Default line reads as the player's total for the
        // BASE_ATTACK_DAMAGE / BASE_ATTACK_SPEED entries (" 6 Attack Damage",
        // dark green), else "+N" / "-N" (percent for the multiplied
        // operations, knockback resistance times ten) in the attribute's
        // sentiment colour; an override prints its own text; hidden, nothing.
        if (shows(DC::ATTRIBUTE_MODIFIERS)) {
            for (int g = 0; g < Game::kEquipmentSlotGroupCount; ++g) {
                const auto group = static_cast<Game::EquipmentSlotGroup>(g);
                bool first = true;
                Game::ForEachTooltipModifier(stack, group, [&](const Game::TooltipModifier& m) {
                    using DisplayType = Game::ItemAttributeModifiers::Display::Type;
                    if (m.display.type == DisplayType::Hidden) return;
                    if (first) {
                        push("", kTooltipWhite);
                        push(Game::Language::Get("item.modifiers." + std::string(Game::EquipmentSlotGroupName(group))),
                             kTooltipGray);
                        first = false;
                    }
                    if (m.display.type == DisplayType::Override) {
                        lines.push_back(TooltipLine{Game::Text::GetString(m.display.value), kTooltipWhite, m.display.value});
                        return;
                    }
                    double amount = m.modifier.amount;
                    bool displayWithBase = false;
                    if (m_player) {
                        const std::string id = m.id.find(':') == std::string::npos ? "minecraft:" + m.id : m.id;
                        if (id == Game::kBaseAttackDamageId) {
                            amount += Game::PlayerBaseAttributeValue(Game::Attribute::AttackDamage);
                            displayWithBase = true;
                        } else if (id == Game::kBaseAttackSpeedId) {
                            amount += Game::PlayerBaseAttributeValue(Game::Attribute::AttackSpeed);
                            displayWithBase = true;
                        }
                    }
                    const Game::AttributeOperation op = m.modifier.operation;
                    double displayAmount = amount;
                    if (op == Game::AttributeOperation::AddMultipliedBase ||
                        op == Game::AttributeOperation::AddMultipliedTotal) {
                        displayAmount = amount * 100.0;
                    } else if (m.attribute == Game::Attribute::KnockbackResistance) {
                        displayAmount = amount * 10.0;
                    }
                    const std::string name = AttributeDisplayName(m.attribute);
                    const std::string opId = std::to_string(OperationId(op));
                    if (displayWithBase) {
                        push(" " + Translated(("attribute.modifier.equals." + opId).c_str(),
                                              {FormatModifierAmount(displayAmount), name}),
                             kTooltipDarkGreen);
                    } else if (amount > 0.0) {
                        push(Translated(("attribute.modifier.plus." + opId).c_str(),
                                        {FormatModifierAmount(displayAmount), name}),
                             AttributeStyle(m.attribute, true));
                    } else if (amount < 0.0) {
                        push(Translated(("attribute.modifier.take." + opId).c_str(),
                                        {FormatModifierAmount(-displayAmount), name}),
                             AttributeStyle(m.attribute, false));
                    }
                });
            }
        }
        providers(CT::Slot::AttributeModifiers);

        // ── addUnitComponentToTooltip(INTANGIBLE_PROJECTILE / UNBREAKABLE).
        providers(CT::Slot::IntangibleProjectile);
        if (stack.get(DC::UNBREAKABLE) && shows(DC::UNBREAKABLE)) {
            push(Game::Language::Get("item.unbreakable"), kTooltipBlue);
        }
        providers(CT::Slot::Unbreakable);
        providers(CT::Slot::SignTextFront);
        providers(CT::Slot::SignTextBack);

        // ── OMINOUS_BOTTLE_AMPLIFIER — the Bad Omen it gives, as a potion's
        // effect line.
        if (auto amplifier = stack.get(DC::OMINOUS_BOTTLE_AMPLIFIER); amplifier && shows(DC::OMINOUS_BOTTLE_AMPLIFIER)) {
            std::vector<Game::MobEffectInstance> effects;
            effects.emplace_back(Game::MobEffectId::BadOmen, DC::kOminousBottleEffectDuration,
                                 *amplifier, /*ambient=*/false, /*visible=*/false, /*showIcon=*/true);
            std::vector<Game::PotionTooltipLine> omenLines;
            Game::AddPotionTooltip(effects, omenLines, 1.0f);
            for (auto& l : omenLines) push(std::move(l.text), l.colorARGB);
        }
        providers(CT::Slot::OminousBottleAmplifier);

        // ── SUSPICIOUS_STEW_EFFECTS — listed only when flag.isCreative().
        if (auto stew = stack.get(DC::SUSPICIOUS_STEW_EFFECTS); stew && creative && shows(DC::SUSPICIOUS_STEW_EFFECTS)) {
            std::vector<Game::MobEffectInstance> effects;
            for (const auto& e : stew->effects) effects.push_back(e.CreateEffectInstance());
            std::vector<Game::PotionTooltipLine> stewLines;
            Game::AddPotionTooltip(effects, stewLines, 1.0f);
            for (auto& l : stewLines) push(std::move(l.text), l.colorARGB);
        }
        providers(CT::Slot::SuspiciousStewEffects);
        providers(CT::Slot::BlockState);
        providers(CT::Slot::EntityData);
        providers(CT::Slot::SpawnerBlockEntityData);

        // Bundle contents — MC draws a slot grid (BundleTooltip, the
        // tooltip image); listed as "Name xN" lines here, newest first.
        if (auto bundle = stack.get(DC::BUNDLE_CONTENTS); bundle && shows(DC::BUNDLE_CONTENTS)) {
            for (const auto& inner : bundle->items) {
                if (inner.IsEmpty()) continue;
                push(Game::GetItemStackHoverName(inner) + " x" + std::to_string(inner.count), kTooltipGray);
            }
        }

        providers(CT::Slot::CanBreak);
        providers(CT::Slot::CanPlaceOn);

        // ── TooltipFlag.ADVANCED (F3+H) ──────────────────────────────────
        if (advanced) {
            // "Durability: remaining / max" while damaged and DAMAGE shown.
            if (Game::IsDamaged(stack) && shows(DC::DAMAGE)) {
                const int maxDamage = Game::GetMaxDamage(stack);
                push(Translated("item.durability", {std::to_string(maxDamage - Game::GetDamageValue(stack)),
                                                    std::to_string(maxDamage)}),
                     kTooltipWhite);
            }
            // The registry id in dark grey (the namespace is dropped here by
            // request).
            const std::string_view slug = Game::ItemRegistry::Slug(stack.itemId);
            if (!slug.empty()) push(std::string(slug), kTooltipDarkGray);
            // `components.size()` of the patched map: DataComponents.
            // COMMON_ITEM_COMPONENTS and the item_name / item_model every item
            // carries, the prototype's own, and the stack's patch — each
            // counted once, minus what the patch removes.
            static const char* const kCommon[] = {
                "max_stack_size", "lore", "enchantments", "repair_cost", "use_effects", "attribute_modifiers",
                "rarity", "break_sound", "tooltip_display", "attack_animation", "interact_animation",
                "item_name", "item_model" };
            std::vector<const Game::DataComponentTypeBase*> present;
            const auto add = [&present](const Game::DataComponentTypeBase* t) {
                if (t && std::find(present.begin(), present.end(), t) == present.end()) present.push_back(t);
            };
            for (const char* name : kCommon) add(DC::ByName(name));
            const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
            for (const Game::DataComponentTypeBase* t : Game::AllComponentTypes()) {
                if (item.defaultComponents.has(*t) || stack.components.has(*t)) add(t);
            }
            size_t count = 0;
            for (const Game::DataComponentTypeBase* t : present) {
                if (!stack.components.isRemoved(*t)) ++count;
            }
            if (count > 0) push(Translated("item.components", {std::to_string(count)}), kTooltipDarkGray);
        }
    }

    namespace {
        // A rich line as its styled runs: the text of each run, drawn in the
        // run's colour (the line's base colour where it sets none), with
        // bold / underline / strikethrough as the font's formatting codes.
        struct TooltipRun { std::string text; uint32_t color; };

        std::vector<TooltipRun> RichRuns(const Game::Text::Component& component, uint32_t baseColor) {
            std::vector<TooltipRun> runs;
            Game::Text::Visit(component, Game::Text::Style{},
                              [&runs, baseColor](const Game::Text::Style& style, std::string_view text) {
                                  if (text.empty()) return true;
                                  std::string prefix;
                                  if (style.IsBold())          prefix += "\xC2\xA7l";
                                  if (style.IsUnderlined())    prefix += "\xC2\xA7n";
                                  if (style.IsStrikethrough()) prefix += "\xC2\xA7m";
                                  const uint32_t color = style.color ? (0xFF000000u | style.color->rgb) : baseColor;
                                  runs.push_back({prefix + std::string(text), color});
                                  return true;
                              });
            return runs;
        }

        int RunsWidth(GuiGraphics& g, const std::vector<TooltipRun>& runs) {
            int width = 0;
            for (size_t i = 0; i < runs.size(); ++i) {
                const int w = g.GetStringWidth(runs[i].text);
                if (w <= 0) continue;
                width += w + (width > 0 ? 1 : 0);
            }
            return width;
        }
    } // namespace

    void AbstractContainerScreen::RenderTooltip(GuiGraphics& g, const Game::ItemStack& stack,
                                                int mx, int my) {
        if (stack.IsEmpty()) return;
        std::vector<TooltipLine> lines;
        BuildTooltipLines(stack, Platform::g_gameSettings.GetAdvancedItemTooltips(), lines);
        if (lines.empty()) return;
        // MC AbstractContainerScreen.getTooltipFromContainerItem — a screen's
        // own additions (the creative screen's tab names).
        DecorateItemTooltip(stack, lines);

        // Each line's runs (a plain line is one run).
        std::vector<std::vector<TooltipRun>> runs(lines.size());
        int textW = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rich) runs[i] = RichRuns(*lines[i].rich, lines[i].color);
            else runs[i].push_back({lines[i].text, lines[i].color});
            textW = std::max(textW, RunsWidth(g, runs[i]));
        }
        // GuiGraphicsExtractor.tooltip: 10 px a line, -2 overall.
        const int LINE_H = 10;
        const int totalH = static_cast<int>(lines.size()) * LINE_H - 2;

        // DefaultTooltipPositioner: 12 right / 12 up of the mouse, flipped
        // left when it would leave the screen, pulled up off the bottom.
        int x = mx + 12;
        int y = my - 12;
        if (x + textW > g.GuiWidth()) x = std::max(x - 24 - textW, 4);
        if (y + totalH + 3 > g.GuiHeight()) y = g.GuiHeight() - totalH - 3;

        // TooltipRenderUtil.extractTooltipBackground: the style's (or the
        // vanilla) background and frame sprites, nine-sliced around the text
        // with 3 px of padding and a 9 px margin. TOOLTIP_STYLE "ns:path"
        // names tooltip/<path>_background / _frame.
        std::string background = "tooltip/background";
        std::string frame = "tooltip/frame";
        if (auto style = stack.get(Game::DataComponents::TOOLTIP_STYLE); style && !style->empty()) {
            const size_t colon = style->find(':');
            const std::string path = colon == std::string::npos ? *style : style->substr(colon + 1);
            if (g.HasSprite("tooltip/" + path + "_background")) background = "tooltip/" + path + "_background";
            if (g.HasSprite("tooltip/" + path + "_frame"))      frame = "tooltip/" + path + "_frame";
        }
        const int x0 = x - 3 - 9;
        const int y0 = y - 3 - 9;
        const int paddedW = textW + 3 + 3 + 18;
        const int paddedH = totalH + 3 + 3 + 18;
        if (g.HasSprite(background) && g.HasSprite(frame)) {
            g.BlitSprite(background, x0, y0, paddedW, paddedH);
            g.BlitSprite(frame, x0, y0, paddedW, paddedH);
        } else {
            // No sprites (a pack without them): the pre-1.20.5 fill.
            const uint32_t bg     = 0xF0100010;
            const uint32_t border = 0x505000FF;
            g.Fill(x - 3, y - 4,           x + textW + 3, y - 3,           bg);
            g.Fill(x - 3, y + totalH + 3,  x + textW + 3, y + totalH + 4,  bg);
            g.Fill(x - 3, y - 3,           x + textW + 3, y + totalH + 3,  bg);
            g.Fill(x - 4, y - 3,           x - 3,         y + totalH + 3,  bg);
            g.Fill(x + textW + 3, y - 3,   x + textW + 4, y + totalH + 3,  bg);
            g.Fill(x - 3,         y - 3 + 1, x - 3 + 1,     y + totalH + 3 - 1, border);
            g.Fill(x + textW + 2, y - 3 + 1, x + textW + 3, y + totalH + 3 - 1, border);
        }
        g.NextStratum();

        for (size_t i = 0; i < lines.size(); ++i) {
            int runX = x;
            const int lineY = y + static_cast<int>(i) * LINE_H;
            for (const TooltipRun& run : runs[i]) {
                const int w = g.GetStringWidth(run.text);
                g.DrawString(run.text, runX, lineY, run.color, true);
                if (w > 0) runX += w + 1;
            }
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
