#!/usr/bin/env python3
# tools/gen_items.py
#
# One-shot generator that mirrors MC's `Items.java` registration order into
# C++ tables. Run by hand whenever new items are added to the upstream
# `minecraft_code/.../Items.java`.
#
# Outputs (overwritten):
#   src/common/entity/GeneratedItemList.hpp  -- `Game::Items::Foo` constants
#   src/common/entity/GeneratedItemList.cpp  -- kPureItemTable[] entries
#
# Append-only: re-running the script after MC adds an item should ONLY add new
# entries at the end. Reordering or removing entries shifts every subsequent
# numeric ID and breaks network/save compatibility.

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT      = Path(__file__).resolve().parent.parent
ITEMS_JAVA     = REPO_ROOT / "minecraft_code" / "decompiled_net" / "minecraft" / "world" / "item" / "Items.java"
MODELS_DIR     = REPO_ROOT / "assets" / "models" / "item"
HPP_OUT        = REPO_ROOT / "src" / "common" / "entity" / "GeneratedItemList.hpp"
CPP_OUT        = REPO_ROOT / "src" / "common" / "entity" / "GeneratedItemList.cpp"
BLOCK_DEFS     = REPO_ROOT / "src" / "common" / "world" / "block" / "BlockDefs.inc"

# Match `   FOO = registerItem("slug" ...` and `   FOO = Items.registerItem("slug" ...`.
# Captures the symbol name + slug. We do NOT match `registerBlock(...)` — those are
# block items which our existing BlockID loop already covers.
RE_PURE = re.compile(
    r"""^\s+
        ([A-Z][A-Z0-9_]*)            # symbol name (group 1)
        \s*=\s*
        (?:Items\.)?registerItem\(
        \s*"([a-z0-9_]+)"            # slug (group 2)
    """,
    re.VERBOSE,
)

# Match `   FOO_SPAWN_EGG = registerSpawnEgg(EntityType.FOO)` — the slug is computed
# at runtime by MC (entity key + "_spawn_egg") so we derive it ourselves from the
# EntityType.X token.
RE_SPAWN_EGG = re.compile(
    r"""^\s+
        ([A-Z][A-Z0-9_]*)            # symbol name (group 1)
        \s*=\s*
        registerSpawnEgg\(
        \s*EntityType\.([A-Z][A-Z0-9_]*)   # entity type name (group 2)
    """,
    re.VERBOSE,
)

# Match `   FOO = registerBlock(Blocks.FOO, ...` — the BLOCK items. Our BlockID
# loop registers these, so they get no kPureItemTable row, but their max stack
# size still has to come from here (74 of them are not 64). MC derives the item
# id from the block's id, so the slug is just the Blocks.X token lowercased.
RE_BLOCK = re.compile(
    r"""^\s+
        ([A-Z][A-Z0-9_]*)            # symbol name (group 1)
        \s*=\s*
        (?:Items\.)?registerBlock\(
        \s*Blocks\.([A-Z][A-Z0-9_]*) # Blocks.X token (group 2)
    """,
    re.VERBOSE,
)

