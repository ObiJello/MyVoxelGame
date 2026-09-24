#!/usr/bin/env python3
# tools/gen_aurelith_textures.py
#
# Deterministic procedural textures for Aurelith, the Lantern City (The Hush;
# docs/the-hush.md, docs/hush-lore.md). Same house style as
# tools/gen_hush_textures.py, whose noise / posterize / palette helpers this
# script imports: block art is seeded value noise POSTERIZED into a handful of
# flat shades, never smooth gradients; derived art (the grate) is a per-pixel
# recolour of the vanilla sprite that keeps its alpha. Animated sprites are
# vertical strips with an .mcmeta written from ANIMATIONS below. Re-running
# always produces identical bytes; `--check` proves it against what is on disk.
#
#   python3 tools/gen_aurelith_textures.py              # write every PNG (+ .mcmeta)
#   python3 tools/gen_aurelith_textures.py --only choirstone,stave_stone
#   python3 tools/gen_aurelith_textures.py --preview sheet.png   # contact sheet
#   python3 tools/gen_aurelith_textures.py --check      # byte-compare, exit 1 on drift
#
# Block textures (assets/textures/block/):
#   The choirstone family — pale, cool ivory-grey stone "sung smooth":
#     choirstone (faint teal veining), polished_choirstone (calm face, 1-px
#     bevel), choirstone_bricks (two 8-px ashlar courses, fine dark mortar),
#     cracked_choirstone_bricks (+ cracks, one with a sculk-teal seam),
#     chiseled_choirstone (a Stave-ring relief round a raised node),
#     choirstone_tiles (2x2 tiles, darker grout), choirstone_pillar (fluted
#     side) + choirstone_pillar_top (ringed end).
#   stave_stone        dark polished stone carved with a five-line Stave and
#                      glyph marks that glow cyan; 8-frame shimmer (a wave of
#                      light walks along the staff). Emissive block: every
#                      non-glyph texel is dark on purpose.
#   nightglass         smoked violet-teal glass, alpha ~0.6, dark frame with a
#                      faint cyan inner edge (tinted_glass's alpha pattern).
#   resonite_grate     copper_grate's cut-out mask, recoloured to resonite.
#   cyan_lumen_panel / violet_lumen_panel / amber_lumen_panel
#                      a glowing diffuser panel in a 2-px dark resonite frame;
#                      24-frame breathing pulse (interpolated).
#   lumen_strip        (side) dark casing with a bright band along the pillar
#                      axis, light flowing along it (16 frames);
#   lumen_strip_top    (end) the band's glowing cross-section in the casing.
#   crystal_conduit    a UV sheet: the 6-px crystal core (x 0..5, full
#                      height), the resonite collar's side (x 6..13, y 0..1)
#                      and its end ring (x 6..13, y 8..15) — see
#                      assets/models/block/crystal_conduit.json.
#   choir_lamp         a UV sheet: the crystal globe (x 0..6, y 0..6), the
#                      cage metal (x 8..15, y 0..7), the finial (x 8..9,
#                      y 8..9) and the hanging chain (x 13..15, y 8..13) —
#                      see assets/models/block/choir_lamp*.json.
#   resonant_water_still (16x512, 32 frames, frametime 2 — water_still's
#                      layout) and resonant_water_flow (32x1024, 32 frames of
#                      32x32 — water_flow's): the river Vesper's luminous
#                      violet water with aurora-cyan ripples. Coloured in the
#                      texture (drawn with a WHITE tint), alpha 176..208.
#   resonance_engine_side / resonance_engine_top
#                      the Heart's core: a crystal eye behind a dark resonite
#                      housing inside an engraved Stave ring; 16-frame pulse.
#   voice_beacon_lens / voice_beacon_side / voice_beacon_top
#                      a gate beacon: a tall prism lens between a resonite
#                      plinth and cap (the lighthouse lamp's language, pale
#                      cyan-white lens, a glowing aperture in the cap).
#   guttering_amber_window / waking_amber_window / restless_amber_window /
#   guttering_cyan_window / waking_cyan_window / guttering_violet_window /
#   waking_violet_window
#                      a lit window (four panes, a mullion cross, a lamp low in
#                      the room): 4 frames (lit, guttering, ember, dark) and a
#                      seeded per-block schedule in .mcmeta frames with
#                      per-frame "time" — see "flickering windows" below.
#
# Seeding: default_rng([0x4155524C ("AURL"), crc32(name)]) per texture, so
# adding or reordering textures never reshuffles the others.

from __future__ import annotations

import argparse
import io
import json
import math
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_hush_textures as ht  # noqa: E402  (noise / posterize / palette helpers)

REPO_ROOT = Path(__file__).resolve().parent.parent
TEX_ROOT = REPO_ROOT / "assets" / "textures"
TEX_DIR = TEX_ROOT / "block"
SEED_TAG = 0x4155524C  # "AURL"
W = H = 16

rgb = ht.rgb
paint = ht.paint
fbm = ht.fbm
posterize = ht.posterize
lerp_palette = ht.lerp_palette

# ── Aurelith's palette ───────────────────────────────────────────────────────
# Choirstone: pale cool ivory-grey (mean ~#C9CCC4) with teal veining. It is
# the one LIGHT stone in the Hush, so it reads as pale grey-blue at night
# against the near-black hushstone.
CHOIR_RAMP = lerp_palette("#A2A69F", "#E1E3DB", 5)
CHOIR_WEIGHTS = [18, 58, 92, 62, 26]
CHOIR_VEIN = ["#9FBEB8", "#B6CFCA"]
CHOIR_MORTAR = "#6F7570"
CHOIR_EDGE_LIGHT = "#E6E8E1"
CHOIR_EDGE_DARK = "#9EA29B"
# Dark resonite: the frames and casings of everything that glows. Emissive
# blocks are drawn UNDIMMED at night, so these must stay near-black.
DARK_METAL = ["#121119", "#1A1826", "#242133", "#302C44", "#3E3958"]
# Glow ramps.
CYAN_GLOW = ["#0F4E5C", "#1C8FA6", "#2CC6DA", "#5FF3FF", "#A6FAFF", "#E4FFFF"]
VIOLET_GLOW = ["#3A1E6A", "#6A3FB3", "#8E5CE6", "#B77CFF", "#D6B3FF", "#F2E6FF"]
AMBER_GLOW = ["#5C2A0C", "#A45418", "#E08A2C", "#FFB45A", "#FFD69A", "#FFF3DE"]
STAVE_STONE = ["#10151F", "#151C28", "#1A2230", "#202A3A", "#283346"]


def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng([SEED_TAG, zlib.crc32(name.encode("utf-8"))])


def blank(h: int = H, w: int = W) -> np.ndarray:
    return np.zeros((h, w, 4), dtype=np.uint8)


def mix(a: tuple, b: tuple, t: float) -> tuple:
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3)) + (255,)


def ramp_at(ramp: list[str], t: float) -> tuple:
    """Piecewise-linear lookup into a hex ramp, t in 0..1."""
    t = min(max(t, 0.0), 1.0) * (len(ramp) - 1)
    k = min(int(t), len(ramp) - 2)
    return mix(rgb(ramp[k]), rgb(ramp[k + 1]), t - k)


def metal(rng: np.random.Generator, shades: list[str] | None = None) -> np.ndarray:
    field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.3)], white=0.2)
    shades = shades or DARK_METAL[1:4]
    return paint(posterize(field, [30, 50, 20][:len(shades)]), [rgb(c) for c in shades])


def bevel(out: np.ndarray, light: str, dark: str) -> None:
    out[0, :] = rgb(light)
    out[:, 0] = rgb(light)
    out[-1, :] = rgb(dark)
    out[:, -1] = rgb(dark)


# ── the choirstone family ───────────────────────────────────────────────────
def choir_base(rng: np.random.Generator) -> np.ndarray:
    field = fbm(rng, [(4, 4, 0.5), (8, 4, 0.25), (2, 2, 0.15)], white=0.30)
    return paint(posterize(field, CHOIR_WEIGHTS), CHOIR_RAMP)


def add_vein(out: np.ndarray, rng: np.random.Generator, faint: bool = True) -> None:
    """A thin teal vein wandering across the tile, tileable (it leaves the
    right edge at the row it entered the left one)."""
    y0 = int(rng.integers(3, 13))
    ys = [y0]
    for x in range(1, W):
        step = int(rng.choice([-1, 0, 0, 1]))
        remaining = W - x
        target = y0 - ys[-1]
        if abs(target) >= remaining:
            step = int(np.sign(target))
        ys.append(ys[-1] + step)
    for x, y in enumerate(ys):
        yy = y % H
        out[yy, x] = rgb(CHOIR_VEIN[0] if (x + y) % 3 else CHOIR_VEIN[1])
        if not faint and rng.random() < 0.35:
            out[(yy + 1) % H, x] = rgb(CHOIR_VEIN[1])


def gen_choirstone(rng: np.random.Generator) -> np.ndarray:
    out = choir_base(rng)
    add_vein(out, rng)
    return out


