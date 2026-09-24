#!/usr/bin/env python3
# tools/gen_hush_textures.py
#
# Deterministic procedural textures for The Hush dimension. Block art is built
# from seeded value noise that is POSTERIZED into a handful of flat shades
# (4-7, the way vanilla 16x16 block art is painted), never smooth gradients;
# derived art (portal, cluster, lantern, items, entities) is a per-pixel
# recolour of the vanilla sprite that keeps its alpha and layout. Re-running
# always produces identical bytes; `--check` proves it against what is
# committed.
#
#   python3 tools/gen_hush_textures.py              # write every PNG (+ .mcmeta)
#   python3 tools/gen_hush_textures.py --only hushstone,echo_ore
#   python3 tools/gen_hush_textures.py --preview     # contact sheet next to vanilla refs
#   python3 tools/gen_hush_textures.py --check       # byte-compare, exit 1 on drift
#   python3 tools/gen_hush_textures.py --outdir /tmp/x   # (mirrors block/ item/ entity/)
#
# Block textures (assets/textures/block/):
#   hushstone, polished_hushstone, hushstone_bricks, cracked_hushstone_bricks
#   (hushstone_bricks + cracked_deepslate_bricks' crack delta), chiseled_
#   hushstone_bricks (chiseled_deepslate's level map), sculk_loam, echo_ore,
#   resonite_ore, resonite_block (iron_block's shading, pale violet, cyan rim),
#   resonant_crystal (fully opaque - the block is an Opaque cube),
#   resonant_cluster (amethyst_cluster hue-set to cyan, + .mcmeta strict_cutout),
#   resonance_bloom (+ .mcmeta strict_cutout, copied from dandelion's),
#   whisperwood_log, whisperwood_log_top, stripped_whisperwood_log (+_top),
#   whisperwood_planks, whisperwood_sapling, whisperwood_door_bottom/_top and
#   whisperwood_trapdoor (oak's door/trapdoor art on the plank ramp), hush_grass (short_grass's blade
#   mask, untinted), lantern_leaves (oak_leaves' hole mask; + .mcmeta
#   dark_cutout), hush_moss, echo_lantern (lantern.png 16x48 recoloured; +
#   .mcmeta frametime 8), echo_core (16x64 4-frame strip; + .mcmeta copied from
#   sculk's: frametime 20, interpolate),
#   hush_portal (nether_portal.png hue-rotated purple -> teal with the
#   PERCEIVED luminance of every pixel matched to the source so the swirl keeps
#   its dark/light spread; alpha and the 16x512 / 32-frame strip kept; .mcmeta
#   byte-copied from nether_portal's).
# Item textures (assets/textures/item/): raw_resonite, resonite_ingot,
#   resonite_sword/_pickaxe/_axe/_shovel/_hoe, resonant_heart, echo_blade,
#   and the tools of the deep: tuning_fork, echo_compass_00..31,
#   cloak_of_silence, resonance_bow, recall_chime, whisperfruit (block art:
#   hanging_whisperfruit_stage0..2, echo_heart_side/_top) — see that section;
#   hush_lighthouse_lamp_lens/_side/_top (the lighthouse lamp).
# Entity textures (assets/textures/entity/): hushling (endermite layout),
#   echo_wraith + echo_wraith_charging (vex layout), silent_warden (warden
#   layout; the vanilla warden emissive layers are reused as-is).
#
# Seeding: default_rng([0x48555348 ("HUSH"), crc32(name)]) per texture, so
# adding or reordering textures never reshuffles the others.

from __future__ import annotations

import argparse
import colorsys
import io
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parent.parent
TEX_ROOT = REPO_ROOT / "assets" / "textures"
TEX_DIR = TEX_ROOT / "block"
ITEM_DIR = TEX_ROOT / "item"
ENTITY_DIR = TEX_ROOT / "entity"
SEED_TAG = 0x48555348  # "HUSH"
W = H = 16

# The Hush's shared palette (also in docs/the-hush.md): cyan glow ramp and
# the pale-violet resonite metal, so every texture agrees on what glows.
CYAN_DEEP, CYAN_DARK, CYAN_MID, CYAN, CYAN_PALE, CYAN_WHITE = (
    "#0E3A3F", "#145C5C", "#178C86", "#2BD4C0", "#7FEDE0", "#B8F7F0")
VIOLET_DARK, VIOLET_MID, VIOLET, VIOLET_PALE, VIOLET_WHITE = (
    "#3D3358", "#7E6BA8", "#B39DDB", "#CDBDEC", "#E6DAFF")


# ── small helpers ───────────────────────────────────────────────────────────
def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng([SEED_TAG, zlib.crc32(name.encode("utf-8"))])


def rgb(hexstr: str) -> tuple[int, int, int, int]:
    h = hexstr.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), 255)


def blank(h: int = H, w: int = W) -> np.ndarray:
    return np.zeros((h, w, 4), dtype=np.uint8)


def paint(idx: np.ndarray, palette: list[tuple[int, int, int, int]]) -> np.ndarray:
    """Index map -> RGBA. Index -1 = transparent."""
    out = blank(*idx.shape)
    for i, col in enumerate(palette):
        out[idx == i] = col
    return out


def smoothstep(t: np.ndarray) -> np.ndarray:
    return t * t * (3.0 - 2.0 * t)


