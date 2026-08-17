#!/usr/bin/env python3
"""Generate src/common/world/crafting/GeneratedRecipeList.{hpp,cpp} from the
vendored vanilla data pack.

Sources (same snapshot as everything else vendored under data/):
    data/minecraft/recipe/*.json      — 1000+ recipe definitions
    data/minecraft/tags/item/*.json   — the #minecraft:planks style tags

Why generate instead of parsing at runtime: `data/` is 35MB and is NOT copied
into the app bundle (only `assets/` is), and parsing 1400 small JSON files at
startup costs hundreds of milliseconds. Baking them into a C++ table mirrors
what tools/gen_items.py already does for Items.java, ships nothing extra, and
resolves every `#tag` here rather than at load time.

Handled recipe types:
    minecraft:crafting_shaped      — pattern + key
    minecraft:crafting_shapeless   — unordered ingredient list
    minecraft:crafting_transmute   — input + material, result copies input's
                                     components (bundle/shulker dyeing)
    minecraft:smelting             — furnace     \\
    minecraft:blasting             — blast furnace|  emitted into their own
    minecraft:smoking              — smoker      |  kCookingRecipeTable
    minecraft:campfire_cooking     — campfire    /

Deliberately skipped:
    minecraft:crafting_special_*   — code-driven (firework, map cloning, armour
                                     dye, banner duplicate, repair). They need
                                     bespoke assemble() logic, not table data.
    minecraft:stonecutting         — emitted into kStonecuttingTable\n    smithing_*                     — belongs to a block we have no menu for.

Known lossy spot: 16 recipes (the suspicious stew variants) carry a
`components` object on their result. We emit the item + count and drop the
components, so the stew crafts but has no effect on it.

Usage:  python3 tools/gen_recipes.py
"""

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECIPE_DIR = os.path.join(REPO, "data", "minecraft", "recipe")
ITEM_TAG_DIR = os.path.join(REPO, "data", "minecraft", "tags", "item")
OUT_DIR = os.path.join(REPO, "src", "common", "world", "crafting")

CRAFTING_TYPES = {
    "minecraft:crafting_shaped",
    "minecraft:crafting_shapeless",
    "minecraft:crafting_transmute",
}

# The furnace family. These live in their own table rather than alongside the
# grid recipes: they carry cookingtime + experience that no grid recipe has,
# they are looked up by a SINGLE ingredient rather than by pattern, and MC
# likewise keeps them as distinct RecipeTypes (SMELTING / BLASTING / SMOKING /
# CAMPFIRE_COOKING) queried by the block that can run them. They do reuse the
# same interned ingredient pool, so a slug set shared with a crafting recipe is
# still stored once.
STONECUTTING_TYPE = "minecraft:stonecutting"

COOKING_TYPES = {
    "minecraft:smelting":         0,
    "minecraft:blasting":         1,
    "minecraft:smoking":          2,
    "minecraft:campfire_cooking": 3,
}


def strip_ns(identifier):
    """'minecraft:oak_planks' -> 'oak_planks'. Non-minecraft namespaces are
    kept whole so they can never collide with a vanilla slug."""
    if identifier.startswith("minecraft:"):
        return identifier[len("minecraft:"):]
    return identifier


# ── Tags ────────────────────────────────────────────────────────────────────

def load_item_tags():
    """slug-keyed map of tag name -> list of raw entries (may contain #tags)."""
    tags = {}
    for name in os.listdir(ITEM_TAG_DIR):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(ITEM_TAG_DIR, name)) as f:
            data = json.load(f)
        tags[strip_ns(name[:-len(".json")])] = data.get("values", [])
    return tags


def resolve_tag(tag_name, tags, seen=None):
    """Flatten a tag to a sorted list of item slugs, following nested #tags.

    MC allows optional entries ({"id": ..., "required": false}); those resolve
    to nothing when the item is absent, and since we have no notion of absent
    vanilla items we just take the id."""
    if seen is None:
        seen = set()
    if tag_name in seen:
        return []          # cyclic tag reference — vanilla has none, be safe
    seen.add(tag_name)

    out = []
    for entry in tags.get(tag_name, []):
        if isinstance(entry, dict):
            entry = entry.get("id", "")
        if not isinstance(entry, str) or not entry:
            continue
        if entry.startswith("#"):
            out.extend(resolve_tag(strip_ns(entry[1:]), tags, seen))
        else:
            out.append(strip_ns(entry))
    return out


