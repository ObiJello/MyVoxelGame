#!/usr/bin/env python3
# tools/gen_items.py
#
# One-shot generator that mirrors MC's `Items.java` registration order into
# C++ tables. Run by hand whenever new items are added to the upstream
# `minecraft_code_26.1-snapshot-1/.../Items.java`.
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
ITEMS_JAVA     = REPO_ROOT / "minecraft_code_26.1-snapshot-1" / "decompiled_net" / "minecraft" / "world" / "item" / "Items.java"
# 26.3's Items.java (minecraft_code_26.3-pre-2). Read for an ALLOWLIST of items only:
# every slug here is appended after the 26.1 set, in 26.3 declaration order,
# the moment it is not already in the table. The whole tree is not merged
# because each row is a wire/save id and most 26.3 additions (sulfur blocks,
# the bucket) have nothing behind them in this engine yet.
ITEMS_JAVA2    = REPO_ROOT / "minecraft_code_26.3-pre-2" / "decompiled_net" / "minecraft" / "world" / "item" / "Items.java"
MC2_ITEMS      = {
    "sulfur_cube_spawn_egg",
    # 26.2/26.3 items imported 2026-09-05 (assets copied from the jar; the
    # maps, boats, disc and bucket have no behaviour behind them yet).
    "abandoned_camp_map", "buried_ancient_city_map", "buried_mineshaft_map",
    "buried_treasure_map", "buried_trial_chambers_map", "desert_pyramid_map",
    "desert_village_map", "jungle_pyramid_map", "music_disc_bounce",
    "ocean_monument_map", "plains_village_map", "poplar_boat", "poplar_chest_boat",
    "savanna_village_map", "snowy_village_map", "sulfur_cube_bucket",
    "swamp_hut_map", "taiga_village_map", "warm_ocean_ruins_map",
    "woodland_mansion_map",
}
# ── Engine-only items ───────────────────────────────────────────────────────
# Items this engine has that no Items.java does (The Hush, docs/the-hush.md).
# They are appended AFTER every MC row, in this order, and become wire/save
# ids like any other row — so this list is append-only too. Each carries its
# own max stack size: merge_append_only keeps an unknown slug's slot but
# resets it to 64, which would make every resonite tool stack. Listing them
# here is what lets a regeneration keep the rows exact.
#
# Rarity, tool tiers and attack attributes are NOT here: they are wired in
# ItemBehaviors.cpp / GeneratedItemAttributes.cpp like every vanilla item's.
ENGINE_ONLY_ITEMS: list[tuple[str, int]] = [
    ("raw_resonite",      64),
    ("resonite_ingot",    64),
    ("resonite_sword",     1),
    ("resonite_pickaxe",   1),
    ("resonite_axe",       1),
    ("resonite_shovel",    1),
    ("resonite_hoe",       1),
    ("resonant_heart",     1),
    ("echo_blade",         1),
    # Twilight Forest + The Aether, pass one (docs/mod-ports.md): the drops
    # of the pass-one blocks. TFItems / AetherItems register all five with
    # the default stack of 64.
    ("torchberries",      64),   # TF torchberry_plant's drop
    ("liveroot",          64),   # TF liveroot_block's drop
    ("ambrosium_shard",   64),   # Aether ambrosium_ore's drop
    ("zanite_gemstone",   64),   # Aether zanite_ore's drop
    ("blue_berry",        64),   # Aether berry_bush's drop (AetherFoods.BLUE_BERRY)
    # ── The Aether, pass two (docs/mod-ports.md). Stack sizes from
    # AetherItems: 64 by default, stacksTo(16) on the empty skyroot bucket,
    # stacksTo(1) on the filled ones and the dungeon keys, 1 for every tool
    # and armour piece (durability implies stacksTo(1)).
    ("skyroot_stick",     64),
    ("golden_amber",      64),
    ("swet_ball",         64),
    ("aechor_petal",      64),
    ("enchanted_berry",   64),
    ("white_apple",       64),
    ("blue_gummy_swet",   64),
    ("golden_gummy_swet", 64),
    ("skyroot_bucket",    16),
    ("skyroot_water_bucket", 1),
    ("skyroot_milk_bucket", 1),
    ("bronze_dungeon_key", 1),
    ("silver_dungeon_key", 1),
    ("gold_dungeon_key",  1),
    ("skyroot_sword",     1),
    ("skyroot_pickaxe",   1),
    ("skyroot_axe",       1),
    ("skyroot_shovel",    1),
    ("skyroot_hoe",       1),
    ("holystone_sword",   1),
    ("holystone_pickaxe", 1),
    ("holystone_axe",     1),
    ("holystone_shovel",  1),
    ("holystone_hoe",     1),
    ("zanite_sword",      1),
    ("zanite_pickaxe",    1),
    ("zanite_axe",        1),
    ("zanite_shovel",     1),
    ("zanite_hoe",        1),
    ("gravitite_sword",   1),
    ("gravitite_pickaxe", 1),
    ("gravitite_axe",     1),
    ("gravitite_shovel",  1),
    ("gravitite_hoe",     1),
    ("zanite_helmet",     1),
    ("zanite_chestplate", 1),
    ("zanite_leggings",   1),
    ("zanite_boots",      1),
    ("gravitite_helmet",  1),
    ("gravitite_chestplate", 1),
    ("gravitite_leggings", 1),
    ("gravitite_boots",   1),
    # ── Twilight Forest, pass two: the four material sets (TFItems; tools
    # and armour stack to 1), naga scale + armour, venison and meef.
    ("raw_ironwood",      64),
    ("ironwood_ingot",    64),
    ("ironwood_helmet",   1),
    ("ironwood_chestplate", 1),
    ("ironwood_leggings", 1),
    ("ironwood_boots",    1),
    ("ironwood_sword",    1),
    ("ironwood_shovel",   1),
    ("ironwood_pickaxe",  1),
    ("ironwood_axe",      1),
    ("ironwood_hoe",      1),
    ("steeleaf_ingot",    64),
    ("steeleaf_helmet",   1),
    ("steeleaf_chestplate", 1),
    ("steeleaf_leggings", 1),
    ("steeleaf_boots",    1),
    ("steeleaf_sword",    1),
    ("steeleaf_shovel",   1),
    ("steeleaf_pickaxe",  1),
    ("steeleaf_axe",      1),
    ("steeleaf_hoe",      1),
    ("armor_shard",       64),
    ("armor_shard_cluster", 64),
    ("knightmetal_ingot", 64),
    ("knightmetal_helmet", 1),
    ("knightmetal_chestplate", 1),
    ("knightmetal_leggings", 1),
    ("knightmetal_boots", 1),
    ("knightmetal_sword", 1),
    ("knightmetal_pickaxe", 1),
    ("knightmetal_axe",   1),
    ("fiery_blood",       64),
    ("fiery_tears",       64),
    ("fiery_ingot",       64),
    ("fiery_helmet",      1),
    ("fiery_chestplate",  1),
    ("fiery_leggings",    1),
    ("fiery_boots",       1),
    ("fiery_sword",       1),
    ("fiery_pickaxe",     1),
    ("naga_scale",        64),
    ("naga_chestplate",   1),
    ("naga_leggings",     1),
    ("raw_venison",       64),
    ("cooked_venison",    64),
    ("raw_meef",          64),
    ("cooked_meef",       64),
    # ── The Hush, the Choir Mother's drop (2026-09-22, docs/the-hush.md).
    # A boss trophy: stack 1, EPIC (ItemBehaviors.cpp).
    ("choir_heart",        1),
    # ── The Hush, the tools of the deep (2026-09-22, docs/the-hush.md;
    # behaviour in ItemBehaviors.cpp + server/items/HushItems). Every tool
    # stacks to 1 (the bow and the cloak are durable in MC terms); the fruit
    # is a food and stacks like sweet berries.
    ("tuning_fork",        1),
    ("echo_compass",       1),
    ("cloak_of_silence",   1),
    ("resonance_bow",      1),
    ("recall_chime",       1),
    ("whisperfruit",      64),
    # ── Spawn eggs for every engine-only mob (2026-09-22): The Hush, then
    # the Twilight Forest, then the Aether — tools/gen_engine_spawn_eggs.py
    # holds the same list with each egg's colours. Stack 64 like MC's eggs.
    ("echo_wraith_spawn_egg", 64),
    ("hushling_spawn_egg", 64),
    ("silent_warden_spawn_egg", 64),
    ("choir_mother_spawn_egg", 64),
    ("crystal_golem_spawn_egg", 64),
    ("echo_mimic_spawn_egg", 64),
    ("hush_leviathan_spawn_egg", 64),
    ("lumen_moth_spawn_egg", 64),
    ("bighorn_sheep_spawn_egg", 64),
    ("boar_spawn_egg", 64),
    ("deer_spawn_egg", 64),
    ("kobold_spawn_egg", 64),
    ("redcap_spawn_egg", 64),
    ("tiny_bird_spawn_egg", 64),
    ("block_and_chain_goblin_spawn_egg", 64),
    ("dwarf_rabbit_spawn_egg", 64),
    ("fire_beetle_spawn_egg", 64),
    ("hedge_spider_spawn_egg", 64),
    ("helmet_crab_spawn_egg", 64),
    ("hostile_wolf_spawn_egg", 64),
    ("king_spider_spawn_egg", 64),
    ("lower_goblin_knight_spawn_egg", 64),
    ("maze_slime_spawn_egg", 64),
    ("minotaur_spawn_egg", 64),
    ("mist_wolf_spawn_egg", 64),
    ("mosquito_swarm_spawn_egg", 64),
    ("penguin_spawn_egg", 64),
    ("pinch_beetle_spawn_egg", 64),
    ("raven_spawn_egg", 64),
    ("redcap_sapper_spawn_egg", 64),
    ("skeleton_druid_spawn_egg", 64),
    ("slime_beetle_spawn_egg", 64),
    ("squirrel_spawn_egg", 64),
    ("swarm_spider_spawn_egg", 64),
    ("towerwood_borer_spawn_egg", 64),
    ("troll_spawn_egg", 64),
    ("upper_goblin_knight_spawn_egg", 64),
    ("winter_wolf_spawn_egg", 64),
    ("wraith_spawn_egg", 64),
    ("yeti_spawn_egg", 64),
    ("aechor_plant_spawn_egg", 64),
    ("aerbunny_spawn_egg", 64),
    ("aerwhale_spawn_egg", 64),
    ("cockatrice_spawn_egg", 64),
    ("fire_minion_spawn_egg", 64),
    ("flying_cow_spawn_egg", 64),
    ("mimic_spawn_egg", 64),
    ("moa_spawn_egg", 64),
    ("phyg_spawn_egg", 64),
    ("sentry_spawn_egg", 64),
    ("sheepuff_spawn_egg", 64),
    ("blue_swet_spawn_egg", 64),
    ("golden_swet_spawn_egg", 64),
    ("whirlwind_spawn_egg", 64),
    ("evil_whirlwind_spawn_egg", 64),
    ("valkyrie_spawn_egg", 64),
    ("zephyr_spawn_egg", 64),
    # Aurelith's boss, The Unsung (2026-09-22, docs/the-hush.md) — its egg,
    # for testing the fight away from the city (gen_engine_spawn_eggs.py).
    ("the_unsung_spawn_egg", 64),
    # ── Aurelith, reawakening the Heart (2026-09-22, docs/the-hush.md;
    # world/block/AurelithQuestBlocks, server/level/AurelithCities). The four
    # voice keys a city hides (one per voice: the Archive, the Hall of
    # Instruments, the Tuners' Works, the Vault) and the Podium takes; the
    # Held Note, the reward for singing the Heart awake (a hold-to-sound
    # item, ItemBehaviors.cpp + server/items/AurelithItems). All stack to 1.
    ("soprano_voice_key",  1),
    ("alto_voice_key",     1),
    ("tenor_voice_key",    1),
    ("bass_voice_key",     1),
    ("held_note",          1),
]

