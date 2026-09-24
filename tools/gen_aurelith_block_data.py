#!/usr/bin/env python3
# tools/gen_aurelith_block_data.py
#
# The resource and data files of Aurelith's 26 blocks (the BlockDefs.inc rows
# choirstone .. voice_beacon; docs/the-hush.md "Aurelith"): blockstates and
# block models (assets/), block loot tables, recipes and the vanilla block tags
# the engine or MC semantics read (data/minecraft/). Textures come from
# tools/gen_aurelith_textures.py. Everything here is emitted from the tables
# below so a family cannot drift out of step with itself; the stairs, slab and
# wall blockstates are the Hush's hushstone-brick ones with the name swapped
# (identical variant sets, since the block classes are the same).
#
#   python3 tools/gen_aurelith_block_data.py            # write every file
#   python3 tools/gen_aurelith_block_data.py --check    # compare, exit 1 on drift
#
# After a change here, re-run tools/gen_loot_tables.py and tools/gen_recipes.py
# (they compile the loot tables and recipes into the engine's generated tables).
#
# Items: like every Hush block, the block items use the block's own model (no
# assets/items entry); the wall's inventory model is <wall>_inventory, as the
# hushstone brick wall's is.

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ASSETS = REPO_ROOT / "assets"
DATA = REPO_ROOT / "data" / "minecraft"
NS = "minecraft:"

# The plain cubes: block -> texture (cube_all).
CUBES = {
    "choirstone": "choirstone",
    "polished_choirstone": "polished_choirstone",
    "choirstone_bricks": "choirstone_bricks",
    "cracked_choirstone_bricks": "cracked_choirstone_bricks",
    "chiseled_choirstone": "chiseled_choirstone",
    "choirstone_tiles": "choirstone_tiles",
    "stave_stone": "stave_stone",
    "nightglass": "nightglass",
    "resonite_grate": "resonite_grate",
    "cyan_lumen_panel": "cyan_lumen_panel",
    "violet_lumen_panel": "violet_lumen_panel",
    "amber_lumen_panel": "amber_lumen_panel",
}
# Rotated pillars (quartz_pillar's pattern): block -> (side, end).
PILLARS = {
    "choirstone_pillar": ("choirstone_pillar", "choirstone_pillar_top"),
    "lumen_strip": ("lumen_strip", "lumen_strip_top"),
}
# Stairs / slabs: block -> the full block (and texture) of its family.
STAIRS = {
    "polished_choirstone_stairs": "polished_choirstone",
    "choirstone_brick_stairs": "choirstone_bricks",
    "choirstone_tile_stairs": "choirstone_tiles",
}
SLABS = {
    "polished_choirstone_slab": "polished_choirstone",
    "choirstone_brick_slab": "choirstone_bricks",
    "choirstone_tile_slab": "choirstone_tiles",
}
WALLS = {"choirstone_brick_wall": "choirstone_bricks"}
# The flickering windows (sea-lantern class, cube_all on their own animated
# texture; tools/gen_aurelith_textures.py "flickering windows"): block ->
# (the lumen panel it is made from, how many a craft makes). Guttering: one
# nightglass over the panel; waking: two; restless: the panel between two.
WINDOWS = {
    "guttering_amber_window": ("amber_lumen_panel", ["N", "P"], 2),
    "waking_amber_window": ("amber_lumen_panel", ["N", "N", "P"], 3),
    "restless_amber_window": ("amber_lumen_panel", ["N", "P", "N"], 3),
    "guttering_cyan_window": ("cyan_lumen_panel", ["N", "P"], 2),
    "waking_cyan_window": ("cyan_lumen_panel", ["N", "N", "P"], 3),
    "guttering_violet_window": ("violet_lumen_panel", ["N", "P"], 2),
    "waking_violet_window": ("violet_lumen_panel", ["N", "N", "P"], 3),
}
HUSH_STAIRS, HUSH_SLAB, HUSH_WALL, HUSH_FULL = (
    "hushstone_brick_stairs", "hushstone_brick_slab", "hushstone_brick_wall", "hushstone_bricks")


def j(obj) -> str:
    return json.dumps(obj, indent=2) + "\n"