def value_noise(rng: np.random.Generator, cell_w: int, cell_h: int,
                w: int = W, h: int = H) -> np.ndarray:
    """Tileable bilinear value noise on a (w/cell_w x h/cell_h) lattice."""
    gw = max(1, w // cell_w)
    gh = max(1, h // cell_h)
    lattice = rng.random((gh, gw))
    ys = np.arange(h, dtype=np.float64) / cell_h
    xs = np.arange(w, dtype=np.float64) / cell_w
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    fy = smoothstep(ys - y0)[:, None]
    fx = smoothstep(xs - x0)[None, :]
    y0 %= gh
    x0 %= gw
    y1 = (y0 + 1) % gh
    x1 = (x0 + 1) % gw
    a = lattice[np.ix_(y0, x0)]
    b = lattice[np.ix_(y0, x1)]
    c = lattice[np.ix_(y1, x0)]
    d = lattice[np.ix_(y1, x1)]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(rng: np.random.Generator, layers: list[tuple[int, int, float]],
        white: float, w: int = W, h: int = H) -> np.ndarray:
    """Weighted sum of value-noise layers (cell_w, cell_h, weight) + white noise."""
    field = np.zeros((h, w), dtype=np.float64)
    for cw, ch, weight in layers:
        field += weight * value_noise(rng, cw, ch, w, h)
    field += white * rng.random((h, w))
    # A vanishing jitter breaks rank ties deterministically.
    field += rng.random((h, w)) * 1e-9
    return field


def posterize(field: np.ndarray, weights: list[float]) -> np.ndarray:
    """Rank-quantize a field into len(weights) shades with EXACT pixel
    proportions (weights are normalised). Shade 0 = lowest field values."""
    flat = field.ravel()
    order = np.argsort(flat, kind="stable")
    total = float(sum(weights))
    n = flat.size
    idx = np.empty(n, dtype=np.int64)
    start = 0
    acc = 0.0
    for shade, wgt in enumerate(weights):
        acc += wgt / total
        end = n if shade == len(weights) - 1 else int(round(acc * n))
        idx[order[start:end]] = shade
        start = end
    return idx.reshape(field.shape)


def lerp_palette(a: str, b: str, n: int) -> list[tuple[int, int, int, int]]:
    ca, cb = rgb(a), rgb(b)
    out = []
    for i in range(n):
        t = i / (n - 1)
        out.append(tuple(int(round(ca[k] + (cb[k] - ca[k]) * t)) for k in range(3)) + (255,))
    return out


def load_vanilla(rel: str) -> np.ndarray:
    """A vanilla sprite as RGBA uint8 (rel is relative to assets/textures/)."""
    return np.array(Image.open(TEX_ROOT / rel).convert("RGBA"))


def luminance(r: float, g: float, b: float) -> float:
    return 0.299 * r + 0.587 * g + 0.114 * b


def recolor(src: np.ndarray, fn) -> np.ndarray:
    """Apply fn(x, y, h_deg, s, v, a) -> (h_deg, s, v, a) to every
    non-transparent pixel. Alpha is kept unless fn changes it."""
    out = src.copy()
    hh, ww = src.shape[:2]
    for y in range(hh):
        for x in range(ww):
            r, g, b, a = (int(c) for c in src[y, x])
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            h2, s2, v2, a2 = fn(x, y, h * 360.0, s, v, a)
            nr, ng, nb = colorsys.hsv_to_rgb((h2 % 360.0) / 360.0,
                                             min(max(s2, 0.0), 1.0), min(max(v2, 0.0), 1.0))
            out[y, x] = (int(round(nr * 255)), int(round(ng * 255)), int(round(nb * 255)),
                         int(min(max(a2, 0), 255)))
    return out


def gradient_map(src: np.ndarray, stops: list[tuple[float, str]], mask=None) -> np.ndarray:
    """Recolour by luminance: t = Y/255 -> piecewise-linear between stops.
    Alpha kept; `mask` (bool HxW) limits which pixels are touched."""
    out = src.copy()
    ts = [t for t, _ in stops]
    cols = [rgb(c) for _, c in stops]
    hh, ww = src.shape[:2]
    for y in range(hh):
        for x in range(ww):
            if src[y, x, 3] == 0 or (mask is not None and not mask[y, x]):
                continue
            t = luminance(*(int(c) for c in src[y, x, :3])) / 255.0
            t = min(max(t, ts[0]), ts[-1])
            k = 0
            while k < len(ts) - 2 and t > ts[k + 1]:
                k += 1
            f = 0.0 if ts[k + 1] == ts[k] else (t - ts[k]) / (ts[k + 1] - ts[k])
            out[y, x, :3] = [int(round(cols[k][i] + (cols[k + 1][i] - cols[k][i]) * f)) for i in range(3)]
    return out


# ── the stone family ────────────────────────────────────────────────────────
HUSHSTONE_PALETTE = lerp_palette("#2C393B", "#526467", 5)
# deepslate.png histogram: 26 / 70 / 78 / 58 / 24 of 256, darkest -> lightest.
HUSHSTONE_WEIGHTS = [26, 70, 78, 58, 24]
BRICK_MORTAR, BRICK_EDGE_DARK, BRICK_EDGE_LIGHT = "#1E2A2C", "#2A3A3E", "#526467"
BRICK_FACE = ["#33454A", "#3B4F53", "#44595C"]


def hushstone_index(rng: np.random.Generator) -> np.ndarray:
    # A taller-than-wide lattice gives the faint vertical grain deepslate has.
    field = fbm(rng, [(2, 4, 0.50), (4, 4, 0.25)], white=0.35)
    return posterize(field, HUSHSTONE_WEIGHTS)


def gen_hushstone(rng: np.random.Generator) -> np.ndarray:
    return paint(hushstone_index(rng), HUSHSTONE_PALETTE)


def gen_polished_hushstone(rng: np.random.Generator) -> np.ndarray:
    # polished_deepslate: a calm interior (3 close shades, big soft blotches)
    # inside a 1-px bevel: light top/left rows, dark bottom/right rows.
    interior = ["#33464C", "#3B4F53", "#44595C", "#4C6265"]
    bevel_light = rgb("#5F7477")
    bevel_dark = rgb("#22302F")
    field = fbm(rng, [(4, 4, 0.55), (2, 2, 0.25)], white=0.20)
    idx = posterize(field, [14, 44, 30, 12])
    out = paint(idx, [rgb(c) for c in interior])
    out[0, :] = bevel_light
    out[:, 0] = bevel_light
    out[15, :] = bevel_dark
    out[:, 15] = bevel_dark
    out[0, 15] = rgb("#3B4F53")
    out[15, 0] = rgb("#3B4F53")
    return out


def gen_hushstone_bricks(rng: np.random.Generator) -> np.ndarray:
    # deepslate_bricks: two 8-px-tall courses, mortar on rows 7 and 15, the
    # lower course offset by 8 (mortar column at x=15 above, x=7 below).
    mortar = rgb(BRICK_MORTAR)
    edge_light = rgb(BRICK_EDGE_LIGHT)
    edge_dark = rgb(BRICK_EDGE_DARK)
    field = fbm(rng, [(4, 2, 0.45), (2, 2, 0.25)], white=0.30)
    idx = posterize(field, [30, 45, 25])
    out = paint(idx, [rgb(c) for c in BRICK_FACE])
    for course in range(2):
        y0 = course * 8
        mortar_x = 15 if course == 0 else 7
        out[y0 + 7, :] = mortar
        out[:, mortar_x][y0:y0 + 8] = mortar
        # Highlight along the top row and the column right of the mortar,
        # shadow along the bottom face row and the column left of it.
        out[y0, :] = edge_light
        out[y0 + 6, :] = edge_dark
        out[y0 + 7, :] = mortar
        left = (mortar_x + 1) % 16
        right = (mortar_x - 1) % 16
        out[y0:y0 + 7, left] = edge_light
        out[y0:y0 + 7, right] = edge_dark
        out[y0 + 6, left] = edge_dark
        out[y0 + 7, :] = mortar
        out[y0:y0 + 8, mortar_x] = mortar
    return out


def gen_cracked_hushstone_bricks(rng: np.random.Generator) -> np.ndarray:
    # cracked_deepslate_bricks IS deepslate_bricks with 68 pixels changed: the
    # crack lines (much darker), chipped edges (a little darker) and two
    # bright chips. Lift that exact delta map onto the hushstone bricks.
    del rng  # nothing random of its own; the bricks seed their own rng
    out = gen_hushstone_bricks(rng_for("hushstone_bricks"))
    cracked = load_vanilla("block/cracked_deepslate_bricks.png").astype(int)
    whole = load_vanilla("block/deepslate_bricks.png").astype(int)
    delta = cracked[..., :3].mean(axis=2) - whole[..., :3].mean(axis=2)
    assert 40 <= int((delta != 0).sum()) <= 120, int((delta != 0).sum())
    crack = rgb(BRICK_MORTAR)
    chip_dark = rgb(BRICK_EDGE_DARK)
    chip = rgb(BRICK_FACE[0])
    chip_light = rgb(BRICK_EDGE_LIGHT)
    for y in range(H):
        for x in range(W):
            d = delta[y, x]
            if d <= -28:
                out[y, x] = crack
            elif d <= -15:
                out[y, x] = chip_dark
            elif d < 0:
                out[y, x] = chip
            elif d > 0:
                out[y, x] = chip_light
    return out


def gen_chiseled_hushstone_bricks(rng: np.random.Generator) -> np.ndarray:
    # chiseled_deepslate is a 6-level relief map (rim, recess, three face
    # tones, highlight). Keep the relief, swap the palette for the bricks'.
    del rng
    src = load_vanilla("block/chiseled_deepslate.png").astype(int)
    lum = src[..., :3].mean(axis=2)
    levels = sorted(set(np.round(lum).astype(int).ravel().tolist()))
    assert len(levels) == 6, levels
    pal = [BRICK_MORTAR, BRICK_EDGE_DARK, BRICK_FACE[0], BRICK_FACE[1], BRICK_FACE[2], BRICK_EDGE_LIGHT]
    lut = {lv: rgb(c) for lv, c in zip(levels, pal)}
    out = blank()
    for y in range(H):
        for x in range(W):
            out[y, x] = lut[int(round(lum[y, x]))]
    return out


def gen_sculk_loam(rng: np.random.Generator) -> np.ndarray:
    # sculk.png: almost all near-black blue, a sprinkle of deep teal, and a
    # couple of glowing spots per 16x16. Same recipe, teal-grey ground.
    base = ["#141B1D", "#182224", "#1C2B2D", "#0E3A3F"]
    field = fbm(rng, [(4, 4, 0.50), (2, 2, 0.30)], white=0.20)
    idx = posterize(field, [40, 32, 16, 12])
    out = paint(idx, [rgb(c) for c in base])
    fleck = rgb(CYAN)
    fleck_hot = rgb(CYAN_PALE)
    halo = rgb(CYAN_MID)
    halo_far = rgb(CYAN_DEEP)
    # Two glowing spots (sculk has ~1.5 bright px per 16x16), off-grid so
    # the tiling does not read as a lattice; a 2-px core, a 4-neighbour halo
    # and a sparse deep-teal fringe.
    centres = [(3, 5), (11, 12)]
    for n, (cx, cy) in enumerate(centres):
        core = [(cx, cy), (cx + 1, cy)] if n == 0 else [(cx, cy), (cx, cy + 1)]
        halo_px = set()
        for (x, y) in core:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                halo_px.add(((x + dx) % 16, (y + dy) % 16))
        far_px = set()
        for (x, y) in halo_px:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                far_px.add(((x + dx) % 16, (y + dy) % 16))
        for (x, y) in sorted(far_px - halo_px - set(core)):
            if rng.random() < 0.35:
                out[y, x] = halo_far
        for (x, y) in sorted(halo_px - set(core)):
            if rng.random() < 0.75:
                out[y, x] = halo
        for i, (x, y) in enumerate(core):
            out[y, x] = fleck_hot if i == 0 else fleck
    # One lone dim fleck, like sculk's stray #009295 pixels.
    out[1, 13] = halo
    return out


def ore_on_hushstone(clusters) -> np.ndarray:
    # The base IS the hushstone texture (vanilla deepslate ores sit on the
    # deepslate art), so the ore reads as hushstone with something in it.
    out = paint(hushstone_index(rng_for("hushstone")), HUSHSTONE_PALETTE)
    for (cx, cy), px in clusters:
        for (dx, dy), col in px:
            out[(cy + dy) % 16, (cx + dx) % 16] = rgb(col)
    return out


def gen_echo_ore(rng: np.random.Generator) -> np.ndarray:
    del rng  # the ore draws nothing of its own; determinism comes from hushstone
    bright, mid, dark, rim = CYAN_WHITE, CYAN, CYAN_MID, CYAN_DEEP
    # Three clusters, 10 cyan/white px: 2x2 with a highlight and a shadow
    # pixel, 2x2 the other way round, and a 2-px sliver; each gets 1-2 rim px.
    return ore_on_hushstone([
        ((3, 3), [((0, 0), bright), ((1, 0), mid), ((0, 1), mid), ((1, 1), dark),
                  ((2, 1), rim), ((1, 2), rim)]),
        ((10, 9), [((0, 0), mid), ((1, 0), bright), ((0, 1), dark), ((1, 1), mid),
                   ((-1, 1), rim), ((2, 0), rim)]),
        ((12, 2), [((0, 0), mid), ((0, 1), dark), ((1, 1), rim)]),
    ])


def gen_resonite_ore(rng: np.random.Generator) -> np.ndarray:
    del rng
    # Pale-violet metal in hushstone: 10 violet/white px in three clusters at
    # different spots from echo_ore's, with a dark violet outline pixel each.
    bright, mid, dark, rim = VIOLET_WHITE, VIOLET, VIOLET_MID, "#26203A"
    return ore_on_hushstone([
        ((5, 2), [((0, 0), mid), ((1, 0), bright), ((0, 1), dark), ((1, 1), mid),
                  ((2, 1), rim), ((0, 2), rim)]),
        ((11, 8), [((0, 0), bright), ((1, 0), mid), ((0, 1), mid), ((1, 1), dark),
                   ((-1, 0), rim), ((1, 2), rim)]),
        ((3, 12), [((0, 0), mid), ((1, 0), dark), ((2, 0), rim), ((0, 1), rim)]),
    ])


def gen_resonite_block(rng: np.random.Generator) -> np.ndarray:
    # iron_block's shading (bevelled rim, three horizontal bands of light /
    # mid / dark per 3 rows) in pale violet, with the rim pulled toward cyan.
    del rng
    src = load_vanilla("block/iron_block.png")
    lum = src[..., :3].astype(int).mean(axis=2)
    lo, hi = lum.min(), lum.max()
    out = gradient_map(src, [(lo / 255.0, VIOLET_MID), ((lo + hi) / 2 / 255.0, VIOLET),
                             (hi / 255.0, "#EEE6FF")])
    rim = rgb("#5FD9CC")
    for y in range(H):
        for x in range(W):
            if x in (0, 15) or y in (0, 15):
                out[y, x, :3] = [int(round(out[y, x, i] * 0.45 + rim[i] * 0.55)) for i in range(3)]
    return out


def gen_resonant_crystal(rng: np.random.Generator) -> np.ndarray:
    # amethyst_block: a few large angular facets, each a flat mid tone, lit
    # from the top-left (a lighter edge where a facet meets the one above or
    # left of it, a darker edge on its bottom/right side) and pale sparkles
    # at the lit corners.
    shades = [CYAN_DEEP, CYAN_DARK, "#1A7F7A", "#22A39A", CYAN, CYAN_PALE, CYAN_WHITE]
    pal = [rgb(c) for c in shades]
    n_seeds = 6
    seeds = rng.random((n_seeds, 2)) * 16.0
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    best = np.full((H, W), np.inf)
    cell = np.zeros((H, W), dtype=np.int64)
    for i, (sx, sy) in enumerate(seeds):
        dx = np.abs(xs + 0.5 - sx)
        dy = np.abs(ys + 0.5 - sy)
        dx = np.minimum(dx, 16.0 - dx)
        dy = np.minimum(dy, 16.0 - dy)
        # Manhattan-leaning metric -> angular, gem-like facets.
        d = 0.75 * (dx + dy) + 0.25 * np.sqrt(dx * dx + dy * dy)
        take = d < best
        best = np.where(take, d, best)
        cell = np.where(take, i, cell)
    facet_shade = rng.choice([2, 3, 3, 3, 4, 4], size=n_seeds)
    idx = facet_shade[cell]
    above = np.roll(cell, 1, axis=0)
    left = np.roll(cell, 1, axis=1)
    below = np.roll(cell, -1, axis=0)
    right = np.roll(cell, -1, axis=1)
    lit = (cell != above) | (cell != left)
    shadow = ((cell != below) | (cell != right)) & ~lit
    idx = np.where(lit, np.minimum(idx + 1, 5), idx)
    idx = np.where(shadow, np.maximum(idx - 1, 1), idx)
    # Sparse grain inside the facets: a pixel one shade off here and there.
    grain = rng.random((H, W))
    inner = ~lit & ~shadow
    idx = np.where((grain < 0.06) & inner, np.maximum(idx - 1, 1), idx)
    idx = np.where((grain > 0.95) & inner, np.minimum(idx + 1, 5), idx)
    out = paint(idx, pal)
    # Sparkles at lit corners (both neighbours differ), like amethyst's #FECBE6.
    corners = np.argwhere((cell != above) & (cell != left))
    picks = rng.choice(len(corners), size=min(5, len(corners)), replace=False)
    for n, k in enumerate(picks):
        y, x = corners[k]
        out[y, x] = pal[6] if n < 3 else pal[5]
    assert (out[..., 3] == 255).all(), "resonant_crystal must be fully opaque"
    return out


def gen_resonant_cluster(rng: np.random.Generator) -> np.ndarray:
    # amethyst_cluster with its hue set to the Hush cyan; saturation and
    # value (and so the crystal's shading and pale tips) are the vanilla ones.
    del rng
    src = load_vanilla("block/amethyst_cluster.png")

    def fn(x, y, h, s, v, a):
        return 174.0, s, v, a

    return recolor(src, fn)


BLOOM_ART = [
    "................",
    "................",
    "......AB........",
    ".....ABCB.......",
    "....ABCDCB......",
    "....BCDEDCB.....",
    ".....CDEDC......",
    "......CDC.......",
    ".......s........",
    ".......s..T.....",
    "......ts.T......",
    ".....t.sT.......",
    "......tsS.......",
    ".......s........",
    ".......s........",
    "......ss........",
]
BLOOM_KEY = {
    "A": CYAN_WHITE, "B": CYAN, "C": CYAN_MID, "D": CYAN, "E": CYAN_WHITE,
    "s": "#1F4A46", "S": "#1F4A46", "t": "#2E6F67", "T": "#2E6F67",
}


def plot_art(art: list[str], key: dict[str, str]) -> np.ndarray:
    out = blank()
    for y, row in enumerate(art):
        assert len(row) == 16, row
        for x, ch in enumerate(row):
            if ch != ".":
                out[y, x] = rgb(key[ch])
    return out


def gen_resonance_bloom(rng: np.random.Generator) -> np.ndarray:
    del rng  # hand-plotted sprite; nothing random
    out = plot_art(BLOOM_ART, BLOOM_KEY)
    count = int((out[..., 3] > 0).sum())
    assert 30 <= count <= 45, count
    return out


# oak_sapling's silhouette: a small trunk with a two-lobed crown. Teal-grey
# leaves with a cyan bud where the lantern leaves will grow.
SAPLING_ART = [
    "................",
    "................",
    ".......LL.......",
    "......LMML......",
    ".....LMDMDL.....",
    "....LMDMMDML....",
    "....LDMMMMDL....",
    ".....LMDMDL.....",
    "......LMML......",
    ".......ss.......",
    ".......ss.......",
    "......tss.......",
    ".......ss.......",
    ".......ss.......",
    "................",
    "................",
]
SAPLING_KEY = {"L": "#27504A", "M": "#3A6F66", "D": "#1E3B36", "s": "#2E4340", "t": "#3F5551"}


def gen_whisperwood_sapling(rng: np.random.Generator) -> np.ndarray:
    del rng
    out = plot_art(SAPLING_ART, SAPLING_KEY)
    # Two young lantern buds in the crown.
    out[5, 9] = rgb(CYAN)
    out[7, 6] = rgb(CYAN_PALE)
    return out


LOG_PALETTE = ["#1A2729", "#223235", "#2A3B3E", "#344A4D", "#3F595C", "#4A6669"]
# oak_log.png: 19 / 37 / 38 / 108 / 42 / 12 of 256.
LOG_WEIGHTS = [19, 37, 38, 108, 42, 12]
PLANK_PALETTE = ["#2E4340", "#3F5551", "#5C7570", "#6E8983", "#7F9994", "#8FA9A4"]
# Stripped wood is the planks' colour with a little more light in it.
STRIPPED_PALETTE = ["#4A625E", "#597470", "#67827D", "#75908A", "#839E98"]


def gen_whisperwood_log(rng: np.random.Generator) -> np.ndarray:
    # Bark: long vertical streaks (1-2 px wide, 4-8 px tall).
    field = fbm(rng, [(2, 8, 0.45), (1, 4, 0.30)], white=0.25)
    return paint(posterize(field, LOG_WEIGHTS), [rgb(c) for c in LOG_PALETTE])


def gen_stripped_whisperwood_log(rng: np.random.Generator) -> np.ndarray:
    # stripped_oak_log: long vertical grain in two dominant close shades with
    # a few darker and lighter streaks; no bark.
    field = fbm(rng, [(1, 8, 0.45), (2, 4, 0.30)], white=0.15)
    return paint(posterize(field, [14, 46, 96, 70, 30]), [rgb(c) for c in STRIPPED_PALETTE])


def log_top(rng: np.random.Generator, rim_a: str, rim_b: str, ring_palette: list[str]) -> np.ndarray:
    # oak_log_top: 1-px rim, then square growth rings alternating light/dark,
    # a small dark heart, and some grain noise on the rings.
    ring_shades = [1, 4, 3, 5, 3, 4, 2]  # r = 0 .. 6 into ring_palette
    out = blank()
    noise = rng.random((H, W))
    for y in range(H):
        for x in range(W):
            r = int(max(abs(x - 7.5), abs(y - 7.5)))  # 0..7 (Chebyshev)
            if r >= 7:
                out[y, x] = rgb(rim_a) if ((x + y) % 3 != 0) else rgb(rim_b)
                continue
            shade = ring_shades[r]
            n = noise[y, x]
            if n < 0.12:
                shade = max(1, shade - 1)
            elif n > 0.90:
                shade = min(5, shade + 1)
            out[y, x] = rgb(ring_palette[shade])
    return out


def gen_whisperwood_log_top(rng: np.random.Generator) -> np.ndarray:
    return log_top(rng, "#2A3B3E", "#223235", PLANK_PALETTE)


def gen_stripped_whisperwood_log_top(rng: np.random.Generator) -> np.ndarray:
    # stripped_oak_log_top: the rim is the darkest stripped shade, not bark.
    return log_top(rng, "#4A625E", "#3F5551", PLANK_PALETTE)


def gen_whisperwood_planks(rng: np.random.Generator) -> np.ndarray:
    # oak_planks: four 4-px-tall boards, each 3 face rows over a 1-px dark
    # seam row; the face rows run light (top) to darker (bottom) and are
    # painted as horizontal grain runs 2-6 px long; one end-seam column per
    # board, alternating x=15 / x=7 so the ends stagger every 8 px. The end
    # seam is a step lighter than the seam row, as on oak.
    face = ["#597470", "#67827D", "#75908A", "#839E98", "#90ACA6"]
    seam = rgb(PLANK_PALETTE[0])
    seam_light = rgb(PLANK_PALETTE[1])
    end_seam = rgb(PLANK_PALETTE[1])
    end_seam_mid = rgb("#4F6864")
    row_shades = [[3, 4, 4, 3], [2, 3, 3, 4], [1, 2, 2, 3]]
    out = blank()
    for board in range(4):
        y0 = board * 4
        for row in range(3):
            y = y0 + row
            x = 0
            while x < W:
                run = int(rng.integers(2, 7))
                shade = int(rng.choice(row_shades[row]))
                out[y, x:min(W, x + run)] = rgb(face[shade])
                x += run
        for x in range(W):
            out[y0 + 3, x] = seam if rng.random() < 0.7 else seam_light
        sx = 15 if board % 2 == 0 else 7
        out[y0, sx] = end_seam
        out[y0 + 1, sx] = end_seam_mid
        out[y0 + 2, sx] = end_seam
    return out


# oak_door_* / oak_trapdoor luminance (0.24-0.60) -> the whisperwood plank ramp,
# darkest for the door's outline and window frame, lightest for the panels.
WOOD_STOPS = [(0.24, "#1E2E2C"), (0.32, "#2E4340"), (0.40, "#3F5551"), (0.47, "#597470"),
              (0.52, "#67827D"), (0.57, "#75908A"), (0.60, "#839E98")]


def wood_recolor(src_name: str) -> np.ndarray:
    return gradient_map(load_vanilla(f"block/{src_name}.png"), WOOD_STOPS)


def gen_whisperwood_door_bottom(rng): del rng; return wood_recolor("oak_door_bottom")
def gen_whisperwood_door_top(rng): del rng; return wood_recolor("oak_door_top")
def gen_whisperwood_trapdoor(rng): del rng; return wood_recolor("oak_trapdoor")


def gen_lantern_leaves(rng: np.random.Generator) -> np.ndarray:
    # oak_leaves' own hole mask (84 of 256 transparent) and its four grey
    # levels, recoloured into the teal leaf palette; the block is Cutout and
    # NOT biome-tinted, so these are the real colours. Then a handful of
    # glowing lantern pixels hung on leaves (never in holes).
    del rng
    src = load_vanilla("block/oak_leaves.png")
    leaf = ["#1E3B36", "#27504A", "#2F5F58", "#3A6F66"]
    opaque = src[..., 3] > 0
    greys = sorted(set(int(luminance(*src[y, x, :3])) for y in range(H) for x in range(W) if opaque[y, x]))
    assert len(greys) == 4, greys
    lut = {g: rgb(c) for g, c in zip(greys, leaf)}
    out = blank()
    for y in range(H):
        for x in range(W):
            if opaque[y, x]:
                out[y, x] = lut[int(luminance(*src[y, x, :3]))]
    lanterns = [((2, 3), CYAN_PALE), ((9, 1), CYAN), ((13, 6), CYAN), ((5, 10), CYAN),
                ((11, 13), CYAN_PALE), ((1, 14), CYAN), ((14, 11), CYAN), ((7, 7), CYAN_PALE)]
    placed = 0
    for (x, y), col in lanterns:
        # Slide right along the row to the nearest leaf pixel.
        for k in range(W):
            xx = (x + k) % W
            if opaque[y, xx]:
                out[y, xx] = rgb(col)
                placed += 1
                break
    assert 6 <= placed <= 8, placed
    return out


def gen_hush_moss(rng: np.random.Generator) -> np.ndarray:
    # moss_block: 6 shades, soft blobs a few pixels across.
    moss = ["#17403A", "#1F4A44", "#26584F", "#2E6A5E", "#36786A", "#3F8A78"]
    field = fbm(rng, [(4, 4, 0.45), (2, 2, 0.35)], white=0.20)
    return paint(posterize(field, [18, 68, 44, 71, 32, 23]), [rgb(c) for c in moss])


def gen_hush_grass(rng: np.random.Generator) -> np.ndarray:
    # short_grass's blade mask and its six grey levels (the vanilla sprite is
    # greyscale and tinted in-game; the Hush's is untinted) as teal-grey
    # blades; the tips of the taller blades (nothing above, upper half) glow cyan.
    del rng
    src = load_vanilla("block/short_grass.png")
    blades = ["#3A5652", "#44625D", "#4E6E69", "#5A7B75", "#688A83", "#7A9C95"]
    opaque = src[..., 3] > 0
    greys = sorted(set(int(luminance(*src[y, x, :3])) for y in range(H) for x in range(W) if opaque[y, x]))
    assert len(greys) == 6, greys
    lut = {g: rgb(c) for g, c in zip(greys, blades)}
    out = blank()
    tips = 0
    for y in range(H):
        for x in range(W):
            if not opaque[y, x]:
                continue
            g = int(luminance(*src[y, x, :3]))
            is_tip = y == 0 or not opaque[y - 1, x]
            if is_tip and y <= 7:                       # the taller blades glow
                out[y, x] = rgb(CYAN_PALE if g >= greys[3] else CYAN)
                tips += 1
            elif is_tip:                                # short blades: a lighter tip
                out[y, x] = lut[greys[min(5, greys.index(g) + 1)]]
            else:
                out[y, x] = lut[g]
    assert 6 <= tips <= 20, tips
    return out


def gen_echo_lantern(rng: np.random.Generator) -> np.ndarray:
    # lantern.png (16x48, 3 frames): the iron frame (blue-grey, h~223) becomes
    # hushstone grey, the flame/glass (orange -> yellow, h 19-40) becomes the
    # cyan glow ramp, dark glass deep teal and the hottest pixels pale cyan.
    del rng
    src = load_vanilla("block/lantern.png")
    assert src.shape == (48, 16, 4), src.shape

    def fn(x, y, h, s, v, a):
        if 5.0 <= h <= 70.0 and s > 0.4:          # flame + glass
            if v < 0.6:
                return 176.0, 0.85, v * 0.95, a    # dark glass -> deep teal
            if v < 0.94:
                return 174.0, 0.8, v, a            # orange -> cyan
            return 172.0, 0.4, 1.0, a              # yellow core -> pale cyan
        return 190.0, min(s, 0.3), v * 0.95, a     # frame -> hushstone grey

    return recolor(src, fn)


def gen_echo_core(rng: np.random.Generator) -> np.ndarray:
    # 16x64 strip, 4 frames, interpolated at frametime 20 (sculk's .mcmeta):
    # a near-black sculk-like cube with a four-fold sigil - a violet diamond
    # ring, cyan inner marks and a 2x2 cyan core - that pulses dim -> bright.
    base = ["#07090B", "#0B1013", "#0F1619", "#12201F"]
    field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.3)], white=0.2)
    ground = paint(posterize(field, [50, 30, 14, 6]), [rgb(c) for c in base])
    ys, xs = np.mgrid[0:H, 0:W]
    dx = np.abs(xs - 7.5)
    dy = np.abs(ys - 7.5)
    ring = (dx + dy) == 5.0
    core = (dx + dy) == 1.0
    inner = (dx == 1.5) & (dy == 1.5)
    marks = ((dx == 0.5) & (dy == 6.5)) | ((dy == 0.5) & (dx == 6.5))
    pulse = [0.55, 0.8, 1.0, 0.8]

    def scaled(hexstr: str, f: float) -> tuple[int, int, int, int]:
        r, g, b, _ = rgb(hexstr)
        return (int(round(r * f)), int(round(g * f)), int(round(b * f)), 255)

    out = blank(H * 4, W)
    for frame, f in enumerate(pulse):
        tile = ground.copy()
        tile[ring] = scaled("#9C7BE8", f)
        tile[marks] = scaled(VIOLET_MID, f)
        tile[inner] = scaled(CYAN, f)
        tile[core] = scaled(CYAN_PALE, 0.7 + 0.3 * f)
        out[frame * H:(frame + 1) * H] = tile
    return out


