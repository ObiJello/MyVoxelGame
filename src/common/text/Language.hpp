// File: src/common/text/Language.hpp
//
// Mirrors net.minecraft.locale.Language — the active translation table that
// TranslatableContents resolves its key against. The engine ships en_us only,
// so this is the en_us table (assets/lang/en_us.json), loaded once on first
// use and immutable afterwards (safe to read from any thread).
//
// Item.cpp and Potions.cpp keep their own narrow subsets for the item names
// they register; this is the general lookup a text component needs, where
// the key can be anything a data pack or a book page names.
#pragma once

#include <string>

namespace Game::Language {

    // MC Language.has(key).
    bool Has(const std::string& key);

    // MC Language.getOrDefault(key, fallback): the translation, else
    // `fallback`.
    std::string GetOrDefault(const std::string& key, const std::string& fallback);

    // MC Language.getOrDefault(key): the translation, else the key itself.
    inline std::string Get(const std::string& key) { return GetOrDefault(key, key); }

} // namespace Game::Language
