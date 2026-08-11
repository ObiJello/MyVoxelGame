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

Deliberately skipped:
    minecraft:crafting_special_*   — code-driven (firework, map cloning, armour
                                     dye, banner duplicate, repair). They need
                                     bespoke assemble() logic, not table data.
    smelting/blasting/smoking/campfire_cooking/stonecutting/smithing_*
                                   — those belong to blocks we have no menu for.

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


def collect():
    tags = load_item_tags()
    collector = Collector()
    recipes = []
    skipped = {}

    for name in sorted(os.listdir(RECIPE_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(RECIPE_DIR, name)) as f:
            data = json.load(f)

        kind = data.get("type", "")
        if kind not in CRAFTING_TYPES:
            skipped[kind] = skipped.get(kind, 0) + 1
            continue

        result = data.get("result", {})
        result_slug = strip_ns(result.get("id", ""))
        if not result_slug:
            continue
        result_count = int(result.get("count", 1))
        recipe_id = "minecraft:" + name[:-len(".json")]

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

    return collector, recipes, skipped


# ── Emit ────────────────────────────────────────────────────────────────────

HEADER = """// File: src/common/world/crafting/GeneratedRecipeList.{ext}
// AUTO-GENERATED by tools/gen_recipes.py — DO NOT EDIT BY HAND.
// Source: data/minecraft/recipe/*.json + data/minecraft/tags/item/*.json
// (the vendored vanilla data pack). Re-run the generator after bumping the MC
// snapshot; see CLAUDE.md, "Updating to a Newer Minecraft Version".
"""


def emit(collector, recipes, skipped):
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
        f.write("} // namespace Game\n")

    kinds = {0: "shaped", 1: "shapeless", 2: "transmute"}
    counts = {}
    for r in recipes:
        counts[kinds[r["kind"]]] = counts.get(kinds[r["kind"]], 0) + 1
    print("Emitted %d recipes (%s)" % (
        len(recipes), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    print("  %d interned ingredients, %d slug entries, %d cells"
          % (len(windows), len(slug_pool), len(cell_pool)))
    if skipped:
        print("  skipped non-crafting types:")
        for k, v in sorted(skipped.items(), key=lambda kv: -kv[1]):
            print("    %-40s %d" % (k, v))


if __name__ == "__main__":
    emit(*collect())
