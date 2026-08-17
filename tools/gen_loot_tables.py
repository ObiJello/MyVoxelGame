#!/usr/bin/env python3
# tools/gen_loot_tables.py
#
# Bakes vanilla BLOCK loot tables (data/minecraft/loot_table/blocks/*.json) into
# flat C++ tables. Run by hand whenever data/ is refreshed from a new MC drop.
#
# Outputs (overwritten):
#   src/common/world/loot/GeneratedLootTables.hpp  -- row structs + enums + externs
#   src/common/world/loot/GeneratedLootTables.cpp  -- the data
#
# Why bake instead of parsing at runtime: same reasoning as gen_recipes.py.
# 1084 files / 5.4 MB of JSON would be ~1084 file opens at startup (or file I/O
# on the server thread mid-game if loaded lazily), and `#minecraft:` item tags
# would need a runtime tag resolver that game code does not have. Flattening at
# generation time costs nothing at runtime.
#
# Slugs stay STRINGS in the generated tables and are resolved to numeric ItemIDs
# once, at startup, by LootTables::Initialize — exactly as GeneratedRecipeList
# does. The generator never needs to know engine ids, so BlockID/ItemID values
# can change freely without regenerating.
#
# Everything below is driven by what the vanilla data actually contains, which
# is a far smaller surface than the loot schema allows:
#   entry types  item 1221, alternatives 79, dynamic 1
#   conditions   survives_explosion 835, block_state_property 252, match_tool 203,
#                any_of 32, table_bonus 29, inverted 13, random_chance 7,
#                location_check 4, entity_properties 2
#   functions    set_count 231, explosion_decay 147, copy_components 71,
#                apply_bonus 32, copy_state 10, limit_count 6
#   pools        0..3 per table; rolls is ALWAYS a constant; exactly ONE entry
#                per pool and no `weight`/`quality` anywhere, so MC's weighted
#                selection in LootPool.addRandomItem collapses to "expand it".
# Any type NOT in those lists aborts the run rather than being skipped — a
# silent skip would mean a block quietly stops dropping after a version bump.

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOOT_DIR = REPO_ROOT / "data" / "minecraft" / "loot_table" / "blocks"
ITEM_TAG_DIR = REPO_ROOT / "data" / "minecraft" / "tags" / "item"
OUT_DIR = REPO_ROOT / "src" / "common" / "world" / "loot"
HPP_OUT = OUT_DIR / "GeneratedLootTables.hpp"
CPP_OUT = OUT_DIR / "GeneratedLootTables.cpp"

# ── Enum encodings. Must stay in lockstep with GeneratedLootTables.hpp ───────
ENTRY_ITEM, ENTRY_ALTERNATIVES, ENTRY_EMPTY = 0, 1, 2

(COND_SURVIVES_EXPLOSION, COND_BLOCK_STATE_PROPERTY, COND_MATCH_TOOL_ITEMS,
 COND_MATCH_TOOL_ENCHANTMENT, COND_ANY_OF, COND_TABLE_BONUS, COND_INVERTED,
 COND_RANDOM_CHANCE, COND_LOCATION_CHECK, COND_ENTITY_PROPERTIES) = range(10)

(FUNC_SET_COUNT, FUNC_EXPLOSION_DECAY, FUNC_APPLY_BONUS, FUNC_LIMIT_COUNT,
 FUNC_COPY_COMPONENTS, FUNC_COPY_STATE) = range(6)

COUNT_CONSTANT, COUNT_UNIFORM, COUNT_BINOMIAL = 0, 1, 2
FORMULA_ORE_DROPS, FORMULA_UNIFORM_BONUS, FORMULA_BINOMIAL_BONUS = 0, 1, 2
ENCH_SILK_TOUCH, ENCH_FORTUNE = 0, 1

ENCHANTMENTS = {"minecraft:silk_touch": ENCH_SILK_TOUCH,
                "minecraft:fortune": ENCH_FORTUNE}