HUSH_PORTAL_HUE_SHIFT = -93.0  # degrees: purple (~265) -> sculk teal (~172)


def gen_hush_portal(rng: np.random.Generator) -> np.ndarray:
    # A plain hue rotation keeps HSV value but not perceived brightness: teal
    # is far brighter than purple at the same V, so the swirl's dark/light
    # spread collapsed. Rotate the hue, then solve V so each pixel's Rec.601
    # luminance equals the source pixel's; the per-frame luminance histogram
    # of the result is then the nether portal's.
    del rng
    src = load_vanilla("block/nether_portal.png")
    assert src.shape == (512, 16, 4), src.shape
    assert src.shape[0] // 16 == 32, "nether_portal.png must be a 32-frame strip"
    out = src.copy()
    cache: dict[tuple[int, int, int], tuple[int, int, int]] = {}
    flat = out.reshape(-1, 4)
    for i in range(flat.shape[0]):
        r, g, b = int(flat[i, 0]), int(flat[i, 1]), int(flat[i, 2])
        key = (r, g, b)
        if key not in cache:
            target = luminance(r, g, b) / 255.0
            h, s, _v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            h = (h + HUSH_PORTAL_HUE_SHIFT / 360.0) % 1.0
            unit = colorsys.hsv_to_rgb(h, s, 1.0)
            k = luminance(*unit)
            v = min(1.0, target / k) if k > 0 else target
            nr, ng, nb = colorsys.hsv_to_rgb(h, s, v)
            cache[key] = (int(round(nr * 255)), int(round(ng * 255)), int(round(nb * 255)))
        flat[i, 0:3] = cache[key]
    assert (out[..., 3] == src[..., 3]).all(), "alpha must be preserved"
    return out


