#!/usr/bin/env python3
# tools/copy_twilight_assets.py
#
# Pass-one Twilight Forest assets, copied from the mod's own sources under
# mods_reference/twilightforest (git-ignored clone of TeamTwilight/twilightforest).
#
# LICENCE: Twilight Forest's textures and models are CC BY-NC-SA 4.0, so they
# may ship in this free, non-commercial game WITH attribution. Every file this
# script writes is listed in assets/ATTRIBUTION.md (regenerated here). Tinted
# textures (the mod colours them in code, we bake the colour in) are marked
# there as adaptations, which the ShareAlike term covers.
#
#   python3 tools/copy_twilight_assets.py            # copy/translate everything
#   python3 tools/copy_twilight_assets.py --check    # compare against what is on disk, exit 1 on drift
#
# What it does:
#   * blockstates + block models: the mod's GENERATED JSON
#     (src/generated/resources/assets/twilightforest/{blockstates,models/block})
#     or, where the mod hand-wrote a model (mushgloom, mayapple), the one in
#     src/main/resources, translated: `twilightforest:block/<x>` becomes
#     `minecraft:block/<ours>`, `render_type` / neoforge keys and every
#     `tintindex` are dropped (our leaves are untinted: the colour is baked).
#   * mangrove_* is renamed tf_mangrove_* (vanilla already has mangrove).
#   * textures: every texture a translated model references is copied byte-for-
#     byte with its .mcmeta; the tinted ones are baked (see BAKED below).
#   * critters (firefly, cicada, moonworm) are block-entity models in the mod;
#     their Java ModelPart boxes (FireflyModel, CicadaModel, MoonwormModel) are
#     converted to JSON elements with MC's entity-cube UV layout, on the
#     entity texture, facing up; the blockstate rotates them like end_rod.
#   * items (torchberries, liveroot) and six mob textures are copied.
#   * every reference in every written model/blockstate is resolved to a file.

from __future__ import annotations

import argparse
import colorsys
import io
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
TF = REPO / "mods_reference" / "twilightforest" / "src"
TF_GEN = TF / "generated" / "resources" / "assets" / "twilightforest"
TF_MAIN = TF / "main" / "resources" / "assets" / "twilightforest"
TF_TEX = TF_MAIN / "textures"
ASSETS = REPO / "assets"
OUT_BS = ASSETS / "blockstates"
OUT_MODEL = ASSETS / "models" / "block"
OUT_TEX = ASSETS / "textures"

# Blocks copied straight from the mod's generated blockstate (our slug -> mod slug).
BLOCKS: dict[str, str] = {
    "twilight_oak_log": "twilight_oak_log",
    "stripped_twilight_oak_log": "stripped_twilight_oak_log",
    "twilight_oak_planks": "twilight_oak_planks",
    "twilight_oak_sapling": "twilight_oak_sapling",
    "canopy_log": "canopy_log",
    "stripped_canopy_log": "stripped_canopy_log",
    "canopy_planks": "canopy_planks",
    "canopy_sapling": "canopy_sapling",
    "tf_mangrove_log": "mangrove_log",
    "stripped_tf_mangrove_log": "stripped_mangrove_log",
    "tf_mangrove_planks": "mangrove_planks",
    "tf_mangrove_sapling": "mangrove_sapling",
    "dark_log": "dark_log",
    "stripped_dark_log": "stripped_dark_log",
    "dark_planks": "dark_planks",
    "darkwood_sapling": "darkwood_sapling",
    "root": "root",
    "liveroot_block": "liveroot_block",
    "hedge": "hedge",
    "mushgloom": "mushgloom",
    "fiddlehead": "fiddlehead",
    "mayapple": "mayapple",
    "torchberry_plant": "torchberry_plant",
    "fallen_leaves": "fallen_leaves",
    "mazestone": "mazestone",
    "mazestone_brick": "mazestone_brick",
    "cracked_mazestone": "cracked_mazestone",
    "mossy_mazestone": "mossy_mazestone",
    "mazestone_mosaic": "mazestone_mosaic",
    "mazestone_border": "mazestone_border",
    # ── pass two (docs/mod-ports.md) ──
    "towerwood": "towerwood",
    "encased_towerwood": "encased_towerwood",
    "cracked_towerwood": "cracked_towerwood",
    "mossy_towerwood": "mossy_towerwood",
    "infested_towerwood": "infested_towerwood",
    "castle_brick": "castle_brick",
    "worn_castle_brick": "worn_castle_brick",
    "cracked_castle_brick": "cracked_castle_brick",
    "castle_roof_tile": "castle_roof_tile",
    "mossy_castle_brick": "mossy_castle_brick",
    "thick_castle_brick": "thick_castle_brick",
    "encased_castle_brick_pillar": "encased_castle_brick_pillar",
    "encased_castle_brick_tile": "encased_castle_brick_tile",
    "bold_castle_brick_pillar": "bold_castle_brick_pillar",
    "bold_castle_brick_tile": "bold_castle_brick_tile",
    "castle_brick_stairs": "castle_brick_stairs",
    "worn_castle_brick_stairs": "worn_castle_brick_stairs",
    "cracked_castle_brick_stairs": "cracked_castle_brick_stairs",
    "mossy_castle_brick_stairs": "mossy_castle_brick_stairs",
    "encased_castle_brick_stairs": "encased_castle_brick_stairs",
    "bold_castle_brick_stairs": "bold_castle_brick_stairs",
    "pink_castle_rune_brick": "pink_castle_rune_brick",
    "blue_castle_rune_brick": "blue_castle_rune_brick",
    "yellow_castle_rune_brick": "yellow_castle_rune_brick",
    "violet_castle_rune_brick": "violet_castle_rune_brick",
    "deadrock": "deadrock",
    "cracked_deadrock": "cracked_deadrock",
    "weathered_deadrock": "weathered_deadrock",
    "huge_water_lily": "huge_water_lily",
    "uncrafting_table": "uncrafting_table",
    "cinder_log": "cinder_log",
    "cinder_wood": "cinder_wood",
    "ironwood_block": "ironwood_block",
    "steeleaf_block": "steeleaf_block",
    "knightmetal_block": "knightmetal_block",
    "fiery_block": "fiery_block",
}

