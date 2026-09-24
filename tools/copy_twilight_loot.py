#!/usr/bin/env python3
# tools/copy_twilight_loot.py
#
# Twilight Forest chest loot tables (pass two), copied from the mod's generated
# data under mods_reference/twilightforest (git-ignored clone) into
# data/twilightforest/loot_table/, keeping the mod's ids: "twilightforest:hill_1"
# -> data/twilightforest/loot_table/hill_1.json, "twilightforest:chests/basement"
# -> data/twilightforest/loot_table/chests/basement.json. The engine's runtime
# chest filler (src/common/world/loot/ChestLootTables.cpp) resolves a chest's
# LootTable id there and looks items up by slug (namespace stripped).
#
# Item mapping: a TF item the engine does not have is rewritten to the engine
# item closest to it (MAPPED below; the mod's mangrove set is tf_mangrove_* in
# the engine). TF items with no fair equivalent (charms, magic maps, trophies,
# boss drops, music discs, TF weapons and armour, ...) are left as they are:
# the chest filler drops an entry whose item the build lacks, so those rolls
# come up empty rather than handing out an unrelated item.
#
#   python3 tools/copy_twilight_loot.py           # copy + map
#   python3 tools/copy_twilight_loot.py --check   # compare with what is on disk, exit 1 on drift
#   python3 tools/copy_twilight_loot.py --report  # list the TF items left unmapped

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "mods_reference/twilightforest/src/generated/resources/data/twilightforest/loot_table"
DEST = ROOT / "data/twilightforest/loot_table"
BLOCK_DEFS = ROOT / "src/common/world/block/BlockDefs.inc"
ITEM_LIST = ROOT / "src/common/entity/GeneratedItemList.cpp"

# Chest tables only: the mod's root-level structure/feature tables and chests/.
# (blocks/, entities/ and items/ are not chest loot.)
SUBDIRS = ["", "chests"]

# TF item -> engine item (both without namespace).
MAPPED = {
    # same item under the engine's name
    "mangrove_sapling": "tf_mangrove_sapling",
    # nearest vanilla equivalents
    "canopy_bookshelf": "bookshelf",
    "pork_jerky": "cooked_porkchop",
    "beef_jerky": "cooked_beef",
    "chicken_jerky": "cooked_chicken",
    "mutton_jerky": "cooked_mutton",
    "rabbit_jerky": "cooked_rabbit",
    "venison_jerky": "cooked_beef",
    "tanned_leather": "leather",
    "raven_feather": "feather",
    "arctic_fur": "white_wool",
    "gelatinous_slime_drop": "slime_ball",
    "hollow_oak_sapling": "twilight_oak_sapling",
    "time_sapling": "twilight_oak_sapling",
    "transformation_sapling": "twilight_oak_sapling",
    "mining_sapling": "twilight_oak_sapling",
    "sorting_sapling": "twilight_oak_sapling",
    "ironwood_block": "iron_block",
    "trollsteinn": "stone",
    "uberous_soil": "farmland",
}


def engine_slugs() -> set[str]:
    slugs: set[str] = set()
    for m in re.finditer(r'BLOCK_DEF\(\w+,\s*"([a-z0-9_]+)"', BLOCK_DEFS.read_text()):
        slugs.add(m.group(1))
    for m in re.finditer(r'\{\s*"([a-z0-9_]+)",', ITEM_LIST.read_text()):
        slugs.add(m.group(1))
    return slugs


def map_items(node, slugs: set[str], unmapped: set[str]):
    if isinstance(node, dict):
        out = {}
        for key, value in node.items():
            out[key] = map_items(value, slugs, unmapped)
        if out.get("type") in ("minecraft:item", "item") and isinstance(out.get("name"), str):
            ns, _, path = out["name"].partition(":")
            if not path:
                ns, path = "minecraft", ns
            if path not in slugs:
                target = MAPPED.get(path)
                if target is not None and target in slugs:
                    out["name"] = "minecraft:" + target
                else:
                    unmapped.add(f"{ns}:{path}")
        return out
    if isinstance(node, list):
        return [map_items(value, slugs, unmapped) for value in node]
    return node


def build() -> tuple[dict[Path, str], set[str]]:
    slugs = engine_slugs()
    outputs: dict[Path, str] = {}
    unmapped: set[str] = set()
    for sub in SUBDIRS:
        directory = SOURCE / sub if sub else SOURCE
        for path in sorted(directory.glob("*.json")):
            table = json.loads(path.read_text())
            mapped = map_items(table, slugs, unmapped)
            rel = Path(sub) / path.name if sub else Path(path.name)
            outputs[DEST / rel] = json.dumps(mapped, indent=2) + "\n"
    return outputs, unmapped


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    if not SOURCE.is_dir():
        print(f"missing {SOURCE}", file=sys.stderr)
        return 1
    outputs, unmapped = build()
    if args.report:
        for item in sorted(unmapped):
            print(item)
        return 0
    drift = 0
    for path, text in outputs.items():
        if args.check:
            if not path.exists() or path.read_text() != text:
                print(f"drift: {path.relative_to(ROOT)}")
                drift += 1
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    if args.check:
        return 1 if drift else 0
    print(f"wrote {len(outputs)} loot tables; {len(unmapped)} TF items left unmapped (--report lists them)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
