#!/usr/bin/env python3
"""Bake MC's entity loot tables into C++.

Reads data/minecraft/loot_table/entities/*.json and emits
src/common/world/loot/GeneratedMobLoot.{hpp,cpp}.

Scope, deliberately narrow: the eight implemented mobs' tables only need
`set_count` with a uniform range, `furnace_smelt` (cook the drop when the mob
died on fire), and `enchanted_count_increase` (looting). Anything else in a pool
is reported and skipped rather than silently mistranslated — the same contract
tools/gen_recipes.py uses for recipe types it does not cover.

Regenerate after a Minecraft version bump; see CLAUDE.md.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOOT_DIR = os.path.join(ROOT, "data", "minecraft", "loot_table", "entities")
ITEM_LIST = os.path.join(ROOT, "src", "common", "entity", "GeneratedItemList.hpp")
OUT_DIR = os.path.join(ROOT, "src", "common", "world", "loot")

# Mob slug -> Game::EntityTypeId enumerator, read from the GENERATED entity
# table so this generator never falls behind it. It used to hard-code the eight
# mobs that existed at the time, which quietly dropped every spawn row for
# everything added since.
def load_known_mobs():
    hpp = os.path.join(ROOT, "src", "common", "entity", "GeneratedEntityTypes.hpp")
    known = {}
    for slug in re.findall(r'//\s*"([a-z_]+)"', open(hpp, encoding="utf-8").read()):
        known[slug] = "".join(p.capitalize() for p in slug.split("_"))
    return known


KNOWN_MOBS = load_known_mobs()


def load_item_identifiers():
    """slug -> Items::Identifier, parsed from the generated item list."""
    items = {}
    pattern = re.compile(r'ItemID\s+(\w+)\s+=\s+PURE_ITEM_BASE\s+\+\s+\d+;\s*//\s*"([a-z0-9_]+)"')
    with open(ITEM_LIST, encoding="utf-8") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                items[m.group(2)] = m.group(1)
    return items


def parse_entry(entry, items, warnings, mob):
    """One loot entry -> (identifier, min, max, smeltIdentifier) or None."""
    if entry.get("type") != "minecraft:item":
        warnings.append(f"{mob}: skipped entry type {entry.get('type')}")
        return None

    slug = entry.get("name", "").removeprefix("minecraft:")
    if slug not in items:
        warnings.append(f"{mob}: unknown item {slug}")
        return None

    lo, hi = 1, 1
    smelt = None

    for fn in entry.get("functions", []):
        kind = fn.get("function", "")
        if kind == "minecraft:set_count":
            count = fn.get("count", {})
            if isinstance(count, dict) and count.get("type") == "minecraft:uniform":
                # MC allows a negative minimum (the spider eye rolls -1..1) as
                # a way of expressing "often nothing"; clamp here so consumers
                # never see a negative count.
                lo = max(0, int(count.get("min", 1)))
                hi = max(0, int(count.get("max", 1)))
            elif isinstance(count, (int, float)):
                lo = hi = int(count)
            else:
                warnings.append(f"{mob}: unhandled set_count on {slug}")
        elif kind == "minecraft:furnace_smelt":
            # The cooked form is the raw slug with a conventional prefix. MC
            # resolves this through a smelting recipe; the naming is regular
            # enough for the eight that a lookup table is not warranted.
            for candidate in (f"cooked_{slug}", f"cooked_{slug.removeprefix('raw_')}"):
                if candidate in items:
                    smelt = items[candidate]
                    break
            if smelt is None:
                warnings.append(f"{mob}: no cooked form for {slug}")
        elif kind == "minecraft:enchanted_count_increase":
            # Looting. No enchantments on mob kills yet, so the bonus is zero
            # and the entry is otherwise unaffected.
            pass
        else:
            warnings.append(f"{mob}: skipped function {kind} on {slug}")

    return (items[slug], lo, hi, smelt)


def main():
    if not os.path.isdir(LOOT_DIR):
        print(f"error: {LOOT_DIR} not found", file=sys.stderr)
        return 1

    items = load_item_identifiers()
    warnings = []
    tables = []

    for mob, enum_name in sorted(KNOWN_MOBS.items(), key=lambda kv: kv[1]):
        path = os.path.join(LOOT_DIR, f"{mob}.json")
        if not os.path.exists(path):
            warnings.append(f"{mob}: no loot table file")
            continue

        with open(path, encoding="utf-8") as f:
            data = json.load(f)

        entries = []
        for pool in data.get("pools", []):
            # A pool or entry carrying `conditions` is one of MC's RARE-DROP
            # pools: zombie iron ingots, skeleton bows, and so on, all gated on
            # killed_by_player plus a looting-scaled random chance. Emitting
            # them unconditionally would make every zombie drop an iron ingot.
            # They are skipped wholesale rather than half-modelled.
            if pool.get("conditions"):
                warnings.append(f"{mob}: skipped conditional (rare-drop) pool")
                continue
            for entry in pool.get("entries", []):
                if entry.get("conditions"):
                    warnings.append(f"{mob}: skipped conditional entry")
                    continue
                parsed = parse_entry(entry, items, warnings, mob)
                if parsed:
                    entries.append(parsed)

        tables.append((enum_name, mob, entries))

    os.makedirs(OUT_DIR, exist_ok=True)

    with open(os.path.join(OUT_DIR, "GeneratedMobLoot.hpp"), "w", encoding="utf-8") as f:
        f.write("""// File: src/common/world/loot/GeneratedMobLoot.hpp
