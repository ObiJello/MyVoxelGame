#!/usr/bin/env python3
# tools/gen_hush_entity_textures.py
#
# Deterministic in-house art for the deep-Hush creatures and the Choir Hall
# (docs/the-hush.md): the five creature sheets and their glow / state
# variants, the two puzzle blocks and the Choir Mother's heart. Nothing here
# is derived from MC or mod art — every pixel is painted from the Hush
# palette (the same cyan ramp and resonite violet gen_hush_textures.py uses),
# seeded value noise and hand-placed features.
#
# Entity sheets are painted CUBE BY CUBE from tools/hush_creature_meshes.py,
# the same module gen_entity_models.py builds the meshes from: each cube's UV
# box is filled per face with its material, then the face's feature (eyes, a
# core, an eyespot) is drawn on top. A mesh edit therefore re-flows the art
# with it and the two can never disagree.
#
#   python3 tools/gen_hush_entity_textures.py            # write every PNG
#   python3 tools/gen_hush_entity_textures.py --check    # byte-compare, exit 1 on drift
#   python3 tools/gen_hush_entity_textures.py --preview out.png   # contact sheet (x4)
#   python3 tools/gen_hush_entity_textures.py --outdir /tmp/x     # mirror tree elsewhere
#
# Entity textures (assets/textures/entity/hush/):
#   lumen_moth, lumen_moth_glow        — fur, feathered antennae, eyespot wings;
#                                        the glow sheet is the abdomen, eyes and
#                                        eyespots only (drawn as an eyes layer)
#   crystal_golem, crystal_golem_angry — hushstone body, cyan crystal; the angry
#                                        sheet whitens every crystal and turns
#                                        the eyes violet-white
#   crystal_golem_glow                 — crystals, eyes and core only
#   hush_leviathan, hush_leviathan_glow — mottled hide, grooved belly, a light
#                                        row along each flank, crystal nodes
#   echo_mimic, echo_mimic_shimmer     — a teal after-image of a player; the
#                                        shimmer sheet is the same figure at a
#                                        fifth of the alpha with bright edges
#   choir_mother, choir_mother_glow    — sculk flesh, lace veil, glowing heart,
#                                        horns and tendril tips
#   the_unsung, the_unsung_glow,       — indigo robes and sculk plates, a pale
#   the_unsung_aura                      blank mask split by a crack of light,
#                                        the hollow crystal throat, the four
#                                        voice shards; the aura is the whole
#                                        silhouette as a tintable white film
# Block textures (assets/textures/block/): resonant_chime, resonant_chime_on,
#   choir_altar_top, choir_altar_side, choir_altar_bottom.
# Item textures (assets/textures/item/): choir_heart.
#
# Seeding: default_rng([0x43484F52 ("CHOR"), crc32(name)]) per texture, so
# adding or reordering textures never reshuffles the others.

from __future__ import annotations

import argparse
import io
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hush_creature_meshes as hcm  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
TEX_ROOT = REPO_ROOT / "assets" / "textures"
SEED_TAG = 0x43484F52  # "CHOR"

# The Hush palette (gen_hush_textures.py / docs/the-hush.md).
CYAN_DEEP, CYAN_DARK, CYAN_MID, CYAN, CYAN_PALE, CYAN_WHITE = (
    "#0E3A3F", "#145C5C", "#178C86", "#2BD4C0", "#7FEDE0", "#B8F7F0")
VIOLET_DARK, VIOLET_MID, VIOLET, VIOLET_PALE, VIOLET_WHITE = (
    "#3D3358", "#7E6BA8", "#B39DDB", "#CDBDEC", "#E6DAFF")
STONE_BLACK, STONE_DARK, STONE_MID, STONE_LIGHT = (
    "#15181F", "#232833", "#343B49", "#4A5263")
SCULK_BLACK, SCULK_DARK, SCULK_MID = ("#051418", "#0A262C", "#10383F")


# ── helpers ─────────────────────────────────────────────────────────────────
def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng([SEED_TAG, zlib.crc32(name.encode("utf-8"))])


def rgb(hexstr: str, a: int = 255) -> np.ndarray:
    h = hexstr.lstrip("#")
    return np.array([int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a], dtype=np.int32)