# Leaves: the mod draws vanilla/its own grey leaf art tinted by a BlockColors
# handler (ColorHandler.java). Our leaves are cube_all with no tintindex, so the
# tint is baked into a texture of the block's own name.
LEAVES = ["twilight_oak_leaves", "canopy_leaves", "tf_mangrove_leaves", "dark_leaves",
          "hardened_dark_leaves"]


def colormap(name: str, temp: float, downfall: float) -> tuple[int, int, int]:
    # FoliageColor/GrassColor.get: downfall *= temp; x = (1-temp)*255, y = (1-downfall)*255.
    img = Image.open(OUT_TEX / "colormap" / f"{name}.png").convert("RGB")
    downfall *= temp
    x = int((1.0 - temp) * 255.0)
    y = int((1.0 - downfall) * 255.0)
    return img.getpixel((x, y))


def hexcol(h: str) -> tuple[int, int, int]:
    h = h.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def half_mix(c: tuple[int, int, int], add: int) -> tuple[int, int, int]:
    # ColorHandler CANOPY/MANGROVE_COLORIZER: ((color & 0xFEFEFE) + K) / 2.
    v = (((c[0] << 16) | (c[1] << 8) | c[2]) & 0xFEFEFE) + add
    v //= 2
    return (v >> 16) & 255, (v >> 8) & 255, v & 255


# TF biomes: forest = temperature 0.5 / downfall 0.5 (worldgen/biome/forest.json);
# swamp (mangroves) foliage #496137, dark forest foliage #3b5e3f (their JSON).
FOREST_FOLIAGE = colormap("foliage", 0.5, 0.5)
FOREST_GRASS = colormap("grass", 0.5, 0.5)
SWAMP_FOLIAGE = hexcol("#496137")
DARK_FOREST_FOLIAGE = hexcol("#3b5e3f")
AURORA_DEFAULT = tuple(int(round(c * 255)) for c in colorsys.hsv_to_rgb(0.45, 1.0, 1.0))  # auroraTint(null)

# baked texture name -> (source path, tint rgb, mcmeta source path or None, note)
BAKED: dict[str, tuple[Path, tuple[int, int, int], Path | None, str]] = {
    "twilight_oak_leaves": (OUT_TEX / "block/oak_leaves.png", FOREST_FOLIAGE,
                            OUT_TEX / "block/oak_leaves.png.mcmeta",
                            "vanilla oak_leaves x forest foliage colour (TF uses vanilla art, tinted)"),
    "canopy_leaves": (OUT_TEX / "block/spruce_leaves.png", half_mix(FOREST_FOLIAGE, 0x469A66),
                      OUT_TEX / "block/oak_leaves.png.mcmeta",
                      "vanilla spruce_leaves x CANOPY_COLORIZER(forest foliage)"),
    "tf_mangrove_leaves": (OUT_TEX / "block/birch_leaves.png", half_mix(SWAMP_FOLIAGE, 0xC0E694),
                           OUT_TEX / "block/oak_leaves.png.mcmeta",
                           "vanilla birch_leaves x MANGROVE_COLORIZER(swamp foliage)"),
    "dark_leaves": (TF_TEX / "block/dark_leaves.png", DARK_FOREST_FOLIAGE,
                    OUT_TEX / "block/oak_leaves.png.mcmeta",
                    "TF dark_leaves x dark forest foliage colour (adaptation)"),
    "fiddlehead": (TF_TEX / "block/fiddlehead.png", FOREST_GRASS,
                   None,
                   "TF fiddlehead x forest grass colour (adaptation)"),
    "aurora_block": (TF_TEX / "block/aurora_block_0.png", AURORA_DEFAULT,
                     TF_TEX / "block/aurora_block_0.png.mcmeta",
                     "TF aurora_block_0 x the mod's default aurora tint hsv(0.45,1,1) (adaptation)"),
}

LEAF_TEXTURE = {"twilight_oak_leaves": "twilight_oak_leaves", "canopy_leaves": "canopy_leaves",
                "tf_mangrove_leaves": "tf_mangrove_leaves", "dark_leaves": "dark_leaves",
                "hardened_dark_leaves": "dark_leaves"}

