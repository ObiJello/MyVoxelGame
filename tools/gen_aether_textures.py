#!/usr/bin/env python3
# tools/gen_aether_textures.py
#
# Deterministic, ORIGINAL textures for the Aether port. The Aether mod's art is
# all rights reserved: nothing here is copied or traced from it. The mod's LGPL
# model classes were read only for texture sizes and the `texOffs(u, v)
# .addBox(...)` UV regions each part samples; every pixel is painted here, from
# seeded noise posterized into a few flat shades (the way vanilla 16x16 art is
# painted), from hand-plotted sprites, or as a recolour of a VANILLA sprite that
# keeps its alpha and layout. Helpers come from tools/gen_hush_textures.py.
# Re-running always produces identical bytes; `--check` proves it.
#
#   python3 tools/gen_aether_textures.py              # write every PNG (+ .mcmeta)
#   python3 tools/gen_aether_textures.py --only holystone,zanite_ore
#   python3 tools/gen_aether_textures.py --preview out.png   # contact sheet
#   python3 tools/gen_aether_textures.py --check       # byte-compare, exit 1 on drift
#
# Block textures (assets/textures/block/): holystone, mossy_holystone,
#   holystone_bricks, aether_dirt, aether_grass_block_top/_side, quicksoil,
#   cold/blue/golden_aercloud (alpha ~180), icestone, ambrosium/zanite/gravitite
#   _ore (holystone + the vanilla ore's vein shape in a new colour),
#   skyroot_log(+_top), stripped_skyroot_log(+_top), skyroot_planks,
#   skyroot_leaves + golden_oak_leaves (oak_leaves' hole mask; + oak_leaves'
#   .mcmeta), skyroot_sapling, golden_oak_log(+_top), golden_oak_sapling,
#   aether_portal (nether_portal.png's 32-frame motion, hue 200, luminance-
#   matched and lifted; + nether_portal's .mcmeta), berry_bush, berry_bush_stem,
#   white_flower, purple_flower.
# Item textures (assets/textures/item/): ambrosium_shard, zanite_gemstone, blue_berry.
# Entity textures (assets/textures/entity/aether/): phyg (pig layout, 64x64) +
#   phyg_wings (QuadrupedWingsModel, 64x32), flying_cow (cow layout, 64x64) +
#   flying_cow_wings, sheepuff (sheep layout 64x32) + sheepuff_wool,
#   cockatrice (BipedBirdModel, 128x64 = 64x32 UV space at 2 px per unit),
#   zephyr (ZephyrModel, 128x32).
# Pass two (docs/mod-ports.md): zanite/ambrosium blocks + enchanted gravitite
#   (vanilla storage-block facets, new ramps), aerogel and quicksoil glass
#   (translucent), the ambrosium torch (torch.png recoloured), skyroot door/
#   trapdoor (oak's, recoloured), the dungeon stones (carved/sentry, angelic,
#   hellfire + lights: seeded masonry tiles with an inset groove and a glowing
#   inlay), pillar side/end/capital; items (skyroot stick, golden amber, swet
#   ball, aechor petal, enchanted berry, white apple, gummy swets, skyroot
#   buckets, dungeon keys), the skyroot/holystone/zanite/gravitite tools (the
#   vanilla tool's head recoloured, its handle a skyroot stick), zanite and
#   gravitite armour items and their worn sheets under
#   entity/equipment/humanoid[_leggings]/ (vanilla iron's layout, recoloured).
#   The stairs/slab/wall/fence/gate/door/trapdoor/button/plate/torch/wood
#   blockstates and models are vanilla's families re-pointed at these textures
#   (PASS2_FAMILIES).
#
# Seeding: default_rng([0x41455448 ("AETH"), crc32(name)]) per texture.

from __future__ import annotations

import argparse
import colorsys
import io
import json
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_hush_textures as hush  # noqa: E402  (shared noise / palette helpers)
from gen_hush_textures import (  # noqa: E402
    blank, fbm, gradient_map, lerp_palette, load_vanilla, luminance, paint, plot_art,
    posterize, recolor, rgb, value_noise,
)

REPO_ROOT = hush.REPO_ROOT
TEX_ROOT = hush.TEX_ROOT
SEED_TAG = 0x41455448  # "AETH"
W = H = 16


def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng([SEED_TAG, zlib.crc32(name.encode("utf-8"))])


def mix(a, b, t: float):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3)) + (a[3] if len(a) > 3 else 255,)


def ramp_by_rank(src: np.ndarray, mask: np.ndarray, palette: list[str]) -> np.ndarray:
    """Recolour the masked pixels of src: distinct luminance levels, darkest
    first, spread evenly over the palette. Alpha kept."""
    out = src.copy()
    lum = np.round(0.299 * src[..., 0] + 0.587 * src[..., 1] + 0.114 * src[..., 2]).astype(int)
    levels = sorted(set(lum[mask].tolist()))
    pal = [rgb(c) for c in palette]
    for y, x in zip(*np.nonzero(mask)):
        k = levels.index(lum[y, x])
        i = 0 if len(levels) == 1 else int(round(k * (len(pal) - 1) / (len(levels) - 1)))
        out[y, x, :3] = pal[i][:3]
    return out


# ── holystone family ────────────────────────────────────────────────────────
HOLY = ["#a3abb6", "#b3bac4", "#c4cad2", "#d8dde3", "#e7eaee"]
HOLY_WEIGHTS = [16, 52, 92, 68, 28]


def holystone_index(rng) -> np.ndarray:
    # stone.png: soft blotches a few px across plus fine speckle.
    field = fbm(rng, [(4, 4, 0.45), (2, 2, 0.30)], white=0.30)
    return posterize(field, HOLY_WEIGHTS)


def gen_holystone(rng):
    return paint(holystone_index(rng), [rgb(c) for c in HOLY])


def gen_mossy_holystone(rng):
    out = gen_holystone(rng_for("holystone"))
    moss = [rgb(c) for c in ("#8fbf8a", "#a2d09a", "#b7e0ad", "#cbeec0")]
    patch = fbm(rng, [(8, 8, 0.6), (4, 4, 0.3)], white=0.15)
    detail = rng.random((H, W))
    cut = np.quantile(patch, 0.64)
    for y in range(H):
        for x in range(W):
            if patch[y, x] >= cut or (patch[y, x] >= cut - 0.04 and detail[y, x] > 0.55):
                depth = (patch[y, x] - cut + 0.04) / (patch.max() - cut + 0.04)
                k = min(3, int(depth * 3.2 + detail[y, x] * 0.9))
                out[y, x] = moss[k]
    return out


def gen_holystone_bricks(rng):
    # Light 8x4 bricks: four 4-px courses, mortar on each course's last row,
    # vertical joints every 8 px staggered by 4 on alternate courses.
    mortar = rgb("#9aa2ae")
    hi = rgb("#eef0f3")
    lo = rgb("#aeb5c0")
    field = fbm(rng, [(4, 2, 0.45), (2, 2, 0.3)], white=0.25)
    out = paint(posterize(field, [30, 45, 25]), [rgb(c) for c in ("#c4cad2", "#cfd4db", "#d8dde3")])
    for course in range(4):
        y0 = course * 4
        joints = (7, 15) if course % 2 == 0 else (3, 11)
        out[y0 + 3, :] = mortar
        for x in range(W):
            out[y0, x] = hi
            out[y0 + 2, x] = lo if rng.random() < 0.7 else out[y0 + 2, x]
        for j in joints:
            out[y0:y0 + 4, j] = mortar
            out[y0:y0 + 3, (j + 1) % 16] = hi
            out[y0 + 1:y0 + 3, (j - 1) % 16] = lo
    return out


def ore_on_holystone(vanilla_ore: str, palette: list[str], rim: str) -> np.ndarray:
    # The vanilla ore's vein shape (pixels that differ strongly from stone)
    # recoloured, on our holystone; the darker outline pixels become `rim`.
    base = gen_holystone(rng_for("holystone"))
    ore = load_vanilla(f"block/{vanilla_ore}.png").astype(int)
    stone = load_vanilla("block/stone.png").astype(int)
    diff = np.abs(ore[..., :3] - stone[..., :3]).sum(axis=2)
    sat = np.zeros((H, W))
    lum = np.zeros((H, W))
    for y in range(H):
        for x in range(W):
            r, g, b = ore[y, x, :3] / 255.0
            _, s, v = colorsys.rgb_to_hsv(r, g, b)
            sat[y, x] = s
            lum[y, x] = luminance(*ore[y, x, :3])
    vein = (diff > 60) & ((sat > 0.18) | (lum < 70))
    outline = (diff > 45) & ~vein & (lum < 110)
    out = base.copy()
    colored = ramp_by_rank(ore.astype(np.uint8), vein, palette)
    out[vein] = colored[vein]
    out[vein, 3] = 255
    out[outline] = rgb(rim)
    return out


def gen_ambrosium_ore(rng):
    del rng
    return ore_on_holystone("coal_ore", ["#c9702a", "#e8913a", "#ffb347", "#ffd27f", "#fff0c4"], "#8f8a86")


def gen_zanite_ore(rng):
    del rng
    return ore_on_holystone("diamond_ore", ["#141c52", "#1f2b78", "#2a3a9a", "#4658c4", "#8190e6"], "#7d8698")


def gen_gravitite_ore(rng):
    del rng
    return ore_on_holystone("redstone_ore", ["#7a2440", "#a83552", "#e05a7a", "#f28aa2", "#ffc4d2"], "#8f8594")


# ── soils, clouds, ice ──────────────────────────────────────────────────────
DIRT = ["#a8977f", "#b8a78f", "#c9b9a5", "#d6c8b6", "#e2d6c6"]


