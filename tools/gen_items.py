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


def parse_items_java() -> list[tuple[str, str, str]]:
    """Returns [(symbol, slug, predicateHint), ...] in declaration order. Handles both
    direct `registerItem("slug", ...)` and the spawn-egg helper
    `registerSpawnEgg(EntityType.X)` which MC computes at runtime as `<x>_spawn_egg`."""
    out: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    with ITEMS_JAVA.open(encoding="utf-8") as f:
        for line in f:
            m = RE_PURE.match(line)
            if m:
                symbol, slug = m.group(1), m.group(2)
                if symbol in seen:
                    continue
                seen.add(symbol)
                out.append((symbol, slug, detect_predicate(slug)))
                continue
            m = RE_SPAWN_EGG.match(line)
            if m:
                symbol, entity = m.group(1), m.group(2)
                slug = entity.lower() + "_spawn_egg"
                if symbol in seen:
                    continue
                seen.add(symbol)
                out.append((symbol, slug, detect_predicate(slug)))
    return out


def read_existing_slugs() -> list[str]:
    """Read the existing GeneratedItemList.cpp and return slugs in their current
    table order. Used to preserve numeric IDs across regenerations — append-only
    is mandatory because IDs are wire/save stable (see CLAUDE.md)."""
    if not CPP_OUT.exists():
        return []
    pattern = re.compile(r'^\s*\{\s*"([a-z0-9_]+)"\s*,')
    out: list[str] = []
    with CPP_OUT.open(encoding="utf-8") as f:
        for line in f:
            m = pattern.match(line)
            if m:
                out.append(m.group(1))
    return out


def merge_append_only(parsed: list[tuple[str, str, str]]) -> list[tuple[str, str, str]]:
    """Reorder `parsed` so that any slug already present in the existing CPP file
    keeps its existing index, and any new slug is appended at the end (in MC's
    declaration order). Existing slugs not found in the parsed set stay where
    they were — they may be MC items that were renamed/removed, but keeping the
    slot prevents IDs from shifting."""
    existing = read_existing_slugs()
    if not existing:
        return parsed
    by_slug = {slug: (sym, slug, pred) for sym, slug, pred in parsed}
    used: set[str] = set()
    out: list[tuple[str, str, str]] = []
    # Pass 1: keep existing slots in order.
    for slug in existing:
        entry = by_slug.get(slug)
        if entry is None:
            # Slug no longer exists in MC source — keep the slot with a placeholder
            # so later IDs don't shift. Use the same slug + "none" hint; the JSON/
            # texture files for it may also be gone, in which case the item just
            # renders as missingno (no crash).
            out.append((pascal_case_from_upper_snake(slug.upper()), slug, "none"))
        else:
            out.append(entry)
        used.add(slug)
    # Pass 2: append everything new at the end, in MC's declaration order.
    for entry in parsed:
        if entry[1] not in used:
            out.append(entry)
            used.add(entry[1])
    return out


def emit_hpp(items: list[tuple[str, str, str]]) -> str:
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
    for i, (symbol, slug, _pred) in enumerate(items):
        name = pascal_case_from_upper_snake(symbol)
        lines.append(f"    static constexpr ItemID {name:<32} = PURE_ITEM_BASE + {i:4d}; // \"{slug}\"")
    lines.append("")
    lines.append("} // namespace Game::Items")
    lines.append("")
    return "\n".join(lines)


def emit_cpp(items: list[tuple[str, str, str]]) -> str:
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
    for symbol, slug, pred in items:
        lines.append(f'        {{ "{slug}", "{pred}" }},')
    lines.append("    };")
    lines.append(f"    const size_t kPureItemTableSize = sizeof(kPureItemTable) / sizeof(kPureItemTable[0]);")
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

    items = parse_items_java()
    if not items:
        print("error: parsed zero items from Items.java — check the regex.", file=sys.stderr)
        return 1
    # Append-only ordering: preserve existing IDs, only append new slugs at the end.
    items = merge_append_only(items)

    HPP_OUT.write_text(emit_hpp(items), encoding="utf-8")
    CPP_OUT.write_text(emit_cpp(items), encoding="utf-8")

    pred_counts: dict[str, int] = {}
    for _, _, p in items:
        pred_counts[p] = pred_counts.get(p, 0) + 1
    print(f"Generated {len(items)} pure items.")
    print(f"  -> {HPP_OUT.relative_to(REPO_ROOT)}")
    print(f"  -> {CPP_OUT.relative_to(REPO_ROOT)}")
    print(f"Predicate breakdown: {pred_counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