FORMULAS = {"minecraft:ore_drops": FORMULA_ORE_DROPS,
            "minecraft:uniform_bonus_count": FORMULA_UNIFORM_BONUS,
            "minecraft:binomial_with_bonus_count": FORMULA_BINOMIAL_BONUS}

INT_MIN, INT_MAX = -2147483648, 2147483647


class GenError(Exception):
    """Anything the generator does not understand. Fatal by design."""


def strip_ns(s: str) -> str:
    """'minecraft:oak_planks' -> 'oak_planks'. Foreign namespaces are kept whole
    so they can never collide with a vanilla slug (same rule as gen_recipes)."""
    return s[len("minecraft:"):] if s.startswith("minecraft:") else s


# ── Item tags (only `#minecraft:cluster_max_harvestables` actually appears) ──
def load_item_tags() -> dict[str, list]:
    tags: dict[str, list] = {}
    if not ITEM_TAG_DIR.is_dir():
        return tags
    for path in sorted(ITEM_TAG_DIR.glob("*.json")):
        try:
            tags[path.stem] = json.loads(path.read_text(encoding="utf-8")).get("values", [])
        except json.JSONDecodeError:
            pass
    return tags


def resolve_tag(tags: dict[str, list], name: str, seen: set[str] | None = None) -> list[str]:
    """Flatten a tag to concrete slugs, expanding nested #tags. Cycle-guarded."""
    seen = seen or set()
    name = strip_ns(name)
    if name in seen:
        return []
    seen.add(name)
    out: list[str] = []
    for value in tags.get(name, []):
        if isinstance(value, dict):
            value = value.get("id", "")
        if not isinstance(value, str) or not value:
            continue
        if value.startswith("#"):
            out.extend(resolve_tag(tags, value[1:], seen))
        else:
            out.append(strip_ns(value))
    return out


def resolve_items(tags: dict[str, list], value) -> list[str]:
    """A match_tool `items` field: one slug, one #tag, or a list of either."""
    if isinstance(value, str):
        return resolve_tag(tags, value[1:]) if value.startswith("#") else [strip_ns(value)]
    if isinstance(value, list):
        out: list[str] = []
        for v in value:
            out.extend(resolve_items(tags, v))
        return out
    raise GenError(f"unsupported match_tool items shape: {value!r}")