# ── Max stack size ──────────────────────────────────────────────────────────
# MC keeps it in the MAX_STACK_SIZE data component, defaulted to 64 for every
# item by DataComponents.COMMON_ITEM_COMPONENTS (DataComponents.java:220).
# Grepping for `stacksTo(` alone is NOT enough: no tool or armour piece writes
# it. They go through `.pickaxe(ToolMaterial.DIAMOND, …)` → ToolMaterial
# .applyCommonProperties (ToolMaterial.java:29-31) → `.durability(N)`, and
# Item.Properties.durability (Item.java:426-431) sets MAX_STACK_SIZE to 1
# alongside MAX_DAMAGE. Item.Properties.buildAndValidateComponents
# (Item.java:569) then makes "durable AND stackable" a hard error, which is why
# an implied 1 can never be contradicted later.
#
# Matching the builder call by name is not enough either. Swords and pickaxes
# use `.sword(ToolMaterial.X, …)` / `.pickaxe(…)` on the Properties, but axes,
# shovels, hoes and spears push the same work into their ITEM CLASS —
#   WOODEN_AXE = registerItem("wooden_axe", (p) -> new AxeItem(ToolMaterial.WOOD, …))
# — so there is no `.axe(` on the line at all. What every durable item DOES
# carry is a material: each public ToolMaterial helper (applyToolProperties,
# applySwordProperties, …) opens with `this.applyCommonProperties(properties)`
# (ToolMaterial.java:29-31), which is `.durability(...)`. Keying off the
# material rather than the call site catches all four families and survives MC
# adding another tool class.
DEFAULT_MAX_STACK = 64
RE_STACKS_TO = re.compile(r"\.stacksTo\((\d+)\)")
RE_IMPLIES_SINGLE = re.compile(
    r"\.durability\("                    # the direct call (bow, fishing rod, shield, …)
    r"|ToolMaterial\."                   # → applyCommonProperties → .durability
    r"|ArmorMaterials\."                 # → armour, same shape
    r"|\.(?:humanoidArmor|wolfArmor|horseArmor|nautilusArmor)\("
)


def max_stack_size(line: str) -> int:
    """MC's Properties builder chain, reduced to the part that moves the number."""
    explicit = RE_STACKS_TO.findall(line)
    if explicit:
        return int(explicit[-1])       # last call wins, as with any builder
    if RE_IMPLIES_SINGLE.search(line):
        return 1
    return DEFAULT_MAX_STACK


def pascal_case_from_upper_snake(s: str) -> str:
    """IRON_PICKAXE -> IronPickaxe; TNT -> Tnt (acceptable; rare)."""
    return "".join(part.capitalize() for part in s.split("_"))


def detect_predicate(slug: str) -> str:
    """Open assets/models/item/<slug>.json and find which predicate name (if any)
    its overrides[] uses. Returns "angle", "time", "pull", "pulling", "cast",
    "blocking", "throwing", "damaged", "damage", "charge", or "none"."""
    path = MODELS_DIR / f"{slug}.json"
    if not path.exists():
        return "none"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return "none"
    overrides = data.get("overrides")
    if not isinstance(overrides, list) or not overrides:
        return "none"
    # Use the first override's first predicate key — vanilla item models always
    # use a single predicate per file (compass uses "angle", clock "time" etc.).
    for ov in overrides:
        pred = ov.get("predicate") if isinstance(ov, dict) else None
        if isinstance(pred, dict):
            for k in pred.keys():
                return k
    return "none"


def parse_items_java() -> tuple[list[tuple[str, str, str, int]], list[tuple[str, int]]]:
    """Returns ([(symbol, slug, predicateHint, maxStack), ...] in declaration order,
    [(blockSlug, maxStack), ...] for block items that are not the default 64).

    Handles direct `registerItem("slug", ...)`, the spawn-egg helper
    `registerSpawnEgg(EntityType.X)` which MC computes at runtime as
    `<x>_spawn_egg`, and `registerBlock(Blocks.X, ...)` for the block items."""
    out: list[tuple[str, str, str, int]] = []
    blocks: list[tuple[str, int]] = []
    seen: set[str] = set()
    with ITEMS_JAVA.open(encoding="utf-8") as f:
        for line in f:
            m = RE_PURE.match(line)
            if m:
                symbol, slug = m.group(1), m.group(2)
                if symbol in seen:
                    continue
                seen.add(symbol)
                out.append((symbol, slug, detect_predicate(slug), max_stack_size(line)))
                continue
            m = RE_SPAWN_EGG.match(line)
            if m:
                symbol, entity = m.group(1), m.group(2)
                slug = entity.lower() + "_spawn_egg"
                if symbol in seen:
                    continue
                seen.add(symbol)
                out.append((symbol, slug, detect_predicate(slug), max_stack_size(line)))
                continue
            m = RE_BLOCK.match(line)
            if m:
                size = max_stack_size(line)
                if size != DEFAULT_MAX_STACK:
                    blocks.append((m.group(2).lower(), size))
    return out, blocks