ITEMS = {"torchberries": "torchberries", "liveroot": "liveroot",
         # pass two: the material sets, naga scale + armour, venison, meef.
         "raw_ironwood": "raw_ironwood",
         "ironwood_ingot": "ironwood_ingot",
         "ironwood_helmet": "ironwood_helmet",
         "ironwood_chestplate": "ironwood_chestplate",
         "ironwood_leggings": "ironwood_leggings",
         "ironwood_boots": "ironwood_boots",
         "ironwood_sword": "ironwood_sword",
         "ironwood_shovel": "ironwood_shovel",
         "ironwood_pickaxe": "ironwood_pickaxe",
         "ironwood_axe": "ironwood_axe",
         "ironwood_hoe": "ironwood_hoe",
         "steeleaf_ingot": "steeleaf_ingot",
         "steeleaf_helmet": "steeleaf_helmet",
         "steeleaf_chestplate": "steeleaf_chestplate",
         "steeleaf_leggings": "steeleaf_leggings",
         "steeleaf_boots": "steeleaf_boots",
         "steeleaf_sword": "steeleaf_sword",
         "steeleaf_shovel": "steeleaf_shovel",
         "steeleaf_pickaxe": "steeleaf_pickaxe",
         "steeleaf_axe": "steeleaf_axe",
         "steeleaf_hoe": "steeleaf_hoe",
         "armor_shard": "armor_shard",
         "armor_shard_cluster": "armor_shard_cluster",
         "knightmetal_ingot": "knightmetal_ingot",
         "knightmetal_helmet": "knightmetal_helmet",
         "knightmetal_chestplate": "knightmetal_chestplate",
         "knightmetal_leggings": "knightmetal_leggings",
         "knightmetal_boots": "knightmetal_boots",
         "knightmetal_sword": "knightmetal_sword",
         "knightmetal_pickaxe": "knightmetal_pickaxe",
         "knightmetal_axe": "knightmetal_axe",
         "fiery_blood": "fiery_blood",
         "fiery_tears": "fiery_tears",
         "fiery_ingot": "fiery_ingot",
         "fiery_helmet": "fiery_helmet",
         "fiery_chestplate": "fiery_chestplate",
         "fiery_leggings": "fiery_leggings",
         "fiery_boots": "fiery_boots",
         "fiery_sword": "fiery_sword",
         "fiery_pickaxe": "fiery_pickaxe",
         "naga_scale": "naga_scale",
         "naga_chestplate": "naga_chestplate",
         "naga_leggings": "naga_leggings",
         "raw_venison": "raw_venison",
         "cooked_venison": "cooked_venison",
         "raw_meef": "raw_meef",
         "cooked_meef": "cooked_meef",
}
# pass two: the worn-armour sheets (TFEquipmentAssets) for the renderer's
# textures/entity/equipment/humanoid[_leggings]/<asset>.png.
EQUIPMENT = ["fiery", "ironwood", "knightmetal", "naga_scale", "steeleaf"]

# pass two: blocks whose mod blockstate reads properties this port drops
# (docs/mod-ports.md). Thorns keep their axis: the connection parts along the
# axis are baked in as connected, the others as unconnected. Trollsteinn is
# always unlit: every face is the plain face.
DROPPED_PROPS = {
    "brown_thorns": ("brown_thorns", "thorns"),
    "green_thorns": ("green_thorns", "thorns"),
    "burnt_thorns": ("burnt_thorns", "thorns"),
    "trollsteinn": ("trollsteinn", "unlit"),
}
ENTITIES = {"deer": "wilddeer", "boar": "wildboar", "bighorn_sheep": "bighorn",
            "tiny_bird": "tinybirdblue", "kobold": "kobold", "redcap": "redcap",
            # Pass two (docs/mod-ports.md): the rest of the biome spawners'
            # creatures, the tiny bird / dwarf rabbit variants, and the
            # landmark hostiles. Names follow each renderer's getModelTexture.
            "tiny_bird_brown": "tinybirdbrown", "tiny_bird_gold": "tinybirdgold",
            "tiny_bird_red": "tinybirdred",
            "squirrel": "squirrel2", "raven": "raven", "penguin": "penguin",
            "dwarf_rabbit_brown": "bunnybrown", "dwarf_rabbit_dutch": "bunnydutch",
            "dwarf_rabbit_white": "bunnywhite",
            "king_spider": "kingspider", "mist_wolf": "mistwolf", "winter_wolf": "winterwolf",
            "mosquito_swarm": "mosquitoswarm", "skeleton_druid": "skeletondruid",
            "yeti": "yeti2",
            "hedge_spider": "hedgespider", "swarm_spider": "swarmspider", "wraith": "ghost",
            "fire_beetle": "firebeetle", "slime_beetle": "slimebeetle",
            "pinch_beetle": "pinchbeetle", "helmet_crab": "helmetcrab",
            "helmet_crab_blue": "helmetcrabblue", "troll": "troll",
            "towerwood_borer": "towertermite", "maze_slime": "mazeslime",
            "minotaur": "minotaur", "redcap_sapper": "redcapsapper",
            "block_and_chain_goblin": "blockgoblin", "block_and_chain": "block_and_chain",
            "goblin_knight": "doublegoblin"}