# ── items ───────────────────────────────────────────────────────────────────
RESONITE_STOPS = [(0.10, "#221C33"), (0.23, VIOLET_DARK), (0.36, "#5E5285"), (0.53, VIOLET_MID),
                  (0.70, VIOLET), (0.86, VIOLET_PALE), (0.93, VIOLET_WHITE), (1.0, "#F6F1FF")]


def gen_raw_resonite(rng: np.random.Generator) -> np.ndarray:
    del rng
    return gradient_map(load_vanilla("item/raw_iron.png"), RESONITE_STOPS)


def gen_resonite_ingot(rng: np.random.Generator) -> np.ndarray:
    del rng
    return gradient_map(load_vanilla("item/iron_ingot.png"), RESONITE_STOPS)


def resonite_tool(src_name: str) -> np.ndarray:
    # diamond_* tools: the cyan head (h 160-200) becomes pale violet with a
    # little less saturation; the stick and its outline are untouched.
    src = load_vanilla(f"item/{src_name}.png")

    def fn(x, y, h, s, v, a):
        if 150.0 <= h <= 200.0 and s > 0.3:
            return 262.0, s * 0.5, min(1.0, v * 1.1), a
        return h, s, v, a

    return recolor(src, fn)


def gen_resonite_sword(rng): del rng; return resonite_tool("diamond_sword")
def gen_resonite_pickaxe(rng): del rng; return resonite_tool("diamond_pickaxe")
def gen_resonite_axe(rng): del rng; return resonite_tool("diamond_axe")
def gen_resonite_shovel(rng): del rng; return resonite_tool("diamond_shovel")
def gen_resonite_hoe(rng): del rng; return resonite_tool("diamond_hoe")


