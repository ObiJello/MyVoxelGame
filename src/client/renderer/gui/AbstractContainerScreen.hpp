// File: src/client/renderer/gui/AbstractContainerScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.AbstractContainerScreen —
// everything a menu-backed screen does that does NOT depend on which menu it is
// showing: slot hit-testing and drawing, the click/drag gestures, the carried
// stack, tooltips, and the predicted-click queue that talks to the server.
//
// Two screens derive from it, exactly as in MC:
//   • InventoryScreen             — survival. textures/gui/container/inventory.png,
//                                   armor column, 2x2 crafting grid, player preview.
//   • CreativeModeInventoryScreen — creative. The tabbed item picker.
// Which one opens is decided by the player's game mode; see InventoryScreen.hpp.
//
// ── Why the menu is a singleton ──────────────────────────────────────────────
// MC has ONE `player.inventoryMenu` and hands it to whichever screen is showing.
// The cursor stack, the container id and the sync revision all live on it, so a
// screen swap (survival ⇄ creative on a /gamemode change) keeps them. We do the
// same rather than giving each screen its own menu, which would have let the two
// disagree about what is on the cursor.
#pragma once

#include "common/entity/Inventory.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/inventory/MenuType.hpp"
#include "common/network/PacketTypes.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Game { class ClientPlayer; }

namespace Render {

    class GuiGraphics;

    // MC: `player.inventoryMenu`. Null until SetInventoryScreenPlayer runs
    // (a menu's slots bind to their container at construction, so it cannot
    // exist before the player does).
    Game::InventoryMenu* PlayerInventoryMenu();

    // MC: `player.containerMenu` — the menu on top. The block container while
    // one is open, otherwise the player's own. Every container screen reads
    // this, and so does every inbound slot packet, so the two can never end up
    // indexing different slot lists.
    Game::AbstractContainerMenu* PlayerContainerMenu();

    // (Re)builds the shared menu over `player`'s inventory. Recreated rather
    // than re-pointed because a menu's slots bind to their container at
    // construction — MC builds a fresh menu per player, too.
    void RebuildPlayerInventoryMenu(Game::ClientPlayer* player);

    // Binds the local player: rebuilds the shared menu and hands the pointer to
    // every container screen. This is the entry point PlatformMain calls.
    void SetInventoryScreenPlayer(Game::ClientPlayer* player);

    // Put `menu` on top, or pass null to fall back to the player's own. The
    // cursor stack rides across, mirroring MC's transferState. Callers that
    // also need the right SCREEN should use OpenClientContainerScreen below.
    void SetClientContainerMenu(std::unique_ptr<Game::AbstractContainerMenu> menu,
                                Game::MenuType type);
    // Which menu the client currently has on top.
    Game::MenuType ClientContainerMenuType();

    // Apply one server-authoritative MENU slot / a whole snapshot's slots.
    // Routed through the menu rather than written into the inventory by index —
    // menu slot 10 of a crafting table is inventory slot 9, and menu slots 0..9
    // are the table's own grid, which the inventory has no room for at all.
    void ApplyContainerSlot(int menuIndex, const Game::ItemStack& stack);
    void ApplyContainerSlots(const std::vector<Game::ItemStack>& slots);

    // Apply one ContainerData index (MC ClientboundContainerSetDataPacket).
    // Dropped when `containerId` names a menu we already replaced — a furnace
    // sends these every tick, so one in flight across a close is normal.
    void ApplyContainerData(uint32_t containerId, uint16_t index, int32_t value);

    // ── Entry points for the network layer (declared again at file scope in
    // ClientPacketHandler.cpp so it needn't include a GUI header) ───────────

    // Server said "I opened menu <containerId> of type <type> for you"
    // (OpenScreenS2C): build the matching menu and put its screen up. The
    // contents arrive right behind in a full snapshot.
    void OpenClientContainerScreen(Game::MenuType type, uint32_t containerId,
                                   const std::string& title);
    // A full container snapshot. `menuType` is authoritative: a snapshot for
    // the plain inventory menu while a table screen is up means the server has
    // closed it, so the screen comes down here.
    void ApplyContainerFullSync(Game::MenuType menuType, uint32_t containerId,
                                const std::vector<Game::ItemStack>& slots);

    class AbstractContainerScreen {
    public:
        virtual ~AbstractContainerScreen() = default;