def our_name(mod_name: str) -> str:
    """Mod texture/model basename -> ours (mangrove_* -> tf_mangrove_*)."""
    if mod_name.startswith("mangrove_"):
        return "tf_" + mod_name
    if mod_name.startswith("stripped_mangrove_"):
        return "stripped_tf_" + mod_name[len("stripped_"):]
    return mod_name


def strip_ns(ref: str) -> tuple[str, str]:
    """'twilightforest:block/x' -> ('twilightforest', 'block/x'); 'block/x' -> ('minecraft', ...)."""
    if ":" in ref:
        ns, path = ref.split(":", 1)
        return ns, path
    return "minecraft", ref


# ── output collection ───────────────────────────────────────────────────────
class Out:
    def __init__(self) -> None:
        self.files: dict[Path, bytes] = {}
        self.attribution: list[tuple[str, str]] = []   # (our path, source/notes)

    def put(self, path: Path, data: bytes, source: str | None = None) -> None:
        if path in self.files and self.files[path] != data:
            raise SystemExit(f"conflicting outputs for {path}")
        self.files[path] = data
        if source is not None:
            self.attribution.append((str(path.relative_to(REPO)), source))

    def put_json(self, path: Path, obj) -> None:
        self.put(path, (json.dumps(obj, indent=2) + "\n").encode())


OUT = Out()