def gen_resonant_heart(rng: np.random.Generator) -> np.ndarray:
    # heart_of_the_sea (sea blue, h 185-221) pulled to the Hush teal and lit
    # up: value x1.25 so the orb reads as a light source, not a stone.
    del rng
    src = load_vanilla("item/heart_of_the_sea.png")

    def fn(x, y, h, s, v, a):
        return h - 22.0, min(1.0, s * 1.05), min(1.0, v * 1.25), a

    return recolor(src, fn)


def gen_echo_blade(rng: np.random.Generator) -> np.ndarray:
    # netherite_sword: the blade (upper-right of the anti-diagonal, d >= 14)
    # is repainted by value into the dark-teal ramp with the lightest pixels
    # - the cutting edge - bright cyan; the guard and grip keep the netherite
    # look so the silhouette reads as a sword.
    del rng
    src = load_vanilla("item/netherite_sword.png")
    blade = np.zeros((H, W), dtype=bool)
    for y in range(H):
        for x in range(W):
            blade[y, x] = (x + (15 - y)) >= 14
    edge_stops = [(0.02, "#07161A"), (0.12, CYAN_DEEP), (0.24, CYAN_DARK), (0.33, "#1A7F7A"),
                  (0.42, "#22A39A"), (0.50, CYAN), (0.60, CYAN_PALE)]
    return gradient_map(src, edge_stops, mask=blade)