        // MC pixel constants shared by every container screen.
        static constexpr int SLOT_SIZE       = 16;
        static constexpr int SLOT_STEP       = 18;
        static constexpr int DOUBLE_CLICK_MS = 250;

        // Hit-test results. Real menu slot indices are >= 0; everything else is
        // a negative sentinel. Subclasses define their own starting at -20.
        static constexpr int HIT_NONE    = -1;
        static constexpr int HIT_OUTSIDE = -10;

        void Open();
        void Close();
        bool IsOpen() const { return m_open; }

        void SetPlayer(Game::ClientPlayer* p) { m_player = p; }

        // ── Per-frame input ──────────────────────────────────────────────
        void OnCharInput(unsigned int codepoint);
        bool OnKeyDown(int glfwKey, int glfwMods);                // true if consumed
        void OnMouseButton(int glfwButton, int action, int mods); // GLFW_PRESS/RELEASE
        void OnMouseMove(double mouseX, double mouseY, int windowW, int windowH,
                         int guiW, int guiH);
        void OnScroll(double dy);
        void Update(float deltaTime);
        void Render(GuiGraphics& graphics);

        // Drain queued clicks (PlatformMain forwards them to the server).
        bool ConsumePendingClick(Network::InventoryClickC2SPacket& out);

        // True once, on the first input frame after this screen opened.
        //
        // The host drives mouse buttons by diffing the polled GLFW state
        // against latches it keeps across frames, and those latches are stale
        // while no screen is up. A screen the SERVER opens therefore appears
        // with the button that opened it still physically down — the
        // right-click on a crafting table — and the very next diff reports it
        // as a fresh press onto whatever slot the cursor happens to be over.
        // Consuming this tells the host to adopt the live button state without
        // dispatching it. MC never sees that press either: the click was
        // consumed by the world before the screen existed.
        bool ConsumeFreshlyOpened() {
            const bool wasFresh = m_freshlyOpened;
            m_freshlyOpened = false;
            return wasFresh;
        }

        // Swap screens without telling the server the container closed — used
        // when the game mode changes while the inventory is open, so the cursor
        // stack survives the swap (MC's setScreen does not close the menu).
        void OpenSilently();
        void CloseSilently();

    protected:
        // ── Panel geometry (MC imageWidth / imageHeight) ─────────────────
        virtual int ImageWidth()  const = 0;
        virtual int ImageHeight() const = 0;
        int LeftPos(int guiW) const { return (guiW - ImageWidth())  / 2; }
        int TopPos (int guiH) const { return (guiH - ImageHeight()) / 2; }

        // ── Draw hooks, in draw order ────────────────────────────────────
        // Behind the panel (creative's unselected tabs tuck under its top edge).
        virtual void RenderBehindBg(GuiGraphics&, int /*leftPos*/, int /*topPos*/) {}
        // MC renderBg — the panel texture and anything painted onto it.
        virtual void RenderBg(GuiGraphics& g, int leftPos, int topPos) = 0;
        // Non-menu item cells drawn in the same layer as real slots (the
        // creative search grid).
        virtual void RenderExtraSlots(GuiGraphics&, int /*leftPos*/, int /*topPos*/) {}
        // MC renderLabels — panel-relative text.
        virtual void RenderLabels(GuiGraphics&, int /*leftPos*/, int /*topPos*/) {}
        // Widgets drawn above the slots (search box, scrollbar, selected tab).
        virtual void RenderExtras(GuiGraphics&, int /*leftPos*/, int /*topPos*/) {}
        // Highlight for a hovered non-slot cell (creative grid / trash).
        virtual void RenderExtraHoverHighlight(GuiGraphics&, int /*leftPos*/, int /*topPos*/) {}

        // ── Slots ────────────────────────────────────────────────────────
        // Panel-relative position of a menu slot, or false when this screen
        // does not show it. The default reads Slot::x/y and honours
        // Slot::IsActive; CreativeModeInventoryScreen overrides it wholesale
        // because its panel puts the same slots somewhere else entirely.
        virtual bool GetSlotPos(int menuIndex, int& outX, int& outY) const;
        // MC Slot.getNoItemIcon — placeholder sprite for an empty slot, or
        // nullptr. Defaults to the armor/shield silhouettes.
        virtual const char* GetNoItemIcon(int menuIndex) const;