def gen_aether_dirt(rng):
    field = fbm(rng, [(2, 2, 0.45), (4, 4, 0.2)], white=0.45)
    return paint(posterize(field, [14, 48, 100, 66, 28]), [rgb(c) for c in DIRT])


GRASS = ["#7fcf9b", "#94e2ae", "#a3f0bd", "#b1ffcb", "#c6ffda", "#dcffe8"]


def gen_aether_grass_block_top(rng):
    # grass_block_top's own fine blades (it is greyscale, tinted in game);
    # ours is untinted, so its levels map onto a baked pale-mint ramp.
    del rng
    src = load_vanilla("block/grass_block_top.png")
    return ramp_by_rank(src, src[..., 3] > 0, GRASS)


def gen_aether_grass_block_side(rng):
    # Aether dirt under grass_block_side_overlay's rim shape (its jagged fringe),
    # the overlay's grey levels as the mint ramp.
    del rng
    out = gen_aether_dirt(rng_for("aether_dirt"))
    ov = load_vanilla("block/grass_block_side_overlay.png")
    mask = ov[..., 3] > 0
    rim = ramp_by_rank(ov, mask, GRASS[:5])
    out[mask] = rim[mask]
    out[..., 3] = 255
    return out


def gen_quicksoil(rng):
    # sand.png's grain, pale gold, with a few glinting grains.
    src = load_vanilla("block/sand.png")
    out = ramp_by_rank(src, src[..., 3] > 0, ["#cdb574", "#dcc88a", "#e7d69a", "#f0e2a8", "#f7ecc0", "#fcf6dc"])
    for _ in range(6):
        x, y = int(rng.integers(0, 16)), int(rng.integers(0, 16))
        out[y, x] = rgb("#fffbea")
    return out


def cloud(rng, palette: list[str], alpha: int) -> np.ndarray:
    field = fbm(rng, [(8, 8, 0.5), (4, 4, 0.3), (2, 2, 0.12)], white=0.08)
    out = paint(posterize(field, [18, 34, 30, 18]), [rgb(c) for c in palette])
    out[..., 3] = alpha
    return out


def gen_cold_aercloud(rng):
    return cloud(rng, ["#dfe6ee", "#eaeff4", "#f4f7fa", "#ffffff"], 180)


def gen_blue_aercloud(rng):
    return cloud(rng, ["#6fb2f2", "#80bdfa", "#8fc7ff", "#a9d5ff"], 180)


def gen_golden_aercloud(rng):
    return cloud(rng, ["#f0cf6a", "#f8db7c", "#ffe58f", "#fff0b8"], 180)


def gen_icestone(rng):
    # packed_ice's streaked facets in a paler ice ramp, a few bright glints.
    src = load_vanilla("block/packed_ice.png")
    out = ramp_by_rank(src, src[..., 3] > 0, ["#a7c9ea", "#bcd8f2", "#cde4f8", "#dbeeff", "#eaf5ff", "#f8fcff"])
    for _ in range(4):
        x, y = int(rng.integers(1, 15)), int(rng.integers(1, 15))
        out[y, x] = rgb("#ffffff")
    out[..., 3] = 255
    return out


# ── skyroot / golden oak ────────────────────────────────────────────────────
BARK = ["#46505d", "#555f6d", "#616d7c", "#6d7a8a", "#7c8999", "#8b98a8"]
RING = ["#7f8b9a", "#97a3b1", "#adb8c4", "#c0c9d3", "#d0d8e0", "#dde3ea"]
PLANK = ["#7f8b99", "#95a1ae", "#a7b2be", "#b6c0cb", "#c4cdd6", "#d2d9e0"]
STRIPPED = ["#8e9aa8", "#9ca8b5", "#aab5c1", "#b8c2cc", "#c7cfd8"]
SKY_LEAF = ["#5a9a7a", "#6bab8b", "#7fbf9f", "#98d4b5"]
GOLD_LEAF = ["#cf9528", "#e2aa37", "#f2c14e", "#fbd97f"]


def gen_skyroot_log(rng):
    del rng
    src = load_vanilla("block/oak_log.png")
    return ramp_by_rank(src, src[..., 3] > 0, BARK)


def log_top_from(bark: list[str], rings: list[str]) -> np.ndarray:
    # oak_log_top: its bark rim (the darkest levels, on the border) becomes
    # our bark, the rings map onto the pale ring ramp.
    src = load_vanilla("block/oak_log_top.png")
    rim = np.zeros((H, W), dtype=bool)
    rim[0, :] = rim[-1, :] = rim[:, 0] = rim[:, -1] = True
    out = ramp_by_rank(src, ~rim, rings)
    out2 = ramp_by_rank(src, rim, bark[1:5])
    out[rim] = out2[rim]
    return out


def gen_skyroot_log_top(rng):
    del rng
    return log_top_from(BARK, RING)


def gen_stripped_skyroot_log(rng):
    del rng
    src = load_vanilla("block/stripped_oak_log.png")
    return ramp_by_rank(src, src[..., 3] > 0, STRIPPED)


def gen_stripped_skyroot_log_top(rng):
    del rng
    return log_top_from(STRIPPED, RING)


def gen_skyroot_planks(rng):
    del rng
    src = load_vanilla("block/oak_planks.png")
    return ramp_by_rank(src, src[..., 3] > 0, PLANK)


def gen_golden_oak_log(rng):
    # Skyroot bark with gold running in the bark's light furrows.
    out = gen_skyroot_log(rng_for("skyroot_log"))
    lum = out[..., :3].astype(int).sum(axis=2)
    cut = np.quantile(lum, 0.80)
    # A few 1-px veins of gold, 2-5 px long, seeded on the bark's lightest ridges.
    gold = [rgb("#d9a23a"), rgb("#f2c14e"), rgb("#ffe08a")]
    seeds = [(y, x) for y in range(H) for x in range(W) if lum[y, x] >= cut]
    order = rng.permutation(len(seeds))
    placed = 0
    for i in order[:9]:
        y, x = seeds[i]
        run = int(rng.integers(2, 6))
        for k in range(run):
            out[(y + k) % H, x] = gold[1] if k else gold[2]
            placed += 1
        out[(y + run) % H, x] = gold[0]
    return out


def gen_golden_oak_log_top(rng):
    # Skyroot's rings, warmed a little, with gold flecks in the bark rim.
    out = log_top_from(BARK, ["#a39c8f", "#b8b1a2", "#cac3b2", "#d9d2c0", "#e5dfcd", "#efe9d8"])
    for i in range(16):
        side = int(rng.integers(0, 4))
        k = int(rng.integers(0, 16))
        y, x = [(0, k), (15, k), (k, 0), (k, 15)][side]
        if rng.random() < 0.5:
            out[y, x] = rgb("#f2c14e" if rng.random() < 0.6 else "#ffe08a")
    return out


def leaves_on_oak_mask(palette: list[str]) -> np.ndarray:
    src = load_vanilla("block/oak_leaves.png")
    return ramp_by_rank(src, src[..., 3] > 0, palette)


def gen_skyroot_leaves(rng):
    del rng
    return leaves_on_oak_mask(SKY_LEAF)


def gen_golden_oak_leaves(rng):
    del rng
    return leaves_on_oak_mask(GOLD_LEAF)


def sapling(leaf: list[str], trunk: list[str]) -> np.ndarray:
    # oak_sapling's silhouette: green pixels -> leaf ramp, brown -> trunk ramp.
    src = load_vanilla("block/oak_sapling.png")
    greenish = np.zeros((H, W), dtype=bool)
    for y in range(H):
        for x in range(W):
            if src[y, x, 3] == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(*(src[y, x, :3] / 255.0))
            greenish[y, x] = 60 / 360 < h < 180 / 360 and s > 0.2
    opaque = src[..., 3] > 0
    out = ramp_by_rank(src, greenish, leaf)
    out2 = ramp_by_rank(src, opaque & ~greenish, trunk)
    out[opaque & ~greenish] = out2[opaque & ~greenish]
    return out


def gen_skyroot_sapling(rng):
    del rng
    return sapling(SKY_LEAF, BARK[1:5])


def gen_golden_oak_sapling(rng):
    del rng
    return sapling(GOLD_LEAF, BARK[1:5])


# ── portal ──────────────────────────────────────────────────────────────────
AETHER_PORTAL_HUE = 200.0
AETHER_PORTAL_LIFT = (0.42, 0.62)  # target Y = a + b * source Y: bright, keeps the swirl


def gen_aether_portal(rng):
    # nether_portal's 32-frame motion; each pixel gets hue 200, and V solved so
    # its Rec.601 luminance is the source's lifted by AETHER_PORTAL_LIFT (the
    # Aether portal is a bright sky blue, the nether's is dark purple). Alpha kept.
    del rng
    src = load_vanilla("block/nether_portal.png")
    assert src.shape == (512, 16, 4), src.shape
    out = src.copy()
    cache: dict[tuple[int, int, int], tuple[int, int, int]] = {}
    flat = out.reshape(-1, 4)
    a, b = AETHER_PORTAL_LIFT
    for i in range(flat.shape[0]):
        key = (int(flat[i, 0]), int(flat[i, 1]), int(flat[i, 2]))
        if key not in cache:
            target = min(1.0, a + b * luminance(*key) / 255.0)
            _h, s, _v = colorsys.rgb_to_hsv(key[0] / 255.0, key[1] / 255.0, key[2] / 255.0)
            h = AETHER_PORTAL_HUE / 360.0
            s = min(1.0, s * 0.85)
            unit = colorsys.hsv_to_rgb(h, s, 1.0)
            k = luminance(*unit)
            v = min(1.0, target / k)
            if target > k:  # past full value: desaturate toward white to keep reaching Y
                s = max(0.0, s * (1.0 - (target - k) / (1.0 - k)))
            nr, ng, nb = colorsys.hsv_to_rgb(h, s, v)
            cache[key] = (int(round(nr * 255)), int(round(ng * 255)), int(round(nb * 255)))
        flat[i, 0:3] = cache[key]
    assert (out[..., 3] == src[..., 3]).all()
    return out