# ── the tools of the deep (2026-09-22) ──────────────────────────────────────
# Items: tuning_fork (hand-plotted), echo_compass_00..31 (compass_NN: the red
# needle cyan, the face dark teal, the case resonite violet), cloak_of_silence
# (leather_chestplate's shading on a teal-black ramp), resonance_bow (bow.png:
# the wood on the whisperwood ramp, the string cyan), recall_chime (bell.png on
# the resonite ramp), whisperfruit (glow_berries: berries cyan, leaf teal).
# Blocks: hanging_whisperfruit_stage0..2 (cave_vines / cave_vines_lit, the
# leaves teal, the berries violet buds then cyan fruit; + .mcmeta none needed),
# echo_heart_side / echo_heart_top (lodestone's faces on a dark resonite ramp
# with a glowing cyan core).

FORK_ART = [
    "................",
    "....A.....A.....",
    "....B.....B.....",
    "....B.....B.....",
    "....BC....BC....",
    "....BC....BC....",
    "....BC....BC....",
    "....BC....BC....",
    "....BCD..DBC....",
    ".....BCDDBC.....",
    "......BCCB......",
    ".......EE.......",
    ".......EF.......",
    ".......EF.......",
    ".......EF.......",
    "........G.......",
]
FORK_KEY = {"A": CYAN_WHITE, "B": VIOLET_PALE, "C": VIOLET_MID, "D": VIOLET,
            "E": "#5E5285", "F": VIOLET_DARK, "G": CYAN}


def gen_tuning_fork(rng: np.random.Generator) -> np.ndarray:
    # A two-tined fork of resonite, the tine tips ringing cyan, the stem
    # darker so it reads as the handle.
    del rng
    out = plot_art(FORK_ART, FORK_KEY)
    count = int((out[..., 3] > 0).sum())
    assert 35 <= count <= 50, count
    return out


def echo_compass_frame(index: int) -> np.ndarray:
    src = load_vanilla(f"item/compass_{index:02d}.png")

    def fn(x, y, h, s, v, a):
        if (h < 20.0 or h > 340.0) and s > 0.45:   # the red needle
            return 172.0, 0.75, min(1.0, v * 1.35), a
        if v > 0.82 and s < 0.2:                    # the white dial
            return 182.0, 0.55, 0.30 + (v - 0.82) * 0.8, a
        return 262.0, 0.35, v * 0.85, a            # the grey case

    return recolor(src, fn)


def _compass_gen(index: int):
    def gen(rng: np.random.Generator) -> np.ndarray:
        del rng
        return echo_compass_frame(index)
    return gen


CLOAK_STOPS = [(0.05, "#060C0E"), (0.25, "#0B1A1D"), (0.45, "#10292C"), (0.62, CYAN_DEEP),
               (0.78, CYAN_DARK), (0.92, CYAN_MID), (1.0, CYAN)]


def gen_cloak_of_silence(rng: np.random.Generator) -> np.ndarray:
    # leather_chestplate is grey (the dye tints it); its shading on a
    # teal-black ramp — a dark cloak whose lit edges catch the Hush's cyan.
    del rng
    return gradient_map(load_vanilla("item/leather_chestplate.png"), CLOAK_STOPS)


def gen_resonance_bow(rng: np.random.Generator) -> np.ndarray:
    # bow.png: the limbs (brown, h 20-50) onto the whisperwood ramp, the
    # string (grey, unsaturated) bright cyan.
    del rng
    src = load_vanilla("item/bow.png")
    wood = np.zeros((H, W), dtype=bool)
    for y in range(H):
        for x in range(W):
            r, g, b, a = (int(c) for c in src[y, x])
            if a == 0:
                continue
            hh, ss, _ = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            wood[y, x] = ss > 0.2 and 10.0 <= hh * 360.0 <= 60.0
    out = gradient_map(src, [(0.10, "#152220"), (0.20, "#1E2E2C"), (0.30, "#2E4340"),
                             (0.40, "#3F5551"), (0.50, "#597470"), (0.60, "#75908A")], mask=wood)

    def string(x, y, h, s, v, a):
        if wood[y, x]:
            return h, s, v, a
        return 172.0, 0.6, min(1.0, 0.55 + v * 0.45), a

    return recolor(out, string)


def gen_recall_chime(rng: np.random.Generator) -> np.ndarray:
    # bell.png on the resonite ramp: a chime cast from the Hush's metal.
    del rng
    return gradient_map(load_vanilla("item/bell.png"), RESONITE_STOPS)


def whisper_berry_recolor(src: np.ndarray, berry: str) -> np.ndarray:
    # Leaves (green, h 60-160) to the lantern-leaf teal; the berries
    # (orange/yellow, h 10-60) to `berry`: "bud" violet, "ripe" glowing cyan.
    def fn(x, y, h, s, v, a):
        if 60.0 <= h <= 170.0:
            return 168.0, s * 0.55, v * 0.62, a
        if h < 60.0 or h > 340.0:
            if berry == "ripe":
                return 172.0, min(1.0, s * 0.7), min(1.0, v * 1.15 + 0.1), a
            return 262.0, s * 0.35, v * 0.72, a
        return h, s, v, a

    return recolor(src, fn)


def gen_whisperfruit(rng: np.random.Generator) -> np.ndarray:
    del rng
    return whisper_berry_recolor(load_vanilla("item/glow_berries.png"), "ripe")


def gen_hanging_whisperfruit_stage0(rng: np.random.Generator) -> np.ndarray:
    del rng
    return whisper_berry_recolor(load_vanilla("block/cave_vines.png"), "bud")


def gen_hanging_whisperfruit_stage1(rng: np.random.Generator) -> np.ndarray:
    del rng
    return whisper_berry_recolor(load_vanilla("block/cave_vines_lit.png"), "bud")


def gen_hanging_whisperfruit_stage2(rng: np.random.Generator) -> np.ndarray:
    del rng
    return whisper_berry_recolor(load_vanilla("block/cave_vines_lit.png"), "ripe")


HEART_STOPS = [(0.10, "#120E1C"), (0.30, "#221C33"), (0.45, VIOLET_DARK), (0.60, "#5E5285"),
               (0.75, VIOLET_MID), (0.90, VIOLET), (1.0, VIOLET_PALE)]


def heart_face(src_name: str, core: int) -> np.ndarray:
    # lodestone's face (its rings and rim) on a dark resonite ramp, then a
    # glowing core: a `core`-wide square in the middle, cyan-white at the
    # centre fading to cyan — the choir heart set in the stone.
    out = gradient_map(load_vanilla(f"block/{src_name}.png"), HEART_STOPS)
    lo = (W - core) // 2
    for y in range(lo, lo + core):
        for x in range(lo, lo + core):
            edge = min(x - lo, y - lo, lo + core - 1 - x, lo + core - 1 - y)
            out[y, x] = rgb(CYAN_WHITE if edge >= 1 else CYAN)
    return out


def gen_echo_heart_side(rng: np.random.Generator) -> np.ndarray:
    del rng
    return heart_face("lodestone_side", 4)


def gen_echo_heart_top(rng: np.random.Generator) -> np.ndarray:
    del rng
    return heart_face("lodestone_top", 6)


# ── the lighthouse lamp (2026-09-22) ────────────────────────────────────────
# hush_lighthouse_lamp_lens (the Fresnel lens: horizontal prism steps in the
# cyan glow ramp, brightest across the middle, a resonite post at each
# corner), _side (the resonite metal of its base and cap, rivets, a cyan rim
# where each band meets the lens), _top (the cap plate: a glowing vent ring).
# The model (assets/models/block/hush_lighthouse_lamp.json) shows the lens on
# x 2..14 / y 3..13, the base band on rows 13..15 of _side and the cap band on
# rows 0..2, the cap plate on x/z 1..15 of _top.
LAMP_METAL = ["#221C33", VIOLET_DARK, "#5E5285"]


def lamp_metal(rng: np.random.Generator) -> np.ndarray:
    field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.3)], white=0.2)
    return paint(posterize(field, [30, 50, 20]), [rgb(c) for c in LAMP_METAL])


