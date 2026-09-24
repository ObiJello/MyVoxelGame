// File: src/common/world/tags/DataTags.hpp
//
// The data pack's tag files, indexed the way the F3 screen reads them:
// "which tags does THIS id carry" (MC TypedInstance.tags(), the
// `#minecraft:...` lines under Targeted Block / Fluid / Entity).
//
// The engine has no tag registry — the spawn tags are baked into predicate
// functions — so this walks `data/<ns>/tags/<registry>/**/*.json` once per
// registry, follows nested `#tag` references, and keeps one sorted list of
// tag names per id. Lookup is a hash probe. Reload() drops the cache (a
// resource reload could in principle change the data pack).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Game::DataTags {

    enum class Registry { Block, Fluid, EntityType, Item };

    // Tags carried by `id` ("minecraft:stone" or plain "stone"), each as
    // "#minecraft:mineable/pickaxe", sorted. Empty when the id has none or
    // the data pack is missing.
    const std::vector<std::string>& TagsFor(Registry registry, std::string_view id);

    // Whether a tag file exists for `tag` ("minecraft:logs" or "logs"),
    // so a command can reject a typo instead of matching nothing.
    bool TagExists(Registry registry, std::string_view tag);

    // Forget every loaded registry; the next lookup rescans the data pack.
    void Reload();

} // namespace Game::DataTags