def resolve_ingredient(value, tags):
    """A recipe ingredient is a slug, a #tag, or a list of either. Returns the
    sorted, de-duplicated set of item slugs that satisfy it."""
    items = []
    values = value if isinstance(value, list) else [value]
    for v in values:
        if isinstance(v, dict):
            v = v.get("item") or v.get("tag") or ""
        if not isinstance(v, str) or not v:
            continue
        if v.startswith("#"):
            items.extend(resolve_tag(strip_ns(v[1:]), tags))
        else:
            items.append(strip_ns(v))
    return sorted(set(items))


# ── Shaped patterns ─────────────────────────────────────────────────────────

def shrink_pattern(pattern):
    """Port of ShapedRecipePattern.shrink — trim the blank border so the stored
    pattern is exactly its bounding box. Matching then only has to compare
    equal-sized grids, which is what MC's matches() relies on."""
    left, right, top, bottom = sys.maxsize, -1, 0, 0
    for i, line in enumerate(pattern):
        first = next((x for x, c in enumerate(line) if c != " "), len(line))
        last = next((x for x in range(len(line) - 1, -1, -1) if line[x] != " "), -1)
        left = min(left, first)
        right = max(right, last)
        if last < 0:
            if top == i:
                top += 1
            bottom += 1
        else:
            bottom = 0
    if len(pattern) == bottom:
        return []
    return [line[left:right + 1] for line in pattern[top:len(pattern) - bottom]]


# ── Collection ──────────────────────────────────────────────────────────────

class Collector:
    def __init__(self):
        self.ingredients = []      # list of tuple(slug, ...)
        self.ingredient_index = {}

    def intern(self, slugs):
        key = tuple(slugs)
        idx = self.ingredient_index.get(key)
        if idx is None:
            idx = len(self.ingredients)
            self.ingredient_index[key] = idx
            self.ingredients.append(key)
        return idx