def gen_hush_lighthouse_lamp_lens(rng: np.random.Generator) -> np.ndarray:
    shades = [rgb(CYAN_MID), rgb(CYAN), rgb(CYAN_PALE), rgb(CYAN_WHITE)]
    idx = np.zeros((H, W), dtype=np.int64)
    for y in range(H):
        for x in range(W):
            level = 3 - int(abs(y - 7.5) / 1.6)
            if y % 3 == 0:
                level -= 1                    # the prism step between two rings
            if x in (3, 12):
                level -= 1                    # the lens curving away at its corners
            idx[y, x] = min(max(level, 0), 3)
    out = paint(idx, shades)
    # Three glints on the central rings.
    for _ in range(3):
        gx, gy = int(rng.integers(4, 12)), int(rng.integers(6, 10))
        out[gy, gx] = rgb("#E8FFFC")
    # The lens's corner posts, resonite with a pale rivet every third row.
    for x in (2, 13):
        for y in range(H):
            out[y, x] = rgb(VIOLET_MID if y % 3 == 1 else VIOLET_DARK)
    return out


def gen_hush_lighthouse_lamp_side(rng: np.random.Generator) -> np.ndarray:
    out = lamp_metal(rng)
    for top, bottom in ((0, 2), (13, 15)):
        out[top, :] = rgb(VIOLET_MID)        # bevel, lit from above
        out[bottom, :] = rgb("#1A1528")      # bevel, in shadow
        for x in (1, 5, 10, 14):
            out[top + 1, x] = rgb(VIOLET_PALE)   # rivets
    out[2, 1:15] = rgb(CYAN)                 # the cap's rim over the lens
    out[13, 1:15] = rgb(CYAN)                # the base's rim under it
    return out


def gen_hush_lighthouse_lamp_top(rng: np.random.Generator) -> np.ndarray:
    out = lamp_metal(rng)
    out[0, :] = out[:, 0] = rgb(VIOLET_MID)
    out[15, :] = out[:, 15] = rgb("#1A1528")
    for y in range(H):
        for x in range(W):
            d = ((x - 7.5) ** 2 + (y - 7.5) ** 2) ** 0.5
            if 2.6 <= d < 3.7:
                out[y, x] = rgb(CYAN_MID)    # the vent ring, the lamp's glow below
            elif d < 1.2:
                out[y, x] = rgb(CYAN_PALE)
    for x, y in ((2, 2), (13, 2), (2, 13), (13, 13)):
        out[y, x] = rgb(VIOLET_PALE)
    return out


# ── entities ────────────────────────────────────────────────────────────────
def gen_hushling(rng: np.random.Generator) -> np.ndarray:
    # endermite.png (64x32): the purple body (h ~270, v .28-.36) becomes
    # black-teal; two cyan eyes on the head segment's front face (box 4x3x2
    # at uv 0,0 -> north face is u 2..5, v 2..4).
    del rng
    src = load_vanilla("entity/endermite.png")
    assert src.shape == (32, 64, 4), src.shape

    def fn(x, y, h, s, v, a):
        return 180.0, 0.6, v * 0.5, a

    out = recolor(src, fn)
    for (x, y) in ((2, 3), (5, 3)):
        assert src[y, x, 3] == 255, (x, y)
        out[y, x] = rgb(CYAN_PALE)
    return out


def wraith_from_vex(src: np.ndarray) -> np.ndarray:
    # vex.png (32x32): opaque body pixels (h ~209 grey-blue) -> teal at alpha
    # 170; the wings (already alpha 160) -> pale teal; the white eyes (alpha
    # 255, luminance > 200) stay opaque and turn bright cyan; the charging
    # red (h 320-360) -> cyan so the "angry" state glows.
    def fn(x, y, h, s, v, a):
        if a == 255 and luminance(*colorsys.hsv_to_rgb(h / 360.0, s, v)) * 255.0 > 200.0 and s < 0.15:
            return 172.0, 0.45, 1.0, 255                     # eyes
        if a < 255:
            return 172.0, min(1.0, s + 0.25), v, a          # wings
        if h >= 320.0 or h <= 15.0:
            return 178.0, min(1.0, s + 0.1), min(1.0, v * 1.1), 170  # charging red -> cyan
        return 172.0, min(1.0, s * 1.6), v, 170              # body

    return recolor(src, fn)


def gen_echo_wraith(rng: np.random.Generator) -> np.ndarray:
    del rng
    src = load_vanilla("entity/illager/vex.png")
    assert src.shape == (32, 32, 4), src.shape
    return wraith_from_vex(src)


def gen_echo_wraith_charging(rng: np.random.Generator) -> np.ndarray:
    del rng
    src = load_vanilla("entity/illager/vex_charging.png")
    assert src.shape == (32, 32, 4), src.shape
    return wraith_from_vex(src)


def gen_silent_warden(rng: np.random.Generator) -> np.ndarray:
    # warden.png (128x128): the near-black blue hide goes black-teal and a
    # touch darker; the sculk growth (h 185-200, saturated) becomes brighter
    # cyan veins; the bone-cream ribs/horns (h 50-150, low sat) go pale cyan
    # so nothing warm is left on it. Emissive layers are the vanilla ones.
    del rng
    src = load_vanilla("entity/warden/warden.png")
    assert src.shape == (128, 128, 4), src.shape

    def fn(x, y, h, s, v, a):
        if v < 0.2:
            return 186.0, min(1.0, s + 0.1), v * 0.85, a
        if s > 0.7 and 180.0 <= h <= 200.0:
            return 174.0, s, min(1.0, v * 1.45), a
        if 50.0 <= h <= 150.0 and s < 0.35:
            return 176.0, 0.4, v, a
        return 186.0, s, v * 0.9, a

    return recolor(src, fn)


# ── registry ────────────────────────────────────────────────────────────────
# name -> (generator, mcmeta source (relative to assets/textures/) or None, kind)
BLOCK, ITEM, ENTITY = "block", "item", "entity"
TEXTURES = {
    "hushstone": (gen_hushstone, None, BLOCK),
    "polished_hushstone": (gen_polished_hushstone, None, BLOCK),
    "hushstone_bricks": (gen_hushstone_bricks, None, BLOCK),
    "cracked_hushstone_bricks": (gen_cracked_hushstone_bricks, None, BLOCK),
    "chiseled_hushstone_bricks": (gen_chiseled_hushstone_bricks, None, BLOCK),
    "sculk_loam": (gen_sculk_loam, None, BLOCK),
    "echo_ore": (gen_echo_ore, None, BLOCK),
    "resonite_ore": (gen_resonite_ore, None, BLOCK),
    "resonite_block": (gen_resonite_block, None, BLOCK),
    "resonant_crystal": (gen_resonant_crystal, None, BLOCK),
    "resonant_cluster": (gen_resonant_cluster, "block/amethyst_cluster", BLOCK),
    "resonance_bloom": (gen_resonance_bloom, "block/dandelion", BLOCK),
    "whisperwood_log": (gen_whisperwood_log, None, BLOCK),
    "whisperwood_log_top": (gen_whisperwood_log_top, None, BLOCK),
    "stripped_whisperwood_log": (gen_stripped_whisperwood_log, None, BLOCK),
    "stripped_whisperwood_log_top": (gen_stripped_whisperwood_log_top, None, BLOCK),
    "whisperwood_planks": (gen_whisperwood_planks, None, BLOCK),
    "whisperwood_sapling": (gen_whisperwood_sapling, None, BLOCK),
    "whisperwood_door_bottom": (gen_whisperwood_door_bottom, None, BLOCK),
    "whisperwood_door_top": (gen_whisperwood_door_top, None, BLOCK),
    "whisperwood_trapdoor": (gen_whisperwood_trapdoor, None, BLOCK),
    "lantern_leaves": (gen_lantern_leaves, "block/oak_leaves", BLOCK),
    "hush_moss": (gen_hush_moss, None, BLOCK),
    "hush_grass": (gen_hush_grass, None, BLOCK),
    "echo_lantern": (gen_echo_lantern, "block/lantern", BLOCK),
    "echo_core": (gen_echo_core, "block/sculk", BLOCK),
    "hush_portal": (gen_hush_portal, "block/nether_portal", BLOCK),
    "raw_resonite": (gen_raw_resonite, None, ITEM),
    "resonite_ingot": (gen_resonite_ingot, None, ITEM),
    "resonite_sword": (gen_resonite_sword, None, ITEM),
    "resonite_pickaxe": (gen_resonite_pickaxe, None, ITEM),
    "resonite_axe": (gen_resonite_axe, None, ITEM),
    "resonite_shovel": (gen_resonite_shovel, None, ITEM),
    "resonite_hoe": (gen_resonite_hoe, None, ITEM),
    "resonant_heart": (gen_resonant_heart, None, ITEM),
    "echo_blade": (gen_echo_blade, None, ITEM),
    # the tools of the deep (2026-09-22)
    "tuning_fork": (gen_tuning_fork, None, ITEM),
    **{f"echo_compass_{i:02d}": (_compass_gen(i), None, ITEM) for i in range(32)},
    "cloak_of_silence": (gen_cloak_of_silence, None, ITEM),
    "resonance_bow": (gen_resonance_bow, None, ITEM),
    "recall_chime": (gen_recall_chime, None, ITEM),
    "whisperfruit": (gen_whisperfruit, None, ITEM),
    "hanging_whisperfruit_stage0": (gen_hanging_whisperfruit_stage0, None, BLOCK),
    "hanging_whisperfruit_stage1": (gen_hanging_whisperfruit_stage1, None, BLOCK),
    "hanging_whisperfruit_stage2": (gen_hanging_whisperfruit_stage2, None, BLOCK),
    "echo_heart_side": (gen_echo_heart_side, None, BLOCK),
    "echo_heart_top": (gen_echo_heart_top, None, BLOCK),
    # the lighthouse lamp (2026-09-22)
    "hush_lighthouse_lamp_lens": (gen_hush_lighthouse_lamp_lens, None, BLOCK),
    "hush_lighthouse_lamp_side": (gen_hush_lighthouse_lamp_side, None, BLOCK),
    "hush_lighthouse_lamp_top": (gen_hush_lighthouse_lamp_top, None, BLOCK),
    "hushling": (gen_hushling, None, ENTITY),
    "echo_wraith": (gen_echo_wraith, None, ENTITY),
    "echo_wraith_charging": (gen_echo_wraith_charging, None, ENTITY),
    "silent_warden": (gen_silent_warden, None, ENTITY),
}
KIND_DIR = {BLOCK: "block", ITEM: "item", ENTITY: "entity"}