def value_noise(rng: np.random.Generator, h: int, w: int, cell: int) -> np.ndarray:
    """Bilinear value noise in [0, 1] on a lattice of `cell` pixels."""
    gh, gw = h // cell + 2, w // cell + 2
    lat = rng.random((gh, gw))
    ys = np.arange(h) / cell
    xs = np.arange(w) / cell
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    fy = (ys - y0)[:, None]
    fx = (xs - x0)[None, :]
    fy = fy * fy * (3 - 2 * fy)
    fx = fx * fx * (3 - 2 * fx)
    a = lat[np.ix_(y0, x0)]
    b = lat[np.ix_(y0, x0 + 1)]
    c = lat[np.ix_(y0 + 1, x0)]
    d = lat[np.ix_(y0 + 1, x0 + 1)]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def ramp(field: np.ndarray, colours: list[str], alpha: int = 255) -> np.ndarray:
    """Posterize a [0,1] field into len(colours) flat shades (rank-quantized so
    the proportions are even whatever the field's range)."""
    h, w = field.shape
    out = np.zeros((h, w, 4), dtype=np.int32)
    if field.size == 0:
        return out
    order = np.argsort(field.ravel(), kind="stable")
    idx = np.empty(field.size, dtype=np.int32)
    idx[order] = (np.arange(field.size) * len(colours)) // field.size
    idx = idx.reshape(h, w)
    for i, col in enumerate(colours):
        out[idx == i] = rgb(col, alpha)
    return out


def fill(h: int, w: int, col: str, a: int = 255) -> np.ndarray:
    out = np.zeros((h, w, 4), dtype=np.int32)
    out[:, :] = rgb(col, a)
    return out


def put(img: np.ndarray, y: int, x: int, col: str, a: int = 255) -> None:
    if 0 <= y < img.shape[0] and 0 <= x < img.shape[1]:
        img[y, x] = rgb(col, a)


def to_png(arr: np.ndarray) -> bytes:
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=False, compress_level=9)
    return buf.getvalue()


# ── materials ───────────────────────────────────────────────────────────────
# A material paints one face (h x w) of a cube. `face` lets a material shade
# its top/bottom differently; `rng` is the sheet's own stream.

def mat_moth_fur(face, h, w, rng):
    f = value_noise(rng, h, w, 2) + 0.35 * rng.random((h, w))
    shades = [VIOLET_DARK, VIOLET_MID, VIOLET_PALE] if face == "top" else \
             [VIOLET_DARK, VIOLET_DARK, VIOLET_MID, VIOLET_PALE]
    return ramp(f, shades)


def mat_moth_antenna(face, h, w, rng):
    out = ramp(rng.random((h, w)), [VIOLET_PALE, VIOLET_WHITE])
    # Feathered: every other row is a barb; the gaps are clear.
    for y in range(h):
        if y % 2 == 1:
            out[y, :, 3] = 0
    return out


def mat_moth_glow(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [CYAN_PALE, CYAN_WHITE, "#FFFFFF"])
    for y in range(h):
        for x in range(w):
            if (x + y) % 3 == 0:
                out[y, x] = rgb(CYAN)
    return out


def _wing(h, w, rng, base, vein, edge):
    out = ramp(value_noise(rng, h, w, 3) + 0.2 * rng.random((h, w)), base)
    for x in range(0, w, 3):
        out[:, x] = rgb(vein)
    # Scalloped outer edge: the far rows lose every other pixel.
    if h > 2:
        for x in range(w):
            if x % 2 == 0:
                out[h - 1, x, 3] = 0
        out[h - 2, :] = rgb(edge)
    return out


def mat_moth_wing(face, h, w, rng):
    return _wing(h, w, rng, [VIOLET_MID, VIOLET, VIOLET_PALE], VIOLET_DARK, VIOLET_WHITE)


def mat_moth_hindwing(face, h, w, rng):
    return _wing(h, w, rng, [CYAN_DARK, CYAN_MID, VIOLET_MID], CYAN_DEEP, CYAN_PALE)


def mat_golem_stone(face, h, w, rng):
    f = value_noise(rng, h, w, 3) + 0.4 * rng.random((h, w))
    out = ramp(f, [STONE_BLACK, STONE_DARK, STONE_MID, STONE_LIGHT] if face != "bottom"
               else [STONE_BLACK, STONE_DARK, STONE_DARK])
    # Crystal veins: a wandering cyan line down the long faces.
    if h >= 6 and w >= 3:
        x = int(rng.integers(0, w))
        for y in range(h):
            if rng.random() < 0.75:
                out[y, x] = rgb(CYAN_MID if y % 4 else CYAN)
            x = int(np.clip(x + rng.integers(-1, 2), 0, w - 1))
    return out


def mat_crystal(face, h, w, rng):
    # Faceted: diagonal bands of the cyan ramp, brightest toward the tip.
    out = np.zeros((h, w, 4), dtype=np.int32)
    shades = [CYAN_DARK, CYAN_MID, CYAN, CYAN_PALE]
    for y in range(h):
        for x in range(w):
            k = ((x + (h - y)) // 2 + (1 if face == "top" else 0)) % len(shades)
            out[y, x] = rgb(shades[k])
    if h > 1 and w > 1:
        out[0, :] = rgb(CYAN_WHITE)
    return out


def mat_lev_hide(face, h, w, rng):
    f = value_noise(rng, h, w, 4) + 0.5 * value_noise(rng, h, w, 2)
    out = ramp(f, ["#0B1E2A", "#12303F", "#1B4152", "#23566A"])
    # Pale mottling.
    spots = rng.random((h, w)) < 0.035
    out[spots] = rgb(CYAN_PALE)
    return out


def mat_lev_fin(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 3), ["#16384A", "#1E5064", "#2B6F84"])
    for x in range(0, w, 2):
        out[:, x] = rgb("#0F2733")
    return out