# 26.3 registers eggs as `registerSpawnEgg(ItemIds.X_SPAWN_EGG, EntityTypes.X)`.
RE_SPAWN_EGG2 = re.compile(
    r"""^\s+
        ([A-Z][A-Z0-9_]*)            # symbol name (group 1)
        \s*=\s*
        registerSpawnEgg\(
        \s*ItemIds\.[A-Z0-9_]+\s*,\s*EntityTypes\.([A-Z][A-Z0-9_]*)   # entity (group 2)
    """,
    re.VERBOSE,
)
RE_PURE2 = re.compile(
    r"""^\s+
        ([A-Z][A-Z0-9_]*)            # symbol name (group 1)
        \s*=\s*
        (?:Items\.)?registerItem\(
        \s*ItemIds\.([A-Z0-9_]+)     # ItemIds constant (group 2) — slug is its lower case
    """,
    re.VERBOSE,
)
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
    out += parse_items_java2(seen)
    out += engine_only_items(seen)
    return out, blocks


def engine_only_items(seen: set[str]) -> list[tuple[str, str, str, int]]:
    """ENGINE_ONLY_ITEMS as table rows. The symbol is the slug upper-cased so
    emit_hpp pascal-cases it the same way it does an MC symbol
    (RAW_RESONITE -> RawResonite)."""
    out: list[tuple[str, str, str, int]] = []
    for slug, max_stack in ENGINE_ONLY_ITEMS:
        symbol = slug.upper()
        if symbol in seen:
            raise SystemExit(f"error: engine-only item '{slug}' collides with an MC item")
        seen.add(symbol)
        out.append((symbol, slug, detect_predicate(slug), max_stack))
    return out


def parse_items_java2(seen: set[str]) -> list[tuple[str, str, str, int]]:
    """The MC2_ITEMS allowlist out of 26.3's Items.java, in its order."""
    out: list[tuple[str, str, str, int]] = []
    if not ITEMS_JAVA2.exists():
        return out
    with ITEMS_JAVA2.open(encoding="utf-8") as f:
        for line in f:
            m = RE_SPAWN_EGG2.match(line)
            if m:
                symbol, slug = m.group(1), m.group(2).lower() + "_spawn_egg"
            else:
                m = RE_PURE2.match(line)
                if not m:
                    continue
                symbol, slug = m.group(1), m.group(2).lower()
            if slug not in MC2_ITEMS or symbol in seen:
                continue
            seen.add(symbol)
            out.append((symbol, slug, detect_predicate(slug), max_stack_size(line)))
    return out


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
            # The symbol stays UPPER_SNAKE here: emit_hpp pascal-cases every
            # symbol, and pascal-casing "MelonSeeds" again gives "Melonseeds".
            out.append((slug.upper(), slug, "none", DEFAULT_MAX_STACK))
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