def face(tex: str, uv=None, cull: str | None = None) -> dict:
    f: dict = {}
    if uv is not None:
        f["uv"] = uv
    f["texture"] = tex
    if cull:
        f["cullface"] = cull
    return f


def box(frm, to, faces: dict, comment: str | None = None, **extra) -> dict:
    e: dict = {}
    if comment:
        e["__comment"] = comment
    e["from"] = frm
    e["to"] = to
    e.update(extra)
    e["faces"] = faces
    return e


def sides(tex: str, uv, cull: bool = False) -> dict:
    return {d: face(tex, uv, d if cull else None) for d in ("north", "south", "west", "east")}


# ── models with their own geometry ──────────────────────────────────────────
def conduit_model() -> dict:
    # A 6x6 crystal core down the axis inside three 8x8 resonite collars
    # (ends and middle); the collars' end faces show the core's glowing cross
    # section (gen_aurelith_textures.py: crystal_conduit's UV sheet).
    ring = [6, 8, 14, 16]
    collar_side = [6, 0, 14, 2]

    def collar(y0, y1, name, cull_down=False, cull_up=False):
        f = sides("#conduit", collar_side)
        f["down"] = face("#conduit", ring, "down" if cull_down else None)
        f["up"] = face("#conduit", ring, "up" if cull_up else None)
        return box([4, y0, 4], [12, y1, 12], f, name)

    return {
        "parent": "minecraft:block/block",
        "textures": {"particle": "minecraft:block/crystal_conduit",
                     "conduit": "minecraft:block/crystal_conduit"},
        "elements": [
            box([5, 0, 5], [11, 16, 11], sides("#conduit", [0, 0, 6, 16]), "the crystal core"),
            collar(0, 2, "lower collar", cull_down=True),
            collar(7, 9, "middle collar"),
            collar(14, 16, "upper collar", cull_up=True),
        ],
    }


def lamp_model(hanging: bool) -> dict:
    # A crystal globe in a four-post resonite cage between a base plate and a
    # cap, a crystal finial on top; the hanging one sits a block-pixel higher
    # and hangs from a chain (lantern_hanging's crossed planes). Inside MC
    # lantern's footprint widened to 8x8.
    o = 1 if hanging else 0
    metal_plate = [8, 0, 16, 8]
    metal_edge = [8, 0, 16, 1]
    globe = [0, 0, 7, 7]
    plate_faces = sides("#lamp", metal_edge)
    plate_faces["down"] = face("#lamp", metal_plate, None if hanging else "down")
    plate_faces["up"] = face("#lamp", metal_plate)
    cap_faces = sides("#lamp", metal_edge)
    cap_faces["down"] = face("#lamp", metal_plate)
    cap_faces["up"] = face("#lamp", metal_plate)
    finial = sides("#lamp", [8, 8, 10, 10])
    finial["up"] = face("#lamp", [8, 8, 10, 10])
    els = [
        box([4, o, 4], [12, o + 1, 12], plate_faces, "base plate"),
        box([4.5, o + 1, 4.5], [11.5, o + 8, 11.5],
            {**sides("#lamp", globe), "up": face("#lamp", globe), "down": face("#lamp", globe)},
            "the crystal globe"),
    ]
    for x, z in ((4, 4), (11, 4), (4, 11), (11, 11)):
        els.append(box([x, o + 1, z], [x + 1, o + 8, z + 1], sides("#lamp", [8, 1, 9, 8]), "cage post"))
    els.append(box([4, o + 8, 4], [12, o + 9, 12], cap_faces, "cap"))
    els.append(box([7, o + 9, 7], [9, o + 11, 9], finial, "crystal finial"))
    if hanging:
        rot = {"origin": [8, 8, 8], "axis": "y", "angle": 45}
        els.append(box([6.5, 12, 8], [9.5, 16, 8],
                       {"north": face("#lamp", [16, 8, 13, 12]), "south": face("#lamp", [13, 8, 16, 12])},
                       "chain", rotation=rot, shade=False))
        els.append(box([8, 12, 6.5], [8, 16, 9.5],
                       {"west": face("#lamp", [16, 8, 13, 12]), "east": face("#lamp", [13, 8, 16, 12])},
                       "chain", rotation=rot, shade=False))
    return {
        "parent": "minecraft:block/block",
        "textures": {"particle": "minecraft:block/choir_lamp", "lamp": "minecraft:block/choir_lamp"},
        "elements": els,
    }


