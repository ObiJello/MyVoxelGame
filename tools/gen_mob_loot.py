#!/usr/bin/env python3
"""Bake MC's entity loot tables into C++.

Reads data/minecraft/loot_table/entities/*.json and emits
src/common/world/loot/GeneratedMobLoot.{hpp,cpp}.

The emitted model is MC's real pool structure, evaluated by
MobManager::DropLoot with MC's algorithm (LootPool.addRandomItems):

    pool  = conditions + rolls (constant or uniform range) + entries[]
    roll  = ONE weighted pick among the entries whose conditions pass
            (a `minecraft:empty` entry contributes weight and drops nothing)

Flattening this — dropping every entry every kill — is what used to make a
guardian drop cod AND prismarine crystals (MC: one weighted 2/2/1 pick) and
the elder guardian's armor-trim template drop 100% (MC: weight 1 vs empty 4).

Conditions supported at runtime:
  * minecraft:killed_by_player          — rides the mob's 100-tick player-kill
                                          credit window (MC LivingEntity
                                          lastHurtByPlayerMemoryTime)
  * minecraft:random_chance             — plain probability
  * minecraft:random_chance_with_enchanted_bonus
                                        — emitted at its LOOTING-0 value
                                          (unenchanted_chance); no enchanted
                                          weapons on mob kills yet
  * minecraft:entity_properties {this, type_specific slime size}
                                        — the slime/magma-cube size gates
                                          (slimeball only from size 1, magma
                                          cream only from size >= 2)

Conditions resolved at GENERATION time, each one reported:
  * damage_source_properties {source_entity frog} — FALSE: the killing damage
    source is not tracked at drop time and frog variants do not exist, so the
    frog-kill branches (exactly-1 slimeball, froglights) are dropped and the
    non-frog branch applies to every kill.  minecraft:inverted over one of
    these resolves TRUE.

Everything else in a pool is reported and skipped rather than silently
mistranslated — the same contract tools/gen_recipes.py uses for recipe types
it does not cover.  A skipped ENTRY inside a multi-entry pool distorts the
surviving weights, so those get a louder warning.

Negative set_count minimums (the wither skeleton's uniform(-1,1) coal, the
magma cube's uniform(-2,1) cream) are KEPT: MC expresses "often nothing" this
way, and the consumer clamps the SAMPLED value, not the range — clamping the
range at generation inflated those drop rates by up to 50%.

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

# Sentinel meaning "no upper bound" for a slime-size gate. Slime sizes are
# tiny ints (1/2/4); anything >= this is effectively unbounded.
SIZE_MAX_OPEN = 127

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


# ── Condition compiler ──────────────────────────────────────────────────────
#
# A condition list compiles to one of:
#   ("gates", dict)   — runtime gates the emitted row carries
#   ("false", why)    — statically never true: the pool/entry is omitted, and
#                       omission is EXACT (MC excludes failing entries from
#                       the weighted pick, so they never consumed weight)
#   ("unsupported", why)

def is_frog_damage_source(cond):
    src = cond.get("predicate", {}).get("source_entity", {})
    return src.get("type") == "minecraft:frog"


def compile_one_condition(cond, gates):
    kind = cond.get("condition", "")

    if kind == "minecraft:killed_by_player":
        gates["player_kill"] = True
        return None

    if kind == "minecraft:random_chance":
        gates["chance"] *= float(cond.get("chance", 1.0))
        return None

    if kind == "minecraft:random_chance_with_enchanted_bonus":
        # Looting is not implemented; the looting-0 probability is exactly
        # `unenchanted_chance` (MC EnchantedCountIncreaseFunction's sibling).
        gates["chance"] *= float(cond.get("unenchanted_chance", 1.0))
        return None

    if kind == "minecraft:entity_properties":
        pred = cond.get("predicate", {})
        ts = pred.get("type_specific", {})
        if cond.get("entity") == "this" and ts.get("type") == "minecraft:slime":
            size = ts.get("size")
            if isinstance(size, (int, float)):
                gates["size_min"] = int(size)
                gates["size_max"] = int(size)
                return None
            if isinstance(size, dict):
                gates["size_min"] = int(size.get("min", 1))
                gates["size_max"] = int(size.get("max", SIZE_MAX_OPEN))
                return None
        return ("unsupported", f"entity_properties {json.dumps(pred, sort_keys=True)[:80]}")

    if kind == "minecraft:damage_source_properties":
        if is_frog_damage_source(cond):
            # The killing damage source is not tracked at drop time and frog
            # variants (froglight color) are not modelled.
            return ("false", "frog-kill branch (killing damage source not tracked)")
        return ("unsupported",
                f"damage_source_properties {json.dumps(cond.get('predicate', {}), sort_keys=True)[:80]}")

    if kind == "minecraft:inverted":
        term = cond.get("term", {})
        sub = compile_one_condition(term, dict(gates, chance=1.0))
        if sub is None:
            return ("unsupported", "inverted over a runtime condition")
        verdict, why = sub
        if verdict == "false":
            return None  # inverted(false) == always true: no gate
        return ("unsupported", f"inverted({why})")

    return ("unsupported", kind or "<missing condition key>")


def compile_conditions(conds):
    gates = {"player_kill": False, "chance": 1.0,
             "size_min": 0, "size_max": SIZE_MAX_OPEN}
    for cond in conds or []:
        result = compile_one_condition(cond, gates)
        if result is not None:
            return result
    return ("gates", gates)


# ── Entry / function parsing ────────────────────────────────────────────────

# The standard shape of furnace_smelt's own condition list: any_of(this
# is_on_fire, attacker holds a #smelts_loot weapon). The on-fire half is what
# the runtime implements; the enchantment half does not exist here. Recognised
# so it does not spam the report.
def is_standard_smelt_conditions(conds):
    if not conds:
        return True
    if len(conds) != 1 or conds[0].get("condition") != "minecraft:any_of":
        return False
    for term in conds[0].get("terms", []):
        if term.get("condition") != "minecraft:entity_properties":
            return False
    return True


def parse_entry(entry, items, warnings, mob, pool_idx):
    """One loot entry -> row dict, or None (skipped, reported by caller)."""
    where = f"{mob} pool {pool_idx}"
    etype = entry.get("type")

    if etype == "minecraft:empty":
        # A weighted "nothing" alternative — it must KEEP its weight or every
        # sibling's probability inflates (the elder guardian's trim template
        # is weight 1 against empty weight 4).
        return {"item": None, "weight": int(entry.get("weight", 1)),
                "lo": 0, "hi": 0, "smelt": None,
                "size_min": 0, "size_max": SIZE_MAX_OPEN}

    if etype != "minecraft:item":
        warnings.append(f"{where}: skipped entry type {etype}"
                        " (weights of surviving siblings distort)"
                        if len_pool_entries_cache.get((mob, pool_idx), 1) > 1 else
                        f"{where}: skipped entry type {etype}")
        return None

    slug = entry.get("name", "").removeprefix("minecraft:")
    if slug not in items:
        warnings.append(f"{where}: unknown item {slug}")
        return None

    verdict = compile_conditions(entry.get("conditions"))
    if verdict[0] == "false":
        warnings.append(f"{where}: entry {slug} omitted — statically false: {verdict[1]}"
                        " (exact: a failing entry never enters MC's weighted pick)")
        return None
    if verdict[0] == "unsupported":
        warnings.append(f"{where}: skipped entry {slug} — unsupported condition {verdict[1]}")
        return None
    gates = verdict[1]
    if gates["player_kill"] or gates["chance"] != 1.0:
        # Nothing in the current data puts these on an ENTRY; the emitted row
        # has no slot for them, so refuse rather than half-model.
        warnings.append(f"{where}: skipped entry {slug} — entry-level "
                        "player-kill/chance gate not modelled")
        return None

    lo, hi = 1, 1
    smelt = None

    for fn in entry.get("functions", []):
        kind = fn.get("function", "")
        if kind == "minecraft:set_count":
            if fn.get("add"):
                warnings.append(f"{where}: set_count add=true on {slug} unhandled")
            count = fn.get("count", {})
            if isinstance(count, dict) and count.get("type") == "minecraft:uniform":
                # KEEP a negative minimum (spider eye -1..1, magma cream
                # -2..1): the consumer clamps the sampled value to "nothing",
                # which is MC's own semantic for these ranges.
                lo = int(count.get("min", 1))
                hi = int(count.get("max", 1))
            elif isinstance(count, (int, float)):
                lo = hi = int(count)
            else:
                warnings.append(f"{where}: unhandled set_count on {slug}")
        elif kind == "minecraft:furnace_smelt":
            if not is_standard_smelt_conditions(fn.get("conditions")):
                warnings.append(f"{where}: nonstandard furnace_smelt conditions on {slug}")
            # The cooked form is the raw slug with a conventional prefix. MC
            # resolves this through a smelting recipe; the naming is regular
            # enough that a lookup table is not warranted (potato is the one
            # baked_ outlier — the zombie's rare-drop pool).
            for candidate in (f"cooked_{slug}", f"cooked_{slug.removeprefix('raw_')}",
                              f"baked_{slug}"):
                if candidate in items:
                    smelt = items[candidate]
                    break
            if smelt is None:
                warnings.append(f"{where}: no cooked form for {slug}")
        elif kind == "minecraft:enchanted_count_increase":
            # Looting. No enchantments on mob kills yet, so the bonus is zero
            # and the entry is otherwise unaffected.
            pass
        else:
            warnings.append(f"{where}: skipped entry {slug} — unsupported function {kind}")
            return None

    return {"item": items[slug], "weight": int(entry.get("weight", 1)),
            "lo": lo, "hi": hi, "smelt": smelt,
            "size_min": gates["size_min"], "size_max": gates["size_max"]}


# parse_entry wants to know whether its pool had siblings when phrasing the
# weight-distortion warning; filled in by the pool loop before entries parse.
len_pool_entries_cache = {}


def parse_rolls(pool, warnings, mob, pool_idx):
    rolls = pool.get("rolls", 1)
    if isinstance(rolls, (int, float)):
        n = int(rolls)
        return n, n
    if isinstance(rolls, dict) and rolls.get("type") == "minecraft:uniform":
        return int(rolls.get("min", 1)), int(rolls.get("max", 1))
    warnings.append(f"{mob} pool {pool_idx}: unhandled rolls {rolls!r}; assuming 1")
    return 1, 1


def parse_pool(pool, items, warnings, mob, pool_idx):
    """-> pool dict or None (skipped/omitted, reported)."""
    where = f"{mob} pool {pool_idx}"

    if float(pool.get("bonus_rolls", 0.0)) != 0.0:
        warnings.append(f"{where}: bonus_rolls != 0 unhandled (luck does not exist)")

    verdict = compile_conditions(pool.get("conditions"))
    if verdict[0] == "false":
        warnings.append(f"{where}: pool omitted — statically false: {verdict[1]}")
        return None
    if verdict[0] == "unsupported":
        warnings.append(f"{where}: skipped pool — unsupported condition {verdict[1]}")
        return None
    gates = verdict[1]

    min_rolls, max_rolls = parse_rolls(pool, warnings, mob, pool_idx)

    raw_entries = pool.get("entries", [])
    len_pool_entries_cache[(mob, pool_idx)] = len(raw_entries)
    entries = []
    for entry in raw_entries:
        parsed = parse_entry(entry, items, warnings, mob, pool_idx)
        if parsed:
            entries.append(parsed)

    if not entries:
        # Every entry fell away (unknown item, unsupported shape). An empty
        # pool can never drop anything, so it is omitted outright.
        warnings.append(f"{where}: pool omitted — no supported entries survived")
        return None

    return {"player_kill": gates["player_kill"], "chance": gates["chance"],
            "size_min": gates["size_min"], "size_max": gates["size_max"],
            "min_rolls": min_rolls, "max_rolls": max_rolls,
            "entries": entries}


# ── Emission ────────────────────────────────────────────────────────────────

HPP = """// File: src/common/world/loot/GeneratedMobLoot.hpp
// AUTO-GENERATED by tools/gen_mob_loot.py — DO NOT EDIT BY HAND.
//
// Death drops per mob, from data/minecraft/loot_table/entities/*.json, in
// MC's real pool structure: each pool rolls N times, and each roll picks ONE
// entry weighted by `weight` (an Items::Air row is MC's `minecraft:empty` —
// weight that drops nothing). MobManager::EvaluateLootTable, on the
// at-death drop path, is the evaluator.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/Item.hpp"