# ── flora ───────────────────────────────────────────────────────────────────
def gen_berry_bush(rng):
    # sweet_berry_bush_stage3's bush: leaves -> Aether green, berries -> blue.
    del rng
    src = load_vanilla("block/sweet_berry_bush_stage3.png")

    def fn(x, y, h, s, v, a):
        if (h < 25 or h > 330) and s > 0.35:                   # berry
            return 215.0, min(1.0, s * 0.9 + 0.1), min(1.0, v * 1.05 + 0.05), a
        if h < 60 and s > 0.2:                                  # stem / dark bits
            return 205.0, s * 0.35, v * 1.05, a
        return 150.0, s * 0.8, min(1.0, v * 1.15 + 0.08), a    # leaves: pale blue-green

    return recolor(src, fn)


def gen_berry_bush_stem(rng):
    # dead_bush's bare twigs, pale grey-blue like skyroot bark.
    del rng
    src = load_vanilla("block/dead_bush.png")
    return ramp_by_rank(src, src[..., 3] > 0, ["#5b6674", "#6d7a8a", "#7f8c9c", "#95a1ae"])


WHITE_FLOWER_ART = [
    "................",
    "................",
    "......w..w......",
    ".....wWwwWw.....",
    "......wyyw......",
    ".....wWyyWw.....",
    "......w..w......",
    ".......ss.......",
    ".......s........",
    ".....l.s........",
    "....lll.s..ll...",
    "......lsl.ll....",
    ".......sll......",
    ".......s........",
    ".......s........",
    "................",
]
WHITE_FLOWER_KEY = {"w": "#e8eef4", "W": "#ffffff", "y": "#f2d77a", "s": "#6fae8f", "l": "#8fcaa9"}

PURPLE_FLOWER_ART = [
    "................",
    "................",
    ".......p........",
    "......pPp.......",
    ".....pPdPp......",
    ".....PdddP......",
    "......pdp.......",
    ".......s........",
    "...p...s........",
    "..pPp..s..l.....",
    "...s...s.ll.....",
    "...sl..sll......",
    "....s..s........",
    ".....l.s........",
    "......ss........",
    "................",
]
PURPLE_FLOWER_KEY = {"p": "#a978d8", "P": "#c9a0f0", "d": "#7a4fb0", "s": "#6fae8f", "l": "#8fcaa9"}


def gen_white_flower(rng):
    del rng
    return plot_art(WHITE_FLOWER_ART, WHITE_FLOWER_KEY)


def gen_purple_flower(rng):
    del rng
    return plot_art(PURPLE_FLOWER_ART, PURPLE_FLOWER_KEY)


# ── items ───────────────────────────────────────────────────────────────────
def gen_ambrosium_shard(rng):
    del rng
    src = load_vanilla("item/amethyst_shard.png")
    return ramp_by_rank(src, src[..., 3] > 0, ["#7a3f12", "#b8641e", "#e8913a", "#ffb347", "#ffd27f", "#fff2cc"])


def gen_zanite_gemstone(rng):
    del rng
    src = load_vanilla("item/diamond.png")
    return ramp_by_rank(src, src[..., 3] > 0, ["#0e1340", "#1c2770", "#2a3a9a", "#3f55c2", "#6f86e6", "#b8c6ff"])


def gen_blue_berry(rng):
    del rng
    src = load_vanilla("item/sweet_berries.png")

    def fn(x, y, h, s, v, a):
        if (h < 30 or h > 320) and s > 0.3:
            return 218.0, s, min(1.0, v * 1.05), a
        return 140.0, s * 0.7, min(1.0, v * 1.1), a

    return recolor(src, fn)


# ── entity helpers ──────────────────────────────────────────────────────────
def cube_rects(u: float, v: float, dx: float, dy: float, dz: float) -> dict[str, tuple[float, float, float, float]]:
    """MC ModelPart.Cube UV rects (u0, v0, u1, v1), UV units, per entity face."""
    return {
        "top": (u + dz, v, u + dz + dx, v + dz),                 # DOWN polygon (visible top)
        "bottom": (u + dz + dx, v, u + dz + dx + dx, v + dz),    # UP polygon (visible bottom)
        "west": (u, v + dz, u + dz, v + dz + dy),
        "front": (u + dz, v + dz, u + dz + dx, v + dz + dy),     # NORTH (-z, the face)
        "east": (u + dz + dx, v + dz, u + dz + dx + dz, v + dz + dy),
        "back": (u + dz + dx + dz, v + dz, u + dz + dx + dz + dx, v + dz + dy),
    }


def fill_rect(img: np.ndarray, rect, scale: int, fn) -> None:
    """fn(s, t, px, py) -> RGBA for every pixel of a UV rect (s, t in 0..1)."""
    u0, v0, u1, v1 = (int(round(c * scale)) for c in rect)
    for py in range(v0, v1):
        for px in range(u0, u1):
            s = (px - u0 + 0.5) / max(1, u1 - u0)
            t = (py - v0 + 0.5) / max(1, v1 - v0)
            img[py, px] = fn(s, t, px, py)


def lighten(src: np.ndarray, toward: str, t: float) -> np.ndarray:
    out = src.copy()
    c = np.array(rgb(toward)[:3], dtype=np.float64)
    m = src[..., 3] > 0
    out[m, :3] = np.round(src[m, :3] * (1 - t) + c * t).astype(np.uint8)
    return out


# ── winged animals ──────────────────────────────────────────────────────────
def gen_wings(rng) -> np.ndarray:
    # QuadrupedWingsModel (64x32): inner panel texOffs(0,0), outer texOffs(20,0),
    # both 2 x 16 x 8 boxes. On the broad faces (west/east, 8 x 16) t = 0 is the
    # panel's far end (y = -16), t = 1 its root; west's u runs rear -> front,
    # east's front -> rear. Feathers run the length of the wing, the rear rows
    # are the long flight feathers.
    img = np.zeros((32, 64, 4), dtype=np.uint8)
    white, pale, line, shade, flight, flight_dk = (rgb(c) for c in (
        "#ffffff", "#f1f4f8", "#d6dde6", "#e4e9ef", "#cfd8e3", "#b9c4d2"))
    jitter = rng.random((32, 64))

    def broad(outer: bool, rear_on_left: bool):
        def fn(s, t, px, py):
            rear = (1 - s) if rear_on_left else s   # 1 = the wing's trailing edge
            col = int(s * 8)
            # vertical feather lines every 2 px
            if col % 2 == 1 and jitter[py, px] < 0.85:
                base = line
            else:
                base = white if jitter[py, px] > 0.3 else pale
            if rear > 0.62:                      # flight feathers along the rear edge
                base = flight if col % 2 == 0 else flight_dk
            if not outer and t > 0.8:            # coverts at the root: soft rows
                base = shade if (py % 2 == 0) else pale
            if outer and t < 0.15 and rear > 0.4:  # tip: flight feathers darker ends
                base = flight_dk
            return base
        return fn

    def edge(s, t, px, py):
        return pale if (py % 2 == 0) else shade

    for u, outer in ((0, False), (20, True)):
        r = cube_rects(u, 0, 2, 16, 8)
        fill_rect(img, r["west"], 1, broad(outer, rear_on_left=True))
        fill_rect(img, r["east"], 1, broad(outer, rear_on_left=False))
        for k in ("top", "bottom", "front", "back"):
            fill_rect(img, r[k], 1, edge)
    return img


def gen_phyg(rng):
    # The pig, a shade paler and pinker: a sky pig.
    del rng
    return lighten(load_vanilla("entity/pig/temperate_pig.png"), "#ffe6ef", 0.22)


def gen_phyg_wings(rng):
    return gen_wings(rng)


def gen_flying_cow(rng):
    del rng
    return lighten(load_vanilla("entity/cow/temperate_cow.png"), "#f4f1ff", 0.15)


def gen_flying_cow_wings(rng):
    return gen_wings(rng)


def gen_sheepuff(rng):
    # sheep.png (the shorn body/face) in pale lavender-white.
    del rng
    src = load_vanilla("entity/sheep/sheep.png")
    return lighten(src, "#f3eefa", 0.5)