def known_block_slugs() -> set[str]:
    """Column 2 of BlockDefs.inc — the engine's registered block slugs. Used to
    warn about block items we'd emit a stack size for but cannot resolve."""
    if not BLOCK_DEFS.exists():
        return set()
    pattern = re.compile(r'^BLOCK_DEF\(\w+,\s*"([a-z0-9_]+)"')
    out: set[str] = set()
    with BLOCK_DEFS.open(encoding="utf-8") as f:
        for line in f:
            m = pattern.match(line)
            if m:
                out.add(m.group(1))
    return out


def read_existing_slugs() -> list[str]:
    """Read the existing GeneratedItemList.cpp and return slugs in their current
    table order. Used to preserve numeric IDs across regenerations — append-only
    is mandatory because IDs are wire/save stable (see CLAUDE.md)."""
    if not CPP_OUT.exists():
        return []
    # Scope the scan to the kPureItemTable braces. The file also holds
    # kBlockItemStackSize[], whose rows are `{ "shulker_box", 1 },` — the same
    # shape — so an unscoped scan would read those 74 block slugs back as pure
    # items and append them to the table on the next run.
    pattern = re.compile(r'^\s*\{\s*"([a-z0-9_]+)"\s*,')
    out: list[str] = []
    inside = False
    with CPP_OUT.open(encoding="utf-8") as f:
        for line in f:
            if not inside:
                if "kPureItemTable[] = {" in line:
                    inside = True
                continue
            if line.strip().startswith("};"):
                break
            m = pattern.match(line)
            if m:
                out.append(m.group(1))
    return out


def merge_append_only(parsed: list[tuple[str, str, str, int]]) -> list[tuple[str, str, str, int]]:
    """Reorder `parsed` so that any slug already present in the existing CPP file
    keeps its existing index, and any new slug is appended at the end (in MC's
    declaration order). Existing slugs not found in the parsed set stay where
    they were — they may be MC items that were renamed/removed, but keeping the
    slot prevents IDs from shifting."""
    existing = read_existing_slugs()
    if not existing:
        return parsed
    by_slug = {slug: entry for entry in parsed for slug in (entry[1],)}
    used: set[str] = set()
    out: list[tuple[str, str, str, int]] = []
    # Pass 1: keep existing slots in order.
    for slug in existing:
        entry = by_slug.get(slug)
        if entry is None:
            # Slug no longer exists in MC source — keep the slot with a placeholder
            # so later IDs don't shift. Use the same slug + "none" hint; the JSON/
            # texture files for it may also be gone, in which case the item just
            # renders as missingno (no crash).
            out.append((pascal_case_from_upper_snake(slug.upper()), slug, "none",
                        DEFAULT_MAX_STACK))
        else:
            out.append(entry)
        used.add(slug)
    # Pass 2: append everything new at the end, in MC's declaration order.
    for entry in parsed:
        if entry[1] not in used:
            out.append(entry)
            used.add(entry[1])
    return out


def emit_hpp(items: list[tuple[str, str, str, int]]) -> str:
    lines = [
        "// File: src/common/entity/GeneratedItemList.hpp",
        "// AUTO-GENERATED by tools/gen_items.py — DO NOT EDIT BY HAND.",
        "// Append-only: never reorder or delete entries (numeric IDs would shift).",
        "#pragma once",
        "",
        "#include \"Item.hpp\"",
        "",
        "namespace Game::Items {",
        "",
        "    // Each constant is PURE_ITEM_BASE + (declaration index in MC's Items.java).",
    ]
    for i, (symbol, slug, _pred, _max) in enumerate(items):
        name = pascal_case_from_upper_snake(symbol)
        lines.append(f"    static constexpr ItemID {name:<32} = PURE_ITEM_BASE + {i:4d}; // \"{slug}\"")
    lines.append("")
    lines.append("} // namespace Game::Items")
    lines.append("")
    return "\n".join(lines)