def beacon_model() -> dict:
    # The lighthouse lamp's language: a resonite plinth, a tall prism lens
    # and a cap with a glowing aperture (VoiceBeaconRenderer's beam leaves
    # from the top). Symmetric, so the four facings share it.
    base = sides("#side", [0, 12, 16, 16], cull=True)
    base["down"] = face("#top", None, "down")
    base["up"] = face("#top")
    cap = sides("#side", [1, 0, 15, 2])
    cap["down"] = face("#top", [1, 1, 15, 15])
    cap["up"] = face("#top", [1, 1, 15, 15], "up")
    return {
        "parent": "minecraft:block/block",
        "textures": {
            "particle": "minecraft:block/voice_beacon_side",
            "side": "minecraft:block/voice_beacon_side",
            "top": "minecraft:block/voice_beacon_top",
            "lens": "minecraft:block/voice_beacon_lens",
        },
        "elements": [
            box([0, 0, 0], [16, 4, 16], base, "plinth"),
            box([2, 4, 2], [14, 14, 14], sides("#lens", [2, 3, 14, 13]), "the prism lens"),
            box([1, 14, 1], [15, 16, 15], cap, "cap with its aperture"),
        ],
    }


# ── Reawakening the Heart ───────────────────────────────────────────────────
# dim block -> its lit twin (AurelithQuestBlocks.cpp kTwins).
DIM_TWINS = {
    "dim_cyan_lumen_panel": "cyan_lumen_panel",
    "dim_violet_lumen_panel": "violet_lumen_panel",
    "dim_amber_lumen_panel": "amber_lumen_panel",
    "dim_lumen_strip": "lumen_strip",
    "dim_stave_stone": "stave_stone",
    "dim_choir_lamp": "choir_lamp",
}


def socket_model() -> dict:
    base = sides("#base", [3, 14, 13, 16])
    base["down"] = face("#base", [3, 3, 13, 13], "down")
    base["up"] = face("#base", [3, 3, 13, 13])
    cradle = sides("#cradle", [4, 0, 12, 3])
    cradle["down"] = face("#cradle", [4, 4, 12, 12])
    cradle["up"] = {"uv": [4, 4, 12, 12], "texture": "#top", "rotation": 90}
    prong = {**sides("#cradle", [0, 0, 1, 2]), "up": face("#cradle", [0, 0, 1, 1])}
    els = [
        box([3, 0, 3], [13, 2, 13], base, "base plate"),
        box([5, 2, 5], [11, 10, 11], sides("#post", [5, 6, 11, 14]), "the post"),
        box([4, 10, 4], [12, 13, 12], cradle, "the cradle (keyway on top)"),
    ]
    for x, z in ((4, 4), (11, 4), (4, 11), (11, 11)):
        els.append(box([x, 13, z], [x + 1, 15, z + 1], prong, "prong"))
    return {"parent": "minecraft:block/block",
            "textures": {"particle": "minecraft:block/chord_socket_cradle",
                         "base": "minecraft:block/polished_choirstone",
                         "post": "minecraft:block/chord_socket_post",
                         "cradle": "minecraft:block/chord_socket_cradle",
                         "top": "minecraft:block/chord_socket_top"},
            "elements": els}


def pedestal_model() -> dict:
    foot = sides("#foot", [1, 13, 15, 16], cull=True)
    foot["down"] = face("#foot", [1, 1, 15, 15], "down")
    foot["up"] = face("#foot", [1, 1, 15, 15])
    top = sides("#rim", [2, 1, 14, 4])
    top["down"] = face("#rim", [2, 2, 14, 14])
    top["up"] = face("#top", [2, 2, 14, 14])
    return {"parent": "minecraft:block/block",
            "textures": {"particle": "minecraft:block/voice_pedestal_side",
                         "foot": "minecraft:block/chiseled_choirstone",
                         "side": "minecraft:block/voice_pedestal_side",
                         "rim": "minecraft:block/polished_choirstone",
                         "top": "minecraft:block/voice_pedestal_top"},
            "elements": [
                box([1, 0, 1], [15, 3, 15], foot, "the chiseled foot"),
                box([4, 3, 4], [12, 12, 12], sides("#side", [4, 4, 12, 13]), "the banded shaft"),
                box([2, 12, 2], [14, 15, 14], top, "the ringed top"),
            ]}