def gen_sheepuff_wool(rng):
    # sheep_wool.png, paler, with round puffs: every wool pixel is lifted, then
    # a cell pattern of 3x3 puffs gets a lit top-left and shaded bottom-right.
    src = load_vanilla("entity/sheep/sheep_wool.png")
    out = lighten(src, "#fbf9ff", 0.55)
    m = out[..., 3] > 0
    hh, ww = out.shape[:2]
    off = rng.integers(0, 3, size=2)
    for y in range(hh):
        for x in range(ww):
            if not m[y, x]:
                continue
            cx, cy = (x + off[0]) % 3, (y + (x // 3) % 2 + off[1]) % 3
            if cx == 0 and cy == 0:
                out[y, x, :3] = rgb("#ffffff")[:3]
            elif cx == 2 and cy == 2:
                out[y, x, :3] = rgb("#d2cbe2")[:3]
            elif cx == 2 or cy == 2:
                out[y, x, :3] = rgb("#e6e1f0")[:3]
    return out


# ── cockatrice (BipedBirdModel: 128x64 image, UVs in 64x32 at 2 px/unit) ────
def gen_cockatrice(rng):
    S = 2
    img = np.zeros((64, 128, 4), dtype=np.uint8)
    noise = rng.random((64, 128))
    body = [rgb(c) for c in ("#eef3f6", "#dfe8ee", "#c6d7e0", "#a9c1cd")]
    teal = [rgb(c) for c in ("#2f6f7c", "#3f8795", "#5fa8b4", "#86c4cc", "#bfe2e6")]
    beak = [rgb(c) for c in ("#c9892a", "#e8a93a", "#f6c75a")]
    leg = [rgb(c) for c in ("#3c4150", "#4f5566", "#666d80")]
    eye_red, pupil = rgb("#d23c3c"), rgb("#1b1b22")

    def scallop(pal):
        # Overlapping feather scallops: a lighter crown and darker lower rim per
        # 4x3 px feather, offset every other row.
        def fn(s, t, px, py):
            row = py // 3
            fx = (px + (2 if row % 2 else 0)) % 4
            fy = py % 3
            if fy == 2 or fx == 3:
                c = pal[2]
            elif fy == 0 and fx in (1, 2):
                c = pal[0]
            else:
                c = pal[1]
            if noise[py, px] > 0.93:
                c = pal[3]
            return c
        return fn

    def striped(pal, tip_light=True):
        # Long feathers: lengthwise lines, pale tips at the far end (t -> 1).
        def fn(s, t, px, py):
            c = pal[2] if px % 2 == 0 else pal[1]
            if tip_light and t > 0.8:
                c = pal[4] if px % 2 == 0 else pal[3]
            if noise[py, px] > 0.9:
                c = pal[0]
            return c
        return fn

    # body (0,0) 6x8x5 -- feathers everywhere
    for k, r in cube_rects(0, 0, 6, 8, 5).items():
        fill_rect(img, r, S, scallop(body))
    # neck (22,0) 2x6x2
    for k, r in cube_rects(22, 0, 2, 6, 2).items():
        fill_rect(img, r, S, scallop(body))
    # head (0,13) 4x4x8: pale with a teal crest on top, red eyes on the sides
    # near the front, the front face beak-coloured in its lower half.
    head = cube_rects(0, 13, 4, 4, 8)
    for k, r in head.items():
        fill_rect(img, r, S, scallop(body))
    fill_rect(img, head["top"], S, lambda s, t, px, py: teal[2] if (px + py) % 3 else teal[1])
    fill_rect(img, head["front"], S, lambda s, t, px, py: beak[1] if t > 0.5 else body[1])

    def side_eye(front_at_right):
        def fn(s, t, px, py):
            fs = s if front_at_right else 1 - s   # 1 = front
            if t < 0.25:
                return teal[2]                     # crest line along the top
            if 0.72 < fs < 0.9 and 0.3 < t < 0.65:
                return pupil if (0.78 < fs < 0.86 and t > 0.45) else eye_red
            if fs > 0.9 and t > 0.55:
                return beak[1]
            return scallop(body)(s, t, px, py)
        return fn

    fill_rect(img, head["west"], S, side_eye(front_at_right=True))
    fill_rect(img, head["east"], S, side_eye(front_at_right=False))
    # jaw (24,13) 4x1x8: the beak
    for k, r in cube_rects(24, 13, 4, 1, 8).items():
        fill_rect(img, r, S, lambda s, t, px, py: beak[2] if (py % 2 == 0) else beak[0 if s > 0.85 else 1])
    # wings (40,0) and (30,0) 1x8x4: teal flight feathers, pale tips
    for u in (40, 30):
        for k, r in cube_rects(u, 0, 1, 8, 4).items():
            fill_rect(img, r, S, striped(teal))
    # legs (54,21) and (46,21) 2x9x2: scaled, dark claws at the bottom
    for u in (54, 46):
        for k, r in cube_rects(u, 21, 2, 9, 2).items():
            fill_rect(img, r, S, lambda s, t, px, py: leg[0] if t > 0.85 else leg[1 + (py // 2) % 2])
    # tail feathers (0,26) (14,26) (28,26) 2x1x5
    for u in (0, 14, 28):
        for k, r in cube_rects(u, 26, 2, 1, 5).items():
            fill_rect(img, r, S, striped(teal))
    return img


# ── zephyr (ZephyrModel, 128x32) ────────────────────────────────────────────
def gen_zephyr(rng):
    img = np.zeros((32, 128, 4), dtype=np.uint8)
    field = fbm(rng, [(8, 8, 0.5), (4, 4, 0.3), (2, 2, 0.15)], white=0.1, w=128, h=32)
    shades = posterize(field, [14, 30, 34, 22])
    pal = [rgb(c) for c in ("#c3ccd8", "#d9e0e8", "#eaeef3", "#f8fafc")]
    dark, darker, rim = rgb("#4a5468"), rgb("#2c3344"), rgb("#9aa6b8")

    def cloud(s, t, px, py):
        return pal[shades[py, px]]

    parts = [
        (0, 0, 8, 6, 2),       # cloud_butt
        (27, 9, 12, 9, 14),    # body
        (0, 20, 2, 6, 6),      # body_*_side_front
        (25, 11, 2, 6, 6),     # body_*_side_back
        (96, 22, 5, 5, 5),     # tail_base
        (80, 24, 4, 4, 4),     # tail_middle
        (84, 18, 3, 3, 3),     # tail_end
        (67, 11, 4, 6, 2),     # right/left_face
        (66, 19, 6, 3, 1),     # mouth
    ]
    for u, v, dx, dy, dz in parts:
        for r in cube_rects(u, v, dx, dy, dz).values():
            fill_rect(img, r, 1, cloud)
    # Cheeks: a dark eye hollow in the upper half of the front face.
    face = cube_rects(67, 11, 4, 6, 2)

    def eye(s, t, px, py):
        if 0.2 < s < 0.85 and 0.15 < t < 0.55:
            return darker if (0.4 < s < 0.7 and 0.25 < t < 0.45) else dark
        return cloud(s, t, px, py)

    fill_rect(img, face["front"], 1, eye)
    # Mouth: a dark blowing hole with a pale rim.
    mouth = cube_rects(66, 19, 6, 3, 1)
    fill_rect(img, mouth["front"], 1,
              lambda s, t, px, py: darker if (0.15 < s < 0.85 and 0.3 < t < 0.8) else rim)
    return img


# ── pass two: storage blocks, aerogel, quicksoil glass, torch, doors ───────
ZANITE = ["#0e1340", "#1c2770", "#2a3a9a", "#3f55c2", "#6f86e6", "#b8c6ff"]
GRAVITITE = ["#5c1a34", "#8a2b4c", "#b8406a", "#e0628e", "#f59ab8", "#ffd2e2"]
AMBROSIUM = ["#7a3f12", "#b8641e", "#e8913a", "#ffb347", "#ffd27f", "#fff2cc"]


def gen_zanite_block(rng):
    # diamond_block's bevelled facets on the zanite gemstone's ramp.
    del rng
    src = load_vanilla("block/diamond_block.png")
    return ramp_by_rank(src, src[..., 3] > 0, ZANITE)


def gen_ambrosium_block(rng):
    del rng
    src = load_vanilla("block/gold_block.png")
    return ramp_by_rank(src, src[..., 3] > 0, AMBROSIUM)


def gen_enchanted_gravitite(rng):
    # A faceted rose-magenta crystal slab: diamond_block's facets on the
    # gravitite ramp, with a scatter of white glints (it is enchanted).
    src = load_vanilla("block/diamond_block.png")
    out = ramp_by_rank(src, src[..., 3] > 0, GRAVITITE)
    for _ in range(7):
        x, y = int(rng.integers(1, 15)), int(rng.integers(1, 15))
        out[y, x] = rgb("#fff4f8")
    return out


def gen_aerogel(rng):
    # A frosted, pale-blue translucent solid: soft cloudy interior at low
    # alpha inside a slightly denser 1-px rim.
    field = fbm(rng, [(8, 8, 0.5), (4, 4, 0.3)], white=0.15)
    out = paint(posterize(field, [30, 40, 30]), [rgb(c) for c in ("#b9d7f2", "#c9e2f8", "#dcedfc")])
    out[..., 3] = 110
    rim = np.zeros((H, W), dtype=bool)
    rim[0, :] = rim[-1, :] = rim[:, 0] = rim[:, -1] = True
    out[rim] = rgb("#e8f4ff")
    out[rim, 3] = 170
    return out


def gen_quicksoil_glass(rng):
    # glass.png's frame and streaks (its opaque pixels) in pale gold; the
    # pane between them a faint amber wash instead of fully clear.
    del rng
    src = load_vanilla("block/glass.png")
    frame = src[..., 3] > 0
    out = ramp_by_rank(src, frame, ["#c9a24a", "#dcb95e", "#ecd07a", "#f8e6a4", "#fff6d6"])
    out[~frame] = rgb("#f2d98a")
    out[~frame, 3] = 56
    return out


def gen_ambrosium_torch(rng):
    # torch.png's layout: the stick in skyroot grey, the flame an ambrosium glow.
    del rng
    src = load_vanilla("block/torch.png")

    def fn(x, y, h, s, v, a):
        if 15 < h < 45 and s > 0.35 and v < 0.75:        # the stick's browns
            return 210.0, 0.14, min(1.0, v * 1.25), a
        if s < 0.2 and v > 0.8:                           # the white-hot core
            return 45.0, 0.25, 1.0, a
        return 34.0, min(1.0, s * 0.95 + 0.1), min(1.0, v * 1.05), a   # flame

    return recolor(src, fn)


def gen_skyroot_door_top(rng):
    del rng
    src = load_vanilla("block/oak_door_top.png")
    return ramp_by_rank(src, src[..., 3] > 0, PLANK)


def gen_skyroot_door_bottom(rng):
    del rng
    src = load_vanilla("block/oak_door_bottom.png")
    return ramp_by_rank(src, src[..., 3] > 0, PLANK)


def gen_skyroot_trapdoor(rng):
    del rng
    src = load_vanilla("block/oak_trapdoor.png")
    return ramp_by_rank(src, src[..., 3] > 0, PLANK)


# ── pass two: the dungeon stones ────────────────────────────────────────────
# Each family is a carved tile: a seeded two-tone stone body, a 1-px bevel
# (light top-left, dark bottom-right) and an inset groove one pixel in, so the
# blocks read as dressed masonry when tiled. The "light"/sentry variants add a
# glowing inlay in the middle.
CARVED = ["#626a74", "#6e7681", "#7b838e", "#89919b"]
ANGELIC = ["#c9bf9f", "#d5cbac", "#e0d8bb", "#ebe4ca"]
HELLFIRE = ["#4a1c1c", "#5a2322", "#6b2a28", "#7b3230"]


def carved_tile(rng, palette: list[str], hi: str, lo: str, groove: str) -> np.ndarray:
    field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.25)], white=0.25)
    out = paint(posterize(field, [20, 35, 30, 15]), [rgb(c) for c in palette])
    out[0, :] = rgb(hi)
    out[:, 0] = rgb(hi)
    out[15, :] = rgb(lo)
    out[:, 15] = rgb(lo)
    for i in range(2, 14):                      # inset groove, 2 px in
        out[2, i] = out[13, i] = out[i, 2] = out[i, 13] = rgb(groove)
    for i in range(3, 13):                      # its lit inner edge
        out[3, i] = out[i, 3] = rgb(hi)
    return out


def inlay(out: np.ndarray, core: str, mid: str, rim: str) -> np.ndarray:
    # A 6x6 diamond centred on the tile.
    for y in range(16):
        for x in range(16):
            d = abs(x - 7.5) + abs(y - 7.5)
            if d <= 2.0:
                out[y, x] = rgb(core)
            elif d <= 3.0:
                out[y, x] = rgb(mid)
            elif d <= 4.0:
                out[y, x] = rgb(rim)
    return out


def gen_carved_stone(rng):
    return carved_tile(rng, CARVED, "#9aa2ad", "#4c535c", "#565e68")


def gen_sentry_stone(rng):
    del rng
    out = gen_carved_stone(rng_for("carved_stone"))
    return inlay(out, "#d8f4ff", "#6fc6ff", "#2f6f9f")


def gen_angelic_stone(rng):
    return carved_tile(rng, ANGELIC, "#f6f1de", "#a79c7c", "#b3a887")


def gen_light_angelic_stone(rng):
    del rng
    out = gen_angelic_stone(rng_for("angelic_stone"))
    return inlay(out, "#fffbe6", "#ffe38a", "#d9a93e")


def gen_hellfire_stone(rng):
    return carved_tile(rng, HELLFIRE, "#8c3c38", "#2e1010", "#361413")


def gen_light_hellfire_stone(rng):
    del rng
    out = gen_hellfire_stone(rng_for("hellfire_stone"))
    return inlay(out, "#fff0b0", "#ff9a3a", "#c2401e")


PILLAR = ["#d9d4c6", "#e3dfd2", "#ece9df", "#f5f3ec"]


def gen_pillar_side(rng):
    # A fluted column: four vertical flutes (a shadowed channel with a lit
    # ridge beside it) over pale stone.
    field = fbm(rng, [(2, 8, 0.4)], white=0.3)
    out = paint(posterize(field, [20, 35, 30, 15]), [rgb(c) for c in PILLAR])
    for x0 in (1, 5, 9, 13):
        out[:, x0] = rgb("#bdb7a6")
        out[:, x0 + 1] = rgb("#cbc6b6")
        out[:, x0 + 2] = rgb("#faf8f2")
    return out


def gen_pillar_end(rng):
    # The column's cut end: concentric rings.
    field = fbm(rng, [(4, 4, 0.4)], white=0.3)
    out = paint(posterize(field, [20, 35, 30, 15]), [rgb(c) for c in PILLAR])
    for y in range(16):
        for x in range(16):
            r = ((x - 7.5) ** 2 + (y - 7.5) ** 2) ** 0.5
            if 6.3 < r < 7.3 or 3.3 < r < 4.1:
                out[y, x] = rgb("#bdb7a6")
            elif 5.5 < r <= 6.3:
                out[y, x] = rgb("#faf8f2")
    out[0, :] = out[15, :] = out[:, 0] = out[:, 15] = rgb("#c9c3b3")
    return out


def gen_pillar_top_side(rng):
    # The capital: the fluted shaft below, a moulded band across the top.
    del rng
    out = gen_pillar_side(rng_for("pillar_side"))
    band = [("#faf8f2", 0), ("#ece9df", 1), ("#ece9df", 2), ("#bdb7a6", 3),
            ("#f5f3ec", 4), ("#d9d4c6", 5), ("#bdb7a6", 6)]
    for col, y in band:
        out[y, :] = rgb(col)
    return out


# ── pass two: items ─────────────────────────────────────────────────────────
def gen_skyroot_door(rng):
    del rng
    src = load_vanilla("item/oak_door.png")
    return ramp_by_rank(src, src[..., 3] > 0, PLANK)


def gen_skyroot_stick(rng):
    del rng
    src = load_vanilla("item/stick.png")
    return ramp_by_rank(src, src[..., 3] > 0, BARK[1:])


def gen_golden_amber(rng):
    # emerald.png's cut, as warm translucent-looking amber.
    del rng
    src = load_vanilla("item/emerald.png")
    return ramp_by_rank(src, src[..., 3] > 0, ["#7a4a0c", "#b0700f", "#e0a020", "#f5c542", "#ffe28a", "#fff6d0"])


def gen_swet_ball(rng):
    del rng
    src = load_vanilla("item/slime_ball.png")
    return ramp_by_rank(src, src[..., 3] > 0, ["#1f4f8f", "#2f6fbf", "#4f93e0", "#7fb8f5", "#bfe0ff"])


AECHOR_PETAL_ART = [
    "................",
    "................",
    "..........pp....",
    "........ppPPp...",
    ".......pPPWPp...",
    "......pPPWWPp...",
    ".....pPPWWPPp...",
    "....pPPWWPPp....",
    "....pPWWPPPp....",
    "...pPWWPPPp.....",
    "...pPWPPPp......",
    "...pPPPpp.......",
    "...ppPp.........",
    "....ss..........",
    "...s............",
    "................",
]
AECHOR_PETAL_KEY = {"p": "#c9669a", "P": "#ec9cc4", "W": "#fcdcec", "s": "#6fae8f"}


def gen_aechor_petal(rng):
    del rng
    return plot_art(AECHOR_PETAL_ART, AECHOR_PETAL_KEY)


def gen_enchanted_berry(rng):
    # sweet_berries' cluster, berries a glowing rose-violet, leaves pale.
    src = load_vanilla("item/sweet_berries.png")

    def fn(x, y, h, s, v, a):
        if (h < 30 or h > 320) and s > 0.3:
            return 318.0, min(1.0, s * 0.9), min(1.0, v * 1.15 + 0.05), a
        return 140.0, s * 0.7, min(1.0, v * 1.1), a

    out = recolor(src, fn)
    opaque = [(y, x) for y in range(16) for x in range(16) if out[y, x, 3] > 0]
    for i in rng.permutation(len(opaque))[:3]:
        y, x = opaque[i]
        out[y, x] = rgb("#fff0fa")
    return out


def gen_white_apple(rng):
    # apple.png's fruit in cream-white with a faint blue shade; the stem and
    # leaf keep their hue.
    del rng
    src = load_vanilla("item/apple.png")

    def fn(x, y, h, s, v, a):
        if (h < 20 or h > 330) and s > 0.3:                   # red skin
            return 210.0, s * 0.12, min(1.0, 0.55 + v * 0.5), a
        return h, s, v, a

    return recolor(src, fn)


def gummy(palette: list[str]) -> np.ndarray:
    src = load_vanilla("item/slime_ball.png")
    out = ramp_by_rank(src, src[..., 3] > 0, palette)
    # A glossy highlight, top-left of the lump.
    ys, xs = np.nonzero(src[..., 3] > 0)
    y0, x0 = ys.min() + 2, xs.min() + 2
    out[y0, x0] = rgb("#ffffff")
    out[y0, x0 + 1] = rgb(palette[-1])
    return out


def gen_blue_gummy_swet(rng):
    del rng
    return gummy(["#1c4a86", "#2c68b4", "#4a8fdc", "#79b6f4", "#b4dcff"])


def gen_golden_gummy_swet(rng):
    del rng
    return gummy(["#8a5a10", "#b88418", "#e0ae2a", "#f6d060", "#fff0b0"])


def skyroot_bucket_from(src_name: str) -> np.ndarray:
    # The vanilla bucket's grey metal becomes skyroot wood; whatever the
    # bucket holds (water's blues, milk's whites) keeps its own colours.
    src = load_vanilla(f"item/{src_name}.png")
    empty = load_vanilla("item/bucket.png")
    metal = (src[..., 3] > 0) & (np.abs(src.astype(int) - empty.astype(int)).sum(axis=2) == 0)
    out = src.copy()
    wood = ramp_by_rank(src, metal, PLANK[:5])
    out[metal] = wood[metal]
    # Stave joints: every third column of the wooden body one shade darker,
    # which is what tells it from the iron bucket at a glance.
    dark = rgb(BARK[2])
    ys, xs = np.nonzero(metal)
    x0 = xs.min()
    for y, x in zip(ys, xs):
        if (x - x0) % 3 == 2 and y > ys.min() + 1:
            out[y, x] = dark
    return out


def gen_skyroot_bucket(rng):
    del rng
    return skyroot_bucket_from("bucket")


def gen_skyroot_water_bucket(rng):
    del rng
    return skyroot_bucket_from("water_bucket")


def gen_skyroot_milk_bucket(rng):
    del rng
    return skyroot_bucket_from("milk_bucket")


def dungeon_key(palette: list[str]) -> np.ndarray:
    src = load_vanilla("item/trial_key.png")
    return ramp_by_rank(src, src[..., 3] > 0, palette)


def gen_bronze_dungeon_key(rng):
    del rng
    return dungeon_key(["#4a2a12", "#7a4a22", "#a8693a", "#cd8a52", "#e8b484"])


def gen_silver_dungeon_key(rng):
    del rng
    return dungeon_key(["#4a4f58", "#737a86", "#a0a8b4", "#c8ced8", "#eef1f6"])


def gen_gold_dungeon_key(rng):
    del rng
    return dungeon_key(["#6a4a0a", "#a07410", "#d4a01e", "#f2c84a", "#fff0a0"])


# Tools and armour: the vanilla sprite's HEAD (the pixels that differ between
# two materials of the same tool) takes the material ramp; the HANDLE (the
# pixels every material shares) becomes a skyroot stick.
def tool_parts(kind: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    a = load_vanilla(f"item/iron_{kind}.png")
    b = load_vanilla(f"item/diamond_{kind}.png")
    opaque = a[..., 3] > 0
    same = opaque & (np.abs(a.astype(int) - b.astype(int)).sum(axis=2) == 0)
    return a, opaque & ~same, same


def material_tool(kind: str, head: list[str], src_material: str = "iron") -> np.ndarray:
    base, head_mask, handle = tool_parts(kind)
    src = load_vanilla(f"item/{src_material}_{kind}.png")
    out = src.copy()
    h = ramp_by_rank(src, head_mask, head)
    out[head_mask] = h[head_mask]
    stick = ramp_by_rank(src, handle, BARK[1:])
    out[handle] = stick[handle]
    return out


HOLY_TOOL = ["#7d8591", "#98a0ab", "#b3bac4", "#cdd2d9", "#e7eaee"]
SKY_TOOL = ["#6d7a8a", "#8795a4", "#a3afbc", "#bec8d2", "#d6dde5"]
ZANITE_TOOL = ["#141c52", "#2a3a9a", "#4658c4", "#7082e0", "#aab6f5"]
GRAV_TOOL = ["#5c1a34", "#8a2b4c", "#c24a74", "#ec7ca2", "#ffc4d8"]


def _tool_gen(kind: str, pal: list[str]):
    return lambda rng: material_tool(kind, pal)


def armor_item(piece: str, pal: list[str]) -> np.ndarray:
    src = load_vanilla(f"item/iron_{piece}.png")
    return ramp_by_rank(src, src[..., 3] > 0, pal)


ZANITE_ARMOR = ["#0e1340", "#1c2770", "#2f40a6", "#4a60cc", "#7c90ea", "#b8c6ff"]
GRAV_ARMOR = ["#4a1228", "#7a2442", "#a8385e", "#d85a86", "#f294b6", "#ffd0e0"]


def _armor_gen(piece: str, pal: list[str]):
    return lambda rng: armor_item(piece, pal)


def armor_layer(sub: str, pal: list[str]) -> np.ndarray:
    src = load_vanilla(f"entity/equipment/{sub}/iron.png")
    return ramp_by_rank(src, src[..., 3] > 0, pal)


# ── registry ────────────────────────────────────────────────────────────────
BLOCK, ITEM, ENTITY = "block", "item", "entity"
TEXTURES = {
    "holystone": (gen_holystone, None, BLOCK),
    "mossy_holystone": (gen_mossy_holystone, None, BLOCK),
    "holystone_bricks": (gen_holystone_bricks, None, BLOCK),
    "ambrosium_ore": (gen_ambrosium_ore, None, BLOCK),
    "zanite_ore": (gen_zanite_ore, None, BLOCK),
    "gravitite_ore": (gen_gravitite_ore, None, BLOCK),
    "aether_dirt": (gen_aether_dirt, None, BLOCK),
    "aether_grass_block_top": (gen_aether_grass_block_top, None, BLOCK),
    "aether_grass_block_side": (gen_aether_grass_block_side, None, BLOCK),
    "quicksoil": (gen_quicksoil, None, BLOCK),
    "cold_aercloud": (gen_cold_aercloud, None, BLOCK),
    "blue_aercloud": (gen_blue_aercloud, None, BLOCK),
    "golden_aercloud": (gen_golden_aercloud, None, BLOCK),
    "icestone": (gen_icestone, None, BLOCK),
    "skyroot_log": (gen_skyroot_log, None, BLOCK),
    "skyroot_log_top": (gen_skyroot_log_top, None, BLOCK),
    "stripped_skyroot_log": (gen_stripped_skyroot_log, None, BLOCK),
    "stripped_skyroot_log_top": (gen_stripped_skyroot_log_top, None, BLOCK),
    "skyroot_planks": (gen_skyroot_planks, None, BLOCK),
    "skyroot_leaves": (gen_skyroot_leaves, "block/oak_leaves", BLOCK),
    "skyroot_sapling": (gen_skyroot_sapling, None, BLOCK),
    "golden_oak_log": (gen_golden_oak_log, None, BLOCK),
    "golden_oak_log_top": (gen_golden_oak_log_top, None, BLOCK),
    "golden_oak_leaves": (gen_golden_oak_leaves, "block/oak_leaves", BLOCK),
    "golden_oak_sapling": (gen_golden_oak_sapling, None, BLOCK),
    "aether_portal": (gen_aether_portal, "block/nether_portal", BLOCK),
    "berry_bush": (gen_berry_bush, None, BLOCK),
    "berry_bush_stem": (gen_berry_bush_stem, None, BLOCK),
    "white_flower": (gen_white_flower, None, BLOCK),
    "purple_flower": (gen_purple_flower, None, BLOCK),
    "ambrosium_shard": (gen_ambrosium_shard, None, ITEM),
    "zanite_gemstone": (gen_zanite_gemstone, None, ITEM),
    "blue_berry": (gen_blue_berry, None, ITEM),
    "phyg": (gen_phyg, None, ENTITY),
    "phyg_wings": (gen_phyg_wings, None, ENTITY),
    "flying_cow": (gen_flying_cow, None, ENTITY),
    "flying_cow_wings": (gen_flying_cow_wings, None, ENTITY),
    "sheepuff": (gen_sheepuff, None, ENTITY),
    "sheepuff_wool": (gen_sheepuff_wool, None, ENTITY),
    "cockatrice": (gen_cockatrice, None, ENTITY),
    "zephyr": (gen_zephyr, None, ENTITY),
    # ── pass two ──
    "zanite_block": (gen_zanite_block, None, BLOCK),
    "ambrosium_block": (gen_ambrosium_block, None, BLOCK),
    "enchanted_gravitite": (gen_enchanted_gravitite, None, BLOCK),
    "aerogel": (gen_aerogel, None, BLOCK),
    "quicksoil_glass": (gen_quicksoil_glass, None, BLOCK),
    "ambrosium_torch": (gen_ambrosium_torch, None, BLOCK),
    "skyroot_door_top": (gen_skyroot_door_top, None, BLOCK),
    "skyroot_door_bottom": (gen_skyroot_door_bottom, None, BLOCK),
    "skyroot_trapdoor": (gen_skyroot_trapdoor, None, BLOCK),
    "carved_stone": (gen_carved_stone, None, BLOCK),
    "sentry_stone": (gen_sentry_stone, None, BLOCK),
    "angelic_stone": (gen_angelic_stone, None, BLOCK),
    "light_angelic_stone": (gen_light_angelic_stone, None, BLOCK),
    "hellfire_stone": (gen_hellfire_stone, None, BLOCK),
    "light_hellfire_stone": (gen_light_hellfire_stone, None, BLOCK),
    "pillar_side": (gen_pillar_side, None, BLOCK),
    "pillar_end": (gen_pillar_end, None, BLOCK),
    "pillar_top_side": (gen_pillar_top_side, None, BLOCK),
    "skyroot_door": (gen_skyroot_door, None, ITEM),
    "skyroot_stick": (gen_skyroot_stick, None, ITEM),
    "golden_amber": (gen_golden_amber, None, ITEM),
    "swet_ball": (gen_swet_ball, None, ITEM),
    "aechor_petal": (gen_aechor_petal, None, ITEM),
    "enchanted_berry": (gen_enchanted_berry, None, ITEM),
    "white_apple": (gen_white_apple, None, ITEM),
    "blue_gummy_swet": (gen_blue_gummy_swet, None, ITEM),
    "golden_gummy_swet": (gen_golden_gummy_swet, None, ITEM),
    "skyroot_bucket": (gen_skyroot_bucket, None, ITEM),
    "skyroot_water_bucket": (gen_skyroot_water_bucket, None, ITEM),
    "skyroot_milk_bucket": (gen_skyroot_milk_bucket, None, ITEM),
    "bronze_dungeon_key": (gen_bronze_dungeon_key, None, ITEM),
    "silver_dungeon_key": (gen_silver_dungeon_key, None, ITEM),
    "gold_dungeon_key": (gen_gold_dungeon_key, None, ITEM),
    **{f"{mat}_{kind}": (_tool_gen(kind, pal), None, ITEM)
       for mat, pal in (("skyroot", SKY_TOOL), ("holystone", HOLY_TOOL),
                        ("zanite", ZANITE_TOOL), ("gravitite", GRAV_TOOL))
       for kind in ("sword", "pickaxe", "axe", "shovel", "hoe")},
    **{f"{mat}_{piece}": (_armor_gen(piece, pal), None, ITEM)
       for mat, pal in (("zanite", ZANITE_ARMOR), ("gravitite", GRAV_ARMOR))
       for piece in ("helmet", "chestplate", "leggings", "boots")},
    # Worn-armour sheets (vanilla iron's layout, recoloured) for the
    # renderer's textures/entity/equipment/humanoid[_leggings]/<material>.png.
    "humanoid/zanite": (lambda rng: armor_layer("humanoid", ZANITE_ARMOR), None, "equipment"),
    "humanoid_leggings/zanite": (lambda rng: armor_layer("humanoid_leggings", ZANITE_ARMOR), None, "equipment"),
    "humanoid/gravitite": (lambda rng: armor_layer("humanoid", GRAV_ARMOR), None, "equipment"),
    "humanoid_leggings/gravitite": (lambda rng: armor_layer("humanoid_leggings", GRAV_ARMOR), None, "equipment"),
}
KIND_DIR = {BLOCK: "block", ITEM: "item", ENTITY: "entity/aether", "equipment": "entity/equipment"}
ENTITY_SIZES = {"phyg": (64, 64), "flying_cow": (64, 64), "phyg_wings": (32, 64),
                "flying_cow_wings": (32, 64), "sheepuff": (32, 64), "sheepuff_wool": (32, 64),
                "cockatrice": (64, 128), "zephyr": (32, 128)}

VANILLA_REFS = ["stone", "cobblestone", "mossy_cobblestone", "stone_bricks", "diamond_ore", "dirt",
                "grass_block_side", "sand", "packed_ice", "oak_log", "oak_log_top", "oak_planks",
                "oak_leaves", "oak_sapling", "dandelion"]


# ── blockstates + block models ─────────────────────────────────────────────
# Structure copied from the vanilla counterparts (stone / oak_log / grass_block
# / oak_sapling / nether_portal_ns+ew); every texture is one written above.
CUBE_ALL = ["aether_dirt", "quicksoil", "holystone", "mossy_holystone", "holystone_bricks",
            "cold_aercloud", "blue_aercloud", "golden_aercloud", "icestone", "ambrosium_ore",
            "zanite_ore", "gravitite_ore", "skyroot_planks", "skyroot_leaves", "golden_oak_leaves"]
CROSS = ["skyroot_sapling", "golden_oak_sapling", "berry_bush", "berry_bush_stem",
         "white_flower", "purple_flower"]
LOGS = {"skyroot_log": ("skyroot_log", "skyroot_log_top"),
        "stripped_skyroot_log": ("stripped_skyroot_log", "stripped_skyroot_log_top"),
        "golden_oak_log": ("golden_oak_log", "golden_oak_log_top")}


def mc(name: str) -> str:
    return f"minecraft:block/{name}"


def json_outputs() -> dict[Path, dict]:
    bs_dir = REPO_ROOT / "assets" / "blockstates"
    md = REPO_ROOT / "assets" / "models" / "block"
    out: dict[Path, dict] = {}
    for n in CUBE_ALL:
        out[md / f"{n}.json"] = {"parent": "minecraft:block/cube_all", "textures": {"all": mc(n)}}
        out[bs_dir / f"{n}.json"] = {"variants": {"": {"model": mc(n)}}}
    for n in CROSS:
        out[md / f"{n}.json"] = {"parent": "minecraft:block/cross", "textures": {"cross": mc(n)}}
        out[bs_dir / f"{n}.json"] = {"variants": {"": {"model": mc(n)}}}
    for n, (side, end) in LOGS.items():
        tex = {"end": mc(end), "side": mc(side)}
        out[md / f"{n}.json"] = {"parent": "minecraft:block/cube_column", "textures": tex}
        out[md / f"{n}_horizontal.json"] = {"parent": "minecraft:block/cube_column_horizontal", "textures": tex}
        out[bs_dir / f"{n}.json"] = {"variants": {
            "axis=x": {"model": mc(f"{n}_horizontal"), "x": 90, "y": 90},
            "axis=y": {"model": mc(n)},
            "axis=z": {"model": mc(f"{n}_horizontal"), "x": 90}}}
    # aether_grass_block: grass_block's shape without the tinted overlay (the
    # side texture already carries the mint rim; nothing here is tinted).
    out[md / "aether_grass_block.json"] = {
        "parent": "minecraft:block/cube_bottom_top",
        "textures": {"top": mc("aether_grass_block_top"), "side": mc("aether_grass_block_side"),
                     "bottom": mc("aether_dirt"), "particle": mc("aether_dirt")}}
    rotations = [{"model": mc("aether_grass_block")}] + [
        {"model": mc("aether_grass_block"), "y": r} for r in (90, 180, 270)]
    out[bs_dir / "aether_grass_block.json"] = {"variants": {
        "snowy=false": rotations, "snowy=true": {"model": mc("aether_grass_block")}}}
    # aether_portal: nether_portal_ns / _ew on our strip.
    for suffix, frm, to, faces in (("ns", [0, 0, 6], [16, 16, 10], ("north", "south")),
                                   ("ew", [6, 0, 0], [10, 16, 16], ("east", "west"))):
        out[md / f"aether_portal_{suffix}.json"] = {
            "textures": {"particle": mc("aether_portal"), "portal": mc("aether_portal")},
            "elements": [{"from": frm, "to": to,
                          "faces": {f: {"uv": [0, 0, 16, 16], "texture": "#portal"} for f in faces}}]}
    out[bs_dir / "aether_portal.json"] = {"variants": {
        "axis=x": {"model": mc("aether_portal_ns")}, "axis=z": {"model": mc("aether_portal_ew")}}}

    # ── pass two ──
    for n in PASS2_CUBES:
        tex = PASS2_CUBES[n]
        out[md / f"{n}.json"] = {"parent": "minecraft:block/cube_all", "textures": {"all": mc(tex)}}
        out[bs_dir / f"{n}.json"] = {"variants": {"": {"model": mc(n)}}}
    for ours, (vanilla, extra_models, texmap) in PASS2_FAMILIES.items():
        family_from_vanilla(out, ours, vanilla, extra_models, texmap)
    # The dungeon pillar (RotatedPillarBlock) and its capital (FacingPillarBlock,
    # end_rod's six rotations, facing=up first so a state without the
    # property gets the upright model).
    ptex = {"end": mc("pillar_end"), "side": mc("pillar_side")}
    out[md / "pillar.json"] = {"parent": "minecraft:block/cube_column", "textures": ptex}
    out[md / "pillar_horizontal.json"] = {"parent": "minecraft:block/cube_column_horizontal",
                                          "textures": ptex}
    out[bs_dir / "pillar.json"] = {"variants": {
        "axis=x": {"model": mc("pillar_horizontal"), "x": 90, "y": 90},
        "axis=y": {"model": mc("pillar")},
        "axis=z": {"model": mc("pillar_horizontal"), "x": 90}}}
    out[md / "pillar_top.json"] = {"parent": "minecraft:block/cube_column",
                                   "textures": {"end": mc("pillar_end"), "side": mc("pillar_top_side")}}
    rot = {"up": {}, "down": {"x": 180}, "north": {"x": 90}, "south": {"x": 90, "y": 180},
           "west": {"x": 90, "y": 270}, "east": {"x": 90, "y": 90}}
    out[bs_dir / "pillar_top.json"] = {"variants": {
        f"facing={f}": {"model": mc("pillar_top"), **r} for f, r in rot.items()}}
    # Flat inventory icons for the door and the torch (vanilla's items/*.json
    # point these at an item/generated sprite, not the block model).
    items_dir = REPO_ROOT / "assets" / "items"
    item_models = REPO_ROOT / "assets" / "models" / "item"
    for item, layer in (("skyroot_door", "minecraft:item/skyroot_door"),
                        ("ambrosium_torch", "minecraft:block/ambrosium_torch")):
        out[item_models / f"{item}.json"] = {"parent": "minecraft:item/generated",
                                             "textures": {"layer0": layer}}
        out[items_dir / f"{item}.json"] = {"model": {"type": "minecraft:model",
                                                     "model": f"minecraft:item/{item}"}}
    return out


# Pass-two cube blocks: block -> its texture (the locked / trapped / doorway
# twins share their base stone's face, as the mod's models do).
PASS2_CUBES = {
    "zanite_block": "zanite_block", "ambrosium_block": "ambrosium_block",
    "enchanted_gravitite": "enchanted_gravitite", "aerogel": "aerogel",
    "quicksoil_glass": "quicksoil_glass",
    **{f"{pre}{base}": base
       for base in ("carved_stone", "sentry_stone", "angelic_stone", "light_angelic_stone",
                    "hellfire_stone", "light_hellfire_stone")
       for pre in ("", "locked_", "trapped_", "treasure_doorway_")},
    # Boss doorways copy the UNLIT stones in AetherBlocks; their sentry /
    # light variants still show their own face.
    **{f"boss_doorway_{base}": base
       for base in ("carved_stone", "sentry_stone", "angelic_stone", "light_angelic_stone",
                    "hellfire_stone", "light_hellfire_stone")},
}

# Block families cloned from a vanilla family's blockstate + models, with the
# vanilla textures swapped: ours -> (vanilla slug, {vanilla model: our model}
# for models outside the vanilla prefix (a slab's double = the full block),
# {vanilla texture: ours}).
_STONE = {"stone": "holystone"}


def _stone_family(prefix: str, block: str, tex: str) -> dict:
    return {
        f"{prefix}_stairs": ("stone_stairs", {}, {"stone": tex}),
        f"{prefix}_slab": ("stone_slab", {"stone": block}, {"stone": tex}),
        f"{prefix}_wall": ("cobblestone_wall", {}, {"cobblestone": tex}),
    }


PASS2_FAMILIES = {
    **_stone_family("holystone", "holystone", "holystone"),
    **_stone_family("mossy_holystone", "mossy_holystone", "mossy_holystone"),
    **_stone_family("holystone_brick", "holystone_bricks", "holystone_bricks"),
    **_stone_family("icestone", "icestone", "icestone"),
    **_stone_family("carved", "carved_stone", "carved_stone"),
    **_stone_family("angelic", "angelic_stone", "angelic_stone"),
    **_stone_family("hellfire", "hellfire_stone", "hellfire_stone"),
    "holystone_button": ("stone_button", {}, {"stone": "holystone"}),
    "holystone_pressure_plate": ("stone_pressure_plate", {}, {"stone": "holystone"}),
    **{f"skyroot_{k}": (f"oak_{k}", {"oak_planks": "skyroot_planks"},
                        {"oak_planks": "skyroot_planks", "oak_door_top": "skyroot_door_top",
                         "oak_door_bottom": "skyroot_door_bottom", "oak_trapdoor": "skyroot_trapdoor"})
       for k in ("stairs", "slab", "fence", "fence_gate", "door", "trapdoor", "button",
                 "pressure_plate")},
    "skyroot_wood": ("oak_wood", {}, {"oak_log": "skyroot_log"}),
    "golden_oak_wood": ("oak_wood", {}, {"oak_log": "golden_oak_log"}),
    "stripped_skyroot_wood": ("stripped_oak_wood", {}, {"stripped_oak_log": "stripped_skyroot_log"}),
    "ambrosium_torch": ("torch", {}, {"torch": "ambrosium_torch"}),
    "ambrosium_wall_torch": ("wall_torch", {}, {"torch": "ambrosium_torch"}),
}


def family_from_vanilla(out: dict, ours: str, vanilla: str, extra_models: dict, texmap: dict) -> None:
    """Clone vanilla `vanilla`'s blockstate and every model it (or its
    `_inventory` model) uses, renaming `<vanilla>*` models to `<ours>*` and
    swapping textures through `texmap`. An unmapped texture is an error."""
    root = REPO_ROOT / "assets"
    bs = json.loads((root / "blockstates" / f"{vanilla}.json").read_text())

    def rename(ref: str) -> str:
        name = ref.split(":", 1)[-1].split("/", 1)[1]
        if name.startswith(vanilla):
            return mc(ours + name[len(vanilla):])
        if name in extra_models:
            return mc(extra_models[name])
        raise SystemExit(f"{ours}: model {ref} outside {vanilla}*")

    wanted: set[str] = set()

    def fix(v):
        if isinstance(v, list):
            return [fix(x) for x in v]
        v = dict(v)
        name = v["model"].split(":", 1)[-1].split("/", 1)[1]
        if name.startswith(vanilla):
            wanted.add(name)
        v["model"] = rename(v["model"])
        return v

    if "variants" in bs:
        new_bs = {"variants": {k: fix(v) for k, v in bs["variants"].items()}}
    else:
        new_bs = {"multipart": [dict(part, apply=fix(part["apply"])) for part in bs["multipart"]]}
    out[root / "blockstates" / f"{ours}.json"] = new_bs
    inv = f"{vanilla}_inventory"
    if (root / "models" / "block" / f"{inv}.json").exists():
        wanted.add(inv)
    for name in sorted(wanted):
        model = json.loads((root / "models" / "block" / f"{name}.json").read_text())
        tex = {}
        for k, t in model.get("textures", {}).items():
            if t.startswith("#"):
                tex[k] = t
                continue
            tname = t.split(":", 1)[-1].split("/", 1)[1]
            if tname not in texmap:
                raise SystemExit(f"{ours}: texture {t} of {name} has no mapping")
            tex[k] = mc(texmap[tname])
        model = dict(model, textures=tex)
        out[root / "models" / "block" / f"{ours + name[len(vanilla):]}.json"] = model


def verify_json(outputs: dict[Path, dict]) -> list[str]:
    errs = []
    root = REPO_ROOT / "assets"

    def model_ok(ref: str) -> bool:
        p = root / "models" / f"{ref.split(':', 1)[1]}.json"
        return p in outputs or p.exists()

    for path, obj in outputs.items():
        if "variants" in obj:
            for v in obj["variants"].values():
                for e in (v if isinstance(v, list) else [v]):
                    if not model_ok(e["model"]):
                        errs.append(f"{path.name}: {e['model']}")
            continue
        if "parent" in obj and not model_ok(obj["parent"]):
            errs.append(f"{path.name}: parent {obj['parent']}")
        for t in obj.get("textures", {}).values():
            if not t.startswith("#"):
                name = t.split(":", 1)[1].split("/", 1)[1]
                if name not in TEXTURES and not (TEX_ROOT / "block" / f"{name}.png").exists():
                    errs.append(f"{path.name}: texture {t}")
    return errs


def png_bytes(arr: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def render(name: str) -> tuple[bytes, bytes | None, str]:
    gen, mcmeta_src, kind = TEXTURES[name]
    arr = gen(rng_for(name))
    assert arr.dtype == np.uint8 and arr.ndim == 3 and arr.shape[2] == 4, name
    if kind == BLOCK:
        assert arr.shape[1] == 16 and arr.shape[0] % 16 == 0, (name, arr.shape)
    elif kind == ITEM:
        assert arr.shape[:2] == (16, 16), (name, arr.shape)
    elif kind == "equipment":
        assert arr.shape[:2] == (32, 64), (name, arr.shape)
    else:
        assert arr.shape[:2] == ENTITY_SIZES[name], (name, arr.shape)
    meta = (TEX_ROOT / f"{mcmeta_src}.png.mcmeta").read_bytes() if mcmeta_src else None
    return png_bytes(arr), meta, kind


def preview(names: list[str], outpath: Path) -> None:
    S = 6
    bg = (58, 64, 78, 255)
    tiles: list[tuple[str, Image.Image]] = []
    for n in names:
        data, _, kind = render(n)
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        if kind in (BLOCK, ITEM):
            if im.size[1] > 16:
                strip = Image.new("RGBA", (16 * 4 + 6, 16), (0, 0, 0, 0))
                for f in range(4):
                    strip.paste(im.crop((0, f * 128, 16, f * 128 + 16)), (f * 17, 0))
                im = strip
                tiles.append((n + " (frames 0/8/16/24)", im.resize((im.size[0] * 4, 64), Image.NEAREST)))
                continue
            big = Image.new("RGBA", (16 * S + 4 + 32 * 3, 16 * S), bg)
            big.alpha_composite(im.resize((16 * S, 16 * S), Image.NEAREST), (0, 0))
            tiled = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
            for tx in (0, 16):
                for ty in (0, 16):
                    tiled.paste(im, (tx, ty))
            big.alpha_composite(tiled.resize((96, 96), Image.NEAREST), (16 * S + 4, 0))
            tiles.append((n, big))
        else:
            s = 4 if im.size[0] <= 64 else 3
            tiles.append((n, im.resize((im.size[0] * s, im.size[1] * s), Image.NEAREST)))
    for r in VANILLA_REFS:
        p = TEX_ROOT / "block" / f"{r}.png"
        im = Image.open(p).convert("RGBA").crop((0, 0, 16, 16))
        tiles.append(("vanilla " + r, im.resize((16 * S, 16 * S), Image.NEAREST)))
    width = 1400
    x = y = 10
    row_h = 0
    placed = []
    for n, im in tiles:
        if x + im.size[0] > width - 10:
            x = 10
            y += row_h + 22
            row_h = 0
        placed.append((x, y + 14, im, n))
        x += im.size[0] + 14
        row_h = max(row_h, im.size[1] + 14)
    sheet = Image.new("RGBA", (width, y + row_h + 30), bg)
    d = ImageDraw.Draw(sheet)
    for x, yy, im, n in placed:
        sheet.alpha_composite(im, (x, yy))
        d.text((x, yy - 13), n, fill=(235, 235, 235, 255))
    sheet.save(outpath)
    print(f"preview -> {outpath}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated texture names")
    ap.add_argument("--outdir", type=Path, default=TEX_ROOT, help="textures root (block/ item/ entity/aether/)")
    ap.add_argument("--preview", nargs="?", const="aether_textures_preview.png", metavar="PNG",
                    help="write a contact sheet instead of the textures")
    ap.add_argument("--check", action="store_true", help="byte-compare against --outdir; exit 1 on drift")
    args = ap.parse_args()

    names = list(TEXTURES)
    if args.only:
        names = [n.strip() for n in args.only.split(",") if n.strip()]
        unknown = [n for n in names if n not in TEXTURES]
        if unknown:
            print(f"error: unknown texture(s): {', '.join(unknown)}", file=sys.stderr)
            return 2
    if args.preview:
        preview(names, Path(args.preview))
        return 0

    def rel(p: Path) -> str:
        return str(p.relative_to(REPO_ROOT)) if p.is_relative_to(REPO_ROOT) else str(p)

    drift = 0
    if not args.only and args.outdir == TEX_ROOT:
        outputs = json_outputs()
        errs = verify_json(outputs)
        if errs:
            for e in errs:
                print(f"UNRESOLVED {e}")
            return 1
        for path, obj in sorted(outputs.items()):
            blob = (json.dumps(obj, indent=2) + "\n").encode()
            if args.check:
                if not path.exists() or path.read_bytes() != blob:
                    print(f"DRIFT    {rel(path)}")
                    drift += 1
                else:
                    print(f"ok       {path.name}")
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(blob)
                print(f"wrote    {rel(path)}")
    for name in names:
        data, meta, kind = render(name)
        outdir: Path = args.outdir / KIND_DIR[kind]
        targets = [(outdir / f"{name}.png", data)]
        if meta is not None:
            targets.append((outdir / f"{name}.png.mcmeta", meta))
        for path, blob in targets:
            if args.check:
                if not path.exists():
                    print(f"MISSING  {rel(path)}")
                    drift += 1
                elif path.read_bytes() != blob:
                    print(f"DRIFT    {rel(path)}")
                    drift += 1
                else:
                    print(f"ok       {path.name}")
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(blob)
                print(f"wrote    {rel(path)} ({len(blob)} bytes)")
    if args.check:
        print("check: " + ("clean" if drift == 0 else f"{drift} file(s) differ"))
        return 1 if drift else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