// AUTO-GENERATED by tools/gen_mob_loot.py — DO NOT EDIT BY HAND.
//
// Death drops per mob, from data/minecraft/loot_table/entities/*.json.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/Item.hpp"

namespace Game {

    struct MobLootEntry {
        ItemID item;
        int    minCount;
        int    maxCount;
        // The cooked form, dropped instead when the mob died on fire
        // (MC's furnace_smelt loot function). 0 when the item has none.
        ItemID smeltedItem;
    };

    struct MobLootTable {
        EntityTypeId        type;
        const MobLootEntry* entries;
        int                 count;
    };

    extern const MobLootTable kMobLootTables[];
    extern const int          kMobLootTableCount;

    const MobLootTable* FindMobLootTable(EntityTypeId type);

} // namespace Game
""")

    with open(os.path.join(OUT_DIR, "GeneratedMobLoot.cpp"), "w", encoding="utf-8") as f:
        f.write("""// File: src/common/world/loot/GeneratedMobLoot.cpp
// AUTO-GENERATED by tools/gen_mob_loot.py — DO NOT EDIT BY HAND.
#include "common/world/loot/GeneratedMobLoot.hpp"
#include "common/entity/GeneratedItemList.hpp"

namespace Game {

""")
        for enum_name, mob, entries in tables:
            # Mobs with no supported entries get no array at all — a zero-length
            # array is a Clang extension that MSVC rejects (C2466). They point at
            # nullptr with count 0 in the table below instead.
            if not entries:
                continue
            f.write(f"    // {mob}\n")
            f.write(f"    static const MobLootEntry k_{mob}[] = {{\n")
            for item, lo, hi, smelt in entries:
                smelt_expr = f"Items::{smelt}" if smelt else "Items::Air"
                f.write(f"        {{ Items::{item}, {lo}, {hi}, {smelt_expr} }},\n")
            f.write("    };\n\n")

        f.write("    const MobLootTable kMobLootTables[] = {\n")
        for enum_name, mob, entries in tables:
            arr = f"k_{mob}" if entries else "nullptr"
            f.write(f"        {{ EntityTypeId::{enum_name}, {arr}, {len(entries)} }},\n")
        f.write("    };\n\n")
        f.write(f"    const int kMobLootTableCount = {len(tables)};\n\n")
        f.write("""    const MobLootTable* FindMobLootTable(EntityTypeId type) {
        for (int i = 0; i < kMobLootTableCount; ++i) {
            if (kMobLootTables[i].type == type) return &kMobLootTables[i];
        }
        return nullptr;
    }

} // namespace Game
""")

    total = sum(len(e) for _, _, e in tables)
    print(f"Wrote {len(tables)} loot tables, {total} entries")
    for w in warnings:
        print(f"  note: {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