namespace Game {

    struct MobLootEntry {
        ItemID item;      // Items::Air = MC's `minecraft:empty` alternative
        int    weight;    // relative pick weight within the pool (MC default 1)
        // Inclusive count range. minCount MAY BE NEGATIVE (MC's uniform(-1,1)
        // spider eye / uniform(-2,1) magma cream — "often nothing"): the
        // consumer clamps the SAMPLED value, never the range.
        int    minCount;
        int    maxCount;
        // The cooked form, dropped instead when the mob died on fire
        // (MC's furnace_smelt loot function). Air when the item has none.
        ItemID smeltedItem;
        // Slime-size gate (MC entity_properties type_specific slime), for the
        // magma cube's size>=2 cream entry. sizeMin 0 = ungated.
        int    sizeMin;
        int    sizeMax;
    };

    struct MobLootPool {
        // Roll count, inclusive (the witch's ingredient pool is uniform 1-3).
        int   minRolls;
        int   maxRolls;
        // minecraft:killed_by_player — only drops inside the mob's 100-tick
        // player-kill credit window (blaze rods, phantom membranes...).
        bool  requiresPlayerKill;
        // random_chance / random_chance_with_enchanted_bonus at looting 0.
        // 1.0f = unconditional.
        float chance;
        // Pool-level slime-size gate (the slime's size==1 slimeball pool).
        // sizeMin 0 = ungated.
        int   sizeMin;
        int   sizeMax;
        const MobLootEntry* entries;
        int   entryCount;
    };