# ── emit ─────────────────────────────────────────────────────────────────────
def build() -> dict[Path, str]:
    files: dict[Path, str] = {}
    bs = ASSETS / "blockstates"
    md = ASSETS / "models" / "block"

    def model_ref(name: str) -> str:
        return f"{NS}block/{name}"

    def single(block: str, model: str) -> None:
        files[bs / f"{block}.json"] = j({"variants": {"": {"model": model_ref(model)}}})

    for block, tex in CUBES.items():
        files[md / f"{block}.json"] = j({"parent": "minecraft:block/cube_all",
                                         "textures": {"all": model_ref(tex)}})
        single(block, block)

    for block, (side, end) in PILLARS.items():
        tex = {"end": model_ref(end), "side": model_ref(side)}
        files[md / f"{block}.json"] = j({"parent": "minecraft:block/cube_column", "textures": tex})
        files[md / f"{block}_horizontal.json"] = j({"parent": "minecraft:block/cube_column_horizontal",
                                                    "textures": tex})
        files[bs / f"{block}.json"] = j({"variants": {
            "axis=x": {"model": model_ref(f"{block}_horizontal"), "x": 90, "y": 90},
            "axis=y": {"model": model_ref(block)},
            "axis=z": {"model": model_ref(f"{block}_horizontal"), "x": 90},
        }})

    def swapped(src_block: str, hush_full: str, block: str, full: str) -> str:
        text = (ASSETS / "blockstates" / f"{src_block}.json").read_text(encoding="utf-8")
        text = text.replace(f"block/{src_block}", f"block/{block}")
        text = text.replace(f"block/{hush_full}\"", f"block/{full}\"")
        return json.dumps(json.loads(text), indent=2) + "\n"

    for block, full in STAIRS.items():
        three = {"bottom": model_ref(full), "side": model_ref(full), "top": model_ref(full)}
        for suffix, parent in (("", "stairs"), ("_inner", "inner_stairs"), ("_outer", "outer_stairs")):
            files[md / f"{block}{suffix}.json"] = j({"parent": f"minecraft:block/{parent}", "textures": three})
        files[bs / f"{block}.json"] = swapped(HUSH_STAIRS, HUSH_FULL, block, full)
    for block, full in SLABS.items():
        three = {"bottom": model_ref(full), "side": model_ref(full), "top": model_ref(full)}
        files[md / f"{block}.json"] = j({"parent": "minecraft:block/slab", "textures": three})
        files[md / f"{block}_top.json"] = j({"parent": "minecraft:block/slab_top", "textures": three})
        files[bs / f"{block}.json"] = swapped(HUSH_SLAB, HUSH_FULL, block, full)
    for block, full in WALLS.items():
        for suffix, parent in (("_inventory", "wall_inventory"), ("_post", "template_wall_post"),
                               ("_side", "template_wall_side"), ("_side_tall", "template_wall_side_tall")):
            files[md / f"{block}{suffix}.json"] = j({"parent": f"minecraft:block/{parent}",
                                                     "textures": {"wall": model_ref(full)}})
        files[bs / f"{block}.json"] = swapped(HUSH_WALL, HUSH_FULL, block, full)

    # The crystal conduit: iron_chain's axis variants.
    files[md / "crystal_conduit.json"] = j(conduit_model())
    files[bs / "crystal_conduit.json"] = j({"variants": {
        "axis=x": {"model": model_ref("crystal_conduit"), "x": 90, "y": 90},
        "axis=y": {"model": model_ref("crystal_conduit")},
        "axis=z": {"model": model_ref("crystal_conduit"), "x": 90},
    }})
    # The choir lamp: lantern's hanging variants.
    files[md / "choir_lamp.json"] = j(lamp_model(False))
    files[md / "choir_lamp_hanging.json"] = j(lamp_model(True))
    files[bs / "choir_lamp.json"] = j({"variants": {
        "hanging=false": {"model": model_ref("choir_lamp")},
        "hanging=true": {"model": model_ref("choir_lamp_hanging")},
    }})
    # The river's water: water's pattern — the fluid renderer draws it, the
    # block model is only the particle sprite.
    files[md / "resonant_water.json"] = j({"textures": {"particle": "block/resonant_water_still"}})
    single("resonant_water", "resonant_water")
    # The Heart's core: a column (glowing eye on every side, the top a vent).
    files[md / "resonance_engine.json"] = j({"parent": "minecraft:block/cube_column", "textures": {
        "end": model_ref("resonance_engine_top"), "side": model_ref("resonance_engine_side")}})
    single("resonance_engine", "resonance_engine")
    # The voice beacon: carved_pumpkin's four facings (the facing is the voice).
    files[md / "voice_beacon.json"] = j(beacon_model())
    files[bs / "voice_beacon.json"] = j({"variants": {
        "facing=east": {"model": model_ref("voice_beacon"), "y": 90},
        "facing=north": {"model": model_ref("voice_beacon")},
        "facing=south": {"model": model_ref("voice_beacon"), "y": 180},
        "facing=west": {"model": model_ref("voice_beacon"), "y": 270},
    }})

    # ── block loot (vanilla's shapes) ────────────────────────────────────────
    lt = DATA / "loot_table" / "blocks"

    def self_drop(block: str) -> dict:
        return {"type": "minecraft:block", "pools": [{
            "bonus_rolls": 0.0,
            "conditions": [{"condition": "minecraft:survives_explosion"}],
            "entries": [{"type": "minecraft:item", "name": NS + block}],
            "rolls": 1.0}], "random_sequence": f"minecraft:blocks/{block}"}

    def slab_drop(block: str) -> dict:
        return {"type": "minecraft:block", "pools": [{
            "bonus_rolls": 0.0,
            "entries": [{"type": "minecraft:item", "functions": [
                {"add": False, "conditions": [{"block": NS + block,
                                               "condition": "minecraft:block_state_property",
                                               "properties": {"type": "double"}}],
                 "count": 2.0, "function": "minecraft:set_count"},
                {"function": "minecraft:explosion_decay"}], "name": NS + block}],
            "rolls": 1.0}], "random_sequence": f"minecraft:blocks/{block}"}

    for block in (list(CUBES) + list(PILLARS) + list(STAIRS) + list(WALLS)
                  + ["crystal_conduit", "choir_lamp", "resonance_engine", "voice_beacon"]):
        files[lt / f"{block}.json"] = j(self_drop(block))
    for block in SLABS:
        files[lt / f"{block}.json"] = j(slab_drop(block))
    # resonant_water: none (water and bubble_column have no block loot table).

    # ── recipes ───────────────────────────────────────────────────────────────
    rc = DATA / "recipe"

    def shaped(name, category, pattern, key, result, count, group=None):
        r = {"type": "minecraft:crafting_shaped", "category": category}
        if group:
            r["group"] = group
        r["key"] = {k: NS + v for k, v in key.items()}
        r["pattern"] = pattern
        r["result"] = {"count": count, "id": NS + result}
        files[rc / f"{name}.json"] = j(r)

    def cutting(result, ingredient, count=1):
        files[rc / f"{result}_from_{ingredient}_stonecutting.json"] = j({
            "type": "minecraft:stonecutting", "ingredient": NS + ingredient,
            "result": {"count": count, "id": NS + result}})

    shaped("choirstone", "building", ["HHH", "HEH", "HHH"], {"H": "hushstone", "E": "echo_shard"},
           "choirstone", 8)
    shaped("polished_choirstone", "building", ["SS", "SS"], {"S": "choirstone"}, "polished_choirstone", 4)
    shaped("choirstone_bricks", "building", ["SS", "SS"], {"S": "polished_choirstone"}, "choirstone_bricks", 4)
    shaped("choirstone_tiles", "building", ["SS", "SS"], {"S": "choirstone_bricks"}, "choirstone_tiles", 4)
    shaped("chiseled_choirstone", "building", ["S", "S"], {"S": "polished_choirstone_slab"},
           "chiseled_choirstone", 1)
    shaped("choirstone_pillar", "building", ["S", "S"], {"S": "polished_choirstone"}, "choirstone_pillar", 2)
    files[rc / "cracked_choirstone_bricks.json"] = j({
        "type": "minecraft:smelting", "category": "blocks", "cookingtime": 200, "experience": 0.1,
        "ingredient": NS + "choirstone_bricks", "result": {"id": NS + "cracked_choirstone_bricks"}})
    for block, full in STAIRS.items():
        shaped(block, "building", ["#  ", "## ", "###"], {"#": full}, block, 4)
    for block, full in SLABS.items():
        shaped(block, "building", ["###"], {"#": full}, block, 6)
    for block, full in WALLS.items():
        shaped(block, "misc", ["###", "###"], {"#": full}, block, 6)
    # Stonecutting, deepslate's tree: every form from each stone it can be
    # cut from (choirstone -> polished -> bricks -> tiles).
    tree = {
        "polished_choirstone": ["choirstone"],
        "choirstone_bricks": ["choirstone", "polished_choirstone"],
        "choirstone_tiles": ["choirstone", "polished_choirstone", "choirstone_bricks"],
        "chiseled_choirstone": ["choirstone", "polished_choirstone"],
        "choirstone_pillar": ["choirstone", "polished_choirstone"],
    }
    for result, sources in tree.items():
        for src in sources:
            cutting(result, src)
    derived = {
        "polished_choirstone": ("polished_choirstone_stairs", "polished_choirstone_slab", None),
        "choirstone_bricks": ("choirstone_brick_stairs", "choirstone_brick_slab", "choirstone_brick_wall"),
        "choirstone_tiles": ("choirstone_tile_stairs", "choirstone_tile_slab", None),
    }
    for full, (stairs, slab, wall) in derived.items():
        for src in [full] + tree.get(full, []):
            cutting(stairs, src)
            cutting(slab, src, 2)
            if wall:
                cutting(wall, src)
    shaped("stave_stone", "building", [" P ", "PGP", " P "],
           {"P": "polished_choirstone", "G": "glow_ink_sac"}, "stave_stone", 4)
    shaped("nightglass", "building", ["GGG", "GRG", "GGG"], {"G": "glass", "R": "resonite_ingot"},
           "nightglass", 8)
    shaped("resonite_grate", "building", [" R ", "R R", " R "], {"R": "resonite_ingot"}, "resonite_grate", 4)
    cutting("resonite_grate", "resonite_block", 4)
    for colour, dye in (("cyan", "cyan_dye"), ("violet", "purple_dye"), ("amber", "orange_dye")):
        shaped(f"{colour}_lumen_panel", "decorations", [" G ", "GCG", " D "],
               {"G": "glass", "C": "resonant_crystal", "D": dye}, f"{colour}_lumen_panel", 4,
               group="lumen_panel")
    shaped("lumen_strip", "decorations", ["RCR"], {"R": "resonite_ingot", "C": "resonant_crystal"},
           "lumen_strip", 6)
    shaped("crystal_conduit", "decorations", ["R", "C", "R"],
           {"R": "resonite_ingot", "C": "resonant_crystal"}, "crystal_conduit", 4)
    shaped("choir_lamp", "decorations", [" I ", "NCN", " I "],
           {"I": "resonite_ingot", "N": "nightglass", "C": "resonant_crystal"}, "choir_lamp", 2)
    shaped("voice_beacon", "misc", ["NNN", "NHN", "RRR"],
           {"N": "nightglass", "H": "resonant_heart", "R": "resonite_ingot"}, "voice_beacon", 1)
    # The flickering windows: model, blockstate, self-drop, a recipe each.
    for block, (panel, pattern, count) in WINDOWS.items():
        files[md / f"{block}.json"] = j({"parent": "minecraft:block/cube_all",
                                         "textures": {"all": model_ref(block)}})
        single(block, block)
        files[lt / f"{block}.json"] = j(self_drop(block))
        shaped(block, "decorations", pattern, {"N": "nightglass", "P": panel}, block, count,
               group="lumen_window")

    # ── Reawakening the Heart (docs/the-hush.md) ─────────────────────────────
    # The dormant city's lights: their lit twins' shapes with the dim art;
    # mined, each drops its lit twin (the crystal is the same, only the note
    # has faded).
    for block, twin in DIM_TWINS.items():
        if block in ("dim_cyan_lumen_panel", "dim_violet_lumen_panel", "dim_amber_lumen_panel",
                     "dim_stave_stone"):
            files[md / f"{block}.json"] = j({"parent": "minecraft:block/cube_all",
                                             "textures": {"all": model_ref(block)}})
            single(block, block)
        files[lt / f"{block}.json"] = j({"type": "minecraft:block", "pools": [{
            "bonus_rolls": 0.0,
            "conditions": [{"condition": "minecraft:survives_explosion"}],
            "entries": [{"type": "minecraft:item", "name": NS + twin}],
            "rolls": 1.0}], "random_sequence": f"minecraft:blocks/{block}"})
    tex = {"end": model_ref("dim_lumen_strip_top"), "side": model_ref("dim_lumen_strip")}
    files[md / "dim_lumen_strip.json"] = j({"parent": "minecraft:block/cube_column", "textures": tex})
    files[md / "dim_lumen_strip_horizontal.json"] = j({"parent": "minecraft:block/cube_column_horizontal",
                                                       "textures": tex})
    files[bs / "dim_lumen_strip.json"] = j({"variants": {
        "axis=x": {"model": model_ref("dim_lumen_strip_horizontal"), "x": 90, "y": 90},
        "axis=y": {"model": model_ref("dim_lumen_strip")},
        "axis=z": {"model": model_ref("dim_lumen_strip_horizontal"), "x": 90},
    }})
    for hanging, suffix in ((False, ""), (True, "_hanging")):
        m = lamp_model(hanging)
        m["textures"] = {"particle": model_ref("dim_choir_lamp"), "lamp": model_ref("dim_choir_lamp")}
        files[md / f"dim_choir_lamp{suffix}.json"] = j(m)
    files[bs / "dim_choir_lamp.json"] = j({"variants": {
        "hanging=false": {"model": model_ref("dim_choir_lamp")},
        "hanging=true": {"model": model_ref("dim_choir_lamp_hanging")},
    }})
    # The chord socket: a choirstone post on a plate, a resonite cradle with
    # the keyway across it (square to the facing, so a seated key shows its
    # face to the Heart) and four prongs; stonecutter's four facings.
    files[md / "chord_socket.json"] = j(socket_model())
    files[bs / "chord_socket.json"] = j({"variants": {
        "facing=east": {"model": model_ref("chord_socket"), "y": 90},
        "facing=north": {"model": model_ref("chord_socket")},
        "facing=south": {"model": model_ref("chord_socket"), "y": 180},
        "facing=west": {"model": model_ref("chord_socket"), "y": 270},
    }})
    # The voice pedestal: a chiseled foot, a banded shaft, a ringed top.
    files[md / "voice_pedestal.json"] = j(pedestal_model())
    single("voice_pedestal", "voice_pedestal")
    # The choir cabinet: orientable (front, side, top) on the four sides,
    # cube_bottom_top facing up or down; barrel's facing x open states.
    for opened in (False, True):
        o = "_open" if opened else ""
        front = model_ref("choir_cabinet_front" + o)
        files[md / f"choir_cabinet{o}.json"] = j({"parent": "minecraft:block/orientable", "textures": {
            "front": front, "side": model_ref("choir_cabinet_side"), "top": model_ref("choir_cabinet_top"),
            "particle": model_ref("choir_cabinet_side")}})
        files[md / f"choir_cabinet_vertical{o}.json"] = j({"parent": "minecraft:block/cube_bottom_top",
            "textures": {"top": front, "side": model_ref("choir_cabinet_side"),
                         "bottom": model_ref("choir_cabinet_back")}})
    variants = {}
    for facing, rot in (("north", {}), ("east", {"y": 90}), ("south", {"y": 180}), ("west", {"y": 270}),
                        ("up", None), ("down", None)):
        for opened in (False, True):
            o = "_open" if opened else ""
            key = f"facing={facing},open={'true' if opened else 'false'}"
            if rot is None:
                v = {"model": model_ref(f"choir_cabinet_vertical{o}")}
                if facing == "down":
                    v["x"] = 180
            else:
                v = {"model": model_ref(f"choir_cabinet{o}"), **rot}
            variants[key] = v
    files[bs / "choir_cabinet.json"] = j({"variants": dict(sorted(variants.items()))})
    for block in ("chord_socket", "voice_pedestal", "choir_cabinet"):
        files[lt / f"{block}.json"] = j(self_drop(block))
    shaped("chord_socket", "decorations", [" R ", "RCR", " P "],
           {"R": "resonite_ingot", "C": "resonant_crystal", "P": "polished_choirstone"}, "chord_socket", 1)
    shaped("voice_pedestal", "decorations", ["SSS", " P ", "PPP"],
           {"S": "polished_choirstone_slab", "P": "polished_choirstone"}, "voice_pedestal", 1)
    return files


