// File: src/common/world/block/entity/LecternBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.LecternBlockEntity — the
// book lying on a lectern and the page it is open at.
//
//   book       the stack (one writable or written book); EMPTY when bare
//   page       the open page, 0-based (MC "Page")
//   pageCount  the book's page count, cached from its content component
//
// The page is shared: every reader's LecternMenu publishes it as data slot 0,
// so a page turned by one player turns for everyone (LecternMenu.setData →
// broadcastChanges). Each change pulses the lectern's redstone output for two
// ticks (LecternBlock.signalPageChange) and moves its comparator reading
// (getRedstoneSignal: page progress through the book, 1..15).
//
// Wire: nothing. MC's LecternBlockEntity sends no update packet — the client
// draws the book on the lectern from the HAS_BOOK blockstate and only sees the
// book's content through the menu when it opens it. Save/Load stay the base's
// no-ops for the same reason; disk persistence is the Anvil writer's (Book +
// Page, BlockEntityNbt.cpp).
#pragma once

#include "BlockEntity.hpp"
#include "common/entity/Item.hpp"

#include <string>

namespace Game {

    class LecternBlockEntity : public BlockEntity {
    public:
        // MC LecternBlockEntity.DATA_PAGE / NUM_DATA / SLOT_BOOK / NUM_SLOTS.
        static constexpr int DATA_PAGE = 0;
        static constexpr int NUM_DATA  = 1;
        static constexpr int SLOT_BOOK = 0;
        static constexpr int NUM_SLOTS = 1;

        LecternBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        // MC getBook / hasBook: a book is "had" when the stack carries either
        // book content component (a written book without content does not
        // count, exactly as in MC).
        const ItemStack& GetBook() const { return m_book; }
        bool HasBook() const;

        // MC setBook(book, player): resolves a written book's pages for
        // `readerName` (the placing player; "Lectern" when none), opens it at
        // page 0 and marks the entity changed. Does NOT touch the blockstate —
        // LecternBlock.placeBook / resetBookState does that.
        void SetBook(ItemStack book, const std::string* readerName = nullptr);

        // MC loadAdditional: the book (resolved with no player) and the page,
        // clamped exactly as Mth.clamp(page, 0, pageCount - 1) — which is -1
        // for a book with no pages, as in vanilla.
        void LoadFromNbt(ItemStack book, int page);

        // MC getPage / setPage. SetPage clamps to the book and, when the page
        // actually moves, marks the entity changed and pulses the lectern
        // (LecternBlock.signalPageChange).
        int  GetPage() const { return m_page; }
        void SetPage(int page);

        int PageCount() const { return m_pageCount; }

        // MC getRedstoneSignal — the comparator reading: how far through the
        // book the open page is, 0..14, plus one while a book is present.
        int GetRedstoneSignal() const;

        // MC bookAccess.removeItemNoUpdate(0): takes the book off (the Take
        // Book button), resets page and count, and puts the lectern back to
        // HAS_BOOK=false (onBookItemRemove → LecternBlock.resetBookState).
        ItemStack RemoveBook();

        // MC preRemoveSideEffects: a lectern broken with a book on it drops
        // the book a quarter-block toward its front, one block up.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

    private:
        // MC BlockEntity.setChanged → Level.blockEntityChanged: mark for
        // saving and let the comparators reading this lectern recompute.
        void SetChanged();

        ItemStack m_book{};
        int       m_page      = 0;
        int       m_pageCount = 0;
    };

} // namespace Game