class Collector:
    """Flat pools + (begin, count) windows, the GeneratedRecipeList layout.

    Children are always emitted BEFORE their parent, so every window stays
    contiguous no matter how deep the nesting goes."""

    def __init__(self, tags: dict[str, list]):
        self.tags = tags
        self.tables: list[dict] = []
        self.pools: list[dict] = []
        self.entries: list[dict] = []
        self.conds: list[dict] = []
        self.funcs: list[dict] = []
        self.args: list[str] = []
        self.floats: list[float] = []
        self.stats: dict[str, int] = {}

    def count(self, key: str) -> None:
        self.stats[key] = self.stats.get(key, 0) + 1

    def add_args(self, values: list[str]) -> tuple[int, int]:
        begin = len(self.args)
        self.args.extend(values)
        return begin, len(values)

    def add_floats(self, values: list[float]) -> tuple[int, int]:
        begin = len(self.floats)
        self.floats.extend(float(v) for v in values)
        return begin, len(values)

    # ── conditions ──────────────────────────────────────────────────────────
    def add_conditions(self, raw: list | None) -> tuple[int, int]:
        if not raw:
            return 0, 0
        rows = [self.build_condition(c) for c in raw]   # children land first
        begin = len(self.conds)
        self.conds.extend(rows)
        return begin, len(rows)

    def build_condition(self, c: dict) -> dict:
        kind = c.get("condition")
        self.count(f"cond {kind}")
        row = dict(type=0, ench=0, chance=0.0, i0=0,
                   argBegin=0, argCount=0, childBegin=0, childCount=0,
                   floatBegin=0, floatCount=0)

        if kind == "minecraft:survives_explosion":
            row["type"] = COND_SURVIVES_EXPLOSION

        elif kind == "minecraft:entity_properties":
            row["type"] = COND_ENTITY_PROPERTIES

        elif kind == "minecraft:random_chance":
            row["type"] = COND_RANDOM_CHANCE
            chance = c.get("chance")
            if not isinstance(chance, (int, float)):
                raise GenError(f"random_chance with non-constant chance: {chance!r}")
            row["chance"] = float(chance)

        elif kind == "minecraft:block_state_property":
            # Flat (property, value) pairs; values are always strings in vanilla.
            row["type"] = COND_BLOCK_STATE_PROPERTY
            pairs: list[str] = []
            for key, value in (c.get("properties") or {}).items():
                if not isinstance(value, str):
                    raise GenError(f"block_state_property non-string value: {key}={value!r}")
                pairs += [key, value]
            row["argBegin"], row["argCount"] = self.add_args(pairs)

        elif kind == "minecraft:match_tool":
            predicate = c.get("predicate") or {}
            enchants = (predicate.get("predicates") or {}).get("minecraft:enchantments")
            if enchants:
                if len(enchants) != 1:
                    raise GenError(f"match_tool with {len(enchants)} enchantment predicates")
                spec = enchants[0]
                name = spec.get("enchantments")
                if name not in ENCHANTMENTS:
                    raise GenError(f"unknown enchantment in match_tool: {name!r}")
                levels = spec.get("levels") or {}
                row["type"] = COND_MATCH_TOOL_ENCHANTMENT
                row["ench"] = ENCHANTMENTS[name]
                row["i0"] = int(levels.get("min", 1))
            elif "items" in predicate:
                row["type"] = COND_MATCH_TOOL_ITEMS
                items = sorted(set(resolve_items(self.tags, predicate["items"])))
                if not items:
                    raise GenError(f"match_tool items resolved empty: {predicate['items']!r}")
                row["argBegin"], row["argCount"] = self.add_args(items)
            else:
                raise GenError(f"match_tool with neither items nor enchantments: {predicate!r}")

        elif kind == "minecraft:any_of":
            row["type"] = COND_ANY_OF
            row["childBegin"], row["childCount"] = self.add_conditions(c.get("terms"))

        elif kind == "minecraft:inverted":
            row["type"] = COND_INVERTED
            row["childBegin"], row["childCount"] = self.add_conditions([c["term"]])

        elif kind == "minecraft:table_bonus":
            name = c.get("enchantment")
            if name not in ENCHANTMENTS:
                raise GenError(f"unknown enchantment in table_bonus: {name!r}")
            row["type"] = COND_TABLE_BONUS
            row["ench"] = ENCHANTMENTS[name]
            row["floatBegin"], row["floatCount"] = self.add_floats(c.get("chances") or [])

        elif kind == "minecraft:location_check":
            # Only the "is the block at +offset such-and-such" shape appears
            # (tall_grass / large_fern checking their own upper half).
            row["type"] = COND_LOCATION_CHECK
            row["i0"] = int(c.get("offsetY", 0))
            block = ((c.get("predicate") or {}).get("block")) or {}
            blocks = block.get("blocks")
            if not isinstance(blocks, str):
                raise GenError(f"location_check without a single block: {block!r}")
            args = [strip_ns(blocks)]
            for key, value in (block.get("state") or {}).items():
                args += [key, str(value)]
            row["argBegin"], row["argCount"] = self.add_args(args)

        else:
            raise GenError(f"unsupported loot condition: {kind!r}")

        return row

    # ── functions ───────────────────────────────────────────────────────────
    def add_functions(self, raw: list | None) -> tuple[int, int]:
        if not raw:
            return 0, 0
        rows = [self.build_function(f) for f in raw]
        begin = len(self.funcs)
        self.funcs.extend(rows)
        return begin, len(rows)

    def build_function(self, f: dict) -> dict:
        kind = f.get("function")
        self.count(f"func {kind}")
        row = dict(type=0, mode=0, ench=0, add=0, a=0.0, b=0.0, i0=0, i1=0,
                   condBegin=0, condCount=0)
        # 165 functions carry their own conditions (mostly set_count on crops).
        row["condBegin"], row["condCount"] = self.add_conditions(f.get("conditions"))

        if kind == "minecraft:set_count":
            row["type"] = FUNC_SET_COUNT
            row["add"] = 1 if f.get("add") else 0
            value = f.get("count")
            if isinstance(value, (int, float)):
                row["mode"], row["a"] = COUNT_CONSTANT, float(value)
            elif isinstance(value, dict) and value.get("type") == "minecraft:uniform":
                row["mode"] = COUNT_UNIFORM
                row["a"], row["b"] = float(value["min"]), float(value["max"])
            elif isinstance(value, dict) and value.get("type") == "minecraft:binomial":
                row["mode"] = COUNT_BINOMIAL
                row["a"], row["b"] = float(value["n"]), float(value["p"])
            else:
                raise GenError(f"unsupported set_count shape: {value!r}")

        elif kind == "minecraft:explosion_decay":
            row["type"] = FUNC_EXPLOSION_DECAY

        elif kind == "minecraft:copy_components":
            row["type"] = FUNC_COPY_COMPONENTS

        elif kind == "minecraft:copy_state":
            row["type"] = FUNC_COPY_STATE

        elif kind == "minecraft:limit_count":
            row["type"] = FUNC_LIMIT_COUNT
            limit = f.get("limit") or {}
            row["i0"] = int(limit["min"]) if "min" in limit else INT_MIN
            row["i1"] = int(limit["max"]) if "max" in limit else INT_MAX

        elif kind == "minecraft:apply_bonus":
            name, formula = f.get("enchantment"), f.get("formula")
            if name not in ENCHANTMENTS:
                raise GenError(f"unknown enchantment in apply_bonus: {name!r}")
            if formula not in FORMULAS:
                raise GenError(f"unknown apply_bonus formula: {formula!r}")
            params = f.get("parameters") or {}
            row["type"] = FUNC_APPLY_BONUS
            row["ench"] = ENCHANTMENTS[name]
            row["mode"] = FORMULAS[formula]
            row["i0"] = int(params.get("bonusMultiplier", 0))
            row["i1"] = int(params.get("extra", 0))
            row["a"] = float(params.get("probability", 0.0))

        else:
            raise GenError(f"unsupported loot function: {kind!r}")

        return row

    # ── entries ─────────────────────────────────────────────────────────────
    def add_entries(self, raw: list | None) -> tuple[int, int]:
        if not raw:
            return 0, 0
        rows = [self.build_entry(e) for e in raw]
        begin = len(self.entries)
        self.entries.extend(rows)
        return begin, len(rows)

    def build_entry(self, e: dict) -> dict:
        kind = e.get("type")
        self.count(f"entry {kind}")
        if "weight" in e or "quality" in e:
            # Vanilla block tables have neither, so the runtime does not
            # implement MC's weighted pick. Shout if that ever changes.
            raise GenError(f"entry carries weight/quality, which the runtime ignores: {e!r}")

        row = dict(kind=ENTRY_EMPTY, slug="", childBegin=0, childCount=0,
                   condBegin=0, condCount=0, fnBegin=0, fnCount=0)
        if kind == "minecraft:item":
            row["kind"] = ENTRY_ITEM
            row["slug"] = strip_ns(e["name"])
        elif kind == "minecraft:alternatives":
            row["kind"] = ENTRY_ALTERNATIVES
            row["childBegin"], row["childCount"] = self.add_entries(e.get("children"))
        elif kind in ("minecraft:empty", "minecraft:dynamic"):
            # `dynamic` is decorated_pot's sherds only — it copies block-entity
            # contents, which needs NBT we don't carry. Drops nothing for now.
            row["kind"] = ENTRY_EMPTY
        else:
            raise GenError(f"unsupported loot entry type: {kind!r}")

        row["condBegin"], row["condCount"] = self.add_conditions(e.get("conditions"))
        row["fnBegin"], row["fnCount"] = self.add_functions(e.get("functions"))
        return row

    # ── tables ──────────────────────────────────────────────────────────────
    def add_table(self, slug: str, data: dict) -> None:
        pool_rows = []
        for pool in data.get("pools") or []:
            rolls = pool.get("rolls")
            if not isinstance(rolls, (int, float)):
                raise GenError(f"{slug}: non-constant rolls {rolls!r}")
            entry_begin, entry_count = self.add_entries(pool.get("entries"))
            cond_begin, cond_count = self.add_conditions(pool.get("conditions"))
            fn_begin, fn_count = self.add_functions(pool.get("functions"))
            pool_rows.append(dict(rolls=int(rolls),
                                  entryBegin=entry_begin, entryCount=entry_count,
                                  condBegin=cond_begin, condCount=cond_count,
                                  fnBegin=fn_begin, fnCount=fn_count))
        pool_begin = len(self.pools)
        self.pools.extend(pool_rows)
        fn_begin, fn_count = self.add_functions(data.get("functions"))
        self.tables.append(dict(slug=slug, poolBegin=pool_begin, poolCount=len(pool_rows),
                                fnBegin=fn_begin, fnCount=fn_count))


