// File: src/common/world/loot/ChestLootTables.hpp
//
// Runtime loot tables for containers — MC's LootTable.fill path, read from
// data/<ns>/loot_table/<path>.json on first use.
//
// Block drops are baked by tools/gen_loot_tables.py into flat rows because
// vanilla block tables never use weights. Container tables (chests/,
// dispensers/, pots/, …) are the opposite shape: weighted entries, uniform
// roll counts, and item functions that need the enchantment registry. They
// are far fewer (57 chest tables) and are only read when a structure chest
// is first opened, so they are parsed with nlohmann::json at runtime and
// cached, the same way DataTags reads the tag files.
//
// Supported (everything the shipped chest tables use):
//   entries    minecraft:item, minecraft:empty, minecraft:loot_table (ref)
//   pools      rolls / bonus_rolls (constant, uniform, binomial providers)
//   conditions minecraft:random_chance; anything else passes with one warning
//   functions  set_count, set_name, enchant_randomly, enchant_with_levels,
//              set_enchantments, set_potion, set_stew_effect,
//              set_written_book_pages, set_book_cover, set_writable_book_pages
//              (every ListOperation mode: replace_all, replace_section,
//              insert, append); the rest
//              (set_damage, exploration_map, …) have no component to write
//              into yet and leave the bare item.
#pragma once

#include "common/inventory/Container.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    class JavaRandom;

    namespace ChestLoot {

        // MC RandomizableContainer.unpackLootTable → LootTable.fill. `key` is
        // the table's resource key ("minecraft:chests/simple_dungeon"). A
        // non-zero `seed` seeds a private java.util.Random exactly as
        // LootContext.Builder.withOptionalRandomSeed does; 0 falls back to
        // `levelRandom` (the level's own source), or to a time-seeded one
        // when the container has no level yet. `luck` is LootContext's luck —
        // the opening player's LUCK attribute (MC LootParams.withLuck), 0
        // when nobody opened it. Returns false when the table does not exist
        // (the container is left untouched).
        bool Fill(IContainer& container, const std::string& key, int64_t seed, JavaRandom* levelRandom,
                  float luck = 0.0f);

        // MC LootTable.getRandomItems (what /loot rolls): the table's stacks,
        // each cut to its max stack size, appended to `out`. False when the
        // table does not exist.
        bool GetRandomItems(const std::string& key, JavaRandom& random, float luck,
                            std::vector<ItemStack>& out);

        bool Exists(const std::string& key);

        // Forget every parsed table; the next Fill re-reads the data pack.
        void Reload();

    } // namespace ChestLoot
} // namespace Game