def gen_polished_choirstone(rng: np.random.Generator) -> np.ndarray:
    interior = ["#C3C6BE", "#CACDC5", "#D1D4CC", "#D8DAD2"]
    field = fbm(rng, [(4, 4, 0.55), (2, 2, 0.25)], white=0.20)
    out = paint(posterize(field, [14, 44, 30, 12]), [rgb(c) for c in interior])
    bevel(out, CHOIR_EDGE_LIGHT, CHOIR_EDGE_DARK)
    out[0, 15] = rgb("#CACDC5")
    out[15, 0] = rgb("#CACDC5")
    return out


BRICK_FACE = ["#BEC1B9", "#C9CCC4", "#D3D6CE"]


def gen_choirstone_bricks(rng: np.random.Generator) -> np.ndarray:
    # Two 8-px ashlar courses; mortar on rows 7 and 15, the head joints at
    # x = 15 (upper course, one long block) and x = 7 (lower course), so the
    # courses break bond like vanilla stone bricks.
    field = fbm(rng, [(8, 2, 0.45), (2, 2, 0.25)], white=0.25)
    out = paint(posterize(field, [30, 45, 25]), [rgb(c) for c in BRICK_FACE])
    mortar, light, dark = rgb(CHOIR_MORTAR), rgb(CHOIR_EDGE_LIGHT), rgb(CHOIR_EDGE_DARK)
    for course, joint in ((0, 15), (1, 7)):
        top, bottom = course * 8, course * 8 + 7
        out[bottom, :] = mortar
        out[top:bottom, joint] = mortar
        out[top, :] = light
        out[bottom - 1, :] = dark
        for y in range(top, bottom):
            nxt = (joint + 1) % W
            out[y, nxt] = light if y < bottom - 1 else out[y, nxt]
            prv = (joint - 1) % W
            out[y, prv] = dark if y > top else out[y, prv]
        out[top, joint] = mortar
        out[bottom, joint] = mortar
    return out


def gen_cracked_choirstone_bricks(rng: np.random.Generator) -> np.ndarray:
    out = gen_choirstone_bricks(ht_seeded("choirstone_bricks"))
    crack = rgb("#5B625D")
    teal = [rgb("#1F8C84"), rgb("#34C2B2")]
    # Three cracks random-walking down from a course top; the second one has
    # sculk seeping through it.
    for n, (x, y, length) in enumerate(((3, 0, 9), (11, 8, 8), (6, 9, 5))):
        for _ in range(length):
            if 0 <= x < W and 0 <= y < H:
                out[y, x] = teal[_ % 2] if n == 1 and 2 <= _ <= 5 else crack
            y += 1
            x += int(rng.choice([-1, 0, 1]))
            x = min(max(x, 0), W - 1)
    # Chipped arrises: a few light flakes next to the cracks.
    for _ in range(4):
        fx, fy = int(rng.integers(1, 15)), int(rng.integers(1, 15))
        if tuple(out[fy, fx]) not in (crack, teal[0], teal[1], rgb(CHOIR_MORTAR)):
            out[fy, fx] = rgb("#E9EBE4")
    return out


def ht_seeded(name: str) -> np.random.Generator:
    """The rng another texture is built from, so a derived texture (cracked
    bricks) starts from exactly the pixels of its parent."""
    return rng_for(name)


def gen_chiseled_choirstone(rng: np.random.Generator) -> np.ndarray:
    # A Stave-ring relief: an engraved outer ring (dark groove, light lip
    # below-right) with four notches at the cardinal points, an inner ring,
    # and a raised central node. Framed like polished choirstone.
    out = gen_polished_choirstone(rng)
    groove, lip, node, node_hi = rgb("#8D928B"), rgb("#EEF0E9"), rgb("#B7BBB3"), rgb("#F4F5F0")
    for y in range(1, 15):
        for x in range(1, 15):
            d = math.hypot(x - 7.5, y - 7.5)
            if 5.6 <= d < 6.5 or 2.9 <= d < 3.7:
                out[y, x] = groove
            elif 6.5 <= d < 7.1 and (x + y) > 15:
                out[y, x] = lip
            elif 3.7 <= d < 4.2 and (x + y) > 15:
                out[y, x] = lip
            elif d < 1.6:
                out[y, x] = node_hi if (x + y) <= 15 else node
    for x, y in ((7, 1), (8, 1), (7, 14), (8, 14), (1, 7), (1, 8), (14, 7), (14, 8)):
        out[y, x] = groove
    return out


