#!/usr/bin/env python3
# tools/gen_aurelith_outskirts.py
#
# The outskirts of Aurelith, the Lantern City (docs/the-hush.md "Aurelith —
# the outskirts", lore in docs/hush-lore.md): what lies outside the walls.
#
#   * four ruined ROADS, one out of each gate, a few hundred blocks into the
#     Hush — paved at the gate, broken further out, fading to scattered stones
#     at the end; lamp-post stumps near the city; they drape over the ground
#     (no levelling) and drown where they meet water;
#   * toppled MILE-MARKERS every measure (a hundred paces; the wall stands one
#     measure from the Heart, so the first stone out says II), a Stave-cut
#     face and the words beneath on a waxed sign;
#   * broken OUTLYING BUILDINGS along the roads: farmsteads, watch posts,
#     roadside shrines, a colonnade fragment, a cistern, and on the Soprano
#     road — the Last Procession — a processional arch;
#   * the GARDEN OF STONES off the Bass road: the Choir's necropolis — a
#     walled precinct of tombs with their names cut on plaques, blank-faced
#     statues, the Conductors' mausoleum, and a round chapel whose lectern
#     holds "The Rite of Rest";
#   * the WAYSTATION on the Alto road: a walled courtyard, a hostel, a well, a
#     shelter, a signpost and the book travellers wrote in.
#
#   python3 tools/gen_aurelith_outskirts.py                 # write templates + JSON
#   python3 tools/gen_aurelith_outskirts.py --check-blocks  # ... and cross-check the palettes
#   python3 tools/gen_aurelith_outskirts.py --preview       # ... and render previews
#   python3 tools/gen_aurelith_outskirts.py --preview-only [--views a,b]
#
# HOW IT IS PLACED (the engine extension — src/my_terrain_library/include/
# levelgen/structure/AurelithOutskirts.h). None of this can hang off the
# city's own start: a start is referenced only within 8 chunks of itself
# (MC ChunkGenerator.createReferences), and the roads run 300+ blocks out.
# So the outskirts are a COMPANION structure set, obeycraft:aurelith_outskirts:
# its placement (obeycraft:anchored) puts a start at every SLOT below — a
# design-frame offset from the city's Heart, turned by the city's rotation —
# wherever the city itself generates; the structure then lays that slot's
# pieces: road stretches (code pieces in the library that set every stone on
# its own column's surface) and templates (rigid, beard_thin, set on the
# median ground of their footprint, dropped per city by `chance`).
#
# DESIGN FRAME. As gen_aurelith.py: x east, z south, the Heart at (0, 0).
# Every template here is authored in the NORTH ROAD's frame — the road
# leaving along -z, the traveller's right = +x — and turned to its gate
# (Soprano north 0, Alto east 1, Tenor south 2, Bass west 3; a piece on the
# road's left is turned a further 180°). Template convention (the city's):
# local y 0 is the footing, y 1 the floor laid in the old surface, y 2 the
# walking level.

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import zlib
from contextlib import contextmanager
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_hush_structures as HS  # noqa: E402  (NBT writer, block registry check)
import gen_aurelith as GA         # noqa: E402  (material kit, NBT helpers, preview renderer)
import aurelith_books as LORE     # noqa: E402  (books and sign text)

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRATCH = GA.SCRATCH
FOLDER = "aurelith_outskirts"
TEMPLATE_NS = "minecraft"
STRUCT_NS = "obeycraft"
PROCESSORS = "minecraft:aurelith_outskirts_weathering"
UNSET, AIR = GA.UNSET, GA.AIR

# The Heart in the city's start piece (gen_aurelith.py: the start is the
# centre 32x32 column, x0 = z0 = 96, the Heart at CX = CZ = 112) and its
# height over that piece's bottom (the piece starts at S - 1, the engine at
# S + 5).
ANCHOR_ORIGIN = (GA.CX - 3 * GA.TILE, 6, GA.CZ - 3 * GA.TILE)
ROAD_START = 108            # the gate complexes end at design radius 107


# ── a small template volume (gen_aurelith's City API, own size) ──────────────
class Vol:
    """Palette-indexed voxel volume of any size, with the City methods the
    shared kit and helpers use (b, set, get, name_of, props_of)."""

    b = GA.City.b
    name_of = GA.City.name_of
    props_of = GA.City.props_of

    def __init__(self, name: str, w: int, h: int, d: int) -> None:
        self.name = name
        self.W, self.HY, self.D = w, h, d
        self.v = np.zeros((w, h, d), dtype=np.uint16)
        self.palette: list[tuple[str, tuple]] = [("", ()), ("minecraft:air", ())]
        self.index = {self.palette[1]: 1}
        self.nbt: dict[tuple[int, int, int], HS.Tag] = {}
        self.rng = random.Random(zlib.crc32(name.encode()))
        self.keep_clear: set[tuple[int, int]] = set()
        # Template entities (x, y, z, nbt), as gen_aurelith.City keeps them
        # (piece_nbt writes them); the outskirts' residents go here.
        self.entities: list[tuple[float, float, float, HS.Tag]] = []

    def inside(self, x: int, y: int, z: int) -> bool:
        return 0 <= x < self.W and 0 <= y < self.HY and 0 <= z < self.D

    def set(self, x: int, y: int, z: int, bid: int, nbt: HS.Tag | None = None) -> None:
        if not self.inside(x, y, z):
            return
        self.v[x, y, z] = bid
        if nbt is not None:
            self.nbt[(x, y, z)] = nbt
        else:
            self.nbt.pop((x, y, z), None)

    def get(self, x: int, y: int, z: int) -> int:
        return int(self.v[x, y, z]) if self.inside(x, y, z) else UNSET

    def fill(self, x0, y0, z0, x1, y1, z1, bid: int) -> None:
        xa, xb = sorted((x0, x1)); ya, yb = sorted((y0, y1)); za, zb = sorted((z0, z1))
        xa, ya, za = max(xa, 0), max(ya, 0), max(za, 0)
        xb, yb, zb = min(xb, self.W - 1), min(yb, self.HY - 1), min(zb, self.D - 1)
        if xa > xb or ya > yb or za > zb:
            return
        self.v[xa:xb + 1, ya:yb + 1, za:zb + 1] = bid
        for key in [key for key in self.nbt if xa <= key[0] <= xb and ya <= key[1] <= yb and za <= key[2] <= zb]:
            del self.nbt[key]

    def is_solid(self, x: int, y: int, z: int) -> bool:
        i = self.get(x, y, z)
        return i > AIR and self.name_of(i) in GA.SOLID_CUBES

    @property
    def size(self) -> tuple[int, int, int]:
        return self.W, self.HY, self.D


def sign(c: Vol, x: int, y: int, z: int, facing: str, key: str, colour: str = "cyan",
         glowing: bool = True) -> None:
    """A waxed wall sign carrying SIGNS[key], hung on the block behind it."""
    c.set(x, y, z, c.b("spruce_wall_sign", facing=facing, waterlogged="false"),
          nbt=GA.sign_nbt(LORE.SIGNS[key], color=colour, glowing=glowing))


def hush_wall(c: Vol) -> int:
    return c.b("hushstone_brick_wall", east="none", north="none", south="none", west="none",
               up="true", waterlogged="false")


# ── shared pieces of masonry ─────────────────────────────────────────────────
def ground_pad(c: Vol, k: GA.Kit, rng: random.Random, x0: int, z0: int, x1: int, z1: int,
               kinds=None) -> None:
    """The footing (y 0) and a weathered floor (y 1) over a rectangle, the
    walking volume above it cleared of terrain for three blocks."""
    kinds = kinds or (k.polished, k.bricks, k.cracked, k.tiles)
    for x in range(x0, x1 + 1):
        for z in range(z0, z1 + 1):
            c.set(x, 0, z, k.stone)
            r = rng.random()
            c.set(x, 1, z, k.moss if r < 0.12 else k.loam if r < 0.18 else rng.choice(kinds))
            for y in range(2, 5):
                if c.get(x, y, z) == UNSET:
                    c.set(x, y, z, AIR)


def ruin_wall_run(c: Vol, k: GA.Kit, rng: random.Random, cells, top: int, stone: int,
                  lo: int = 1, broken: float = 0.6) -> None:
    """A wall along `cells` (x, z) broken to a ragged top: each column keeps
    a random height between `lo` and `top`, smoothed with its neighbour so
    the break reads as a slope, not noise."""
    h = rng.randint(lo, top)
    for (x, z) in cells:
        if rng.random() < broken:
            h = max(lo, min(top, h + rng.choice((-2, -1, -1, 0, 1, 1, 2))))
        else:
            h = top
        for y in range(2, 2 + h):
            c.set(x, y, z, stone if rng.random() > 0.18 else k.cracked)


def perimeter(x0: int, z0: int, x1: int, z1: int) -> list[tuple[int, int]]:
    """The edge cells of a rectangle in order round it (so a broken wall's
    height walks continuously)."""
    cells = [(x, z0) for x in range(x0, x1 + 1)]
    cells += [(x1, z) for z in range(z0 + 1, z1 + 1)]
    cells += [(x, z1) for x in range(x1 - 1, x0 - 1, -1)]
    cells += [(x0, z) for z in range(z1 - 1, z0, -1)]
    return cells


def rubble(c: Vol, k: GA.Kit, rng: random.Random, x0: int, z0: int, x1: int, z1: int,
           n: int, stones=None) -> None:
    """Fallen masonry lying on the floor (y 2) inside a rectangle."""
    stones = stones or (k.cracked, k.bricks, k.slab("bricks"), k.slab("polished"), k.cracked)
    for _ in range(n):
        x = rng.randint(x0, x1); z = rng.randint(z0, z1)
        if c.get(x, 2, z) in (AIR, UNSET):
            c.set(x, 2, z, rng.choice(stones))


def weather(c: Vol, k: GA.Kit, rng: random.Random, moss: float = 0.06, sculk: float = 0.03,
            webs: int = 3) -> None:
    """Age on everything: moss and sculk on exposed floor, grass and blooms in
    the cracks, cobwebs in the corners."""
    W, H, D = c.size
    for x in range(W):
        for z in range(D):
            top = None
            for y in range(H - 2, 0, -1):
                i = c.get(x, y, z)
                if i not in (UNSET, AIR):
                    top = y
                    break
            if top is None or c.get(x, top + 1, z) not in (AIR, UNSET):
                continue
            name = c.name_of(c.get(x, top, z))
            if not (name in GA.SOLID_CUBES or name.endswith("tiles")):
                continue
            r = rng.random()
            if r < sculk:
                c.set(x, top, z, k.sculk)
            elif r < sculk + moss:
                c.set(x, top, z, k.moss)
                if rng.random() < 0.5 and top + 1 < H:
                    c.set(x, top + 1, z, rng.choice((k.grass, k.grass, k.bloom)))
    for _ in range(webs * 40):
        if webs <= 0:
            break
        x, y, z = rng.randrange(W), rng.randrange(2, H), rng.randrange(D)
        if c.get(x, y, z) == AIR and c.get(x, y + 1, z) not in (AIR, UNSET) \
                and sum(1 for dx, dz in GA.N4 if c.get(x + dx, y, z + dz) not in (AIR, UNSET)) >= 2:
            c.set(x, y, z, k.cobweb)
            webs -= 1