# ── Furnace fuel ────────────────────────────────────────────────────────────
# Verbatim port of FuelValues.vanillaBurnTimes (FuelValues.java:43) with
# baseUnit = 200, in SOURCE ORDER. Order is load-bearing: Builder.putInternal
# does a plain map put, so a later entry overrides an earlier one for the same
# item (bamboo_mosaic_slab's 150 beats #planks' 300). The trailing
# remove(#non_flammable_wood) strips the crimson/warped set, which is why
# nether "wood" doesn't burn.
#
# Integer division is Java's, i.e. truncating: baseUnit*3/4 = 150,
# 1 + baseUnit/3 = 67.
FUEL_BASE_UNIT = 200
_U = FUEL_BASE_UNIT
FUEL_SPEC = [
    ("lava_bucket", _U * 100), ("coal_block", _U * 8 * 10), ("blaze_rod", _U * 12),
    ("coal", _U * 8), ("charcoal", _U * 8),
    ("#logs", _U * 3 // 2), ("#bamboo_blocks", _U * 3 // 2), ("#planks", _U * 3 // 2),
    ("bamboo_mosaic", _U * 3 // 2), ("#wooden_stairs", _U * 3 // 2),
    ("bamboo_mosaic_stairs", _U * 3 // 2),
    ("#wooden_slabs", _U * 3 // 4), ("bamboo_mosaic_slab", _U * 3 // 4),
    ("#wooden_trapdoors", _U * 3 // 2), ("#wooden_pressure_plates", _U * 3 // 2),
    ("#wooden_shelves", _U * 3 // 2), ("#wooden_fences", _U * 3 // 2),
    ("#fence_gates", _U * 3 // 2),
    ("note_block", _U * 3 // 2), ("bookshelf", _U * 3 // 2),
    ("chiseled_bookshelf", _U * 3 // 2), ("lectern", _U * 3 // 2),
    ("jukebox", _U * 3 // 2), ("chest", _U * 3 // 2), ("trapped_chest", _U * 3 // 2),
    ("crafting_table", _U * 3 // 2), ("daylight_detector", _U * 3 // 2),
    ("#banners", _U * 3 // 2), ("bow", _U * 3 // 2), ("fishing_rod", _U * 3 // 2),
    ("ladder", _U * 3 // 2),
    ("#signs", _U), ("#hanging_signs", _U * 4),
    ("wooden_shovel", _U), ("wooden_sword", _U), ("wooden_spear", _U),
    ("wooden_hoe", _U), ("wooden_axe", _U), ("wooden_pickaxe", _U),
    ("#wooden_doors", _U), ("#boats", _U * 6),
    ("#wool", _U // 2), ("#wooden_buttons", _U // 2), ("stick", _U // 2),
    ("#saplings", _U // 2), ("bowl", _U // 2),
    ("#wool_carpets", 1 + _U // 3), ("dried_kelp_block", 1 + _U * 20),
    ("crossbow", _U * 3 // 2), ("bamboo", _U // 4),
    ("dead_bush", _U // 2), ("short_dry_grass", _U // 2), ("tall_dry_grass", _U // 2),
    ("scaffolding", _U // 4),
    ("loom", _U * 3 // 2), ("barrel", _U * 3 // 2), ("cartography_table", _U * 3 // 2),
    ("fletching_table", _U * 3 // 2), ("smithing_table", _U * 3 // 2),
    ("composter", _U * 3 // 2),
    ("azalea", _U // 2), ("flowering_azalea", _U // 2),
    ("mangrove_roots", _U * 3 // 2), ("leaf_litter", _U // 2),
]
FUEL_REMOVE_TAG = "non_flammable_wood"


def build_fuel(tags):
    """Resolve FUEL_SPEC to {slug: burnTicks}, applying MC's later-wins order
    and the trailing non_flammable_wood removal."""
    values = {}
    for entry, time in FUEL_SPEC:
        if entry.startswith("#"):
            for slug in resolve_tag(strip_ns(entry[1:]), tags):
                values[slug] = time
        else:
            values[entry] = time
    for slug in resolve_tag(FUEL_REMOVE_TAG, tags):
        values.pop(slug, None)
    return sorted(values.items())


def collect():
    tags = load_item_tags()
    collector = Collector()
    recipes = []
    cooking = []
    stonecutting = []
    skipped = {}

    for name in sorted(os.listdir(RECIPE_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(RECIPE_DIR, name)) as f:
            data = json.load(f)

        kind = data.get("type", "")
        if (kind not in CRAFTING_TYPES and kind not in COOKING_TYPES
                and kind != STONECUTTING_TYPE):
            skipped[kind] = skipped.get(kind, 0) + 1
            continue

        result = data.get("result", {})
        result_slug = strip_ns(result.get("id", ""))
        if not result_slug:
            continue
        result_count = int(result.get("count", 1))
        recipe_id = "minecraft:" + name[:-len(".json")]

        if kind == STONECUTTING_TYPE:
            # One ingredient → one result, with a count. No time, no XP: a
            # stonecutter is instant, which is why MC keeps StonecutterRecipe
            # as its own SingleItemRecipe rather than a cooking one.
            input_slugs = resolve_ingredient(data.get("ingredient", ""), tags)
            if not input_slugs:
                continue
            stonecutting.append(dict(ingredient=collector.intern(input_slugs),
                                     result=result_slug, count=result_count))
            continue

        if kind in COOKING_TYPES:
            # One ingredient (a slug, a #tag, or a list of either) → one result,
            # plus how long it takes and the XP it awards.
            input_slugs = resolve_ingredient(data.get("ingredient", ""), tags)
            if not input_slugs:
                continue
            cooking.append(dict(id=recipe_id, kind=COOKING_TYPES[kind],
                                ingredient=collector.intern(input_slugs),
                                result=result_slug, count=result_count,
                                time=int(data.get("cookingtime", 200)),
                                experience=float(data.get("experience", 0.0))))
            continue

        if kind == "minecraft:crafting_shaped":
            pattern = shrink_pattern(data.get("pattern", []))
            if not pattern:
                continue
            key = data.get("key", {})
            width, height = len(pattern[0]), len(pattern)
            cells = []
            broken = False
            for line in pattern:
                for ch in line:
                    if ch == " ":
                        cells.append(-1)
                        continue
                    if ch not in key:
                        broken = True
                        break
                    slugs = resolve_ingredient(key[ch], tags)
                    if not slugs:
                        broken = True
                        break
                    cells.append(collector.intern(slugs))
                if broken:
                    break
            if broken:
                continue
            recipes.append(dict(id=recipe_id, kind=0, result=result_slug,
                                count=result_count, width=width, height=height,
                                cells=cells))

        elif kind == "minecraft:crafting_shapeless":
            cells = []
            broken = False
            for raw in data.get("ingredients", []):
                slugs = resolve_ingredient(raw, tags)
                if not slugs:
                    broken = True
                    break
                cells.append(collector.intern(slugs))
            if broken or not cells:
                continue
            recipes.append(dict(id=recipe_id, kind=1, result=result_slug,
                                count=result_count, width=0, height=0,
                                cells=cells))

        else:  # crafting_transmute
            input_slugs = resolve_ingredient(data.get("input", ""), tags)
            material_slugs = resolve_ingredient(data.get("material", ""), tags)
            if not input_slugs or not material_slugs:
                continue
            # Ingredient 0 is the component donor — see kind 2 in the loader.
            cells = [collector.intern(input_slugs), collector.intern(material_slugs)]
            recipes.append(dict(id=recipe_id, kind=2, result=result_slug,
                                count=result_count, width=0, height=0,
                                cells=cells))

    return collector, recipes, cooking, stonecutting, build_fuel(tags), skipped


# ── Emit ────────────────────────────────────────────────────────────────────

HEADER = """// File: src/common/world/crafting/GeneratedRecipeList.{ext}
// AUTO-GENERATED by tools/gen_recipes.py — DO NOT EDIT BY HAND.
// Source: data/minecraft/recipe/*.json + data/minecraft/tags/item/*.json
// (the vendored vanilla data pack). Re-run the generator after bumping the MC
// snapshot; see CLAUDE.md, "Updating to a Newer Minecraft Version".
"""


def emit(collector, recipes, cooking, stonecutting, fuel, skipped):
    os.makedirs(OUT_DIR, exist_ok=True)

    # Flat slug pool: every ingredient is a [begin, count) window into it.
    slug_pool = []
    windows = []
    for slugs in collector.ingredients:
        windows.append((len(slug_pool), len(slugs)))
        slug_pool.extend(slugs)

    cell_pool = []
    for r in recipes:
        r["cell_begin"] = len(cell_pool)
        cell_pool.extend(r["cells"])

    with open(os.path.join(OUT_DIR, "GeneratedRecipeList.hpp"), "w") as f:
        f.write(HEADER.format(ext="hpp"))
        f.write("""#pragma once

#include <cstddef>
#include <cstdint>

namespace Game {

    // One ingredient = the set of item slugs that satisfy it, stored as a
    // [begin, count) window into kRecipeIngredientSlugs. Tag references
    // (#minecraft:planks) are already flattened, and identical sets are
    // interned, so the 12-slug "planks" set exists exactly once.
    struct RecipeIngredientRow {
        uint32_t begin;
        uint16_t count;
    };

    // Recipe kinds, matching the vanilla `type` field.
    enum class RecipeKind : uint8_t {
        Shaped    = 0,   // width x height grid of cells; -1 cell = must be empty
        Shapeless = 1,   // unordered ingredient list
        // input + material; the result copies the INPUT stack's components
        // (ingredient 0 is the donor). Vanilla bundle / shulker-box dyeing.
        Transmute = 2,
    };

    struct RecipeRow {
        const char* id;            // "minecraft:stick" — identity for logs
        const char* resultSlug;
        uint8_t     resultCount;
        uint8_t     kind;          // RecipeKind
        uint8_t     width;         // 0 for the unordered kinds
        uint8_t     height;        // 0 for the unordered kinds
        uint32_t    cellBegin;     // window into kRecipeCells
        uint8_t     cellCount;     // width*height (shaped) or ingredient count
    };

    // Ingredient index per cell; -1 means "this cell must be empty" (shaped
    // patterns only — the unordered kinds never store one).
    extern const int32_t             kRecipeCells[];
    extern const char* const         kRecipeIngredientSlugs[];
    extern const RecipeIngredientRow kRecipeIngredients[];
    extern const size_t              kRecipeIngredientCount;
    extern const RecipeRow           kRecipeTable[];
    extern const size_t              kRecipeTableSize;

    // The furnace family. MC keeps SMELTING / BLASTING / SMOKING /
    // CAMPFIRE_COOKING as distinct RecipeTypes queried by the block that can
    // run them, and so do we — a furnace only ever sees Smelting, a blast
    // furnace only Blasting.
    enum class CookingKind : uint8_t {
        Smelting        = 0,
        Blasting        = 1,
        Smoking         = 2,
        CampfireCooking = 3,
    };

    struct CookingRecipeRow {
        const char* id;
        uint8_t     kind;             // CookingKind
        uint32_t    ingredient;       // index into kRecipeIngredients
        const char* resultSlug;
        uint8_t     resultCount;
        uint16_t    cookingTime;      // ticks (MC's `cookingtime`, default 200)
        float       experience;       // XP awarded when the result is taken
    };

    extern const CookingRecipeRow    kCookingRecipeTable[];
    extern const size_t              kCookingRecipeTableSize;

    // Furnace fuel — MC FuelValues.vanillaBurnTimes, tags flattened.
    // `burnTicks` is how long ONE of this item keeps a furnace lit.
    struct FuelRow {
        const char* slug;
        uint16_t    burnTicks;
    };
    extern const FuelRow             kFuelTable[];
    extern const size_t              kFuelTableSize;

    // Stonecutter — MC StonecutterRecipe, a SingleItemRecipe. One input maps to
    // SEVERAL results (andesite gives slab / stairs / wall / …), which is why
    // the menu shows a pick-list rather than a single output.
    struct StonecuttingRow {
        uint32_t    ingredient;   // index into kRecipeIngredients
        const char* resultSlug;
        uint8_t     resultCount;
    };
    extern const StonecuttingRow     kStonecuttingTable[];
    extern const size_t              kStonecuttingTableSize;

} // namespace Game
""")

    with open(os.path.join(OUT_DIR, "GeneratedRecipeList.cpp"), "w") as f:
        f.write(HEADER.format(ext="cpp"))
        f.write('#include "GeneratedRecipeList.hpp"\n\nnamespace Game {\n\n')

        f.write("    const char* const kRecipeIngredientSlugs[] = {\n")
        for i in range(0, len(slug_pool), 6):
            f.write("        " + " ".join('"%s",' % s for s in slug_pool[i:i + 6]) + "\n")
        f.write("    };\n\n")

        f.write("    const RecipeIngredientRow kRecipeIngredients[] = {\n")
        for begin, count in windows:
            f.write("        { %d, %d },\n" % (begin, count))
        f.write("    };\n")
        f.write("    const size_t kRecipeIngredientCount = %d;\n\n" % len(windows))

        f.write("    const int32_t kRecipeCells[] = {\n")
        for i in range(0, len(cell_pool), 16):
            f.write("        " + " ".join("%d," % c for c in cell_pool[i:i + 16]) + "\n")
        f.write("    };\n\n")

        f.write("    const RecipeRow kRecipeTable[] = {\n")
        for r in recipes:
            f.write('        { "%s", "%s", %d, %d, %d, %d, %d, %d },\n' % (
                r["id"], r["result"], r["count"], r["kind"],
                r["width"], r["height"], r["cell_begin"], len(r["cells"])))
        f.write("    };\n")
        f.write("    const size_t kRecipeTableSize = sizeof(kRecipeTable) / sizeof(kRecipeTable[0]);\n\n")

        f.write("    // Furnace-family recipes. `ingredient` indexes the SAME\n"
                "    // interned pool as the grid recipes above.\n")
        f.write("    const CookingRecipeRow kCookingRecipeTable[] = {\n")
        for c in cooking:
            f.write('        { "%s", %d, %d, "%s", %d, %d, %sf },\n' % (
                c["id"], c["kind"], c["ingredient"], c["result"],
                c["count"], c["time"], repr(c["experience"])))
        f.write("    };\n")
        f.write("    const size_t kCookingRecipeTableSize = "
                "sizeof(kCookingRecipeTable) / sizeof(kCookingRecipeTable[0]);\n\n")

        f.write("    const StonecuttingRow kStonecuttingTable[] = {\n")
        for c in stonecutting:
            f.write('        { %d, "%s", %d },\n' % (c["ingredient"], c["result"], c["count"]))
        f.write("    };\n")
        f.write("    const size_t kStonecuttingTableSize = "
                "sizeof(kStonecuttingTable) / sizeof(kStonecuttingTable[0]);\n\n")

        f.write("    const FuelRow kFuelTable[] = {\n")
        for slug, ticks in fuel:
            f.write('        { "%s", %d },\n' % (slug, ticks))
        f.write("    };\n")
        f.write("    const size_t kFuelTableSize = "
                "sizeof(kFuelTable) / sizeof(kFuelTable[0]);\n\n")
        f.write("} // namespace Game\n")

    kinds = {0: "shaped", 1: "shapeless", 2: "transmute"}
    counts = {}
    for r in recipes:
        counts[kinds[r["kind"]]] = counts.get(kinds[r["kind"]], 0) + 1
    print("Emitted %d recipes (%s)" % (
        len(recipes), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    print("  %d interned ingredients, %d slug entries, %d cells"
          % (len(windows), len(slug_pool), len(cell_pool)))
    cook_kinds = {0: "smelting", 1: "blasting", 2: "smoking", 3: "campfire"}
    cook_counts = {}
    for c in cooking:
        cook_counts[cook_kinds[c["kind"]]] = cook_counts.get(cook_kinds[c["kind"]], 0) + 1
    print("Emitted %d cooking recipes (%s)" % (
        len(cooking), ", ".join("%s %d" % kv for kv in sorted(cook_counts.items()))))
    print("Emitted %d stonecutting recipes" % len(stonecutting))
    print("Emitted %d fuel entries" % len(fuel))
    if skipped:
        print("  skipped non-crafting types:")
        for k, v in sorted(skipped.items(), key=lambda kv: -kv[1]):
            print("    %-40s %d" % (k, v))


if __name__ == "__main__":
    emit(*collect())