def gen_choirstone_tiles(rng: np.random.Generator) -> np.ndarray:
    grout = rgb("#8F948D")
    shades = [["#C0C3BB", "#C7CAC2", "#CDD0C8"], ["#C9CCC4", "#D0D3CB", "#D6D8D0"]]
    out = blank()
    for ty in range(2):
        for tx in range(2):
            pal = shades[(tx + ty) % 2]
            field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.3)], white=0.2, w=8, h=8)
            tile = paint(posterize(field, [30, 45, 25]), [rgb(c) for c in pal])
            tile[0, :7] = rgb(CHOIR_EDGE_LIGHT)
            tile[:7, 0] = rgb(CHOIR_EDGE_LIGHT)
            tile[6, 1:7] = rgb(CHOIR_EDGE_DARK)
            tile[1:7, 6] = rgb(CHOIR_EDGE_DARK)
            tile[7, :] = grout
            tile[:, 7] = grout
            out[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = tile
    return out


def gen_choirstone_pillar(rng: np.random.Generator) -> np.ndarray:
    # Fluted shaft: four flutes per face (period 4: lip, face, groove, face),
    # continuous top to bottom so a stacked column reads as one shaft.
    flute = ["#E2E4DC", "#CBCEC6", "#A7ABA4", "#BFC2BA"]
    field = fbm(rng, [(2, 8, 0.5), (2, 4, 0.2)], white=0.3)
    jitter = posterize(field, [20, 60, 20]) - 1
    out = blank()
    for y in range(H):
        for x in range(W):
            base = rgb(flute[x % 4])
            k = int(jitter[y, x]) * 6
            out[y, x] = tuple(min(max(c + k, 0), 255) for c in base[:3]) + (255,)
    return out


def gen_choirstone_pillar_top(rng: np.random.Generator) -> np.ndarray:
    out = gen_polished_choirstone(rng)
    for y in range(1, 15):
        for x in range(1, 15):
            d = math.hypot(x - 7.5, y - 7.5)
            if 6.0 <= d < 6.9:
                out[y, x] = rgb("#A5A9A2")
            elif 4.2 <= d < 5.0:
                out[y, x] = rgb("#B4B8B0")
            elif d < 1.9:
                out[y, x] = rgb("#E4E6DF")
    return out


# ── stave stone (emissive) ──────────────────────────────────────────────────
STAVE_LINES = (3, 5, 7, 9, 11)
STAVE_FRAMES = 8


def stave_layout(rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    """(stone index map, glow map): glow 0 = stone, 1 = staff line,
    2 = glyph mark. The staff runs edge to edge so a wall reads as one Stave."""
    field = fbm(rng, [(4, 4, 0.5), (2, 2, 0.3)], white=0.25)
    stone = posterize(field, [15, 35, 35, 15])
    glow = np.zeros((H, W), dtype=np.int64)
    for y in STAVE_LINES:
        glow[y, :] = 1
    # Glyphs: vertical strokes crossing two or three lines, and "notes"
    # (2x2 diamonds) sitting on a line or in a space. Spaced so no two touch.
    x = int(rng.integers(0, 3))
    while x < W - 1:
        kind = int(rng.integers(0, 3))
        if kind == 0:                                  # a stroke
            y0 = int(rng.choice([2, 4, 6]))
            for y in range(y0, y0 + int(rng.integers(4, 7))):
                if y < 13:
                    glow[y, x] = 2
        elif kind == 1:                                # a note with a stem
            yc = int(rng.choice(STAVE_LINES[:-1]))
            glow[yc, x] = glow[yc, x + 1] = 2
            glow[yc + 1, x] = glow[yc + 1, x + 1] = 2
            for y in range(yc - 3, yc):
                glow[max(y, 1), x + 1] = 2
        else:                                          # a bar: a line doubled
            yc = int(rng.choice(STAVE_LINES))
            glow[yc - 1, x] = 2
            glow[yc + 1, x] = 2
        x += int(rng.integers(3, 5))
    return stone, glow


def gen_stave_stone(rng: np.random.Generator) -> np.ndarray:
    stone, glow = stave_layout(rng)
    strip = blank(H * STAVE_FRAMES, W)
    for f in range(STAVE_FRAMES):
        frame = paint(stone, [rgb(c) for c in STAVE_STONE[:4]])
        frame[0, :] = rgb(STAVE_STONE[4])
        frame[15, :] = rgb(STAVE_STONE[0])
        for y in range(H):
            for x in range(W):
                if glow[y, x] == 0:
                    continue
                # A wave of light walking along the staff, one pass per loop.
                phase = (x / W - f / STAVE_FRAMES) * 2.0 * math.pi
                wave = 0.5 + 0.5 * math.cos(phase)
                if glow[y, x] == 1:
                    frame[y, x] = ramp_at(CYAN_GLOW[:4], 0.35 + 0.35 * wave)
                else:
                    frame[y, x] = ramp_at(CYAN_GLOW[2:], 0.25 + 0.75 * wave)
        strip[f * H:(f + 1) * H] = frame
    return strip


# ── nightglass ──────────────────────────────────────────────────────────────
def gen_nightglass(rng: np.random.Generator) -> np.ndarray:
    # tinted_glass's two alphas: the pane at ~0.6, the frame and streaks at
    # ~0.8. Smoked violet-teal with a thin cyan line just inside the frame.
    out = blank()
    pane = (30, 42, 58, 152)
    out[:, :] = pane
    field = fbm(rng, [(8, 8, 0.6), (4, 4, 0.3)], white=0.1)
    idx = posterize(field, [70, 30])
    out[idx == 1] = (36, 48, 68, 152)
    frame, inner = (20, 26, 38, 214), (46, 132, 148, 196)
    out[0, :] = out[15, :] = frame
    out[:, 0] = out[:, 15] = frame
    for i in range(2, 14):
        if i % 4 != 1:
            out[1, i] = out[14, i] = inner
            out[i, 1] = out[i, 14] = inner
    # Two diagonal glints across the pane.
    for start, length in ((4, 5), (8, 3)):
        for k in range(length):
            x, y = start + k, 11 - k
            if 2 <= x <= 13 and 2 <= y <= 13:
                out[y, x] = (70, 88, 118, 176)
    return out


# ── resonite grate (copper_grate's mask) ────────────────────────────────────
def gen_resonite_grate(rng: np.random.Generator) -> np.ndarray:
    del rng
    src = ht.load_vanilla("block/copper_grate.png")
    # copper_grate's luminance runs ~0.3..0.55; map it onto a narrow
    # violet-steel ramp so the bars read as one clean metal with lit edges.
    return ht.gradient_map(src, [(0.28, "#35304F"), (0.40, "#4F4970"), (0.47, "#5E5784"),
                                 (0.53, "#71699A"), (0.62, "#9A8FC6")])


# ── lumen panels (emissive) ─────────────────────────────────────────────────
PANEL_FRAMES = 24


def panel_strip(rng: np.random.Generator, ramp: list[str]) -> np.ndarray:
    # Static shading bands (posterized) that the pulse scales, so the panel
    # keeps its painted look while it breathes. 2-px frame: outer near-black
    # metal, inner bevel; the diffuser has faint vertical slats.
    level = np.zeros((H, W))
    for y in range(2, 14):
        for x in range(2, 14):
            # Rounded-square falloff from the centre (0) to the frame (1),
            # posterized into bands below; a faint sheen along the top-left.
            dx, dy = abs(x - 7.5) / 5.5, abs(y - 7.5) / 5.5
            d = (dx ** 4 + dy ** 4) ** 0.25
            level[y, x] = 1.0 - 0.6 * d ** 1.5
            if (x == 3 and 3 <= y <= 8) or (y == 3 and 3 <= x <= 8):
                level[y, x] += 0.12
    level += (fbm(rng, [(4, 4, 1.0)], white=0.0) - 0.5) * 0.05
    strip = blank(H * PANEL_FRAMES, W)
    for f in range(PANEL_FRAMES):
        breath = 0.84 + 0.16 * math.sin(2.0 * math.pi * f / PANEL_FRAMES)
        frame = blank()
        frame[:, :] = rgb(DARK_METAL[1])
        frame[0, :] = frame[:, 0] = rgb(DARK_METAL[2])
        frame[15, :] = frame[:, 15] = rgb(DARK_METAL[0])
        frame[1, 1:15] = frame[1:15, 1] = rgb(DARK_METAL[3])
        frame[14, 1:15] = frame[1:15, 14] = rgb(DARK_METAL[2])
        for y in range(2, 14):
            for x in range(2, 14):
                t = level[y, x] * breath
                # Posterize to 6 steps, like hand-painted block art.
                t = round(t * 6) / 6
                frame[y, x] = ramp_at(ramp, 0.25 + 0.75 * t)
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_cyan_lumen_panel(rng):   return panel_strip(rng, CYAN_GLOW)
def gen_violet_lumen_panel(rng): return panel_strip(rng, VIOLET_GLOW)
def gen_amber_lumen_panel(rng):  return panel_strip(rng, AMBER_GLOW)


# ── lumen strip (emissive, a pillar) ────────────────────────────────────────
STRIP_FRAMES = 16
BAND = {5: 0.30, 6: 0.62, 7: 1.0, 8: 1.0, 9: 0.62, 10: 0.30}   # column -> weight


def strip_casing(rng: np.random.Generator) -> np.ndarray:
    out = metal(rng, DARK_METAL[0:3])
    out[:, 4] = rgb(DARK_METAL[3])
    out[:, 11] = rgb(DARK_METAL[3])
    out[:, 3] = rgb(DARK_METAL[0])
    out[:, 12] = rgb(DARK_METAL[0])
    return out


def gen_lumen_strip(rng: np.random.Generator) -> np.ndarray:
    casing = strip_casing(rng)
    strip = blank(H * STRIP_FRAMES, W)
    for f in range(STRIP_FRAMES):
        frame = casing.copy()
        for y in range(H):
            # Light flowing along the axis: a soft pulse sliding one texel a
            # frame (downward in the texture), plus a steady base glow.
            p = ((y - f) % H) / H
            pulse = max(0.0, math.cos(2.0 * math.pi * p)) ** 3
            for x, wgt in BAND.items():
                t = wgt * (0.55 + 0.45 * pulse)
                frame[y, x] = ramp_at(CYAN_GLOW, round(t * 6) / 6)
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_lumen_strip_top(rng: np.random.Generator) -> np.ndarray:
    out = metal(rng, DARK_METAL[0:3])
    bevel(out, DARK_METAL[3], DARK_METAL[0])
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - 7.5, y - 7.5)
            if d < 1.6:
                out[y, x] = rgb(CYAN_GLOW[5])
            elif d < 2.6:
                out[y, x] = rgb(CYAN_GLOW[3])
            elif d < 3.4:
                out[y, x] = rgb(CYAN_GLOW[1])
            elif d < 4.2:
                out[y, x] = rgb(DARK_METAL[3])
    return out


# ── crystal conduit (UV sheet) ──────────────────────────────────────────────
def gen_crystal_conduit(rng: np.random.Generator) -> np.ndarray:
    out = blank()
    # The core, x 0..5: faceted crystal, a violet seam down one facet and a
    # bright cyan line down the middle; brightness wanders along its length.
    wander = fbm(rng, [(1, 4, 1.0)], white=0.15, w=1, h=16)[:, 0]
    for y in range(H):
        for x in range(6):
            base = [0.35, 0.62, 0.9, 1.0, 0.7, 0.42][x]
            t = base * (0.8 + 0.25 * wander[y])
            col = ramp_at(CYAN_GLOW, t)
            if x == 1 and (y // 3) % 2 == 0:
                col = ramp_at(VIOLET_GLOW, 0.55 + 0.2 * wander[y])
            out[y, x] = col
    # Collar side, x 6..13, y 0..1: dark resonite with two rivets.
    for x in range(6, 14):
        out[0, x] = rgb(DARK_METAL[3])
        out[1, x] = rgb(DARK_METAL[1])
    out[0, 8] = out[0, 11] = rgb("#6A6294")
    # Collar end ring, x 6..13, y 8..15: the metal ring round the core's
    # glowing cross-section (the core is 6 wide inside an 8-wide collar).
    for y in range(8, 16):
        for x in range(6, 14):
            edge = x in (6, 13) or y in (8, 15)
            out[y, x] = rgb(DARK_METAL[3] if edge else DARK_METAL[2])
            if 7 <= x <= 12 and 9 <= y <= 14:
                d = max(abs(x - 9.5), abs(y - 11.5))
                out[y, x] = ramp_at(CYAN_GLOW, 1.0 - 0.18 * d)
    return out


# ── choir lamp (UV sheet) ───────────────────────────────────────────────────
def gen_choir_lamp(rng: np.random.Generator) -> np.ndarray:
    out = blank()
    # Globe face, x 0..6, y 0..6 (7x7): a crystal sphere lit from within,
    # brightest just above centre, a violet cast at the rim.
    for y in range(7):
        for x in range(7):
            d = math.hypot(x - 3.0, y - 2.6) / 4.2
            t = 1.0 - d
            col = ramp_at(CYAN_GLOW, 0.35 + 0.7 * t)
            if d > 0.78:
                col = ramp_at(VIOLET_GLOW, 0.45)
            out[y, x] = col
    out[1, 2] = rgb("#FFFFFF")
    # Cage metal, x 8..15, y 0..7: dark resonite with a lit rim row.
    cage = metal(rng, DARK_METAL[1:4])
    out[0:8, 8:16] = cage[0:8, 0:8]
    out[0, 8:16] = rgb(DARK_METAL[4])
    out[7, 8:16] = rgb(DARK_METAL[0])
    for x in (9, 14):
        out[3, x] = rgb("#6A6294")                   # rivets
    # Finial, x 8..9, y 8..9: a small crystal bead.
    out[8, 8] = rgb(CYAN_GLOW[4])
    out[8, 9] = out[9, 8] = rgb(CYAN_GLOW[3])
    out[9, 9] = rgb(CYAN_GLOW[2])
    # Chain, x 13..15, y 8..13: resonite links (the hanging lamp's chain).
    for y in range(8, 14):
        for x in range(13, 16):
            link = (y - 8) % 3
            if (link == 0 and x == 14) or (link != 0 and x in (13, 15)):
                out[y, x] = rgb(DARK_METAL[4] if link == 1 else DARK_METAL[3])
    return out


# ── resonant water (the river Vesper) ───────────────────────────────────────
WATER_FRAMES = 32
WATER_RAMP = ["#2A1A6E", "#43299E", "#6440D2", "#8A5CFF", "#7FA8FF", "#6AF0FF", "#CFFFFF"]
WATER_WEIGHTS = [8, 22, 30, 20, 10, 7, 3]


def water_field(size: int, frames: int, flow: bool, rng: np.random.Generator) -> np.ndarray:
    """(frames, size, size) field, tileable in x/y and looping in time: a sum
    of plane waves with INTEGER spatial frequencies and integer temporal
    cycles, plus ridged caustic terms. `flow` advects the whole pattern one
    texel per frame down the sprite (the direction MC's flowing sprite runs)."""
    tau = 2.0 * math.pi
    waves = []
    # Still water: long, slow swells (low frequencies) so a 16-px tile does
    # not speckle; flowing water carries finer ripples.
    kmax = 3 if flow else 2
    for _ in range(6):
        kx, ky = int(rng.integers(-kmax + 1, kmax)), int(rng.integers(1, kmax + 1))
        cyc = int(rng.integers(1, 3)) * (1 if rng.random() < 0.5 else -1)
        waves.append((kx, ky, cyc, rng.random() * tau, 0.4 + rng.random() * 0.6))
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64)
    out = np.zeros((frames, size, size))
    for f in range(frames):
        t = f / frames
        yy = (ys - (f * size / frames if flow else 0.0))
        field = np.zeros((size, size))
        for n, (kx, ky, cyc, ph, amp) in enumerate(waves):
            arg = tau * (kx * xs / size + ky * yy / size) + tau * cyc * t + ph
            if n < 2:
                field += amp * (1.0 - np.abs(np.sin(arg))) ** 4     # caustic ridges
            else:
                field += amp * 0.5 * np.sin(arg)
        out[f] = field
    return out


def water_strip(size: int, flow: bool, rng: np.random.Generator) -> np.ndarray:
    field = water_field(size, WATER_FRAMES, flow, rng)
    strip = blank(size * WATER_FRAMES, size)
    for f in range(WATER_FRAMES):
        idx = posterize(field[f] + rng.random((size, size)) * 1e-9, WATER_WEIGHTS)
        frame = paint(idx, [rgb(c) for c in WATER_RAMP])
        # Denser where it glows: alpha 176 (the violet body) .. 208 (the
        # brightest ripples); vanilla water is a flat 180.
        frame[:, :, 3] = (176 + idx * 32 // (len(WATER_RAMP) - 1)).astype(np.uint8)
        strip[f * size:(f + 1) * size] = frame
    return strip


def gen_resonant_water_still(rng: np.random.Generator) -> np.ndarray:
    return water_strip(16, False, rng)


def gen_resonant_water_flow(rng: np.random.Generator) -> np.ndarray:
    return water_strip(32, True, rng)


# ── resonance engine (emissive) ─────────────────────────────────────────────
ENGINE_FRAMES = 16


def engine_face(rng: np.random.Generator, top: bool) -> np.ndarray:
    housing = metal(rng, DARK_METAL[0:3])
    bevel(housing, DARK_METAL[4], DARK_METAL[0])
    strip = blank(H * ENGINE_FRAMES, W)
    for f in range(ENGINE_FRAMES):
        pulse = 0.5 + 0.5 * math.sin(2.0 * math.pi * f / ENGINE_FRAMES)
        frame = housing.copy()
        for y in range(1, 15):
            for x in range(1, 15):
                d = math.hypot(x - 7.5, y - 7.5)
                ang = (math.atan2(y - 7.5, x - 7.5) / (2.0 * math.pi)) % 1.0
                if 5.5 <= d < 6.4:
                    # The engraved Stave ring: five arcs with gaps, a light
                    # chasing round it.
                    if int(ang * 10) % 2 == 0:
                        chase = 0.5 + 0.5 * math.cos(2.0 * math.pi * (ang - f / ENGINE_FRAMES))
                        frame[y, x] = ramp_at(CYAN_GLOW, 0.2 + 0.5 * chase)
                    else:
                        frame[y, x] = rgb(DARK_METAL[3])
                elif 4.2 <= d < 5.5:
                    frame[y, x] = rgb(DARK_METAL[2] if (x + y) % 2 else DARK_METAL[1])
                elif d < 4.2:
                    # The crystal eye: bright core, violet at the rim.
                    t = 1.0 - d / 4.2
                    col = ramp_at(CYAN_GLOW, 0.3 + 0.55 * t + 0.25 * pulse * t)
                    if d > 3.3:
                        col = ramp_at(VIOLET_GLOW, 0.4 + 0.2 * pulse)
                    frame[y, x] = col
        if not top:
            # Housing seams on the sides: a vertical rib either side of the eye.
            for y in (1, 14):
                frame[y, 3:13] = rgb(DARK_METAL[3])
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_resonance_engine_side(rng): return engine_face(rng, top=False)
def gen_resonance_engine_top(rng):  return engine_face(rng, top=True)


# ── voice beacon (emissive) ─────────────────────────────────────────────────
def gen_voice_beacon_lens(rng: np.random.Generator) -> np.ndarray:
    # Model: lens on x 3..13 / y 4..14 of the side faces. Tall prism steps,
    # brightest down the vertical centre line (the beam leaves upward).
    out = blank()
    for y in range(H):
        for x in range(W):
            t = 1.0 - abs(x - 7.5) / 8.0
            if y % 4 == 0:
                t -= 0.18                              # prism step
            out[y, x] = ramp_at(CYAN_GLOW, 0.35 + 0.7 * t)
    for x in (2, 13):
        for y in range(H):
            out[y, x] = rgb(DARK_METAL[3] if y % 3 else "#6A6294")
    for _ in range(3):
        out[int(rng.integers(2, 14)), int(rng.integers(5, 11))] = rgb("#F4FFFF")
    return out


def gen_voice_beacon_side(rng: np.random.Generator) -> np.ndarray:
    # Model: plinth band on rows 12..15, cap band on rows 0..1.
    out = metal(rng, DARK_METAL[1:4])
    out[0, :] = rgb(DARK_METAL[4])
    out[1, :] = rgb(CYAN_GLOW[2])                 # the cap's lit rim
    out[12, :] = rgb(CYAN_GLOW[2])                # the plinth's lit rim
    out[13, :] = rgb(DARK_METAL[4])
    out[15, :] = rgb(DARK_METAL[0])
    for x in range(1, 15, 3):                      # Stave ticks on the plinth
        out[14, x] = rgb(CYAN_GLOW[1])
    return out


def gen_voice_beacon_top(rng: np.random.Generator) -> np.ndarray:
    out = metal(rng, DARK_METAL[1:4])
    bevel(out, DARK_METAL[4], DARK_METAL[0])
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - 7.5, y - 7.5)
            if d < 1.8:
                out[y, x] = rgb(CYAN_GLOW[5])
            elif d < 2.8:
                out[y, x] = rgb(CYAN_GLOW[3])
            elif 4.6 <= d < 5.4:
                out[y, x] = rgb(CYAN_GLOW[1])
    return out


# ── flickering windows (emissive) ───────────────────────────────────────────
# A lit window in a tower or a Stillhouse: four panes behind a resonite
# mullion cross, a lamp glowing low in the room behind them. Four frames —
# 0 lit, 1 guttering (the lamp sags), 2 ember (all but out), 3 dark (smoked
# glass with a faint reflection) — and an irregular schedule per block written
# as .mcmeta frames with per-frame "time" (MC AnimationFrame.time; the engine's
# TextureAnimator honours it). Every block has its own seeded schedule, loops
# of ~2-3 minutes, so no two kinds of window blink together.
# The blocks are emissive (drawn undimmed), so the dark frame is painted at the
# Hush's night level already.
WINDOW_LIT, WINDOW_LOW, WINDOW_EMBER, WINDOW_DARK = 0, 1, 2, 3
WINDOWS = {
    # block: (glow ramp, behaviour)
    "guttering_amber_window": (AMBER_GLOW, "guttering"),
    "waking_amber_window": (AMBER_GLOW, "waking"),
    "restless_amber_window": (AMBER_GLOW, "restless"),
    "guttering_cyan_window": (CYAN_GLOW, "guttering"),
    "waking_cyan_window": (CYAN_GLOW, "waking"),
    "guttering_violet_window": (VIOLET_GLOW, "guttering"),
    "waking_violet_window": (VIOLET_GLOW, "waking"),
}
WINDOW_LOOP_TICKS = (2400, 3600)


def window_is_pane(x: int, y: int) -> bool:
    return 2 <= x <= 13 and 2 <= y <= 13 and x not in (7, 8) and y not in (7, 8)


def window_frame(ramp: list[str], level: float, rng: np.random.Generator) -> np.ndarray:
    """One frame: the metal frame and mullions, and the panes lit to `level`
    (1 full, 0 dark)."""
    out = blank()
    out[:, :] = rgb(DARK_METAL[1])
    out[0, :] = out[:, 0] = rgb(DARK_METAL[2])
    out[15, :] = out[:, 15] = rgb(DARK_METAL[0])
    out[1, 1:15] = out[1:15, 1] = rgb(DARK_METAL[3])
    out[14, 1:15] = out[1:15, 14] = rgb(DARK_METAL[2])
    for i in (7, 8):                                   # the mullion cross
        out[2:14, i] = rgb(DARK_METAL[2 if i == 7 else 1])
        out[i, 2:14] = rgb(DARK_METAL[2 if i == 7 else 1])
    for y in range(2, 14):
        for x in range(2, 14):
            if not window_is_pane(x, y):
                continue
            if level <= 0.0:
                # Smoked glass at night: near-black teal, a faint reflection
                # of the sky slanting across the upper panes.
                refl = (x + y) in (9, 10) or (x + y) in (19,)
                out[y, x] = (18, 24, 32, 255) if refl else (9, 12, 18, 255)
                continue
            # The lamp sits low in the room: brightest at the bottom middle,
            # the upper panes dimmer; posterized like block art.
            d = math.hypot((x - 7.5) / 7.0, (y - 12.0) / 9.0)
            t = max(0.0, 1.0 - 0.75 * d) * level
            t = round(t * 6) / 6
            col = ramp_at(ramp, 0.15 + 0.85 * t)
            if level < 0.5:
                # An ember: the lamp's colour sinking into the dark glass.
                col = mix((9, 12, 18), col, 0.25 + 0.75 * (level / 0.5) ** 1.5)
            out[y, x] = col
    if level >= 0.99:
        # A glint on the glass, top-left of each lower pane.
        for (x, y) in ((3, 9), (10, 9)):
            out[y, x] = ramp_at(ramp, 1.0)
    return out


def window_schedule(name: str, behaviour: str) -> list[dict]:
    """The block's frame schedule: [{index, time}], one seeded loop."""
    r = np.random.default_rng([SEED_TAG, zlib.crc32(("schedule/" + name).encode("utf-8"))])

    def ri(a: int, b: int) -> int:
        return int(r.integers(a, b + 1))

    steps: list[tuple[int, int]] = []
    target = ri(*WINDOW_LOOP_TICKS)
    total = 0

    def put(frame: int, ticks: int) -> None:
        nonlocal total
        if steps and steps[-1][0] == frame:
            steps[-1] = (frame, steps[-1][1] + ticks)
        else:
            steps.append((frame, ticks))
        total += ticks

    while total < target:
        roll = r.random()
        if behaviour == "guttering":
            # Mostly lit; the lamp sags now and then, once in a while goes out
            # and is coaxed back.
            put(WINDOW_LIT, ri(260, 1100))
            if roll < 0.55:
                put(WINDOW_LOW, ri(2, 4))
                if r.random() < 0.5:
                    put(WINDOW_LIT, ri(2, 4))
                    put(WINDOW_LOW, ri(2, 3))
            elif roll < 0.85:
                put(WINDOW_LOW, ri(2, 4))
                put(WINDOW_EMBER, ri(4, 10))
                put(WINDOW_LOW, ri(3, 5))
            else:
                put(WINDOW_LOW, 3)
                put(WINDOW_EMBER, 3)
                put(WINDOW_DARK, ri(40, 160))
                put(WINDOW_EMBER, ri(3, 5))
                put(WINDOW_LOW, 3)
                put(WINDOW_EMBER, 2)
                put(WINDOW_LOW, ri(3, 6))
        elif behaviour == "waking":
            # Mostly dark; now and then the lamp is lit for a spell, or a
            # glow passes (a candle carried past the glass).
            put(WINDOW_DARK, ri(400, 1400))
            if roll < 0.5:
                put(WINDOW_EMBER, ri(6, 14))
                put(WINDOW_LOW, ri(4, 8))
                put(WINDOW_LIT, ri(160, 620))
                put(WINDOW_LOW, ri(3, 5))
                put(WINDOW_EMBER, ri(5, 9))
            elif roll < 0.8:
                put(WINDOW_EMBER, ri(20, 80))
            else:
                put(WINDOW_LOW, 2)
                put(WINDOW_DARK, 3)
                put(WINDOW_LOW, 3)
                put(WINDOW_LIT, ri(60, 240))
                put(WINDOW_LOW, 3)
                put(WINDOW_EMBER, 5)
        else:  # restless: as often lit as not, never settled for long
            put(WINDOW_LIT, ri(100, 360))
            if roll < 0.7:
                put(WINDOW_LOW, ri(2, 5))
                put(WINDOW_EMBER, ri(3, 6))
                put(WINDOW_DARK, ri(140, 440))
                put(WINDOW_EMBER, ri(3, 6))
                put(WINDOW_LOW, ri(2, 4))
            else:
                put(WINDOW_LOW, ri(3, 8))
                put(WINDOW_LIT, ri(10, 30))
                put(WINDOW_LOW, ri(2, 4))
    return [{"index": f, "time": t} for f, t in steps]


def window_strip(name: str) -> np.ndarray:
    ramp, _ = WINDOWS[name]
    rng = rng_for(name)
    strip = blank(H * 4, W)
    for frame, level in ((WINDOW_LIT, 1.0), (WINDOW_LOW, 0.55), (WINDOW_EMBER, 0.2), (WINDOW_DARK, 0.0)):
        strip[frame * H:(frame + 1) * H] = window_frame(ramp, level, rng)
    return strip


def window_meta(name: str) -> dict:
    # frametime is only the default for an entry without its own time (every
    # entry here has one); interpolation off, as MC requires with frames.
    return {"animation": {"frametime": 1, "frames": window_schedule(name, WINDOWS[name][1])}}


# ── registry ────────────────────────────────────────────────────────────────
# name -> (generator, .mcmeta dict or None)
def anim(frametime: int, interpolate: bool = False) -> dict:
    a: dict = {"frametime": frametime}
    if interpolate:
        a["interpolate"] = True
    return {"animation": a}


# ── reawakening the Heart: the dormant lights ───────────────────────────────
# docs/the-hush.md "Reawakening the Heart". While the Heart is silent the
# city's lights burn at about half their voice: the same art (seeded from the
# lit twin's name, so a dim panel and its lit twin are the same panel), the
# glow ramp read lower and breathing shallower and slower. The awakening's
# light wave swaps each dim_* block for its twin (AurelithQuestBlocks.hpp).
DIM_LEVEL = 0.48          # the glow ramp's top, relative to the lit twin's


def dim_panel_strip(ramp: list[str], twin: str) -> np.ndarray:
    rng = rng_for(twin)
    level = np.zeros((H, W))
    for y in range(2, 14):
        for x in range(2, 14):
            dx, dy = abs(x - 7.5) / 5.5, abs(y - 7.5) / 5.5
            d = (dx ** 4 + dy ** 4) ** 0.25
            level[y, x] = 1.0 - 0.6 * d ** 1.5
            if (x == 3 and 3 <= y <= 8) or (y == 3 and 3 <= x <= 8):
                level[y, x] += 0.12
    level += (fbm(rng, [(4, 4, 1.0)], white=0.0) - 0.5) * 0.05
    strip = blank(H * PANEL_FRAMES, W)
    for f in range(PANEL_FRAMES):
        breath = 0.92 + 0.08 * math.sin(2.0 * math.pi * f / PANEL_FRAMES)
        frame = blank()
        frame[:, :] = rgb(DARK_METAL[1])
        frame[0, :] = frame[:, 0] = rgb(DARK_METAL[2])
        frame[15, :] = frame[:, 15] = rgb(DARK_METAL[0])
        frame[1, 1:15] = frame[1:15, 1] = rgb(DARK_METAL[3])
        frame[14, 1:15] = frame[1:15, 14] = rgb(DARK_METAL[2])
        for y in range(2, 14):
            for x in range(2, 14):
                t = round(level[y, x] * breath * 6) / 6
                frame[y, x] = ramp_at(ramp, 0.10 + DIM_LEVEL * 0.75 * t)
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_dim_cyan_lumen_panel(rng):   return dim_panel_strip(CYAN_GLOW, "cyan_lumen_panel")
def gen_dim_violet_lumen_panel(rng): return dim_panel_strip(VIOLET_GLOW, "violet_lumen_panel")
def gen_dim_amber_lumen_panel(rng):  return dim_panel_strip(AMBER_GLOW, "amber_lumen_panel")


def gen_dim_stave_stone(rng: np.random.Generator) -> np.ndarray:
    stone, glow = stave_layout(rng_for("stave_stone"))
    strip = blank(H * STAVE_FRAMES, W)
    for f in range(STAVE_FRAMES):
        frame = paint(stone, [rgb(c) for c in STAVE_STONE[:4]])
        frame[0, :] = rgb(STAVE_STONE[4])
        frame[15, :] = rgb(STAVE_STONE[0])
        for y in range(H):
            for x in range(W):
                if glow[y, x] == 0:
                    continue
                phase = (x / W - f / STAVE_FRAMES) * 2.0 * math.pi
                wave = 0.5 + 0.5 * math.cos(phase)
                if glow[y, x] == 1:
                    frame[y, x] = ramp_at(CYAN_GLOW[:3], 0.25 + 0.30 * wave)
                else:
                    frame[y, x] = ramp_at(CYAN_GLOW[:4], 0.30 + 0.45 * wave)
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_dim_lumen_strip(rng: np.random.Generator) -> np.ndarray:
    casing = strip_casing(rng_for("lumen_strip"))
    strip = blank(H * STRIP_FRAMES, W)
    for f in range(STRIP_FRAMES):
        frame = casing.copy()
        for y in range(H):
            p = ((y - f) % H) / H
            pulse = max(0.0, math.cos(2.0 * math.pi * p)) ** 3
            for x, wgt in BAND.items():
                t = wgt * (0.62 + 0.38 * pulse) * DIM_LEVEL
                frame[y, x] = ramp_at(CYAN_GLOW, round(t * 6) / 6)
        strip[f * H:(f + 1) * H] = frame
    return strip


def gen_dim_lumen_strip_top(rng: np.random.Generator) -> np.ndarray:
    out = metal(rng_for("lumen_strip_top"), DARK_METAL[0:3])
    bevel(out, DARK_METAL[3], DARK_METAL[0])
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - 7.5, y - 7.5)
            if d < 1.6:
                out[y, x] = rgb(CYAN_GLOW[3])
            elif d < 2.6:
                out[y, x] = rgb(CYAN_GLOW[2])
            elif d < 3.4:
                out[y, x] = rgb(CYAN_GLOW[0])
            elif d < 4.2:
                out[y, x] = rgb(DARK_METAL[3])
    return out