    struct MobLootTable {
        EntityTypeId       type;
        const MobLootPool* pools;
        int                poolCount;
    };

    extern const MobLootTable kMobLootTables[];
    extern const int          kMobLootTableCount;

    const MobLootTable* FindMobLootTable(EntityTypeId type);

} // namespace Game
"""


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

        pools = []
        for pool_idx, pool in enumerate(data.get("pools", [])):
            parsed = parse_pool(pool, items, warnings, mob, pool_idx)
            if parsed:
                pools.append(parsed)

        tables.append((enum_name, mob, pools))

    os.makedirs(OUT_DIR, exist_ok=True)

    with open(os.path.join(OUT_DIR, "GeneratedMobLoot.hpp"), "w", encoding="utf-8") as f:
        f.write(HPP)

    with open(os.path.join(OUT_DIR, "GeneratedMobLoot.cpp"), "w", encoding="utf-8") as f:
        f.write("""// File: src/common/world/loot/GeneratedMobLoot.cpp
// AUTO-GENERATED by tools/gen_mob_loot.py — DO NOT EDIT BY HAND.
#include "common/world/loot/GeneratedMobLoot.hpp"
#include "common/entity/GeneratedItemList.hpp"

namespace Game {

""")
        for enum_name, mob, pools in tables:
            # Mobs with no supported pools get no arrays at all — a zero-length
            # array is a Clang extension that MSVC rejects (C2466). They point
            # at nullptr with count 0 in the table below instead.
            if not pools:
                continue
            f.write(f"    // {mob}\n")
            for pi, pool in enumerate(pools):
                f.write(f"    static const MobLootEntry k_{mob}_p{pi}[] = {{\n")
                for e in pool["entries"]:
                    item_expr = f"Items::{e['item']}" if e["item"] else "Items::Air"
                    smelt_expr = f"Items::{e['smelt']}" if e["smelt"] else "Items::Air"
                    f.write(f"        {{ {item_expr}, {e['weight']}, {e['lo']}, {e['hi']},"
                            f" {smelt_expr}, {e['size_min']}, {e['size_max']} }},\n")
                f.write("    };\n")
            f.write(f"    static const MobLootPool k_{mob}[] = {{\n")
            for pi, pool in enumerate(pools):
                pk = "true" if pool["player_kill"] else "false"
                chance = f"{pool['chance']:.6g}"
                if "." not in chance and "e" not in chance:
                    chance += ".0"
                chance += "f"
                f.write(f"        {{ {pool['min_rolls']}, {pool['max_rolls']}, {pk}, {chance},"
                        f" {pool['size_min']}, {pool['size_max']},"
                        f" k_{mob}_p{pi}, {len(pool['entries'])} }},\n")
            f.write("    };\n\n")

        f.write("    const MobLootTable kMobLootTables[] = {\n")
        for enum_name, mob, pools in tables:
            arr = f"k_{mob}" if pools else "nullptr"
            f.write(f"        {{ EntityTypeId::{enum_name}, {arr}, {len(pools)} }},\n")
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

    total_pools = sum(len(p) for _, _, p in tables)
    total_entries = sum(len(pool["entries"]) for _, _, p in tables for pool in p)
    print(f"Wrote {len(tables)} loot tables, {total_pools} pools, {total_entries} entries")
    for w in warnings:
        print(f"  note: {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