def png_bytes(arr: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(buf, format="PNG", optimize=False)
    return buf.getvalue()


# ── textures ────────────────────────────────────────────────────────────────
def copy_block_texture(mod_tex: str) -> str:
    """Copy twilightforest block texture `mod_tex` (+ .mcmeta); return our name."""
    ours = our_name(mod_tex)
    if ours in BAKED:
        bake(ours)
        return ours
    src = TF_TEX / "block" / f"{mod_tex}.png"
    if not src.exists():
        raise SystemExit(f"TF texture not found: {src}")
    dst = OUT_TEX / "block" / f"{ours}.png"
    OUT.put(dst, src.read_bytes(), f"twilightforest textures/block/{mod_tex}.png")
    meta = src.with_suffix(".png.mcmeta")
    if meta.exists():
        OUT.put(dst.with_suffix(".png.mcmeta"), meta.read_bytes(),
                f"twilightforest textures/block/{mod_tex}.png.mcmeta")
    return ours


def bake(name: str) -> None:
    src, tint, meta, note = BAKED[name]
    arr = np.array(Image.open(src).convert("RGBA")).astype(np.float64)
    arr[..., :3] = np.round(arr[..., :3] * (np.array(tint, dtype=np.float64) / 255.0))
    dst = OUT_TEX / "block" / f"{name}.png"
    from_tf = src.is_relative_to(TF)
    OUT.put(dst, png_bytes(arr.astype(np.uint8)),
            f"baked: {note}; tint #{tint[0]:02x}{tint[1]:02x}{tint[2]:02x}" if from_tf else None)
    if meta is not None:
        OUT.put(dst.with_suffix(".png.mcmeta"), meta.read_bytes(),
                f"twilightforest {meta.relative_to(TF).as_posix()}" if meta.is_relative_to(TF) else None)


# ── models ──────────────────────────────────────────────────────────────────
def find_mod_model(name: str) -> Path:
    for base in (TF_GEN, TF_MAIN):
        p = base / "models" / "block" / f"{name}.json"
        if p.exists():
            return p
    raise SystemExit(f"TF model not found: {name}")


# The mod tints these in code; our copies are untinted, so point them at
# untinted parents / the baked texture instead.
PARENT_REMAP = {"minecraft:block/tinted_cross": "minecraft:block/cross"}
VANILLA_TEX_REMAP = {"minecraft:block/oak_leaves": "twilight_oak_leaves"}  # fallen_leaves


def translate_texture_ref(ref: str) -> str:
    if ref.startswith("#"):
        return ref
    ns, path = strip_ns(ref)
    if f"{ns}:{path}" in VANILLA_TEX_REMAP:
        name = VANILLA_TEX_REMAP[f"{ns}:{path}"]
        bake(name)
        return f"minecraft:block/{name}"
    if ns == "twilightforest":
        assert path.startswith("block/"), ref
        return "minecraft:block/" + copy_block_texture(path[len("block/"):])
    return f"minecraft:{path}"


def scrub(obj):
    """Drop render_type / neoforge keys / tintindex, recursively."""
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            if k in ("render_type", "tintindex", "loader") or k.startswith("neoforge"):
                continue
            out[k] = scrub(v)
        return out
    if isinstance(obj, list):
        return [scrub(v) for v in obj]
    return obj


def translate_model(mod_name: str) -> str:
    """Translate a TF block model (and its TF parents); return our model name."""
    ours = our_name(mod_name)
    path = OUT_MODEL / f"{ours}.json"
    if path in OUT.files:
        return ours
    src = find_mod_model(mod_name)
    model = scrub(json.loads(src.read_text()))
    if "parent" in model:
        ns, p = strip_ns(model["parent"])
        if ns == "twilightforest":
            model["parent"] = "minecraft:block/" + translate_model(p[len("block/"):])
        else:
            model["parent"] = PARENT_REMAP.get(f"minecraft:{p}", f"minecraft:{p}")
    if "textures" in model:
        model["textures"] = {k: translate_texture_ref(v) for k, v in model["textures"].items()}
    OUT.put_json(path, model)
    OUT.attribution.append((str(path.relative_to(REPO)),
                            f"twilightforest {src.relative_to(TF).as_posix()} (translated)"))
    return ours


def translate_blockstate(ours: str, mod_slug: str) -> None:
    src = TF_GEN / "blockstates" / f"{mod_slug}.json"
    bs = json.loads(src.read_text())

    def fix(v):
        if isinstance(v, list):
            return [fix(x) for x in v]
        v = dict(v)
        ns, p = strip_ns(v["model"])
        assert ns == "twilightforest", v
        v["model"] = "minecraft:block/" + translate_model(p[len("block/"):])
        return v

    if "variants" in bs:
        variants = {k: fix(v) for k, v in bs["variants"].items()}
        # A state that lacks the property matches every variant with one
        # "extra" and the engine breaks the tie on JSON order: put the state
        # the block should show without that property first.
        if ours == "torchberry_plant":
            variants = {k: variants[k] for k in ("has_torchberries=true", "has_torchberries=false")}
        out = {"variants": variants}
    else:
        raise SystemExit(f"unexpected blockstate shape for {mod_slug}")
    OUT.put_json(OUT_BS / f"{ours}.json", out)
    OUT.attribution.append((str((OUT_BS / f"{ours}.json").relative_to(REPO)),
                            f"twilightforest {src.relative_to(TF).as_posix()} (translated)"))


def single(ours: str, model: str) -> None:
    OUT.put_json(OUT_BS / f"{ours}.json", {"variants": {"": {"model": f"minecraft:block/{model}"}}})


# ── critters: Java ModelPart boxes -> JSON elements ─────────────────────────
# (texOffs u, v, box min x, y, z, size dx, dy, dz, part offset)
CRITTERS = {
    # FireflyModel.create(), 64x32, entity/firefly-tiny.png
    "firefly": ("firefly-tiny", 64, 32, [
        (0, 21, (-4.0, 7.9, -5.0), (8.0, 0.0, 10.0), (0, 0, 0)),     # legs
        (0, 11, (-2.0, 6.0, -4.0), (4.0, 2.0, 6.0), (0, 0, 0)),      # fat_body
        (0, 0, (-1.0, 6.9, -5.0), (2.0, 1.0, 8.0), (0, 0, 0)),       # skinny_body
        (20, 0, (-5.0, 5.9, -9.0), (10.0, 0.0, 10.0), (0, 0, 0)),    # glow
    ]),
    # CicadaModel.create(), 64x32, entity/cicada-model.png
    "cicada": ("cicada-model", 64, 32, [
        (0, 21, (-4.0, 7.9, -5.0), (8.0, 1.0, 9.0), (0, 0, 0)),      # legs
        (0, 11, (-2.0, 6.0, -4.0), (4.0, 2.0, 6.0), (0, 0, 0)),      # fat_body
        (0, 0, (-1.0, 7.0, -5.0), (2.0, 1.0, 8.0), (0, 0, 0)),       # skinny_body
        (20, 15, (1.0, 5.0, 2.0), (2.0, 2.0, 2.0), (0, 0, 0)),       # eye_1
        (20, 15, (-3.0, 5.0, 2.0), (2.0, 2.0, 2.0), (0, 0, 0)),      # eye_2
        (20, 0, (-4.0, 5.0, -7.0), (8.0, 1.0, 8.0), (0, 0, 0)),      # wings
    ]),
    # MoonwormModel.create(), 32x32, entity/moonworm.png
    "moonworm": ("moonworm", 32, 32, [
        (0, 4, (-1.0, -1.0, -1.0), (4.0, 2.0, 2.0), (-1.0, 7.0, 3.0)),  # shape1
        (0, 8, (-1.0, -1.0, -1.0), (2.0, 2.0, 4.0), (3.0, 7.0, 0.0)),   # shape2
        (0, 14, (-1.0, -1.0, -1.0), (2.0, 2.0, 2.0), (2.0, 7.0, -2.0)), # shape3
        (0, 0, (-1.0, -1.0, -1.0), (2.0, 2.0, 2.0), (-3.0, 7.0, 2.0)),  # head
    ]),
}


def entity_box_faces(u: float, v: float, mn, size):
    """MC ModelPart.Cube: (entity face, [(vertex xyz, (U, V))] x4) in model space."""
    x0, y0, z0 = mn
    x1, y1, z1 = x0 + size[0], y0 + size[1], z0 + size[2]
    w, h, d = size
    t0, t1, t2, t3 = (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)
    l0, l1, l2, l3 = (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)
    u0, u1, u2, u22, u3, u4 = u, u + d, u + d + w, u + d + w + w, u + d + w + d, u + d + w + d + w
    v0, v1, v2 = v, v + d, v + d + h

    def poly(verts, a, b, c, dd):
        # Polygon(vertices, u0=a, v0=b, u1=c, v1=dd): [0]->(c,b) [1]->(a,b) [2]->(a,dd) [3]->(c,dd)
        uvs = [(c, b), (a, b), (a, dd), (c, dd)]
        return list(zip(verts, uvs))

    return [
        ("DOWN", poly([l1, l0, t0, t1], u1, v0, u2, v1)),
        ("UP", poly([t2, t3, l3, l2], u2, v1, u22, v0)),
        ("WEST", poly([t0, l0, l3, t3], u0, v1, u1, v2)),
        ("NORTH", poly([t1, t0, t3, t2], u1, v1, u2, v2)),
        ("EAST", poly([l1, t1, t2, l2], u2, v1, u3, v2)),
        ("SOUTH", poly([l0, l1, l2, l3], u3, v1, u4, v2)),
    ]


# The block entity renderer does translate(0.5) * facing.getRotation() * rotZ(180)
# * rotY(180 + rand) * rotY(-yaw). Facing UP, rand = yaw = 0: block px =
# (8 + x, 8 - y, 8 - z). Entity face -> block face under that map:
FACE_MAP = {"DOWN": "up", "UP": "down", "WEST": "west", "EAST": "east", "NORTH": "south", "SOUTH": "north"}


def json_st(face: str, p, frm, to):
    """Normalised (s, t) of block point p in the JSON face's default-UV frame
    (BlockElement.uvsByFace: up (x, z), down (x, 16-z), north (16-x, 16-y),
    south (x, 16-y), west (z, 16-y), east (16-z, 16-y))."""
    X, Y, Z = p

    def n(val, lo, hi):
        return (val - lo) / (hi - lo)

    if face == "up":
        return n(X, frm[0], to[0]), n(Z, frm[2], to[2])
    if face == "down":
        return n(X, frm[0], to[0]), n(to[2] - Z + frm[2], frm[2], to[2])
    if face == "north":
        return n(to[0] - X + frm[0], frm[0], to[0]), n(to[1] - Y + frm[1], frm[1], to[1])
    if face == "south":
        return n(X, frm[0], to[0]), n(to[1] - Y + frm[1], frm[1], to[1])
    if face == "west":
        return n(Z, frm[2], to[2]), n(to[1] - Y + frm[1], frm[1], to[1])
    if face == "east":
        return n(to[2] - Z + frm[2], frm[2], to[2]), n(to[1] - Y + frm[1], frm[1], to[1])
    raise ValueError(face)


def rnd(x: float) -> float:
    r = round(x, 4)
    return int(r) if r == int(r) else r


def critter_model(name: str) -> dict:
    tex, tw, th, boxes = CRITTERS[name]
    elements = []
    for (u, v, mn, size, off) in boxes:
        mn = (mn[0] + off[0], mn[1] + off[1], mn[2] + off[2])
        mx = (mn[0] + size[0], mn[1] + size[1], mn[2] + size[2])
        frm = (8 + mn[0], 8 - mx[1], 8 - mx[2])
        to = (8 + mx[0], 8 - mn[1], 8 - mn[2])
        faces = {}
        for eface, verts in entity_box_faces(u, v, mn, size):
            face = FACE_MAP[eface]
            axis_extent = {"up": (0, 2), "down": (0, 2), "north": (0, 1), "south": (0, 1),
                           "west": (2, 1), "east": (2, 1)}[face]
            if any(to[i] - frm[i] == 0 for i in axis_extent):
                continue  # zero-area side of a flat plane
            corners = {}
            for (x, y, z), uv in verts:
                s, t = json_st(face, (8 + x, 8 - y, 8 - z), frm, to)
                corners[(round(s), round(t))] = uv
            a, b = corners[(0, 0)]
            c, d = corners[(1, 1)]
            assert corners[(1, 0)] == (c, b) and corners[(0, 1)] == (a, d), (name, eface, corners)
            faces[face] = {"uv": [rnd(a * 16 / tw), rnd(b * 16 / th), rnd(c * 16 / tw), rnd(d * 16 / th)],
                           "texture": "#critter"}
        elements.append({"from": [rnd(c) for c in frm], "to": [rnd(c) for c in to], "faces": faces})
    return {
        "ambientocclusion": False,
        "textures": {"critter": f"minecraft:block/{name}_critter", "particle": f"minecraft:block/{name}"},
        "elements": elements,
    }


def build_critters() -> None:
    for name, (tex, _, _, _) in CRITTERS.items():
        src = TF_TEX / "entity" / f"{tex}.png"
        OUT.put(OUT_TEX / "block" / f"{name}_critter.png", src.read_bytes(),
                f"twilightforest textures/entity/{tex}.png")
        copy_block_texture(name)  # the mod's 16x16 block sprite, used as the particle
        path = OUT_MODEL / f"{name}.json"
        OUT.put_json(path, critter_model(name))
        OUT.attribution.append((str(path.relative_to(REPO)),
                                f"geometry converted from twilightforest {name.capitalize()}Model.java"))
        # end_rod's rotations; facing=up first so a state without `facing` gets it.
        rot = {"up": {}, "down": {"x": 180}, "north": {"x": 90}, "south": {"x": 90, "y": 180},
               "west": {"x": 90, "y": 270}, "east": {"x": 90, "y": 90}}
        OUT.put_json(OUT_BS / f"{name}.json", {"variants": {
            f"facing={f}": {"model": f"minecraft:block/{name}", **r} for f, r in rot.items()}})


# ── hand-made models ────────────────────────────────────────────────────────
def build_leaves() -> None:
    for slug in LEAVES:
        tex = LEAF_TEXTURE[slug]
        bake(tex)
        OUT.put_json(OUT_MODEL / f"{slug}.json",
                     {"parent": "minecraft:block/cube_all", "textures": {"all": f"minecraft:block/{tex}"}})
        single(slug, slug)


def flat_patch(tex: str) -> dict:
    # The mod's plant_patch loader has no JSON; ours is a flat carpet plane
    # (the mod's fallen_leaves1 height), both sides drawn.
    return {
        "parent": "minecraft:block/thin_block",
        "ambientocclusion": False,
        "textures": {"particle": "#patch", "patch": tex},
        "elements": [{
            "from": [0, 0, 0], "to": [16, 0.25, 16],
            "faces": {
                "up": {"uv": [0, 0, 16, 16], "texture": "#patch"},
                "down": {"uv": [0, 16, 16, 0], "texture": "#patch", "cullface": "down"},
            },
        }],
    }


def build_misc() -> None:
    for slug, tex in (("clover_patch", "cloverpatch"), ("moss_patch", "mosspatch")):
        ours = copy_block_texture(tex)
        OUT.put_json(OUT_MODEL / f"{slug}.json", flat_patch(f"minecraft:block/{ours}"))
        single(slug, slug)
    bake("aurora_block")
    OUT.put_json(OUT_MODEL / "aurora_block.json",
                 {"parent": "minecraft:block/cube_all", "textures": {"all": "minecraft:block/aurora_block"}})
    single("aurora_block", "aurora_block")
    # TF models/block/twilight_portal.json: one upward plane at y = 13 on the
    # nether portal texture (the barrier overlay is not ported).
    OUT.put_json(OUT_MODEL / "twilight_portal.json", {
        "parent": "minecraft:block/block",
        "textures": {"all": "minecraft:block/nether_portal", "particle": "minecraft:block/nether_portal"},
        "elements": [{"from": [0, 13, 0], "to": [16, 13, 16],
                      "faces": {"up": {"uv": [0, 0, 16, 16], "texture": "#all"}}}],
    })
    single("twilight_portal", "twilight_portal")


def build_items_entities() -> None:
    for ours, mod in ITEMS.items():
        src = TF_TEX / "item" / f"{mod}.png"
        im = Image.open(src)
        if im.size[1] > im.size[0]:
            # An animated strip (the fiery set glows through a .mcmeta). Item
            # sprites here are static, so ship frame 0 only.
            frame = np.array(im.convert("RGBA"))[: im.size[0]]
            OUT.put(OUT_TEX / "item" / f"{ours}.png", png_bytes(frame),
                    f"twilightforest textures/item/{mod}.png (frame 0 of its animation)")
            continue
        OUT.put(OUT_TEX / "item" / f"{ours}.png", src.read_bytes(),
                f"twilightforest textures/item/{mod}.png")
    for ours, mod in ENTITIES.items():
        OUT.put(OUT_TEX / "entity" / "twilightforest" / f"{ours}.png",
                (TF_TEX / "entity" / f"{mod}.png").read_bytes(), f"twilightforest textures/entity/{mod}.png")


def _when_matches(when, state: dict) -> bool:
    if "AND" in when:
        return all(_when_matches(w, state) for w in when["AND"])
    if "OR" in when:
        return any(_when_matches(w, state) for w in when["OR"])
    return all(state.get(k) in v.split("|") for k, v in when.items())


def build_dropped_props() -> None:
    """Blockstates for the blocks whose mod properties are dropped: evaluate
    the mod's multipart at the fixed values this port uses."""
    for ours, (mod, mode) in DROPPED_PROPS.items():
        src = TF_GEN / "blockstates" / f"{mod}.json"
        parts = json.loads(src.read_text())["multipart"]

        def fix(apply: dict) -> dict:
            a = dict(apply)
            ns, p = strip_ns(a["model"])
            if not p.startswith("block/"):
                p = "block/" + p
            a["model"] = "minecraft:block/" + translate_model(p[len("block/"):])
            return a

        out_parts = []
        if mode == "thorns":
            along = {"y": ("up", "down"), "x": ("east", "west"), "z": ("north", "south")}
            for axis, conn in along.items():
                state = {"axis": axis, **{d: ("true" if d in conn else "false")
                                          for d in ("up", "down", "north", "south", "east", "west")}}
                for part in parts:
                    if _when_matches(part["when"], state):
                        out_parts.append({"apply": fix(part["apply"]), "when": {"axis": axis}})
        else:  # unlit: every `<face>=false` part, unconditionally
            state = {d: "false" for d in ("up", "down", "north", "south", "east", "west")}
            for part in parts:
                if _when_matches(part["when"], state):
                    out_parts.append({"apply": fix(part["apply"])})
        OUT.put_json(OUT_BS / f"{ours}.json", {"multipart": out_parts})
        # The inventory icon (BlockRegistry looks for `<model>_inventory`
        # first): the thorns' own pillar part / trollsteinn as a plain cube.
        if mode == "thorns":
            inv = {"parent": "minecraft:block/" + translate_model(f"{mod}_thorns")}
        else:
            inv = {"parent": "minecraft:block/cube_all",
                   "textures": {"all": "minecraft:block/" + copy_block_texture(mod)}}
        OUT.put_json(OUT_MODEL / f"{ours}_inventory.json", inv)
        OUT.attribution.append((str((OUT_BS / f"{ours}.json").relative_to(REPO)),
                                f"twilightforest {src.relative_to(TF).as_posix()} (translated; dropped properties baked)"))


def build_equipment() -> None:
    for asset in EQUIPMENT:
        for sub in ("humanoid", "humanoid_leggings"):
            src = TF_TEX / "entity" / "equipment" / sub / f"{asset}.png"
            OUT.put(OUT_TEX / "entity" / "equipment" / sub / f"{asset}.png", src.read_bytes(),
                    f"twilightforest textures/entity/equipment/{sub}/{asset}.png")


# ── verification ────────────────────────────────────────────────────────────
def model_file(ref: str) -> Path:
    ns, p = strip_ns(ref)
    assert ns == "minecraft", ref
    return ASSETS / "models" / f"{p}.json"


def verify() -> list[str]:
    errors = []

    def exists(p: Path) -> bool:
        return p in OUT.files or p.exists()

    def load(p: Path):
        return json.loads(OUT.files[p] if p in OUT.files else p.read_bytes())

    for path in [p for p in OUT.files if p.suffix == ".json"]:
        obj = load(path)
        if path.parent == OUT_BS:
            refs = []
            entries = (obj["variants"].values() if "variants" in obj
                       else [p["apply"] for p in obj["multipart"]])
            for v in entries:
                for e in (v if isinstance(v, list) else [v]):
                    refs.append(e["model"])
            for r in refs:
                if not exists(model_file(r)):
                    errors.append(f"{path.name}: model {r} missing")
        else:
            if "parent" in obj and not exists(model_file(obj["parent"])):
                errors.append(f"{path.name}: parent {obj['parent']} missing")
            for k, t in obj.get("textures", {}).items():
                if t.startswith("#"):
                    continue
                ns, p = strip_ns(t)
                if not exists(OUT_TEX / f"{p}.png"):
                    errors.append(f"{path.name}: texture {t} missing")
    return errors


def write_attribution() -> None:
    lines = [
        "# Third-party asset attribution",
        "",
        "## Twilight Forest",
        "",
        "Textures and block models from **The Twilight Forest** mod,",
        "© Benimatic and TeamTwilight, licensed under",
        "[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)]"
        "(https://creativecommons.org/licenses/by-nc-sa/4.0/).",
        "Source: https://github.com/TeamTwilight/twilightforest",
        "",
        "They ship here unchanged except where noted: model JSON is translated to this engine's",
        "namespaces (`twilightforest:` -> `minecraft:`, `mangrove_*` renamed `tf_mangrove_*`,",
        "render types and tint indices dropped); textures the mod colours in code have that colour",
        "baked in (\"baked\" rows) — those adaptations are shared under the same licence.",
        "Critter block models are converted from the mod's Java entity models. Files are",
        "(re)generated by `tools/copy_twilight_assets.py`.",
        "",
        "| File | Source |",
        "|---|---|",
    ]
    seen = set()
    for path, src in sorted(OUT.attribution):
        if path in seen:
            continue
        seen.add(path)
        lines.append(f"| `{path}` | {src} |")
    lines += [
        "",
        "## The Aether",
        "",
        "The Aether's textures and models are all rights reserved and are **not** used here.",
        "Every Aether-themed asset in this repository (`tools/gen_aether_textures.py`,",
        "`tools/gen_aether_entity_textures.py` and the files they write: every creature sheet",
        "and saddle in `assets/textures/entity/aether/`, holystone, aerclouds, skyroot, golden oak, the Aether portal, Aether",
        "flora, the dungeon stones, storage blocks, aerogel, quicksoil glass, the ambrosium torch,",
        "items, tools and armour (including `assets/textures/entity/equipment/humanoid*/zanite.png`",
        "and `gravitite.png`) and `assets/textures/entity/aether/`) is an original work made for",
        "this project; the mod's LGPL code was consulted only for model UV layouts and texture sizes.",
        "Vanilla Minecraft art used as a base (pig, cow, sheep, leaves, nether portal, and the",
        "recoloured vanilla tool, armour, bucket, key, torch, door and storage-block sprites) is Mojang's.",
        "",
    ]
    OUT.put(ASSETS / "ATTRIBUTION.md", "\n".join(lines).encode())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare against disk; exit 1 on drift")
    args = ap.parse_args()

    for ours, mod in BLOCKS.items():
        translate_blockstate(ours, mod)
    build_leaves()
    build_critters()
    build_misc()
    build_items_entities()
    build_dropped_props()
    build_equipment()

    errors = verify()
    if errors:
        for e in errors:
            print("UNRESOLVED", e)
        return 1
    write_attribution()

    drift = 0
    for path, data in sorted(OUT.files.items()):
        rel = path.relative_to(REPO)
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                print(f"DRIFT    {rel}")
                drift += 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f"wrote    {rel}")
    print(f"{len(OUT.files)} files" + (f", {drift} drift" if args.check else ""))
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