VANILLA_REFS = ["deepslate", "polished_deepslate", "deepslate_bricks", "cracked_deepslate_bricks",
                "chiseled_deepslate", "sculk", "deepslate_diamond_ore", "iron_block",
                "amethyst_block", "amethyst_cluster", "dandelion", "oak_log", "oak_log_top",
                "stripped_oak_log", "stripped_oak_log_top", "oak_planks", "oak_sapling",
                "oak_door_bottom", "oak_door_top", "oak_trapdoor",
                "oak_leaves", "moss_block", "short_grass", "lantern", "nether_portal"]
VANILLA_ITEM_REFS = ["raw_iron", "iron_ingot", "diamond_sword", "diamond_pickaxe", "diamond_axe",
                     "diamond_shovel", "diamond_hoe", "heart_of_the_sea", "netherite_sword",
                     "compass_00", "leather_chestplate", "bow", "bell", "glow_berries"]
VANILLA_ENTITY_REFS = ["entity/endermite", "entity/illager/vex", "entity/illager/vex_charging",
                       "entity/warden/warden"]


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
    meta = None
    if mcmeta_src is not None:
        meta = (TEX_ROOT / f"{mcmeta_src}.png.mcmeta").read_bytes()
    return png_bytes(arr), meta, kind


def preview(names: list[str], outpath: Path) -> None:
    scale = 6
    cell = 16 * scale
    tile_scale = 3
    cols = 5
    pitch_x = cell + 12
    fg = (230, 230, 230, 255)
    sections: list[tuple[str, list[tuple[str, Image.Image]]]] = []

    def img_of(name: str) -> Image.Image:
        data, _, _ = render(name)
        return Image.open(io.BytesIO(data)).convert("RGBA")

    blocks = [(n, img_of(n)) for n in names if TEXTURES[n][2] == BLOCK]
    items = [(n, img_of(n)) for n in names if TEXTURES[n][2] == ITEM]
    entities = [(n, img_of(n)) for n in names if TEXTURES[n][2] == ENTITY]
    sections.append(("hush blocks", [(n, im) for n, im in blocks if im.size[1] == 16]))
    sections.append(("hush animated strips (frames left to right)", [(n, im) for n, im in blocks if im.size[1] > 16]))
    sections.append(("hush items", items))
    sections.append(("hush entities", entities))
    refs = [(r, Image.open(TEX_DIR / f"{r}.png").convert("RGBA")) for r in VANILLA_REFS if (TEX_DIR / f"{r}.png").exists()]
    sections.append(("vanilla block references", [(n, im) for n, im in refs if im.size[1] == 16]))
    sections.append(("vanilla strips", [(n, im) for n, im in refs if im.size[1] > 16]))
    sections.append(("vanilla item references", [(r, Image.open(ITEM_DIR / f"{r}.png").convert("RGBA"))
                                                 for r in VANILLA_ITEM_REFS if (ITEM_DIR / f"{r}.png").exists()]))
    sections.append(("vanilla entity references", [(r, Image.open(TEX_ROOT / f"{r}.png").convert("RGBA"))
                                                   for r in VANILLA_ENTITY_REFS if (TEX_ROOT / f"{r}.png").exists()]))

    # Lay out: every section is a grid of cells whose size depends on kind.
    layout: list[tuple[int, int, Image.Image, str]] = []
    headers: list[tuple[int, str]] = []
    y = 8
    width = cols * pitch_x + 12
    for title, entries in sections:
        if not entries:
            continue
        headers.append((y, title))
        y += 16
        x = 12
        row_h = 0
        for name, im in entries:
            w, h = im.size
            if w == 16 and h == 16:
                block = Image.new("RGBA", (cell, cell + 2 + 32 * tile_scale), (40, 40, 40, 255))
                block.paste(im.resize((cell, cell), Image.NEAREST), (0, 0))
                tiled = Image.new("RGBA", (32, 32))
                for tx in (0, 16):
                    for ty in (0, 16):
                        tiled.paste(im, (tx, ty))
                block.paste(tiled.resize((32 * tile_scale, 32 * tile_scale), Image.NEAREST), (0, cell + 2))
                shown = block
            elif w == 16 and h % 16 == 0:
                frames = min(h // 16, 8)
                shown = Image.new("RGBA", (frames * (cell // 2 + 2), cell // 2), (40, 40, 40, 255))
                for f in range(frames):
                    shown.paste(im.crop((0, f * 16, 16, f * 16 + 16)).resize((cell // 2, cell // 2), Image.NEAREST),
                                (f * (cell // 2 + 2), 0))
            else:
                s = max(1, min(4, 256 // max(w, h)))
                shown = im.resize((w * s, h * s), Image.NEAREST)
            if x + shown.size[0] > width - 12 and x > 12:
                x = 12
                y += row_h + 18
                row_h = 0
            layout.append((x, y + 12, shown, name))
            x += shown.size[0] + 12
            row_h = max(row_h, shown.size[1])
        y += row_h + 26
    sheet = Image.new("RGBA", (max(width, 900), y + 8), (40, 40, 40, 255))
    draw = ImageDraw.Draw(sheet)
    for hy, title in headers:
        draw.text((12, hy), title, fill=(255, 200, 120, 255))
    for x, yy, im, name in layout:
        sheet.paste(im, (x, yy), im)
        draw.text((x, yy - 11), name, fill=fg)
    sheet.save(outpath)
    print(f"preview -> {outpath}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated texture names")
    ap.add_argument("--outdir", type=Path, default=TEX_ROOT,
                    help="textures root to write under (block/, item/, entity/ subdirs)")
    ap.add_argument("--preview", nargs="?", const="hush_textures_preview.png", metavar="PNG",
                    help="write a contact sheet (default: ./hush_textures_preview.png) instead of the textures")
    ap.add_argument("--check", action="store_true",
                    help="regenerate in memory and byte-compare against --outdir; exit 1 on drift")
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
                outdir.mkdir(parents=True, exist_ok=True)
                path.write_bytes(blob)
                print(f"wrote    {rel(path)} ({len(blob)} bytes)")
    if args.check:
        print("check: " + ("clean" if drift == 0 else f"{drift} file(s) differ"))
        return 1 if drift else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