def gen_dim_choir_lamp(rng: np.random.Generator) -> np.ndarray:
    out = gen_choir_lamp(rng_for("choir_lamp"))
    # The globe (x 0..6, y 0..6) turned down: the crystal still holds a
    # little light, the violet rim barely.
    for y in range(7):
        for x in range(7):
            d = math.hypot(x - 3.0, y - 2.6) / 4.2
            t = 1.0 - d
            col = ramp_at(CYAN_GLOW, 0.15 + 0.42 * t)
            if d > 0.78:
                col = ramp_at(VIOLET_GLOW, 0.2)
            out[y, x] = col
    out[1, 2] = rgb(CYAN_GLOW[3])
    out[8, 8] = rgb(CYAN_GLOW[2])
    out[8, 9] = out[9, 8] = rgb(CYAN_GLOW[1])
    out[9, 9] = rgb(CYAN_GLOW[1])
    return out


# ── reawakening the Heart: the quest's blocks ───────────────────────────────
RESONITE = ["#15131D", "#221F2E", "#322D45", "#48416A", "#6A6294", "#8F87B8"]
WHISPERWOOD = ["#2A2433", "#3A3346", "#4A4058", "#5C506C", "#6E6180"]


def gen_chord_socket_post(rng: np.random.Generator) -> np.ndarray:
    # The socket's post: polished choirstone with a single Stave line (dim,
    # glowing only when the city wakes — the renderer adds that light)
    # running up the face, and a darker banding at foot and head.
    out = choir_base(rng)
    add_vein(out, rng, faint=True)
    for y in range(H):
        out[y, 7] = ramp_at(CYAN_GLOW[:3], 0.35 + 0.25 * (y % 4 == 1))
        out[y, 8] = rgb(STAVE_STONE[3])
    out[0, :] = rgb(CHOIR_EDGE_LIGHT)
    out[15, :] = rgb(CHOIR_EDGE_DARK)
    return out


