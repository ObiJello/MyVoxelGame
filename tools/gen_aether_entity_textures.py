#!/usr/bin/env python3
"""In-house textures for the Aether's pass-two creatures (docs/mod-ports.md).

LICENCE: The Aether's art is all rights reserved. Nothing here copies, traces
or samples it — every sheet is painted procedurally from scratch. The mod's
LGPL model code (client/renderer/entity/model/*.java) was read only for the
UV layout: each box's texOffs and size decide where a face's pixels live,
the same contract MC's ModelPart cube UVs follow. Pass one's Aether sheets
(phyg, cockatrice, zephyr, ...) belong to tools/gen_aether_textures.py.

Writes assets/textures/entity/aether/:
  moa_blue / moa_white / moa_black   MoaModel (BipedBirdModel, 64x32 UV space
                                     painted at 2 px per unit: 128x64)
  moa_saddle / black_moa_saddle      MOA_SADDLE (the same layout, inflated)
  flying_cow_saddle                  the engine's CowModel layout (64x64)
  aerbunny                           AerbunnyModel (64x32)
  aerwhale                           AerwhaleModel (256x128)
  swet_blue / swet_golden            SlimeModel inner + outer (64x32; the
                                     shell rows translucent)
  aechor_plant                       AechorPlantModel (64x32)
  mimic                              MimicModel (128x64)
  sentry / sentry_lit / sentry_eye   SlimeModel outer (64x32)
  valkyrie                           ValkyrieModel + ValkyrieWingsModel — the
                                     wings' texOffs (24, 31) run past a 32-row
                                     sheet, so the sheet is 64x64 and both
                                     models read it at height 64
  fire_minion                        SunSpiritModel (64x64)

    python3 tools/gen_aether_entity_textures.py            # write
    python3 tools/gen_aether_entity_textures.py --check    # byte-compare, exit 1 on drift
    python3 tools/gen_aether_entity_textures.py --preview out.png   # contact sheet

Deterministic: every sheet draws from default_rng([0x41455432, crc32(name)]).
"""

from __future__ import annotations

import argparse
import io
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "assets" / "textures" / "entity" / "aether"
SEED_TAG = 0x41455432  # "AET2"


# ── helpers ──────────────────────────────────────────────────────────────────
def rng_for(name: str) -> np.random.Generator:
    return np.random.default_rng([SEED_TAG, zlib.crc32(name.encode())])


def rgb(h: str, a: int = 255) -> tuple[int, int, int, int]:
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(a, b, t: float):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] * (1 - t) + b[i] * t)) for i in range(4))


def shade(c, f: float):
    return (max(0, min(255, int(round(c[0] * f)))), max(0, min(255, int(round(c[1] * f)))),
            max(0, min(255, int(round(c[2] * f)))), c[3])


def alpha(c, a: int):
    return (c[0], c[1], c[2], a)


def cube_rects(u, v, dx, dy, dz):
    """MC ModelPart.Cube UV rects (u0, v0, u1, v1) per face, in UV units."""
    return {
        "top": (u + dz, v, u + dz + dx, v + dz),
        "bottom": (u + dz + dx, v, u + dz + dx + dx, v + dz),
        "west": (u, v + dz, u + dz, v + dz + dy),
        "front": (u + dz, v + dz, u + dz + dx, v + dz + dy),
        "east": (u + dz + dx, v + dz, u + dz + dx + dz, v + dz + dy),
        "back": (u + dz + dx + dz, v + dz, u + dz + dx + dz + dx, v + dz + dy),
    }


