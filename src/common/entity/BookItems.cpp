// File: src/common/entity/BookItems.cpp
//
// MC WritableBookItem / WrittenBookItem, the LECTERN_BOOKS tag and
// WrittenBookContent.resolveForItem. See BookItems.hpp.
#include "BookItems.hpp"

#include "GeneratedItemList.hpp"
#include "IUsePlayer.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Game {

    namespace {

        // WritableBookItem.use / WrittenBookItem.use: player.openItemGui, then
        // SUCCESS on both sides. What opens is the side's own business
        // (IUsePlayer::OpenItemGui).
        UseResult Use_Book(ILevelWrite* /*world*/, IUsePlayer* player, uint32_t hand, ItemStack& stack) {
            if (!player) return UseResult::Pass;
            player->OpenItemGui(stack, hand);
            return UseResult::Success;
        }

        // ComponentUtils.resolve for one page, with WrittenBookContent
        // .resolvePage's size gate: a page whose JSON would exceed
        // PAGE_LENGTH does not resolve.
        std::optional<Text::Component> ResolvePage(const Text::Component& page, const std::string& readerName) {
            // The command source is the reader (ServerPlayer.createCommandSourceStack)
            // or the lectern; "@s" / "@p" name it. Other selectors would need
            // an entity search this layer does not have and resolve to nothing.
            const auto selectorName = [&readerName](const std::string& selector) -> std::optional<std::string> {
                if (selector == "@s" || selector == "@p") return readerName;
                return std::string();
            };
            Text::Component resolved = Text::Resolve(page, selectorName);
            if (Text::EncodesLongerThan(resolved, WrittenBookContent::PAGE_LENGTH)) return std::nullopt;
            return resolved;
        }

    } // namespace

    void ItemRegistry_RegisterBookItems(std::unordered_map<ItemID, Item>& pureItems) {
        // Items.java:3020 WRITABLE_BOOK — stacksTo(1),
        // WRITABLE_BOOK_CONTENT = WritableBookContent.EMPTY.
        if (auto it = pureItems.find(Items::WritableBook); it != pureItems.end()) {
            it->second.maxStackSize = 1;
            it->second.defaultComponents.set(DataComponents::WRITABLE_BOOK_CONTENT, WritableBookContent{});
            it->second.use = &Use_Book;
        }
        // Items.java:3021 WRITTEN_BOOK — stacksTo(16),
        // ENCHANTMENT_GLINT_OVERRIDE = true. Its content is per stack only.
        if (auto it = pureItems.find(Items::WrittenBook); it != pureItems.end()) {
            it->second.maxStackSize = 16;
            it->second.defaultComponents.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, true);
            it->second.use = &Use_Book;
        }
    }

    namespace Books {

        bool IsLecternBook(const ItemStack& stack) {
            return stack.itemId == Items::WritableBook || stack.itemId == Items::WrittenBook;
        }

        int PageCount(const ItemStack& book) {
            if (auto written = book.get(DataComponents::WRITTEN_BOOK_CONTENT)) {
                return static_cast<int>(written->pages.size());
            }
            if (auto writable = book.get(DataComponents::WRITABLE_BOOK_CONTENT)) {
                return static_cast<int>(writable->pages.size());
            }
            return 0;
        }

        bool ResolveForItem(ItemStack& stack, const std::string& readerName) {
            auto content = stack.get(DataComponents::WRITTEN_BOOK_CONTENT);
            if (!content || content->resolved) return false;

            // WrittenBookContent.resolve: every page (raw and, when present,
            // filtered) or nothing.
            std::vector<Filterable<Text::Component>> pages;
            pages.reserve(content->pages.size());
            bool complete = true;
            for (const auto& page : content->pages) {
                std::optional<Text::Component> raw = ResolvePage(page.raw, readerName);
                if (!raw) { complete = false; break; }
                Filterable<Text::Component> resolved;
                resolved.raw = std::move(*raw);
                if (page.filtered) {
                    std::optional<Text::Component> filtered = ResolvePage(*page.filtered, readerName);
                    if (!filtered) { complete = false; break; }
                    resolved.filtered = std::move(*filtered);
                }
                pages.push_back(std::move(resolved));
            }

            if (!complete) {
                // resolveForItem: an unresolvable book is only marked.
                stack.components.set(DataComponents::WRITTEN_BOOK_CONTENT, content->MarkResolved());
                return false;
            }
            WrittenBookContent resolved = *content;
            resolved.pages = std::move(pages);
            resolved.resolved = true;
            stack.components.set(DataComponents::WRITTEN_BOOK_CONTENT, std::move(resolved));
            return true;
        }

        std::string WrittenBookTitle(const ItemStack& stack) {
            auto content = stack.get(DataComponents::WRITTEN_BOOK_CONTENT);
            if (!content) return {};
            // StringUtil.isBlank: empty or only whitespace.
            const std::string& title = content->title.raw;
            for (unsigned char ch : title) {
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') return title;
            }
            return {};
        }

    } // namespace Books

} // namespace Game