def gen_chord_socket_cradle(rng: np.random.Generator) -> np.ndarray:
    # The resonite cradle's sides: dark metal with a lit rim and rivets.
    out = metal(rng, RESONITE[1:4])
    out[0, :] = rgb(RESONITE[4])
    out[1, :] = rgb(RESONITE[3])
    out[15, :] = rgb(RESONITE[0])
    for x in range(2, 16, 4):
        out[8, x] = rgb(RESONITE[5])
        out[9, x] = rgb(RESONITE[2])
    return out


def gen_chord_socket_top(rng: np.random.Generator) -> np.ndarray:
    # Looking down into the cradle: a resonite ring round a keyway — the
    # narrow slot a voice key stands in — with a faint crystal lip.
    out = metal(rng, RESONITE[1:4])
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - 7.5, y - 7.5)
            if d > 7.2:
                out[y, x] = rgb(RESONITE[3])
            elif d > 6.2:
                out[y, x] = rgb(RESONITE[4])
            elif d < 4.6 and d > 3.6:
                out[y, x] = ramp_at(CYAN_GLOW[:3], 0.3)
    for y in range(4, 12):                     # the keyway
        out[y, 7] = rgb("#07060B")
        out[y, 8] = rgb("#07060B")
    for x in (6, 9):
        out[4, x] = out[11, x] = rgb(RESONITE[0])
    return out


