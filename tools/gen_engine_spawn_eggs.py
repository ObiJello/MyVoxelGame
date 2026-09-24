#!/usr/bin/env python3
"""Spawn eggs for the mobs no Items.java registers: The Hush, the Twilight
Forest and the Aether.

Every egg is the classic two-layer spawn egg — a base and a spot overlay, each
tinted with a constant colour — which is exactly how both mods build theirs:
Twilight Forest's generated item definitions (twilightforest/items/*_spawn_egg
.json) carry two `minecraft:constant` tints over its spawn_egg_base /
spawn_egg_overlay, and the Aether's AetherItems registers DeferredSpawnEggItem
with a (base, spots) colour pair.

Art: Twilight Forest's two template sprites (CC BY-NC-SA, credited in
assets/ATTRIBUTION.md), shared by all three sets. The Aether's own egg art is
all-rights-reserved and is not used — only its colour pairs, which are code.

Colours:
  Twilight Forest  its generated items/<mob>_spawn_egg.json, read at run time
  Aether           AetherItems.java's DeferredSpawnEggItem pairs (below)
  The Hush         ours (docs/the-hush.md palette)

Writes, per egg:
  assets/items/<mob>_spawn_egg.json        the tinted two-layer definition
  assets/models/item/<mob>_spawn_egg.json  layer0 base, layer1 overlay
and once:
  assets/textures/item/mod_spawn_egg_{base,overlay}.png
  src/common/entity/EngineSpawnEggs.inc    kSpawnEggTable rows

The item rows themselves live in gen_items.py's ENGINE_ONLY_ITEMS (append-only
wire/save ids); run that after adding an egg here.

Usage: python3 tools/gen_engine_spawn_eggs.py [--check]
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TF_ITEMS = REPO / "mods_reference/twilightforest/src/generated/resources/assets/twilightforest/items"
TF_TEX = REPO / "mods_reference/twilightforest/src/main/resources/assets/twilightforest/textures/item"
ENTITY_TYPES = REPO / "src/common/entity/GeneratedEntityTypes.hpp"

BASE_TEX = "mod_spawn_egg_base"
OVERLAY_TEX = "mod_spawn_egg_overlay"

# The Hush — deep teal and sculk cyan, each mob's accent from its look.
HUSH = [
    ("echo_wraith",    0x0E3A3F, 0x2BD4C0),
    ("hushling",       0x2C393B, 0x7FEDE0),
    ("silent_warden",  0x0B1F24, 0x29DFEB),
    ("choir_mother",   0x1A2F3A, 0xB8F7F0),
    ("crystal_golem",  0x526467, 0x7FEDE0),
    ("echo_mimic",     0x2E6F67, 0x141B1D),
    ("hush_leviathan", 0x0E3A3F, 0x178C86),
    ("lumen_moth",     0x1F4A46, 0xE8FFB0),
    # Aurelith's boss: sculk-black robes, the stolen voices' violet light.
    ("the_unsung",     0x0A1418, 0xB77CFF),
]

# Twilight Forest mobs this port has. Colours come from the mod's generated
# item definitions; the upper goblin knight has none there (it only ever
# arrives riding a lower knight), so it borrows the lower knight's pair with
# the spots brightened — an egg exists so it can be picked and summoned.
TWILIGHT = [
    "bighorn_sheep", "boar", "deer", "kobold", "redcap", "tiny_bird",
    "block_and_chain_goblin", "dwarf_rabbit", "fire_beetle", "hedge_spider",
    "helmet_crab", "hostile_wolf", "king_spider", "lower_goblin_knight",
    "maze_slime", "minotaur", "mist_wolf", "mosquito_swarm", "penguin",
    "pinch_beetle", "raven", "redcap_sapper", "skeleton_druid", "slime_beetle",
    "squirrel", "swarm_spider", "towerwood_borer", "troll",
    "upper_goblin_knight", "winter_wolf", "wraith", "yeti",
]

# AetherItems.java: DeferredSpawnEggItem(type, base, spots).
AETHER = [
    ("aechor_plant",   0x076178, 0x4BC69E),
    ("aerbunny",       0xE2FCFF, 0xFFDFF9),
    ("aerwhale",       0xC0E7FD, 0x879EAA),
    ("cockatrice",     0x6CB15C, 0x6C579D),
    ("fire_minion",    0xFF6D01, 0xFEF500),
    ("flying_cow",     0xD8D8D8, 0xFFD939),
    ("mimic",          0xB18132, 0x605A4E),
    ("moa",            0x87BFEF, 0x7A7A7A),
    ("phyg",           0xFFC1D0, 0xFFD939),
    ("sentry",         0x808080, 0x3A8AEC),
    ("sheepuff",       0xE2FCFF, 0xCB9090),
    ("blue_swet",      0x4FB1DA, 0xCDDA4F),
    ("golden_swet",    0xCDDA4F, 0x4FB1DA),
    ("whirlwind",      0x9FC3F7, 0xFFFFFF),
    ("evil_whirlwind", 0x9FC3F7, 0x111111),
    ("valkyrie",       0xF9F5E3, 0xF2D200),
    ("zephyr",         0xDFDFDF, 0x99CFE8),
]


def java_int(argb: int) -> int:
    """0xAARRGGBB as the signed Java int an item definition stores."""
    argb &= 0xFFFFFFFF
    return argb - (1 << 32) if argb & 0x80000000 else argb


def tf_colours(mob: str) -> tuple[int, int]:
    path = TF_ITEMS / f"{mob}_spawn_egg.json"
    if not path.exists():
        if mob == "upper_goblin_knight":
            base, spots = tf_colours("lower_goblin_knight")
            return base, spots | 0x303030
        raise SystemExit(f"error: no Twilight Forest egg definition for '{mob}' ({path})")
    tints = json.loads(path.read_text())["model"]["tints"]
    return tuple(int(t["value"]) & 0xFFFFFF for t in tints[:2])  # type: ignore[return-value]


def entity_symbols() -> dict[str, str]:
    text = ENTITY_TYPES.read_text()
    return {slug: sym for sym, slug in re.findall(r'^\s*(\w+) = \d+,\s*//\s*"([^"]+)"', text, re.M)}


def pascal(slug: str) -> str:
    return "".join(part.capitalize() for part in slug.split("_"))


def build() -> dict[Path, bytes]:
    symbols = entity_symbols()
    eggs: list[tuple[str, int, int, str]] = []
    eggs += [(m, b, s, "The Hush") for m, b, s in HUSH]
    eggs += [(m, *tf_colours(m), "Twilight Forest") for m in TWILIGHT]
    eggs += [(m, b, s, "The Aether") for m, b, s in AETHER]

    out: dict[Path, bytes] = {}
    rows = []
    for mob, base, spots, source in eggs:
        if mob not in symbols:
            raise SystemExit(f"error: no EntityTypeId for '{mob}'")
        slug = f"{mob}_spawn_egg"
        item = {
            "model": {
                "type": "minecraft:model",
                "model": f"minecraft:item/{slug}",
                "tints": [
                    {"type": "minecraft:constant", "value": java_int(0xFF000000 | base)},
                    {"type": "minecraft:constant", "value": java_int(0xFF000000 | spots)},
                ],
            }
        }
        model = {
            "parent": "minecraft:item/generated",
            "textures": {
                "layer0": f"minecraft:item/{BASE_TEX}",
                "layer1": f"minecraft:item/{OVERLAY_TEX}",
            },
        }
        out[REPO / f"assets/items/{slug}.json"] = (json.dumps(item, indent=2) + "\n").encode()
        out[REPO / f"assets/models/item/{slug}.json"] = (json.dumps(model, indent=2) + "\n").encode()
        rows.append(f"        {{ Items::{pascal(slug)}, EntityTypeId::{symbols[mob]} }},   // {source}")

    inc = [
        "// File: src/common/entity/EngineSpawnEggs.inc",
        "// GENERATED by tools/gen_engine_spawn_eggs.py — do not edit by hand.",
        "// kSpawnEggTable rows for the engine-only mobs (The Hush, Twilight",
        "// Forest, the Aether). Included inside SpawnEggs.hpp's table.",
        *rows,
        "",
    ]
    out[REPO / "src/common/entity/EngineSpawnEggs.inc"] = "\n".join(inc).encode()
    for name, src in ((BASE_TEX, "spawn_egg_base.png"), (OVERLAY_TEX, "spawn_egg_overlay.png")):
        out[REPO / f"assets/textures/item/{name}.png"] = (TF_TEX / src).read_bytes()
    return out


def main() -> int:
    check = "--check" in sys.argv[1:]
    files = build()
    stale = [p for p, data in files.items() if not p.exists() or p.read_bytes() != data]
    if check:
        for p in stale:
            print(f"stale: {p.relative_to(REPO)}")
        return 1 if stale else 0
    for p, data in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    print(f"wrote {len(files)} files ({len(stale)} changed); "
          f"{sum(1 for p in files if p.name.endswith('_spawn_egg.json')) // 2} eggs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
