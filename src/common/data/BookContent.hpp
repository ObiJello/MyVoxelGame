// File: src/common/data/BookContent.hpp
//
// The two book components — MC world/item/component/WrittenBookContent,
// WritableBookContent and server/network/Filterable.
//
// A Filterable<T> is a value plus an optional "filtered" variant (MC's chat
// filter output); a reader with text filtering on sees the filtered one when
// present. The engine has no text filter, so every value it creates is
// pass-through, but the field is carried so books from real worlds round-trip.
#pragma once

#include "common/text/TextComponent.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Game {

    // MC server/network/Filterable.
    template <typename T>
    struct Filterable {
        T                raw{};
        std::optional<T> filtered;

        static Filterable PassThrough(T value) { return Filterable{std::move(value), std::nullopt}; }

        // MC Filterable.get(filterEnabled).
        const T& Get(bool filterEnabled) const {
            return (filterEnabled && filtered) ? *filtered : raw;
        }

        bool operator==(const Filterable& o) const { return raw == o.raw && filtered == o.filtered; }
        bool operator!=(const Filterable& o) const { return !(*this == o); }
    };

    // MC WrittenBookContent — a signed book: title, author, generation
    // (0 original … 3 tattered), pages as text components, and whether the
    // pages' selectors / scores have been resolved yet.
    struct WrittenBookContent {
        static constexpr int PAGE_LENGTH      = 32767;   // JSON length of one page
        static constexpr int TITLE_LENGTH     = 16;      // what the sign screen lets you type (+1)
        static constexpr int TITLE_MAX_LENGTH = 32;      // what the codec accepts
        static constexpr int MAX_GENERATION   = 3;

        Filterable<std::string>                  title;
        std::string                              author;
        int                                      generation = 0;
        std::vector<Filterable<Text::Component>> pages;
        bool                                     resolved = false;

        // MC WrittenBookContent.EMPTY: blank title and author, no pages,
        // resolved (there is nothing to resolve).
        static WrittenBookContent Empty() {
            WrittenBookContent c;
            c.resolved = true;
            return c;
        }

        // MC getPages(filterEnabled).
        std::vector<Text::Component> GetPages(bool filterEnabled) const {
            std::vector<Text::Component> out;
            out.reserve(pages.size());
            for (const auto& p : pages) out.push_back(p.Get(filterEnabled));
            return out;
        }

        // MC craftCopy: the book-cloning recipe's output, one generation on.
        WrittenBookContent CraftCopy() const {
            WrittenBookContent c = *this;
            c.generation = generation + 1;
            return c;
        }

        // MC markResolved.
        WrittenBookContent MarkResolved() const {
            WrittenBookContent c = *this;
            c.resolved = true;
            return c;
        }

        // MC withReplacedPages: new pages are unresolved.
        WrittenBookContent WithReplacedPages(std::vector<Filterable<Text::Component>> newPages) const {
            WrittenBookContent c = *this;
            c.pages = std::move(newPages);
            c.resolved = false;
            return c;
        }

        bool operator==(const WrittenBookContent& o) const {
            return title == o.title && author == o.author && generation == o.generation &&
                   pages == o.pages && resolved == o.resolved;
        }
    };

    // MC WritableBookContent — a book and quill's pages, plain strings.
    struct WritableBookContent {
        static constexpr int PAGE_EDIT_LENGTH = 1024;
        static constexpr int MAX_PAGES        = 100;

        std::vector<Filterable<std::string>> pages;

        // MC getPages(filterEnabled).
        std::vector<std::string> GetPages(bool filterEnabled) const {
            std::vector<std::string> out;
            out.reserve(pages.size());
            for (const auto& p : pages) out.push_back(p.Get(filterEnabled));
            return out;
        }

        bool operator==(const WritableBookContent& o) const { return pages == o.pages; }
    };

} // namespace Game
