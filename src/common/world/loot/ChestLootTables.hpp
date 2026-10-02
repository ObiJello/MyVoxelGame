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
//   entries    minecraft:item, minecraft:empty, minecraft:loot_table (a key,
//              an inline table or a list of either — the equipment tables)
//   pools      rolls / bonus_rolls (constant, uniform, binomial providers)
//   conditions minecraft:random_chance, inverted, all_of, any_of,
//              location_check (its biome clause, at ORIGIN),
//              entity_properties on `this` with the fishing_hook
//              type_specific predicate (in_open_water); anything else
//              passes with one warning
//   functions  set_count, set_name (custom_name / item_name target),
//              enchant_randomly, enchant_with_levels, set_enchantments,
//              set_damage, set_potion, set_stew_effect,
//              set_written_book_pages, set_book_cover, set_writable_book_pages
//              (every ListOperation mode: replace_all, replace_section,
//              insert, append), exploration_map (through the server's map
//              system — needs the level and ORIGIN), filtered (the map_id
//              item predicate), discard, set_ominous_bottle_amplifier,
//              set_instrument (the goat horn's INSTRUMENT, from a tag of
//              data/<ns>/instrument definitions) and set_components (for
//              the components this engine carries; an armor trim has none
//              and is skipped).
#pragma once

#include "common/inventory/Container.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    class JavaRandom;

    namespace ChestLoot {

        // The LootContext parameters a roll may carry beyond its random:
        // the level (as a dimension id — the server resolves it) and
        // LootContextParams.ORIGIN. exploration_map and location_check read
        // them; a roll without an origin (/loot) leaves those alone.
        struct LootLevelContext {
            int dimensionId = 0;
            std::optional<glm::dvec3> origin;
            // LootContextParams.THIS_ENTITY when it is a fishing hook (the
            // FISHING parameter set): its isOpenWaterFishing, which the
            // `minecraft:fishing_hook` entity sub-predicate (in_open_water)
            // tests. Absent = no hook, and that predicate fails.
            std::optional<bool> fishingHookInOpenWater;
        };

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
                  float luck = 0.0f, const LootLevelContext* level = nullptr);

        // MC LootTable.getRandomItems (what /loot rolls): the table's stacks,
        // each cut to its max stack size, appended to `out`. False when the
        // table does not exist.
        bool GetRandomItems(const std::string& key, JavaRandom& random, float luck,
                            std::vector<ItemStack>& out, const LootLevelContext* level = nullptr);

        bool Exists(const std::string& key);

        // MC LootItemFunction.apply for an item modifier (`/item modify`,
        // `/item replace … from … <modifier>`): `modifier` is one function
        // object or a list of them (data/<ns>/item_modifier/*.json, or the
        // inline SNBT a command carries, as JSON). Runs each in order with
        // the same functions the container tables support; false, with
        // `error`, when one names a function this engine does not model.
        bool ApplyItemModifier(const nlohmann::json& modifier, ItemStack& stack, JavaRandom& random,
                               std::string& error, const LootLevelContext* level = nullptr);

        // Forget every parsed table; the next Fill re-reads the data pack.
        void Reload();

    } // namespace ChestLoot
} // namespace Game