def mat_lev_glow(face, h, w, rng):
    return ramp(rng.random((h, w)), [CYAN, CYAN_PALE, CYAN_WHITE])


def _mimic_base(face, h, w, rng):
    # A player's after-image in teal monochrome: darker cloth bands low,
    # pale skin high — the zones a skin has, no one player's colours.
    f = value_noise(rng, h, w, 2) + 0.25 * rng.random((h, w))
    return ramp(f, ["#0C2D33", "#134149", "#1C5A63", "#2A7680"])


def mat_mimic_skin(face, h, w, rng):
    out = _mimic_base(face, h, w, rng)
    # A bright rim on the silhouette edges — what shimmers.
    if h > 2 and w > 2:
        out[0, :] = rgb(CYAN_PALE)
        out[:, 0] = rgb(CYAN)
        out[:, w - 1] = rgb(CYAN)
    return out


def mat_mimic_shell(face, h, w, rng):
    out = np.zeros((h, w, 4), dtype=np.int32)
    sparks = rng.random((h, w)) < 0.12
    out[sparks] = rgb(CYAN_WHITE, 200)
    return out


def mat_mimic_wisp(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [CYAN_MID, CYAN, CYAN_PALE])
    for y in range(h):
        # Frays toward the end: lower rows keep fewer pixels.
        keep = rng.random(w) < (1.0 - y / max(h, 1) * 0.8)
        out[y, ~keep, 3] = 0
    return out


def mat_sculk_flesh(face, h, w, rng):
    f = value_noise(rng, h, w, 2) + 0.3 * rng.random((h, w))
    out = ramp(f, [SCULK_BLACK, SCULK_DARK, SCULK_DARK, SCULK_MID])
    specks = rng.random((h, w)) < 0.05
    out[specks] = rgb(CYAN)
    return out


def mat_sculk_tendril(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [SCULK_BLACK, SCULK_DARK, SCULK_MID])
    if h >= 2:
        out[h - 1, :] = rgb(CYAN_MID)
    return out


def mat_sculk_tendril_glow(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [CYAN_MID, CYAN, CYAN_PALE])
    # Ragged horn edges: a sparse clear fringe on the outer column.
    if w > 2:
        for y in range(h):
            if y % 3 == 0:
                out[y, w - 1, 3] = 0
    return out


def mat_mother_veil(face, h, w, rng):
    # Lace: a diamond lattice of pale thread over clear cells (binary alpha —
    # the body batch is a cutout, not a blend).
    out = np.zeros((h, w, 4), dtype=np.int32)
    for y in range(h):
        for x in range(w):
            if (x + y) % 3 == 0 or (x - y) % 3 == 0:
                out[y, x] = rgb(VIOLET_PALE if (x + y) % 2 else CYAN_PALE)
    if h > 0:
        out[h - 1, :] = rgb(CYAN)
    return out


# ── The Unsung (Aurelith's boss) ───────────────────────────────────────────
# The Undersong in a conductor's shape: indigo-black robes and layered sculk
# plates, a pale blank mask split by a crack of light, a hollow crystal
# throat, and four shards in the voices' colours (Game::Aurelith::
# VoiceColour). Its light is violet more than cyan: stolen song.
ROBE_BLACK, ROBE_DARK, ROBE_MID = ("#07080F", "#0E1124", "#171C3A")
MASK_DARK, MASK_MID, MASK_LIGHT = ("#9FA5A8", "#C3C8C8", "#DDE1DD")
VOICE_HEX = {"soprano": "#DFFFFF", "alto": "#5FF3FF", "tenor": "#B77CFF", "bass": "#6F7BFF"}


def mat_unsung_robe(face, h, w, rng):
    # Heavy cloth: vertical folds (a slow sine across the width) under noise.
    f = value_noise(rng, h, w, 3) * 0.6
    f += 0.4 * (0.5 + 0.5 * np.sin(np.arange(w) * 1.3))[None, :]
    return ramp(f, [ROBE_BLACK, ROBE_BLACK, ROBE_DARK, ROBE_MID])


