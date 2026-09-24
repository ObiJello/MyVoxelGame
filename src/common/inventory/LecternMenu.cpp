// File: src/common/inventory/LecternMenu.cpp
#include "LecternMenu.hpp"

#include "Slot.hpp"
#include "common/world/block/entity/LecternBlockEntity.hpp"

namespace Game {

    // MC LecternBlockEntity.bookAccess — the lectern seen as a one-slot
    // container. Reads come from the block entity; writes through the
    // container are refused (setItem is empty in MC, canPlaceItem false):
    // the book only leaves through the Take Book button, which runs the
    // lectern's own removal (RemoveBook → resetBookState).
    class LecternMenu::BookAccess : public IContainer {
    public:
        explicit BookAccess(LecternBlockEntity* lectern) : m_lectern(lectern) {}

        int GetContainerSize() const override { return LecternMenu::SLOT_COUNT; }
        int GetMaxStackSize() const override { return 1; }
        bool CanPlaceItem(int, const ItemStack&) const override { return false; }

        const ItemStack& GetItem(int index) const override {
            static const ItemStack kEmpty{};
            return (index == 0 && m_lectern) ? m_lectern->GetBook() : kEmpty;
        }
        // The mutable read hands out a copy: nothing may change the lectern's
        // book behind its back (the slot below refuses every click anyway).
        ItemStack& GetItem(int index) override {
            m_scratch = static_cast<const BookAccess&>(*this).GetItem(index);
            return m_scratch;
        }
        void SetItem(int, const ItemStack&) override {}

    private:
        LecternBlockEntity* m_lectern = nullptr;
        ItemStack           m_scratch{};
    };

    namespace {
        // MC's lectern slot sits at (0, 0) and is never drawn; the book can
        // neither be placed nor picked up through it, only viewed.
        class LecternBookSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack&) const override { return false; }
            bool MayPickup() const override { return false; }
            bool IsActive() const override { return false; }
        };
    } // namespace

    LecternMenu::LecternMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_container(std::make_unique<SimpleContainer>(SLOT_COUNT)) {
        AddSlot(std::make_unique<LecternBookSlot>(m_container.get(), 0, 0, 0));
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
    }

    LecternMenu::LecternMenu(Inventory* playerInventory, LecternBlockEntity* lectern)
        : AbstractContainerMenu(playerInventory),
          m_lectern(lectern),
          m_container(std::make_unique<BookAccess>(lectern)) {
        AddSlot(std::make_unique<LecternBookSlot>(m_container.get(), 0, 0, 0));
        // MC LecternBlockEntity.dataAccess: data 0 IS the lectern's page.
        SetOwnedData(std::make_unique<DelegatingContainerData>(
            std::vector<DelegatingContainerData::Entry>{
                {[lectern] { return lectern ? lectern->GetPage() : 0; },
                 [lectern](int v) { if (lectern) lectern->SetPage(v); }},
            }));
    }

    LecternMenu::~LecternMenu() = default;

    bool LecternMenu::ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) {
        if (buttonId >= BUTTON_PAGE_JUMP_RANGE_START) {
            SetData(DATA_PAGE, buttonId - BUTTON_PAGE_JUMP_RANGE_START);
            return true;
        }
        switch (buttonId) {
            case BUTTON_PREV_PAGE:
                SetData(DATA_PAGE, GetData(DATA_PAGE) - 1);
                return true;
            case BUTTON_NEXT_PAGE:
                SetData(DATA_PAGE, GetData(DATA_PAGE) + 1);
                return true;
            case BUTTON_TAKE_BOOK: {
                if (!mayBuild || !m_lectern) return false;
                // lectern.removeItemNoUpdate(0) + setChanged, then
                // player.getInventory().add(book) or player.drop(book).
                ItemStack book = m_lectern->RemoveBook();
                if (book.IsEmpty()) return true;
                const int leftover = getInventory().AddStack(book);
                if (leftover > 0) {
                    book.count = leftover;
                    result.droppedItem = book;
                }
                return true;
            }
            default:
                return false;
        }
    }

} // namespace Game