def statue(c: Vol, k: GA.Kit, x: int, z: int, facing: str, height: int = 5) -> None:
    """A small robed figure with a blank face (docs/hush-lore.md, Statues):
    a plinth, the robe, the shoulders and a smooth head. No crystal at the
    throat — at a tomb the voice has been given back to the Heart."""
    c.set(x, 1, z, k.polished)
    c.set(x, 2, z, k.chiseled)
    for y in range(3, 3 + height - 3):
        c.set(x, y, z, k.stone)
    c.set(x, 3 + height - 3, z, k.polished)                 # shoulders
    c.set(x, 4 + height - 3, z, k.polished)                 # the blank face
    fx, fz = GA.STEP[facing]
    lx, lz = GA.STEP[GA.LEFT[facing]]
    # Hands folded in front of the robe: a slab stair at chest height.
    c.set(x + fx, 3 + height - 4, z + fz, k.stairs("polished", GA.OPP[facing], half="top"))
    for s in (-1, 1):
        c.set(x + lx * s, 3, z + lz * s, k.slab("polished"))   # the robe's hem


def cypress(c: Vol, k: GA.Kit, x: int, z: int, rng: random.Random, h: int = 7) -> None:
    """A tall, narrow whisperwood — the necropolis' cypresses: a straight
    trunk wrapped in lantern leaves that close to a point."""
    c.set(x, 1, z, k.loam)
    for y in range(2, 2 + h):
        c.set(x, y, z, k.log_y)
    # Thin, ragged foliage on the upper half only: these are old trees, and
    # a necropolis should glow faintly, not blaze.
    for y in range(2 + h // 2, 2 + h + 2):
        rad = 1 if y < 2 + h else 0
        for dx in range(-rad, rad + 1):
            for dz in range(-rad, rad + 1):
                if (dx or dz or y >= 2 + h) and abs(dx) + abs(dz) <= rad and c.get(x + dx, y, z + dz) in (AIR, UNSET):
                    if rng.random() < 0.55:
                        c.set(x + dx, y, z + dz, k.leaves)


def tomb(c: Vol, k: GA.Kit, rng: random.Random, x: int, z: int, along: str, sign_key: str | None,
         opened: bool = False) -> None:
    """A sarcophagus two long on a one-block plinth: polished sides, a lid of
    slabs (pushed half off when `opened`), the name on a plaque at its foot
    (the +axis end) and at its head the lamp the Rite of Rest lights — most
    long since out. `along` is the long axis ('x' or 'z')."""
    dx, dz = (1, 0) if along == "x" else (0, 1)
    cells = [(x, z), (x + dx, z + dz)]
    for (cx, cz) in cells:
        c.set(cx, 1, cz, k.polished)
        c.set(cx, 2, cz, k.chiseled if rng.random() < 0.3 else k.polished)
        c.set(cx, 3, cz, k.slab("polished") if not opened else AIR)
    if opened:
        # The lid slid off to one side and lies cracked on the ground.
        sx, sz = (0, 1) if along == "x" else (1, 0)
        c.set(x + sx, 2, z + sz, k.slab("polished"))
        c.set(x + dx + sx, 2, z + dz + sz, k.cracked)
    fx, fz = x + 2 * dx, z + 2 * dz
    if sign_key:
        facing = "east" if along == "x" else "south"
        sign(c, fx, 2, fz, facing, sign_key, colour="white", glowing=False)
    # The lamp at the foot of the tomb ("VI. Light the lamp at the foot").
    lx, lz = (x - dx, z - dz)
    c.set(lx, 1, lz, k.polished)
    c.set(lx, 2, lz, k.echo_lantern_stand if rng.random() < 0.12 else k.slab("polished"))


# ── the mile-markers ────────────────────────────────────────────────────────
MARKER_VOICES = ("soprano", "alto", "tenor", "bass")


def build_milestone(voice: str, measures: int) -> Vol:
    """A toppled mile-marker, 7 x 7, the road on its west (-x) side: the
    stump of a Stave-banded column standing on its plinth with the sign on
    the face toward the road, the shaft fallen away from the road, its
    crystal cap broken off in the grass."""
    c = Vol(f"milestone_{voice}_{measures}", 7, 6, 7)
    k = GA.Kit(c)
    rng = c.rng
    # Plinth and stump (x 1..2, z 2..3 footprint for the stump's cell x 1).
    for x in range(0, 4):
        for z in range(1, 6):
            c.set(x, 0, z, k.stone)
            c.set(x, 1, z, k.polished if (x, z) != (0, 1) else k.cracked)
    c.set(1, 2, 3, k.chiseled)
    c.set(1, 3, 3, k.stave)                     # the Stave-cut face
    c.set(1, 4, 3, k.cracked if rng.random() < 0.5 else k.slab("polished"))
    sign(c, 0, 3, 3, "west", f"mile_{voice}_{measures}")
    # The fallen shaft: three pillar blocks lying east, the Stave band on
    # its middle drum, the cap beyond.
    c.set(2, 2, 3, k.pillar_x)
    c.set(3, 2, 3, k.stave)
    c.set(4, 1, 3, k.loam)
    c.set(4, 2, 3, k.pillar_x)
    c.set(5, 1, 2, k.loam)
    c.set(5, 2, 2, k.crystal if rng.random() < 0.5 else k.cluster_up)
    rubble(c, k, rng, 2, 1, 6, 5, 3)
    for (x, z) in ((2, 1), (6, 4), (0, 5)):
        c.set(x, 1, z, k.moss)
        c.set(x, 2, z, k.grass)
    return c


# ── the outlying ruins (front = west, toward the road) ──────────────────────
def build_farmstead() -> Vol:
    """A farmstead of the outer choirs, 13 x 11: a stillhouse's plan whose
    roof fell in — walls broken to a slope, the hearth cold (an amber panel
    dimmed to nothing), a bed frame, a table, a wall-walled plot of whisper
    saplings run wild, and a lectern-less niche where a book was taken."""
    c = Vol("farmstead", 13, 8, 11)
    k = GA.Kit(c)
    rng = c.rng
    ground_pad(c, k, rng, 0, 0, 12, 10, kinds=(k.polished, k.bricks, k.tiles))
    # The house: x 0..7, z 1..9; the door on the west face.
    wall = k.bricks
    ruin_wall_run(c, k, rng, perimeter(0, 1, 7, 9), 5, wall, lo=1)
    for (x, z) in ((0, 1), (7, 1), (0, 9), (7, 9)):
        for y in range(2, 7):
            c.set(x, y, z, k.pillar_y)
    for y in range(2, 5):
        c.set(0, y, 5, AIR)                                   # the doorway
    c.set(0, 5, 5, k.chiseled)
    for (x, z) in ((3, 1), (5, 9), (7, 4)):                   # windows
        c.set(x, 3, z, k.glass if rng.random() < 0.5 else AIR)
    for x in range(1, 7):
        for z in range(2, 9):
            for y in range(2, 7):
                if c.get(x, y, z) in (UNSET,):
                    c.set(x, y, z, AIR)
            c.set(x, 1, z, k.planks if rng.random() > 0.15 else k.loam)
    # Hearth, bed, table, shelf.
    c.set(6, 2, 5, k.polished); c.set(6, 3, 5, k.amber); c.set(6, 4, 5, k.polished)
    c.set(5, 2, 7, c.b("cyan_bed", facing="south", part="head", occupied="false"))
    c.set(5, 2, 6, c.b("cyan_bed", facing="south", part="foot", occupied="false"))
    c.set(3, 2, 4, k.ww_slab_top); c.set(2, 2, 4, k.stairs("ww", "east"))
    c.set(3, 3, 4, c.b("white_candle", candles="2", lit="false", waterlogged="false"))
    c.set(1, 2, 8, k.bookshelf); c.set(2, 2, 8, c.b("barrel", facing="up", open="false"),
                                         nbt=GA.loot_nbt("aurelith_stillhouse", "minecraft:barrel"))
    c.set(1, 2, 2, c.b("flower_pot"))
    # The fallen roof: slabs and planks on the floor.
    rubble(c, k, rng, 1, 2, 6, 8, 9, stones=(k.ww_slab, k.planks, k.slab("bricks"), k.cracked))
    # The plot: a low wall and wild saplings, blooms.
    for x in range(8, 13):
        for z in range(0, 11):
            if x == 12 or z in (0, 10):
                if rng.random() < 0.8:
                    c.set(x, 2, z, hush_wall(c))
            else:
                c.set(x, 1, z, k.loam if rng.random() < 0.7 else k.moss)
                r = rng.random()
                if r < 0.2:
                    c.set(x, 2, z, c.b("whisperwood_sapling", stage="0"))
                elif r < 0.45:
                    c.set(x, 2, z, rng.choice((k.grass, k.bloom, k.grass)))
    weather(c, k, rng, webs=2)
    return c


def build_watchpost() -> Vol:
    """A road watch post, 9 x 9: a square tower stump of hushstone brick up
    to 11 blocks, broken on a slant, a ladder up the inside to a surviving
    ledge, the crown fallen beside it; a lantern still standing on the sill."""
    c = Vol("watchpost", 9, 14, 9)
    k = GA.Kit(c)
    rng = c.rng
    ground_pad(c, k, rng, 0, 0, 8, 8, kinds=(k.hush_polished, k.hush_bricks))
    x0, z0, x1, z1 = 1, 2, 5, 6
    for x in range(x0, x1 + 1):
        for z in range(z0, z1 + 1):
            edge = x in (x0, x1) or z in (z0, z1)
            # The break slants down to the south-east.
            top = 12 - (x - x0) - (z - z0) // 2 + rng.choice((0, 0, -1))
            for y in range(2, max(3, top)):
                c.set(x, y, z, (k.hush_bricks if rng.random() > 0.2 else k.hush_cracked) if edge else AIR)
    for y in range(2, 5):
        c.set(x0, y, 4, AIR)                                  # door, west
    c.set(x0, 5, 4, k.chiseled)
    for y in (5, 8):
        c.set(x1, y, 4, k.glass)                              # slits
        c.set(3, y, z0, k.glass)
    for y in range(2, 10):
        c.set(3, y, 5, c.b("ladder", facing="north", waterlogged="false"))
    for x in range(x0 + 1, x1):
        c.set(x, 9, z0 + 1, k.slab("hush", "top"))            # the ledge
    c.set(2, 10, 3, k.echo_lantern_stand)
    # The fallen crown to the east.
    for x in range(6, 9):
        for z in range(3, 8):
            if rng.random() < 0.55:
                c.set(x, 2, z, rng.choice((k.hush_bricks, k.hush_cracked, k.slab("hush"), k.hush_polished)))
    c.set(7, 2, 5, c.b("chest", facing="west", type="single", waterlogged="false"),
          nbt=GA.loot_nbt("aurelith_gatehouse"))
    weather(c, k, rng, webs=3)
    return c


def build_shrine() -> Vol:
    """A roadside shrine, 9 x 9: a round stone bench of six short pillars
    round an echo lantern on a plinth — REST AND LISTEN — the kind of stop a
    traveller hummed at. One pillar lies fallen."""
    c = Vol("roadside_shrine", 9, 7, 9)
    k = GA.Kit(c)
    rng = c.rng
    ground_pad(c, k, rng, 0, 0, 8, 8, kinds=(k.polished, k.tiles, k.tiles))
    for x in range(9):
        for z in range(9):
            r = math.hypot(x - 4, z - 4)
            if r > 4.4:
                c.set(x, 1, z, k.loam if rng.random() < 0.6 else k.moss)
            elif 2.5 < r <= 3.5:
                c.set(x, 1, z, k.stave if (x + z) % 3 == 0 else k.polished)
    for i in range(6):
        th = 2 * math.pi * i / 6
        x = int(round(4 + math.cos(th) * 3.4)); z = int(round(4 + math.sin(th) * 3.4))
        if i == 4:
            c.set(x, 2, z, k.pillar_x)                        # fallen
            c.set(x + 1, 2, z, k.cracked)
            continue
        for y in range(2, 2 + rng.randint(2, 3)):
            c.set(x, y, z, k.pillar_y)
        c.set(x, 2 + 3, z, k.slab("polished"))
    c.set(4, 2, 4, k.chiseled)
    c.set(4, 3, 4, k.echo_lantern_stand)
    sign(c, 3, 2, 4, "west", "roadside_shrine")
    for (x, z) in ((1, 7), (7, 1), (7, 7)):
        c.set(x, 2, z, k.bloom)
    weather(c, k, rng, moss=0.08, webs=0)
    return c


def build_colonnade() -> Vol:
    """A colonnade fragment, 15 x 7: the portico of a lost roadhouse, five
    pillars on a stepped stylobate, two standing to the lintel, one broken,
    two fallen across the steps."""
    c = Vol("colonnade", 15, 10, 7)
    k = GA.Kit(c)
    rng = c.rng
    ground_pad(c, k, rng, 0, 0, 14, 6, kinds=(k.polished, k.tiles))
    for x in range(1, 14):
        for z in range(1, 6):
            c.set(x, 2, z, k.slab("polished") if z in (1, 5) else k.polished)
    xs = (2, 5, 8, 11, 13)
    heights = (6, 6, 3, 0, 0)
    for x, h in zip(xs, heights):
        c.set(x, 3, 3, k.chiseled)
        for y in range(4, 4 + h):
            c.set(x, y, 3, k.pillar_y)
    for x in range(1, 7):
        c.set(x, 9, 3, k.bricks if rng.random() > 0.2 else k.cracked)   # the lintel on the two
    c.set(6, 9, 3, k.slab("bricks"))
    # Fallen drums across the steps.
    for x in range(9, 13):
        c.set(x, 3, 1 + (x % 2), k.pillar_x)
    rubble(c, k, rng, 7, 0, 14, 6, 8)
    weather(c, k, rng, webs=0)
    return c


def build_cistern() -> Vol:
    """A round cistern, 11 x 11, sunk two into the ground: a ring wall with a
    polished coping, glowing river water still standing in it, a broken
    windlass post and a trough."""
    c = Vol("cistern", 11, 7, 11)
    k = GA.Kit(c)
    rng = c.rng
    for x in range(11):
        for z in range(11):
            r = math.hypot(x - 5, z - 5)
            if r > 5.4:
                continue
            c.set(x, 0, z, k.stone)
            if r <= 3.5:
                c.set(x, 0, z, k.tiles)
                c.set(x, 1, z, k.water)
            elif r <= 4.5:
                c.set(x, 1, z, k.bricks)
                c.set(x, 2, z, k.polished if rng.random() > 0.25 else k.cracked)
            else:
                c.set(x, 1, z, k.polished if rng.random() > 0.3 else k.moss)
            for y in range(3, 6):
                c.set(x, y, z, AIR)
            if r <= 4.5:
                c.set(x, 2, z, c.get(x, 2, z) if c.get(x, 2, z) != UNSET else AIR)
    c.set(5, 0, 5, k.violet)                                 # the glow in the depths
    c.set(1, 2, 5, k.wall()); c.set(1, 3, 5, k.wall()); c.set(1, 4, 5, k.planks)
    c.set(9, 2, 4, k.ww_slab); c.set(9, 2, 6, k.ww_slab)
    c.set(8, 2, 8, k.cluster_up)
    weather(c, k, rng, webs=0)
    return c


def build_procession_arch() -> Vol:
    """The Soprano road's processional arch, 17 wide across the road (x) and
    5 deep: two Stave-banded piers and a lintel of which the middle has
    fallen onto the road; the plaque — THE LAST PROCESSION PASSED HERE — on
    the city face. The road runs through it along z (the city at +z)."""
    c = Vol("procession_arch", 17, 14, 5)
    k = GA.Kit(c)
    rng = c.rng
    for px in (0, 15):
        for x in range(px, px + 2):
            for z in range(0, 5):
                c.set(x, 0, z, k.stone)
                c.set(x, 1, z, k.polished)
                for y in range(2, 11):
                    band = y in (4, 8)
                    c.set(x, y, z, k.stave if band else k.bricks if rng.random() > 0.15 else k.cracked)
        # The lintel stubs over each pier (broken off toward the middle).
        for x in (range(0, 5) if px == 0 else range(12, 17)):
            for z in range(1, 4):
                c.set(x, 11, z, k.polished)
                if abs(x - 8) > 5:
                    c.set(x, 12, z, k.bricks)
    c.set(4, 11, 2, k.slab("polished"))
    c.set(12, 11, 2, k.slab("polished"))
    # The fallen middle lies on the road beneath (walk level, y 2), broken.
    for x in range(5, 12):
        for z in range(0, 5):
            if rng.random() < 0.4:
                c.set(x, 2, z, rng.choice((k.cracked, k.slab("bricks"), k.slab("polished"))))
    c.set(8, 2, 2, k.stave)
    # The plaque on the west pier's city face (+z): the pier's last row is
    # cut back one block so the sign hangs flush in the notch.
    c.set(1, 6, 4, AIR)
    sign(c, 1, 6, 4, "south", "procession_arch")
    return c


def be_nbt(block: str) -> HS.Tag:
    """The minimal block-entity tag a template block needs for the terrain
    library to emit its entity (TemplateEngine::blockEntityPayloadFor runs
    only for entries carrying nbt): furnaces, smokers, bells, pots."""
    return GA.T_comp({"id": GA.T_str(f"minecraft:{block}")})


# ── the Garden of Stones (the necropolis) ────────────────────────────────────
NECRO_W, NECRO_D = 47, 47


def build_necropolis() -> Vol:
    """The Garden of Stones, the Choir's necropolis outside the Bass Gate, 47
    x 47, its gate on the west (toward the road): a hushstone-brick precinct
    wall broken in places, a processional way to a round chapel — twelve
    pillars round a bier, a Stave ring in the floor, the lectern with "The
    Rite of Rest" — rows of tombs north and south with their names cut on
    plaques and a lamp at each foot (the Rite's sixth part), blank-faced
    statues along the way, cypresses of whisperwood along the wall, and at
    the far end the Conductors' mausoleum (the First Conductor, Severin,
    Ismay; one plaque with no name cut) with the Keepers' chest."""
    c = Vol("garden_of_stones", NECRO_W, 16, NECRO_D)
    k = GA.Kit(c)
    rng = c.rng
    W, D = NECRO_W, NECRO_D
    mid = D // 2                                            # 23: the processional axis
    # Grounds: loam and moss, grass and blooms; cleared of terrain.
    for x in range(W):
        for z in range(D):
            c.set(x, 0, z, k.stone)
            r = rng.random()
            c.set(x, 1, z, k.moss if r < 0.45 else k.loam)
            for y in range(2, 7):
                c.set(x, y, z, AIR)
            if 0 < x < W - 1 and 0 < z < D - 1 and rng.random() < 0.14:
                c.set(x, 2, z, k.bloom if rng.random() < 0.08 else k.grass)
    # The precinct wall: three high with a polished coping, broken.
    ring = perimeter(0, 0, W - 1, D - 1)
    ruin_wall_run(c, k, rng, ring, 3, k.hush_bricks, lo=0, broken=0.35)
    for (x, z) in ring:
        c.set(x, 1, z, k.hush_polished)
        top = max((y for y in range(2, 6) if c.get(x, y, z) not in (AIR, UNSET)), default=None)
        if top == 4 and rng.random() < 0.7:
            c.set(x, 5, z, k.slab("hush"))
    for (x, z) in ((0, 0), (W - 1, 0), (0, D - 1), (W - 1, D - 1)):
        for y in range(2, 7):
            c.set(x, y, z, c.b("chiseled_hushstone_bricks"))
        c.set(x, 7, z, k.echo_lantern_stand)
    # The gate: two Stave-banded piers, the lintel, the plaques outside.
    for gz in (mid - 3, mid + 3):
        for gx in (0, 1):
            for y in range(1, 9):
                c.set(gx, y, gz, k.stave if y in (3, 7) else k.hush_polished)
    for z in range(mid - 3, mid + 4):
        c.set(0, 9, z, k.hush_bricks)
        c.set(1, 9, z, k.hush_bricks)
    c.set(0, 10, mid, k.chiseled)
    for z in range(mid - 2, mid + 3):
        for x in (0, 1):
            for y in range(2, 9):
                c.set(x, y, z, AIR)
            c.set(x, 1, z, k.hush_polished)
    # Plaques on the piers' outer faces: the gate is flush with the wall, so
    # the signs sit on the piers' inner-edge faces, toward the passage.
    sign(c, 1, 4, mid - 2, "south", "garden_of_stones")
    sign(c, 1, 4, mid + 2, "north", "rest_plaque")
    # The processional way to the chapel.
    for x in range(2, 16):
        for z in range(mid - 2, mid + 3):
            c.set(x, 1, z, k.hush_polished if abs(z - mid) == 2 else k.tiles)
            if c.get(x, 2, z) in (k.grass, k.bloom):
                c.set(x, 2, z, AIR)
    # Cross paths between the tomb rows.
    for z in list(range(4, mid - 2)) + list(range(mid + 3, D - 4)):
        for x in (11, 12):
            c.set(x, 1, z, k.hush_polished if rng.random() > 0.15 else k.moss)
            c.set(x, 2, z, AIR)
    # Statues along the way, facing it, blank-faced.
    for sx in (6, 13):
        statue(c, k, sx, mid - 4, "south")
        statue(c, k, sx, mid + 4, "north")
    # The chapel: a rotunda on the axis.
    ccx, ccz = 23, mid
    for x in range(ccx - 8, ccx + 9):
        for z in range(ccz - 8, ccz + 9):
            r = math.hypot(x - ccx, z - ccz)
            if r <= 7.5:
                c.set(x, 1, z, k.polished)
                c.set(x, 2, z, AIR)
                if 4.5 < r <= 5.5:
                    c.set(x, 1, z, k.stave)
            for y in range(3, 13):
                if r <= 7.5:
                    c.set(x, y, z, AIR)
    for i in range(12):
        th = 2 * math.pi * (i + 0.5) / 12
        x = int(round(ccx + math.cos(th) * 7)); z = int(round(ccz + math.sin(th) * 7))
        fallen = i == 2                                      # one on the far side fell
        c.set(x, 2, z, k.chiseled)
        if fallen:
            for n_ in range(1, 4):
                c.set(x + n_, 2, z + 1, k.pillar_x)
            continue
        h = 7 if rng.random() > 0.3 else rng.randint(3, 6)
        for y in range(3, 3 + h):
            c.set(x, y, z, k.pillar_y)
        if h == 7:
            c.set(x, 10, z, k.polished)
    # The ring beam, broken, and a few panes of the dome that held.
    for x in range(ccx - 8, ccx + 9):
        for z in range(ccz - 8, ccz + 9):
            r = math.hypot(x - ccx, z - ccz)
            ang = math.degrees(math.atan2(z - ccz, x - ccx)) % 360
            if 6.5 < r <= 7.5 and not (40 <= ang <= 140):
                c.set(x, 11, z, k.bricks if rng.random() > 0.2 else k.cracked)
            if 5.5 < r <= 6.5 and 200 <= ang <= 300 and rng.random() < 0.6:
                c.set(x, 12, z, k.glass)
    # The bier at the centre, candles, the lectern facing the door.
    c.set(ccx, 2, ccz, k.chiseled)
    c.set(ccx + 1, 2, ccz, k.chiseled)
    c.set(ccx, 3, ccz, k.slab("polished"))
    c.set(ccx + 1, 3, ccz, k.slab("polished"))
    for (dx, dz) in ((-1, -1), (-1, 1), (2, -1), (2, 1)):
        c.set(ccx + dx, 2, ccz + dz, c.b("white_candle", candles=str(rng.randint(1, 4)), lit="false",
                                         waterlogged="false"))
    c.set(ccx - 3, 2, ccz, c.b("lectern", facing="west", has_book="true", powered="false"),
          nbt=GA.lectern_nbt("rite_of_rest"))
    c.set(ccx, 10, ccz, k.chain); c.set(ccx, 9, ccz, k.lamp_hang)
    # The tomb rows: north (z < mid) and south (z > mid) fields, long axis x.
    names = ["tomb_aldric", "tomb_bryony", "tomb_cassian", "tomb_delphine",
             "tomb_emeric", "tomb_fenna", "tomb_gideon", "tomb_hollis"]
    rng_names = random.Random(1147)
    rng_names.shuffle(names)
    slots = []
    for z in (5, 9, 13, 17):
        for x in (3, 7, 14, 18, 27, 31):
            slots.append((x, z))
    for z in (29, 33, 37, 41):
        for x in (3, 7, 14, 18, 27, 31):
            slots.append((x, z))
    for n_, (x, z) in enumerate(slots):
        if math.hypot(x + 1 - ccx, z - ccz) < 10.5:
            continue                                         # the chapel's ground
        key = names[n_ % len(names)] if n_ % 3 == 0 and names else None
        if key:
            names.remove(key)
        tomb(c, k, rng, x, z, "x", key, opened=rng.random() < 0.18)
    # Cypresses inside the wall, every seventh block.
    for t in range(5, W - 3, 9):
        for (x, z) in ((t, 2), (t, D - 3)):
            if c.get(x, 2, z) in (AIR, k.grass, k.bloom):
                cypress(c, k, x, z, rng, h=rng.randint(6, 9))
    # Lanterns on posts at the corners of the way.
    for (x, z) in ((3, mid - 3), (3, mid + 3), (15, mid - 3), (15, mid + 3)):
        c.set(x, 2, z, hush_wall(c)); c.set(x, 3, z, hush_wall(c))
        c.set(x, 4, z, k.echo_lantern_stand if rng.random() < 0.6 else k.slab("hush"))
    # The Conductors' mausoleum: x 34..44, z mid-6..mid+6, door west.
    mx0, mx1, mz0, mz1 = 34, 44, mid - 6, mid + 6
    for x in range(mx0, mx1 + 1):
        for z in range(mz0, mz1 + 1):
            edge = x in (mx0, mx1) or z in (mz0, mz1)
            c.set(x, 1, z, k.hush_polished)
            for y in range(2, 9):
                c.set(x, y, z, (k.hush_bricks if rng.random() > 0.15 else k.hush_cracked) if edge else AIR)
            c.set(x, 9, z, k.hush_polished if edge else k.slab("hush", "top"))
            if edge and (x + z) % 2 == 0:
                c.set(x, 6, z, k.stave)                       # the frieze
    for (x, z) in ((mx0, mz0), (mx1, mz0), (mx0, mz1), (mx1, mz1)):
        for y in range(1, 10):
            c.set(x, y, z, c.b("chiseled_hushstone_bricks"))
    for y in range(2, 5):
        c.set(mx0, y, mid, AIR)
        c.set(mx0, y, mid - 1, AIR)
        c.set(mx0, y, mid + 1, AIR)
    c.set(mx0, 5, mid, k.chiseled)
    c.fill(mx0 + 2, 10, mz0 + 2, mx1 - 2, 10, mz1 - 2, k.hush_polished)
    c.set((mx0 + mx1) // 2, 11, mid, k.crystal)
    c.set((mx0 + mx1) // 2, 12, mid, k.cluster_up)
    c.set((mx0 + mx1) // 2, 8, mid, k.lamp_hang)
    # The conductors lie along z against the far wall, the unnamed one last.
    for n_, key in enumerate(("tomb_first", "tomb_severin", "tomb_ismay", "tomb_empty")):
        z = mz0 + 2 + n_ * 3
        tomb(c, k, rng, mx1 - 4, z, "x", key, opened=False)
    c.set(mx0 + 2, 2, mz0 + 1, c.b("chest", facing="south", type="single", waterlogged="false"),
          nbt=GA.loot_nbt("aurelith_necropolis"))
    c.set(mx0 + 2, 2, mz1 - 1, c.b("decorated_pot", facing="north", cracked="true", waterlogged="false"),
          nbt=be_nbt("decorated_pot"))
    # Sculk creeps in from a catalyst by the mausoleum; moss, webs.
    c.set(mx0 - 3, 1, mz1 + 3, k.catalyst)
    for _ in range(60):
        x = mx0 - 3 + rng.randint(-5, 5); z = mz1 + 3 + rng.randint(-5, 5)
        if 0 < x < W - 1 and 0 < z < D - 1 and c.get(x, 1, z) in (k.moss, k.loam):
            c.set(x, 1, z, k.sculk)
    weather(c, k, rng, moss=0.03, sculk=0.02, webs=6)
    return c


# ── the waystation ───────────────────────────────────────────────────────────
WAY_W, WAY_D = 29, 25


def build_waystation() -> Vol:
    """The waystation on the Alto road, 29 x 25, its gate on the west: a
    courtyard wall of choirstone, a two-room hostel (beds for four, a table
    laid for travellers, the hearth, the book everyone wrote in), an open
    shelter with troughs, a well of river water under a lantern post, a bell
    by the gate and a signpost pointing the way to the city."""
    c = Vol("waystation", WAY_W, 10, WAY_D)
    k = GA.Kit(c)
    rng = c.rng
    W, D = WAY_W, WAY_D
    mid = D // 2                                             # 12
    for x in range(W):
        for z in range(D):
            c.set(x, 0, z, k.stone)
            r = rng.random()
            c.set(x, 1, z, k.tiles if r < 0.45 else k.polished if r < 0.7 else k.moss if r < 0.85 else k.loam)
            for y in range(2, 8):
                c.set(x, y, z, AIR)
    ruin_wall_run(c, k, rng, perimeter(0, 0, W - 1, D - 1), 3, k.bricks, lo=1, broken=0.3)
    for z in range(mid - 2, mid + 3):
        for y in range(2, 6):
            c.set(0, y, z, AIR)                               # the gate
    for gz in (mid - 3, mid + 3):
        for y in range(2, 6):
            c.set(0, y, gz, k.pillar_y)
        c.set(0, 6, gz, k.lamp)
    sign(c, 1, 4, mid - 3, "east", "waystation_door")
    # The bell by the gate (its own post) and the signpost.
    c.set(3, 2, mid - 4, k.wall()); c.set(3, 3, mid - 4, k.wall())
    c.set(3, 4, mid - 4, c.b("bell", attachment="floor", facing="west", powered="false"),
          nbt=be_nbt("bell"))
    for y in range(2, 5):
        c.set(3, y, mid + 4, k.pillar_y)                      # the signpost
    c.set(3, 5, mid + 4, k.slab("polished"))
    sign(c, 2, 4, mid + 4, "west", "signpost_city")
    sign(c, 4, 3, mid + 4, "east", "signpost_steppe")
    # The hostel: x 13..27, z 1..11, door on its south face into the yard.
    hx0, hx1, hz0, hz1 = 13, 27, 1, 11
    for x in range(hx0, hx1 + 1):
        for z in range(hz0, hz1 + 1):
            edge = x in (hx0, hx1) or z in (hz0, hz1)
            c.set(x, 1, z, k.planks if not edge else k.polished)
            for y in range(2, 6):
                c.set(x, y, z, k.bricks if edge else AIR)
            c.set(x, 6, z, k.ww_slab if rng.random() > 0.2 else AIR)   # the roof, holed
    for (x, z) in ((hx0, hz0), (hx1, hz0), (hx0, hz1), (hx1, hz1)):
        for y in range(2, 7):
            c.set(x, y, z, k.pillar_y)
    xw = 20                                                   # the partition
    for z in range(hz0 + 1, hz1):
        for y in range(2, 6):
            c.set(xw, y, z, k.bricks)
    for y in range(2, 5):
        c.set(xw, y, 6, AIR)                                  # between the rooms
        c.set(17, y, hz1, AIR)                                # the front door
        c.set(24, y, hz1, AIR)
    for x in (15, 22, 26):
        c.set(x, 3, hz1, k.amber)                             # warm windows left lit
    # West room: the common room — table laid, benches, the hearth, the book.
    c.set(14, 2, 6, k.polished); c.set(14, 3, 6, k.amber); c.set(14, 4, 6, k.polished)
    c.set(15, 2, 7, c.b("smoker", facing="east", lit="false"), nbt=be_nbt("smoker"))
    for x in (16, 17, 18):
        c.set(x, 2, 4, k.ww_slab_top)
        c.set(x, 2, 3, k.stairs("ww", "south"))
        c.set(x, 2, 5, k.stairs("ww", "north"))
    c.set(16, 3, 4, c.b("flower_pot")); c.set(18, 3, 4, c.b("white_candle", candles="3", lit="false",
                                                            waterlogged="false"))
    c.set(19, 2, 9, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=GA.lectern_nbt("waystation_book"))
    c.set(14, 2, 2, c.b("barrel", facing="up", open="false"),
          nbt=GA.loot_nbt("aurelith_waystation", "minecraft:barrel"))
    c.set(14, 2, 10, k.bookshelf); c.set(14, 3, 10, k.bookshelf)
    c.set(17, 5, 6, k.lamp_hang)
    # East room: four bunks, a chest at the foot.
    for (bx, bz) in ((22, 2), (25, 2), (22, 8), (25, 8)):
        facing = "north" if bz < 6 else "south"
        head = (bx, 2, bz) if bz < 6 else (bx, 2, bz + 2)
        foot = (bx, 2, bz + 1)
        colour = rng.choice(("cyan_bed", "purple_bed", "light_blue_bed", "white_bed"))
        c.set(*head, c.b(colour, facing=facing, part="head", occupied="false"))
        c.set(*foot, c.b(colour, facing=facing, part="foot", occupied="false"))
    c.set(26, 2, 6, c.b("chest", facing="west", type="single", waterlogged="false"),
          nbt=GA.loot_nbt("aurelith_waystation"))
    c.set(23, 5, 6, k.lamp_hang)
    c.set(21, 2, 6, c.b("cyan_carpet")); c.set(22, 2, 6, c.b("cyan_carpet"))
    # The shelter: an open roof on posts over troughs and moss bedding.
    sx0, sx1, sz0, sz1 = 15, 26, 15, 22
    for x in range(sx0, sx1 + 1):
        for z in range(sz0, sz1 + 1):
            post = x in (sx0, sx1) and z in (sz0, sz1, (sz0 + sz1) // 2) or (x == (sx0 + sx1) // 2 and z in (sz0, sz1))
            if post:
                for y in range(2, 5):
                    c.set(x, y, z, k.log_y)
            c.set(x, 5, z, k.ww_slab if rng.random() > 0.15 else AIR)
            if z == sz1 - 1 and sx0 < x < sx1 and x % 3 == 0:
                c.set(x, 2, z, c.b("cauldron"))
            elif sx0 < x < sx1 and sz0 < z < sz1 - 2 and rng.random() < 0.3:
                c.set(x, 1, z, k.moss)
    # The well: a ring of polished stone round glowing water, a lantern post.
    wx, wz = 8, 17
    for x in range(wx - 2, wx + 3):
        for z in range(wz - 2, wz + 3):
            ring_ = max(abs(x - wx), abs(z - wz))
            if ring_ <= 1:
                c.set(x, 0, z, k.water)
                c.set(x, 1, z, k.water)
            else:
                c.set(x, 2, z, k.slab("polished") if (x + z) % 2 else k.polished)
    c.set(wx - 2, 3, wz - 2, k.wall()); c.set(wx - 2, 4, wz - 2, k.wall())
    c.set(wx - 2, 5, wz - 2, k.polished)
    c.set(wx - 1, 5, wz - 2, k.slab("polished", "top"))
    c.set(wx - 1, 4, wz - 2, k.echo_lantern)
    weather(c, k, rng, moss=0.04, sculk=0.01, webs=4)
    return c


# ── the layout: roads, markers, ruins, the two set pieces ────────────────────
# Gate direction (design frame) and the traveller's right, per voice; the
# road's length past ROAD_START; its seed (the meander's phase, the stones).
GATES = (
    # (voice,   rotation, dir,     right,    length)
    ("soprano", 0, (0, -1), (1, 0), 330),
    ("alto",    1, (1, 0),  (0, 1), 290),
    ("tenor",   2, (0, 1),  (-1, 0), 310),
    ("bass",    3, (-1, 0), (0, -1), 270),
)
ROAD_HALF = 3.0
ROAD_APRON = (24.0, 5.0)        # the first 24 blocks out of a gate are 11 wide
ROAD_KEEP = (0.92, 0.20)
ROAD_WOBBLE = 3.0
ROAD_LAMPS = (24.0, 150.0)      # a post every 24 blocks for the first 150
STRETCH = 64                    # one companion start per 64 blocks of road


def road_seed(voice: str) -> int:
    return zlib.crc32(f"aurelith/road/{voice}".encode()) & 0xFFFF


def road_centre(t: float, seed: int, wobble: float = ROAD_WOBBLE) -> float:
    """The meander — AurelithRoadBehavior::centre, exactly."""
    if wobble <= 0:
        return 0.0
    s = min(max((t - 60.0) / 80.0, 0.0), 1.0)
    s = s * s * (3.0 - 2.0 * s)
    return wobble * s * math.sin(t / 37.0 + (seed % 628) / 100.0)


def rot_xz(x: int, z: int, rotation: int) -> tuple[int, int]:
    """MC Rotation.rotate on (x, z): NONE, CW_90, CW_180, CCW_90."""
    r = rotation & 3
    if r == 1:
        return -z, x
    if r == 2:
        return -x, -z
    if r == 3:
        return z, -x
    return x, z


# Every template, built once: name -> Vol.
def all_templates() -> dict[str, Vol]:
    vols = [build_farmstead(), build_watchpost(), build_shrine(), build_colonnade(), build_cistern(),
            build_procession_arch(), build_necropolis(), build_waystation()]
    vols += [build_milestone(v, m) for v in MARKER_VOICES for m in (2, 3, 4)]
    for v in vols:
        GA.bake_connections(v)
    return {v.name: v for v in vols}


# Roadside set: (template, t, side (+1 right / -1 left), distance from the
# road's centre to the piece's centre, chance per city).
ROADSIDE = {
    "soprano": [("procession_arch", 36, 0, 0, 1.0), ("roadside_shrine", 118, -1, 12, 0.85),
                ("watchpost", 205, 1, 15, 0.75), ("farmstead", 262, -1, 18, 0.75),
                ("colonnade", 305, 1, 14, 0.7)],
    "alto":    [("farmstead", 70, 1, 16, 0.75), ("waystation", 165, -1, 27, 1.0),
                ("roadside_shrine", 232, 1, 12, 0.85), ("cistern", 268, -1, 14, 0.7)],
    "tenor":   [("watchpost", 58, -1, 13, 0.75), ("colonnade", 138, 1, 14, 0.7),
                ("farmstead", 205, -1, 17, 0.75), ("roadside_shrine", 276, 1, 11, 0.85)],
    "bass":    [("roadside_shrine", 48, 1, 11, 0.85), ("garden_of_stones", 150, 1, 41, 1.0),
                ("farmstead", 222, -1, 16, 0.75), ("cistern", 250, 1, 13, 0.6)],
}
# The two set pieces get their own start and a path from the road.
SET_PIECES = {"garden_of_stones", "waystation"}


def road_point(gate, t: float, u: float) -> tuple[float, float]:
    """Design (x, z) of road-frame (t, u) for a gate."""
    _v, _rot, d, r, _len = gate
    return (d[0] * (ROAD_START + t) + r[0] * u, d[1] * (ROAD_START + t) + r[1] * u)


def piece_origin(vol: Vol, rotation: int, centre: tuple[float, float]) -> tuple[int, int]:
    """Design position of a template's local origin so that its local
    centre lands on `centre`, turned by `rotation` (design(local) = origin +
    R(local), the jigsaw's pivot-zero rule)."""
    lc = rot_xz(vol.W // 2, vol.D // 2, rotation)
    return int(round(centre[0])) - lc[0], int(round(centre[1])) - lc[1]


def build_layout(vols: dict[str, Vol]) -> list[dict]:
    """The companion's slots: one per 64-block stretch of each road (with the
    markers and ruins whose centre falls in that stretch) and one each for
    the necropolis and the waystation (the piece and its path)."""
    slots: list[dict] = []
    for gate in GATES:
        voice, grot, d, r, length = gate
        seed = road_seed(voice)
        a = (d[0] * ROAD_START, d[1] * ROAD_START)
        b = (d[0] * (ROAD_START + length), d[1] * (ROAD_START + length))
        stretches = []
        for t0 in range(0, length, STRETCH):
            t1 = min(length, t0 + STRETCH)
            tm = (t0 + t1) / 2.0
            at = road_point(gate, tm, road_centre(tm, seed))
            slot = {"at": [int(round(at[0])), int(round(at[1]))], "roads": [{
                "a": list(a), "b": list(b), "t": [t0, t1], "half": ROAD_HALF,
                "apron": list(ROAD_APRON), "keep": list(ROAD_KEEP), "wobble": ROAD_WOBBLE,
                "seed": seed, "lamps": list(ROAD_LAMPS)}], "pieces": []}
            stretches.append((t0, t1, slot))
            slots.append(slot)

        def stretch_for(t: float) -> dict:
            for (t0, t1, slot) in stretches:
                if t0 <= t < t1:
                    return slot
            return stretches[-1][2]

        # Mile-markers: one per measure from the Heart (r = 200, 300, 400),
        # on the right of the road going out, the stump beside the verge.
        for measures in (2, 3, 4):
            t = measures * 100 - ROAD_START
            if t > length - 25:
                continue
            vol = vols[f"milestone_{voice}_{measures}"]
            centre = road_point(gate, t, road_centre(t, seed) + ROAD_HALF + 4)
            ox, oz = piece_origin(vol, grot, centre)
            stretch_for(t)["pieces"].append({"template": f"{TEMPLATE_NS}:{FOLDER}/{vol.name}",
                                             "origin": [ox, oz], "rotation": grot, "chance": 1.0,
                                             "dry": True})
        for (name, t, side, dist, chance) in ROADSIDE[voice]:
            vol = vols[name]
            rot = grot if side >= 0 else (grot + 2) & 3
            u = road_centre(t, seed) + side * dist
            centre = road_point(gate, t, u)
            ox, oz = piece_origin(vol, rot, centre)
            piece = {"template": f"{TEMPLATE_NS}:{FOLDER}/{name}", "origin": [ox, oz],
                     "rotation": rot, "chance": chance, "dry": name != "procession_arch"}
            if name in SET_PIECES:
                # Its own start at its centre, and a path from the road's
                # edge to its gate (the template's west face, turned).
                edge_u = road_centre(t, seed) + side * ROAD_HALF
                gate_u = u - side * (vol.W // 2 + 1)
                pa = road_point(gate, t, edge_u)
                pb = road_point(gate, t, gate_u)
                slots.append({"at": [int(round(centre[0])), int(round(centre[1]))],
                              "roads": [{"a": [round(pa[0], 2), round(pa[1], 2)],
                                         "b": [round(pb[0], 2), round(pb[1], 2)],
                                         "t": [0, abs(gate_u - edge_u) + 1], "half": 1.0,
                                         "keep": [0.85, 0.7], "wobble": 0.0,
                                         "seed": seed ^ 0x5A5A}],
                              "pieces": [piece]})
            else:
                stretch_for(t)["pieces"].append(piece)
    return slots


# ── loot ─────────────────────────────────────────────────────────────────────
OUTSKIRTS_LOOT = {
    "aurelith_necropolis": ((3, 5), [
        ("white_candle", 1, 4, 8), ("echo_shard", 1, 3, 6), ("amethyst_shard", 1, 4, 5),
        ("resonance_bloom", 1, 4, 6), ("paper", 1, 3, 4), ("bone", 1, 3, 3),
        ("resonant_crystal", 1, 2, 3), ("book*", 1, 1, 3), ("music_disc_11", 1, 1, 1),
        ("golden_apple", 1, 1, 1)]),
    "aurelith_waystation": ((3, 6), [
        ("whisperfruit", 2, 5, 8), ("bread", 1, 3, 5), ("paper", 1, 4, 5), ("string", 1, 4, 4),
        ("lead", 1, 1, 3), ("compass", 1, 1, 2), ("echo_shard", 1, 2, 4), ("white_candle", 1, 2, 4),
        ("cyan_wool", 1, 3, 3), ("echo_compass", 1, 1, 1), ("recall_chime", 1, 1, 1)]),
}
OUTSKIRTS_LOOT_HOMES = {"aurelith_necropolis": "necropolis", "aurelith_waystation": "waystation"}


# ── output ───────────────────────────────────────────────────────────────────
def write_template(vol: Vol, path: Path) -> int:
    p = GA.PieceOut(vol.name, 0, 0, 0, vol.W - 1, vol.HY - 1, vol.D - 1)
    data, n = GA.piece_nbt(vol, p)
    path.write_bytes(data)
    return n


def write_all(vols: dict[str, Vol], slots: list[dict], outdir: Path) -> list[Path]:
    written: list[Path] = []
    struct_dir = outdir / "minecraft" / "structure" / FOLDER
    struct_dir.mkdir(parents=True, exist_ok=True)
    for stale in struct_dir.glob("*.nbt"):
        stale.unlink()
    total = 0
    for vol in vols.values():
        path = struct_dir / f"{vol.name}.nbt"
        total += write_template(vol, path)
        written.append(path)
    print(f"wrote {len(vols)} outskirts templates, {total} listed blocks")

    wg = outdir / STRUCT_NS / "worldgen"
    (wg / "structure").mkdir(parents=True, exist_ok=True)
    (wg / "structure_set").mkdir(parents=True, exist_ok=True)
    structure = {
        "type": f"{STRUCT_NS}:aurelith_outskirts",
        # The city's own biome list: the companion's starts are further
        # filtered by the city generating at all (AurelithOutskirts.h).
        "biomes": "#minecraft:has_structure/aurelith",
        "step": "surface_structures",
        "terrain_adaptation": "beard_thin",
        "spawn_overrides": {},
        "anchor_set": "minecraft:aurelith",
        "anchor_structure": "minecraft:aurelith",
        "anchor_origin": list(ANCHOR_ORIGIN),
        "processors": PROCESSORS,
        "slots": slots,
    }
    (wg / "structure" / "aurelith_outskirts.json").write_text(json.dumps(structure, indent=1) + "\n")
    structure_set = {
        "placement": {"type": f"{STRUCT_NS}:anchored", "salt": 1147031148,
                      "anchor_set": "minecraft:aurelith",
                      "anchor_origin": [ANCHOR_ORIGIN[0], ANCHOR_ORIGIN[2]],
                      "slots": [s["at"] for s in slots]},
        "structures": [{"structure": f"{STRUCT_NS}:aurelith_outskirts", "weight": 1}],
    }
    (wg / "structure_set" / "aurelith_outskirts.json").write_text(json.dumps(structure_set, indent=2) + "\n")
    processors = {"processors": [
        {"processor_type": "minecraft:rule", "rules": [
            GA._rule("minecraft:choirstone_bricks", 0.14, "minecraft:cracked_choirstone_bricks"),
            GA._rule("minecraft:choirstone_tiles", 0.08, "minecraft:cracked_choirstone_bricks"),
            GA._rule("minecraft:polished_choirstone", 0.03, "minecraft:hush_moss"),
            GA._rule("minecraft:hushstone_bricks", 0.16, "minecraft:cracked_hushstone_bricks"),
            GA._rule("minecraft:nightglass", 0.25, "minecraft:air"),
            GA._rule("minecraft:echo_lantern", 0.2, "minecraft:air"),
        ]},
        {"processor_type": "minecraft:protected_blocks", "value": "#minecraft:features_cannot_replace"},
    ]}
    (outdir / "minecraft" / "worldgen" / "processor_list" / "aurelith_outskirts_weathering.json").write_text(
        json.dumps(processors, indent=2) + "\n")
    # Loot: the city's table writer, with the outskirts' two tables added.
    GA.LOOT.update(OUTSKIRTS_LOOT)
    GA.LOOT_BOOK_HOMES.update(OUTSKIRTS_LOOT_HOMES)
    loot_dir = outdir / "minecraft" / "loot_table" / "chests"
    for name in OUTSKIRTS_LOOT:
        (loot_dir / f"{name}.json").write_text(json.dumps(GA.loot_table(name), indent=2) + "\n")
    print(f"wrote obeycraft:aurelith_outskirts ({len(slots)} slots), its structure set, "
          f"processor list and {len(OUTSKIRTS_LOOT)} loot tables")
    return written


# ── preview ──────────────────────────────────────────────────────────────────
# The city (gen_aurelith.build_city) and its outskirts on a synthetic Hush
# landscape, rendered with gen_aurelith's ray-caster. The land is the city's
# levelled platform blending into rolling hills past the walls; the roads are
# laid by a Python twin of AurelithRoadBehavior (same frame, same meander,
# the same thinning, cracking and moss — its own hash), the templates on the
# median of their footprint with an approximate beard_thin under them.
SCENE_R = 430                    # design radius the scene covers
SCENE_H = 128


def terrain_height(dx: np.ndarray, dz: np.ndarray) -> np.ndarray:
    """Ground top y at design (dx, dz): the street level inside the city's
    beard, rolling hills beyond it, a shallow valley to the south-east."""
    s = GA.S
    r = np.hypot(dx, dz)
    hills = (6.0 * np.sin(dx / 61.0) * np.cos(dz / 47.0) + 4.0 * np.sin((dx + dz) / 29.0)
             + 3.0 * np.cos((dx - 2 * dz) / 83.0) + 5.0 * np.sin(dz / 97.0 + 1.3))
    valley = -6.0 * np.exp(-(((dx - 220) / 70.0) ** 2 + ((dz - 180) / 110.0) ** 2))
    w = np.clip((r - 115.0) / 60.0, 0.0, 1.0)
    w = w * w * (3 - 2 * w)
    return np.round(s + w * (hills + valley)).astype(np.int32)


class Scene(Vol):
    """The preview volume: one palette for the city, the templates, the road
    and the synthetic ground."""

    def __init__(self) -> None:
        n = 2 * SCENE_R
        super().__init__("scene", n, SCENE_H, n)
        self.off = SCENE_R
        k = GA.Kit(self)
        self.k = k
        xs = np.arange(n)[:, None] - self.off
        zs = np.arange(n)[None, :] - self.off
        self.h = terrain_height(xs + 0.0, zs + 0.0)
        ys = np.arange(SCENE_H)[None, :, None]
        hh = self.h[:, None, :]
        grass_top = self.b("hush_moss")
        self.v[:] = 0
        self.v[ys <= hh] = k.loam
        self.v[ys < hh - 4] = k.hush
        top = ys == hh
        self.v[top & np.ones_like(self.v, bool)] = grass_top

    def remap(self, src) -> np.ndarray:
        return np.array([0] + [self.b(name.split(":")[1], **dict(props)) if i > 0 else 0
                               for i, (name, props) in enumerate(src.palette) if i > 0], dtype=np.uint16)


def place_city(sc: Scene, c: GA.City) -> None:
    m = np.zeros(len(c.palette), dtype=np.uint16)
    for i, (name, props) in enumerate(c.palette):
        if i == UNSET:
            continue
        m[i] = AIR if i == AIR else sc.b(name.split(":")[1], **dict(props))
    x0 = sc.off - GA.CX
    z0 = sc.off - GA.CZ
    sub = sc.v[x0:x0 + GA.W, :GA.HY, z0:z0 + GA.D]
    cv = c.v[:, :SCENE_H, :]
    listed = cv != UNSET
    sub[listed] = m[cv[listed]]
    # Inside the walls the city's unset cells are its platform (solid under
    # the street, open above) whatever the hills would have been.
    ys = np.arange(SCENE_H)[None, :, None]
    inside = (GA.INSIDE | GA.WALL_BAND)[:, None, :] & ~listed
    sub[inside & (ys <= GA.S)] = sc.k.loam
    sub[inside & (ys > GA.S)] = 0


def place_template(sc: Scene, vol: Vol, origin: tuple[int, int], rotation: int) -> None:
    """Rotate `vol` (MC pivot-zero rule), set it on the median ground of its
    footprint (the engine's five probes), beard under and carve over it."""
    m = np.zeros(len(vol.palette), dtype=np.uint16)
    for i, (name, props) in enumerate(vol.palette):
        if i > AIR:
            m[i] = sc.b(name.split(":")[1], **dict(props))
        elif i == AIR:
            m[i] = AIR
    lx, ly, lz = np.nonzero(vol.v != UNSET)
    ids = m[vol.v[lx, ly, lz]]
    rx, rz = rot_xz(lx, lz, rotation)
    wx = rx + origin[0] + sc.off
    wz = rz + origin[1] + sc.off
    # The five probes over the rotated footprint.
    xa, xb, za, zb = wx.min(), wx.max(), wz.min(), wz.max()
    probes = [((xa + xb) // 2, (za + zb) // 2), (xa + 1, za + 1), (xb - 1, za + 1), (xa + 1, zb - 1), (xb - 1, zb - 1)]
    hs = sorted(int(sc.h[np.clip(px, 0, sc.W - 1), np.clip(pz, 0, sc.D - 1)]) + 1 for (px, pz) in probes)
    oy = hs[2] - 2
    wy = ly + oy
    ok = (wx >= 0) & (wx < sc.W) & (wz >= 0) & (wz < sc.D) & (wy >= 0) & (wy < SCENE_H)
    # beard_thin, roughly: ground up to the floor under the footprint, the
    # hill carved away above it.
    cols = set(zip(wx[ok].tolist(), wz[ok].tolist()))
    for (x, z) in cols:
        g = int(sc.h[x, z])
        if g < oy + 1:
            sc.v[x, g + 1:oy + 1, z] = sc.k.loam
        elif g > oy + 1:
            sc.v[x, oy + 2:g + 1, z] = 0
    ids_ok = ids[ok]
    sc.v[wx[ok], wy[ok], wz[ok]] = np.where(ids_ok == AIR, 0, ids_ok)


def draw_road(sc: Scene, road: dict, heart_seed: int = 0) -> None:
    """The Python twin of AurelithRoadBehavior::postProcess."""
    k = sc.k
    ax, az = road["a"]; bx, bz = road["b"]
    length = max(1.0, math.hypot(bx - ax, bz - az))
    dx, dz = (bx - ax) / length, (bz - az) / length
    t0, t1 = road["t"]
    half = road.get("half", 3.0)
    apron_len, apron_half = road.get("apron", [0.0, half])
    keep_a, keep_b = road["keep"]
    wobble = road.get("wobble", 0.0)
    seed = road["seed"]
    lamp_every, lamp_until = road.get("lamps", [0.0, 0.0])
    rng = random.Random(seed * 7919 + int(t0))

    def centre(t):
        return road_centre(t, seed, wobble)

    def half_w(t):
        if t >= apron_len:
            return half
        s = min(max((t - (apron_len - 8.0)) / 8.0, 0.0), 1.0)
        s = s * s * (3 - 2 * s)
        return apron_half + (half - apron_half) * s

    reach = max(half, apron_half) + wobble + 2
    xs, zs = [], []
    for t in (t0, t1):
        for u in (-reach, reach):
            xs.append(ax + dx * t - dz * u); zs.append(az + dz * t + dx * u)
    slab = k.slab("bricks")
    for x in range(int(math.floor(min(xs))) - 1, int(math.ceil(max(xs))) + 2):
        for z in range(int(math.floor(min(zs))) - 1, int(math.ceil(max(zs))) + 2):
            px, pz = x - ax, z - az
            t = px * dx + pz * dz
            if t < t0 or t >= t1 or t < 0 or t > length:
                continue
            u = -px * dz + pz * dx
            du = u - centre(t)
            w = half_w(t)
            if abs(du) > w + 1.5:
                continue
            gx, gz = x + sc.off, z + sc.off
            if not (0 <= gx < sc.W and 0 <= gz < sc.D):
                continue
            top = int(sc.h[gx, gz])
            f = t / length
            fade = min(max((f - 0.86) / 0.14, 0.0), 1.0)
            keep = (keep_a + (keep_b - keep_a) * f ** 1.2) * (1 - 0.85 * fade * fade * (3 - 2 * fade))
            h1, h2, h3 = rng.random(), rng.random(), rng.random()
            if lamp_every > 0 and t < lamp_until:
                kk = round((t - lamp_every * 0.5) / lamp_every)
                tk = lamp_every * 0.5 + kk * lamp_every
                side = 1 if kk % 2 else -1
                if abs(t - tk) < 0.5 and abs(u - (centre(tk) + side * (half_w(tk) + 1))) < 0.5:
                    hgt = 0 if h2 < 0.15 else 1 if h2 < 0.45 else 2 if h2 < 0.75 else 3
                    sc.v[gx, top, gz] = k.polished
                    for i in range(1, hgt + 1):
                        sc.v[gx, top + i, gz] = k.wall()
                    if hgt == 3 and h3 < 0.6 * (1 - f):
                        sc.v[gx, top + 4, gz] = k.echo_lantern_stand
                    continue
            if abs(du) > w:
                if h1 < 0.22 * keep:
                    sc.v[gx, top, gz] = k.polished if h2 < 0.5 else k.cracked
                elif h1 < 0.22 * keep + 0.03:
                    sc.v[gx, top + 1, gz] = slab
                continue
            if h1 > keep:
                if h2 < 0.04:
                    sc.v[gx, top + 1, gz] = slab
                continue
            stone = k.polished if abs(du) >= w - 0.5 else k.tiles if abs(du) <= 0.75 else k.bricks
            if h2 < 0.08 + 0.22 * f:
                stone = k.cracked
            elif h2 > 0.93 - 0.20 * f:
                stone = k.moss
            if h3 < 0.012:
                stone = k.sculk
            if h3 > 0.985 - 0.05 * f:
                sc.v[gx, top, gz] = 0
                sc.v[gx, top - 1, gz] = stone
                continue
            sc.v[gx, top, gz] = stone


def build_scene(vols: dict[str, Vol], slots: list[dict], with_city: bool = True) -> tuple[Scene, list]:
    sc = Scene()
    beams = []
    if with_city:
        city, _k = GA.build_city()
        GA.bake_connections(city)
        place_city(sc, city)
        for (x, y, z, colour) in GA.voice_beacons(city):
            beams.append((x - GA.CX + sc.off, y, z - GA.CZ + sc.off, colour))
    for slot in slots:
        for road in slot.get("roads", []):
            draw_road(sc, road)
    for slot in slots:
        for p in slot.get("pieces", []):
            name = p["template"].split("/")[-1]
            place_template(sc, vols[name], tuple(p["origin"]), p["rotation"])
    return sc, beams


@contextmanager
def scene_dims(sc: Scene):
    """gen_aurelith's ray-caster sizes its volume from module constants; lend
    it the scene's for the length of a render."""
    saved = (GA.W, GA.HY, GA.D, GA.S)
    GA.W, GA.HY, GA.D = sc.W, sc.HY, sc.D
    try:
        yield
    finally:
        GA.W, GA.HY, GA.D, GA.S = saved


def render_scene_view(sc: Scene, cam: GA.Camera, w: int, h: int, path: Path, beams: list,
                      crop: tuple[int, int, int, int] | None = None) -> None:
    """Render `cam` over the scene (optionally a horizontal crop x0, z0, x1,
    z1 in scene coords, to keep the volume small)."""
    from PIL import Image
    from scipy.ndimage import gaussian_filter
    if crop:
        x0, z0, x1, z1 = crop
    else:
        x0, z0, x1, z1 = 0, 0, sc.W, sc.D
    sub = Vol("crop", x1 - x0, sc.HY, z1 - z0)
    sub.palette, sub.index = sc.palette, sc.index
    sub.v = np.ascontiguousarray(sc.v[x0:x1, :, z0:z1])
    col, alpha, emis, _terrain = GA.preview_tables(sub)
    grid = sub.v.astype(np.int32)
    grid[grid == AIR] = 0
    cam = GA.Camera(cam.pos - np.array([x0, 0, z0]), math.degrees(cam.yaw), math.degrees(cam.pitch),
                    math.degrees(cam.fov), cam.ortho)
    with scene_dims(sub):
        o, d = cam.rays(w, h)
        rgb, erg, depth = GA.raycast(grid, col, alpha, emis, o, d, max_t=900.0)
    img = rgb.reshape(h, w, 3); em = erg.reshape(h, w, 3); dep = depth.reshape(h, w)
    glow = np.zeros_like(img)
    for (bx, by, bz, colour) in beams:
        ys = np.arange(by + 1.0, by + 257.0, 0.5)
        pts = np.stack([np.full_like(ys, bx - x0 + 0.5), ys, np.full_like(ys, bz - z0 + 0.5)], axis=1)
        px, py, z = cam.project(pts, w, h)
        pxi = px.astype(int); pyi = py.astype(int)
        inb = (z > 0) & (pxi >= 0) & (pxi < w) & (pyi >= 0) & (pyi < h)
        vis = inb.copy()
        vis[inb] = dep[pyi[inb], pxi[inb]] > z[inb] - 1.0
        fade = np.clip(1.0 - (ys - by) / 256.0, 0, 1) ** 0.6
        for i in np.nonzero(vis)[0]:
            glow[pyi[i], pxi[i]] += GA._hex(colour) * 0.9 * fade[i]
    bloom_src = em + glow
    bloom = gaussian_filter(bloom_src, sigma=(2.0, 2.0, 0)) * 0.9 \
        + gaussian_filter(bloom_src, sigma=(9.0, 9.0, 0)) * 0.6 \
        + gaussian_filter(glow, sigma=(1.0, 1.0, 0)) * 2.0
    final = 1 - np.exp(-(img + bloom) * 1.6)
    final = np.clip(final ** (1 / 1.05), 0, 1)
    Image.fromarray((final * 255).astype(np.uint8)).save(path)
    print(f"preview: {path}")


def look_at(eye: tuple[float, float, float], target: tuple[float, float, float],
            fov: float = 66.0) -> GA.Camera:
    """A perspective camera at `eye` aimed at `target` (scene coordinates)."""
    dx, dy, dz = (target[0] - eye[0], target[1] - eye[1], target[2] - eye[2])
    yaw = math.degrees(math.atan2(dx, -dz))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    return GA.Camera(eye, yaw, pitch, fov_deg=fov)


def find_piece(slots: list[dict], name: str) -> tuple[tuple[int, int], int]:
    for s in slots:
        for p in s.get("pieces", []):
            if p["template"].endswith("/" + name):
                return tuple(p["origin"]), p["rotation"]
    raise KeyError(name)


def render_previews(vols: dict[str, Vol], slots: list[dict], only: str | None) -> None:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    wanted = only.split(",") if only else None
    if wanted == ["ruins_sheet"]:
        render_sheet(vols, SCRATCH / "outskirts_ruins.png",
                     ["farmstead", "watchpost", "roadside_shrine", "colonnade", "cistern",
                      "procession_arch", "milestone_soprano_2"])
        return
    sc, beams = build_scene(vols, slots, with_city=True)
    off = sc.off
    S = GA.S

    def want(n):
        return wanted is None or n in wanted

    def piece_centre(name):
        (ox, oz), rot = find_piece(slots, name)
        v = vols[name]
        cx, cz = rot_xz(v.W // 2, v.D // 2, rot)
        return ox + cx + off, oz + cz + off

    views = []
    if want("outskirts_map"):
        cam = GA.Camera((off, SCENE_H + 20, off), 0.0, -90.0, ortho=sc.W)
        views.append(("outskirts_map", cam, sc.W, sc.D, None))
    if want("necropolis"):
        nx, nz = piece_centre("garden_of_stones")
        g = int(sc.h[nx, nz])
        cam = look_at((nx - 36, g + 30, nz + 40), (nx + 2, g + 2, nz - 2), fov=64)
        views.append(("necropolis", cam, 1280, 720, (nx - 110, nz - 110, nx + 110, nz + 110)))
    if want("necropolis_chapel"):
        (ox, oz), rot = find_piece(slots, "garden_of_stones")
        # Inside the gate, looking up the processional way at the chapel.
        gx, gz = rot_xz(3, NECRO_D // 2, rot)
        cx, cz = rot_xz(23, NECRO_D // 2, rot)
        g = int(sc.h[ox + cx + off, oz + cz + off])
        cam = look_at((ox + gx + off + 0.5, g + 3.6, oz + gz + off + 0.5),
                      (ox + cx + off + 0.5, g + 4.0, oz + cz + off + 0.5), fov=74)
        views.append(("necropolis_chapel", cam, 1280, 720, (ox + cx + off - 80, oz + cz + off - 80,
                                                            ox + cx + off + 80, oz + cz + off + 80)))
    if want("waystation"):
        wx, wz = piece_centre("waystation")
        g = int(sc.h[wx, wz])
        cam = look_at((wx - 34, g + 20, wz + 30), (wx, g + 2, wz), fov=62)
        views.append(("waystation", cam, 1280, 720, (wx - 100, wz - 100, wx + 100, wz + 100)))
    if want("road_soprano"):
        # On the Soprano road past the arch, looking back at the city.
        t = 150
        seed = road_seed("soprano")
        px, pz = road_point(GATES[0], t, road_centre(t, seed))
        g = int(sc.h[int(px) + off, int(pz) + off])
        cam = look_at((px + off + 2.5, g + 6, pz + off), (off, S + 18, off), fov=70)
        views.append(("road_soprano", cam, 1280, 720, (off - 200, off - 420, off + 200, off + 140)))
    if want("road_bass"):
        # Leaving by the Bass Gate: the road west, the Garden of Stones on
        # the right.
        px, pz = road_point(GATES[3], 10, 0)
        g = int(sc.h[int(px) + off, int(pz) + off])
        tx, tz = road_point(GATES[3], 170, 18)
        cam = look_at((px + off, g + 14, pz + off), (tx + off, g, tz + off), fov=70)
        views.append(("road_bass", cam, 1280, 720, (0, off - 200, off + 20, off + 200)))
    if want("arch"):
        px, pz = road_point(GATES[0], 60, 0)
        g = int(sc.h[int(px) + off, int(pz) + off])
        ax, az = road_point(GATES[0], 36, 0)
        cam = look_at((px + off + 1.5, g + 3.5, pz + off), (ax + off + 0.5, g + 6, az + off), fov=72)
        views.append(("arch", cam, 1280, 720, (off - 150, off - 300, off + 150, off + 60)))
    if want("milestone"):
        t = 192
        seed = road_seed("tenor")
        mx, mz = road_point(GATES[2], t, road_centre(t, seed) + ROAD_HALF + 2)
        ex, ez = road_point(GATES[2], t - 7, road_centre(t - 7, seed) - 1)
        g = int(sc.h[int(mx) + off, int(mz) + off])
        cam = look_at((ex + off, g + 3.4, ez + off), (mx + off, g + 1.5, mz + off), fov=70)
        views.append(("milestone", cam, 1280, 720, (int(mx) + off - 90, int(mz) + off - 90,
                                                     int(mx) + off + 90, int(mz) + off + 90)))
    for (name, cam, w, h, crop) in views:
        render_scene_view(sc, cam, w, h, SCRATCH / f"{name}.png", beams, crop)
    if want("ruins_sheet"):
        render_sheet(vols, SCRATCH / "outskirts_ruins.png",
                     ["farmstead", "watchpost", "roadside_shrine", "colonnade", "cistern",
                      "procession_arch", "milestone_soprano_2"])


def render_sheet(vols: dict[str, Vol], path: Path, names: list[str]) -> None:
    """Every roadside template side by side on flat ground (a contact sheet
    for iterating on their look), seen from the south-west above."""
    gap = 6
    width = sum(vols[n].W for n in names) + gap * (len(names) + 1)
    depth = max(vols[n].D for n in names) + 2 * gap
    sc = Vol("sheet", width, GA.S + 24, depth)
    k = GA.Kit(sc)
    sc.v[:, :GA.S + 1, :] = k.loam
    sc.v[:, GA.S, :] = sc.b("hush_moss")
    x = gap
    for n in names:
        v = vols[n]
        m = np.zeros(len(v.palette), dtype=np.uint16)
        for i, (name, props) in enumerate(v.palette):
            m[i] = AIR if i == AIR else (sc.b(name.split(":")[1], **dict(props)) if i > AIR else 0)
        lx, ly, lz = np.nonzero(v.v != UNSET)
        sc.v[lx + x, ly + GA.S - 1, lz + gap] = m[v.v[lx, ly, lz]]
        x += v.W + gap
    col, alpha, emis, _t = GA.preview_tables(sc)
    grid = sc.v.astype(np.int32)
    grid[grid == AIR] = 0
    from PIL import Image
    from scipy.ndimage import gaussian_filter
    cam = look_at((width * 0.5, GA.S + 34, depth + 52), (width * 0.5, GA.S + 3, depth * 0.5), fov=78)
    w, h = 1800, 700
    with scene_dims(sc):
        o, d = cam.rays(w, h)
        rgb, erg, _dep = GA.raycast(grid, col, alpha, emis, o, d)
    img = rgb.reshape(h, w, 3); em = erg.reshape(h, w, 3)
    bloom = gaussian_filter(em, sigma=(2.0, 2.0, 0)) * 0.9 + gaussian_filter(em, sigma=(9.0, 9.0, 0)) * 0.6
    final = np.clip((1 - np.exp(-(img + bloom) * 1.6)) ** (1 / 1.05), 0, 1)
    Image.fromarray((final * 255).astype(np.uint8)).save(path)
    print(f"preview: {path}")


def render_interiors(only: str | None) -> None:
    """Close-ups of the city's furnished rooms (gen_aurelith's room kits):
    a Stillhouse kitchen, an Archive reading gallery, the Tuners' workshop,
    the Arcade's stalls and a flat in a mid-rise — cameras placed from the
    generator's own records, rendered with its ray-caster."""
    SCRATCH.mkdir(parents=True, exist_ok=True)
    wanted = only.split(",") if only else None
    c, _k = GA.build_city()
    GA.bake_connections(c)
    S = GA.S
    beams = GA.voice_beacons(c)
    views = {}

    def room_view(x0: int, z0: int, x1: int, z1: int, y: int, fov: float = 88.0):
        """A camera in the corner of a room looking across it: the eye on
        the free (walkable) cell farthest from the furniture's centre, aimed
        at that centre, just under head height."""
        free, stuff = [], []
        for x in range(x0, x1 + 1):
            for z in range(z0, z1 + 1):
                i = c.get(x, y, z)
                if i == GA.AIR and c.get(x, y + 1, z) == GA.AIR:
                    free.append((x, z))
                elif i > GA.AIR and c.get(x, y - 1, z) > GA.AIR and c.name_of(i) not in GA.SOLID_CUBES:
                    stuff.append((x, z))
        if not free:
            return None
        pool = stuff or free
        cx = sum(p[0] for p in pool) / len(pool); cz = sum(p[1] for p in pool) / len(pool)
        ex, ez = max(free, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cz) ** 2)
        return look_at((ex + 0.5, y + 1.6, ez + 0.5), (cx + 0.5, y + 0.4, cz + 0.5), fov=fov)

    x0, z0, x1, z1 = GA.HOUSES[len(GA.HOUSES) // 2]
    views["interior_stillhouse"] = room_view(x0 + 1, z0 + 1, x1 - 1, z1 - 1, S + 1)
    views["interior_stillhouse_upper"] = room_view(x0 + 1, z0 + 1, x1 - 1, z1 - 1, S + 6)
    ax, az = GA.ARCHIVE
    # From the ramp's edge on the third gallery, across the reading desks.
    views["interior_archive"] = look_at((ax + 6.5, S + 22.6, az - 2.5), (ax - 3.0, S + 21.8, az + 8.5), fov=84)
    # The Tuners' Works (north-east crescent) and a flat high in a mid-rise.
    # The Works: the crescent on the plaza's north-east (build_crescents).
    ne = (GA.XS > GA.CX + GA.AVENUE_HALF) & (GA.ZS < GA.CZ - GA.AVENUE_HALF) & (GA.RADIUS <= 50)
    works = sorted((b for b in GA.BUILT if (b[0] & ne).sum() > 40), key=lambda b: -(b[0] & ne).sum())
    if works:
        pts = np.argwhere(GA.erode(works[0][0]))
        (xa, za), (xb, zb) = pts.min(axis=0), pts.max(axis=0)
        views["interior_tuners"] = room_view(int(xa), int(za), int(xb), int(zb), S + 1)
    views["interior_arcade"] = look_at((GA.CX + 10.5, S + 2.6, 186.5), (GA.CX + 40.0, S + 1.8, 186.5), fov=78)
    fm, info = max(GA.BUILT, key=lambda b: b[1]["roof_y"])
    pts = np.argwhere(GA.erode(fm))
    (xa, za), (xb, zb) = pts.min(axis=0), pts.max(axis=0)
    views["interior_flat"] = room_view(int(xa), int(za), int(xb), int(zb), S + 7)   # the first floor up
    for name, cam in views.items():
        if cam is None or (wanted and name not in wanted):
            continue
        GA.render_view(c, cam, 1280, 720, SCRATCH / f"{name}.png", beams, (GA.CX, S + 5, GA.CZ))

    # Cutaways: each floor plan seen from above with everything over head
    # height sliced off — the clearest way to read a room kit's layout.
    def cutaway(name: str, x0: int, z0: int, x1: int, z1: int, y: int) -> None:
        if wanted and name not in wanted:
            return
        # The city up to two over the floor (the room's contents and the
        # stubs of its walls), unset cells as empty.
        sc = Vol("cutaway", GA.W, y + 2, GA.D)
        sc.palette, sc.index = c.palette, c.index
        sc.v = np.array(c.v[:, :y + 2, :])
        sc.v[sc.v == GA.UNSET] = 0
        cx, cz = (x0 + x1 + 1) / 2.0, (z0 + z1 + 1) / 2.0
        span = max(x1 - x0, z1 - z0) + 1
        eye = (cx, y + 1.2 * span + 4.0, cz + 0.45 * span)
        cam = look_at(eye, (cx, y, cz), fov=48.0)
        render_scene_view(sc, cam, 1280, 720, SCRATCH / f"{name}.png", [], (x0, z0, x1 + 1, z1 + 1))

    x0, z0, x1, z1 = GA.HOUSES[len(GA.HOUSES) // 2]
    cutaway("cutaway_stillhouse", x0 - 2, z0 - 2, x1 + 2, z1 + 2, S + 1)
    cutaway("cutaway_stillhouse_upper", x0 - 2, z0 - 2, x1 + 2, z1 + 2, S + 6)
    if works:
        pts = np.argwhere(works[0][0])
        (xa, za), (xb, zb) = pts.min(axis=0), pts.max(axis=0)
        cutaway("cutaway_tuners", int(xa) - 1, int(za) - 1, int(xb) + 1, int(zb) + 1, S + 1)
    cutaway("cutaway_archive", ax - 13, az - 13, ax + 13, az + 13, S + 22)
    cutaway("cutaway_arcade", GA.CX + 8, 168, GA.CX + 56, 205, S + 1)
    pts = np.argwhere(fm)
    (xa, za), (xb, zb) = pts.min(axis=0), pts.max(axis=0)
    cutaway("cutaway_flat", int(xa) - 1, int(za) - 1, int(xb) + 1, int(zb) + 1, S + 7)


def check_all(vols: dict[str, Vol], written: list[Path]) -> int:
    bad = HS.check_blocks(written)
    for v in vols.values():
        bad += GA.check_properties(v)
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", action="store_true", help="render previews into the scratchpad")
    ap.add_argument("--preview-only", action="store_true", help="render previews, write nothing")
    ap.add_argument("--views", default=None, help="comma list of preview names")
    ap.add_argument("--check-blocks", action="store_true",
                    help="cross-check every palette block against the library; exit 1 on a miss")
    ap.add_argument("--interiors", action="store_true",
                    help="render close-ups of the city's furnished rooms (gen_aurelith's room kits)")
    ap.add_argument("--outdir", type=Path, default=REPO_ROOT / "data")
    args = ap.parse_args()
    if args.interiors:
        render_interiors(args.views)
        return 0
    vols = all_templates()
    slots = build_layout(vols)
    n_pieces = sum(len(s.get("pieces", [])) for s in slots)
    print(f"{len(vols)} templates, {len(slots)} slots, {n_pieces} placed pieces")
    if not args.preview_only:
        written = write_all(vols, slots, args.outdir)
        if args.check_blocks and check_all(vols, written):
            return 1
    if args.preview or args.preview_only:
        render_previews(vols, slots, args.views)
    return 0


if __name__ == "__main__":
    sys.exit(main())