def fmt_float(v: float) -> str:
    # %g drops the fractional part on round numbers, and `1f` / `0f` are not
    # valid C++ literals (the compiler reads them as a malformed integer). Make
    # sure a decimal point or exponent is always present before the suffix.
    text = f"{v:g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text + "f"


def emit_hpp() -> str:
    return '''// File: src/common/world/loot/GeneratedLootTables.hpp
// AUTO-GENERATED by tools/gen_loot_tables.py — DO NOT EDIT BY HAND.
//
// Vanilla block loot tables, flattened. Windows are (begin, count) pairs into
// the flat pools below, mirroring GeneratedRecipeList's layout. Slugs stay as
// strings; LootTables::Initialize resolves them to BlockID/ItemID once.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Game {

    enum class LootEntryKind : uint8_t {
        Item = 0,          // drop `itemSlug`
        Alternatives = 1,  // first child whose conditions pass (MC AlternativesEntry)
        Empty = 2,         // drops nothing (also stands in for `dynamic`)
    };

    enum class LootCondType : uint8_t {
        SurvivesExplosion = 0,
        BlockStateProperty = 1,     // args = flat (property, value) pairs
        MatchToolItems = 2,         // args = item slugs
        MatchToolEnchantment = 3,   // ench + i0 = minimum level
        AnyOf = 4,                  // children
        TableBonus = 5,             // ench + floats = per-level chances
        Inverted = 6,               // one child
        RandomChance = 7,           // chance
        LocationCheck = 8,          // i0 = offsetY, args = blockSlug then state pairs
        EntityProperties = 9,
    };

    enum class LootFuncType : uint8_t {
        SetCount = 0,        // mode/a/b/add
        ExplosionDecay = 1,
        ApplyBonus = 2,      // ench + mode(formula) + i0/i1/a
        LimitCount = 3,      // i0 = min, i1 = max
        CopyComponents = 4,
        CopyState = 5,
    };

    enum class LootCountMode : uint8_t { Constant = 0, Uniform = 1, Binomial = 2 };
    enum class LootBonusFormula : uint8_t { OreDrops = 0, UniformBonusCount = 1, BinomialWithBonusCount = 2 };
    enum class LootEnchantment : uint8_t { SilkTouch = 0, Fortune = 1 };

    struct LootTableRow {
        const char* blockSlug;   // vanilla registry name, matched to Block::registrySlug
        uint32_t poolBegin;  uint16_t poolCount;
        uint32_t fnBegin;    uint16_t fnCount;   // table-level functions
    };

    struct LootPoolRow {
        uint8_t  rolls;
        uint32_t entryBegin; uint16_t entryCount;
        uint32_t condBegin;  uint16_t condCount;
        uint32_t fnBegin;    uint16_t fnCount;
    };

    struct LootEntryRow {
        uint8_t     kind;        // LootEntryKind
        const char* itemSlug;    // Item entries only; "" otherwise
        uint32_t childBegin; uint16_t childCount;
        uint32_t condBegin;  uint16_t condCount;
        uint32_t fnBegin;    uint16_t fnCount;
    };

    struct LootCondRow {
        uint8_t  type;           // LootCondType
        uint8_t  ench;           // LootEnchantment
        float    chance;
        int32_t  i0;
        uint32_t argBegin;   uint16_t argCount;
        uint32_t childBegin; uint16_t childCount;
        uint32_t floatBegin; uint16_t floatCount;
    };

    struct LootFuncRow {
        uint8_t  type;           // LootFuncType
        uint8_t  mode;           // LootCountMode or LootBonusFormula
        uint8_t  ench;           // LootEnchantment
        uint8_t  add;            // set_count: add to the current count instead of replacing
        float    a, b;
        int32_t  i0, i1;
        uint32_t condBegin;  uint16_t condCount;
    };

    extern const LootTableRow kLootTables[];      extern const size_t kLootTableCount;
    extern const LootPoolRow  kLootPools[];       extern const size_t kLootPoolCount;
    extern const LootEntryRow kLootEntries[];     extern const size_t kLootEntryCount;
    extern const LootCondRow  kLootConditions[];  extern const size_t kLootConditionCount;
    extern const LootFuncRow  kLootFunctions[];   extern const size_t kLootFunctionCount;
    extern const char* const  kLootArgs[];        extern const size_t kLootArgCount;
    extern const float        kLootFloats[];      extern const size_t kLootFloatCount;

} // namespace Game
'''


