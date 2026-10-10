#!/usr/bin/env python3
"""Generate the assets and data of the 16 dyed torches (engine, 2026-10-10).

For every dye colour <c> (DyeColor order):
  assets/textures/block/<c>_torch.png        torch.png with its flame recoloured
  assets/blockstates/<c>_torch.json          torch.json / wall_torch.json, models renamed
  assets/blockstates/<c>_wall_torch.json
  assets/models/block/<c>_torch.json         template_torch / template_torch_wall
  assets/models/block/<c>_wall_torch.json
  assets/items/<c>_torch.json                the item (the wall twin has none)
  assets/models/item/<c>_torch.json
  data/minecraft/loot_table/blocks/<c>_torch.json   torch.json's table (the wall
                                                    twin drops through LootTables'
                                                    StandingCounterpart)
  data/minecraft/recipe/<c>_torch.json       shapeless: the dye with a torch or
                                             any other dyed torch (re-dyeing, as
                                             MC's dye_<c>_wool recipes do)

The colours are ColoredTorches::kColor (src/common/world/block/
ColoredTorches.hpp) — keep the two in step; BlockLightColor.inc gives the
light the same values.

Afterwards run tools/gen_recipes.py and tools/gen_loot_tables.py (or append
the loot rows by hand — see the report in the commit that added this script).

Usage: python3 tools/gen_colored_torches.py [--check]
"""
import argparse
import json
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent

COLORS = [
    ("white",      0xFFFFFF),
    ("orange",     0xFF8A1F),
    ("magenta",    0xFF3DF0),
    ("light_blue", 0x5CC8FF),
    ("yellow",     0xFFE23A),
    ("lime",       0x7DFF2A),
    ("pink",       0xFF7FB6),
    ("gray",       0xA9B3C2),
    ("light_gray", 0xD5DCE6),
    ("cyan",       0x1FE6E6),
    ("purple",     0xA040FF),
    ("blue",       0x3050FF),
    ("brown",      0xD08848),
    ("green",      0x2ED12E),
    ("red",        0xFF2A1A),
    ("black",      0x6A3CFF),
]


def rgb(value):
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)


def mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def recolor_torch(src, color):
    """torch.png's flame in `color`: its white-hot core and pale ring stay
    pale (whitened toward the colour), the body takes the colour, the deep
    orange tip a darker shade. The stick's browns are left alone."""
    out = src.copy()
    w, h = src.size
    base = rgb(color)
    dark = tuple(int(c * 0.72) for c in base)
    white = (255, 255, 255)
    for y in range(h):
        for x in range(w):
            r, g, b, a = src.getpixel((x, y))
            if a == 0:
                continue
            # The flame: bright and warm. The stick: browns, darker than 0xA0.
            if max(r, g, b) < 0xC0:
                continue
            if r > 0xF0 and g > 0xF0 and b > 0xF0:          # white-hot core
                new = mix(base, white, 0.78)
            elif r > 0xF0 and g > 0xF0:                      # pale yellow ring
                new = mix(base, white, 0.45)
            elif g > 0xC0:                                   # yellow body
                new = base
            else:                                            # orange tip
                new = dark
            out.putpixel((x, y), (*new, a))
    return out


def dump(obj):
    return json.dumps(obj, indent=2) + "\n"


def files_for(name, color, torch_png):
    files = {}
    tex = f"minecraft:block/{name}_torch"
    files[f"assets/textures/block/{name}_torch.png"] = recolor_torch(torch_png, color)
    files[f"assets/blockstates/{name}_torch.json"] = dump(
        {"variants": {"": {"model": f"minecraft:block/{name}_torch"}}})
    wall = f"minecraft:block/{name}_wall_torch"
    files[f"assets/blockstates/{name}_wall_torch.json"] = dump({"variants": {
        "facing=east":  {"model": wall},
        "facing=north": {"model": wall, "y": 270},
        "facing=south": {"model": wall, "y": 90},
        "facing=west":  {"model": wall, "y": 180},
    }})
    files[f"assets/models/block/{name}_torch.json"] = dump(
        {"parent": "minecraft:block/template_torch", "textures": {"torch": tex}})
    files[f"assets/models/block/{name}_wall_torch.json"] = dump(
        {"parent": "minecraft:block/template_torch_wall", "textures": {"torch": tex}})
    files[f"assets/items/{name}_torch.json"] = dump(
        {"model": {"type": "minecraft:model", "model": f"minecraft:item/{name}_torch"}})
    files[f"assets/models/item/{name}_torch.json"] = dump(
        {"parent": "minecraft:item/generated", "textures": {"layer0": tex}})
    files[f"data/minecraft/loot_table/blocks/{name}_torch.json"] = dump({
        "type": "minecraft:block",
        "pools": [{
            "bonus_rolls": 0.0,
            "conditions": [{"condition": "minecraft:survives_explosion"}],
            "entries": [{"type": "minecraft:item", "name": f"minecraft:{name}_torch"}],
            "rolls": 1.0,
        }],
        "random_sequence": f"minecraft:blocks/{name}_torch",
    })
    torches = ["minecraft:torch"] + [f"minecraft:{other}_torch" for other, _ in COLORS if other != name]
    files[f"data/minecraft/recipe/{name}_torch.json"] = dump({
        "type": "minecraft:crafting_shapeless",
        "category": "misc",
        "group": "dyed_torch",
        "ingredients": [f"minecraft:{name}_dye", torches],
        "result": {"count": 1, "id": f"minecraft:{name}_torch"},
    })
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="report stale files, write nothing")
    args = ap.parse_args()

    torch_png = Image.open(ROOT / "assets/textures/block/torch.png").convert("RGBA")
    stale = []
    for name, color in COLORS:
        for rel, content in files_for(name, color, torch_png).items():
            path = ROOT / rel
            if isinstance(content, Image.Image):
                same = path.exists() and list(Image.open(path).convert("RGBA").getdata()) == list(content.getdata())
                if not same:
                    stale.append(rel)
                    if not args.check:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        content.save(path)
            else:
                same = path.exists() and path.read_text() == content
                if not same:
                    stale.append(rel)
                    if not args.check:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(content)
    verb = "stale" if args.check else "wrote"
    print(f"gen_colored_torches: {verb} {len(stale)} file(s)")
    for rel in stale[:8]:
        print(f"  {rel}")
    return 1 if (args.check and stale) else 0


if __name__ == "__main__":
    sys.exit(main())