def gen_voice_pedestal_side(rng: np.random.Generator) -> np.ndarray:
    # Polished choirstone with a band of Stave glyphs at mid-height.
    out = choir_base(rng)
    stone, glow = stave_layout(rng)
    for y in range(6, 11):
        for x in range(W):
            out[y, x] = rgb(STAVE_STONE[2 if y in (6, 10) else 1])
            if glow[y - 3, x]:
                out[y, x] = ramp_at(CYAN_GLOW[:4], 0.45)
    out[5, :] = rgb(CHOIR_EDGE_LIGHT)
    out[11, :] = rgb(CHOIR_EDGE_DARK)
    return out


def gen_voice_pedestal_top(rng: np.random.Generator) -> np.ndarray:
    # A chiseled ring with a crystal inlay: the seat an offering rests over.
    out = choir_base(rng)
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - 7.5, y - 7.5)
            if 5.2 < d < 6.3:
                out[y, x] = rgb(CHOIR_EDGE_DARK)
            elif 3.6 < d < 4.6:
                out[y, x] = ramp_at(CYAN_GLOW, 0.55 + 0.1 * math.cos(math.atan2(y - 7.5, x - 7.5) * 4))
            elif d <= 2.0:
                out[y, x] = rgb(CHOIR_EDGE_LIGHT)
    bevel(out, CHOIR_EDGE_LIGHT, CHOIR_EDGE_DARK)
    return out


def _cabinet_wood(rng: np.random.Generator) -> np.ndarray:
    field = fbm(rng, [(16, 2, 0.6), (4, 1, 0.3)], white=0.15)
    return paint(posterize(field, [18, 40, 30, 12]), [rgb(c) for c in WHISPERWOOD[1:5]])


def gen_choir_cabinet_side(rng: np.random.Generator) -> np.ndarray:
    # Whisperwood boards in a resonite frame, a recessed panel.
    out = _cabinet_wood(rng)
    bevel(out, RESONITE[4], RESONITE[1])
    out[1, 1:15] = out[1:15, 1] = rgb(RESONITE[3])
    out[14, 1:15] = out[1:15, 14] = rgb(RESONITE[2])
    for y in (4, 11):
        out[y, 3:13] = rgb(WHISPERWOOD[0])
    for x in (3, 12):
        out[4:12, x] = rgb(WHISPERWOOD[0])
    return out


def gen_choir_cabinet_back(rng: np.random.Generator) -> np.ndarray:
    out = _cabinet_wood(rng)
    bevel(out, RESONITE[3], RESONITE[1])
    return out


def gen_choir_cabinet_top(rng: np.random.Generator) -> np.ndarray:
    out = _cabinet_wood(rng)
    bevel(out, RESONITE[4], RESONITE[1])
    out[2:14, 7] = rgb(WHISPERWOOD[0])
    return out


def _cabinet_doors(rng: np.random.Generator, opened: bool) -> np.ndarray:
    out = _cabinet_wood(rng)
    bevel(out, RESONITE[4], RESONITE[1])
    out[1, 1:15] = out[1:15, 1] = rgb(RESONITE[3])
    out[14, 1:15] = out[1:15, 14] = rgb(RESONITE[2])
    if opened:
        # The doors swung back: the dark inside, shelf lines, the doors'
        # edges standing proud at the sides, the lock's crystal gone dim.
        out[2:14, 3:13] = rgb("#0C0A12")
        out[7, 3:13] = rgb(WHISPERWOOD[1])
        out[11, 3:13] = rgb(WHISPERWOOD[1])
        out[2:14, 2] = rgb(WHISPERWOOD[3])
        out[2:14, 13] = rgb(WHISPERWOOD[3])
        out[3, 5] = ramp_at(CYAN_GLOW[:3], 0.3)
        return out
    # Two doors meeting at a resonite lock plate with a Stave-carved crystal.
    out[2:14, 7] = rgb(WHISPERWOOD[0])
    out[2:14, 8] = rgb(WHISPERWOOD[4])
    for x in (4, 11):
        out[3:13, x] = rgb(WHISPERWOOD[1])
    out[6:10, 6:10] = rgb(RESONITE[3])
    out[6, 6:10] = rgb(RESONITE[4])
    out[9, 6:10] = rgb(RESONITE[1])
    out[7, 7] = ramp_at(CYAN_GLOW, 0.7)
    out[7, 8] = ramp_at(CYAN_GLOW, 0.55)
    out[8, 7] = ramp_at(VIOLET_GLOW, 0.55)
    out[8, 8] = ramp_at(AMBER_GLOW, 0.55)
    for y in (3, 12):                              # hinges
        out[y, 2] = out[y, 13] = rgb(RESONITE[5])
    return out