class Sheet:
    """An RGBA sheet in UV units painted at `scale` pixels per unit."""

    def __init__(self, name: str, w: int, h: int, scale: int = 1):
        self.rng = rng_for(name)
        self.s = scale
        self.img = np.zeros((h * scale, w * scale, 4), dtype=np.uint8)
        self.noise = self.rng.random(self.img.shape[:2])
        self.noise2 = self.rng.random(self.img.shape[:2])
        # Blob noise: white noise box-blurred twice (3x3), renormalised.
        b = self.rng.random(self.img.shape[:2])
        for _ in range(2):
            p = np.pad(b, 1, mode="wrap")
            b = sum(p[1 + dy:1 + dy + b.shape[0], 1 + dx:1 + dx + b.shape[1]]
                    for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0
        self.blob = (b - b.min()) / max(1e-9, b.max() - b.min())

    def rect(self, r, fn):
        """fn(s, t, px, py) -> RGBA, s/t in 0..1 across the rect."""
        u0, v0, u1, v1 = (int(round(c * self.s)) for c in r)
        hh, ww = self.img.shape[:2]
        for py in range(max(0, v0), min(hh, v1)):
            for px in range(max(0, u0), min(ww, u1)):
                s = (px - u0 + 0.5) / max(1, u1 - u0)
                t = (py - v0 + 0.5) / max(1, v1 - v0)
                col = fn(s, t, px, py)
                if col is not None:
                    self.img[py, px] = col

    def box(self, u, v, dx, dy, dz, fn):
        """fn(face, s, t, px, py) over every face of a box."""
        for face, r in cube_rects(u, v, dx, dy, dz).items():
            self.rect(r, lambda s, t, px, py, f=face: fn(f, s, t, px, py))

    def n(self, px, py):
        return self.noise[py, px]

    def n2(self, px, py):
        return self.noise2[py, px]

    def nb(self, px, py):
        return self.blob[py, px]


def feathers(sheet: Sheet, pal, dark, light, rows=3):
    """Overlapping feather scallops (pal: base colour)."""
    def fn(s, t, px, py):
        row = py // rows
        fx = (px + (2 if row % 2 else 0)) % 4
        fy = py % rows
        c = pal
        if fy == rows - 1 or fx == 3:
            c = dark
        elif fy == 0 and fx in (1, 2):
            c = light
        return shade(c, 0.94 + 0.12 * sheet.n(px, py))
    return fn


# ── moa ──────────────────────────────────────────────────────────────────────
MOA_PALETTES = {
    # base, dark, light, wing, wing_dark, beak
    "blue": ("#4f86cf", "#35609e", "#7eaee6", "#3c6db4", "#264c86", "#d9cfa6"),
    "white": ("#eceff3", "#c9d0da", "#ffffff", "#dde3ea", "#b3bdc9", "#d6c48a"),
    "black": ("#34303f", "#211e29", "#54506a", "#2b2735", "#18161f", "#a89060"),
}


def moa_body(name: str, kind: str) -> np.ndarray:
    base, dark, light, wing, wing_dark, beak = (rgb(c) for c in MOA_PALETTES[kind])
    sh = Sheet(name, 64, 32, 2)
    eye_white, eye = rgb("#f4f1e6"), rgb("#15131a")
    leg, leg_dark, claw = rgb("#6d6a62"), rgb("#55524b"), rgb("#2e2c28")
    body = feathers(sh, base, dark, light)
    sh.box(0, 0, 6, 8, 5, lambda f, s, t, px, py: body(s, t, px, py))      # body
    sh.box(22, 0, 2, 6, 2, lambda f, s, t, px, py: body(s, t, px, py))     # neck

    def head(f, s, t, px, py):
        if f == "top":
            return shade(dark if (px // 2 + py // 2) % 3 == 0 else base, 0.95 + 0.1 * sh.n(px, py))
        if f in ("west", "east"):
            fs = s if f == "west" else 1 - s          # 1 = the beak end
            if 0.66 < fs < 0.86 and 0.25 < t < 0.7:
                if 0.72 < fs < 0.82 and 0.35 < t < 0.6:
                    return eye
                return eye_white
            if fs > 0.9 and t > 0.55:
                return beak
            return body(s, t, px, py)
        if f == "front":
            return beak if t > 0.45 else shade(base, 1.05)
        return body(s, t, px, py)
    sh.box(0, 13, 4, 4, 8, head)

    def jaw(f, s, t, px, py):
        return shade(beak, 0.85 if (py % 2) else 1.0)
    sh.box(24, 13, 4, 1, 8, jaw)

    def wings(f, s, t, px, py):
        c = wing if (px // 2) % 2 == 0 else wing_dark
        if t > 0.8:
            c = shade(c, 0.8)
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    sh.box(40, 0, 1, 8, 4, wings)
    sh.box(30, 0, 1, 8, 4, wings)

    def legs(f, s, t, px, py):
        if t > 0.86:
            return claw
        return leg if (py // 2) % 2 == 0 else leg_dark
    sh.box(54, 21, 2, 9, 2, legs)
    sh.box(46, 21, 2, 9, 2, legs)

    def tail(f, s, t, px, py):
        c = wing_dark if (px // 2) % 2 else wing
        if t > 0.75 or (f in ("top", "bottom") and s > 0.0 and t > 0.7):
            c = shade(light, 0.95)
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    for u in (0, 14, 28):
        sh.box(u, 26, 2, 1, 5, tail)
    return sh.img


def moa_saddle(name: str, leather: str, trim: str, stud: str) -> np.ndarray:
    # MOA_SADDLE: MoaModel inflated 0.27 — only the body carries paint; the
    # saddle sits across the body's back (its front face, the body lying
    # along +z) with straps down both flanks.
    sh = Sheet(name, 64, 32, 2)
    lea, lea_dark, tri, stu = rgb(leather), shade(rgb(leather), 0.72), rgb(trim), rgb(stud)
    r = cube_rects(0, 0, 6, 8, 5)

    def seat(s, t, px, py):
        if 0.25 < t < 0.8:
            edge = t < 0.32 or t > 0.73 or s < 0.1 or s > 0.9
            if edge:
                return tri
            if (px + py) % 7 == 0:
                return stu
            return shade(lea, 0.9 + 0.2 * sh.n(px, py))
        return None
    sh.rect(r["front"], seat)

    def strap(s, t, px, py):
        if 0.42 < s < 0.62:
            return lea_dark if t < 0.85 else stu
        return None
    sh.rect(r["west"], strap)
    sh.rect(r["east"], strap)
    return sh.img


# ── flying cow saddle (engine CowModel, 64x64) ───────────────────────────────
def gen_flying_cow_saddle(name: str) -> np.ndarray:
    sh = Sheet(name, 64, 64)
    lea, lea_dark, trim, gold = rgb("#7a4a2a"), rgb("#553219"), rgb("#c9b37a"), rgb("#e8c85a")
    r = cube_rects(18, 4, 12, 18, 10)          # the body, lying along its length

    def seat(s, t, px, py):
        if 0.3 < t < 0.72:
            if t < 0.36 or t > 0.66 or s < 0.08 or s > 0.92:
                return trim
            if (px * 3 + py) % 9 == 0:
                return gold
            return shade(lea, 0.9 + 0.2 * sh.n(px, py))
        return None
    sh.rect(r["front"], seat)

    def strap(s, t, px, py):
        if 0.46 < t < 0.58:
            return lea_dark if s < 0.85 else gold
        return None
    sh.rect(r["west"], strap)
    sh.rect(r["east"], strap)
    return sh.img


# ── aerbunny ─────────────────────────────────────────────────────────────────
def gen_aerbunny(name: str) -> np.ndarray:
    sh = Sheet(name, 64, 32)
    white, cream, shadow = rgb("#fbfbfd"), rgb("#eeeaf2"), rgb("#d7d3e0")
    pink, eye, nose = rgb("#f2b6c6"), rgb("#2a2238"), rgb("#e98aa3")

    def fur(s, t, px, py):
        n = sh.n(px, py)
        return white if n > 0.55 else (cream if n > 0.18 else shadow)

    def head(f, s, t, px, py):
        if f == "front":
            if t > 0.25 and t < 0.55 and (0.08 < s < 0.3 or 0.7 < s < 0.92):
                return eye
            if 0.4 < s < 0.6 and 0.6 < t < 0.8:
                return nose
        return fur(s, t, px, py)
    sh.box(0, 0, 4, 4, 6, head)
    sh.box(14, 0, 1, 4, 2, lambda f, s, t, px, py:
           pink if (f in ("front", "back") and 0.2 < t < 0.9) else fur(s, t, px, py))   # ears
    sh.box(20, 0, 2, 3, 2, lambda f, s, t, px, py: cream if (px + py) % 2 else white)  # whiskers
    sh.box(0, 10, 6, 8, 6, lambda f, s, t, px, py: fur(s, t, px, py))                  # body

    def puff(f, s, t, px, py):
        # Big soft cloud of fur: lighter towards the face centres.
        d = ((s - 0.5) ** 2 + (t - 0.5) ** 2) ** 0.5
        c = mix(white, shadow, d * 1.1 + 0.25 * sh.n(px, py))
        return c
    sh.box(29, 0, 7, 7, 7, puff)
    sh.box(0, 24, 4, 3, 4, lambda f, s, t, px, py: white)                              # tail
    sh.box(24, 16, 2, 2, 2, lambda f, s, t, px, py: shade(cream, 0.95))                # front legs
    sh.box(16, 24, 2, 2, 4, lambda f, s, t, px, py: shade(cream, 0.95))                # back legs
    return sh.img


# ── aerwhale (256x128) ───────────────────────────────────────────────────────
def gen_aerwhale(name: str) -> np.ndarray:
    sh = Sheet(name, 256, 128)
    top, mid, belly = rgb("#5e7fa8"), rgb("#86a6c9"), rgb("#dfe9f3")
    spot, dark, eye = rgb("#b9cde2"), rgb("#41597a"), rgb("#1b2230")

    def hide(f, s, t, px, py):
        n = sh.n(px, py)
        b = sh.nb(px, py)
        if f == "top":
            c = mix(top, dark, max(0.0, 0.45 - b))
            if n > 0.985:
                c = spot
        elif f == "bottom":
            c = belly
            if (py % 3 == 0) and n > 0.4:
                c = shade(belly, 0.93)           # throat grooves
        else:
            c = mix(top, belly, max(0.0, t - 0.25) * 1.35)
            if n > 0.985 and t < 0.6:
                c = spot
        return shade(c, 0.96 + 0.08 * sh.n2(px, py))

    def head_big(f, s, t, px, py):
        c = hide(f, s, t, px, py)
        if f in ("west", "east"):
            fs = s if f == "west" else 1 - s      # 1 = the nose end
            if 0.08 < fs < 0.2 and 0.45 < t < 0.6:
                return eye
        if f == "front" and t > 0.75 and 0.1 < s < 0.9 and py % 2 == 0:
            return shade(belly, 0.85)             # the mouth line
        return c

    sh.box(104, 36, 24, 6, 26, hide)
    sh.box(104, 0, 26, 6, 30, hide)
    sh.box(0, 0, 24, 18, 28, head_big)
    sh.box(0, 46, 2, 7, 8, lambda f, s, t, px, py: shade(dark, 0.95 + 0.1 * sh.n(px, py)))
    sh.box(0, 46, 22, 14, 25, hide)

    def fin(f, s, t, px, py):
        c = dark if f == "top" else mix(dark, mid, t)
        if f in ("front", "back", "west", "east") and s > 0.85:
            c = shade(c, 0.8)
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    sh.box(104, 94, 19, 3, 14, fin)
    sh.box(170, 84, 15, 3, 24, fin)
    sh.box(104, 68, 19, 5, 21, hide)
    sh.box(0, 85, 17, 9, 22, hide)
    return sh.img


# ── swets (SlimeModel layout, 64x32) ─────────────────────────────────────────
def swet(name: str, shell: str, core: str, deep: str) -> np.ndarray:
    sh = Sheet(name, 64, 32)
    shl, cor, dp = rgb(shell), rgb(core), rgb(deep)
    eye, glint = rgb("#10223a") if "blue" in name else rgb("#3a2408"), rgb("#ffffff")

    def outer(f, s, t, px, py):
        d = ((s - 0.5) ** 2 + (t - 0.5) ** 2) ** 0.5
        a = int(120 + 70 * d + 20 * sh.n(px, py))
        c = mix(shl, dp, d * 0.8)
        if f == "top" and s < 0.4 and t < 0.4 and sh.n2(px, py) > 0.4:
            c = mix(c, glint, 0.45)
        if f in ("front", "west", "east", "back") and t < 0.2 and s < 0.35:
            c = mix(c, glint, 0.5)
        return alpha(c, min(235, a))
    sh.box(0, 0, 8, 8, 8, outer)

    def inner(f, s, t, px, py):
        d = ((s - 0.5) ** 2 + (t - 0.5) ** 2) ** 0.5
        return mix(cor, dp, d + 0.15 * sh.n(px, py))
    sh.box(0, 16, 6, 6, 6, inner)
    sh.box(32, 0, 2, 2, 2, lambda f, s, t, px, py: glint if (f == "front" and s < 0.5 and t < 0.5) else eye)
    sh.box(32, 4, 2, 2, 2, lambda f, s, t, px, py: glint if (f == "front" and s < 0.5 and t < 0.5) else eye)
    sh.box(32, 8, 1, 1, 1, lambda f, s, t, px, py: eye)
    return sh.img


# ── aechor plant (64x32) ─────────────────────────────────────────────────────
def gen_aechor_plant(name: str) -> np.ndarray:
    sh = Sheet(name, 64, 32)
    petal, petal_pink, petal_deep = rgb("#fbf3f7"), rgb("#f3b6d0"), rgb("#d9709f")
    leaf, leaf_dark, leaf_light = rgb("#5aa64a"), rgb("#3c7a34"), rgb("#86c96a")
    stem, thorn = rgb("#4f8f3e"), rgb("#2f3a22")
    centre, centre_dot, tip = rgb("#e9d86a"), rgb("#b89c3a"), rgb("#ffe46b")

    def upper(f, s, t, px, py):
        # Petal length runs along z: t = 0 at the tip on the broad faces.
        tipness = 1.0 - t if f in ("top", "bottom") else 0.5
        c = mix(petal, petal_pink, max(0.0, tipness - 0.45) * 1.8)
        if f in ("top", "bottom") and abs(s - 0.5) < 0.07:
            c = mix(c, petal_deep, 0.35)          # the central vein
        return shade(c, 0.97 + 0.06 * sh.n(px, py))
    sh.box(28, 2, 8, 1, 10, upper)

    def lower(f, s, t, px, py):
        tipness = 1.0 - t if f in ("top", "bottom") else 0.5
        c = mix(petal_pink, petal_deep, max(0.0, tipness - 0.3) * 1.3)
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    sh.box(0, 0, 8, 1, 10, lower)

    def leaves(f, s, t, px, py):
        c = leaf if (px + py) % 3 else leaf_light
        if f in ("top", "bottom") and abs(s - 0.5) < 0.13:
            c = leaf_dark
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    sh.box(38, 13, 4, 1, 8, leaves)
    sh.box(24, 13, 2, 6, 2, lambda f, s, t, px, py: shade(stem, 0.9 + 0.2 * sh.n(px, py)))

    def head(f, s, t, px, py):
        if f == "top":
            return centre_dot if (px + 2 * py) % 3 == 0 else centre
        return shade(leaf_dark, 0.95 + 0.1 * sh.n(px, py))
    sh.box(0, 12, 6, 2, 6, head)
    sh.box(32, 13, 1, 1, 1, lambda f, s, t, px, py: thorn)
    sh.box(36, 13, 1, 6, 1, lambda f, s, t, px, py: leaf_light)
    sh.box(32, 15, 1, 1, 1, lambda f, s, t, px, py: tip)
    return sh.img


# ── mimic (128x64) ───────────────────────────────────────────────────────────
def gen_mimic(name: str) -> np.ndarray:
    sh = Sheet(name, 128, 64)
    plank, plank_dark, rim = rgb("#9c6a36"), rgb("#7a4f24"), rgb("#4a2f15")
    iron, mouth, gum, tooth = rgb("#a9a9ad"), rgb("#6b1320"), rgb("#9c2a3a"), rgb("#f1ead6")
    gold = rgb("#e7c142")

    def wood(s, t, px, py, border=True):
        if border and (s < 0.07 or s > 0.93 or t < 0.08 or t > 0.92):
            return rim
        c = plank if (py // 4) % 2 == 0 else plank_dark
        if sh.n(px, py) > 0.92:
            c = shade(c, 0.85)
        return shade(c, 0.95 + 0.1 * sh.n2(px, py))

    def lid(f, s, t, px, py):
        # upper_body is flipped half a turn: its BOTTOM face is the inside of
        # the lid — the roof of the mouth, fringed with teeth.
        if f == "bottom":
            if s < 0.12 or s > 0.88 or t < 0.12 or t > 0.88:
                return tooth if (px // 2 + py // 2) % 2 == 0 else gum
            return mouth if sh.n(px, py) > 0.2 else gum
        if f in ("front", "back", "west", "east") and t > 0.65:
            # the lid's lip, teeth hanging over the edge
            return tooth if (px // 2) % 2 == 0 and t > 0.8 else wood(s, t, px, py)
        return wood(s, t, px, py)
    sh.box(0, 10, 16, 6, 16, lid)

    def base(f, s, t, px, py):
        if f == "top":
            if s < 0.12 or s > 0.88 or t < 0.12 or t > 0.88:
                return tooth if (px // 2 + py // 2) % 2 == 0 else gum
            return mouth if sh.n(px, py) > 0.25 else gum
        if f in ("front", "back", "west", "east") and t < 0.2:
            return tooth if (px // 2) % 2 == 1 and t < 0.12 else wood(s, t, px, py)
        if f in ("front", "back", "west", "east") and 0.45 < t < 0.55:
            return iron
        return wood(s, t, px, py)
    sh.box(0, 38, 16, 10, 16, base)

    def leg(f, s, t, px, py):
        c = plank_dark if (py // 3) % 2 else plank
        if t > 0.9:
            return rim
        return shade(c, 0.9 + 0.15 * sh.n(px, py))
    sh.box(64, 0, 6, 15, 6, leg)
    sh.box(0, 0, 2, 4, 1, lambda f, s, t, px, py: gold if sh.n(px, py) > 0.2 else shade(gold, 0.8))
    return sh.img


# ── sentry (SlimeModel outer, 64x32) ─────────────────────────────────────────
def sentry(name: str, lit: bool, eye_only: bool) -> np.ndarray:
    sh = Sheet(name, 64, 32)
    stone, stone_dark, groove = rgb("#8f949a"), rgb("#6f7479"), rgb("#4a4e52")
    glow, glow_core = rgb("#6fd3ff"), rgb("#e8fbff")
    socket = rgb("#1d2024")

    def body(f, s, t, px, py):
        # Carved stone: a border groove and a cross-hatched inlay.
        on_groove = (0.1 < s < 0.16 or 0.84 < s < 0.9 or 0.1 < t < 0.16 or 0.84 < t < 0.9)
        is_eye = f == "front" and 0.3 < s < 0.7 and 0.3 < t < 0.7
        if eye_only:
            if is_eye:
                d = ((s - 0.5) ** 2 + (t - 0.5) ** 2) ** 0.5
                return glow_core if d < 0.1 else glow
            return (0, 0, 0, 0)
        if is_eye:
            if lit:
                d = ((s - 0.5) ** 2 + (t - 0.5) ** 2) ** 0.5
                return glow_core if d < 0.1 else glow
            return socket if 0.36 < s < 0.64 and 0.36 < t < 0.64 else groove
        if on_groove:
            return mix(groove, glow, 0.45) if lit else groove
        c = stone if sh.n(px, py) > 0.3 else stone_dark
        return shade(c, 0.95 + 0.1 * sh.n2(px, py))
    sh.box(0, 0, 8, 8, 8, body)
    return sh.img


# ── valkyrie (64x64: ValkyrieModel in the top 32 rows, wings below) ──────────
def gen_valkyrie(name: str) -> np.ndarray:
    sh = Sheet(name, 64, 64)
    skin, skin_dark = rgb("#f0cfae"), rgb("#d9b08c")
    hair, hair_dark = rgb("#f2d879"), rgb("#caa845")
    steel, steel_dark, steel_light = rgb("#c3cad3"), rgb("#8d96a3"), rgb("#eef2f6")
    cloth, cloth_dark = rgb("#f6f6fa"), rgb("#d6d8e4")
    gold, eye, lip = rgb("#e3bd4f"), rgb("#3a78c8"), rgb("#c98579")
    white, feather_dark = rgb("#ffffff"), rgb("#d9dfe8")

    def head(f, s, t, px, py):
        if f == "top" or f == "back":
            return hair if (px + py) % 3 else hair_dark
        if f in ("west", "east"):
            return hair if t < 0.75 or (s > 0.5) == (f == "west") else skin
        if f == "bottom":
            return skin_dark
        # front: fringe, eyes, mouth
        if t < 0.25:
            return hair
        if 0.4 < t < 0.55 and (0.15 < s < 0.35 or 0.65 < s < 0.85):
            return eye if (0.2 < s < 0.3 or 0.7 < s < 0.8) else white
        if 0.72 < t < 0.8 and 0.4 < s < 0.6:
            return lip
        return skin
    sh.box(0, 0, 8, 8, 8, head)

    def armour(f, s, t, px, py):
        if t < 0.08 or t > 0.92:
            return gold
        c = steel if (py // 2) % 3 else steel_dark
        if f == "front" and abs(s - 0.5) < 0.08:
            c = steel_light
        return shade(c, 0.95 + 0.1 * sh.n(px, py))
    sh.box(12, 16, 6, 12, 3, armour)

    def arm(f, s, t, px, py):
        if t < 0.3:
            return steel if (px + py) % 2 else steel_light     # pauldron
        if 0.72 < t < 0.8:
            return gold                                        # bracer
        return skin if t < 0.72 else steel_dark
    sh.box(30, 16, 3, 12, 3, arm)

    def leg(f, s, t, px, py):
        if t > 0.78:
            return steel_dark                                  # greaves
        return cloth if (py // 2) % 2 else cloth_dark
    sh.box(0, 16, 3, 12, 3, leg)
    # The skirt panels sit in the head sheet's unused corner (texOffs 0, 0).
    sh.box(0, 0, 3, 6, 1, lambda f, s, t, px, py: cloth if (px % 2) else cloth_dark)
    sh.box(55, 19, 1, 6, 3.01, lambda f, s, t, px, py: cloth if (py % 2) else cloth_dark)
    # Hair strands (42..49, 17..28), drawn before the blade's faces.
    sh.rect((42, 17, 50, 28), lambda s, t, px, py: hair if (px + py // 2) % 3 else hair_dark)
    # Sword: grip, crossguard, blade.
    sh.box(9, 16, 2, 2, 1, lambda f, s, t, px, py: rgb("#6b4a2a"))
    sh.box(32, 10, 3, 5, 1, lambda f, s, t, px, py: gold)
    blade = cube_rects(42, 18, 1, 3, 10)
    for k in ("top", "bottom", "front", "east", "back"):
        sh.rect(blade[k], lambda s, t, px, py: steel_light if (px + py) % 3 else steel)
    sh.rect(blade["west"], lambda s, t, px, py: steel_light if (px + py) % 3 else steel)
    sh.box(28, 17, 1, 1, 1, lambda f, s, t, px, py: steel_light)
    # Wings (ValkyrieWingsModel, texOffs 24, 31; 19 x 8 x 1).

    def wing(f, s, t, px, py):
        col = int(s * 19)
        c = white if (col % 2 == 0) else feather_dark
        if t > 0.7:
            c = shade(c, 0.92)
        return c
    sh.box(24, 31, 19, 8, 1, wing)
    return sh.img


# ── fire minion (SunSpiritModel, 64x64) ──────────────────────────────────────
def gen_fire_minion(name: str) -> np.ndarray:
    sh = Sheet(name, 64, 64)
    ember = [rgb(c) for c in ("#5a1406", "#a82a0a", "#e8520e", "#ff8a1c", "#ffc53a", "#fff1a0")]
    eye = rgb("#fff8d8")

    def lava(f, s, t, px, py, heat=0.0):
        n = sh.nb(px, py) * 0.8 + sh.n(px, py) * 0.2
        # Dark crust over a glowing body; hotter towards the top.
        k = n * 0.85 + (1.0 - t) * 0.3 + heat
        if sh.nb(px, py) < 0.18:
            return ember[1]
        i = int(max(0, min(5, k * 5.2)))
        return ember[i]

    def head(f, s, t, px, py):
        if f == "front" and 0.45 < t < 0.75 and (0.15 < s < 0.35 or 0.65 < s < 0.85):
            return eye
        return lava(f, s, t, px, py, 0.1)
    sh.box(0, 0, 8, 5, 7, head)
    sh.box(0, 12, 8, 3, 8, lambda f, s, t, px, py: lava(f, s, t, px, py, 0.05))
    for u, v, dx, dy, dz in ((34, 0, 10, 6, 5), (34, 11, 9, 5, 4), (0, 54, 9, 1, 5),
                             (20, 33, 5, 10, 5), (20, 23, 5, 5, 5), (20, 48, 5, 1, 5),
                             (0, 23, 5, 5, 5), (0, 33, 5, 10, 5), (0, 48, 5, 1, 5)):
        sh.box(u, v, dx, dy, dz, lambda f, s, t, px, py: lava(f, s, t, px, py))
    return sh.img


# ── registry ────────────────────────────────────────────────────────────────
TEXTURES = {
    "moa_blue": lambda n: moa_body(n, "blue"),
    "moa_white": lambda n: moa_body(n, "white"),
    "moa_black": lambda n: moa_body(n, "black"),
    "moa_saddle": lambda n: moa_saddle(n, "#6e4526", "#b99a5c", "#e4c35a"),
    "black_moa_saddle": lambda n: moa_saddle(n, "#3a2a45", "#9a7cc0", "#e4c35a"),
    "flying_cow_saddle": gen_flying_cow_saddle,
    "aerbunny": gen_aerbunny,
    "aerwhale": gen_aerwhale,
    "swet_blue": lambda n: swet(n, "#7fc8f0", "#3f95d8", "#1f5c9c"),
    "swet_golden": lambda n: swet(n, "#f6d879", "#e0a632", "#a86a12"),
    "aechor_plant": gen_aechor_plant,
    "mimic": gen_mimic,
    "sentry": lambda n: sentry(n, False, False),
    "sentry_lit": lambda n: sentry(n, True, False),
    "sentry_eye": lambda n: sentry(n, True, True),
    "valkyrie": gen_valkyrie,
    "fire_minion": gen_fire_minion,
}


def png_bytes(arr: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def render(name: str) -> bytes:
    arr = TEXTURES[name](name)
    assert arr.dtype == np.uint8 and arr.ndim == 3 and arr.shape[2] == 4, name
    return png_bytes(arr)


def preview(names: list[str], outpath: Path) -> None:
    bg = (58, 64, 78, 255)
    tiles = []
    for n in names:
        im = Image.open(io.BytesIO(render(n))).convert("RGBA")
        s = 6 if im.size[0] <= 64 else (3 if im.size[0] <= 128 else 2)
        checker = Image.new("RGBA", (im.size[0] * s, im.size[1] * s), bg)
        d = ImageDraw.Draw(checker)
        for y in range(0, checker.size[1], 8):
            for x in range(0, checker.size[0], 8):
                if (x // 8 + y // 8) % 2:
                    d.rectangle((x, y, x + 7, y + 7), fill=(70, 76, 92, 255))
        checker.alpha_composite(im.resize(checker.size, Image.NEAREST))
        tiles.append((n, checker))
    width = 1600
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
    ap.add_argument("--preview", metavar="PNG", help="write a contact sheet instead of the textures")
    ap.add_argument("--check", action="store_true", help="byte-compare against disk; exit 1 on drift")
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

    drift = 0
    for name in names:
        data = render(name)
        path = OUT_DIR / f"{name}.png"
        rel = path.relative_to(REPO_ROOT)
        if args.check:
            if not path.exists():
                print(f"MISSING  {rel}")
                drift += 1
            elif path.read_bytes() != data:
                print(f"DRIFT    {rel}")
                drift += 1
            else:
                print(f"ok       {path.name}")
        else:
            OUT_DIR.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f"wrote    {rel} ({len(data)} bytes)")
    if args.check:
        print("check: " + ("clean" if drift == 0 else f"{drift} file(s) differ"))
        return 1 if drift else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