        // ── Screen-specific interaction ──────────────────────────────────
        // Zones this screen owns, tested BEFORE the panel-bounds check so a
        // subclass can claim something drawn outside the panel (creative tabs).
        // Return HIT_NONE to fall through.
        virtual int  HitTestExtras(int /*lx*/, int /*ly*/) { return HIT_NONE; }
        // Called for every press before the generic slot handling. Return true
        // when the click was consumed.
        virtual bool HandleExtraClick(int /*hit*/, int /*glfwButton*/, bool /*shift*/) { return false; }
        virtual void HandleExtraRelease() {}
        // First refusal on keys (after ESC) and on typed characters.
        virtual bool HandleExtraKey(int /*glfwKey*/, int /*glfwMods*/) { return false; }
        virtual bool HandleExtraCharInput(unsigned int /*codepoint*/) { return false; }
        virtual bool HandleExtraScroll(double /*dy*/) { return false; }
        virtual void OnExtraMouseMove(int /*leftPos*/, int /*topPos*/) {}
        // Stack whose tooltip to show when the hover is not a real slot.
        virtual const Game::ItemStack* HoveredExtraStack() const { return nullptr; }

        virtual void OnOpen()  {}
        virtual void OnClose() {}
        // MC AbstractContainerScreen.containerTick — runs each frame while open.
        virtual void ContainerTick() {}

        // ── Shared state for subclasses ──────────────────────────────────
        Game::AbstractContainerMenu* Menu() const { return PlayerContainerMenu(); }
        Game::InventorySlot&       Carried();
        const Game::InventorySlot& Carried() const;

        Game::ClientPlayer* Player() const { return m_player; }
        int       HoveredSlot() const { return m_hoveredSlot; }
        glm::vec2 MouseGui()    const { return m_mouseGui; }

        // Queue one click: predicts locally, then hands the packet to
        // PlatformMain. `creativeStack` (when non-null) rides along so the
        // full creative-source stack — components and all — reaches the server.
        void QueueClick(Network::ContainerInput action, int16_t slotIndex,
                        uint8_t button, Game::ItemID creativeItem = Game::Items::Air,
                        const Game::ItemStack* creativeStack = nullptr);

        // Shared drawing helpers.
        void RenderHoverHighlight(GuiGraphics& g, int x, int y, bool front);
        void RenderTooltip(GuiGraphics& g, const Game::ItemStack& stack, int mx, int my);

    private:
        // ── Rendering passes ─────────────────────────────────────────────
        void RenderSlots(GuiGraphics& g, int leftPos, int topPos);
        void RenderSlotHighlight(GuiGraphics& g, int leftPos, int topPos, bool front);
        void RenderCarriedItem(GuiGraphics& g);

        int  HitTest(int leftPos, int topPos);

        // ── Drag-distribute preview ──────────────────────────────────────
        // The server only commits a drag at QUICK_CRAFT END (mouse up), so
        // until then the screen shows what the commit WILL produce rather than
        // raw server state — mirroring MC's AbstractContainerScreen.renderSlot.
        int                 DragPerSlotCount() const;
        int                 DragRemainingCarriedCount() const;
        Game::InventorySlot DisplayedSlot(int slotIndex,
                                          const Game::InventorySlot& base) const;

        bool                m_open = false;
        bool                m_freshlyOpened = false;   // see ConsumeFreshlyOpened
        Game::ClientPlayer* m_player = nullptr;
        int                 m_hoveredSlot = HIT_NONE;
        glm::vec2           m_mouseGui{0.0f, 0.0f};

        // Drag (QUICK_CRAFT)
        bool                 m_isDragging = false;
        uint8_t              m_dragType = 0;   // 0=split, 1=one-each, 2=clone
        std::vector<uint8_t> m_dragSlots;
        // Cursor count when the drag began — the preview subtracts what has
        // been distributed so far so the held stack visibly shrinks.
        int                  m_dragStartCarriedCount = 0;

        // Double-click (PICKUP_ALL)
        long long m_lastClickTimeMs = 0;
        int       m_lastClickedSlot = HIT_NONE;

        // Pending packets, drained by PlatformMain.
        std::vector<Network::InventoryClickC2SPacket> m_pendingClicks;
    };

} // namespace Render