def mat_unsung_robe_plate(face, h, w, rng):
    # Overlapping plates of sculk, row upon row, each lit along its lower
    # edge; the hem is ragged (clear teeth) so the robe dissolves downward.
    out = ramp(value_noise(rng, h, w, 2), [ROBE_BLACK, SCULK_BLACK, ROBE_DARK, SCULK_DARK])
    for y in range(3, h, 4):
        out[y, :] = rgb(ROBE_MID)
        edge = rng.random(w) < 0.08
        out[y, edge] = rgb(VIOLET_MID)
    for x in range(w):
        teeth = int(rng.integers(0, 4))
        if teeth:
            out[h - teeth:, x, 3] = 0
    return out


def mat_unsung_mantle(face, h, w, rng):
    # The shorter outer layer: broader plates, a violet seam on each edge.
    out = ramp(value_noise(rng, h, w, 2) + 0.3 * rng.random((h, w)),
               [SCULK_BLACK, SCULK_DARK, SCULK_DARK, SCULK_MID])
    for y in range(2, h, 3):
        out[y, :] = rgb(SCULK_BLACK)
    if h > 0:
        out[h - 1, :] = rgb(VIOLET_DARK)
    for x in range(0, w, 3):
        if h > 1 and rng.random() < 0.5:
            out[h - 1, x, 3] = 0
    return out


def mat_unsung_plate(face, h, w, rng):
    # Armour-like horizontal bands of sculk, each band's top edge a shade
    # lighter, sparse violet and cyan motes caught in the seams.
    out = ramp(value_noise(rng, h, w, 2), [ROBE_BLACK, SCULK_BLACK, SCULK_DARK, SCULK_DARK])
    for y in range(0, h, 3):
        out[y, :] = rgb(SCULK_MID)
    specks = rng.random((h, w)) < 0.03
    out[specks] = rgb(VIOLET_MID)
    specks = rng.random((h, w)) < 0.01
    out[specks] = rgb(CYAN_MID)
    return out


def mat_unsung_sculk(face, h, w, rng):
    f = value_noise(rng, h, w, 2) + 0.3 * rng.random((h, w))
    out = ramp(f, [SCULK_BLACK, SCULK_BLACK, SCULK_DARK, SCULK_MID])
    specks = rng.random((h, w)) < 0.03
    out[specks] = rgb(VIOLET_MID)
    return out


def mat_unsung_mask(face, h, w, rng):
    # Pale smooth choirstone, the statues' blank face: almost no texture.
    f = value_noise(rng, h, w, 3) + 0.15 * rng.random((h, w))
    return ramp(f, [MASK_DARK, MASK_MID, MASK_MID, MASK_LIGHT])


def mat_unsung_hood(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [SCULK_BLACK, ROBE_BLACK, SCULK_DARK])
    # A frayed outer edge.
    for y in range(h):
        if rng.random() < 0.35 and w > 1:
            out[y, w - 1, 3] = 0
    if h > 0:
        out[h - 1, ::2, 3] = 0
    return out


def mat_unsung_throat(face, h, w, rng):
    # Hollow crystal, violet within and cyan at the rims — stolen light.
    out = ramp(value_noise(rng, h, w, 1), [VIOLET_MID, VIOLET, VIOLET_PALE, CYAN_PALE])
    if h > 1:
        out[0, :] = rgb(CYAN_WHITE)
    return out


def mat_unsung_tendril_glow(face, h, w, rng):
    out = ramp(value_noise(rng, h, w, 2), [VIOLET_MID, VIOLET, CYAN_PALE])
    if w > 2:
        for y in range(h):
            if y % 3 == 1:
                out[y, w - 1, 3] = 0
    return out


def mat_unsung_baton(face, h, w, rng):
    # Dark crystal: near-black violet with a thin bright facet line.
    out = ramp(value_noise(rng, h, w, 2), [ROBE_BLACK, VIOLET_DARK, VIOLET_DARK])
    if w >= 1:
        out[:, 0] = rgb(VIOLET_MID)
    return out