def emit_cpp(c: Collector) -> str:
    lines = [
        "// File: src/common/world/loot/GeneratedLootTables.cpp",
        "// AUTO-GENERATED by tools/gen_loot_tables.py — DO NOT EDIT BY HAND.",
        '#include "GeneratedLootTables.hpp"',
        "",
        "namespace Game {",
        "",
    ]

    lines.append(f"    // {len(c.args)} interned strings: item slugs, state key/value pairs.")
    lines.append("    const char* const kLootArgs[] = {")
    for i in range(0, len(c.args), 6):
        lines.append("        " + " ".join(f'"{s}",' for s in c.args[i:i + 6]))
    lines.append("    };")
    lines.append("    const size_t kLootArgCount = sizeof(kLootArgs) / sizeof(kLootArgs[0]);")
    lines.append("")

    lines.append("    const float kLootFloats[] = {")
    for i in range(0, len(c.floats), 8):
        lines.append("        " + " ".join(fmt_float(f) + "," for f in c.floats[i:i + 8]))
    lines.append("    };")
    lines.append("    const size_t kLootFloatCount = sizeof(kLootFloats) / sizeof(kLootFloats[0]);")
    lines.append("")

    lines.append(f"    // {len(c.conds)} conditions. Children always precede their parent.")
    lines.append("    const LootCondRow kLootConditions[] = {")
    for r in c.conds:
        lines.append(f'        {{ {r["type"]}, {r["ench"]}, {fmt_float(r["chance"])}, {r["i0"]}, '
                     f'{r["argBegin"]}, {r["argCount"]}, {r["childBegin"]}, {r["childCount"]}, '
                     f'{r["floatBegin"]}, {r["floatCount"]} }},')
    lines.append("    };")
    lines.append("    const size_t kLootConditionCount = sizeof(kLootConditions) / sizeof(kLootConditions[0]);")
    lines.append("")

    lines.append(f"    // {len(c.funcs)} functions.")
    lines.append("    const LootFuncRow kLootFunctions[] = {")
    for r in c.funcs:
        lines.append(f'        {{ {r["type"]}, {r["mode"]}, {r["ench"]}, {r["add"]}, '
                     f'{fmt_float(r["a"])}, {fmt_float(r["b"])}, {r["i0"]}, {r["i1"]}, '
                     f'{r["condBegin"]}, {r["condCount"]} }},')
    lines.append("    };")
    lines.append("    const size_t kLootFunctionCount = sizeof(kLootFunctions) / sizeof(kLootFunctions[0]);")
    lines.append("")

    lines.append(f"    // {len(c.entries)} entries.")
    lines.append("    const LootEntryRow kLootEntries[] = {")
    for r in c.entries:
        lines.append(f'        {{ {r["kind"]}, "{r["slug"]}", {r["childBegin"]}, {r["childCount"]}, '
                     f'{r["condBegin"]}, {r["condCount"]}, {r["fnBegin"]}, {r["fnCount"]} }},')
    lines.append("    };")
    lines.append("    const size_t kLootEntryCount = sizeof(kLootEntries) / sizeof(kLootEntries[0]);")
    lines.append("")

    lines.append(f"    // {len(c.pools)} pools.")
    lines.append("    const LootPoolRow kLootPools[] = {")
    for r in c.pools:
        lines.append(f'        {{ {r["rolls"]}, {r["entryBegin"]}, {r["entryCount"]}, '
                     f'{r["condBegin"]}, {r["condCount"]}, {r["fnBegin"]}, {r["fnCount"]} }},')
    lines.append("    };")
    lines.append("    const size_t kLootPoolCount = sizeof(kLootPools) / sizeof(kLootPools[0]);")
    lines.append("")

    lines.append(f"    // {len(c.tables)} block loot tables, sorted by slug.")
    lines.append("    const LootTableRow kLootTables[] = {")
    for r in c.tables:
        lines.append(f'        {{ "{r["slug"]}", {r["poolBegin"]}, {r["poolCount"]}, '
                     f'{r["fnBegin"]}, {r["fnCount"]} }},')
    lines.append("    };")
    lines.append("    const size_t kLootTableCount = sizeof(kLootTables) / sizeof(kLootTables[0]);")
    lines.append("")
    lines.append("} // namespace Game")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    if not LOOT_DIR.is_dir():
        print(f"error: cannot find {LOOT_DIR}", file=sys.stderr)
        return 1

    collector = Collector(load_item_tags())
    files = sorted(LOOT_DIR.glob("*.json"))
    for path in files:
        data = json.loads(path.read_text(encoding="utf-8"))
        try:
            collector.add_table(path.stem, data)
        except GenError as e:
            print(f"error: {path.name}: {e}", file=sys.stderr)
            print("Refusing to generate: an unhandled loot type would silently "
                  "stop a block from dropping. Teach the generator and the "
                  "runtime about it, then re-run.", file=sys.stderr)
            return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    HPP_OUT.write_text(emit_hpp(), encoding="utf-8")
    CPP_OUT.write_text(emit_cpp(collector), encoding="utf-8")

    empty = sum(1 for t in collector.tables if t["poolCount"] == 0)
    print(f"Generated {len(collector.tables)} block loot tables "
          f"({empty} of them drop nothing).")
    print(f"  -> {HPP_OUT.relative_to(REPO_ROOT)}")
    print(f"  -> {CPP_OUT.relative_to(REPO_ROOT)}")
    print(f"  pools={len(collector.pools)} entries={len(collector.entries)} "
          f"conditions={len(collector.conds)} functions={len(collector.funcs)} "
          f"args={len(collector.args)} floats={len(collector.floats)}")
    for key in sorted(collector.stats):
        print(f"    {collector.stats[key]:5}  {key}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
