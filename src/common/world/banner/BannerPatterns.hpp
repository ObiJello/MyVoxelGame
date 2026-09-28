// File: src/common/world/banner/BannerPatterns.hpp
//
// The BANNER_PATTERN registry as the loom needs it, from the data pack:
// data/<ns>/banner_pattern/<id>.json (asset_id, translation_key) and the
// pattern tags data/<ns>/tags/banner_pattern/*.json — #no_item_required (the
// loom's list with no pattern item) and #pattern_item/<name> (what each
// banner pattern item provides, MC Items.java PROVIDES_BANNER_PATTERNS).
#pragma once

#include <string>
#include <vector>

namespace Game::BannerPatterns {

    // A tag's pattern ids ("#minecraft:no_item_required" or one id), nested
    // tags expanded, file order kept; ids the registry lacks dropped.
    std::vector<std::string> Resolve(const std::string& spec);

    // True when data/<ns>/banner_pattern/<path>.json exists.
    bool Exists(const std::string& id);

    // The texture sheet a pattern draws (asset_id's path, "globe"); the id's
    // own path when the definition is missing.
    std::string AssetOf(const std::string& id);

    // BannerPattern.translationKey ("block.minecraft.banner.globe"); the
    // loom's tooltip appends "." + the dye's name.
    std::string TranslationKeyOf(const std::string& id);

    // PROVIDES_BANNER_PATTERNS of a banner pattern item ("flower_banner_
    // pattern" → "#minecraft:pattern_item/flower"); empty for anything else.
    std::string ProvidedTagOf(const std::string& itemSlug);

} // namespace Game::BannerPatterns