# ── vanilla block tags ──────────────────────────────────────────────────────
# MC semantics the Aurelith blocks share with their vanilla analogues. The
# engine reads #blocks_motion_no_leaves (fluid flow: FluidState.cpp) and the
# mineable/needs tags through gen_block_hardness (which also falls back to the
# alias); the family tags keep the data pack truthful for anything that asks.
SOLIDS = (list(CUBES) + list(PILLARS) + list(STAIRS) + list(SLABS) + list(WALLS) + list(WINDOWS)
          + ["crystal_conduit", "choir_lamp", "resonance_engine", "voice_beacon"])
QUEST_SOLIDS = list(DIM_TWINS) + ["chord_socket", "voice_pedestal", "choir_cabinet"]
TAGS = {
    "blocks_motion_no_leaves": SOLIDS + QUEST_SOLIDS,
    "mineable/pickaxe": [b for b in SOLIDS if b not in WINDOWS and b not in (
        "nightglass", "cyan_lumen_panel", "violet_lumen_panel", "amber_lumen_panel",
        "lumen_strip", "resonance_engine", "voice_beacon")],
    "stairs": list(STAIRS),
    "slabs": list(SLABS),
    "walls": list(WALLS),
    "impermeable": ["nightglass"],
}
# The quest's blocks break with the tools their classes do: stave stone,
# the socket and the pedestal with a pickaxe, the cabinet with an axe (the
# dim panels, strip and lamp need none, like their twins).
TAGS["mineable/pickaxe"] = TAGS["mineable/pickaxe"] + ["dim_stave_stone", "dim_choir_lamp",
                                                       "chord_socket", "voice_pedestal"]