def gen_choir_cabinet_front(rng):      return _cabinet_doors(rng, False)
def gen_choir_cabinet_front_open(rng): return _cabinet_doors(rng, True)


# ── reawakening the Heart: the items ────────────────────────────────────────
# The four voice keys and the Held Note, 16x16 item sprites. Each key is the
# same tool drawn in its voice's metal and crystal with its own bow and bit:
#   Soprano  pale steel, a thin ring with a spire (the high voice keeps watch),
#            one long slender tooth
#   Alto     sea-blue steel, a round bow set with a cyan crystal (the
#            warm voice welcomes), two teeth
#   Tenor    dusk-violet metal, a diamond bow round a star (the bright voice
#            carries), three stepped teeth
#   Bass     near-black resonite, a heavy square bow with an indigo gem (the
#            deep voice holds), a broad blocky bit
# The Held Note is the Chord's own note set solid: a crystal eighth note.
KEY_OUTLINE = "#0B0A10"
KEYS = {
    "soprano_voice_key": (["#3B4A5C", "#7E97AE", "#BFD6E6", "#F2FBFF"],
                          ["#7FD8F0", "#DFFFFF", "#FFFFFF"], "ring", 1),
    "alto_voice_key": (["#16303A", "#2F5868", "#5E8FA0", "#9CC8D4"],
                       ["#1C8FA6", "#5FF3FF", "#E4FFFF"], "round", 2),
    "tenor_voice_key": (["#2B1F3D", "#56407A", "#8A6CB8", "#B9A2E0"],
                        ["#6A3FB3", "#B77CFF", "#F2E6FF"], "diamond", 3),
    "bass_voice_key": (["#121119", "#242133", "#3E3958", "#5C5780"],
                       ["#2B2F8A", "#6F7BFF", "#C9CEFF"], "square", 4),
}


def _seg_dist(px, py, ax, ay, bx, by) -> float:
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - (ax + vx * t), py - (ay + vy * t))