def _mat_shard(voice):
    def paint(face, h, w, rng):
        base = VOICE_HEX[voice]
        out = ramp(value_noise(rng, h, w, 1), [base, base, CYAN_WHITE])
        # A darker core stripe down the middle facet reads as a crystal edge.
        if w >= 2:
            out[:, w // 2] = (rgb(base) * 0.7).astype(np.int32)
            out[:, w // 2, 3] = 255
        return out
    return paint


MATERIALS = {
    "moth_fur": mat_moth_fur, "moth_antenna": mat_moth_antenna,
    "moth_glow": mat_moth_glow, "moth_wing": mat_moth_wing,
    "moth_hindwing": mat_moth_hindwing,
    "golem_stone": mat_golem_stone, "crystal": mat_crystal,
    "lev_hide": mat_lev_hide, "lev_fin": mat_lev_fin, "lev_glow": mat_lev_glow,
    "mimic_skin": mat_mimic_skin, "mimic_shell": mat_mimic_shell,
    "mimic_wisp": mat_mimic_wisp,
    "sculk_flesh": mat_sculk_flesh, "sculk_tendril": mat_sculk_tendril,
    "sculk_tendril_glow": mat_sculk_tendril_glow, "mother_veil": mat_mother_veil,
    "unsung_robe": mat_unsung_robe, "unsung_robe_plate": mat_unsung_robe_plate,
    "unsung_mantle": mat_unsung_mantle, "unsung_plate": mat_unsung_plate,
    "unsung_sculk": mat_unsung_sculk, "unsung_mask": mat_unsung_mask,
    "unsung_hood": mat_unsung_hood, "unsung_throat": mat_unsung_throat,
    "unsung_tendril_glow": mat_unsung_tendril_glow, "unsung_baton": mat_unsung_baton,
    "shard_soprano": _mat_shard("soprano"), "shard_alto": _mat_shard("alto"),
    "shard_tenor": _mat_shard("tenor"), "shard_bass": _mat_shard("bass"),
}

# Materials that GLOW: kept on the glow sheets, cleared elsewhere.
GLOW_MATERIALS = {"moth_glow", "crystal", "lev_glow", "sculk_tendril_glow",
                  "unsung_throat", "unsung_tendril_glow", "shard_soprano", "shard_alto",
                  "shard_tenor", "shard_bass"}


# ── face features ───────────────────────────────────────────────────────────
# Drawn onto the face rectangle after the material. Each returns the pixels
# it lit (so the glow sheet can keep exactly those).

def feat_moth_eyes(img, h, w, rng):
    lit = [(1, 0), (1, w - 1)]
    for y, x in lit:
        put(img, y, x, CYAN_WHITE)
    return lit


def feat_wing_eye(img, h, w, rng):
    cy, cx = h // 2, w // 2 + 1
    lit = []
    for y in range(h):
        for x in range(w):
            d = (y - cy) ** 2 + (x - cx) ** 2
            if d <= 1:
                put(img, y, x, CYAN_WHITE)
                lit.append((y, x))
            elif d <= 4:
                put(img, y, x, CYAN)
                lit.append((y, x))
    return lit


def feat_golem_face(img, h, w, rng):
    # Heavy brow, two glowing slits, a crack of a mouth.
    img[0:2, :] = rgb(STONE_BLACK)
    lit = []
    for x in (1, 2, w - 3, w - 2):
        put(img, 3, x, CYAN_WHITE)
        lit.append((3, x))
    for x in range(2, w - 2):
        put(img, h - 3, x, STONE_BLACK)
    return lit


def feat_golem_core(img, h, w, rng):
    cy, cx = h // 2 - 1, w // 2
    lit = []
    for y in range(h):
        for x in range(w):
            d = abs(y - cy) + abs(x - cx)
            if d <= 1:
                put(img, y, x, CYAN_WHITE)
                lit.append((y, x))
            elif d <= 3:
                put(img, y, x, CYAN)
                lit.append((y, x))
    return lit


def feat_lev_belly(img, h, w, rng):
    # Ventral grooves: pale stripes running nose to tail.
    for x in range(w):
        col = "#6FA7B0" if x % 2 == 0 else "#9CC9CF"
        img[:, x] = rgb(col)
    return []


def feat_lev_lights(img, h, w, rng):
    # A row of photophores along the flank.
    y = h // 2 + 2
    lit = []
    for x in range(1, w - 1, 3):
        put(img, y, x, CYAN_WHITE)
        put(img, y - 1, x, CYAN)
        lit += [(y, x), (y - 1, x)]
    return lit


def feat_lev_eye(img, h, w, rng):
    y, x = h // 2 - 1, 2
    lit = [(y, x), (y, x + 1), (y + 1, x), (y + 1, x + 1)]
    for yy, xx in lit:
        put(img, yy, xx, CYAN_WHITE)
    return lit


def feat_lev_face(img, h, w, rng):
    for x in range(1, w - 1):
        put(img, h - 2, x, "#0A1820")
    return []


def feat_lev_mouth(img, h, w, rng):
    for x in range(w):
        put(img, h // 2 + 1, x, "#050C10")
    return []


def feat_mimic_face(img, h, w, rng):
    # Hollow eyes and no mouth — it has only ever seen your back.
    lit = []
    for x in (1, 2, w - 3, w - 2):
        put(img, 3, x, CYAN_WHITE)
        put(img, 4, x, "#000000")
        lit.append((3, x))
    return lit


def feat_mimic_chest(img, h, w, rng):
    cy, cx = h // 3, w // 2
    lit = []
    for y in range(h):
        for x in range(w):
            if abs(((y - cy) ** 2 + (x - cx) ** 2) ** 0.5 - 2.5) < 0.5:
                put(img, y, x, CYAN_PALE)
                lit.append((y, x))
    return lit


def feat_mother_face(img, h, w, rng):
    img[:, :] = rgb(SCULK_BLACK)
    lit = []
    for x in (1, w - 2):
        put(img, 3, x, CYAN_WHITE)
        lit.append((3, x))
    return lit


def feat_mother_heart(img, h, w, rng):
    cy, cx = 3, w // 2
    heart = [".XX.XX.", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."]
    lit = []
    for dy, row in enumerate(heart):
        for dx, ch in enumerate(row):
            if ch == "X":
                y, x = cy - 1 + dy, cx - 3 + dx
                put(img, y, x, CYAN_WHITE if dy < 2 else CYAN)
                lit.append((y, x))
    return lit


def feat_unsung_robe_notes(img, h, w, rng):
    # A few Stave glyphs on five faint lines down the front of the robe —
    # the notation of a song it learned and cannot finish. Lit, dimly.
    lit = []
    for y in range(2, h - 2, 3):
        for x in range(1, w - 1):
            if rng.random() < 0.12:
                put(img, y, x, VIOLET_MID)
                lit.append((y, x))
    return lit


def feat_unsung_chest(img, h, w, rng):
    # Cracks of light running up from the hem of the plates to the throat.
    lit = []
    cx = w // 2
    x = cx
    for y in range(h - 1, -1, -1):
        x = min(w - 2, max(1, x + int(rng.integers(-1, 2))))
        put(img, y, x, VIOLET_PALE if y > h // 2 else CYAN_PALE)
        lit.append((y, x))
    for y in range(0, 3):
        for xx in (cx - 1, cx, cx + 1):
            put(img, y, xx, VIOLET)
            lit.append((y, xx))
    return lit


def feat_unsung_face(img, h, w, rng):
    # Blank — no eyes, no mouth — split top to bottom by one jagged crack of
    # white light, brightest where a mouth would be.
    lit = []
    x = w // 2 - 1
    for y in range(h):
        if y in (2, 4):
            x += 1 if y == 2 else -1
        col = CYAN_WHITE if h // 2 <= y <= h // 2 + 1 else VIOLET_WHITE
        put(img, y, x, col)
        lit.append((y, x))
    # Hairline shadow either side of the crack.
    return lit


def feat_baton_tip(img, h, w, rng):
    lit = []
    for y in range(h):
        for x in range(w):
            put(img, y, x, VIOLET_WHITE)
            lit.append((y, x))
    return lit


FEATURES = {
    "moth_eyes": feat_moth_eyes, "wing_eye": feat_wing_eye,
    "golem_face": feat_golem_face, "golem_core": feat_golem_core,
    "lev_belly": feat_lev_belly, "lev_lights": feat_lev_lights,
    "lev_eye": feat_lev_eye, "lev_face": feat_lev_face, "lev_mouth": feat_lev_mouth,
    "mimic_face": feat_mimic_face, "mimic_chest": feat_mimic_chest,
    "mother_face": feat_mother_face, "mother_heart": feat_mother_heart,
    "unsung_robe_notes": feat_unsung_robe_notes, "unsung_chest": feat_unsung_chest,
    "unsung_face": feat_unsung_face, "baton_tip": feat_baton_tip,
}


# ── entity sheets ───────────────────────────────────────────────────────────
def paint_mesh(mesh: hcm.Mesh, name: str) -> tuple[np.ndarray, np.ndarray]:
    """(body sheet, glow mask) — the mask is True on every glowing pixel."""
    rng = rng_for(name)
    img = np.zeros((mesh.texh, mesh.texw, 4), dtype=np.int32)
    glow = np.zeros((mesh.texh, mesh.texw), dtype=bool)
    for part in mesh.parts:
        for cube in part.cubes:
            for face, (u0, v0, u1, v1) in hcm.face_rects(cube).items():
                h, w = v1 - v0, u1 - u0
                if h <= 0 or w <= 0:
                    continue
                tile = MATERIALS[cube.mat](face, h, w, rng)
                lit = []
                feat = cube.feats.get(face)
                if feat:
                    lit = FEATURES[feat](tile, h, w, rng)
                img[v0:v1, u0:u1] = tile
                if cube.mat in GLOW_MATERIALS:
                    glow[v0:v1, u0:u1] |= tile[:, :, 3] > 0
                for y, x in lit:
                    if 0 <= y < h and 0 <= x < w:
                        glow[v0 + y, u0 + x] = True
    return img, glow


def glow_sheet(img: np.ndarray, mask: np.ndarray) -> np.ndarray:
    out = np.zeros_like(img)
    out[mask] = img[mask]
    return out


def brighten(img: np.ndarray, mask: np.ndarray, toward: str, t: float) -> np.ndarray:
    out = img.copy()
    target = rgb(toward)[:3]
    sel = mask & (img[:, :, 3] > 0)
    out[sel, :3] = (img[sel, :3] * (1 - t) + target * t).astype(np.int32)
    return out


def entity_textures() -> dict[str, np.ndarray]:
    meshes = {m.slug: m for m in hcm.meshes()}
    out: dict[str, np.ndarray] = {}

    img, mask = paint_mesh(meshes["lumen_moth"], "lumen_moth")
    out["entity/hush/lumen_moth"] = img
    out["entity/hush/lumen_moth_glow"] = glow_sheet(img, mask)

    img, mask = paint_mesh(meshes["crystal_golem"], "crystal_golem")
    out["entity/hush/crystal_golem"] = img
    # Angry: every crystal and the eyes whiten toward violet-white — the
    # "angry state visible" read, a brighter golem, not a different one.
    out["entity/hush/crystal_golem_angry"] = brighten(img, mask, VIOLET_WHITE, 0.55)
    out["entity/hush/crystal_golem_glow"] = glow_sheet(img, mask)

    img, mask = paint_mesh(meshes["hush_leviathan"], "hush_leviathan")
    out["entity/hush/hush_leviathan"] = img
    out["entity/hush/hush_leviathan_glow"] = glow_sheet(img, mask)

    img, _ = paint_mesh(meshes["echo_mimic"], "echo_mimic")
    # Revealed: a near-solid figure (the body batch is blended).
    solid = img.copy()
    solid[:, :, 3] = np.where(img[:, :, 3] > 0, np.minimum(img[:, :, 3], 235), 0)
    out["entity/hush/echo_mimic"] = solid
    # Shimmer: the same figure at a fifth of the alpha, rims kept brighter.
    faint = img.copy()
    bright = img[:, :, :3].sum(axis=2) > 330
    faint[:, :, 3] = np.where(img[:, :, 3] > 0, np.where(bright, 110, 48), 0)
    out["entity/hush/echo_mimic_shimmer"] = faint

    img, mask = paint_mesh(meshes["choir_mother"], "choir_mother")
    out["entity/hush/choir_mother"] = img
    out["entity/hush/choir_mother_glow"] = glow_sheet(img, mask)

    img, mask = paint_mesh(meshes["the_unsung"], "the_unsung")
    out["entity/hush/the_unsung"] = img
    out["entity/hush/the_unsung_glow"] = glow_sheet(img, mask)
    # The aura: the whole silhouette as a soft white film, tinted by the
    # render module (a stolen voice's colour while shielded, white while
    # staggered) and drawn blended over the body.
    aura = np.zeros_like(img)
    opaque = img[:, :, 3] > 0
    aura[opaque, :3] = 255
    aura[opaque, 3] = np.where(mask[opaque], 200, 90)
    out["entity/hush/the_unsung_aura"] = aura
    return out


# ── blocks and the item ─────────────────────────────────────────────────────
def hushstone_16(name: str) -> np.ndarray:
    rng = rng_for(name)
    f = value_noise(rng, 16, 16, 4) + 0.5 * rng.random((16, 16))
    return ramp(f, [STONE_BLACK, STONE_DARK, STONE_MID, STONE_MID, STONE_LIGHT])


def chime(name: str, lit: bool) -> np.ndarray:
    # A hushstone frame holding five tuned crystal tubes of falling length;
    # lit, the tubes blaze white-cyan and the frame's inlay glows.
    img = hushstone_16("resonant_chime_frame")
    img[0:2, :] = rgb(STONE_LIGHT)
    img[14:16, :] = rgb(STONE_DARK)
    tube_on = [CYAN_PALE, CYAN_WHITE, "#FFFFFF"]
    tube_off = [CYAN_DEEP, CYAN_DARK, CYAN_MID]
    shades = tube_on if lit else tube_off
    for i, length in enumerate((11, 10, 9, 8, 7)):
        x = 2 + i * 3
        for y in range(2, 2 + length):
            img[y, x] = rgb(shades[1])
            img[y, x + 1] = rgb(shades[0] if y % 3 else shades[2])
        img[2 + length, x] = rgb(shades[2])
    if lit:
        for x in range(1, 15):
            if x % 2 == 0:
                img[1, x] = rgb(CYAN)
    return img


def altar_top() -> np.ndarray:
    # A carved ring of six chime marks round a socket that takes a shard.
    img = hushstone_16("choir_altar_top")
    c = 7.5
    for y in range(16):
        for x in range(16):
            r = ((y - c) ** 2 + (x - c) ** 2) ** 0.5
            if 5.6 <= r <= 6.6:
                img[y, x] = rgb(CYAN_DARK)
            if r <= 1.6:
                img[y, x] = rgb(CYAN_WHITE if r < 0.8 else CYAN)
    for k in range(6):
        a = np.radians(30 + 60 * k)
        y, x = int(round(c + 6.1 * np.sin(a))), int(round(c + 6.1 * np.cos(a)))
        put(img, y, x, CYAN_PALE)
    img[0, :] = img[15, :] = rgb(STONE_LIGHT)
    img[:, 0] = img[:, 15] = rgb(STONE_LIGHT)
    return img


def altar_side() -> np.ndarray:
    # Hushstone courses with a band of glowing notation — the choir's score.
    img = hushstone_16("choir_altar_side")
    for y in (0, 5, 10, 15):
        img[y, :] = rgb(STONE_BLACK)
    img[0, :] = rgb(STONE_LIGHT)
    rng = rng_for("choir_altar_runes")
    for x in range(1, 15):
        img[7, x] = rgb(CYAN_DARK)
        if rng.random() < 0.55:
            img[6 + int(rng.integers(0, 3)), x] = rgb(CYAN if rng.random() < 0.7 else CYAN_WHITE)
    return img


def altar_bottom() -> np.ndarray:
    return hushstone_16("choir_altar_bottom")


def choir_heart() -> np.ndarray:
    # A heart of sculk bound in crystal: dark flesh, a bright crystal core,
    # a cyan rim and three tendrils trailing below.
    shape = [
        "................",
        "...XXX....XXX...",
        "..XoooX..XoooX..",
        ".XoooooXXoooooX.",
        ".XoooccooccoooX.",
        ".XooocWWWWcoooX.",
        "..XooocWWcoooX..",
        "..XooooccooooX..",
        "...XooooooooX...",
        "....XooooooX....",
        ".....XooooX.....",
        "......XooX......",
        ".......XX.......",
        "......t..t......",
        ".....t....t.....",
        "................",
    ]
    rng = rng_for("choir_heart")
    img = np.zeros((16, 16, 4), dtype=np.int32)
    flesh = [SCULK_BLACK, SCULK_DARK, SCULK_MID]
    for y, row in enumerate(shape):
        for x, ch in enumerate(row):
            if ch == "X":
                img[y, x] = rgb(CYAN)
            elif ch == "o":
                img[y, x] = rgb(flesh[int(rng.integers(0, 3))])
            elif ch == "c":
                img[y, x] = rgb(CYAN_PALE)
            elif ch == "W":
                img[y, x] = rgb(CYAN_WHITE)
            elif ch == "t":
                img[y, x] = rgb(CYAN_MID)
    return img


def other_textures() -> dict[str, np.ndarray]:
    return {
        "block/resonant_chime": chime("resonant_chime", False),
        "block/resonant_chime_on": chime("resonant_chime_on", True),
        "block/choir_altar_top": altar_top(),
        "block/choir_altar_side": altar_side(),
        "block/choir_altar_bottom": altar_bottom(),
        "item/choir_heart": choir_heart(),
    }


def all_textures() -> dict[str, np.ndarray]:
    out = entity_textures()
    out.update(other_textures())
    return out


def preview(textures: dict[str, np.ndarray], path: Path) -> None:
    """Every texture at 4x on a mid-grey checker, one row each."""
    scale, pad = 4, 6
    rows = [(k, v) for k, v in textures.items()]
    width = max(v.shape[1] for _, v in rows) * scale + 2 * pad
    height = sum(v.shape[0] * scale + pad for _, v in rows) + pad
    sheet = np.zeros((height, width, 4), dtype=np.int32)
    yy, xx = np.mgrid[0:height, 0:width]
    checker = ((yy // 8 + xx // 8) % 2).astype(np.int32)
    sheet[:, :, :3] = (70 + 25 * checker)[:, :, None]
    sheet[:, :, 3] = 255
    y = pad
    for _, arr in rows:
        big = np.kron(arr, np.ones((scale, scale, 1), dtype=np.int32))
        h, w = big.shape[:2]
        a = big[:, :, 3:4] / 255.0
        region = sheet[y:y + h, pad:pad + w, :3]
        sheet[y:y + h, pad:pad + w, :3] = (big[:, :, :3] * a + region * (1 - a)).astype(np.int32)
        y += h + pad
    path.write_bytes(to_png(sheet))
    print(f"preview -> {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Hush creature / Choir Hall textures")
    ap.add_argument("--check", action="store_true", help="byte-compare, exit 1 on drift")
    ap.add_argument("--preview", type=Path, help="write a contact sheet here")
    ap.add_argument("--outdir", type=Path, help="write under this root instead of assets/textures")
    args = ap.parse_args()

    textures = all_textures()
    root = args.outdir or TEX_ROOT
    drift = []
    for rel, arr in textures.items():
        data = to_png(arr)
        path = root / f"{rel}.png"
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                drift.append(rel)
            continue
        if args.preview:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    if args.preview:
        preview(textures, args.preview)
        return 0
    if args.check:
        if drift:
            print("drift: " + ", ".join(drift))
            return 1
        print(f"{len(textures)} textures up to date")
        return 0
    print(f"wrote {len(textures)} textures under {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
