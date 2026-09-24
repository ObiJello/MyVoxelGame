// File: src/common/entity/BookItems.hpp
//
// The book items — MC WritableBookItem / WrittenBookItem — and the book
// helpers the lectern, the loot functions and the server's edit-book
// handler share.
//
//   writable_book  stacks to 1, default WRITABLE_BOOK_CONTENT EMPTY; using it
//                  opens the edit screen (client side: LocalPlayer.openItemGui).
//   written_book   stacks to 16, glints (ENCHANTMENT_GLINT_OVERRIDE true);
//                  using it resolves the pages and opens the reading screen
//                  (server side: ServerPlayer.openItemGui → OpenBookS2C).
//
// Both uses go through IUsePlayer::OpenItemGui, which each side implements
// the way MC's LocalPlayer / ServerPlayer do.
#pragma once

#include "Item.hpp"

#include <string>
#include <unordered_map>

namespace Game {

    // Wires the two items' defaults and `use` callbacks. Called from
    // ItemRegistry_RegisterBehaviors.
    void ItemRegistry_RegisterBookItems(std::unordered_map<ItemID, Item>& pureItems);

    namespace Books {

        // MC ItemTags.LECTERN_BOOKS: writable_book and written_book.
        bool IsLecternBook(const ItemStack& stack);

        // MC LecternBlockEntity.getPageCount: the written content's pages,
        // else the writable content's, else 0.
        int PageCount(const ItemStack& book);

        // MC WrittenBookContent.resolveForItem: a written book whose pages
        // are not yet resolved has its selector / score / nbt contents
        // resolved for `readerName` (MC's command source is the reader, so
        // "@s" / "@p" name them) and is marked resolved; a page that would
        // grow past PAGE_LENGTH leaves the pages as they were, only marked.
        // Returns true when the pages themselves changed.
        bool ResolveForItem(ItemStack& stack, const std::string& readerName);

        // The written-book title MC shows as the stack's name
        // (ItemStack.getCustomName's WRITTEN_BOOK_CONTENT branch): the raw
        // title when it is not blank, else empty.
        std::string WrittenBookTitle(const ItemStack& stack);

    } // namespace Books

} // namespace Game