def key_sprite(name: str) -> np.ndarray:
    metal_ramp, crystal_ramp, bow, bit = KEYS[name]
    # 0 empty, 1 metal, 2 crystal.
    grid = np.zeros((H, W), dtype=np.int64)
    bx, by = 3.5, 12.5              # the bit's end (bottom-left)
    cx, cy = 11.0, 4.8              # the bow's centre (top-right)
    shaft_w = {"ring": 0.75, "round": 0.9, "diamond": 0.9, "square": 1.25}[bow]
    for y in range(H):
        for x in range(W):
            px, py = x + 0.5, y + 0.5
            if _seg_dist(px, py, bx, by, cx - 2.0, cy + 2.0) <= shaft_w:
                grid[y, x] = 1
            dx, dy = px - cx, py - cy
            r = math.hypot(dx, dy)
            if bow == "ring":
                if 1.9 <= r <= 3.0:
                    grid[y, x] = 1
                if abs(dx) <= 0.6 and -4.6 <= dy <= -2.4:          # the spire
                    grid[y, x] = 2
                if r < 1.4:
                    grid[y, x] = 2
            elif bow == "round":
                if 2.2 <= r <= 3.6:
                    grid[y, x] = 1
                elif r < 2.2:
                    grid[y, x] = 2
            elif bow == "diamond":
                m = abs(dx) + abs(dy)
                if 2.4 <= m <= 4.2:
                    grid[y, x] = 1
                elif m < 2.4 and (abs(dx) < 0.7 or abs(dy) < 0.7):  # the star
                    grid[y, x] = 2
            else:
                if max(abs(dx), abs(dy)) <= 3.3:
                    grid[y, x] = 2 if max(abs(dx), abs(dy)) <= 1.5 else 1
    # The bit: teeth hanging off the shaft's lower end, square to it.
    ux, uy = (cx - bx), (cy - by)
    ln = math.hypot(ux, uy)
    ux, uy = ux / ln, uy / ln                  # along the shaft, toward the bow
    nx, ny = -uy, ux                           # perpendicular: down-right
    teeth = {1: [(0.0, 3.2)], 2: [(0.0, 2.4), (1.8, 2.0)], 3: [(0.0, 2.6), (1.5, 2.0), (3.0, 1.4)],
             4: [(0.0, 2.4), (0.9, 2.4), (1.8, 2.4)]}[bit]
    tooth_w = 0.55 if bit != 4 else 0.75
    for along, length in teeth:
        sx, sy = bx + ux * (0.6 + along), by + uy * (0.6 + along)
        ex, ey = sx + nx * length, sy + ny * length
        for y in range(H):
            for x in range(W):
                if _seg_dist(x + 0.5, y + 0.5, sx, sy, ex, ey) <= tooth_w:
                    grid[y, x] = max(grid[y, x], 1)
    out = blank()
    for y in range(H):
        for x in range(W):
            g = grid[y, x]
            if g == 0:
                continue
            up_left = (y == 0 or x == 0 or grid[y - 1, x] == 0 or grid[y, x - 1] == 0)
            down_right = (y == H - 1 or x == W - 1 or grid[y + 1, x] == 0 or grid[y, x + 1] == 0)
            if g == 1:
                shade = 2 if not (up_left or down_right) else (3 if up_left else 1)
                if (x + y) % 5 == 0 and shade == 2:
                    shade = 1
                out[y, x] = rgb(metal_ramp[shade])
            else:
                out[y, x] = rgb(crystal_ramp[2 if up_left else (0 if down_right else 1)])
    # Outline every empty cell that touches the sprite (MC item art's rim).
    rim = blank()
    for y in range(H):
        for x in range(W):
            if grid[y, x]:
                continue
            if any(0 <= y + dy < H and 0 <= x + dx < W and grid[y + dy, x + dx]
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                rim[y, x] = rgb(KEY_OUTLINE)
    mask = out[:, :, 3] == 0
    out[mask] = rim[mask]
    return out


def gen_held_note(rng: np.random.Generator) -> np.ndarray:
    # An eighth note in resonant crystal: the head a tilted oval, the stem,
    # a curling flag; cyan with a white heart and violet in the shadow, and
    # a glint on the head.
    grid = np.zeros((H, W), dtype=np.int64)
    hx, hy = 5.4, 11.3
    ang = math.radians(-24)
    ca, sa = math.cos(ang), math.sin(ang)
    for y in range(H):
        for x in range(W):
            px, py = x + 0.5 - hx, y + 0.5 - hy
            u, v = px * ca - py * sa, px * sa + py * ca
            if (u / 3.3) ** 2 + (v / 2.35) ** 2 <= 1.0:
                grid[y, x] = 2 if (u / 3.3) ** 2 + (v / 2.35) ** 2 <= 0.35 else 1
            if 7.4 <= x + 0.5 <= 9.0 and 2.0 <= y + 0.5 <= 11.0:        # the stem
                grid[y, x] = max(grid[y, x], 1)
    # The flag: a band curling off the stem's top to the right and down.
    for i in range(40):
        t = i / 39.0
        fx = 8.6 + 4.2 * math.sin(t * math.pi * 0.62)
        fy = 2.3 + 5.4 * t - 1.2 * math.sin(t * math.pi)
        for y in range(H):
            for x in range(W):
                if math.hypot(x + 0.5 - fx, y + 0.5 - fy) <= 0.95 - 0.35 * t:
                    grid[y, x] = max(grid[y, x], 1)
    ramp = ["#5A3FB3", "#2CC6DA", "#5FF3FF", "#A6FAFF", "#FFFFFF"]
    out = blank()
    for y in range(H):
        for x in range(W):
            g = grid[y, x]
            if g == 0:
                continue
            up_left = (y == 0 or x == 0 or grid[y - 1, x] == 0 or grid[y, x - 1] == 0)
            down_right = (y == H - 1 or x == W - 1 or grid[y + 1, x] == 0 or grid[y, x + 1] == 0)
            if g == 2:
                out[y, x] = rgb(ramp[4] if (x + y) % 3 else ramp[3])
            else:
                out[y, x] = rgb(ramp[3] if up_left else (ramp[0] if down_right else ramp[2]))
    out[9, 3] = rgb("#FFFFFF")                           # the glint
    rim = blank()
    for y in range(H):
        for x in range(W):
            if grid[y, x]:
                continue
            if any(0 <= y + dy < H and 0 <= x + dx < W and grid[y + dy, x + dx]
                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                rim[y, x] = rgb("#0E0A1C")
    mask = out[:, :, 3] == 0
    out[mask] = rim[mask]
    return out


QUEST_TEXTURES = {
    "dim_cyan_lumen_panel": (gen_dim_cyan_lumen_panel, anim(6, True)),
    "dim_violet_lumen_panel": (gen_dim_violet_lumen_panel, anim(6, True)),
    "dim_amber_lumen_panel": (gen_dim_amber_lumen_panel, anim(6, True)),
    "dim_stave_stone": (gen_dim_stave_stone, anim(9, True)),
    "dim_lumen_strip": (gen_dim_lumen_strip, anim(5, True)),
    "dim_lumen_strip_top": (gen_dim_lumen_strip_top, None),
    "dim_choir_lamp": (gen_dim_choir_lamp, None),
    "chord_socket_post": (gen_chord_socket_post, None),
    "chord_socket_cradle": (gen_chord_socket_cradle, None),
    "chord_socket_top": (gen_chord_socket_top, None),
    "voice_pedestal_side": (gen_voice_pedestal_side, None),
    "voice_pedestal_top": (gen_voice_pedestal_top, None),
    "choir_cabinet_front": (gen_choir_cabinet_front, None),
    "choir_cabinet_front_open": (gen_choir_cabinet_front_open, None),
    "choir_cabinet_side": (gen_choir_cabinet_side, None),
    "choir_cabinet_back": (gen_choir_cabinet_back, None),
    "choir_cabinet_top": (gen_choir_cabinet_top, None),
    # Item sprites (written under assets/textures/item/).
    "item/held_note": (gen_held_note, None),
}
for _key in KEYS:
    QUEST_TEXTURES[f"item/{_key}"] = ((lambda rng, _n=_key: key_sprite(_n)), None)
QUEST_GLOWING = {"dim_cyan_lumen_panel", "dim_violet_lumen_panel", "dim_amber_lumen_panel",
                 "dim_stave_stone", "dim_lumen_strip", "dim_lumen_strip_top", "dim_choir_lamp"}


TEXTURES = {
    "choirstone": (gen_choirstone, None),
    "polished_choirstone": (gen_polished_choirstone, None),
    "choirstone_bricks": (gen_choirstone_bricks, None),
    "cracked_choirstone_bricks": (gen_cracked_choirstone_bricks, None),
    "chiseled_choirstone": (gen_chiseled_choirstone, None),
    "choirstone_tiles": (gen_choirstone_tiles, None),
    "choirstone_pillar": (gen_choirstone_pillar, None),
    "choirstone_pillar_top": (gen_choirstone_pillar_top, None),
    "stave_stone": (gen_stave_stone, anim(6, True)),
    "nightglass": (gen_nightglass, None),
    "resonite_grate": (gen_resonite_grate, None),
    "cyan_lumen_panel": (gen_cyan_lumen_panel, anim(4, True)),
    "violet_lumen_panel": (gen_violet_lumen_panel, anim(4, True)),
    "amber_lumen_panel": (gen_amber_lumen_panel, anim(4, True)),
    "lumen_strip": (gen_lumen_strip, anim(3, True)),
    "lumen_strip_top": (gen_lumen_strip_top, None),
    "crystal_conduit": (gen_crystal_conduit, None),
    "choir_lamp": (gen_choir_lamp, None),
    "resonant_water_still": (gen_resonant_water_still, anim(2)),
    "resonant_water_flow": (gen_resonant_water_flow, {"animation": {}}),
    "resonance_engine_side": (gen_resonance_engine_side, anim(5, True)),
    "resonance_engine_top": (gen_resonance_engine_top, anim(5, True)),
    "voice_beacon_lens": (gen_voice_beacon_lens, None),
    "voice_beacon_side": (gen_voice_beacon_side, None),
    "voice_beacon_top": (gen_voice_beacon_top, None),
}
# The flickering windows (their schedules are per block, see above).
for _window in WINDOWS:
    TEXTURES[_window] = ((lambda rng, _n=_window: window_strip(_n)), window_meta(_window))
# Reawakening the Heart: the dormant lights, the quest's blocks and items.
TEXTURES.update(QUEST_TEXTURES)


def png_bytes(arr: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def render(name: str) -> tuple[np.ndarray, bytes, bytes | None]:
    gen, meta = TEXTURES[name]
    arr = gen(rng_for(name))
    assert arr.dtype == np.uint8 and arr.ndim == 3 and arr.shape[2] == 4, name
    w = arr.shape[1]
    assert w in (16, 32) and arr.shape[0] % w == 0, (name, arr.shape)
    assert (meta is not None) == (arr.shape[0] > w), f"{name}: strip/mcmeta mismatch"
    meta_bytes = None if meta is None else (json.dumps(meta, indent=2) + "\n").encode("utf-8")
    return arr, png_bytes(arr), meta_bytes


def main_face_colour(arr: np.ndarray) -> str:
    """Mean colour of the first frame's opaque texels, as #RRGGBB."""
    w = arr.shape[1]
    f = arr[:w].reshape(-1, 4).astype(np.float64)
    f = f[f[:, 3] > 0]
    r, g, b = (int(round(v)) for v in f[:, :3].mean(axis=0))
    return f"#{r:02X}{g:02X}{b:02X}"


def preview(names: list[str], outpath: Path) -> None:
    scale, tile = 8, 3
    cell = 16 * scale
    cols = 5
    pitch_x, pitch_y = cell + 3 * 16 * tile // 2 + 24, cell + 40
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * pitch_x + 16, rows * pitch_y + 24), (34, 36, 40, 255))
    night = Image.new("RGBA", sheet.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    for i, name in enumerate(names):
        arr, _, _ = render(name)
        w = arr.shape[1]
        first = Image.fromarray(arr[:w], "RGBA").resize((cell, cell), Image.NEAREST)
        x0, y0 = 16 + (i % cols) * pitch_x, 24 + (i // cols) * pitch_y
        # Checker behind so alpha reads.
        chk = Image.new("RGBA", (cell, cell), (90, 90, 90, 255))
        cd = ImageDraw.Draw(chk)
        for cy in range(0, cell, 16):
            for cx in range(0, cell, 16):
                if (cx // 16 + cy // 16) % 2:
                    cd.rectangle([cx, cy, cx + 15, cy + 15], fill=(60, 60, 60, 255))
        chk.alpha_composite(first)
        sheet.paste(chk, (x0, y0))
        # 3x3 tiling at the right, dimmed to the Hush's night level unless it
        # glows (so the sheet shows what a wall of it looks like at night).
        glows = name in GLOWING
        tiled = Image.new("RGBA", (w * 3, w * 3), (0, 0, 0, 255))
        src = Image.fromarray(arr[:w], "RGBA")
        for tx in range(3):
            for ty in range(3):
                tiled.alpha_composite(src, (tx * w, ty * w))
        if not glows:
            a = np.array(tiled).astype(np.float64)
            a[:, :, :3] *= 0.27
            tiled = Image.fromarray(a.astype(np.uint8), "RGBA")
        tsz = 16 * tile * 3 // 2
        sheet.paste(tiled.resize((tsz, tsz), Image.NEAREST), (x0 + cell + 8, y0))
        label = f"{name}  {main_face_colour(arr)}" + (f"  x{arr.shape[0] // w}" if arr.shape[0] > w else "")
        draw.text((x0, y0 + cell + 4), label, fill=(235, 235, 235, 255))
    del night
    sheet.save(outpath)
    print(f"preview -> {outpath}")


# Textures whose blocks are emissive (drawn undimmed at night).
GLOWING = {"stave_stone", "cyan_lumen_panel", "violet_lumen_panel", "amber_lumen_panel",
           "lumen_strip", "lumen_strip_top", "crystal_conduit", "choir_lamp",
           "resonant_water_still", "resonant_water_flow", "resonance_engine_side",
           "resonance_engine_top", "voice_beacon_lens", "voice_beacon_side", "voice_beacon_top"}
GLOWING |= set(WINDOWS)
GLOWING |= QUEST_GLOWING


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated texture names")
    ap.add_argument("--outdir", type=Path, default=TEX_DIR, help="block texture dir to write")
    ap.add_argument("--preview", metavar="PNG", help="write a contact sheet instead of the textures")
    ap.add_argument("--check", action="store_true",
                    help="regenerate in memory and byte-compare against --outdir; exit 1 on drift")
    ap.add_argument("--colours", action="store_true", help="print each texture's mean face colour")
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
    if args.colours:
        for name in names:
            print(f"{name:28s} {main_face_colour(render(name)[0])}")
        return 0

    def rel(p: Path) -> str:
        return str(p.relative_to(REPO_ROOT)) if p.is_relative_to(REPO_ROOT) else str(p)

    drift = 0
    for name in names:
        _, data, meta = render(name)
        # "item/<name>" textures are item sprites: written beside the block
        # directory, under textures/item/.
        base = args.outdir.parent / "item" if name.startswith("item/") else args.outdir
        stem = name[len("item/"):] if name.startswith("item/") else name
        targets = [(base / f"{stem}.png", data)]
        if meta is not None:
            targets.append((base / f"{stem}.png.mcmeta", meta))
        for path, blob in targets:
            if args.check:
                if not path.exists():
                    print(f"MISSING  {rel(path)}")
                    drift += 1
                elif path.read_bytes() != blob:
                    print(f"DRIFT    {rel(path)}")
                    drift += 1
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