TAGS["mineable/axe"] = ["choir_cabinet"]


def tag_files() -> dict[Path, str]:
    """Each tag file with our ids appended (once, in our order) after the
    existing values, which are kept verbatim."""
    out: dict[Path, str] = {}
    for tag, blocks in TAGS.items():
        path = DATA / "tags" / "block" / f"{tag}.json"
        original = path.read_text(encoding="utf-8")
        data = json.loads(original)
        values = [v for v in data["values"] if not (isinstance(v, str) and v[len(NS):] in blocks)]
        values += [NS + b for b in blocks]
        data["values"] = values
        # Keep the file's own trailing-newline convention so only our lines change.
        out[path] = json.dumps(data, indent=2) + ("\n" if original.endswith("\n") else "")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare with the files on disk; exit 1 on drift")
    args = ap.parse_args()
    files = build()
    files.update(tag_files())
    drift = 0
    for path, text in sorted(files.items()):
        rel = path.relative_to(REPO_ROOT)
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                print(f"DRIFT    {rel}")
                drift += 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                path.write_text(text, encoding="utf-8")
                print(f"wrote    {rel}")
    if args.check:
        print(f"check: {'clean' if drift == 0 else f'{drift} file(s) differ'} ({len(files)} files)")
        return 1 if drift else 0
    print(f"{len(files)} files up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
