// File: src/common/inventory/LecternMenu.hpp
//
// Mirrors net.minecraft.world.inventory.LecternMenu — what a lectern's
// reading screen is backed by. ONE slot (the book, read straight from the
// lectern) and ONE data slot (the open page), and no player inventory at all:
// the screen is a book view, not a container grid.
//
// The page is shared state. Turning it is a menu BUTTON (1 previous, 2 next,
// 100 + n "jump to page n" for a change_page click), sent by the client and
// applied here on the server, which writes the lectern's page through the
// data slot; the per-tick data diff then shows the new page to every player
// reading the same lectern. Button 3 takes the book.
//
// Two constructors, as for every menu here: the server's reads through to the
// LecternBlockEntity, the client's owns a scratch slot and data array that
// the server's syncs fill.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "ContainerData.hpp"
#include "SimpleContainer.hpp"

#include <memory>

namespace Game {

    class LecternBlockEntity;

    class LecternMenu : public AbstractContainerMenu {
    public:
        // MC LecternMenu button ids.
        static constexpr int BUTTON_PREV_PAGE             = 1;
        static constexpr int BUTTON_NEXT_PAGE             = 2;
        static constexpr int BUTTON_TAKE_BOOK             = 3;
        static constexpr int BUTTON_PAGE_JUMP_RANGE_START = 100;

        static constexpr int SLOT_BOOK  = 0;
        static constexpr int SLOT_COUNT = 1;
        static constexpr int DATA_PAGE  = 0;
        static constexpr int DATA_COUNT = 1;

        // Client: a scratch book slot and page, filled by the server's syncs.
        explicit LecternMenu(Inventory* playerInventory);
        // Server: the lectern's own book and page.
        LecternMenu(Inventory* playerInventory, LecternBlockEntity* lectern);
        ~LecternMenu() override;

        // MC getBook / getPage — what the lectern screen shows.
        const ItemStack& GetBook() const { return GetSlot(SLOT_BOOK).GetItem(); }
        int GetPage() const { return GetData(DATA_PAGE); }

        // The lectern this menu reads (server only; null on the client).
        LecternBlockEntity* Lectern() const { return m_lectern; }

        // MC LecternMenu.clickMenuButton.
        bool ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) override;

        // MC quickMoveStack returns EMPTY: there is nowhere to shift-click to.
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override {
            (void)slotIndex; (void)result;
        }
        // No player-inventory slots in this menu.
        int MenuIndexForInventorySlot(int inventoryIndex) const override {
            (void)inventoryIndex;
            return -1;
        }

    private:
        class BookAccess;

        LecternBlockEntity*              m_lectern = nullptr;   // server only
        std::unique_ptr<IContainer>      m_container;           // BookAccess (server) / SimpleContainer (client)
    };

} // namespace Game