def emit_cpp(items: list[tuple[str, str, str, int]],
             block_stacks: list[tuple[str, int]]) -> str:
    lines = [
        "// File: src/common/entity/GeneratedItemList.cpp",
        "// AUTO-GENERATED by tools/gen_items.py — DO NOT EDIT BY HAND.",
        "// Append-only: never reorder or delete entries (numeric IDs would shift).",
        "#include \"GeneratedItemList.hpp\"",
        "",
        "namespace Game {",
        "",
        f"    // {len(items)} items, in MC's Items.java declaration order.",
        "    const PureItemTableEntry kPureItemTable[] = {",
    ]
    for symbol, slug, pred, maxstack in items:
        lines.append(f'        {{ "{slug}", "{pred}", {maxstack} }},')
    lines.append("    };")
    lines.append(f"    const size_t kPureItemTableSize = sizeof(kPureItemTable) / sizeof(kPureItemTable[0]);")
    lines.append("")
    lines.append("    // Block items whose max stack size is NOT the default 64. They have no")
    lines.append("    // kPureItemTable row (ItemRegistry builds one Item per BlockID instead),")
    lines.append("    // so this is the only place their limit can come from. Keyed by block")
    lines.append("    // registry slug — resolved against Block::registrySlug at startup.")
    lines.append("    const BlockItemStackSizeEntry kBlockItemStackSize[] = {")
    for slug, maxstack in block_stacks:
        lines.append(f'        {{ "{slug}", {maxstack} }},')
    lines.append("    };")
    lines.append("    const size_t kBlockItemStackSizeCount = "
                 "sizeof(kBlockItemStackSize) / sizeof(kBlockItemStackSize[0]);")
    lines.append("")
    lines.append("} // namespace Game")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    if not ITEMS_JAVA.exists():
        print(f"error: cannot find {ITEMS_JAVA}", file=sys.stderr)
        return 1
    if not MODELS_DIR.is_dir():
        print(f"error: cannot find {MODELS_DIR}", file=sys.stderr)
        return 1

    items, block_stacks = parse_items_java()
    if not items:
        print("error: parsed zero items from Items.java — check the regex.", file=sys.stderr)
        return 1
    # Append-only ordering: preserve existing IDs, only append new slugs at the end.
    items = merge_append_only(items)

    # A block item we can't resolve to a BlockID would silently keep 64. Say so
    # rather than emitting a row that never matches.
    known = known_block_slugs()
    if known:
        unknown = [s for s, _ in block_stacks if s not in known]
        if unknown:
            print(f"warning: {len(unknown)} block items have no BlockDefs.inc row "
                  f"(their stack size will not apply): {', '.join(sorted(unknown))}",
                  file=sys.stderr)

    HPP_OUT.write_text(emit_hpp(items), encoding="utf-8")
    CPP_OUT.write_text(emit_cpp(items, block_stacks), encoding="utf-8")

    pred_counts: dict[str, int] = {}
    pure_stacks: dict[int, int] = {}
    for _, _, p, m in items:
        pred_counts[p] = pred_counts.get(p, 0) + 1
        pure_stacks[m] = pure_stacks.get(m, 0) + 1
    block_counts: dict[int, int] = {}
    for _, m in block_stacks:
        block_counts[m] = block_counts.get(m, 0) + 1
    print(f"Generated {len(items)} pure items.")
    print(f"  -> {HPP_OUT.relative_to(REPO_ROOT)}")
    print(f"  -> {CPP_OUT.relative_to(REPO_ROOT)}")
    print(f"Predicate breakdown: {pred_counts}")
    print(f"Pure-item max stack sizes: {dict(sorted(pure_stacks.items()))}")
    print(f"Block items overriding the default 64: {len(block_stacks)} "
          f"{dict(sorted(block_counts.items()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
