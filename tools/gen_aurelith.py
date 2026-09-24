#!/usr/bin/env python3
# tools/gen_aurelith.py
#
# Aurelith, the Lantern City — the Hush's capital (docs/the-hush.md "Aurelith",
# docs/hush-lore.md). A walled city ~210 blocks across on one levelled
# platform: a street grid of avenues, streets, plazas, stairs and bridges; a
# glowing violet river (the Vesper, `resonant_water`) with quays, a harbour
# and three arched bridges; towers 40-92 tall (the Conductor's Spire, the
# ring-tower Archive of Echoes, the Starward observatory, crystal needles, the
# four gate towers with their sky beams); mid-rise blocks with colonnades,
# the Stillhouses, the Arcade of Echoes, the Quiet Gardens; the Heart — the
# silenced resonance engine — on its dais in the central plaza, and the Vault
# of the Chord hidden under it. All of it abandoned mid-life, weathered and
# grown through with sculk.
#
#   python3 tools/gen_aurelith.py                   # write templates, pools, JSON
#   python3 tools/gen_aurelith.py --preview         # ... and render previews
#   python3 tools/gen_aurelith.py --check-blocks    # ... and cross-check every
#       # palette block against the terrain library's registrations
#   python3 tools/gen_aurelith.py --preview-only    # previews, write nothing
#
# HOW THE CITY IS BUILT
# The whole city is designed as ONE voxel volume (numpy, 224 x 136 x 224:
# x east, y up, z south; the street surface is y = S), then cut into jigsaw
# pieces so the terrain library places it the vanilla way:
#
#   * 7 x 7 columns of 32 x 32 blocks. Each column is split at the street:
#     an UPPER piece (y >= S - 1: the sub-base, the paving and everything
#     above it) and, where anything reaches deeper (the river channel, the
#     wall footings, the vault), a LOWER piece (y <= S - 2).
#   * The centre column's upper piece is the start. Upper pieces are linked in
#     a breadth-first spanning tree over the grid; every link is a unique
#     jigsaw name, so each child can attach in exactly one rotation and the
#     assembled city is the designed one, rotated as a whole (the start's
#     random rotation). A lower piece hangs from its upper piece through a
#     down-facing link. Every piece is rigid, so all of them share the start's
#     ground level: with the start's box bottom at S - 1, the beard_box
#     adaptation carves the terrain above the street and builds it up below
#     (the Beardifier's groundY is the START piece's minY + 1 for every rigid
#     child). That is why nothing in the start piece may go below S - 1.
#   * Cells a piece does not list are left to the terrain (the loader places
#     only the listed blocks; structure_void is not registered, so omission is
#     the only "leave it" there is). Listed: every solid block, building
#     interiors, the walking volume over streets and plazas (air a few blocks
#     up), the river channel. The air above that is left to beard_box's carve,
#     which keeps the templates near the bastion's size (~500k blocks).
#
# The structure (data/minecraft/worldgen/structure/aurelith.json) uses the
# engine's `level_site` extension (StructureInfo::jigsawSiteRadius): the site
# is sampled on a 7x7 grid, refused when rougher than max_spread, and the
# start set on the median height.
#
# Loader contract: see tools/gen_hush_structures.py (the NBT writer is shared).
# Palette entries must be blocks the library registers with those properties
# (--check-blocks); walls, fences and panes have their connections baked here.

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import zlib
from collections import deque
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_hush_structures as HS  # noqa: E402  (NBT writer, block registry check)
import aurelith_books as LORE     # noqa: E402  (book and sign text)

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRATCH = Path("/private/tmp/claude-501/-Users-obey-Desktop-MyVoxelGame/"
               "d51eccd5-cd4e-4d11-9911-e755ca4a73fc/scratchpad/aurelith")

# ── dimensions ───────────────────────────────────────────────────────────────
W = 224                 # x extent (7 tiles)
D = 224                 # z extent
HY = 136                # y extent
S = 24                  # street surface block y (walk on S + 1)
TILE = 32
NT = W // TILE          # 7
CX = CZ = 112           # city centre (the Heart)
WALL_IN = 100           # octagon "radius" of the wall's inner face
WALL_OUT = 103
CHAMFER = 150           # |dx| + |dz| cut of the octagon's corners
CLEAR = 4               # air listed this many blocks above open paving

NS = "minecraft"
FOLDER = "aurelith"
PROCESSORS = "minecraft:aurelith_weathering"

UNSET = 0
AIR = 1


# ── the volume ───────────────────────────────────────────────────────────────
class City:
    """Palette-indexed voxel volume. 0 = unset (terrain decides), 1 = air."""

    def __init__(self) -> None:
        self.v = np.zeros((W, HY, D), dtype=np.uint16)
        self.palette: list[tuple[str, tuple]] = [("", ()), ("minecraft:air", ())]
        self.index: dict[tuple[str, tuple], int] = {self.palette[1]: 1}
        self.nbt: dict[tuple[int, int, int], HS.Tag] = {}
        self.rng = random.Random(0xA0E117)
        # Columns kept clear for circulation (stair cores and their landings):
        # furniture never goes there.
        self.keep_clear: set[tuple[int, int]] = set()
        # The city's inhabitants (place_inhabitants): (x, y, z, entity nbt),
        # city coordinates, written into the piece whose box holds the block.
        self.entities: list[tuple[float, float, float, HS.Tag]] = []

    # palette ----------------------------------------------------------------
    def b(self, name: str, **props) -> int:
        if ":" not in name:
            name = f"{NS}:{name}"
        key = (name, tuple(sorted((k, str(v).lower() if isinstance(v, bool) else str(v))
                                  for k, v in props.items())))
        i = self.index.get(key)
        if i is None:
            i = len(self.palette)
            self.palette.append(key)
            self.index[key] = i
        return i

    def name_of(self, i: int) -> str:
        return self.palette[i][0]

    def props_of(self, i: int) -> dict:
        return dict(self.palette[i][1])

    # access -----------------------------------------------------------------
    @staticmethod
    def inside(x: int, y: int, z: int) -> bool:
        return 0 <= x < W and 0 <= y < HY and 0 <= z < D

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
        xb, yb, zb = min(xb, W - 1), min(yb, HY - 1), min(zb, D - 1)
        if xa > xb or ya > yb or za > zb:
            return
        self.v[xa:xb + 1, ya:yb + 1, za:zb + 1] = bid
        if self.nbt:
            for k in [k for k in self.nbt if xa <= k[0] <= xb and ya <= k[1] <= yb and za <= k[2] <= zb]:
                del self.nbt[k]

    def fill_mask(self, mask2d: np.ndarray, y0: int, y1: int, bid: int) -> None:
        """Fill every (x, z) where mask2d is true, for y0..y1 inclusive."""
        ya, yb = sorted((y0, y1))
        for y in range(max(ya, 0), min(yb, HY - 1) + 1):
            layer = self.v[:, y, :]
            layer[mask2d] = bid

    def replace(self, frm: int, to: int, box=None) -> None:
        if box is None:
            self.v[self.v == frm] = to
        else:
            x0, y0, z0, x1, y1, z1 = box
            sub = self.v[x0:x1 + 1, y0:y1 + 1, z0:z1 + 1]
            sub[sub == frm] = to

    def is_solid(self, x: int, y: int, z: int) -> bool:
        i = self.get(x, y, z)
        return i > AIR and self.name_of(i) in SOLID_CUBES


# Blocks treated as full cubes (wall/fence connections, support checks, preview
# shading of neighbours). Kept by name so the palette can grow freely.
SOLID_CUBES = {f"{NS}:{n}" for n in (
    "choirstone", "polished_choirstone", "choirstone_bricks", "cracked_choirstone_bricks",
    "chiseled_choirstone", "choirstone_tiles", "choirstone_pillar", "stave_stone",
    "cyan_lumen_panel", "violet_lumen_panel", "amber_lumen_panel", "lumen_strip",
    "resonance_engine", "voice_beacon", "resonant_crystal", "resonite_block",
    "hushstone", "polished_hushstone", "hushstone_bricks", "cracked_hushstone_bricks",
    "chiseled_hushstone_bricks", "sculk", "sculk_loam", "hush_moss", "whisperwood_planks",
    "whisperwood_log", "stripped_whisperwood_log", "bookshelf", "note_block", "barrel",
    "reinforced_deepslate", "deepslate_tiles", "polished_deepslate", "calcite",
    "amethyst_block", "crying_obsidian", "resonite_grate", "nightglass", "sculk_catalyst",
    "resonant_chime", "choir_altar", "echo_heart", "lantern_leaves",
    # The flickering windows (place_flickering_windows).
    "guttering_amber_window", "waking_amber_window", "restless_amber_window",
    "guttering_cyan_window", "waking_cyan_window", "guttering_violet_window", "waking_violet_window",
    # The dormant lights and the Hall of Instruments' cabinet (build_quest).
    "dim_stave_stone", "dim_cyan_lumen_panel", "dim_violet_lumen_panel", "dim_amber_lumen_panel",
    "dim_lumen_strip", "choir_cabinet",
)}

# ── geometry helpers ─────────────────────────────────────────────────────────
XS = np.arange(W)[:, None]
ZS = np.arange(D)[None, :]


def dist_c(x, z):
    return np.hypot(x - CX, z - CZ)


def octagon(dx, dz):
    """Octagon 'radius': the wall runs along octagon == WALL_IN..WALL_OUT."""
    ax, az = np.abs(dx), np.abs(dz)
    return np.maximum(np.maximum(ax, az), (ax + az) * (WALL_IN / CHAMFER))


OCT = octagon(XS - CX, ZS - CZ)
INSIDE = OCT < WALL_IN                           # the walled city
WALL_BAND = (OCT >= WALL_IN) & (OCT < WALL_OUT)  # the wall itself
RADIUS = np.hypot(XS - CX, ZS - CZ)

FACINGS = ("north", "south", "west", "east")
STEP = {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}
OPP = {"north": "south", "south": "north", "west": "east", "east": "west"}
LEFT = {"north": "west", "west": "south", "south": "east", "east": "north"}
RIGHT = {v: k for k, v in LEFT.items()}


def piece_rng(key: str) -> random.Random:
    return random.Random(zlib.crc32(key.encode("utf8")))


# ── block-state helpers ──────────────────────────────────────────────────────
class Kit:
    """The city's material language as palette ids (built once per City)."""

    def __init__(self, c: City) -> None:
        b = c.b
        self.c = c
        self.air = AIR
        self.stone = b("choirstone")
        self.polished = b("polished_choirstone")
        self.bricks = b("choirstone_bricks")
        self.cracked = b("cracked_choirstone_bricks")
        self.chiseled = b("chiseled_choirstone")
        self.tiles = b("choirstone_tiles")
        self.pillar_y = b("choirstone_pillar", axis="y")
        self.pillar_x = b("choirstone_pillar", axis="x")
        self.pillar_z = b("choirstone_pillar", axis="z")
        # The city's lights are placed DIM (docs/the-hush.md "Reawakening the
        # Heart"): the dormant city burns at half its voice, and the
        # awakening's light wave swaps each dim_* block for its lit twin
        # (server/level/AurelithCities). The lit ids stay reachable as
        # self.lit_* for anything that must shine regardless.
        self.stave = b("dim_stave_stone")
        self.glass = b("nightglass")
        self.grate = b("resonite_grate", waterlogged="false")
        self.cyan = b("dim_cyan_lumen_panel")
        self.violet = b("dim_violet_lumen_panel")
        self.amber = b("dim_amber_lumen_panel")
        self.strip_x = b("dim_lumen_strip", axis="x")
        self.strip_y = b("dim_lumen_strip", axis="y")
        self.strip_z = b("dim_lumen_strip", axis="z")
        self.lit_cyan = b("cyan_lumen_panel")
        self.lit_violet = b("violet_lumen_panel")
        self.lit_amber = b("amber_lumen_panel")
        self.conduit_x = b("crystal_conduit", axis="x", waterlogged="false")
        self.conduit_y = b("crystal_conduit", axis="y", waterlogged="false")
        self.conduit_z = b("crystal_conduit", axis="z", waterlogged="false")
        self.lamp = b("dim_choir_lamp", hanging="false", waterlogged="false")
        self.lamp_hang = b("dim_choir_lamp", hanging="true", waterlogged="false")
        self.water = b("resonant_water")
        self.engine = b("resonance_engine")
        self.crystal = b("resonant_crystal")
        self.resonite = b("resonite_block")
        self.hush = b("hushstone")
        self.hush_bricks = b("hushstone_bricks")
        self.hush_cracked = b("cracked_hushstone_bricks")
        self.hush_polished = b("polished_hushstone")
        self.sculk = b("sculk")
        self.loam = b("sculk_loam")
        self.moss = b("hush_moss")
        self.grass = b("hush_grass")
        self.bloom = b("resonance_bloom")
        self.leaves = b("lantern_leaves", distance="7", persistent="true", waterlogged="false")
        self.log_y = b("whisperwood_log", axis="y")
        self.log_x = b("whisperwood_log", axis="x")
        self.log_z = b("whisperwood_log", axis="z")
        self.planks = b("whisperwood_planks")
        self.ww_slab = b("whisperwood_slab", type="bottom", waterlogged="false")
        self.ww_slab_top = b("whisperwood_slab", type="top", waterlogged="false")
        self.bookshelf = b("bookshelf")
        self.echo_lantern = b("echo_lantern", hanging="true", waterlogged="false")
        self.echo_lantern_stand = b("echo_lantern", hanging="false", waterlogged="false")
        self.chain = b("iron_chain", axis="y", waterlogged="false")
        self.cluster_up = b("resonant_cluster", facing="up", waterlogged="false")
        self.cluster_up_wet = b("resonant_cluster", facing="up", waterlogged="true")
        self.note = b("note_block")
        self.chime = b("resonant_chime")
        self.cobweb = b("cobweb")
        self.catalyst = b("sculk_catalyst", bloom="false")
        self.tuff_placeholder = None

    # families ---------------------------------------------------------------
    def stairs(self, fam: str, facing: str, half: str = "bottom", shape: str = "straight",
               wet: bool = False) -> int:
        name = {"polished": "polished_choirstone_stairs", "bricks": "choirstone_brick_stairs",
                "tiles": "choirstone_tile_stairs", "ww": "whisperwood_stairs",
                "hush": "hushstone_brick_stairs"}[fam]
        return self.c.b(name, facing=facing, half=half, shape=shape,
                        waterlogged="true" if wet else "false")

    def slab(self, fam: str, kind: str = "bottom", wet: bool = False) -> int:
        name = {"polished": "polished_choirstone_slab", "bricks": "choirstone_brick_slab",
                "tiles": "choirstone_tile_slab", "ww": "whisperwood_slab",
                "hush": "hushstone_brick_slab"}[fam]
        return self.c.b(name, type=kind, waterlogged="true" if wet else "false")

    def wall(self) -> int:
        # Connections are baked in bake_connections(); this is the placeholder.
        return self.c.b("choirstone_brick_wall", east="none", north="none", south="none",
                        west="none", up="true", waterlogged="false")

    def fence(self) -> int:
        return self.c.b("whisperwood_fence", east="false", north="false", south="false",
                        west="false", waterlogged="false")


WALL_NAME = f"{NS}:choirstone_brick_wall"
FENCE_NAME = f"{NS}:whisperwood_fence"
BAR_NAME = f"{NS}:iron_bars"


# ── block-entity nbt ─────────────────────────────────────────────────────────
def T_str(s): return HS.T_str(s)
def T_int(i): return HS.T_int(i)
def T_byte(i): return HS.T_byte(i)
def T_comp(d): return HS.T_comp(d)
def T_list(et, items): return HS.T_list(et, items)


def book_item_nbt(key: str) -> HS.Tag:
    """MC ItemStack codec for a written book (DataComponents
    WRITTEN_BOOK_CONTENT: title Filterable<string>, author, pages
    Filterable<Component>), in the full {raw: ...} form MC itself writes."""
    book = LORE.BOOKS[key]
    content = T_comp({
        "title": T_comp({"raw": T_str(book["title"])}),
        "author": T_str(book["author"]),
        "pages": T_list(10, [{"raw": T_str(pg)} for pg in book["pages"]]),
        "resolved": T_byte(1),
    })
    return T_comp({
        "id": T_str("minecraft:written_book"),
        "count": T_int(1),
        "components": T_comp({"minecraft:written_book_content": content}),
    })


def sign_nbt(lines: list[str], color: str = "cyan", glowing: bool = True) -> HS.Tag:
    """SignBlockEntity: front_text/back_text SignText (messages as plain
    string components), is_waxed so nobody edits the city's plaques."""
    lines = (list(lines) + ["", "", "", ""])[:4]
    front = T_comp({
        "messages": T_list(8, lines),
        "color": T_str(color),
        "has_glowing_text": T_byte(1 if glowing else 0),
    })
    back = T_comp({
        "messages": T_list(8, ["", "", "", ""]),
        "color": T_str("black"),
        "has_glowing_text": T_byte(0),
    })
    return T_comp({"front_text": front, "back_text": back, "is_waxed": T_byte(1),
                   "id": T_str("minecraft:sign")})


def loot_nbt(table: str, block: str = "minecraft:chest") -> HS.Tag:
    return T_comp({"LootTable": T_str(f"minecraft:chests/{table}"), "id": T_str(block)})


def engine_be_nbt(block: str) -> HS.Tag:
    return T_comp({"id": T_str(f"minecraft:{block}")})


def lectern_nbt(book: str) -> HS.Tag:
    """LecternBlockEntity: the Book (an ItemStack) open at its first page."""
    return T_comp({"Book": book_item_nbt(book), "Page": T_int(0), "id": T_str("minecraft:lectern")})


# ── the plan ─────────────────────────────────────────────────────────────────
# Street centre lines (7 wide: c - 3 .. c + 3). The two avenues (13 wide) are
# Canticle Way (x = CX, north-south) and Held Note Avenue (z = CZ, east-west).
AVENUE_HALF = 6
STREETS_X = (44, 78, 146, 180)
STREETS_Z = (44, 78, 192)
STREET_HALF = 3
PLAZA_R = 30            # plaza floor radius; the colonnade stands at 31..33
COLONNADE_R0, COLONNADE_R1 = 31, 34
HEART_CLEAR_R = 15      # nothing built within this radius above the dais floor

# The river Vesper: centre line z = river_z(x), spring at the west end,
# harbour and the Listening Well at the east end.
RIVER_X0, RIVER_X1 = 30, 200
RIVER_HALF = 4          # water |t| <= 4 (9 wide)
QUAY_WALL = 6           # quay wall 5..6, promenade to QUAY_EDGE
QUAY_EDGE = 10
WATER_TOP = S - 1       # the water's top block (its surface ~one block under the street)
WATER_BOT = S - 4
BED = S - 5
SPRING = (34, 156)
HARBOUR = (180, 164)
HARBOUR_RX, HARBOUR_RZ = 17, 10
WELL = (203, 170)
BRIDGES_X = (78, 112, 146)


def river_z(x):
    return 160.0 + 6.0 * np.sin(2.0 * np.pi * (np.asarray(x, dtype=float) - 34.0) / 130.0)


def river_t():
    """Signed distance (in z) of every column from the river's centre line,
    +inf outside the river's x span."""
    t = ZS - river_z(XS)
    t = np.broadcast_to(t, (W, D)).copy()
    t[(XS[:, 0] < RIVER_X0) | (XS[:, 0] > RIVER_X1), :] = np.inf
    return t


RIVER_T = river_t()
_HX = (XS - HARBOUR[0]) / HARBOUR_RX
_HZ = (ZS - HARBOUR[1]) / HARBOUR_RZ
HARBOUR_E = np.sqrt(_HX ** 2 + _HZ ** 2)          # < 1 inside the basin
SPRING_R = np.hypot(XS - SPRING[0], ZS - SPRING[1])
WELL_R = np.hypot(XS - WELL[0], ZS - WELL[1])

WATER_MASK = INSIDE & ((np.abs(RIVER_T) <= RIVER_HALF + 0.5) | (HARBOUR_E < 1.0)
                       | (SPRING_R <= 8.5) | (WELL_R <= 4.5))
QUAYWALL_MASK = INSIDE & ~WATER_MASK & ((np.abs(RIVER_T) <= QUAY_WALL + 0.5) | (HARBOUR_E < 1.18)
                                        | (SPRING_R <= 10.5) | (WELL_R <= 6.5))
QUAY_MASK = INSIDE & ~WATER_MASK & ~QUAYWALL_MASK & ((np.abs(RIVER_T) <= QUAY_EDGE + 0.5)
                                                      | (HARBOUR_E < 1.45) | (SPRING_R <= 13.5)
                                                      | (WELL_R <= 9.5))
RIVER_ZONE = WATER_MASK | QUAYWALL_MASK | QUAY_MASK

AVE_NS = INSIDE & (np.abs(XS - CX) <= AVENUE_HALF) & np.ones((1, D), bool)
AVE_EW = INSIDE & (np.abs(ZS - CZ) <= AVENUE_HALF) & np.ones((W, 1), bool)
STREET_MASK = np.zeros((W, D), bool)
for _sx in STREETS_X:
    STREET_MASK |= (np.abs(XS - _sx) <= STREET_HALF) & np.ones((1, D), bool)
for _sz in STREETS_Z:
    STREET_MASK |= (np.abs(ZS - _sz) <= STREET_HALF) & np.ones((W, 1), bool)
STREET_MASK &= INSIDE
PLAZA_MASK = RADIUS <= PLAZA_R + 0.5
COLONNADE_MASK = (RADIUS > COLONNADE_R0 - 0.5) & (RADIUS <= COLONNADE_R1 + 0.5)


def build_ground(c: City, k: Kit) -> None:
    """The platform: sub-base and default paving under the whole walled city,
    the wall ring with its footing, the glowing Stave band that draws the
    city's outline at night, the wall walk and its crenels."""
    inside = INSIDE | WALL_BAND
    c.fill_mask(inside, S - 1, S - 1, k.stone)
    c.fill_mask(INSIDE, S, S, k.bricks)
    # The wall: footing down to S - 12 (it reads as a plinth where the
    # ground falls away), a polished base course, bricks, the Stave band.
    c.fill_mask(WALL_BAND, S - 12, S + 9, k.bricks)
    c.fill_mask(WALL_BAND, S - 12, S - 2, k.stone)
    c.fill_mask(WALL_BAND, S + 1, S + 1, k.polished)
    c.fill_mask(WALL_BAND, S + 6, S + 6, k.stave)
    c.fill_mask(WALL_BAND, S + 9, S + 9, k.polished)
    outer = WALL_BAND & (OCT >= WALL_OUT - 1)
    inner = WALL_BAND & (OCT < WALL_IN + 1)
    # A solid parapet on the outer edge with a merlon every fourth block,
    # a low wall inside; the walk between.
    merlon = outer & (((XS + ZS) % 4) == 0)
    c.fill_mask(outer, S + 10, S + 10, k.bricks)
    c.fill_mask(outer, S + 11, S + 12, AIR)
    c.fill_mask(merlon, S + 11, S + 11, k.polished)
    c.fill_mask(inner, S + 10, S + 10, k.wall())
    c.fill_mask(inner, S + 11, S + 12, AIR)
    walk = WALL_BAND & ~outer & ~inner
    c.fill_mask(walk, S + 10, S + 12, AIR)
    # Wall-walk lamps every ~14 blocks round the circuit.
    ang = np.degrees(np.arctan2(ZS - CZ, XS - CX)) % 360
    lamp_cells = np.argwhere(inner & (np.round(ang / 7.0) * 7.0 - ang < 0.25) & (np.round(ang / 7.0) * 7.0 - ang > -0.25))
    for (x, z) in lamp_cells[::2]:
        c.set(int(x), S + 11, int(z), k.lamp)


def paint_avenues(c: City, k: Kit) -> None:
    """Canticle Way and Held Note Avenue: choirstone tiles, polished
    borders, flush lumen curb lines, the avenues' lamps."""
    for mask, along in ((AVE_NS, "z"), (AVE_EW, "x")):
        c.fill_mask(mask, S, S, k.tiles)
        if along == "z":
            off = np.abs(XS - CX) * np.ones((1, D))
        else:
            off = np.abs(ZS - CZ) * np.ones((W, 1))
        c.fill_mask(mask & (off == AVENUE_HALF), S, S, k.polished)
        strip = k.strip_z if along == "z" else k.strip_x
        c.fill_mask(mask & (off == AVENUE_HALF - 1), S, S, strip)
        # A centre inlay line of polished stone.
        c.fill_mask(mask & (off == 0), S, S, k.polished)


def paint_streets(c: City, k: Kit) -> None:
    streets = STREET_MASK & ~AVE_NS & ~AVE_EW
    c.fill_mask(streets, S, S, k.bricks)
    # Kerb lines of polished stone on both edges of every street.
    for sx in STREETS_X:
        edge = (np.abs(XS - sx) == STREET_HALF) & np.ones((1, D), bool)
        c.fill_mask(streets & edge, S, S, k.polished)
    for sz in STREETS_Z:
        edge = (np.abs(ZS - sz) == STREET_HALF) & np.ones((W, 1), bool)
        c.fill_mask(streets & edge, S, S, k.polished)


def lamp_post(c: City, k: Kit, x: int, z: int, y: int = S + 1, height: int = 3) -> None:
    """A street light: a choirstone-brick post with the crystal lamp on top."""
    for yy in range(y, y + height):
        c.set(x, yy, z, k.wall())
    c.set(x, y + height, z, k.lamp)


def place_street_lamps(c: City, k: Kit) -> None:
    # Along the avenues, both sides, every 8 blocks (skipping the plaza).
    for t in range(12, 212, 8):
        for side in (-AVENUE_HALF, AVENUE_HALF):
            for (x, z) in ((CX + side, t), (t, CZ + side)):
                if not INSIDE[x, z] or RADIUS[x, z] < COLONNADE_R1 + 3 or RIVER_ZONE[x, z]:
                    continue
                if WALL_BAND[x, z] or OCT[x, z] > WALL_IN - 4:
                    continue
                lamp_post(c, k, x, z)
    # Along the streets, alternating sides every 10 blocks.
    for sx in STREETS_X:
        for n, z in enumerate(range(14, 212, 10)):
            x = sx + (STREET_HALF if n % 2 else -STREET_HALF)
            crossing = any(abs(z - sz) <= STREET_HALF + 1 for sz in STREETS_Z)
            if INSIDE[x, z] and OCT[x, z] < WALL_IN - 4 and not RIVER_ZONE[x, z] \
                    and not AVE_EW[x, z] and not PLAZA_MASK[x, z] and not crossing:
                lamp_post(c, k, x, z)
    for sz in STREETS_Z:
        for n, x in enumerate(range(14, 212, 10)):
            z = sz + (STREET_HALF if n % 2 else -STREET_HALF)
            crossing = any(abs(x - sx) <= STREET_HALF + 1 for sx in STREETS_X)
            if INSIDE[x, z] and OCT[x, z] < WALL_IN - 4 and not RIVER_ZONE[x, z] \
                    and not AVE_NS[x, z] and not crossing:
                lamp_post(c, k, x, z)


# ── the Plaza of the Held Note and the Heart ────────────────────────────────
def build_plaza(c: City, k: Kit) -> None:
    """The round plaza: concentric paving, a ring of cyan light round the
    dais, violet and Stave rings further out, eight radial lumen spokes
    pointing at the Heart; the half-step dais with the resonance engine on a
    crystal pedestal (its rings are AurelithHeartRenderer's); the colonnade
    whose lintel frames every avenue that enters."""
    r = RADIUS
    ang = np.degrees(np.arctan2(ZS - CZ, XS - CX)) % 360.0
    floor = PLAZA_MASK
    c.fill_mask(floor, S, S, k.polished)
    c.fill_mask(floor & (r > 11.5) & (r <= 13.5), S, S, k.tiles)
    c.fill_mask(floor & (r > 23.5) & (r <= 25.5), S, S, k.tiles)
    c.fill_mask(floor & (r > 9.5) & (r <= 10.5), S, S, k.cyan)
    c.fill_mask(floor & (r > 20.5) & (r <= 21.5), S, S, k.violet)
    c.fill_mask(floor & (r > 28.5) & (r <= 29.5), S, S, k.stave)
    # Spokes: every 45 degrees, from the cyan ring to the Stave ring.
    for a in range(0, 360, 45):
        th = math.radians(a)
        for step in range(22, 57):
            rr = step / 2.0
            x = int(round(CX + math.cos(th) * rr))
            z = int(round(CZ + math.sin(th) * rr))
            if 10.5 < rr < 28.5:
                strip = k.strip_x if abs(math.cos(th)) >= abs(math.sin(th)) - 1e-6 else k.strip_z
                c.set(x, S, z, strip)
    # The dais: half steps (slab, block, slab, block ...) up to S + 3.
    slab = k.slab("polished")
    for (rin, rout, y, bid) in ((7.5, 8.5, S + 1, slab), (6.5, 7.5, S + 1, k.polished),
                                (5.5, 6.5, S + 2, slab), (4.5, 5.5, S + 2, k.polished),
                                (3.5, 4.5, S + 3, slab), (-1.0, 3.5, S + 3, k.polished)):
        ring = (r > rin) & (r <= rout)
        for yy in range(S + 1, y):
            c.fill_mask(ring, yy, yy, k.polished)
        c.fill_mask(ring, y, y, bid)
    c.fill_mask(r <= 2.5, S + 3, S + 3, k.chiseled)
    c.fill_mask((r > 5.5) & (r <= 6.5), S + 1, S + 1, k.stave)   # glowing riser ring
    # Air over the plaza floor: nothing may stand in the Heart's volume.
    c.fill_mask(floor & ~(r <= 8.5), S + 1, S + 6, AIR)
    c.fill_mask(r <= 8.5, S + 4, S + 6, AIR)
    # Crystal pedestal, the engine, four crystal "tuning posts" round it.
    c.set(CX, S + 4, CZ, k.crystal)
    c.set(CX, S + 5, CZ, k.engine, nbt=engine_be_nbt("resonance_engine"))
    for (dx, dz) in ((2, 0), (-2, 0), (0, 2), (0, -2)):
        c.set(CX + dx, S + 4, CZ + dz, k.conduit_y)
        c.set(CX + dx, S + 5, CZ + dz, k.lamp)

    # The colonnade: choirstone pillars round the plaza, a lintel ring at
    # S + 9..10 (a lumen band on its inner face), a walk on top.
    col = COLONNADE_MASK
    c.fill_mask(col, S, S, k.polished)
    c.fill_mask(col, S + 1, S + 8, AIR)
    pillar_r = 32.5
    n_pillars = 52
    for i in range(n_pillars):
        th = 2 * math.pi * i / n_pillars
        x = int(round(CX + math.cos(th) * pillar_r))
        z = int(round(CZ + math.sin(th) * pillar_r))
        if abs(x - CX) <= AVENUE_HALF + 1 or abs(z - CZ) <= AVENUE_HALF + 1:
            continue
        c.set(x, S + 1, z, k.chiseled)
        c.fill(x, S + 2, z, x, S + 7, z, k.pillar_y)
        c.set(x, S + 8, z, k.chiseled)
    lintel = col
    c.fill_mask(lintel, S + 9, S + 10, k.bricks)
    c.fill_mask(lintel & (r <= COLONNADE_R0 + 0.5), S + 9, S + 9, k.strip_y)
    c.fill_mask(lintel & (r <= COLONNADE_R0 + 0.5), S + 10, S + 10, k.polished)
    c.fill_mask(lintel & (r > COLONNADE_R1 - 0.5), S + 10, S + 10, k.polished)
    c.fill_mask(lintel, S + 11, S + 13, AIR)
    c.fill_mask(lintel & (r <= COLONNADE_R0 + 0.5), S + 11, S + 11, k.wall())
    c.fill_mask(lintel & (r > COLONNADE_R1 - 0.5), S + 11, S + 11, k.wall())
    # Lamps hanging under the lintel between pillars.
    for i in range(n_pillars):
        th = 2 * math.pi * (i + 0.5) / n_pillars
        x = int(round(CX + math.cos(th) * pillar_r))
        z = int(round(CZ + math.sin(th) * pillar_r))
        if abs(x - CX) <= AVENUE_HALF + 1 or abs(z - CZ) <= AVENUE_HALF + 1:
            continue
        c.set(x, S + 8, z, k.lamp_hang)


# ── the river Vesper ─────────────────────────────────────────────────────────
def build_river(c: City, k: Kit) -> None:
    """The channel: four blocks of resonant water two below the street, a bed
    of choirstone tiles lit by violet panels in downstream chevrons, crystal
    clusters, quay walls with a railing, the quay promenade; the spring
    basin with its crystal fountain at the west end; the harbour basin and
    the Listening Well at the east end."""
    t = np.abs(RIVER_T)
    water = WATER_MASK
    # Foundation and bed under every river-zone column.
    c.fill_mask(RIVER_ZONE, S - 8, S - 7, k.stone)
    c.fill_mask(water, BED, BED, k.tiles)
    c.fill_mask(water, WATER_BOT, WATER_TOP, k.water)
    c.fill_mask(water, WATER_TOP + 1, S + CLEAR, AIR)
    # Quay walls: solid from the bed up to the street, polished coping.
    c.fill_mask(QUAYWALL_MASK, S - 7, S - 1, k.bricks)
    c.fill_mask(QUAYWALL_MASK, S, S, k.polished)
    c.fill_mask(QUAYWALL_MASK, S - 2, S - 2, k.strip_x)   # a light line along the wall face
    # The promenade.
    c.fill_mask(QUAY_MASK, S - 7, S - 1, k.stone)
    c.fill_mask(QUAY_MASK, S, S, k.polished)
    c.fill_mask(QUAY_MASK & ((XS + ZS) % 6 == 0), S, S, k.tiles)
    # The water's edge: a low kerb (half slabs, stepped over easily) with a
    # post every fourth block, so the river's glow is in view from the
    # promenade instead of behind a parapet.
    edge = QUAYWALL_MASK & (np.roll(water, 1, 0) | np.roll(water, -1, 0)
                            | np.roll(water, 1, 1) | np.roll(water, -1, 1))
    c.fill_mask(edge, S + 1, S + 1, k.slab("polished"))
    c.fill_mask(edge & (((XS + ZS) % 4) == 0), S + 1, S + 1, k.wall())
    # Violet chevrons on the bed pointing downstream (east), and cyan eyes.
    for x in range(RIVER_X0, RIVER_X1 + 1):
        zc = float(river_z(x))
        for z in range(int(zc) - RIVER_HALF - 1, int(zc) + RIVER_HALF + 2):
            if not water[x, z]:
                continue
            tz = abs(z - zc)
            if (x + int(round(tz * 1.5))) % 9 == 0 and tz <= RIVER_HALF - 0.5:
                c.set(x, BED, z, k.violet)
            elif (x % 18 == 4) and tz < 0.5:
                c.set(x, BED, z, k.cyan)
    rng = piece_rng("aurelith/river")
    cells = np.argwhere(water)
    for (x, z) in cells:
        if rng.random() < 0.035 and c.get(int(x), BED, int(z)) == k.tiles:
            c.set(int(x), WATER_BOT, int(z), k.cluster_up_wet)
    # The spring: a deeper round basin with a crystal fountain island.
    sp = SPRING_R
    c.fill_mask(INSIDE & (sp <= 6.5), BED - 3, BED, k.tiles)
    c.fill_mask(INSIDE & (sp <= 6.5), BED - 2, WATER_TOP, k.water)
    c.fill_mask(INSIDE & (sp <= 6.5) & (sp > 5.5), BED - 3, BED - 3, k.violet)
    c.fill_mask(INSIDE & (sp <= 1.5), BED - 3, WATER_TOP, k.polished)
    c.fill_mask(INSIDE & (sp <= 1.5), WATER_TOP + 1, S + 1, k.polished)
    c.fill(SPRING[0], S + 2, SPRING[1], SPRING[0], S + 6, SPRING[1], k.crystal)
    for (dx, dz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(SPRING[0] + dx, S + 2, SPRING[1] + dz, k.crystal)
        c.set(SPRING[0] + dx, S + 3, SPRING[1] + dz, k.cluster_up)
    c.set(SPRING[0], S + 7, SPRING[1], k.lamp)
    # The Listening Well: a round shaft of glowing water going down.
    wl = WELL_R
    c.fill_mask(INSIDE & (wl <= 4.5), S - 16, BED, k.bricks)
    c.fill_mask(INSIDE & (wl <= 3.5), S - 15, WATER_TOP, k.water)
    c.fill_mask(INSIDE & (wl <= 3.5), S - 16, S - 16, k.cyan)
    c.fill_mask(INSIDE & (wl <= 4.5) & (wl > 3.5), S - 14, S - 14, k.strip_y)
    c.fill_mask(INSIDE & (wl <= 4.5) & (wl > 3.5), S - 9, S - 9, k.strip_y)


def build_bridges(c: City, k: Kit) -> None:
    """Three arched bridges over the Vesper where Canticle Way (13 wide) and
    the streets at x = 78 and x = 146 (7 wide) cross it. Each deck humps two
    blocks over the middle in half steps (walkable, no jumping); under it a
    true arch — the opening follows a sine from the water at the quays to
    just under the deck at mid-span — its curve outlined on both faces in a
    lumen strip. Parapets with Stave inscriptions every fourth block, a
    crystal lamp on a pillar at each corner."""
    for bx in BRIDGES_X:
        half = AVENUE_HALF if bx == CX else STREET_HALF
        zc = float(river_z(bx))
        z0 = int(math.floor(zc - QUAY_WALL - 1))
        z1 = int(math.ceil(zc + QUAY_WALL + 1))
        span = z1 - z0
        wz = [z for z in range(z0, z1 + 1) if WATER_MASK[bx, z]]
        wa, wb = (min(wz), max(wz)) if wz else (z0, z1)
        for x in range(bx - half, bx + half + 1):
            edge = abs(x - bx) == half
            for z in range(z0, z1 + 1):
                if not INSIDE[x, z]:
                    continue
                u = (z - z0) / span
                exact = 2.4 * math.sin(math.pi * u)
                rise = int(exact)
                deck_y = S + rise
                c.fill(x, S + 1, z, x, S + 6, z, AIR)
                if WATER_MASK[x, z]:
                    # Masonry from the arch's crown to the deck; open (water,
                    # then air) under it.
                    t = (z - wa + 0.5) / max(1, wb - wa + 1)
                    ceil_y = WATER_TOP + int(round((deck_y - 1 - WATER_TOP) * math.sin(math.pi * t)))
                    c.fill(x, WATER_BOT, z, x, WATER_TOP, z, k.water)
                    if ceil_y > WATER_TOP:
                        c.fill(x, WATER_TOP + 1, z, x, ceil_y, z, AIR)
                    c.fill(x, ceil_y + 1, z, x, deck_y, z, k.bricks)
                    if edge:
                        c.set(x, ceil_y + 1, z, k.strip_x)
                else:
                    c.fill(x, S - 1, z, x, deck_y, z, k.bricks)
                c.set(x, deck_y, z, k.polished if (edge or abs(x - bx) <= 1) else k.tiles)
                if exact - rise >= 0.5:
                    c.set(x, deck_y + 1, z, k.slab("polished"))
                    top = deck_y + 1
                else:
                    top = deck_y
                if edge:
                    c.set(x, top + 1, z, k.stave if z % 4 == 0 else k.bricks)
                    c.set(x, top + 2, z, k.slab("bricks"))
        # Pillars with lamps at the four corners, the river's plaque on the
        # grand bridge.
        for z in (z0, z1):
            for x in (bx - half, bx + half):
                if INSIDE[x, z]:
                    c.set(x, S + 1, z, k.chiseled)
                    c.fill(x, S + 2, z, x, S + 3, z, k.pillar_y)
                    c.set(x, S + 4, z, k.lamp)


# ── flickering windows ───────────────────────────────────────────────────────
# Some of the city's windows have a lamp that will not settle (docs/the-hush.md
# "Motion and life"): the flickering-window blocks, each with its own irregular
# frame schedule in its texture (tools/gen_aurelith_textures.py). The facade
# builders note every window cell they lay (note_window: the cell and the lit
# panel that building uses); once the city is weathered, place_flickering_
# windows turns a share of them into flickering windows — a window (a vertical
# run of cells) always as one, its kind drawn from a hash of the window, so
# neighbours differ. Lit windows become guttering ones (mostly lit, a gutter
# now and then) or, in the amber quarters, restless ones; a few dark windows
# get a lamp that wakes now and then. Maren's house is left alone.
WINDOW_CELLS: list = []          # (x, y, z, lit panel id) of every facade window cell
FLICKER_LIT_SHARE = 0.55         # lit windows that gutter
FLICKER_RESTLESS_SHARE = 0.12    # amber lit windows that are restless instead
FLICKER_WAKING_SHARE = 0.04      # dark windows that wake now and then
# Preview colours (the lit frame; a waking window is drawn dark, as it mostly is).
WINDOW_PREVIEW_COLOURS = {
    "guttering_amber_window": ("FFB45A", 1, 1), "restless_amber_window": ("E08A2C", 1, 1),
    "guttering_cyan_window": ("5FF3FF", 1, 1), "guttering_violet_window": ("B77CFF", 1, 1),
    "waking_amber_window": ("1A2230", 1, 0), "waking_cyan_window": ("1A2230", 1, 0),
    "waking_violet_window": ("1A2230", 1, 0),
}


def note_window(x: int, y: int, z: int, lit_id: int) -> None:
    """A facade builder laid a window cell (lit or not) in a building whose
    lit windows are `lit_id`."""
    WINDOW_CELLS.append((int(x), int(y), int(z), int(lit_id)))


def _window_colour(c: City, lit_id: int) -> str | None:
    name = c.name_of(lit_id)
    for colour in ("amber", "cyan", "violet"):
        if colour in name:
            return colour
    return None


def place_flickering_windows(c: City, k: Kit) -> None:
    PREVIEW_COLOURS.update(WINDOW_PREVIEW_COLOURS)
    glass = {k.glass}
    cells = {}
    for (x, y, z, lit_id) in WINDOW_CELLS:
        cells[(x, y, z)] = lit_id
    done = set()
    placed = {"lit": 0, "restless": 0, "waking": 0}
    for (x, y, z) in sorted(cells):
        if (x, y, z) in done:
            continue
        lit_id = cells[(x, y, z)]
        # The window: the run of noted cells up this column from here.
        run = []
        yy = y
        while (x, yy, z) in cells and cells[(x, yy, z)] == lit_id:
            run.append(yy)
            done.add((x, yy, z))
            yy += 1
        colour = _window_colour(c, lit_id)
        if colour is None:
            continue
        # The window as it stands after weathering: all lit, all dark glass,
        # or neither (broken, a ruin took it) — only whole windows change.
        ids = [c.get(x, yy, z) for yy in run]
        if all(i == lit_id for i in ids):
            state = "lit"
        elif all(i in glass for i in ids):
            state = "dark"
        else:
            continue
        roll = zlib.crc32(f"aurelith/window/{x}/{run[0]}/{z}".encode()) / 0xFFFFFFFF
        block = None
        if state == "lit":
            if colour == "amber" and roll < FLICKER_RESTLESS_SHARE:
                block, kind = "restless_amber_window", "restless"
            elif roll < FLICKER_RESTLESS_SHARE + FLICKER_LIT_SHARE:
                block, kind = f"guttering_{colour}_window", "lit"
        elif roll < FLICKER_WAKING_SHARE:
            block, kind = f"waking_{colour}_window", "waking"
        if block is None:
            continue
        bid = c.b(block)
        for yy in run:
            c.set(x, yy, z, bid)
        placed[kind] += 1
    print(f"flickering windows: {placed['lit']} guttering, {placed['restless']} restless, "
          f"{placed['waking']} waking (of {len(cells)} window cells)")


# ── preview renderer ─────────────────────────────────────────────────────────
# A vectorised voxel ray-caster (Amanatides-Woo DDA over the numpy volume) so
# the city can be SEEN while it is designed: night lighting as the engine
# draws it (non-emissive faces at the Hush's midnight dim with MC's face
# shades, emissive blocks undimmed), translucent water and glass composited,
# teal fog, the sky with stars and aurora, bloom on everything that glows,
# the gate beams and the Heart's rings drawn as the renderers draw them.
# Colours are the textures' mean colours (tools/gen_aurelith_textures.py
# --colours) and the Hush's existing blocks'.
PREVIEW_COLOURS = {
    # name: (hex, alpha, emissive)
    "choirstone": ("C1C5BE", 1, 0), "polished_choirstone": ("CBCEC6", 1, 0),
    "choirstone_bricks": ("B8BBB4", 1, 0), "cracked_choirstone_bricks": ("AFB4AD", 1, 0),
    "chiseled_choirstone": ("BFC2BB", 1, 0), "choirstone_tiles": ("BBBEB7", 1, 0),
    "choirstone_pillar": ("C5C8C0", 1, 0), "polished_choirstone_stairs": ("CBCEC6", 1, 0),
    "polished_choirstone_slab": ("CBCEC6", 1, 0), "choirstone_brick_stairs": ("B8BBB4", 1, 0),
    "choirstone_brick_slab": ("B8BBB4", 1, 0), "choirstone_brick_wall": ("B8BBB4", 1, 0),
    "choirstone_tile_stairs": ("BBBEB7", 1, 0), "choirstone_tile_slab": ("BBBEB7", 1, 0),
    "stave_stone": ("2E8A9C", 1, 1), "nightglass": ("203545", 0.6, 0),
    # The dormant lights (half their voice) and the quest's blocks.
    "dim_stave_stone": ("1C5563", 1, 1), "dim_cyan_lumen_panel": ("2C8FA6", 1, 1),
    "dim_violet_lumen_panel": ("6A45AA", 1, 1), "dim_amber_lumen_panel": ("A8642A", 1, 1),
    "dim_lumen_strip": ("286E80", 1, 1), "dim_choir_lamp": ("5AA8B8", 1, 1),
    "chord_socket": ("4A4468", 1, 0), "voice_pedestal": ("C2C5BD", 1, 0),
    "choir_cabinet": ("4A4058", 1, 0),
    "resonite_grate": ("6E6696", 1, 0), "cyan_lumen_panel": ("5FF3FF", 1, 1),
    "violet_lumen_panel": ("B77CFF", 1, 1), "amber_lumen_panel": ("FFB45A", 1, 1),
    "lumen_strip": ("48C8DC", 1, 1), "crystal_conduit": ("68D3E6", 1, 1),
    "choir_lamp": ("C6FCFF", 1, 1), "resonant_water": ("7A5CE6", 0.62, 1),
    "resonance_engine": ("9FE8FF", 1, 1), "voice_beacon": ("C8F8FF", 1, 1),
    "resonant_crystal": ("57D6E0", 1, 1), "resonant_cluster": ("57D6E0", 1, 1),
    "resonite_block": ("3F4E63", 1, 0), "hushstone": ("3C4248", 1, 0),
    "polished_hushstone": ("454C53", 1, 0), "hushstone_bricks": ("3E454C", 1, 0),
    "cracked_hushstone_bricks": ("3A4046", 1, 0), "chiseled_hushstone_bricks": ("40474E", 1, 0),
    "hushstone_brick_stairs": ("3E454C", 1, 0), "hushstone_brick_slab": ("3E454C", 1, 0),
    "sculk": ("0D2530", 1, 0), "sculk_vein": ("0F2E3A", 1, 0), "sculk_catalyst": ("1B3A44", 1, 0),
    "sculk_sensor": ("0F3440", 1, 0), "sculk_shrieker": ("C8C6B0", 1, 0),
    "sculk_loam": ("243238", 1, 0), "hush_moss": ("1E4A4C", 1, 0), "hush_grass": ("2A6468", 1, 0),
    "resonance_bloom": ("7FF0FF", 1, 1), "lantern_leaves": ("3FB7A8", 1, 1),
    "whisperwood_log": ("3B3446", 1, 0), "whisperwood_planks": ("4A4058", 1, 0),
    "whisperwood_slab": ("4A4058", 1, 0), "whisperwood_stairs": ("4A4058", 1, 0),
    "whisperwood_fence": ("4A4058", 1, 0), "whisperwood_door": ("4A4058", 1, 0),
    "whisperwood_trapdoor": ("4A4058", 1, 0), "stripped_whisperwood_log": ("5A4E6A", 1, 0),
    "echo_lantern": ("7FE9F0", 1, 1), "iron_chain": ("3A3F44", 1, 0),
    "bookshelf": ("6B5236", 1, 0), "lectern": ("8A6A44", 1, 0), "chest": ("8C6433", 1, 0),
    "barrel": ("6E5234", 1, 0), "note_block": ("5A3E2C", 1, 0), "resonant_chime": ("7FE0F0", 1, 1),
    "spruce_wall_sign": ("5A4630", 1, 0), "cobweb": ("D0D0D0", 0.3, 0),
    "cyan_carpet": ("158B94", 1, 0), "purple_carpet": ("6A2A9C", 1, 0),
    "cyan_bed": ("158B94", 1, 0), "purple_bed": ("6A2A9C", 1, 0), "white_candle": ("E0E0D8", 1, 0),
    "flower_pot": ("7A4430", 1, 0), "decorated_pot": ("8A5A40", 1, 0),
    "amethyst_cluster": ("A77CE0", 1, 1), "amethyst_block": ("8A60C8", 1, 0),
    "reinforced_deepslate": ("4A4E52", 1, 0), "deepslate_tiles": ("36363A", 1, 0),
    "calcite": ("DFE0DC", 1, 0), "crying_obsidian": ("2A0E48", 1, 1),
    "water": ("2A5A8A", 0.6, 0), "vine": ("1E5A40", 1, 0), "glow_lichen": ("6EA890", 1, 1),
    "moss_carpet": ("1E4A4C", 1, 0), "hanging_roots": ("5A3E36", 1, 0),
    "iron_bars": ("5A5E62", 0.7, 0), "ladder": ("7A5A36", 1, 0), "crafting_table": ("7A5A36", 1, 0),
    "cartography_table": ("6A5030", 1, 0), "loom": ("8A7050", 1, 0), "bell": ("D8B040", 1, 0),
    "skeleton_skull": ("D8D8C8", 1, 0), "cauldron": ("3A3A3E", 1, 0), "echo_heart": ("7FF0FF", 1, 1),
    "end_rod": ("F0F0F0", 1, 1), "sea_lantern": ("C8E8E0", 1, 1),
}
NIGHT = 0.30                 # the engine's sky dim at the Hush's midnight (~0.27)
FOG_RGB = np.array([0.035, 0.115, 0.125])
SHADE = {0: 0.6, 1: 1.0, 2: 0.8}      # x-faces, y top, z-faces (MC); y bottom 0.5


def _hex(h: str) -> np.ndarray:
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], dtype=float) / 255.0


# Reawakening the Heart: render the city as it looks once awake (--awake):
# every dim_* light drawn as its lit twin, the four gate beams bent together
# into the pillar over the Heart.
PREVIEW_AWAKE = False


def preview_tables(c: City):
    n = len(c.palette) + 3
    col = np.zeros((n, 3)); alpha = np.zeros(n); emis = np.zeros(n, bool)
    for i, (name, _props) in enumerate(c.palette):
        if i <= AIR:
            continue
        short = name.split(":")[1]
        if PREVIEW_AWAKE and short.startswith("dim_"):
            short = short[len("dim_"):]
        entry = PREVIEW_COLOURS.get(short)
        if entry is None:
            entry = ("808080", 1, 0)
        col[i] = _hex(entry[0]); alpha[i] = entry[1]; emis[i] = bool(entry[2])
    # Synthetic terrain: grass top, loam, stone.
    GRASS, LOAM, ROCK = n - 3, n - 2, n - 1
    col[GRASS] = _hex("24524E"); alpha[GRASS] = 1
    col[LOAM] = _hex("243238"); alpha[LOAM] = 1
    col[ROCK] = _hex("3C4248"); alpha[ROCK] = 1
    return col, alpha, emis, (GRASS, LOAM, ROCK)


def preview_grid(c: City, terrain_ids) -> np.ndarray:
    """The render volume: the city, with unset cells resolved the way the
    terrain would most likely fill them (flat ground at the street level
    outside, solid under the paving inside)."""
    GRASS, LOAM, ROCK = terrain_ids
    g = c.v.astype(np.int32).copy()
    g[g == AIR] = 0
    unset = c.v == UNSET
    ys = np.arange(HY)[None, :, None]
    below = unset & (ys < S)
    top = unset & (ys == S)
    g[below] = LOAM
    g[top] = GRASS
    g[unset & (ys < S - 4)] = ROCK
    return g


def _sky(dirs: np.ndarray) -> np.ndarray:
    """Night sky: dark teal horizon to near-black zenith, cyan stars, faint
    aurora curtains (the Hush's AuroraRenderer, roughly)."""
    up = np.clip(dirs[..., 1], -1, 1)
    h = np.clip(up, 0, 1)[..., None]
    base = (1 - h) * np.array([0.030, 0.085, 0.095]) + h * np.array([0.004, 0.016, 0.024])
    az = np.arctan2(dirs[..., 2], dirs[..., 0])
    # Stars: hash of the direction on a fine grid.
    q = np.floor(dirs * 400.0).astype(np.int64)
    hsh = (q[..., 0] * 73856093) ^ (q[..., 1] * 19349663) ^ (q[..., 2] * 83492791)
    star = ((hsh % 997) == 0) & (up > 0.05)
    base = base + star[..., None] * np.array([0.55, 0.85, 0.9])
    # Aurora: curtains between 15 and 55 degrees elevation.
    el = np.degrees(np.arcsin(np.clip(up, -1, 1)))
    band = np.exp(-((el - 32) / 12.0) ** 2)
    wave = 0.5 + 0.5 * np.sin(az * 5.0 + np.sin(az * 2.0) * 2.0)
    streak = 0.6 + 0.4 * np.sin(az * 57.0 + el * 0.3)
    a = (band * wave * streak)[..., None]
    aur = a * (np.array([0.05, 0.28, 0.24]) * (1 - (el[..., None] - 15) / 60)
               + np.array([0.12, 0.05, 0.22]) * ((el[..., None] - 15) / 60))
    return base + np.clip(aur, 0, 1) * 0.55


def raycast(grid: np.ndarray, col, alpha, emis, origins: np.ndarray, dirs: np.ndarray,
            max_t: float = 520.0, max_layers: int = 3):
    """Cast every ray through the volume. Returns (rgb, emissive rgb, depth)."""
    n = origins.shape[0]
    out = np.zeros((n, 3)); out_e = np.zeros((n, 3))
    trans = np.ones(n)                       # remaining transmittance
    depth = np.full(n, np.inf)
    d = dirs.copy()
    d[np.abs(d) < 1e-9] = 1e-9
    inv = 1.0 / d
    lo = np.array([0.0, 0.0, 0.0]); hi = np.array([W, HY, D], dtype=float)
    t1 = (lo - origins) * inv; t2 = (hi - origins) * inv
    tnear = np.max(np.minimum(t1, t2), axis=1); tfar = np.min(np.maximum(t1, t2), axis=1)
    t_enter = np.maximum(tnear, 0.0)
    hit_box = tfar > t_enter
    idx = np.nonzero(hit_box)[0]
    # Entry voxel and the first crossed axis.
    p = origins[idx] + d[idx] * (t_enter[idx, None] + 1e-4)
    vox = np.floor(p).astype(np.int64)
    vox = np.clip(vox, [0, 0, 0], [W - 1, HY - 1, D - 1])
    step = np.sign(d[idx]).astype(np.int64)
    tdelta = np.abs(inv[idx])
    nxt = vox + (step > 0)
    tmax = (nxt - origins[idx]) * inv[idx]
    t = t_enter[idx].copy()
    last_axis = np.argmax(np.minimum(t1, t2)[idx], axis=1)
    layers = np.zeros(len(idx), np.int32)
    last_id = np.full(len(idx), -1, np.int64)
    active = np.arange(len(idx))
    it = 0
    while active.size and it < 2000:
        it += 1
        v = vox[active]
        ids = grid[v[:, 0], v[:, 1], v[:, 2]]
        hit = ids > 0
        if hit.any():
            a_hit = active[hit]
            hid = ids[hit]
            ray = idx[a_hit]
            same = last_id[a_hit] == hid        # inside a run of the same translucent block
            ax = last_axis[a_hit]
            sgn = step[a_hit, ax]
            shade = np.where(ax == 1, np.where(sgn < 0, 1.0, 0.5), np.where(ax == 0, 0.6, 0.8))
            base = col[hid] * shade[:, None]
            em = emis[hid]
            lit = np.where(em[:, None], base, base * NIGHT)
            al = alpha[hid]
            al = np.where(same, 0.0, al)
            tt = t[a_hit]
            fog = np.clip((tt - 40.0) / 300.0, 0, 1)[:, None] ** 1.2
            contrib = lit * (1 - fog) + FOG_RGB * fog
            w = (trans[ray] * al)[:, None]
            out[ray] += w * contrib
            out_e[ray] += w * np.where(em[:, None], lit, 0.0) * (1 - fog)
            first = np.isinf(depth[ray]) & (al > 0.5)
            depth[ray[first]] = tt[first]
            trans[ray] *= (1 - al)
            last_id[a_hit] = hid
            layers[a_hit] += (~same).astype(np.int32)
            done_mask = (trans[ray] < 0.02) | (layers[a_hit] >= max_layers)
            # Rays that stopped leave the active set.
            keep = np.ones(active.size, bool)
            hit_pos = np.nonzero(hit)[0]
            keep[hit_pos[done_mask]] = False
            active = active[keep]
            last_id_reset = None
        # rays in empty cells reset the run tracker
        if active.size == 0:
            break
        v = vox[active]
        ids = grid[v[:, 0], v[:, 1], v[:, 2]]
        last_id[active[ids == 0]] = -1
        # Advance one voxel.
        tm = tmax[active]
        ax = np.argmin(tm, axis=1)
        rows = np.arange(active.size)
        t[active] = tm[rows, ax]
        vox[active, ax] += step[active, ax]
        tmax[active, ax] += tdelta[active, ax]
        last_axis[active] = ax
        vv = vox[active]
        inside = (vv[:, 0] >= 0) & (vv[:, 0] < W) & (vv[:, 1] >= 0) & (vv[:, 1] < HY) \
            & (vv[:, 2] >= 0) & (vv[:, 2] < D) & (t[active] < max_t)
        active = active[inside]
    # Whatever is left: the ground plane (outside the volume) or the sky.
    rem = trans > 0.02
    if rem.any():
        ri = np.nonzero(rem)[0]
        dd = d[ri]; oo = origins[ri]
        tg = (S + 1 - oo[:, 1]) / dd[:, 1]
        ground = (dd[:, 1] < 0) & (tg > 0)
        # Only rays that did not already hit the ground inside the volume.
        gi = ri[ground]
        tgg = tg[ground]
        gp = oo[ground] + dd[ground] * tgg[:, None]
        rough = 0.9 + 0.1 * np.sin(gp[:, 0] * 0.21) * np.cos(gp[:, 2] * 0.17)
        gcol = _hex("24524E") * NIGHT * rough[:, None]
        fog = np.clip((tgg - 40.0) / 300.0, 0, 1)[:, None] ** 1.2
        out[gi] += trans[gi, None] * (gcol * (1 - fog) + FOG_RGB * fog)
        depth[gi] = np.minimum(depth[gi], tgg)
        trans[gi] = 0
        si = ri[~ground]
        out[si] += trans[si, None] * _sky(d[si])
    return out, out_e, depth


class Camera:
    def __init__(self, pos, yaw_deg, pitch_deg, fov_deg=70.0, ortho=None):
        self.pos = np.array(pos, dtype=float)
        self.yaw = math.radians(yaw_deg)       # 0 = looking north (-z), 90 = east
        self.pitch = math.radians(pitch_deg)   # negative = looking down
        self.fov = math.radians(fov_deg)
        self.ortho = ortho                     # width in blocks for an orthographic view

    def basis(self):
        f = np.array([math.sin(self.yaw) * math.cos(self.pitch), math.sin(self.pitch),
                      -math.cos(self.yaw) * math.cos(self.pitch)])
        right = np.array([math.cos(self.yaw), 0.0, math.sin(self.yaw)])
        up = np.cross(right, f)
        return f, right, up

    def rays(self, w: int, h: int):
        f, right, up = self.basis()
        xs = (np.arange(w) + 0.5) / w * 2 - 1
        ys = 1 - (np.arange(h) + 0.5) / h * 2
        gx, gy = np.meshgrid(xs, ys)
        if self.ortho:
            half_w = self.ortho / 2; half_h = half_w * h / w
            o = self.pos + gx[..., None] * right * half_w + gy[..., None] * up * half_h
            dirs = np.broadcast_to(f, o.shape).copy()
            return o.reshape(-1, 3), dirs.reshape(-1, 3)
        tan = math.tan(self.fov / 2)
        dirs = f + gx[..., None] * right * tan + gy[..., None] * up * tan * h / w
        dirs /= np.linalg.norm(dirs, axis=-1, keepdims=True)
        o = np.broadcast_to(self.pos, dirs.shape)
        return o.reshape(-1, 3).copy(), dirs.reshape(-1, 3)

    def project(self, pts: np.ndarray, w: int, h: int):
        """World points -> (px, py, depth); depth <= 0 is behind the camera."""
        f, right, up = self.basis()
        rel = pts - self.pos
        z = rel @ f
        if self.ortho:
            half_w = self.ortho / 2; half_h = half_w * h / w
            sx = (rel @ right) / half_w; sy = (rel @ up) / half_h
        else:
            tan = math.tan(self.fov / 2)
            zz = np.where(z > 1e-3, z, 1e-3)
            sx = (rel @ right) / (zz * tan); sy = (rel @ up) / (zz * tan * h / w)
        px = (sx + 1) / 2 * w; py = (1 - sy) / 2 * h
        return px, py, z


def _splat(img, px, py, rgb, sigma_px, depth_ok):
    """Additive soft dots (beams, rings) where the depth test passed."""
    h, w, _ = img.shape
    for x, y, c, ok in zip(px, py, rgb, depth_ok):
        if not ok or x < -20 or y < -20 or x > w + 20 or y > h + 20:
            continue
        xi, yi = int(x), int(y)
        if 0 <= xi < w and 0 <= yi < h:
            img[yi, xi] += c


def render_view(c: City, cam: Camera, w: int, h: int, path: Path, beams: list, heart) -> None:
    from PIL import Image
    from scipy.ndimage import gaussian_filter
    col, alpha, emis, terrain = preview_tables(c)
    grid = preview_grid(c, terrain)
    o, d = cam.rays(w, h)
    rgb, erg, depth = raycast(grid, col, alpha, emis, o, d)
    img = rgb.reshape(h, w, 3)
    em = erg.reshape(h, w, 3)
    dep = depth.reshape(h, w)
    glow = np.zeros_like(img)
    # Sky beams: a column of light from each voice beacon — or, awake, the
    # arc each bends into up to the meeting point over the Heart (Voice-
    # BeaconRenderer's quadratic), and the Heart's pillar on up from there.
    for (bx, by, bz, colour) in beams:
        ys = np.arange(by + 1.0, by + 257.0, 0.5)
        pts = np.stack([np.full_like(ys, bx + 0.5), ys, np.full_like(ys, bz + 0.5)], axis=1)
        if PREVIEW_AWAKE and heart is not None:
            a = np.array([bx + 0.5, by + 1.0, bz + 0.5])
            meet = np.array([heart[0] + 0.5, heart[1] + 90.0, heart[2] + 0.5])
            ctrl = np.array([a[0], meet[1] + 12.0, a[2]])
            t = np.linspace(0.0, 1.0, 600)[:, None]
            pts = a * (1 - t) ** 2 + ctrl * 2 * (1 - t) * t + meet * t ** 2
            ys = by + 1.0 + t[:, 0] * 60.0          # fade coordinate: barely fades
        px, py, z = cam.project(pts, w, h)
        ok = (z > 0)
        pxi = px.astype(int); pyi = py.astype(int)
        inb = ok & (pxi >= 0) & (pxi < w) & (pyi >= 0) & (pyi < h)
        vis = inb.copy()
        vis[inb] = dep[pyi[inb], pxi[inb]] > z[inb] - 1.0
        fade = np.clip(1.0 - (ys - by) / 256.0, 0, 1) ** 0.6
        for i in np.nonzero(vis)[0]:
            glow[pyi[i], pxi[i]] += _hex(colour) * 0.9 * fade[i]
    # The Heart's rings (AurelithHeartRenderer: radii, tilts at t = 0).
    if heart is not None and PREVIEW_AWAKE:
        hx, hy, hz = heart
        ys = np.arange(hy + 24.0, hy + 256.0, 0.5)
        pts = np.stack([np.full_like(ys, hx + 0.5), ys, np.full_like(ys, hz + 0.5)], axis=1)
        px, py, z = cam.project(pts, w, h)
        pxi = px.astype(int); pyi = py.astype(int)
        inb = (z > 0) & (pxi >= 0) & (pxi < w) & (pyi >= 0) & (pyi < h)
        vis = inb.copy()
        vis[inb] = dep[pyi[inb], pxi[inb]] > z[inb] - 1.0
        fade = np.clip(1.0 - (ys - hy) / 256.0, 0, 1) ** 0.5
        for i in np.nonzero(vis)[0]:
            for dxp in (-1, 0, 1):
                if 0 <= pxi[i] + dxp < w:
                    glow[pyi[i], pxi[i] + dxp] += _hex("E8FFFF") * 1.4 * fade[i]
    if heart is not None:
        hx, hy, hz = heart
        centre = np.array([hx + 0.5, hy + 1 + 11.5, hz + 0.5])
        for (radius, tilt, spin) in ((2.6, 40, 0.3), (5.0, 8, 1.1), (8.0, 23, 2.0), (12.0, 52, 0.7)):
            th = np.linspace(0, 2 * np.pi, int(radius * 60))
            ring = np.stack([np.cos(th) * radius, np.zeros_like(th), np.sin(th) * radius], axis=1)
            tl = math.radians(tilt)
            rot_x = np.array([[1, 0, 0], [0, math.cos(tl), -math.sin(tl)], [0, math.sin(tl), math.cos(tl)]])
            rot_y = np.array([[math.cos(spin), 0, math.sin(spin)], [0, 1, 0], [-math.sin(spin), 0, math.cos(spin)]])
            pts = ring @ rot_x.T @ rot_y.T + centre
            gap = (np.floor(th / (2 * np.pi) * int(radius * 3)) % 7) == 3      # missing staves
            px, py, z = cam.project(pts, w, h)
            pxi = px.astype(int); pyi = py.astype(int)
            inb = (z > 0) & (pxi >= 0) & (pxi < w) & (pyi >= 0) & (pyi < h) & ~gap
            vis = inb.copy()
            vis[inb] = dep[pyi[inb], pxi[inb]] > z[inb] - 0.5
            for i in np.nonzero(vis)[0]:
                glow[pyi[i], pxi[i]] += _hex("7FF6FF") * 1.6
        px, py, z = cam.project(centre[None, :], w, h)
        if z[0] > 0 and 0 <= px[0] < w and 0 <= py[0] < h:
            glow[int(py[0]), int(px[0])] += np.array([6.0, 9.0, 9.0])
    bloom_src = em * 1.0 + glow
    bloom = gaussian_filter(bloom_src, sigma=(2.0, 2.0, 0)) * 0.9 \
        + gaussian_filter(bloom_src, sigma=(9.0, 9.0, 0)) * 0.6 \
        + gaussian_filter(glow, sigma=(1.0, 1.0, 0)) * 2.0
    final = img + bloom
    final = 1 - np.exp(-final * 1.6)            # soft tone map
    final = np.clip(final ** (1 / 1.05), 0, 1)
    Image.fromarray((final * 255).astype(np.uint8)).save(path)
    print(f"preview: {path}")


def render_map(c: City, path: Path, scale: int = 4) -> None:
    """Top-down night map: an orthographic ray-cast straight down, plus a
    labelled daylight-ish plan for reading the layout."""
    cam = Camera((CX, HY + 20, CZ), 0.0, -90.0, ortho=W)
    # Straight-down basis: yaw 0 / pitch -90 makes 'up' point north (-z).
    render_view(c, cam, W * scale, D * scale, path, beams=[], heart=None)


# ── finishing passes ─────────────────────────────────────────────────────────
def finalize_air(c: City) -> None:
    """The walking volume: every still-unset cell a few blocks over the
    walled city's paving becomes explicit air, so grass, snow or a bump of
    terrain never stands in a street."""
    for y in range(S + 1, S + CLEAR + 1):
        layer = c.v[:, y, :]
        layer[INSIDE & (layer == UNSET)] = AIR


def build_city() -> tuple[City, Kit]:
    BUILT.clear()
    HOUSES.clear()
    WINDOW_CELLS.clear()
    c = City()
    k = Kit(c)
    build_ground(c, k)
    paint_streets(c, k)
    paint_avenues(c, k)
    build_river(c, k)
    build_bridges(c, k)
    build_plaza(c, k)
    build_gates(c, k)
    build_towers(c, k)
    build_statues(c, k)
    build_vault(c, k)
    free = free_mask() & ~reserved_mask()
    free &= ~build_crescents(c, k, free)
    free &= ~build_gardens(c, k, free)
    free &= ~build_stillhouses(c, k, free & (XS < CX))
    free &= ~build_arcade(c, k, free & (XS > CX))
    build_harbour(c, k)
    build_fill(c, k, free)
    plant_quays(c, k)
    place_street_lamps(c, k)
    corner_signs(c, k)
    build_details(c, k)
    weather(c, k)
    build_quest(c, k)
    place_flickering_windows(c, k)
    finalize_air(c)
    place_inhabitants(c, k)
    return c, k


def voice_beacons(c: City) -> list:
    out = []
    colours = {"north": "DFFFFF", "east": "5FF3FF", "south": "B77CFF", "west": "6F7BFF"}
    for (x, y, z), tag in c.nbt.items():
        if tag.v.get("id") and tag.v["id"].v == "minecraft:voice_beacon":
            facing = c.props_of(c.get(x, y, z)).get("facing", "north")
            out.append((x, y, z, colours[facing]))
    return out


def render_previews(c: City, only: str | None = None) -> None:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    beams = voice_beacons(c)
    heart = (CX, S + 5, CZ)
    views = {
        "map": None,
        "aerial_sw": Camera((CX - 175, S + 150, CZ + 190), 40.0, -30.0, fov_deg=58),
        "aerial_ne": Camera((CX + 190, S + 120, CZ - 170), 228.0, -24.0, fov_deg=58),
        "approach": Camera((CX + 60, S + 26, CZ + 360), 352.0, 1.0, fov_deg=60),
        "plaza": Camera((CX - 3, S + 3.6, CZ + 27), 358.0, 14.0, fov_deg=78),
        "avenue": Camera((CX + 2, S + 2.6, CZ - 90), 180.0, 5.0, fov_deg=75),
        "river": Camera((79.5, S + 6.4, float(river_z(78))), 92.0, -16.0, fov_deg=78),
        "spire_view": Camera((SPIRE[0] - 3, S + 75.6, SPIRE[1] + 3), 215.0, -22.0, fov_deg=80),
        "arcade": Camera((CX + 14, S + 2.6, 192), 90.0, 3.0, fov_deg=75),
        "gate_south": Camera((CX + 0.5, S + 2.6, CZ + 122), 0.0, 7.0, fov_deg=72),
        "crest": Camera((CX - 60, S + 34, CZ + 230), 15.0, -6.0, fov_deg=55),
        # Reawakening the Heart: the Podium's sockets from the dais, the
        # Hall of Instruments' cabinet bay, the Tuners' mast.
        "podium": Camera((CX + 7.5, S + 6.5, CZ - 3.5), 330.0, -18.0, fov_deg=70),
        "alto_bay": Camera((ALTO_CABINET[0] - 4.6, S + 3.6, ALTO_CHIMES_Z + 0.35), 117.0, -22.0, fov_deg=92),
        "tenor_mast": Camera((TENOR_MAST[0] - 18, S + 34, TENOR_MAST[1] + 22), 140.0, -8.0, fov_deg=60),
    }
    for name, cam in views.items():
        if only and name not in only.split(","):
            continue
        path = SCRATCH / (f"{name}_awake.png" if PREVIEW_AWAKE else f"{name}.png")
        if cam is None:
            render_map(c, path)
        else:
            render_view(c, cam, 1280, 720, path, beams, heart)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", action="store_true", help="render previews into the scratchpad")
    ap.add_argument("--preview-only", action="store_true", help="render previews, write nothing")
    ap.add_argument("--views", default=None, help="comma list of preview names")
    ap.add_argument("--awake", action="store_true",
                    help="previews of the awakened city (lit lights, the beams bent into the pillar)")
    ap.add_argument("--check-blocks", action="store_true",
                    help="cross-check every palette block against Blocks.cpp; exit 1 on a miss")
    ap.add_argument("--outdir", type=Path, default=REPO_ROOT / "data")
    ap.add_argument("--walk-check", action="store_true",
                    help="walk the city from the plaza and report what cannot be reached on foot")
    args = ap.parse_args()
    global PREVIEW_AWAKE
    PREVIEW_AWAKE = args.awake
    c, k = build_city()
    bake_connections(c)
    print(f"palette {len(c.palette)} entries; listed cells {int((c.v != UNSET).sum())}")
    if args.walk_check:
        walk_report(c)
    if args.preview or args.preview_only:
        render_previews(c, args.views)
    if args.preview_only or args.walk_check:
        return 0
    written = write_all(c, args.outdir)
    if args.check_blocks:
        bad = HS.check_blocks(written)
        bad += check_properties(c)
        return 1 if bad else 0
    return 0

# ── building library ─────────────────────────────────────────────────────────
N4 = ((1, 0), (-1, 0), (0, 1), (0, -1))
BUILT: list = []            # (top footprint, info) of every mid-rise, for weathering and the walk check
HOUSES: list = []           # (x0, z0, x1, z1) of every Stillhouse


def rect(x0: int, z0: int, x1: int, z1: int) -> np.ndarray:
    m = np.zeros((W, D), bool)
    m[max(x0, 0):min(x1, W - 1) + 1, max(z0, 0):min(z1, D - 1) + 1] = True
    return m


def disc(cx: float, cz: float, r: float) -> np.ndarray:
    return np.hypot(XS - cx, ZS - cz) <= r


def erode(m: np.ndarray) -> np.ndarray:
    e = m.copy()
    e[1:, :] &= m[:-1, :]; e[:-1, :] &= m[1:, :]
    e[:, 1:] &= m[:, :-1]; e[:, :-1] &= m[:, 1:]
    e[0, :] = False; e[-1, :] = False; e[:, 0] = False; e[:, -1] = False
    return e


def dilate(m: np.ndarray) -> np.ndarray:
    d = m.copy()
    d[1:, :] |= m[:-1, :]; d[:-1, :] |= m[1:, :]
    d[:, 1:] |= m[:, :-1]; d[:, :-1] |= m[:, 1:]
    return d


def outward_faces(m: np.ndarray, x: int, z: int) -> list[str]:
    """Directions in which the mask cell (x, z) faces outside the mask."""
    out = []
    for f, (dx, dz) in STEP.items():
        xx, zz = x + dx, z + dz
        if not (0 <= xx < W and 0 <= zz < D) or not m[xx, zz]:
            out.append(f)
    return out


def district_of(x: float, z: float) -> str:
    """The city's quarters by position: they set a building's accent light."""
    if z > 168:
        return "stillhouses" if x < CX else "arcade"
    if x < CX and z < CZ:
        return "listeners"          # the Archive's quarter: cool cyan
    if x >= CX and z < CZ:
        return "conductors"         # the Spire and Starward: cyan and violet
    return "river"                  # the quays: violet


DISTRICT_LIGHT = {
    # (lit-window panels to pick from, cornice kind weights)
    "listeners": (("cyan", "cyan", "amber"), {"strip": 3, "stave": 2, "plain": 2}),
    "conductors": (("violet", "cyan", "amber"), {"strip": 2, "stave": 3, "plain": 2}),
    "river": (("violet", "violet", "amber"), {"strip": 2, "stave": 1, "plain": 3}),
    "stillhouses": (("amber", "amber", "amber", "violet"), {"strip": 1, "stave": 1, "plain": 4}),
    "arcade": (("amber", "violet", "violet"), {"strip": 2, "stave": 1, "plain": 3}),
}


class Style:
    """A facade vocabulary. Every mid-rise picks one (seeded); the quarter
    it stands in picks the colour of the light left on in its windows and
    how its cornice glows (a lumen strip, a Stave inscription, or none)."""

    def __init__(self, k: Kit, kind: str, rng: random.Random, district: str = "listeners") -> None:
        self.kind = kind
        self.wall = {"classic": k.bricks, "glass": k.polished, "stepped": k.bricks,
                     "stone": k.stone, "hush": k.hush_bricks}[kind]
        self.corner = k.pillar_y if kind != "hush" else k.hush_polished
        self.band = k.polished if kind != "hush" else k.chiseled
        self.window = k.glass
        lights, cornices = DISTRICT_LIGHT[district]
        self.lit = {"cyan": k.cyan, "violet": k.violet, "amber": k.amber}[rng.choice(lights)]
        self.lit_rate = rng.uniform(0.05, 0.16)
        self.period = rng.choice([2, 2, 3])
        self.window_h = 2 if kind != "glass" else 3
        self.cornice = rng.choices(list(cornices), weights=list(cornices.values()))[0]
        self.lumen_bands = kind == "glass" or (kind == "stepped" and rng.random() < 0.5)


def stair_step(c: City, k: Kit, x: int, y: int, z: int, facing: str, fam: str = "polished") -> None:
    """One step of a walkable stair: the stair block, and three blocks of
    headroom cleared above it (which is also what cuts the stairwell through
    the slab overhead). The step and the cells round it stay clear of
    furniture, so the stair can always be reached."""
    for yy in range(y + 1, y + 4):
        c.set(x, yy, z, AIR)
    c.set(x, y, z, k.stairs(fam, facing))
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            c.keep_clear.add((x + dx, z + dz))


def furnish_room(c: City, k: Kit, cells: list[tuple[int, int]], y: int, rng: random.Random,
                 loot: str | None, book: str | None, dense: float = 0.12,
                 kind: str | None = None) -> None:
    """Scatter the life that was left behind over a room's free floor cells
    (y = standing level): shelves and lecterns against walls, a table and
    chairs, a chest or barrel with the district's loot, carpets. Cells must
    be interior (air at y..y+2). With a `kind` the room is laid out by that
    room kit instead (furnish_kit, "lived-in interiors" below)."""
    if kind:
        furnish_kit(c, k, cells, y, rng, kind, loot, book)
        return
    cells = [p_ for p_ in cells if p_ not in c.keep_clear]
    if not cells:
        return
    rng.shuffle(cells)
    wallside = [(x, z) for (x, z) in cells
                if any(c.get(x + dx, y, z + dz) not in (AIR, UNSET) for dx, dz in N4)]
    if loot and wallside:
        x, z = wallside.pop()
        face = [f for f, (dx, dz) in STEP.items() if c.get(x + dx, y, z + dz) == AIR]
        facing = face[0] if face else "north"
        if rng.random() < 0.7:
            c.set(x, y, z, c.b("chest", facing=facing, type="single", waterlogged="false"),
                  nbt=loot_nbt(loot))
        else:
            c.set(x, y, z, c.b("barrel", facing="up", open="false"), nbt=loot_nbt(loot, "minecraft:barrel"))
    if book and wallside:
        x, z = wallside.pop()
        face = [f for f, (dx, dz) in STEP.items() if c.get(x + dx, y, z + dz) == AIR]
        facing = face[0] if face else "north"
        c.set(x, y, z, c.b("lectern", facing=facing, has_book="true", powered="false"),
              nbt=lectern_nbt(book))
    for (x, z) in wallside[: max(1, int(len(wallside) * dense))]:
        r = rng.random()
        if r < 0.45:
            c.set(x, y, z, k.bookshelf)
            if rng.random() < 0.5:
                c.set(x, y + 1, z, k.bookshelf)
        elif r < 0.6:
            c.set(x, y, z, k.note)
        elif r < 0.75:
            c.set(x, y, z, c.b("flower_pot"))
        else:
            c.set(x, y, z, k.slab("ww", "bottom"))
    free = [(x, z) for (x, z) in cells if c.get(x, y, z) == AIR]
    if len(free) >= 6:
        # A table with two chairs.
        x, z = free[len(free) // 2]
        c.set(x, y, z, k.ww_slab_top)
        for f, (dx, dz) in (("east", (-1, 0)), ("west", (1, 0))):
            if c.get(x + dx, y, z + dz) == AIR and rng.random() < 0.8:
                c.set(x + dx, y, z + dz, k.stairs("ww", f))
        if rng.random() < 0.5 and c.get(x, y + 1, z) == AIR:
            c.set(x, y + 1, z, c.b("white_candle", candles="1", lit="false", waterlogged="false"))
    # Carpet runs.
    carpet = c.b(rng.choice(["cyan_carpet", "purple_carpet", "light_blue_carpet"]))
    for (x, z) in free[: len(free) // 3]:
        if c.get(x, y, z) == AIR and rng.random() < 0.5:
            c.set(x, y, z, carpet)


def build_midrise(c: City, k: Kit, mask: np.ndarray, floors: int, seed: str,
                  kind: str | None = None, floor_h: int = 4, ground_h: int = 5,
                  loot: str | None = None, book: str | None = None,
                  setback_at: int | None = None, roof: str = "auto",
                  colonnade: tuple[str, ...] = (), interior: str | None = None) -> dict:
    """A mid-rise block on an arbitrary footprint mask: pillar corners, a
    facade rhythm of dark nightglass windows (a few lit as amber or cyan
    lumen panels — the lamp someone left on), floor bands, lumen strips at
    the cornice and on some floors, a ground floor with doors on every face,
    a switchback stair from the street to the roof, furnished floors with a
    hanging lamp in each, a roof terrace with a railing (garden, pavilion or
    crystal needle). An optional setback steps the upper floors in by two
    with a planted terrace; `colonnade` faces get a two-deep covered walk of
    pillars at street level. Returns facts the caller may use."""
    rng = piece_rng(seed)
    pts0 = np.argwhere(mask)
    mcx, mcz = pts0.mean(axis=0) if len(pts0) else (CX, CZ)
    st = Style(k, kind or rng.choice(["classic", "classic", "glass", "stepped", "stone"]), rng,
               district_of(mcx, mcz))
    y0 = S + 1
    heights = [ground_h] + [floor_h] * (floors - 1)
    tops = np.cumsum(heights) + y0 - 1            # last room row of each floor
    slabs = [y0 - 1] + [int(t) + 1 for t in tops[:-1]]   # floor slab y per floor
    roof_y = int(tops[-1]) + 1
    setbacks = () if setback_at is None else ((setback_at,) if isinstance(setback_at, int) else tuple(setback_at))
    footprints = []
    m = mask.copy()
    for f in range(floors):
        if f in setbacks and erode(erode(erode(m))).sum() > 20:
            m = erode(erode(m))
        footprints.append(m.copy())
    info = {"roof_y": roof_y, "top": roof_y + 1}
    for f, fm in enumerate(footprints):
        edge = fm & ~erode(fm)
        inner = fm & ~edge
        ya = slabs[f] + 1
        yb = int(tops[f])
        # Slab (not for the ground floor: the paving is there) and the room.
        if f > 0:
            c.fill_mask(fm, slabs[f], slabs[f], st.band)
        else:
            c.fill_mask(inner, S, S, k.polished)
        c.fill_mask(inner, ya, yb, AIR)
        c.fill_mask(edge, ya, yb, st.wall)
        # Facade: corners are pillars, windows in the rhythm.
        cells = np.argwhere(edge)
        for (x, z) in cells:
            x, z = int(x), int(z)
            faces = outward_faces(fm, x, z)
            if len(faces) >= 2 or not faces:
                c.fill(x, ya, z, x, yb, z, st.corner)
                continue
            if f == 0:
                continue
            along = z if faces[0] in ("east", "west") else x
            wy0 = ya + 1
            if (along % st.period) == 0:
                for yy in range(wy0, min(wy0 + st.window_h, yb + 1)):
                    lit = rng.random() < st.lit_rate
                    c.set(x, yy, z, st.lit if lit else st.window)
                    note_window(x, yy, z, st.lit)
            if st.lumen_bands and f > 0:
                strip = k.strip_z if faces[0] in ("east", "west") else k.strip_x
                c.set(x, slabs[f], z, strip)
        # Doors on the ground floor: the middle cells of each straight face.
        if f == 0:
            for face in FACINGS:
                run = [(int(x), int(z)) for (x, z) in cells if outward_faces(fm, int(x), int(z)) == [face]]
                if len(run) < 3:
                    continue
                run.sort()
                mx, mz = run[len(run) // 2]
                ix, iz = STEP[OPP[face]]
                for (dx, dz) in ((0, 0), (1, 0) if face in ("north", "south") else (0, 1)):
                    if (mx + dx, mz + dz) in run:
                        c.fill(mx + dx, ya, mz + dz, mx + dx, ya + 2, mz + dz, AIR)
                        c.set(mx + dx, ya + 3, mz + dz, k.chiseled)
                        for n_ in (0, 1, 2):
                            c.keep_clear.add((mx + dx + ix * n_, mz + dz + iz * n_))
                # Shop windows either side of the door.
                for (x, z) in run:
                    if abs((x - mx) + (z - mz)) in (3, 4) and c.get(x, ya + 1, z) == st.wall:
                        c.fill(x, ya + 1, z, x, ya + 2, z, st.window if rng.random() > 0.2 else st.lit)
        # A hanging lamp in each floor's middle, glowing through the windows.
        pts = np.argwhere(inner)
        if len(pts):
            mx, mz = pts[len(pts) // 2]
            c.set(int(mx), yb, int(mz), k.lamp_hang)
    # The cornice over the top floor: a lumen strip, a Stave inscription
    # or plain polished stone, by the building's style.
    top = footprints[-1]
    edge = top & ~erode(top)
    c.fill_mask(top, roof_y, roof_y, st.band)
    for (x, z) in np.argwhere(edge):
        faces = outward_faces(top, int(x), int(z))
        if len(faces) == 1:
            if st.cornice == "strip":
                c.set(int(x), roof_y, int(z), k.strip_z if faces[0] in ("east", "west") else k.strip_x)
            elif st.cornice == "stave":
                c.set(int(x), roof_y, int(z), k.stave)
    # Parapet and the roof's life.
    c.fill_mask(top & ~edge, roof_y + 1, roof_y + 3, AIR)
    c.fill_mask(edge, roof_y + 1, roof_y + 1, k.wall())
    c.fill_mask(edge, roof_y + 2, roof_y + 3, AIR)
    for sb in setbacks:
        # Each setback terrace: planters along its outer edge.
        if sb >= floors or (footprints[sb - 1] == footprints[sb]).all():
            continue
        lower = footprints[sb - 1]
        terrace = lower & ~footprints[sb]
        t_edge = lower & ~erode(lower)
        ty = slabs[sb]
        c.fill_mask(terrace, ty, ty, st.band)
        c.fill_mask(terrace, ty + 1, ty + 3, AIR)
        c.fill_mask(t_edge & terrace, ty + 1, ty + 1, k.wall())
        planter = terrace & ~t_edge & dilate(t_edge)
        for (x, z) in np.argwhere(planter):
            if rng.random() < 0.45:
                c.set(int(x), ty, int(z), k.moss)
                c.set(int(x), ty + 1, int(z), rng.choice([k.grass, k.bloom, k.grass]))
    roof_kind = roof if roof != "auto" else rng.choice(
        ["garden", "pavilion", "needle", "plain", "garden", "dome", "crown", "mast"])
    inner_top = np.argwhere(top & ~edge)
    if len(inner_top):
        cx_, cz_ = inner_top[len(inner_top) // 2]
        cx_, cz_ = int(cx_), int(cz_)
        ext = min(int(np.ptp(inner_top[:, 0])), int(np.ptp(inner_top[:, 1])))
        if roof_kind == "garden":
            for (x, z) in inner_top:
                if rng.random() < 0.28:
                    c.set(int(x), roof_y, int(z), k.moss)
                    c.set(int(x), roof_y + 1, int(z), rng.choice([k.grass, k.grass, k.bloom]))
            if ext >= 4:
                plant_tree(c, k, cx_, cz_, roof_y + 1, rng, size=0)
        elif roof_kind == "pavilion" and ext >= 5:
            for (dx, dz) in ((-2, -2), (2, -2), (-2, 2), (2, 2)):
                if top[cx_ + dx, cz_ + dz]:
                    c.fill(cx_ + dx, roof_y + 1, cz_ + dz, cx_ + dx, roof_y + 3, cz_ + dz, k.pillar_y)
            c.fill(cx_ - 2, roof_y + 4, cz_ - 2, cx_ + 2, roof_y + 4, cz_ + 2, k.slab("polished"))
            c.set(cx_, roof_y + 3, cz_, k.lamp_hang)
            info["top"] = roof_y + 4
        elif roof_kind == "dome" and ext >= 6:
            # A ribbed nightglass dome over the middle of the roof.
            rd = min(ext // 2, 5) + 0.5
            for yy in range(int(rd) + 1):
                rr = math.sqrt(max(0.0, rd * rd - yy * yy))
                d_ = np.hypot(XS - cx_, ZS - cz_)
                shell = top & (d_ <= rr) & (d_ > rr - 1.3)
                c.fill_mask(shell, roof_y + 1 + yy, roof_y + 1 + yy, k.glass)
                ribs = shell & ((XS == cx_) | (ZS == cz_))
                c.fill_mask(ribs, roof_y + 1 + yy, roof_y + 1 + yy, k.polished)
            c.set(cx_, roof_y + 1 + int(rd), cz_, k.crystal)
            c.set(cx_, roof_y + 2 + int(rd), cz_, k.cluster_up)
            c.set(cx_, roof_y + int(rd) - 1, cz_, k.lamp_hang)
            info["top"] = roof_y + 2 + int(rd)
        elif roof_kind == "crown" and ext >= 4:
            # A stepped crown: two shrinking polished tiers and a crystal.
            for n_ in range(2):
                ring = top & erode(erode(top) if n_ == 0 else erode(erode(erode(top))))
                c.fill_mask(ring, roof_y + 1 + n_, roof_y + 1 + n_, k.polished)
                c.fill_mask(ring & ~erode(ring), roof_y + 1 + n_, roof_y + 1 + n_, k.stave)
            c.fill(cx_, roof_y + 3, cz_, cx_, roof_y + 5, cz_, k.crystal)
            info["top"] = roof_y + 5
        elif roof_kind == "mast":
            # A conduit mast with a lamp: the city's antennae.
            c.fill(cx_, roof_y + 1, cz_, cx_, roof_y + 6, cz_, k.conduit_y)
            c.set(cx_, roof_y + 7, cz_, k.lamp)
            info["top"] = roof_y + 7
        elif roof_kind == "needle":
            c.fill(cx_, roof_y + 1, cz_, cx_, roof_y + 5, cz_, k.strip_y)
            c.set(cx_, roof_y + 6, cz_, k.crystal)
            c.set(cx_, roof_y + 7, cz_, k.cluster_up)
            info["top"] = roof_y + 7
    if colonnade:
        _colonnade(c, k, footprints[0], colonnade, y0, y0 + ground_h - 1)
    # Stairs from the street to the roof: a switchback core two rows wide
    # and long enough for the tallest run, inside every floor's footprint
    # and clear of the colonnade's covered walk.
    levels = list(slabs) + [roof_y]
    core_len = max(b - a for a, b in zip(levels, levels[1:])) + 2
    inner_all = erode(footprints[-1])
    for _ in range(3 if colonnade else 0):
        inner_all = erode(inner_all) | (inner_all & ~dilate(footprints[0] & ~erode(footprints[0])))
    pts = [tuple(map(int, p)) for p in np.argwhere(inner_all)]
    rng.shuffle(pts)
    for along in ("x", "z"):
        spot = None
        for (sx, sz) in pts:
            if along == "x":
                cells_ = [(sx + u, sz + r) for u in range(core_len) for r in (0, 1)]
            else:
                cells_ = [(sx + r, sz + u) for u in range(core_len) for r in (0, 1)]
            if all(0 <= x < W and 0 <= z < D and inner_all[x, z] for (x, z) in cells_):
                spot = (sx, sz)
                break
        if spot:
            _core_stairs(c, k, spot[0], spot[1], along, levels)
            info["stairs"] = (spot, along)
            break
    if "stairs" not in info:
        # Too slender for a switchback: a ladder shaft against a pillar
        # from the street to the roof.
        pts = [tuple(map(int, p)) for p in np.argwhere(erode(footprints[-1]))]
        for (lx, lz) in pts:
            if (lx + 1, lz) in pts and (lx, lz) not in c.keep_clear:
                for yy in range(S + 1, roof_y + 1):
                    c.set(lx + 1, yy, lz, k.pillar_y)
                    c.set(lx, yy, lz, c.b("ladder", facing="west", waterlogged="false"))
                c.fill(lx, roof_y + 1, lz, lx, roof_y + 2, lz, AIR)
                c.set(lx - 1, S + 1, lz, AIR)
                for dx in (-1, 0):
                    c.keep_clear.add((lx + dx, lz))
                info["stairs"] = ((lx, lz), "ladder")
                break
    # Furniture on every floor.
    for f, fm in enumerate(footprints):
        inner = erode(fm)
        ya = slabs[f] + 1
        free = [(int(x), int(z)) for (x, z) in np.argwhere(inner)
                if c.get(int(x), ya, int(z)) == AIR and c.get(int(x), ya + 1, int(z)) == AIR
                and c.get(int(x), ya - 1, int(z)) not in (AIR, UNSET)]
        room_kind = interior if interior is not None else \
            interior_kind(district_of(mcx, mcz), f, floors, rng, bool(colonnade))
        furnish_room(c, k, free, ya, rng,
                     loot if (f == floors - 1 or f == 0) and rng.random() < 0.7 else None,
                     book if f == 0 else None, kind=room_kind or None)
    BUILT.append((footprints[-1], info))
    return info


def _colonnade(c: City, k: Kit, fm: np.ndarray, faces: tuple[str, ...], y0: int, y1: int) -> None:
    """Hollow out a two-deep covered walk along the given faces of the ground
    floor, leaving a pillar every third cell on the outer line and shop
    windows in the new facade behind."""
    edge = fm & ~erode(fm)
    for (x, z) in np.argwhere(edge):
        x, z = int(x), int(z)
        out = outward_faces(fm, x, z)
        if len(out) != 1 or out[0] not in faces:
            continue
        dx, dz = STEP[OPP[out[0]]]
        along = z if out[0] in ("east", "west") else x
        c.fill(x, y0, z, x, y1 - 1, z, AIR)
        c.fill(x + dx, y0, z + dz, x + dx, y1 - 1, z + dz, AIR)
        if along % 3 == 0:
            c.fill(x, y0, z, x, y1 - 1, z, k.pillar_y)
        # The shopfronts behind the walk: wall, a window every third block,
        # a door every sixth.
        bx, bz = x + 2 * dx, z + 2 * dz
        if fm[bx, bz]:
            c.fill(bx, y0, bz, bx, y1 - 1, bz, k.bricks)
            if along % 6 == 0:
                c.fill(bx, y0, bz, bx, y0 + 2, bz, AIR)
                for n_ in (0, 1, 2):
                    c.keep_clear.add((bx + dx * n_, bz + dz * n_))
            elif along % 3 == 1:
                c.fill(bx, y0 + 1, bz, bx, y0 + 2, bz, k.glass)
        if along % 6 == 3:
            c.set(x + dx, y1 - 1, z + dz, k.lamp_hang)


# ── the Four Gates ───────────────────────────────────────────────────────────
GATES = {
    # direction: (voice, sign key)
    "north": ("soprano", "soprano_gate"),
    "east": ("alto", "alto_gate"),
    "south": ("tenor", "tenor_gate"),
    "west": ("bass", "bass_gate"),
}


def gate_frame(direction: str):
    """(u, v) -> (x, z) for a gate: u runs along the wall, v outward from the
    city centre."""
    if direction == "north":
        return lambda u, v: (CX + u, CZ - v)
    if direction == "south":
        return lambda u, v: (CX - u, CZ + v)
    if direction == "east":
        return lambda u, v: (CX + v, CZ + u)
    return lambda u, v: (CX - v, CZ - u)


def build_gate(c: City, k: Kit, direction: str) -> None:
    """A gate of the Four Voices: two towers flanking an arch over the
    avenue, a bridge walk across the arch, and over it the voice spire with
    the voice beacon whose beam stands into the sky (VoiceBeaconRenderer;
    the beacon's facing is the gate's direction, which picks the voice).
    Each tower: glowing inscription rings, corner light lines, slit windows,
    storeys every eight blocks joined by a switchback, a crowned top with a
    crystal needle; the gatekeepers' chest and book on the ground floor."""
    to = gate_frame(direction)
    _voice, sign_key = GATES[direction]
    rng = piece_rng(f"aurelith/gate/{direction}")
    outward = direction
    inward = OPP[direction]
    V0, V1 = 94, 107            # the gate complex's depth (the wall band is 100..102)

    def put(u, y, v, bid, nbt=None):
        x, z = to(u, v)
        c.set(x, y, z, bid, nbt)

    def fill_uv(u0, u1, y0, y1, v0, v1, bid):
        for u in range(min(u0, u1), max(u0, u1) + 1):
            for v in range(min(v0, v1), max(v0, v1) + 1):
                x, z = to(u, v)
                c.fill(x, y0, z, x, y1, z, bid)

    along_strip = k.strip_x if direction in ("north", "south") else k.strip_z
    # The passage: the avenue runs through, paved, open under an arch.
    fill_uv(-7, 7, S - 1, S - 1, V0, V1, k.stone)
    fill_uv(-7, 7, S, S, V0, V1, k.tiles)
    fill_uv(-7, 7, S + 1, S + 18, V0, V1, AIR)
    for u in range(-7, 8):
        arch = S + 9 + int(round(4.0 * math.sqrt(max(0.0, 1.0 - (u / 7.6) ** 2))))
        for v in range(97, 106):
            x, z = to(u, v)
            c.fill(x, arch + 1, z, x, S + 18, z, k.bricks)
            c.set(x, arch + 1, z, k.polished)
            if v in (97, 105):
                c.set(x, arch + 1, z, along_strip)
        # The walk over the arch, parapets on both faces.
        for v in range(97, 106):
            x, z = to(u, v)
            c.set(x, S + 19, z, k.polished)
            c.fill(x, S + 20, z, x, S + 22, z, AIR)
        for v in (97, 105):
            x, z = to(u, v)
            c.set(x, S + 20, z, k.wall())
    # Keystones with the voice's inscription, both faces.
    for v, face in ((96, inward), (106, outward)):
        x, z = to(0, v)
        c.set(x, S + 14, z, k.stave)
        x2, z2 = to(0, v + (-1 if v == 96 else 1))
        c.set(x2, S + 14, z2, c.b("spruce_wall_sign", facing=face, waterlogged="false"),
              nbt=sign_nbt(LORE.SIGNS[sign_key]))
    # Two towers.
    for side in (-1, 1):
        ua, ub = sorted((side * 8, side * 17))
        fill_uv(ua, ub, S - 12, S - 1, V0, V1, k.stone)
        fill_uv(ua, ub, S, S + 40, V0, V1, k.bricks)
        fill_uv(ua + 1, ub - 1, S + 1, S + 39, V0 + 1, V1 - 1, AIR)
        for (uu, vv) in [(uu, vv) for uu in range(ua, ub + 1) for vv in range(V0, V1 + 1)
                         if uu in (ua, ub) or vv in (V0, V1)]:
            x, z = to(uu, vv)
            c.fill(x, S + 1, z, x, S + 2, z, k.polished)         # plinth (the outline only)
        fill_uv(ua + 1, ub - 1, S, S, V0 + 1, V1 - 1, k.polished)
        for yy in (S + 6, S + 22, S + 38):
            fill_uv(ua, ub, yy, yy, V0, V1, k.stave)             # glowing inscription rings
        for (uu, vv) in ((ua, V0), (ua, V1), (ub, V0), (ub, V1)):
            x, z = to(uu, vv)
            c.fill(x, S + 3, z, x, S + 40, z, k.strip_y)         # corner light lines
        for yy in range(S + 8, S + 40, 8):
            fill_uv(ua + 1, ub - 1, yy, yy, V0 + 1, V1 - 1, k.polished)
        # Slit windows on every face.
        for yy in range(S + 10, S + 38, 8):
            for uu in range(ua + 2, ub - 1, 3):
                for vv in (V0, V1):
                    x, z = to(uu, vv)
                    c.fill(x, yy, z, x, yy + 2, z, k.glass)
            for vv in range(V0 + 2, V1 - 1, 3):
                for uu in (ua, ub):
                    x, z = to(uu, vv)
                    c.fill(x, yy, z, x, yy + 2, z, k.glass)
        # Crown: a stepped cornice, crenels, a crystal needle, lamps.
        fill_uv(ua - 1, ub + 1, S + 41, S + 41, V0 - 1, V1 + 1, k.polished)
        fill_uv(ua, ub, S + 42, S + 44, V0, V1, AIR)
        for uu in range(ua - 1, ub + 2):
            for vv in range(V0 - 1, V1 + 2):
                if uu in (ua - 1, ub + 1) or vv in (V0 - 1, V1 + 1):
                    put(uu, S + 42, vv, k.bricks if (uu + vv) % 2 == 0 else k.slab("bricks"))
        mu = (ua + ub) // 2
        mv = (V0 + V1) // 2
        for yy in range(S + 42, S + 47):
            put(mu, yy, mv, k.pillar_y if yy < S + 46 else k.crystal)
        put(mu, S + 47, mv, k.cluster_up)
        for (du, dv) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            put(mu + du, S + 42, mv + dv, k.lamp)
        # The door into the passage (the tower face at u = +-8).
        door_u = side * 8
        for vv in (99, 100, 101):
            x, z = to(door_u, vv)
            c.fill(x, S + 1, z, x, S + 3, z, AIR)
        x, z = to(door_u, 100)
        c.set(x, S + 4, z, k.chiseled)
        # Stairs: a switchback along v, just inside the outer face.
        _tower_stairs(c, k, to, side * 15, side * 14, V0 + 2,
                      [S, S + 8, S + 16, S + 24, S + 32, S + 41])
        # The gatekeepers' room.
        free = []
        for uu in range(ua + 1, ub):
            for vv in range(V0 + 1, V1):
                x, z = to(uu, vv)
                if c.get(x, S + 1, z) == AIR and c.get(x, S + 2, z) == AIR:
                    free.append((x, z))
        furnish_room(c, k, free, S + 1, rng, "aurelith_gatehouse",
                     "legend" if side < 0 else "four_voices", dense=0.25, kind="guard")
        # A hanging lamp in every storey.
        for yy in (S + 7, S + 15, S + 23, S + 31, S + 39):
            x, z = to(mu, mv)
            if c.get(x, yy, z) == AIR:
                c.set(x, yy, z, k.lamp_hang)
    # From each tower's second storey (S + 16) a door and three steps lead
    # up onto the walk over the arch.
    for side in (-1, 1):
        for vv in (99, 100, 101):
            x, z = to(side * 8, vv)
            c.fill(x, S + 17, z, x, S + 19, z, AIR)
            for n_, u in enumerate((side * 7, side * 6, side * 5)):
                x, z = to(u, vv)
                x2, z2 = to(u - side, vv)
                facing = {(1, 0): "east", (-1, 0): "west", (0, 1): "south", (0, -1): "north"}[(x2 - x, z2 - z)]
                stair_step(c, k, x, S + 17 + n_, z, facing)
    # The voice spire over the arch, the beacon on top.
    fill_uv(-2, 2, S + 19, S + 50, 98, 102, k.bricks)
    fill_uv(-1, 1, S + 20, S + 49, 99, 101, AIR)
    for (uu, vv) in ((-2, 98), (-2, 102), (2, 98), (2, 102)):
        x, z = to(uu, vv)
        c.fill(x, S + 19, z, x, S + 50, z, k.strip_y)
    for yy in range(S + 24, S + 50, 6):
        fill_uv(-2, 2, yy, yy, 98, 102, k.stave)
        for (uu, vv) in ((0, 98), (0, 102), (-2, 100), (2, 100)):
            x, z = to(uu, vv)
            c.fill(x, yy + 2, z, x, yy + 3, z, k.glass)
    # A crystal conduit climbs the spire's hollow core.
    x, z = to(0, 100)
    c.fill(x, S + 20, z, x, S + 49, z, k.conduit_y)
    fill_uv(-3, 3, S + 51, S + 51, 97, 103, k.polished)
    fill_uv(-3, 3, S + 52, S + 55, 97, 103, AIR)
    for (uu, vv) in ((-3, 97), (-3, 103), (3, 97), (3, 103)):
        put(uu, S + 52, vv, k.cluster_up)
    x, z = to(0, 100)
    c.set(x, S + 52, z, c.b("voice_beacon", facing=outward), nbt=engine_be_nbt("voice_beacon"))
    for (du, dv) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        put(du, S + 52, 100 + dv, k.chiseled)
    # "we will come back": scratched on the Soprano Gate's inner face, and
    # the road's name beside it.
    if direction == "north":
        x, z = to(-5, 96)
        c.set(x, S + 2, z, c.b("spruce_wall_sign", facing=inward, waterlogged="false"),
              nbt=sign_nbt(LORE.SIGNS["scratched_note"], color="white", glowing=False))
        x, z = to(4, 96)
        c.set(x, S + 3, z, c.b("spruce_wall_sign", facing=inward, waterlogged="false"),
              nbt=sign_nbt(LORE.SIGNS["last_procession"]))


def _tower_stairs(c: City, k: Kit, to, u_a: int, u_b: int, v0: int, levels: list[int]) -> None:
    """A switchback in a gate tower: rows at u_a and u_b, runs along v."""
    for f in range(len(levels) - 1):
        base, nxt = levels[f], levels[f + 1]
        n = nxt - base
        even = f % 2 == 0
        u = u_a if even else u_b
        for i in range(n):
            vv = v0 + (1 + i if even else n - i)
            h = base + 1 + i
            x, z = to(u, vv)
            x2, z2 = to(u, vv + (1 if even else -1))
            dx, dz = x2 - x, z2 - z
            facing = {(1, 0): "east", (-1, 0): "west", (0, 1): "south", (0, -1): "north"}[(dx, dz)]
            stair_step(c, k, x, h, z, facing)


def build_gates(c: City, k: Kit) -> None:
    for d in GATES:
        build_gate(c, k, d)


# ── the Archive of Echoes ────────────────────────────────────────────────────
ARCHIVE = (61, 61)
ARCHIVE_R = 12.5
BRIDGE_Y = S + 44           # sky-bridge deck (walk on BRIDGE_Y + 1): the Spire's floor


def build_archive(c: City, k: Kit) -> None:
    """A ring tower 68 tall: galleries of shelves every seven blocks round an
    open atrium, a helical ramp of half-steps climbing the atrium's edge
    (one turn per storey, so every gallery is reached on foot), a crystal
    column lit from the ground to the crown, Stave inscription rings on the
    outside at every floor, tall slit windows between vertical light lines,
    a crown of pillars and violet light. The sky bridge leaves the sixth
    gallery eastward. Lecterns hold the Listeners' books; the lowest shelf
    hides a chest of sealed shards; a sealed reading cell sits in the wall."""
    ax, az = ARCHIVE
    rng = piece_rng("aurelith/archive")
    r = np.hypot(XS - ax, ZS - az)
    ang = (np.degrees(np.arctan2(ZS - az, XS - ax)) + 360.0) % 360.0
    top = S + 63
    body = r <= ARCHIVE_R
    wall = body & (r > ARCHIVE_R - 2.0)
    gallery = body & (r > 6.5) & (r <= ARCHIVE_R - 2.0)
    ramp = (r > 4.5) & (r <= 6.5)
    atrium = r <= 4.5
    c.fill_mask(body, S - 1, S, k.polished)
    c.fill_mask(body, S + 1, top, AIR)
    c.fill_mask(wall, S + 1, top, k.bricks)
    storey = 7
    floors = list(range(S, top - 6, storey))            # gallery slab levels
    for fy in floors[1:]:
        c.fill_mask(gallery, fy, fy, k.polished)
        c.fill_mask(wall, fy, fy, k.stave)              # glowing ring on the outside
    # Floor inlay: a cyan ring at the atrium's rim on the ground floor.
    c.fill_mask((r > 3.5) & (r <= 4.5), S, S, k.cyan)
    # The ramp: half-block heights rising 2 * storey per turn, clockwise from
    # angle 0 (east), starting at the ground floor.
    for (x, z) in np.argwhere(ramp & body):
        x, z = int(x), int(z)
        a = ang[x, z]
        for turn in range((top - S) // storey):
            hh = int(math.floor(a / 360.0 * 2 * storey)) + turn * 2 * storey   # half blocks above S
            y = S + hh // 2
            if y >= top - 2:
                continue
            if hh % 2:
                c.set(x, y + 1, z, k.slab("polished"))
                c.set(x, y, z, k.polished)
            else:
                c.set(x, y, z, k.polished)
            # Underside finish: a lumen strip every few cells.
            if (x + z) % 5 == 0:
                c.set(x, y - 1, z, k.strip_y if c.get(x, y - 1, z) == AIR else c.get(x, y - 1, z))
    # Railings on the gallery rims, open where the ramp meets each gallery
    # (angles near 0, where the ramp is level with the slab).
    rim = gallery & (r <= 7.5)
    for fy in floors[1:]:
        for (x, z) in np.argwhere(rim):
            x, z = int(x), int(z)
            if ang[x, z] < 40 or ang[x, z] > 340:
                continue
            c.set(x, fy + 1, z, k.fence())
    # The crystal column and hanging lamps in the atrium.
    c.fill_mask(r <= 1.0, S + 1, top - 1, k.crystal)
    c.fill_mask((r <= 1.8) & (r > 1.0), S + 1, S + 2, k.chiseled)
    for fy in floors[1:]:
        for (dx, dz) in ((3, 0), (-3, 0), (0, 3), (0, -3)):
            c.set(ax + dx, fy + 5, az + dz, k.lamp_hang)
            c.set(ax + dx, fy + 6, az + dz, k.chain)
    # Windows: tall slits in the wall between vertical light lines.
    for (x, z) in np.argwhere(wall & (r > ARCHIVE_R - 1.0)):
        x, z = int(x), int(z)
        a = ang[x, z]
        slot = int(a // 10) % 3
        for fy in floors:
            if slot == 0:
                c.fill(x, fy + 2, z, x, fy + 5, z, k.glass)
                ix, iz = int(round(ax + (x - ax) * 0.88)), int(round(az + (z - az) * 0.88))
                if c.get(ix, fy + 2, iz) == k.bricks:
                    c.fill(ix, fy + 2, iz, ix, fy + 5, iz, k.glass)
            elif slot == 1 and int(a) % 10 == 5:
                c.fill(x, fy + 1, z, x, fy + storey - 1, z, k.strip_y)
    # Galleries: shelves against the wall, lecterns, chests, lamps.
    shelf_ring = gallery & (r > ARCHIVE_R - 3.0)
    homes = list(LORE.BOOK_HOMES["archive"])
    for n, fy in enumerate(floors):
        y = fy + 1
        for (x, z) in np.argwhere(shelf_ring):
            x, z = int(x), int(z)
            # No shelves across the doors (east, south) or the east side
            # where the ramp meets every gallery and the bridge leaves.
            if abs(z - az) <= 2 and x > ax or abs(x - ax) <= 2 and z > az:
                continue
            if c.get(x, y, z) == AIR and rng.random() < 0.8:
                c.fill(x, y, z, x, y + 2, z, k.bookshelf)
        # Lecterns facing the atrium at the four quarter angles.
        for qa, book in zip((45, 135, 225, 315), homes + homes):
            th = math.radians(qa + n * 20)
            x = int(round(ax + math.cos(th) * 8.5)); z = int(round(az + math.sin(th) * 8.5))
            facing = ("west" if math.cos(th) > 0.7 else "east" if math.cos(th) < -0.7
                      else "north" if math.sin(th) > 0 else "south")
            if c.get(x, y, z) == AIR:
                c.set(x, y, z, c.b("lectern", facing=facing, has_book="true", powered="false"),
                      nbt=lectern_nbt(book))
                break
        th = math.radians(200 + n * 37)
        x = int(round(ax + math.cos(th) * 9.5)); z = int(round(az + math.sin(th) * 9.5))
        c.set(x, y, z, c.b("chest", facing="north", type="single", waterlogged="false"),
              nbt=loot_nbt("aurelith_archive"))
        if rng.random() < 0.6:
            th = math.radians(100 + n * 53)
            x = int(round(ax + math.cos(th) * 8.0)); z = int(round(az + math.sin(th) * 8.0))
            if c.get(x, y, z) == AIR:
                c.set(x, y, z, k.cobweb)
    # The sealed shards on the lowest shelf: a chest behind the ground ring.
    c.set(ax - 10, S + 1, az, c.b("chest", facing="east", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_archive"))
    # A sealed reading cell in the wall's thickness, behind a bookshelf, on
    # the third gallery: the Undersong recordings.
    fy = floors[3]
    cell = (ax, az - 11)
    c.fill(cell[0] - 1, fy + 1, cell[1], cell[0] + 1, fy + 3, cell[1], AIR)
    c.set(cell[0], fy + 1, cell[1], c.b("chest", facing="south", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_vault"))
    c.set(cell[0] - 1, fy + 1, cell[1], c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("undersong"))
    c.set(cell[0] + 1, fy + 3, cell[1], k.lamp_hang)
    c.fill(cell[0] - 1, fy + 1, cell[1] + 1, cell[0] + 1, fy + 3, cell[1] + 1, k.bookshelf)
    # Doors: east (to the street and the bridge side) and south.
    for (dx, dz, face) in ((12, 0, "east"), (0, 12, "south")):
        for w in (-1, 0, 1):
            x = ax + dx + (w if dx == 0 else 0); z = az + dz + (w if dz == 0 else 0)
            x2 = ax + dx - (1 if dx else 0) + (w if dx == 0 else 0)
            z2 = az + dz - (1 if dz else 0) + (w if dz == 0 else 0)
            c.fill(x, S + 1, z, x, S + 4, z, AIR)
            c.fill(x2, S + 1, z2, x2, S + 4, z2, AIR)
            c.set(x, S + 5, z, k.chiseled)
    c.set(ax + 13, S + 5, az, c.b("spruce_wall_sign", facing="east", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["archive_door"]))
    # The bridge door on the gallery at S + 42, two steps up onto the
    # bridge's deck (BRIDGE_Y = S + 44) just outside the wall.
    gy = S + 42
    for w in (-1, 0, 1):
        c.fill(ax + 11, gy + 1, az + w, ax + 12, gy + 4, az + w, AIR)
        c.set(ax + 13, gy, az + w, k.polished)
        stair_step(c, k, ax + 13, gy + 1, az + w, "east")
        c.set(ax + 14, gy + 1, az + w, k.polished)
        stair_step(c, k, ax + 14, gy + 2, az + w, "east")
    # The crown: a ring of pillars carrying a violet halo.
    crown = body & (r > ARCHIVE_R - 1.5)
    c.fill_mask(body, top, top, k.polished)
    c.fill_mask(crown & (((XS + ZS) % 3) == 0), top + 1, top + 4, k.pillar_y)
    c.fill_mask(crown, top + 5, top + 5, k.violet)
    c.fill_mask(body & (r > ARCHIVE_R - 3.0), top + 5, top + 5, k.polished)
    c.fill_mask(r <= 1.0, top, top + 6, k.crystal)
    c.set(ax, top + 7, az, k.cluster_up)
    c.fill_mask(atrium & (r > 1.0), top, top, k.glass)


# ── the Conductor's Spire ────────────────────────────────────────────────────
SPIRE = (131, 61)


def build_spire(c: City, k: Kit) -> None:
    """The tallest tower: three tiers stepping in (19, 15, 11 wide), every
    corner a line of light, planted terraces at each setback, then the view
    terrace at S + 70 — the Conductor's chair and a lectern with Ysolde's
    diary — and the needle with its crystal crown to S + 95. A switchback
    climbs the whole height. The sky bridge from the Archive arrives on the
    west face of the second tier."""
    sx, sz = SPIRE
    tiers = [(9, S + 1, 6), (7, S + 25, 6), (5, S + 49, 5)]   # half-width, base y, storeys
    rng = piece_rng("aurelith/spire")
    info_tops = []
    for n, (hw, yb, storeys) in enumerate(tiers):
        m = rect(sx - hw, sz - hw, sx + hw, sz + hw)
        # Chamfer the corners of each tier so the silhouette tapers.
        m &= (np.abs(XS - sx) + np.abs(ZS - sz)) <= hw * 2 - 2
        heights = [4] * storeys
        y_top = yb + sum(heights) - 1
        edge = m & ~erode(m)
        inner = m & ~edge
        c.fill_mask(inner, yb - 1, yb - 1, k.polished)
        c.fill_mask(inner, yb, y_top, AIR)
        c.fill_mask(edge, yb, y_top, k.bricks if n == 0 else k.polished)
        for f in range(1, storeys):
            fy = yb - 1 + 4 * f
            c.fill_mask(m, fy, fy, k.polished)
            c.fill_mask(edge, fy, fy, k.stave if f % 2 == 0 else k.polished)
        # Corner light lines on the chamfers, tall windows between.
        for (x, z) in np.argwhere(edge):
            x, z = int(x), int(z)
            faces = outward_faces(m, x, z)
            if len(faces) >= 2:
                c.fill(x, yb, z, x, y_top, z, k.strip_y)
            elif ((x + z) % 2) == 0:
                for f in range(storeys):
                    fy = yb + 4 * f
                    c.fill(x, fy + 1, z, x, fy + 2, z, k.glass if rng.random() > 0.06 else k.cyan)
                    for yy in (fy + 1, fy + 2):
                        note_window(x, yy, z, k.cyan)
        # Setback terrace on top of the tier.
        c.fill_mask(m, y_top + 1, y_top + 1, k.polished)
        c.fill_mask(edge, y_top + 1, y_top + 1, k.strip_x)
        c.fill_mask(m, y_top + 2, y_top + 4, AIR)
        c.fill_mask(edge, y_top + 2, y_top + 2, k.wall())
        info_tops.append(y_top + 1)
        # Hanging lamps in each storey's centre, chests and books.
        for f in range(storeys):
            c.set(sx + 2, yb + 4 * f + 3, sz + 2, k.lamp_hang)
    # Terrace gardens on the two lower setbacks.
    for n in range(2):
        hw_lo, hw_hi = tiers[n][0], tiers[n + 1][0]
        ty = info_tops[n]
        ring = rect(sx - hw_lo + 1, sz - hw_lo + 1, sx + hw_lo - 1, sz + hw_lo - 1) \
            & ~rect(sx - hw_hi - 1, sz - hw_hi - 1, sx + hw_hi + 1, sz + hw_hi + 1) \
            & ((np.abs(XS - sx) + np.abs(ZS - sz)) <= hw_lo * 2 - 3)
        for (x, z) in np.argwhere(ring):
            if rng.random() < 0.35:
                c.set(int(x), ty, int(z), k.moss)
                c.set(int(x), ty + 1, int(z), rng.choice([k.grass, k.bloom, k.leaves]))
    # The view terrace (the top of tier 3) and the needle.
    vt = info_tops[2]
    c.fill(sx - 1, vt + 1, sz - 4, sx + 1, vt + 1, sz - 4, k.polished)
    # The Conductor's chair looking south-west over the city, and the diary.
    c.set(sx - 3, vt + 1, sz + 3, k.stairs("polished", "north"))
    c.set(sx - 4, vt + 1, sz + 3, k.slab("polished"))
    c.set(sx - 2, vt + 1, sz + 3, k.slab("polished"))
    c.set(sx - 3, vt + 1, sz + 1, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("diary"))
    c.set(sx + 3, vt + 1, sz - 3, c.b("chest", facing="south", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_spire"))
    # Needle: 5x5 to S + 84, 3x3 to S + 90, crystal to S + 95.
    c.fill(sx - 2, vt + 1, sz - 2, sx + 2, vt + 14, sz - 2 + 4, k.polished)
    c.fill(sx - 1, vt + 1, sz - 1, sx + 1, vt + 13, sz + 1, AIR)
    for (dx, dz) in ((-2, -2), (2, -2), (-2, 2), (2, 2)):
        c.fill(sx + dx, vt + 1, sz + dz, sx + dx, vt + 14, sz + dz, k.strip_y)
    c.fill(sx - 1, vt + 15, sz - 1, sx + 1, vt + 20, sz + 1, k.polished)
    c.fill(sx, vt + 15, sz, sx, vt + 25, sz, k.crystal)
    for (dx, dz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(sx + dx, vt + 21, sz + dz, k.cluster_up)
        c.set(sx + dx, vt + 18, sz + dz, k.cyan)
    c.set(sx, vt + 26, sz, k.cluster_up)
    # A doorway from the needle's base onto the view terrace, and the stairs.
    c.fill(sx, vt + 1, sz + 2, sx, vt + 3, sz + 2, AIR)
    # Every storey slab of the three tiers (a tier's slabs sit at its base
    # - 1 + 4f), then the view terrace.
    levels = sorted({yb - 1 + 4 * f for (_hw, yb, storeys) in tiers for f in range(storeys)} | {vt})
    _core_stairs(c, k, sx - 1, sz - 4, "x", levels)
    # Doors at street level on all four sides, the Spire's plaque.
    for face, (dx, dz) in STEP.items():
        for w in (-1, 0, 1):
            x = sx + dx * 9 + (w if dx == 0 else 0)
            z = sz + dz * 9 + (w if dz == 0 else 0)
            c.fill(x, S + 1, z, x, S + 3, z, AIR)
            c.set(x, S + 4, z, k.chiseled)
    c.set(sx - 2, S + 4, sz + 10, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["spire_door"]))
    # The sky bridge arrives on the west face of tier 2.
    for w in (-1, 0, 1):
        c.fill(sx - 8, BRIDGE_Y + 1, sz + w, sx - 6, BRIDGE_Y + 3, sz + w, AIR)
        c.set(sx - 7, BRIDGE_Y, sz + w, k.polished)
        c.set(sx - 6, BRIDGE_Y, sz + w, k.polished)
    # Furnish the ground floor.
    free = [(int(x), int(z)) for (x, z) in np.argwhere(rect(sx - 7, sz - 7, sx + 7, sz + 7))
            if c.get(int(x), S + 1, int(z)) == AIR and c.get(int(x), S + 2, int(z)) == AIR]
    furnish_room(c, k, free, S + 1, rng, "aurelith_spire", "legend", dense=0.18, kind="study")


def _core_stairs(c: City, k: Kit, sx: int, sz: int, along: str, levels: list[int]) -> None:
    """A switchback between arbitrary slab levels: each run climbs exactly
    to the next level, the runs alternate rows and directions, and a landing
    block is laid at each end of every run where there is no floor (so a
    core may climb through a tall room with no storeys in it)."""
    def cell(u, row):
        return (sx + u, sz + row) if along == "x" else (sx + row, sz + u)

    for f in range(len(levels) - 1):
        base, nxt = levels[f], levels[f + 1]
        n = nxt - base
        even = f % 2 == 0
        row = 0 if even else 1
        for i in range(n):
            u = (1 + i) if even else (n - i)
            h = base + 1 + i
            x, z = cell(u, row)
            if along == "x":
                facing = "east" if even else "west"
            else:
                facing = "south" if even else "north"
            stair_step(c, k, x, h, z, facing)
        # Landings: where the run starts (level base) and where it ends
        # (level nxt), on both rows, so the next run can be joined.
        start_u, end_u = (0, n + 1) if even else (n + 1, 0)
        for (u, lvl) in ((start_u, base), (end_u, nxt)):
            for r in (0, 1):
                x, z = cell(u, r)
                # Never within a step's headroom (a climber's feet or head).
                under_step = any(c.name_of(c.get(x, lvl - d, z)).endswith("_stairs")
                                 for d in (1, 2, 3) if c.get(x, lvl - d, z) > AIR)
                if c.get(x, lvl, z) in (AIR, UNSET) and not under_step:
                    c.set(x, lvl, z, k.polished)
                # Headroom over the landing (a roof's dome, crown or tree
                # must not cap the way out).
                if not c.name_of(c.get(x, lvl, z)).endswith("_stairs") or lvl == base:
                    for yy in (lvl + 1, lvl + 2):
                        if c.get(x, yy, z) > AIR and not c.name_of(c.get(x, yy, z)).endswith("_stairs"):
                            c.set(x, yy, z, AIR)
                c.keep_clear.add((x, z))


# ── Starward, the observatory ────────────────────────────────────────────────
STARWARD = (163, 61)


def build_starward(c: City, k: Kit) -> None:
    """A stepped ziggurat (25, 19, 13 wide) whose terrace edges are lines of
    light, a grand exterior stair up the south face of every step, a drum
    with a ribbed nightglass dome, a star-map floor inside (Stave stone and
    cyan panels scattered like the Hush's sky) and the observers' desk."""
    ox, oz = STARWARD
    rng = piece_rng("aurelith/starward")
    steps = [(12, S + 1, S + 10), (9, S + 11, S + 20), (6, S + 21, S + 30)]
    for n, (hw, y0, y1) in enumerate(steps):
        m = rect(ox - hw, oz - hw, ox + hw, oz + hw)
        edge = m & ~erode(m)
        c.fill_mask(m, y0, y1, k.bricks)
        c.fill_mask(erode(erode(m)), y0, y1 - 1, AIR)
        c.fill_mask(m, y1, y1, k.polished)
        c.fill_mask(edge, y1, y1, k.strip_x)
        c.fill_mask(edge, y0 + 4, y0 + 4, k.stave)
        c.fill_mask(edge, y1 + 1, y1 + 1, k.wall())
        # Floors inside each step.
        c.fill_mask(erode(erode(m)), y0 - 1, y0 - 1, k.polished)
        for (x, z) in np.argwhere(edge):
            x, z = int(x), int(z)
            if len(outward_faces(m, x, z)) == 1 and (x + z) % 4 == 0:
                c.fill(x, y0 + 1, z, x, y0 + 2, z, k.glass)
        if n < 2:
            c.set(ox + hw - 3, y1 - 1, oz, k.lamp_hang)
    # The grand stair: five wide, up the south face of the first step, onto
    # its terrace; inside, a switchback climbs through the three steps to the
    # drum, with a door onto each terrace.
    for n, (hw, y0, y1) in enumerate(steps[:1]):
        rise = y1 - y0 + 1
        for i in range(rise):
            z = oz + hw + rise - i
            y = y0 + i
            for x in range(ox - 2, ox + 3):
                if z > oz + hw:
                    c.set(x, y, z, k.stairs("polished", "north"))
                    c.fill(x, y - (i + 1), z, x, y - 1, z, k.bricks) if i < 3 else None
                    c.fill(x, y + 1, z, x, y + 3, z, AIR)
        # The stair rests on the terrace below: open the terrace railing.
        for x in range(ox - 2, ox + 3):
            c.set(x, y1 + 1, oz + hw, AIR)
            for zz in range(oz + hw + 1, oz + hw + rise + 1):
                c.set(x, y0 - 1 if n else S, zz, c.get(x, y0 - 1, zz) or k.polished)
    for (hw, y0, _y1) in steps:
        c.fill(ox - 1, y0, oz + hw, ox + 1, y0 + 2, oz + hw, AIR)
        c.fill(ox - 1, y0, oz + hw - 1, ox + 1, y0 + 2, oz + hw - 1, AIR)
    # The drum and the dome.
    drum = disc(ox, oz, 5.0)
    d_edge = drum & ~erode(drum)
    c.fill_mask(drum, S + 31, S + 38, AIR)
    c.fill_mask(d_edge, S + 31, S + 38, k.polished)
    c.fill_mask(drum, S + 30, S + 30, k.polished)
    for (x, z) in np.argwhere(d_edge):
        x, z = int(x), int(z)
        if (x + z) % 3 == 0:
            c.fill(x, S + 32, z, x, S + 36, z, k.glass)
    for yy in range(S + 39, S + 45):
        rr = math.sqrt(max(0.0, 5.5 ** 2 - (yy - S - 38) ** 2))
        shell = (np.hypot(XS - ox, ZS - oz) <= rr) & (np.hypot(XS - ox, ZS - oz) > rr - 1.2)
        c.fill_mask(disc(ox, oz, rr), yy, yy, AIR)
        c.fill_mask(shell, yy, yy, k.glass)
        ribs = shell & ((np.abs(XS - ox) <= 0) | (np.abs(ZS - oz) <= 0))
        c.fill_mask(ribs, yy, yy, k.polished)
    c.set(ox, S + 45, oz, k.crystal)
    c.set(ox, S + 46, oz, k.cluster_up)
    # The star-map floor.
    for (x, z) in np.argwhere(erode(drum)):
        x, z = int(x), int(z)
        r_ = rng.random()
        c.set(x, S + 30, z, k.cyan if r_ < 0.12 else k.stave if r_ < 0.4 else k.polished)
    c.set(ox, S + 31, oz - 2, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("starward"))
    c.set(ox + 3, S + 31, oz + 2, c.b("chest", facing="west", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_observatory"))
    c.set(ox - 3, S + 31, oz + 2, c.b("cartography_table"))
    c.set(ox, S + 38, oz, k.lamp_hang)
    # The door into the drum from the top terrace, the plaque at the foot.
    c.fill(ox, S + 31, oz + 5, ox, S + 33, oz + 5, AIR)
    c.set(ox - 3, S + 3, oz + 13, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["starward_door"]))
    # Doors into the first step (east and west).
    for dx in (12, -12):
        c.fill(ox + dx, S + 1, oz - 1, ox + dx, S + 3, oz + 1, AIR)
    # Last, so nothing covers it: the switchback from the ground up through
    # the three steps into the drum.
    _core_stairs(c, k, ox - 3, oz - 1, "x", [S, S + 5, S + 10, S + 15, S + 20, S + 25, S + 30])


# ── needles and sky bridges ──────────────────────────────────────────────────
NEEDLE_A = (93, 61)
NEEDLE_B = (197, 61)


def build_needle(c: City, k: Kit, cx: int, cz: int, height: int, hw: int = 2,
                 bridge_y: int | None = None, seed: str = "") -> None:
    """A slender crystal-capped needle: light lines on the corners, a stair
    core inside, an open landing at the bridge level."""
    top = S + height
    c.fill(cx - hw, S - 1, cz - hw, cx + hw, S, cz + hw, k.polished)
    c.fill(cx - hw, S + 1, cz - hw, cx + hw, top, cz + hw, k.bricks)
    c.fill(cx - hw + 1, S + 1, cz - hw + 1, cx + hw - 1, top - 1, cz + hw - 1, AIR)
    for (dx, dz) in ((-hw, -hw), (hw, -hw), (-hw, hw), (hw, hw)):
        c.fill(cx + dx, S + 1, cz + dz, cx + dx, top, cz + dz, k.strip_y)
    for yy in range(S + 8, top, 8):
        c.fill(cx - hw, yy, cz - hw, cx + hw, yy, cz + hw, k.stave)
        c.fill(cx - hw + 1, yy, cz - hw + 1, cx + hw - 1, yy, cz + hw - 1, AIR)
    c.fill(cx - hw - 1, top, cz - hw - 1, cx + hw + 1, top, cz + hw + 1, k.polished)
    c.fill(cx, top + 1, cz, cx, top + 5, cz, k.crystal)
    for (dx, dz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(cx + dx, top + 1, cz + dz, k.cluster_up)
    c.set(cx, top + 6, cz, k.cluster_up)
    # Ladder up the inside (a needle is too slender for stairs).
    for yy in range(S + 1, top):
        c.set(cx, yy, cz + hw - 1, c.b("ladder", facing="north", waterlogged="false"))
    c.fill(cx, S + 1, cz - hw, cx, S + 3, cz - hw, AIR)
    if bridge_y is not None:
        c.fill(cx - hw, bridge_y + 1, cz - 1, cx + hw, bridge_y + 3, cz + 1, AIR)
        c.fill(cx - hw + 1, bridge_y, cz - 1, cx + hw - 1, bridge_y, cz + 1, k.polished)
        c.set(cx, bridge_y + 3, cz, k.lamp_hang)


def build_sky_bridge(c: City, k: Kit, x0: int, x1: int, z: int, y: int,
                     broken: tuple[int, int] | None = None) -> None:
    """A deck three wide at `y` from x0 to x1 along z: polished deck, a
    lumen strip under each edge, a railing, lamps on the railing every 6.
    `broken` = (xa, xb): the span that fell."""
    for x in range(min(x0, x1), max(x0, x1) + 1):
        if broken and broken[0] <= x <= broken[1]:
            continue
        for w in (-1, 0, 1):
            c.set(x, y, z + w, k.polished)
            c.fill(x, y + 1, z + w, x, y + 3, z + w, AIR)
        c.set(x, y - 1, z, k.bricks)
        c.set(x, y - 1, z - 1, k.strip_x)
        c.set(x, y - 1, z + 1, k.strip_x)
        c.set(x, y + 1, z - 2 + 0, c.get(x, y + 1, z - 2)) if False else None
        c.set(x, y, z - 2, k.polished)
        c.set(x, y, z + 2, k.polished)
        c.set(x, y + 1, z - 2, k.wall() if x % 6 else k.lamp)
        c.set(x, y + 1, z + 2, k.wall() if x % 6 else k.lamp)
    # The fallen span lies in the street below.
    if broken:
        rng = piece_rng(f"aurelith/fallen/{x0}")
        for x in range(broken[0], broken[1] + 1):
            for w in range(-3, 4):
                h = int(max(0, 3 - abs(w) - rng.random() * 1.5))
                for yy in range(S + 1, S + 1 + h):
                    c.set(x, yy, z + w, rng.choice([k.cracked, k.bricks, k.polished, k.cracked]))
                if h > 0 and rng.random() < 0.2:
                    c.set(x, S + 1 + h, z + w, k.slab("polished"))
        c.set(broken[0] + 2, S + 1 + 2, z, k.lamp)
        c.set(broken[1] - 1, S + 2, z + 2, k.sculk)
        # Broken ends: jagged stubs.
        for xe in (broken[0] - 1, broken[1] + 1):
            c.set(xe, y + 1, z, k.slab("polished"))
            c.set(xe, y, z - 1, k.cracked)


def build_towers(c: City, k: Kit) -> None:
    build_archive(c, k)
    archive_reading_rooms(c, k)
    build_spire(c, k)
    build_starward(c, k)
    build_needle(c, k, NEEDLE_A[0], NEEDLE_A[1], 58, bridge_y=BRIDGE_Y, seed="a")
    build_needle(c, k, NEEDLE_B[0], NEEDLE_B[1], 52, bridge_y=S + 38, seed="b")
    # Archive -> needle A -> Spire.
    build_sky_bridge(c, k, ARCHIVE[0] + 15, NEEDLE_A[0] - 3, ARCHIVE[1], BRIDGE_Y)
    build_sky_bridge(c, k, NEEDLE_A[0] + 3, SPIRE[0] - 8, SPIRE[1], BRIDGE_Y)
    # Starward -> needle B: the fallen bridge. Its middle lies in the street.
    build_sky_bridge(c, k, STARWARD[0] + 7, NEEDLE_B[0] - 3, STARWARD[1], S + 38,
                     broken=(178, 187))



# ── districts ────────────────────────────────────────────────────────────────
XB = [(9, 40), (48, 74), (82, 105), (119, 142), (150, 176), (184, 215)]
ZB_N = [(9, 40), (48, 74), (82, 105)]


def free_mask() -> np.ndarray:
    """Where buildings may stand: inside the walls, off the streets, the
    avenues, the plaza and its colonnade walk, the river zone, with a
    one-block sidewalk along every street."""
    m = INSIDE & ~WALL_BAND & (OCT < WALL_IN - 2)
    roads = STREET_MASK | AVE_NS | AVE_EW
    m &= ~dilate(roads)
    m &= ~(RADIUS <= COLONNADE_R1 + 2.5)
    m &= ~dilate(dilate(RIVER_ZONE))
    return m


def build_crescents(c: City, k: Kit, free: np.ndarray) -> np.ndarray:
    """The four blocks that face the plaza become crescents following its
    curve, three storeys with a colonnade on the plaza side: the Hall of
    Instruments (north-west), the Tuners' Works (north-east), and two
    houses of the conductors' choir (south)."""
    used = np.zeros((W, D), bool)
    for (qx, qz, name) in ((-1, -1, "instruments"), (1, -1, "tuners"), (-1, 1, "cres_sw"), (1, 1, "cres_se")):
        quad = ((XS - CX) * qx > AVENUE_HALF + 1) & ((ZS - CZ) * qz > AVENUE_HALF + 1)
        m = free & quad & (RADIUS >= COLONNADE_R1 + 3) & (RADIUS <= 49.5)
        if name in ("cres_sw", "cres_se"):
            m &= ~dilate(dilate(RIVER_ZONE))
        if m.sum() < 60:
            continue
        info = build_midrise(c, k, m, 3, f"aurelith/{name}", kind="classic", ground_h=6,
                             loot="aurelith_tuners" if name == "tuners" else
                             "aurelith_instruments" if name == "instruments" else "aurelith_stillhouse",
                             book="heart_log" if name == "tuners" else "four_voices" if name == "instruments"
                             else "charter", roof="garden",
                             # The Works are the Tuners' workshop; the Hall
                             # keeps its own instruments (and the quest's
                             # cabinet alcove), so no kit there.
                             interior="tuners" if name == "tuners" else "" if name == "instruments" else None)
        used |= m
        if name == "instruments":
            _instruments_interior(c, k, m)
        if name == "tuners":
            _tuners_interior(c, k, m)
    return used


def _instruments_interior(c: City, k: Kit, m: np.ndarray) -> None:
    """The Hall of Instruments: rows of note blocks, resonant chimes on
    plinths, a wall of crystal 'organ pipes' of rising heights."""
    rng = piece_rng("aurelith/instruments")
    inner = erode(erode(m))
    pts = [(int(x), int(z)) for (x, z) in np.argwhere(inner)]
    for (x, z) in pts:
        if c.get(x, S + 1, z) != AIR:
            continue
        if (x + 2 * z) % 5 == 0:
            c.set(x, S + 1, z, k.note)
        elif (x * 3 + z) % 11 == 0:
            c.set(x, S + 1, z, k.chiseled)
            c.set(x, S + 2, z, k.chime)
    # Organ pipes against the wall nearest the plaza.
    pipes = sorted(pts, key=lambda p: RADIUS[p[0], p[1]])[:18]
    for n, (x, z) in enumerate(pipes):
        h = 2 + (n * 7) % 5
        for yy in range(S + 1, S + 1 + h):
            if c.get(x, yy, z) == AIR:
                c.set(x, yy, z, k.conduit_y)
    x, z = pts[len(pts) // 3]
    c.set(x, S + 4, z, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["instruments_hall"])) if c.get(x, S + 4, z) == AIR else None


def _tuners_interior(c: City, k: Kit, m: np.ndarray) -> None:
    """The Tuners' Works: conduits along the ceiling and, under grates in the
    plaza floor, a conduit line running to the Heart's dais."""
    inner = erode(erode(m))
    for (x, z) in np.argwhere(inner):
        x, z = int(x), int(z)
        if (x + z) % 4 == 0 and c.get(x, S + 6, z) == AIR:
            c.set(x, S + 6, z, k.conduit_y)
    # The conduit line: 45 degrees from the Works to the dais, under grates.
    for i in range(10, 44):
        x = CX + int(round(i * 0.7071)); z = CZ - int(round(i * 0.7071))
        if RADIUS[x, z] > 8.5:
            c.set(x, S, z, k.grate)
            c.set(x, S - 1, z, k.conduit_x)
            c.set(x + 1, S - 1, z, k.conduit_z)
    pts = [(int(x), int(z)) for (x, z) in np.argwhere(inner)]
    x, z = pts[len(pts) // 2]
    c.set(x, S + 1, z, k.chiseled)
    c.set(x, S + 2, z, k.crystal)
    c.set(x, S + 3, z, k.chime)


def build_house(c: City, k: Kit, x0: int, z0: int, x1: int, z1: int, front: str, seed: str,
                maren: bool = False) -> None:
    """A Stillhouse: two storeys and a roof terrace, choirstone with amber
    hearth-light in some windows, a whisperwood door, a stair along one side
    wall and another to the roof, a bed upstairs, a table laid below, a
    chest, sometimes a lectern with a child's lesson book. `maren`: the
    house of the unsent letter — the table still laid, the lamp left on."""
    rng = piece_rng(seed)
    HOUSES.append((x0, z0, x1, z1))
    m = rect(x0, z0, x1, z1)
    edge = m & ~erode(m)
    inner = erode(m)
    wall = rng.choice([k.stone, k.bricks, k.bricks, k.polished])
    c.fill_mask(inner, S, S, k.planks)
    c.fill_mask(inner, S + 1, S + 8, AIR)
    c.fill_mask(edge, S + 1, S + 9, wall)
    c.fill_mask(m, S + 5, S + 5, k.polished)
    c.fill_mask(inner, S + 5, S + 5, k.planks)
    c.fill_mask(m, S + 10, S + 10, k.polished)
    c.fill_mask(inner, S + 9, S + 9, AIR)
    c.fill_mask(edge, S + 11, S + 11, k.wall())
    c.fill_mask(inner, S + 11, S + 12, AIR)
    # Corner posts and an amber band under the eaves.
    for (x, z) in np.argwhere(edge):
        x, z = int(x), int(z)
        f = outward_faces(m, x, z)
        if len(f) >= 2:
            c.fill(x, S + 1, z, x, S + 10, z, k.pillar_y)
        elif f:
            c.set(x, S + 10, z, k.strip_x if f[0] in ("north", "south") else k.strip_z)
    # Windows front and back, both storeys; some glow amber.
    lit_rate = 0.9 if maren else 0.22
    for (x, z) in np.argwhere(edge):
        x, z = int(x), int(z)
        f = outward_faces(m, x, z)
        if len(f) != 1:
            continue
        along = x if f[0] in ("north", "south") else z
        if along % 2 == 1:
            for base in (S + 2, S + 7):
                c.fill(x, base, z, x, base + 1, z, k.amber if rng.random() < lit_rate else k.glass)
                if not maren:          # Maren's lamp stays steady: she left it on
                    for yy in (base, base + 1):
                        note_window(x, yy, z, k.amber)
    # The door, mid-front.
    fx, fz = STEP[front]
    if front in ("north", "south"):
        dx_, dz_ = (x0 + x1) // 2, (z0 if front == "north" else z1)
    else:
        dx_, dz_ = (x0 if front == "west" else x1), (z0 + z1) // 2
    c.set(dx_, S + 1, dz_, c.b("whisperwood_door", facing=OPP[front], half="lower", hinge="left",
                               open="false", powered="false"))
    c.set(dx_, S + 2, dz_, c.b("whisperwood_door", facing=OPP[front], half="upper", hinge="left",
                               open="false", powered="false"))
    c.set(dx_ + fx, S, dz_ + fz, k.polished)
    c.set(dx_, S + 3, dz_, k.chiseled)
    for n_ in (0, 1, 2):
        c.keep_clear.add((dx_ - fx * n_, dz_ - fz * n_))
    c.set(dx_ + fx, S + 4, dz_ + fz, k.lamp_hang if c.get(dx_ + fx, S + 4, dz_ + fz) in (AIR, UNSET) else
          c.get(dx_ + fx, S + 4, dz_ + fz))
    # Stairs: along a side wall from the ground to the upper floor (5 steps),
    # then along the opposite side to the roof.
    back = OPP[front]
    bx, bz = STEP[back]
    side = LEFT[front]
    sx_, sz_ = STEP[side]
    # Local frame: 'depth' runs from the front wall inward.
    def at(depth, lateral):
        # lateral: 1 = just inside the left side wall, -1 = right side wall
        if front in ("north", "south"):
            cx_ = x0 + 1 if lateral == 1 and side == "west" or lateral == -1 and side == "east" else x1 - 1
            cz_ = (z0 + depth) if front == "north" else (z1 - depth)
        else:
            cz_ = z0 + 1 if lateral == 1 and side == "north" or lateral == -1 and side == "south" else z1 - 1
            cx_ = (x0 + depth) if front == "west" else (x1 - depth)
        return cx_, cz_
    depth_n = (z1 - z0) if front in ("north", "south") else (x1 - x0)
    for i in range(5):
        x, z = at(2 + i, 1)
        if 2 + i >= depth_n:
            break
        stair_step(c, k, x, S + 1 + i, z, back, fam="ww")
    for i in range(5):
        x, z = at(depth_n - 2 - i, -1)
        if depth_n - 2 - i <= 0:
            break
        stair_step(c, k, x, S + 6 + i, z, front, fam="ww")
    # Rooms.
    free0 = [(int(x), int(z)) for (x, z) in np.argwhere(erode(m))
             if c.get(int(x), S + 1, int(z)) == AIR and c.get(int(x), S + 2, int(z)) == AIR]
    furnish_room(c, k, free0, S + 1, rng, "aurelith_stillhouse",
                 ("letter" if maren else ("lesson" if rng.random() < 0.3 else None)), dense=0.2,
                 kind="stillhouse")
    free1 = [(int(x), int(z)) for (x, z) in np.argwhere(erode(m))
             if c.get(int(x), S + 6, int(z)) == AIR and c.get(int(x), S + 7, int(z)) == AIR
             and c.get(int(x), S + 5, int(z)) not in (AIR, UNSET) and (int(x), int(z)) not in c.keep_clear]
    # A bed against the back wall.
    colour = rng.choice(["cyan_bed", "purple_bed", "blue_bed", "white_bed"])
    for (x, z) in free1:
        hx, hz = x + bx, z + bz
        fx2, fz2 = x - bx, z - bz
        if (hx, hz) in free1 or c.get(hx, S + 6, hz) in (AIR,):
            continue
        if (fx2, fz2) in free1 and c.get(fx2, S + 6, fz2) == AIR:
            c.set(x, S + 6, z, c.b(colour, facing=back, part="head", occupied="false"))
            c.set(fx2, S + 6, fz2, c.b(colour, facing=back, part="foot", occupied="false"))
            break
    free1 = [p for p in free1 if c.get(p[0], S + 6, p[1]) == AIR]
    furnish_room(c, k, free1, S + 6, rng, None, None, dense=0.15, kind="bedroom")
    c.set((x0 + x1) // 2, S + 9, (z0 + z1) // 2, k.lamp_hang if not maren else k.lamp_hang)
    c.set((x0 + x1) // 2, S + 4, (z0 + z1) // 2, k.lamp_hang)
    if maren:
        # The table still laid: candles, pots, a chair pushed back.
        cxm, czm = (x0 + x1) // 2, (z0 + z1) // 2
        for (dx, dz) in ((0, 0), (1, 0)):
            if c.get(cxm + dx, S + 1, czm + dz) == AIR:
                c.set(cxm + dx, S + 1, czm + dz, k.ww_slab_top)
                c.set(cxm + dx, S + 2, czm + dz, c.b("white_candle", candles="3", lit="false",
                                                      waterlogged="false"))
    # Roof terrace: planters.
    for (x, z) in np.argwhere(erode(m)):
        if rng.random() < 0.25:
            c.set(int(x), S + 10, int(z), k.moss)
            c.set(int(x), S + 11, int(z), rng.choice([k.grass, k.bloom]))


def build_stillhouses(c: City, k: Kit, free: np.ndarray) -> np.ndarray:
    """Terraced houses along Stillhouse Row (z = 192) west of Canticle Way,
    both sides of the street, seven wide with a one-block gap between; the
    houses of the north row back onto the river gardens."""
    used = np.zeros((W, D), bool)
    rows = ((192 - STREET_HALF - 2 - 8, 192 - STREET_HALF - 2, "south"),
            (192 + STREET_HALF + 2, 192 + STREET_HALF + 2 + 8, "north"))
    n = 0
    maren_done = False
    for (z0, z1, front) in rows:
        x = 10
        while x + 6 < CX - AVENUE_HALF - 2:
            x1 = x + 6
            m = rect(x, z0, x1, z1)
            if (m & free).sum() != m.sum():
                x += 1                                  # slide to the next free start
                continue
            maren = (not maren_done) and front == "south" and x > 60
            build_house(c, k, x, z0, x1, z1, front, f"aurelith/house/{n}", maren=maren)
            if maren:
                maren_done = True
            used |= m
            n += 1
            x += 8
    # The row's name on the first house of the north row.
    north_row = [h for h in HOUSES if h[3] < 192]
    if north_row:
        x0, z0, x1, z1 = north_row[-1]
        place_plaque(c, k, x1 - 1, S + 4, z1 + 1, "south", "stillhouse_row")
    return used


def build_arcade(c: City, k: Kit, free: np.ndarray) -> np.ndarray:
    """The Arcade of Echoes: two-storey arcaded buildings along the street at
    z = 192 east of Canticle Way, their colonnades lined with stalls —
    counters, crates and the goods left on them — and violet and amber light
    strips over every stall. Stall nine is Hesper's lamp stall."""
    used = np.zeros((W, D), bool)
    stall_no = 0
    for (z0, z1, face) in ((192 - STREET_HALF - 2 - 11, 192 - STREET_HALF - 2, "south"),
                           (192 + STREET_HALF + 2, 192 + STREET_HALF + 2 + 11, "north")):
        x = CX + AVENUE_HALF + 3
        while x + 12 < 214:
            x1 = x + 13
            m = rect(x, z0, x1, z1) & free
            if m.sum() > 100 and erode(erode(m)).sum() > 40:
                build_midrise(c, k, m, 2, f"aurelith/arcade/{x}/{face}", kind="classic", ground_h=6,
                              loot="aurelith_arcade", book="ledger" if stall_no % 3 == 0 else None,
                              colonnade=(face,), roof="garden")
                used |= m
                # Stalls under the colonnade.
                zc = z1 - 1 if face == "south" else z0 + 1
                for sx in range(x + 1, x1 - 1, 4):
                    stall_no += 1
                    if not m[sx, zc]:
                        continue
                    # Never in front of a shop door: the stall (and Hesper's
                    # lamps beside hers) would close the only way in.
                    ix, iz = STEP[OPP[face]]
                    def door_behind(xx: int) -> bool:
                        return c.get(xx, S + 1, zc + iz) == AIR and c.get(xx, S + 2, zc + iz) == AIR
                    if any(door_behind(xx) for xx in (sx, sx + 1)):
                        continue
                    c.set(sx, S + 1, zc, k.ww_slab_top)
                    c.set(sx + 1, S + 1, zc, c.b("barrel", facing="up", open="false"),
                          nbt=loot_nbt("aurelith_arcade", "minecraft:barrel"))
                    stall_goods(c, k, stall_no, sx, zc)
                    c.set(sx, S + 5, zc, k.violet if stall_no % 2 else k.amber)
                    if stall_no == 9:
                        c.set(sx, S + 3, zc - STEP[face][1] * 0,
                              c.b("spruce_wall_sign", facing=face, waterlogged="false"),
                              nbt=sign_nbt(LORE.SIGNS["hesper_stall"])) if c.get(sx, S + 3, zc) == AIR else None
                        for xx in (sx + 2, sx + 3):
                            if not door_behind(xx):
                                c.set(xx, S + 1, zc, k.lamp)
            x += 15
    # The Arcade's name on its westmost building, facing Canticle Way.
    x = CX + AVENUE_HALF + 3
    for z in range(192 - STREET_HALF - 12, 192 - STREET_HALF - 1):
        if used[x, z] and place_plaque(c, k, x - 1, S + 4, z, "west", "arcade"):
            break
    return used


def rng_goods(k: Kit, c: City, seed: int) -> int:
    goods = [k.lamp, c.b("flower_pot"), k.cluster_up, c.b("decorated_pot", facing="north",
                                                           cracked="false", waterlogged="false"),
             k.echo_lantern_stand, c.b("white_candle", candles="2", lit="false", waterlogged="false")]
    return goods[seed % len(goods)]


def build_harbour(c: City, k: Kit) -> None:
    """The Vesper Quays: two whisperwood piers into the harbour basin on
    resonite-capped posts, stone mooring posts along the quay (Corvin's
    sign asks you to moor at them, not at the lamps), three moored boats,
    the harbourmaster's house on the north quay."""
    hx, hz = HARBOUR
    rng = piece_rng("aurelith/harbour")
    for px in (hx - 8, hx + 4):
        # Pier from the north quay out over the water.
        zc = hz - HARBOUR_RZ - 1
        for z in range(zc, hz + 3):
            for x in (px, px + 1, px + 2):
                if WATER_MASK[x, z] or QUAYWALL_MASK[x, z]:
                    c.set(x, S, z, k.planks)
                    c.fill(x, S + 1, z, x, S + 3, z, AIR)
            if WATER_MASK[px, z] and z % 3 == 0:
                for x in (px, px + 2):
                    c.fill(x, BED + 1, z, x, S - 1, z, k.log_y)
            if z % 4 == 0:
                c.set(px, S + 1, z, k.fence())
                c.set(px + 2, S + 1, z, k.fence())
        c.set(px + 1, S + 1, hz + 2, k.lamp)
        c.set(px, S + 1, hz - 3, c.b("barrel", facing="up", open="false"),
              nbt=loot_nbt("aurelith_quays", "minecraft:barrel"))
    # Mooring posts round the basin's rim.
    for i in range(0, 360, 24):
        th = math.radians(i)
        x = int(round(hx + math.cos(th) * (HARBOUR_RX + 1.5)))
        z = int(round(hz + math.sin(th) * (HARBOUR_RZ + 1.5)))
        if QUAYWALL_MASK[x, z] or QUAY_MASK[x, z]:
            c.set(x, S + 1, z, k.wall())
            c.set(x, S + 2, z, k.chiseled)
    # Boats: whisperwood hulls floating on the water, one per berth.
    for (bx, bz, along) in ((hx - 3, hz + 4, "x"), (hx + 9, hz - 2, "z"), (hx - 13, hz + 1, "x")):
        for i in range(-3, 4):
            x = bx + (i if along == "x" else 0); z = bz + (i if along == "z" else 0)
            if not WATER_MASK[x, z]:
                continue
            c.set(x, S - 1, z, k.ww_slab)
            for w in (-1, 1):
                xx = x + (0 if along == "x" else w); zz = z + (w if along == "x" else 0)
                if WATER_MASK[xx, zz]:
                    if abs(i) == 3:
                        continue
                    c.set(xx, S - 1, zz, k.planks)
                    c.set(xx, S, zz, k.fence() if abs(i) < 3 else AIR)
        c.set(bx, S, bz, k.lamp)
    # The harbourmaster's house on the north quay, its barrel and lectern
    # out front, Corvin's notice by the door.
    x0, z0 = hx - 3, hz - HARBOUR_RZ - 14
    build_house(c, k, x0, z0, x0 + 6, z0 + 8, "south", "aurelith/harbourmaster")
    c.set(x0 + 1, S + 1, z0 + 9, c.b("barrel", facing="up", open="false"),
          nbt=loot_nbt("aurelith_quays", "minecraft:barrel"))
    c.set(x0 + 5, S + 1, z0 + 9, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("canticle"))
    c.set(x0 + 1, S + 3, z0 + 9, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["quay_moorings"]))
    # The quay's name and the river's plaque by the grand bridge.
    zq = int(river_z(CX) - QUAY_EDGE)
    c.set(CX + AVENUE_HALF + 2, S + 1, zq, k.chiseled)
    c.set(CX + AVENUE_HALF + 2, S + 2, zq, k.chiseled)
    c.set(CX + AVENUE_HALF + 2, S + 2, zq + 1, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["vesper_plaque"]))
    c.set(CX + AVENUE_HALF + 2, S + 1, zq + 1, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["vesper_quay"]))


def plant_tree(c: City, k: Kit, x: int, z: int, y: int, rng: random.Random, size: int = 1) -> None:
    """A small whisperwood with a glowing lantern-leaf crown."""
    h = 3 + size + rng.randint(0, 1)
    c.set(x, y - 1, z, k.loam)
    c.fill(x, y, z, x, y + h - 1, z, k.log_y)
    for dy in range(h - 2, h + 2):
        rad = 2 if dy < h + 1 else 1
        for dx in range(-rad, rad + 1):
            for dz in range(-rad, rad + 1):
                if abs(dx) + abs(dz) > rad + (1 if dy == h else 0):
                    continue
                if (dx or dz or dy >= h) and c.get(x + dx, y + dy, z + dz) in (AIR, UNSET):
                    if rng.random() < 0.9:
                        c.set(x + dx, y + dy, z + dz, k.leaves)


def build_gardens(c: City, k: Kit, free: np.ndarray) -> np.ndarray:
    """The Quiet Gardens in the north-west: three terraces stepping up
    toward the wall (half-step edges and stairs between them), loam and moss
    beds, whisperwood groves with glowing crowns, a still pool of river
    water, benches, blooms, and the gardeners' rule on a plaque."""
    rng = piece_rng("aurelith/gardens")
    zone = free & rect(9, 30, 40, 105)
    used = zone.copy()
    levels = ((zone & (XS >= 28), S), (zone & (XS >= 19) & (XS < 28), S + 2), (zone & (XS < 19), S + 4))
    for (m, y) in levels:
        c.fill_mask(m, S - 1, y - 1, k.stone)
        c.fill_mask(m, y, y, k.loam)
        c.fill_mask(m, y + 1, y + 5, AIR)
        for (x, z) in np.argwhere(m):
            x, z = int(x), int(z)
            r_ = rng.random()
            if r_ < 0.35:
                c.set(x, y, z, k.moss)
            if r_ < 0.18:
                c.set(x, y + 1, z, rng.choice([k.grass, k.grass, k.bloom]))
    # Retaining walls along each terrace edge (polished, a Stave line), and
    # three-wide flights of stairs up them where the paths cross.
    for (xe, lo) in ((27, S), (18, S + 2)):
        for z in range(30, 106):
            if zone[xe, z] and zone[xe + 1, z]:
                c.set(xe, lo + 2, z, k.polished if z % 6 else k.stave)
    for (xe, lo) in ((27, S), (18, S + 2)):
        for z in range(30, 106):
            if not (zone[xe, z] and zone[xe + 1, z]) or z % 12 not in (11, 0, 1):
                continue
            stair_step(c, k, xe, lo + 1, z, "west")
            stair_step(c, k, xe - 1, lo + 2, z, "west")
    # Paths.
    for z in range(30, 106):
        for x in range(9, 41):
            if zone[x, z] and (z % 12 in (0, 1)):
                y = S if x >= 28 else S + 2 if x >= 19 else S + 4
                c.set(x, y, z, k.polished)
                c.set(x, y + 1, z, AIR)
    # Groves.
    for i in range(14):
        x = rng.randint(11, 38); z = rng.randint(32, 103)
        if zone[x, z] and z % 12 not in (0, 1, 2, 11):
            y = S + 1 if x >= 28 else S + 3 if x >= 19 else S + 5
            plant_tree(c, k, x, z, y, rng, size=rng.randint(0, 2))
    # A still pool of river water on the middle terrace.
    pool = disc(23, 66, 3.2) & zone
    c.fill_mask(pool, S - 1, S + 1, k.water)
    c.fill_mask(pool & ~erode(pool), S + 2, S + 2, k.polished)
    c.fill_mask(pool, S - 2, S - 2, k.violet)
    # Benches and the plaque.
    for (x, z) in ((30, 52), (30, 80), (21, 90)):
        if zone[x, z]:
            y = S + 1 if x >= 28 else S + 3
            c.set(x, y, z, k.stairs("polished", "west"))
            c.set(x, y, z + 1, k.stairs("polished", "west"))
    c.set(40, S + 2, 42, c.b("spruce_wall_sign", facing="east", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["gardens_plaque"])) if c.get(40, S + 2, 42) in (AIR, UNSET) else None
    c.set(24, S + 3, 60, c.b("lectern", facing="east", has_book="true", powered="false"),
          nbt=lectern_nbt("canticle"))
    c.set(12, S + 5, 70, c.b("chest", facing="east", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_gardens"))
    return used



def reserved_mask() -> np.ndarray:
    """Footprints owned by the landmarks (the fill must not build there)."""
    m = np.zeros((W, D), bool)
    m |= disc(ARCHIVE[0], ARCHIVE[1], ARCHIVE_R + 2)
    m |= rect(SPIRE[0] - 11, SPIRE[1] - 11, SPIRE[0] + 11, SPIRE[1] + 11)
    m |= rect(STARWARD[0] - 13, STARWARD[1] - 13, STARWARD[0] + 13, STARWARD[1] + 22)
    m |= rect(NEEDLE_A[0] - 4, NEEDLE_A[1] - 4, NEEDLE_A[0] + 4, NEEDLE_A[1] + 4)
    m |= rect(NEEDLE_B[0] - 4, NEEDLE_B[1] - 12, NEEDLE_B[0] + 4, NEEDLE_B[1] + 12)
    m |= rect(176, 55, 196, 67)                      # the fallen bridge's line
    for d in GATES:                                  # the gate complexes
        to = gate_frame(d)
        pts = [to(u, v) for u in (-19, 19) for v in (90, 108)]
        xs = [p_[0] for p_ in pts]; zs = [p_[1] for p_ in pts]
        m |= rect(min(xs), min(zs), max(xs), max(zs))
    m |= rect(HARBOUR[0] - 6, HARBOUR[1] - HARBOUR_RZ - 14, HARBOUR[0] + 6, HARBOUR[1] - HARBOUR_RZ - 4)
    return m


def build_fill(c: City, k: Kit, free: np.ndarray) -> None:
    """Every block no landmark owns is cut into lots (11-18 wide, three-block
    alleys between them) and each gets a mid-rise: taller toward the plaza
    and the avenues, lower toward the walls, one in fifteen a crystal needle
    in its own little square."""
    rng = piece_rng("aurelith/fill")
    zbands = ZB_N + [(119, 150), (170, 187), (197, 215)]
    n = 0
    for (xa, xb) in XB:
        for (za, zb) in zbands:
            block = rect(xa, za, xb, zb) & free
            if block.sum() < 60:
                continue
            pts = np.argwhere(block)
            bx0, bz0 = pts.min(axis=0); bx1, bz1 = pts.max(axis=0)
            along_x = (bx1 - bx0) >= (bz1 - bz0)
            lo, hi = (bx0, bx1) if along_x else (bz0, bz1)
            olo, ohi = (bz0, bz1) if along_x else (bx0, bx1)
            # Split the other axis in two when the block is deep.
            cuts = [(olo, ohi)]
            if ohi - olo > 28:
                mid = (olo + ohi) // 2
                cuts = [(olo, mid - 2), (mid + 2, ohi)]
            p = lo
            while p < hi - 6:
                wlot = rng.randint(11, 18)
                q = min(hi, p + wlot - 1)
                if hi - q < 9:
                    q = hi
                for (oa, ob) in cuts:
                    if along_x:
                        m = rect(int(p), int(oa), int(q), int(ob)) & block
                    else:
                        m = rect(int(oa), int(p), int(ob), int(q)) & block
                    core = erode(erode(m))
                    if m.sum() < 70 or core.sum() < 20:
                        continue
                    ptsm = np.argwhere(m)
                    mx, mz = ptsm.mean(axis=0)
                    dist = RADIUS[int(mx), int(mz)]
                    near_avenue = min(abs(mx - CX), abs(mz - CZ)) < 24
                    floors = int(round(8.5 - dist / 16.0 + rng.uniform(-1.4, 1.4) + (1.5 if near_avenue else 0)))
                    floors = max(2, min(9, floors))
                    n += 1
                    if rng.random() < 1 / 15 and core.sum() > 40:
                        # A crystal needle in a small square.
                        c.fill_mask(m, S, S, k.tiles)
                        c.fill_mask(m & ~erode(m), S, S, k.polished)
                        nx, nz = int(round(mx)), int(round(mz))
                        build_needle(c, k, nx, nz, rng.randint(34, 48), hw=2, seed=f"fill{n}")
                        for (dx, dz) in ((3, 3), (-3, 3), (3, -3), (-3, -3)):
                            if m[nx + dx, nz + dz]:
                                lamp_post(c, k, nx + dx, nz + dz)
                        continue
                    shape = m
                    roof = "auto"
                    if dist < 85 and core.sum() > 90 and rng.random() < 0.3:
                        # A tall one: 10-14 storeys stepping in twice.
                        floors = rng.randint(10, 14)
                    elif core.sum() > 70 and rng.random() < 0.12:
                        # A rotunda: round, domed.
                        ext = min(np.ptp(ptsm[:, 0]), np.ptp(ptsm[:, 1]))
                        shape = m & disc(mx, mz, ext / 2.0 - 0.5)
                        roof = "dome"
                        floors = max(floors, 5)
                    if floors >= 10:
                        setback = (floors // 3 + 1, 2 * floors // 3 + 1)
                    else:
                        setback = (floors // 2 + 1) if floors >= 5 and rng.random() < 0.6 else None
                    faces = []
                    if near_avenue and floors >= 3:
                        for f, (dx, dz) in STEP.items():
                            ex = int(mx) + dx * 12; ez = int(mz) + dz * 12
                            if 0 <= ex < W and 0 <= ez < D and (AVE_NS[ex, ez] or AVE_EW[ex, ez]):
                                faces.append(f)
                    loot = rng.choice(["aurelith_stillhouse", "aurelith_arcade", "aurelith_archive",
                                       "aurelith_stillhouse", None])
                    book = rng.choice(["lesson", "letter", "primer", None, None, None])
                    build_midrise(c, k, shape, floors, f"aurelith/lot/{n}", loot=loot, book=book,
                                  setback_at=setback, colonnade=tuple(faces), roof=roof)
                p = q + 4


# ── the Vault of the Chord ───────────────────────────────────────────────────
VAULT_HATCH = (CX - 23, CZ)


def build_statues(c: City, k: Kit) -> None:
    """The Four Voices on the plaza's axes, facing the Heart: robed figures
    of pale choirstone on Stave-banded plinths, a crystal voice at the
    throat, blank faces "so that anyone may sing in it". The Conductor's
    Podium stands north of the dais, behind the Soprano. Behind the Bass
    (west), a hatch in the paving leads down to the Vault of the Chord."""
    for (dx, dz, voice) in ((0, -19, "soprano"), (19, 0, "alto"), (0, 19, "tenor"), (-19, 0, "bass")):
        x, z = CX + dx, CZ + dz
        c.fill(x - 1, S + 1, z - 1, x + 1, S + 1, z + 1, k.chiseled)
        c.fill(x - 1, S + 2, z - 1, x + 1, S + 2, z + 1, k.stave)
        c.fill(x - 1, S + 3, z - 1, x + 1, S + 3, z + 1, k.polished)
        # Robe: 3 wide at the hem narrowing to the shoulders.
        c.fill(x - 1, S + 4, z - 1, x + 1, S + 6, z + 1, k.stone)
        c.fill(x - 1, S + 7, z, x + 1, S + 8, z, k.stone) if dx == 0 else c.fill(x, S + 7, z - 1, x, S + 8, z + 1, k.stone)
        c.fill(x, S + 7, z, x, S + 9, z, k.polished)
        c.set(x, S + 9, z, k.crystal)                       # the voice at the throat
        c.set(x, S + 10, z, k.polished)                     # the blank face
        c.set(x, S + 11, z, k.chiseled)
        # Arms raised toward the Heart.
        ix, iz = (0 if dx == 0 else -int(math.copysign(1, dx))), (0 if dz == 0 else -int(math.copysign(1, dz)))
        c.set(x + ix, S + 9, z + iz, k.stone)
        c.set(x + 2 * ix, S + 10, z + 2 * iz, k.stone)
        c.set(x + 2 * ix, S + 11, z + 2 * iz, k.cluster_up)
        # The voice's name, on the plinth's outer face.
        ox, oz = -ix, -iz
        facing = {(1, 0): "east", (-1, 0): "west", (0, 1): "south", (0, -1): "north"}[(ox, oz)]
        c.set(x + 2 * ox, S + 2, z + 2 * oz, c.b("spruce_wall_sign", facing=facing, waterlogged="false"),
              nbt=sign_nbt([voice.upper(), "", "", ""]))
    # The Conductor's Podium: a half-step apron, the base, a raised half
    # tier with the Conductor's chair facing the Heart, a Stave backdrop
    # crowned with crystal, the Charter on a lectern, Ysolde's plaque.
    px, pz = CX, CZ - 12
    c.fill(px - 2, S + 1, pz - 2, px + 2, S + 1, pz + 1, k.polished)
    for dx in (-2, -1, 1, 2):
        c.set(px + dx, S + 1, pz + 2, k.slab("polished"))
    c.fill(px - 2, S + 2, pz - 2, px + 2, S + 2, pz - 1, k.slab("polished"))
    c.set(px, S + 2, pz - 2, k.stairs("polished", "north"))           # the chair
    c.fill(px - 2, S + 1, pz - 3, px + 2, S + 5, pz - 3, k.polished)   # backdrop
    c.fill(px - 1, S + 3, pz - 3, px + 1, S + 5, pz - 3, k.stave)
    c.set(px, S + 6, pz - 3, k.crystal)
    c.set(px, S + 7, pz - 3, k.cluster_up)
    c.set(px + 2, S + 2, pz, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("charter"))
    c.set(px, S + 1, pz + 2, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["podium_plaque"]))
    # The dais plaques, facing each avenue.
    for (dx, dz, facing, key) in ((0, 9, "south", "held_note_plaque"), (0, -9, "north", "rest_plaque"),
                                  (9, 0, "east", "hold_the_note"), (-9, 0, "west", "light_is_song")):
        c.set(CX + dx, S + 1, CZ + dz, c.b("spruce_wall_sign", facing=facing, waterlogged="false"),
              nbt=sign_nbt(LORE.SIGNS[key]))


def build_vault(c: City, k: Kit) -> None:
    """Under the Heart: a hatch behind the Bass statue opens on a ladder down
    to a passage, which runs east under the plaza into the Vault of the
    Chord — a vaulted chamber round the spring the Heart was raised over,
    crystal pillars, the Choir's last and best things in three chests, and
    Ysolde's last words on a lectern."""
    hx, hz = VAULT_HATCH
    # The hatch: a whisperwood trapdoor flush in the paving, a ring of Stave
    # stone round it (the only hint), the ladder down.
    c.set(hx, S, hz, c.b("whisperwood_trapdoor", facing="east", half="top", open="false",
                         powered="false", waterlogged="false"))
    for (dx, dz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(hx + dx, S, hz + dz, k.stave)
    c.fill(hx - 1, S - 9, hz - 1, hx + 1, S - 1, hz + 1, k.bricks)
    c.fill(hx, S - 8, hz, hx, S - 1, hz, AIR)
    for yy in range(S - 8, S):
        c.set(hx, yy, hz, c.b("ladder", facing="east", waterlogged="false"))
    c.set(hx + 1, S - 8, hz, AIR)
    # The passage east, 3 wide, 3 tall, lamps in the ceiling.
    c.fill(hx + 1, S - 10, hz - 2, CX - 8, S - 4, hz + 2, k.bricks)
    c.fill(hx + 1, S - 8, hz - 1, CX - 8, S - 6, hz + 1, AIR)
    c.fill(hx + 1, S - 9, hz - 1, CX - 8, S - 9, hz + 1, k.tiles)
    for x in range(hx + 3, CX - 8, 4):
        c.set(x, S - 5, hz, k.lamp_hang)
        c.set(x, S - 9, hz, k.strip_x)
    # The chamber: 17 wide round the spring.
    r = RADIUS
    chamber = r <= 8.5
    c.fill_mask(r <= 9.5, S - 12, S - 2, k.bricks)
    c.fill_mask(chamber, S - 9, S - 3, AIR)
    c.fill_mask(chamber, S - 10, S - 10, k.tiles)
    c.fill_mask((r > 7.5) & (r <= 8.5), S - 10, S - 10, k.stave)
    c.fill_mask(chamber, S - 2, S - 2, k.polished)
    # The spring: a pool of resonant water in the floor.
    c.fill_mask(r <= 2.5, S - 12, S - 10, k.water)
    c.fill_mask((r > 2.5) & (r <= 3.5), S - 10, S - 10, k.cyan)
    # Crystal pillars round it.
    for i in range(8):
        th = 2 * math.pi * i / 8
        x = int(round(CX + math.cos(th) * 6)); z = int(round(CZ + math.sin(th) * 6))
        c.fill(x, S - 9, z, x, S - 3, z, k.crystal if i % 2 == 0 else k.pillar_y)
    c.fill(CX - 8, S - 8, CZ - 1, CX - 8, S - 6, CZ + 1, AIR)
    # The treasury.
    for (dx, dz, facing) in ((0, -7, "south"), (0, 7, "north"), (7, 0, "west")):
        c.set(CX + dx, S - 9, CZ + dz, c.b("chest", facing=facing, type="single", waterlogged="false"),
              nbt=loot_nbt("aurelith_vault"))
    c.set(CX - 4, S - 9, CZ - 4, c.b("lectern", facing="south", has_book="true", powered="false"),
          nbt=lectern_nbt("broadcast"))
    c.set(CX + 4, S - 9, CZ + 4, c.b("lectern", facing="north", has_book="true", powered="false"),
          nbt=lectern_nbt("diary"))
    c.set(CX, S - 4, CZ, k.lamp_hang)
    c.set(CX, S - 3, CZ, k.chain)


# ── reawakening the Heart ────────────────────────────────────────────────────
# docs/the-hush.md "Reawakening the Heart". The quest's pieces, placed LAST
# (after the weathering, so no ruin or dead lamp can break them):
#
#   * the Podium's four chord sockets (design x -2, -1, +1, +2 at z -13, on
#     the raised half-tier before the Conductor's chair, facing the Heart —
#     a player on the apron reaches them at waist height) and the order on
#     its backdrop;
#   * the four voice keys, one per district —
#       Soprano  the Archive of Echoes' SEALED reading cell (3rd gallery,
#                north wall, behind bookshelves under a glowing glyph);
#       Alto     the Hall of Instruments' tuned cabinet: struck in the Alto's
#                song — hearth, river, Chord, hearth (amber, violet, cyan,
#                amber: the Lampwright's Primer's colours) — on the four
#                chimes before it;
#       Tenor    the Tuners' Works' HIGH PERCH: the tuning mast on its roof,
#                a ladder to a railed platform (Orrin's log: "the Tenor's key
#                up there again");
#       Bass     the Vault of the Chord under the dais, before the spring.
# The positions are design coordinates; the engine finds the sockets the
# same way (common/world/level/AurelithQuest.hpp kSocketXs / kSocketRow) and
# /aurelith tp mirrors the rest (server/commands/AurelithCommand.cpp — keep
# QUEST_SPOTS and that table in step).
ALTO_CABINET = (97, 72)          # against the Hall of Instruments' street wall, facing in
ALTO_CHIMES_Z = 70
ALTO_CHIMES = ((93, "violet"), (95, "amber"), (99, "stave"), (101, "cyan"))
ALTO_MELODY = ("amber", "violet", "cyan", "amber")
TENOR_MAST = (155, 94)           # the Tuners' Works' roof (the crescent's east block)
TENOR_MAST_HEIGHT = 14           # platform this far over the roof
SOPRANO_CELL = (62, 50)          # the Archive's sealed cell (floors[3] + 1)
BASS_PEDESTAL = (CX - 4, CZ)     # the Vault, before the spring, facing the way in
QUEST_SPOTS = {
    # name: (x, y, z) where a player stands — /aurelith tp (design frame).
    "podium": (CX, S + 2, CZ - 11),
    "heart": (CX + 5, S + 4, CZ),
    "soprano": (SOPRANO_CELL[0] - 1, S + 22, SOPRANO_CELL[1] + 2),
    "alto": (ALTO_CABINET[0] - 1, S + 1, ALTO_CABINET[1] - 1),
    "tenor": (TENOR_MAST[0], None, TENOR_MAST[1] - 2),     # y: the perch, found at build
    "bass": (CX - 6, S - 9, CZ + 2),
}


def item_nbt(item: str) -> HS.Tag:
    return T_comp({"id": T_str(f"minecraft:{item}"), "count": T_int(1)})


def pedestal_nbt(item: str | None) -> HS.Tag:
    d = {"id": T_str("minecraft:voice_pedestal")}
    if item:
        d["Item"] = item_nbt(item)
    return T_comp(d)


def cabinet_nbt(items: list[str], melody: tuple[str, ...]) -> HS.Tag:
    entries = []
    for slot, item in enumerate(items):
        entries.append({"Slot": T_byte(slot), "id": T_str(f"minecraft:{item}"), "count": T_int(1)})
    return T_comp({"Items": T_list(10, entries), "Melody": T_list(8, list(melody)),
                   "id": T_str("minecraft:choir_cabinet")})


def _clear_room(c: City, x0: int, z0: int, x1: int, z1: int, y0: int, y1: int) -> None:
    """Empty a box of whatever the furnishing put there (never a wall: the
    caller picks a box inside the building)."""
    for x in range(min(x0, x1), max(x0, x1) + 1):
        for z in range(min(z0, z1), max(z0, z1) + 1):
            for y in range(y0, y1 + 1):
                c.set(x, y, z, AIR)


def build_quest(c: City, k: Kit) -> None:
    rng = piece_rng("aurelith/quest")
    # 1. The Podium's sockets and the order carved over them.
    for sx in (-2, -1, 1, 2):
        c.set(CX + sx, S + 2, CZ - 13, c.b("chord_socket", facing="south"),
              nbt=T_comp({"id": T_str("minecraft:chord_socket")}))
    c.set(CX - 2, S + 3, CZ - 14, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["podium_order"]))

    # 2. Soprano: the sealed reading cell in the Archive's wall (build_archive
    #    left it: a chest, Liesl's lectern with the Undersong, a lamp, and a
    #    wall of bookshelves toward the gallery). The key on a pedestal; the
    #    shelf wall carries one glowing Stave glyph and Liesl's seal.
    px, pz = SOPRANO_CELL
    fy = S + 21
    c.set(px, fy + 1, pz, c.b("voice_pedestal"), nbt=pedestal_nbt("soprano_voice_key"))
    c.set(px - 1, fy + 3, pz + 1, k.stave)
    c.set(px - 1, fy + 3, pz + 2, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["archive_seal"], color="white", glowing=False))

    # 3. Alto: the tuned cabinet against the Hall of Instruments' street wall
    #    (the crescent's widest bay, clear of its stair core), four chimes on
    #    coloured plinths before it, the Lampwright's Primer on a lectern
    #    beside it (the colours' meanings), the Four Voices on the other side
    #    (the Alto's song), the plaque above.
    ax, az = ALTO_CABINET
    _clear_room(c, ax - 5, ALTO_CHIMES_Z, ax + 5, az, S + 1, S + 4)
    for x in range(ax - 5, ax + 6):
        for z in range(ALTO_CHIMES_Z, az + 1):
            c.set(x, S, z, k.polished)
    c.set(ax, S + 1, az, c.b("choir_cabinet", facing="north", open="false"),
          nbt=cabinet_nbt(["alto_voice_key"], ALTO_MELODY))
    c.set(ax, S + 2, az, k.chiseled)
    c.set(ax, S + 3, az, k.lamp)
    for dx in (-1, 1):
        c.set(ax + dx, S + 1, az, k.bookshelf)
        c.set(ax + dx, S + 2, az, k.bookshelf)
    c.set(ax, S + 2, az - 1, c.b("spruce_wall_sign", facing="north", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["cabinet_plaque"]))
    plinth = {"violet": k.violet, "amber": k.amber, "cyan": k.cyan, "stave": k.stave}
    for (x, colour) in ALTO_CHIMES:
        c.set(x, S + 1, ALTO_CHIMES_Z, plinth[colour])
        c.set(x, S + 2, ALTO_CHIMES_Z, k.chime)
    c.set(ax + 4, S + 1, az, c.b("lectern", facing="north", has_book="true", powered="false"),
          nbt=lectern_nbt("primer"))
    c.set(ax - 4, S + 1, az, c.b("lectern", facing="north", has_book="true", powered="false"),
          nbt=lectern_nbt("four_voices"))
    c.set(ax, S + 4, az - 1, k.lamp_hang)

    # 4. Tenor: the tuning mast on the Tuners' Works' roof. A choirstone
    #    pillar core with a crystal conduit riding its east side, a ladder up
    #    its south face, a railed platform with the pedestal, and the tuning
    #    spire over it (the Tenor's pitch was set here for the lighthouses).
    mx, mz = TENOR_MAST
    roof = S + 1
    for y in range(HY - 2, S, -1):
        i = c.get(mx, y, mz)
        if i > AIR and c.name_of(i) in SOLID_CUBES:
            roof = y
            break
    top = roof + TENOR_MAST_HEIGHT
    _clear_room(c, mx - 2, mz - 2, mx + 2, mz + 2, roof + 1, top + 8)
    for y in range(roof + 1, top):
        c.set(mx, y, mz, k.pillar_y)
        c.set(mx + 1, y, mz, k.conduit_y)
        c.set(mx, y, mz + 1, c.b("ladder", facing="south", waterlogged="false"))
        if (y - roof) % 5 == 0:
            c.set(mx - 1, y, mz, k.stave)
    c.set(mx, top, mz + 1, c.b("ladder", facing="south", waterlogged="false"))
    for x in range(mx - 2, mx + 3):
        for z in range(mz - 2, mz + 3):
            if (x, z) != (mx, mz + 1):
                c.set(x, top, z, k.polished)
            edge = abs(x - mx) == 2 or abs(z - mz) == 2
            if edge and (x, z) != (mx, mz + 2):
                c.set(x, top + 1, z, k.wall())
    for (dx, dz) in ((-2, -2), (2, -2), (-2, 2), (2, 2)):
        c.set(mx + dx, top + 2, mz + dz, k.lamp)
    c.set(mx, top + 1, mz - 1, c.b("voice_pedestal"), nbt=pedestal_nbt("tenor_voice_key"))
    for y in range(top + 1, top + 5):
        c.set(mx, y, mz, k.pillar_y)
    c.set(mx, top + 5, mz, k.crystal)
    c.set(mx, top + 6, mz, k.cluster_up)
    c.set(mx, top + 3, mz + 1, c.b("spruce_wall_sign", facing="south", waterlogged="false"),
          nbt=sign_nbt(LORE.SIGNS["tuners_mast"]))
    QUEST_SPOTS["tenor"] = (mx + 1, top + 1, mz)

    # 5. Bass: the Vault of the Chord, before the spring, facing the way in.
    bx, bz = BASS_PEDESTAL
    c.set(bx, S - 9, bz, c.b("voice_pedestal"), nbt=pedestal_nbt("bass_voice_key"))
    c.set(bx, S - 8, bz, AIR)
    del rng


# ── the city's life and age ──────────────────────────────────────────────────
def plant_quays(c: City, k: Kit) -> None:
    """Glowing whisperwoods along the river promenade and the avenues."""
    rng = piece_rng("aurelith/trees")
    # Boulevard trees in the avenues' outer lanes, the middle kept open so
    # the view down each avenue ends on the Heart.
    for t in range(22, 206, 16):
        for off in (-3, 3):
            for (x, z) in ((CX + off, t), (t, CZ + off)):
                if not INSIDE[x, z] or RADIUS[x, z] < COLONNADE_R1 + 6 or RIVER_ZONE[x, z] \
                        or OCT[x, z] > WALL_IN - 10 or c.get(x, S + 1, z) != AIR:
                    continue
                if any(abs(z - sz) <= STREET_HALF + 2 for sz in STREETS_Z) and abs(x - CX) <= AVENUE_HALF:
                    continue
                if any(abs(x - sx) <= STREET_HALF + 2 for sx in STREETS_X) and abs(z - CZ) <= AVENUE_HALF:
                    continue
                c.set(x, S, z, k.loam)
                for (dx, dz) in N4:
                    if c.get(x + dx, S, z + dz) in (k.tiles, k.polished):
                        c.set(x + dx, S, z + dz, k.moss)
                plant_tree(c, k, x, z, S + 1, rng, size=1)
    for x in range(RIVER_X0 + 6, RIVER_X1 - 4, 9):
        for sign in (-1, 1):
            zc = float(river_z(x))
            z = int(round(zc + sign * (QUAY_EDGE - 1)))
            if QUAY_MASK[x, z] and c.get(x, S + 1, z) == AIR and HARBOUR_E[x, z] > 1.6 \
                    and abs(x - CX) > AVENUE_HALF + 2 and all(abs(x - b) > 5 for b in BRIDGES_X):
                c.set(x, S, z, k.loam)
                plant_tree(c, k, x, z, S + 1, rng, size=rng.randint(0, 1))


def weather(c: City, k: Kit) -> None:
    """Age: cracked bricks, sculk grown in from the river and from a few
    seeds deep in the city (sculk on the ground, veins up the walls, a
    catalyst or a sensor where it is thickest), moss and grass in the paving
    cracks, cobwebs indoors, the odd dead lamp, one collapsed corner."""
    rng = piece_rng("aurelith/weather")
    v = c.v
    # 1. Cracks.
    bricks = v == k.bricks
    cracks = bricks & (np.random.default_rng(11).random(v.shape) < 0.09)
    v[cracks] = k.cracked
    # 2. Sculk blooms on the paving: seeds along the river and in the city.
    seeds = [(int(x), int(float(river_z(x)) + rng.choice((-9, 9)))) for x in range(40, 200, 23)]
    seeds += [(rng.randint(20, 204), rng.randint(20, 204)) for _ in range(10)]
    vein = {}
    for (sx, sz) in seeds:
        rad = rng.uniform(3.0, 7.5)
        for x in range(int(sx - rad - 1), int(sx + rad + 2)):
            for z in range(int(sz - rad - 1), int(sz + rad + 2)):
                if not (0 <= x < W and 0 <= z < D) or not INSIDE[x, z]:
                    continue
                d = math.hypot(x - sx, z - sz) / rad
                if d > 1 or rng.random() > (1.05 - d) * 1.1:
                    continue
                g = int(v[x, S, z]); above = int(v[x, S + 1, z])
                if g in (k.bricks, k.cracked, k.tiles, k.polished, k.stone) and above == AIR:
                    v[x, S, z] = k.sculk
                    if rng.random() < 0.015:
                        c.set(x, S + 1, z, c.b("sculk_sensor", power="0", sculk_sensor_phase="inactive",
                                               waterlogged="false"))
                    elif rng.random() < 0.01:
                        c.set(x, S, z, k.catalyst)
                    # Veins up any wall beside it.
                    for f, (dx, dz) in STEP.items():
                        n_ = int(v[x + dx, S + 1, z + dz]) if 0 <= x + dx < W and 0 <= z + dz < D else 0
                        if n_ > AIR and c.name_of(n_) in SOLID_CUBES:
                            for h in range(1, rng.randint(2, 5)):
                                if v[x, S + h, z] == AIR:
                                    vein.setdefault((x, S + h, z), set()).add(f)
                elif g == k.sculk:
                    pass
    for (x, y, z), faces in vein.items():
        props = {d: ("true" if d in faces else "false") for d in ("down", "east", "north", "south", "up", "west")}
        c.set(x, y, z, c.b("sculk_vein", waterlogged="false", **props))
    # 2b. Round the Heart the sculk is thickest ("where the sculk is
    #     thickest, the Chord was loudest"): a noisy bloom over the plaza's
    #     inner rings, veins up the dais steps and the statues, sensors
    #     listening at the dais, one shrieker that cannot summon.
    plaza_rng = np.random.default_rng(7)
    noise = plaza_rng.random((W, D))
    ang = np.arctan2(ZS - CZ, XS - CX)
    fringe = 11.0 + 5.0 * (0.5 + 0.5 * np.sin(ang * 5.0 + 1.3)) + 3.0 * np.sin(ang * 11.0)
    bloom = (RADIUS > 8.5) & (RADIUS < fringe) & (noise < 0.78)
    for (x, z) in np.argwhere(bloom):
        x, z = int(x), int(z)
        if v[x, S, z] in (k.polished, k.tiles) and v[x, S + 1, z] == AIR:
            v[x, S, z] = k.sculk
    for (x, z) in np.argwhere((RADIUS <= 8.5) & (RADIUS > 3.5) & (noise < 0.25)):
        x, z = int(x), int(z)
        for y in range(S + 3, S, -1):
            if v[x, y, z] in (k.polished,) and v[x, y + 1, z] == AIR:
                v[x, y, z] = k.sculk
                break
    for (dx, dz) in ((5, 3), (-4, 5), (-5, -3), (3, -5)):
        x, z = CX + dx, CZ + dz
        for y in range(S + 4, S, -1):
            if v[x, y, z] == AIR and v[x, y - 1, z] not in (AIR, UNSET):
                c.set(x, y, z, c.b("sculk_sensor", power="0", sculk_sensor_phase="inactive", waterlogged="false"))
                break
    c.set(CX + 8, S + 1, CZ - 9, c.b("sculk_shrieker", can_summon="false", shrieking="false", waterlogged="false"))
    c.set(CX + 8, S, CZ - 9, k.sculk)
    c.set(CX - 10, S, CZ + 7, k.catalyst)
    # 2c. Ruins: a few buildings have lost an upper corner; the rubble lies
    #     at their feet.
    ruin_rng = piece_rng("aurelith/ruins")
    tall = [b for b in BUILT if b[1]["roof_y"] > S + 14]
    ruin_rng.shuffle(tall)
    for (fm, info) in tall[:5]:
        pts = np.argwhere(fm & ~erode(fm))
        cx_, cz_ = pts[ruin_rng.randrange(len(pts))]
        cy = info["roof_y"] - ruin_rng.randint(1, 4)
        rad = ruin_rng.uniform(3.5, 5.5)
        for x in range(int(cx_ - rad), int(cx_ + rad) + 1):
            for y in range(int(cy - rad), int(cy + rad) + 3):
                for z in range(int(cz_ - rad), int(cz_ + rad) + 1):
                    if math.dist((x, y, z), (cx_, cy, cz_)) <= rad and 0 <= x < W and 0 <= z < D \
                            and y > S + 6 and v[x, y, z] > AIR and (x, z) not in c.keep_clear:
                        v[x, y, z] = AIR
                        c.nbt.pop((x, y, z), None)
        # Rubble outside the broken corner.
        for _ in range(int(rad * 9)):
            x = int(cx_ + ruin_rng.uniform(-rad - 3, rad + 3))
            z = int(cz_ + ruin_rng.uniform(-rad - 3, rad + 3))
            if 0 <= x < W and 0 <= z < D and INSIDE[x, z] and v[x, S + 1, z] == AIR \
                    and v[x, S, z] not in (AIR, UNSET):
                v[x, S + 1, z] = ruin_rng.choice([k.cracked, k.bricks, k.cracked])
                if ruin_rng.random() < 0.4 and v[x, S + 2, z] == AIR:
                    v[x, S + 2, z] = k.slab("bricks")
    # 3. Moss and grass in the cracks of the outer streets.
    outer = INSIDE & (OCT > 70)
    for (x, z) in np.argwhere(outer):
        x, z = int(x), int(z)
        if v[x, S, z] in (k.bricks, k.cracked) and v[x, S + 1, z] == AIR and rng.random() < 0.035:
            v[x, S, z] = k.moss
            if rng.random() < 0.6:
                v[x, S + 1, z] = k.grass
    # 4. Dead lamps: one street lamp in twelve has gone out (its crystal
    #    dropped); a few lumen strips in the paving are dark now.
    lamps = np.argwhere(v == k.lamp)
    for (x, y, z) in lamps:
        if rng.random() < 1 / 12 and y == S + 4:
            c.set(int(x), int(y), int(z), AIR)
    # 5. Cobwebs in upper corners indoors.
    for _ in range(900):
        x, y, z = rng.randint(1, W - 2), rng.randint(S + 2, S + 40), rng.randint(1, D - 2)
        if v[x, y, z] == AIR and v[x, y + 1, z] not in (AIR, UNSET) and \
                sum(1 for dx, dz in N4 if v[x + dx, y, z + dz] not in (AIR, UNSET)) >= 2:
            v[x, y, z] = k.cobweb


def corner_signs(c: City, k: Kit) -> None:
    """Street names on the corners where streets meet the avenues."""
    for (x, z, facing, key) in (
            (CX + AVENUE_HALF + 1, 30, "east", "canticle_way"),
            (CX - AVENUE_HALF - 1, 190, "west", "canticle_way"),
            (40, CZ + AVENUE_HALF + 1, "south", "held_note_avenue"),
            (184, CZ - AVENUE_HALF - 1, "north", "held_note_avenue"),
            (78 + STREET_HALF + 1, 30, "east", "listeners_stair"),
            (146 + STREET_HALF + 1, 30, "east", "starward_steps"),
            (146 - STREET_HALF - 1, 90, "west", "tuners_lane"),
            (44 - STREET_HALF - 1, 52, "west", "garden_of_quiet"),
            (180 + STREET_HALF + 1, 96, "east", "lantern_walk")):
        if not INSIDE[x, z]:
            continue
        c.fill(x, S + 1, z, x, S + 2, z, k.wall())
        c.set(x, S + 3, z, k.chiseled)
        dx, dz = STEP[facing]
        c.set(x + dx, S + 3, z + dz, c.b("spruce_wall_sign", facing=facing, waterlogged="false"),
              nbt=sign_nbt(LORE.SIGNS[key]))



# ── connections ──────────────────────────────────────────────────────────────
def bake_connections(c: City) -> None:
    """Walls and fences carry their connections as state (nothing re-evaluates
    them after placement, see gen_hush_structures.py). MC WallBlock: a side is
    `low` toward another wall or a sturdy cube, `tall` when a cube sits right
    above the neighbour on that side; `up` unless the wall is a straight run
    with nothing standing on it. FenceBlock: true toward fences and cubes."""
    def name(i: int) -> str:
        return c.palette[i][0] if i > AIR else ""

    def cube(i: int) -> bool:
        return name(i) in SOLID_CUBES

    wall_ids = [i for i, (n, _) in enumerate(c.palette) if n == WALL_NAME]
    fence_ids = [i for i, (n, _) in enumerate(c.palette) if n == FENCE_NAME]
    walls = [tuple(map(int, p)) for p in np.argwhere(np.isin(c.v, wall_ids))]
    fences = [tuple(map(int, p)) for p in np.argwhere(np.isin(c.v, fence_ids))]
    new_ids = {}
    for (x, y, z) in walls:
        sides = {}
        for f, (dx, dz) in STEP.items():
            n_ = c.get(x + dx, y, z + dz)
            if name(n_) == WALL_NAME or cube(n_):
                sides[f] = "tall" if cube(c.get(x + dx, y + 1, z + dz)) else "low"
            else:
                sides[f] = "none"
        above = c.get(x, y + 1, z)
        straight = (sides["north"] != "none" and sides["south"] != "none" and sides["east"] == "none"
                    and sides["west"] == "none") or \
                   (sides["east"] != "none" and sides["west"] != "none" and sides["north"] == "none"
                    and sides["south"] == "none")
        up = not straight or above > AIR
        new_ids[(x, y, z)] = c.b("choirstone_brick_wall", east=sides["east"], north=sides["north"],
                                 south=sides["south"], west=sides["west"],
                                 up="true" if up else "false", waterlogged="false")
    for (x, y, z) in fences:
        props = {}
        for f, (dx, dz) in STEP.items():
            n_ = c.get(x + dx, y, z + dz)
            props[f] = "true" if (name(n_) == FENCE_NAME or cube(n_)) else "false"
        new_ids[(x, y, z)] = c.b("whisperwood_fence", waterlogged="false", **props)
    for (x, y, z), i in new_ids.items():
        c.v[x, y, z] = i


def check_properties(c: City) -> int:
    """Every palette entry's properties against the block's state definition
    (the engine's, which gen_block_states.py derives from vanilla and which
    Blocks.cpp's classes must match): an unknown property or value throws in
    TemplateEngine::resolvePaletteEntry and kills structure generation."""
    import gen_block_states as GBS
    upstream = {b["name"]: b for b in json.load(open(GBS.MCDATA))}
    used = set(int(i) for i in np.unique(c.v)) | {AIR}
    bad = 0
    for i in sorted(used):
        if i <= AIR:
            continue
        name, props = c.palette[i]
        slug = name.split(":")[1]
        explicit = GBS.explicit_for(slug)
        if explicit is not None:
            known = {p[0]: [str(v) for v in p[2]] for p in explicit}
        else:
            row = upstream.get(slug) or upstream.get(GBS.alias_for(slug) or "") \
                or upstream.get(GBS.ALIAS_EXACT.get(GBS.alias_for(slug) or "", ""), None)
            if row is None:
                print(f"   UNKNOWN block {name}")
                bad += 1
                continue
            known = {st["name"]: GBS.property_values(st) for st in row["states"]}
        for key, val in props:
            if key not in known or val not in known[key]:
                print(f"   BAD property {name}[{key}={val}] (has {sorted(known)})")
                bad += 1
    print("property check: " + ("all palette properties valid" if bad == 0 else f"{bad} problems"))
    return bad


# ── pieces ───────────────────────────────────────────────────────────────────
class PieceOut:
    def __init__(self, name: str, x0: int, y0: int, z0: int, x1: int, y1: int, z1: int) -> None:
        self.name = name
        self.box = (x0, y0, z0, x1, y1, z1)
        self.jigsaws: dict[tuple[int, int, int], HS.Tag] = {}
        self.orient: dict[tuple[int, int, int], str] = {}

    @property
    def location(self) -> str:
        return f"minecraft:{FOLDER}/{self.name}"


def block_spec(c: City, i: int) -> str:
    name, props = c.palette[i]
    if not props:
        return name
    return name + "[" + ",".join(f"{k_}={v_}" for k_, v_ in props) + "]"


def cut_pieces(c: City) -> tuple[list[PieceOut], dict]:
    """Cut the volume into the 7 x 7 upper pieces (y >= S - 1) and the lower
    pieces under them, and link them: a breadth-first tree over the upper
    grid from the centre (unique jigsaw names, so each piece attaches in one
    rotation only), each lower piece hanging from its upper piece."""
    pieces: dict[tuple, PieceOut] = {}
    for ti in range(NT):
        for tj in range(NT):
            x0, z0 = ti * TILE, tj * TILE
            sub = c.v[x0:x0 + TILE, :, z0:z0 + TILE]
            ys = np.nonzero((sub != UNSET).any(axis=(0, 2)))[0]
            if ys.size == 0:
                continue
            ytop = int(ys.max())
            if ytop >= S - 1:
                pieces[("u", ti, tj)] = PieceOut(f"u_{ti}_{tj}", x0, S - 1, z0, x0 + TILE - 1, max(ytop, S), z0 + TILE - 1)
            if int(ys.min()) <= S - 2:
                pieces[("l", ti, tj)] = PieceOut(f"l_{ti}_{tj}", x0, int(ys.min()), z0, x0 + TILE - 1, S - 2,
                                                 z0 + TILE - 1)
    start = ("u", NT // 2, NT // 2)
    assert start in pieces
    # Breadth-first over the upper grid.
    depth = {start: 0}
    parent = {}
    q = deque([start])
    while q:
        cur = q.popleft()
        _, ti, tj = cur
        for (di, dj) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nb = ("u", ti + di, tj + dj)
            if nb in pieces and nb not in depth and _link_cell(c, pieces[cur], pieces[nb], di, dj) is not None:
                depth[nb] = depth[cur] + 1
                parent[nb] = cur
                q.append(nb)
    missing = [key for key in pieces if key[0] == "u" and key not in depth]
    assert not missing, f"unreachable upper pieces: {missing}"
    links = []
    for child, par in parent.items():
        di = child[1] - par[1]; dj = child[2] - par[2]
        links.append((par, child, _link_cell(c, pieces[par], pieces[child], di, dj)))
    for key in list(pieces):
        if key[0] == "l":
            up = ("u", key[1], key[2])
            cell = _down_cell(c, pieces[up], pieces[key])
            assert cell is not None, f"no link cell for {key}"
            depth[key] = depth[up] + 1
            links.append((up, key, cell))
    for (par, child, (pc, cc, front)) in links:
        pp, cp = pieces[par], pieces[child]
        name = f"minecraft:{FOLDER}/link_{pp.name}_{cp.name}"
        pp.jigsaws[pc] = _jigsaw_nbt(name, f"minecraft:{FOLDER}/{cp.name}", block_spec(c, c.get(*pc)))
        pp.orient[pc] = front
        cp.jigsaws[cc] = _jigsaw_nbt(name, "minecraft:empty", block_spec(c, c.get(*cc)))
        cp.orient[cc] = OPPOSITE_FRONT[front]
    meta = {"start": pieces[start], "max_depth": max(depth.values()), "pieces": pieces}
    return list(pieces.values()), meta


OPPOSITE_FRONT = {"north": "south", "south": "north", "east": "west", "west": "east", "up": "down", "down": "up"}


def _link_cell(c: City, a: PieceOut, b: PieceOut, di: int, dj: int):
    """A pair of set cells across the shared face of two upper pieces (a's
    cell on its face toward b, b's cell just across), street level first."""
    ax0, ay0, az0, ax1, ay1, az1 = a.box
    bx0, by0, bz0, bx1, by1, bz1 = b.box
    front = {(1, 0): "east", (-1, 0): "west", (0, 1): "south", (0, -1): "north"}[(di, dj)]
    ylo, yhi = max(ay0, by0), min(ay1, by1)
    order = [S] + [y for y in range(ylo, yhi + 1) if y != S]
    for y in order:
        if y < ylo or y > yhi:
            continue
        span = range(az0, az1 + 1) if di else range(ax0, ax1 + 1)
        mid = (az0 + az1) // 2 if di else (ax0 + ax1) // 2
        for t in sorted(span, key=lambda t: abs(t - mid)):
            if di:
                pc = (ax1 if di > 0 else ax0, y, t)
                cc = (pc[0] + di, y, t)
            else:
                pc = (t, y, az1 if dj > 0 else az0)
                cc = (t, y, pc[2] + dj)
            if c.get(*pc) > UNSET and c.get(*cc) > UNSET and pc not in c.nbt and cc not in c.nbt \
                    and pc not in a.jigsaws and cc not in b.jigsaws:
                return (pc, cc, front)
    return None


def _down_cell(c: City, up: PieceOut, low: PieceOut):
    x0, _, z0, x1, _, z1 = up.box
    for x in range(x0, x1 + 1):
        for z in range(z0, z1 + 1):
            pc, cc = (x, S - 1, z), (x, S - 2, z)
            if c.get(*pc) > UNSET and c.get(*cc) > UNSET and pc not in c.nbt and cc not in c.nbt \
                    and pc not in up.jigsaws and cc not in low.jigsaws:
                return (pc, cc, "down")
    return None


def _jigsaw_nbt(name: str, pool: str, final_state: str) -> HS.Tag:
    return T_comp({
        "joint": T_str("aligned"),
        "final_state": T_str(final_state),
        "name": T_str(name),
        "pool": T_str(pool),
        "id": T_str("minecraft:jigsaw"),
        "target": T_str(name),
        "selection_priority": T_int(0),
        "placement_priority": T_int(0),
    })


def piece_nbt(c: City, p: PieceOut) -> tuple[bytes, int]:
    x0, y0, z0, x1, y1, z1 = p.box
    sub = c.v[x0:x1 + 1, y0:y1 + 1, z0:z1 + 1]
    coords = np.argwhere(sub != UNSET)
    # Loader order: y, then z, then x (StructureTemplate's own save order).
    order = np.lexsort((coords[:, 0], coords[:, 2], coords[:, 1]))
    coords = coords[order]
    palette: list[tuple] = []
    index: dict[tuple, int] = {}
    blocks = []
    for (lx, ly, lz) in coords:
        wx, wy, wz = int(lx) + x0, int(ly) + y0, int(lz) + z0
        if (wx, wy, wz) in p.jigsaws:
            key = ("minecraft:jigsaw", (("orientation", JIGSAW_ORIENT[p.orient[(wx, wy, wz)]]),))
            nbt = p.jigsaws[(wx, wy, wz)]
        else:
            key = c.palette[int(sub[lx, ly, lz])]
            nbt = c.nbt.get((wx, wy, wz))
        if key not in index:
            index[key] = len(palette)
            palette.append(key)
        entry = {"pos": T_list(3, [int(lx), int(ly), int(lz)]), "state": T_int(index[key])}
        if nbt is not None:
            entry["nbt"] = nbt
        blocks.append(entry)
    palette_tags = []
    for (name, props) in palette:
        e = {}
        if props:
            e["Properties"] = T_comp({k_: T_str(v_) for k_, v_ in props})
        e["Name"] = T_str(name)
        palette_tags.append(e)
    # The inhabitants whose block lies in this piece, piece-local (the
    # loader re-bases them to the placement; see place_inhabitants).
    entities = [HS.template_entity(ex - x0, ey - y0, ez - z0, nbt)
                for (ex, ey, ez, nbt) in c.entities
                if x0 <= math.floor(ex) <= x1 and y0 <= math.floor(ey) <= y1
                and z0 <= math.floor(ez) <= z1]
    root = T_comp({
        "size": T_list(3, [x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1]),
        "entities": T_list(10, entities) if entities else T_list(0, []),
        "blocks": T_list(10, blocks),
        "palette": T_list(10, palette_tags),
        "DataVersion": T_int(HS.DATA_VERSION),
    })
    return HS.gzip_bytes(HS.serialize("", root)), len(blocks)


JIGSAW_ORIENT = {"north": "north_up", "south": "south_up", "east": "east_up", "west": "west_up",
                 "up": "up_north", "down": "down_north"}


# ── loot ─────────────────────────────────────────────────────────────────────
# (item, min, max, weight). "book*" is an enchanted book.
LOOT = {
    "aurelith_stillhouse": ((3, 6), [
        ("whisperfruit", 1, 3, 8), ("resonance_bloom", 1, 4, 6), ("paper", 1, 4, 6),
        ("glow_ink_sac", 1, 2, 4), ("echo_shard", 1, 2, 5), ("white_candle", 1, 3, 5),
        ("amethyst_shard", 1, 3, 4), ("string", 1, 3, 3), ("book", 1, 2, 4), ("name_tag", 1, 1, 1),
        ("cyan_carpet", 1, 3, 3), ("music_disc_cat", 1, 1, 1)]),
    "aurelith_arcade": ((4, 7), [
        ("choir_lamp", 1, 2, 5), ("cyan_lumen_panel", 1, 4, 4), ("violet_lumen_panel", 1, 4, 3),
        ("amber_lumen_panel", 1, 4, 3), ("nightglass", 2, 6, 4), ("resonite_ingot", 1, 3, 5),
        ("echo_shard", 2, 5, 6), ("echo_lantern", 1, 2, 4), ("amethyst_shard", 2, 6, 4),
        ("spyglass", 1, 1, 2), ("glow_ink_sac", 2, 4, 4), ("lead", 1, 1, 2), ("cyan_dye", 2, 6, 2),
        ("purple_dye", 2, 6, 2), ("orange_dye", 2, 6, 2)]),
    "aurelith_archive": ((3, 6), [
        ("paper", 3, 8, 8), ("book", 1, 3, 8), ("writable_book", 1, 1, 3), ("book*", 1, 1, 6),
        ("glow_ink_sac", 1, 3, 4), ("echo_shard", 2, 4, 6), ("disc_fragment_5", 1, 2, 3),
        ("recovery_compass", 1, 1, 1), ("echo_compass", 1, 1, 2)]),
    "aurelith_quays": ((3, 6), [
        ("fishing_rod", 1, 1, 4), ("glow_ink_sac", 2, 5, 6), ("ink_sac", 1, 3, 4),
        ("nautilus_shell", 1, 2, 3), ("heart_of_the_sea", 1, 1, 1), ("lead", 1, 2, 3),
        ("whisperwood_planks", 4, 10, 5), ("string", 2, 6, 4), ("iron_ingot", 1, 3, 3),
        ("echo_shard", 1, 3, 4), ("compass", 1, 1, 2)]),
    "aurelith_tuners": ((4, 7), [
        ("resonite_ingot", 2, 5, 8), ("raw_resonite", 2, 6, 6), ("resonant_crystal", 2, 5, 6),
        ("crystal_conduit", 2, 6, 5), ("lumen_strip", 2, 6, 4), ("echo_shard", 2, 5, 6),
        ("tuning_fork", 1, 1, 2), ("redstone", 2, 8, 3), ("amethyst_shard", 2, 6, 3),
        ("resonite_pickaxe", 1, 1, 1)]),
    "aurelith_instruments": ((3, 5), [
        ("note_block", 1, 3, 5), ("resonant_chime", 1, 2, 4), ("amethyst_shard", 2, 6, 5),
        ("goat_horn", 1, 1, 2), ("bell", 1, 1, 1), ("music_disc_13", 1, 1, 1),
        ("music_disc_otherside", 1, 1, 1), ("music_disc_relic", 1, 1, 1), ("music_disc_5", 1, 1, 1),
        ("disc_fragment_5", 1, 3, 4), ("echo_shard", 1, 3, 4), ("recall_chime", 1, 1, 1)]),
    "aurelith_gardens": ((3, 6), [
        ("whisperwood_sapling", 1, 3, 6), ("resonance_bloom", 2, 6, 8), ("whisperfruit", 2, 5, 6),
        ("hush_moss", 2, 6, 5), ("bone_meal", 3, 8, 5), ("glow_berries", 2, 5, 3),
        ("honey_bottle", 1, 2, 2)]),
    "aurelith_observatory": ((3, 5), [
        ("spyglass", 1, 1, 5), ("compass", 1, 1, 3), ("clock", 1, 1, 3), ("glow_ink_sac", 1, 4, 4),
        ("amethyst_shard", 2, 6, 5), ("paper", 2, 6, 5), ("echo_compass", 1, 1, 2),
        ("firework_rocket", 1, 3, 2), ("resonant_crystal", 1, 3, 3)]),
    "aurelith_gatehouse": ((3, 5), [
        ("arrow", 4, 12, 8), ("iron_ingot", 1, 4, 5), ("shield", 1, 1, 3), ("echo_shard", 1, 3, 5),
        ("resonance_bow", 1, 1, 1), ("lantern", 1, 2, 3), ("string", 2, 6, 4), ("raw_resonite", 1, 3, 4)]),
    "aurelith_spire": ((4, 7), [
        ("echo_shard", 3, 6, 6), ("resonite_ingot", 2, 4, 5), ("golden_apple", 1, 1, 3), ("book*", 1, 1, 4),
        ("spyglass", 1, 1, 2), ("echo_compass", 1, 1, 2), ("recall_chime", 1, 1, 1),
        ("cloak_of_silence", 1, 1, 1), ("experience_bottle", 2, 6, 4), ("diamond", 1, 3, 3)]),
    "aurelith_vault": ((5, 8), [
        ("resonite_ingot", 4, 8, 8), ("resonite_block", 1, 2, 3), ("echo_shard", 6, 12, 8),
        ("diamond", 2, 5, 5), ("enchanted_golden_apple", 1, 1, 2), ("golden_apple", 1, 3, 5),
        ("music_disc_5", 1, 1, 3), ("book*", 1, 1, 6), ("voice_beacon", 1, 1, 2),
        ("recall_chime", 1, 1, 2), ("cloak_of_silence", 1, 1, 2), ("echo_compass", 1, 1, 2),
        ("experience_bottle", 4, 10, 5), ("resonant_heart", 1, 1, 1)]),
}
LOOT_BOOK_HOMES = {
    "aurelith_stillhouse": "stillhouses", "aurelith_arcade": "arcade", "aurelith_archive": "archive",
    "aurelith_quays": "quays", "aurelith_tuners": "tuners", "aurelith_instruments": "instruments",
    "aurelith_gardens": "gardens", "aurelith_observatory": "observatory", "aurelith_gatehouse": "gates",
    "aurelith_spire": "spire", "aurelith_vault": "vault",
}


def _count(lo: int, hi: int) -> dict:
    if lo == hi:
        return {"add": False, "count": float(lo), "function": "minecraft:set_count"}
    return {"add": False, "count": {"type": "minecraft:uniform", "min": float(lo), "max": float(hi)},
            "function": "minecraft:set_count"}


def loot_table(name: str) -> dict:
    (rmin, rmax), items = LOOT[name]
    entries = []
    for (item, lo, hi, w) in items:
        if item == "book*":
            entries.append({"type": "minecraft:item", "name": "minecraft:book", "weight": w,
                            "functions": [_count(1, 1), {"function": "minecraft:enchant_randomly",
                                                         "options": "#minecraft:on_random_loot"}]})
        else:
            entries.append({"type": "minecraft:item", "name": f"minecraft:{item}", "weight": w,
                            "functions": [_count(lo, hi)]})
    pools = [{"bonus_rolls": 0.0, "rolls": {"type": "minecraft:uniform", "min": float(rmin), "max": float(rmax)},
              "entries": entries}]
    # The district's lore: one written book, most of the time (always in the vault).
    books = []
    for key in LORE.BOOK_HOMES[LOOT_BOOK_HOMES[name]]:
        b = LORE.BOOKS[key]
        books.append({
            "type": "minecraft:item", "name": "minecraft:written_book", "weight": 3,
            "functions": [
                {"function": "minecraft:set_book_cover", "title": {"raw": b["title"]},
                 "author": b["author"], "generation": 0},
                {"function": "minecraft:set_written_book_pages", "pages": list(b["pages"]),
                 "mode": "replace_all"},
            ]})
    if name != "aurelith_vault":
        books.append({"type": "minecraft:empty", "weight": 2})
    pools.append({"bonus_rolls": 0.0, "rolls": 1.0, "entries": books})
    return {"type": "minecraft:chest", "pools": pools,
            "random_sequence": f"minecraft:chests/{name}"}


# ── output ───────────────────────────────────────────────────────────────────
def pool_json(location: str) -> dict:
    return {"elements": [{"element": {"element_type": "minecraft:single_pool_element",
                                      "location": location, "processors": PROCESSORS,
                                      "projection": "rigid"}, "weight": 1}],
            "fallback": "minecraft:empty"}


def write_all(c: City, outdir: Path) -> list[Path]:
    pieces, meta = cut_pieces(c)
    written = []
    struct_dir = outdir / "minecraft" / "structure" / FOLDER
    pool_dir = outdir / "minecraft" / "worldgen" / "template_pool" / FOLDER
    struct_dir.mkdir(parents=True, exist_ok=True)
    pool_dir.mkdir(parents=True, exist_ok=True)
    for stale in list(struct_dir.glob("*.nbt")) + list(pool_dir.glob("*.json")):
        stale.unlink()
    total = 0
    for p in pieces:
        data, n = piece_nbt(c, p)
        total += n
        path = struct_dir / f"{p.name}.nbt"
        path.write_bytes(data)
        written.append(path)
        (pool_dir / f"{p.name}.json").write_text(json.dumps(pool_json(p.location), indent=2) + "\n")
    start = meta["start"]
    (pool_dir / "start.json").write_text(json.dumps(pool_json(start.location), indent=2) + "\n")
    print(f"wrote {len(pieces)} pieces, {total} listed blocks, max depth {meta['max_depth']}")
    wg = outdir / "minecraft" / "worldgen"
    structure = {
        "type": "minecraft:jigsaw",
        "biomes": "#minecraft:has_structure/aurelith",
        "max_distance_from_center": {"horizontal": 128, "vertical": 160},
        "size": meta["max_depth"] + 1,
        "spawn_overrides": aurelith_spawn_overrides(pieces),
        "start_height": {"absolute": (S - 1) - S},
        "start_pool": f"minecraft:{FOLDER}/start",
        "project_start_to_heightmap": "WORLD_SURFACE_WG",
        "step": "surface_structures",
        "terrain_adaptation": "beard_box",
        "use_expansion_hack": False,
        "level_site": {"radius": 100, "max_spread": 24},
    }
    (wg / "structure" / "aurelith.json").write_text(json.dumps(structure, indent=2) + "\n")
    structure_set = {
        "placement": {"type": "minecraft:random_spread", "salt": 1147031147, "separation": 32, "spacing": 90},
        "structures": [{"structure": "minecraft:aurelith", "weight": 1}],
    }
    (wg / "structure_set" / "aurelith.json").write_text(json.dumps(structure_set, indent=2) + "\n")
    processors = {"processors": [
        {"processor_type": "minecraft:rule", "rules": [
            _rule("minecraft:choirstone_bricks", 0.06, "minecraft:cracked_choirstone_bricks"),
            _rule("minecraft:choirstone_tiles", 0.03, "minecraft:cracked_choirstone_bricks"),
            _rule("minecraft:polished_choirstone", 0.015, "minecraft:sculk"),
            _rule("minecraft:dim_choir_lamp", 0.04, "minecraft:air"),
            _rule("minecraft:nightglass", 0.03, "minecraft:air"),
        ]},
        {"processor_type": "minecraft:protected_blocks", "value": "#minecraft:features_cannot_replace"},
    ]}
    (wg / "processor_list" / "aurelith_weathering.json").write_text(json.dumps(processors, indent=2) + "\n")
    tag = outdir / "minecraft" / "tags" / "worldgen" / "biome" / "has_structure" / "aurelith.json"
    tag.write_text(json.dumps({"values": ["minecraft:hush_meadows", "minecraft:aurora_steppe",
                                          "minecraft:resonant_barrens"]}, indent=2) + "\n")
    loot_dir = outdir / "minecraft" / "loot_table" / "chests"
    for name in LOOT:
        (loot_dir / f"{name}.json").write_text(json.dumps(loot_table(name), indent=2) + "\n")
    print(f"wrote structure, structure_set, processor_list, biome tag, {len(LOOT)} loot tables")
    return written


def _rule(block: str, p: float, out: str) -> dict:
    return {"input_predicate": {"block": block, "predicate_type": "minecraft:random_block_match",
                                "probability": p},
            "location_predicate": {"predicate_type": "minecraft:always_true"},
            "output_state": {"Name": out}}



# ── walkability check ────────────────────────────────────────────────────────
# A breadth-first walk over the positions a player can stand at, from the
# plaza, so "walkable, no parkour" is checked rather than hoped for. Heights
# are in half blocks. A step up of half a block is walked (a slab, the low
# half of a stair), a full stair is walked from the half below it, a one-block
# step is a JUMP (counted separately), any drop of up to 4 blocks is taken.
# Ladders and water move you vertically; doors and the vault's trapdoor open.
PASS_NAMES = {"air", "hush_grass", "resonance_bloom", "sculk_vein", "glow_lichen", "vine", "cobweb",
              "spruce_wall_sign", "oak_wall_sign", "cyan_carpet", "purple_carpet", "light_blue_carpet",
              "white_candle", "resonant_cluster", "whisperwood_door", "whisperwood_trapdoor", "ladder",
              "resonant_water", "water", "iron_chain", "moss_carpet", "hanging_roots", "sculk_sensor"}
TALL_NAMES = {"choirstone_brick_wall", "whisperwood_fence"}


def walk_check(c: City, start=(CX + 12, S + 1, CZ + 14), jumps: bool = False) -> set:
    names = [n.split(":")[1] if n else "" for (n, _) in c.palette]
    props = [dict(p) for (_, p) in c.palette]
    v = c.v

    def kind(x, y, z):
        """'pass', 'full', 'slab', 'tslab', 'stair', 'tall', 'ladder', 'water'."""
        if not (0 <= x < W and 0 <= y < HY and 0 <= z < D):
            return "pass"
        i = int(v[x, y, z])
        if i == UNSET:
            return "full" if y <= S and not INSIDE[x, z] else "pass"
        n = names[i]
        if n in ("resonant_water", "water"):
            return "water"
        if n == "ladder":
            return "ladder"
        if n in PASS_NAMES:
            return "pass"
        if n in TALL_NAMES:
            return "tall"
        if n.endswith("_slab"):
            return "slab" if props[i].get("type") == "bottom" else ("tslab" if props[i].get("type") == "top" else "full")
        if n.endswith("_stairs"):
            return "stair" if props[i].get("half", "bottom") == "bottom" else "full"
        return "full"

    def free(x, y, z):
        return kind(x, y, z) in ("pass", "ladder", "water")

    def floors(x, z):
        """Feet heights (half blocks) a player can stand at in this column."""
        out = []
        for y in range(S - 14, HY - 3):
            k_ = kind(x, y, z)
            if k_ == "full" or k_ == "tslab":
                f = 2 * y + 2
                if free(x, y + 1, z) and free(x, y + 2, z):
                    out.append(f)
            elif k_ in ("slab", "stair"):
                if free(x, y + 1, z) and kind(x, y + 2, z) != "full":
                    out.append(2 * y + 1 if k_ == "slab" else 2 * y + 2)
            elif k_ in ("ladder", "water"):
                if free(x, y + 1, z) or kind(x, y + 1, z) in ("ladder", "water"):
                    out.append(2 * y)
        return out

    cache = {}

    def col(x, z):
        key = (x, z)
        if key not in cache:
            cache[key] = floors(x, z) if (0 <= x < W and 0 <= z < D) else []
        return cache[key]

    sx, sy, sz = start
    seen = set()
    q = deque()
    for f in col(sx, sz):
        if abs(f - 2 * sy) <= 1:
            seen.add((sx, sz, f)); q.append((sx, sz, f))
    while q:
        x, z, f = q.popleft()
        y = f // 2
        here = kind(x, y, z)
        # Vertical movement on ladders and in water.
        if here in ("ladder", "water"):
            for nf in col(x, z):
                if abs(nf - f) <= 2 and (x, z, nf) not in seen:
                    seen.add((x, z, nf)); q.append((x, z, nf))
        for dx, dz in N4:
            nx, nz = x + dx, z + dz
            for nf in col(nx, nz):
                d = nf - f
                ny = nf // 2
                stair_up = kind(nx, (nf - 2) // 2, nz) == "stair" and d == 2
                # Off the top of a ladder (or out of water) onto the ledge
                # beside it: MC lets you climb until your feet clear it.
                climb_out = here in ("ladder", "water") and d == 2
                ok = d <= 1 or stair_up or climb_out or (jumps and d <= 2) or (-8 <= d < 0)
                if d > 2:
                    ok = False
                if ok and (nx, nz, nf) not in seen:
                    seen.add((nx, nz, nf)); q.append((nx, nz, nf))
    return seen


def walk_report(c: City) -> int:
    walk = walk_check(c)
    jump = walk_check(c, jumps=True)
    stand = lambda x, y, z, s_: any((x + dx, z + dz, f) in s_ for dx in (-1, 0, 1) for dz in (-1, 0, 1)
                                    for f in (2 * y - 1, 2 * y, 2 * y + 1))
    targets = {
        "dais top (the Heart)": (CX + 1, S + 4, CZ + 1),
        "Conductor's Podium": (CX, S + 3, CZ - 13),
        "vault chamber": (CX + 3, S - 9, CZ + 3),
        "Spire view terrace": (SPIRE[0] - 3, S + 71, SPIRE[1] + 3),
        "sky bridge (Archive side)": (ARCHIVE[0] + 16, BRIDGE_Y + 1, ARCHIVE[1]),
        "sky bridge (Spire side)": (SPIRE[0] - 12, BRIDGE_Y + 1, SPIRE[1]),
        "Archive fifth gallery": (ARCHIVE[0] + 9, S + 36, ARCHIVE[1]),
        "Starward drum": (STARWARD[0], S + 31, STARWARD[1] + 2),
        "north gate crown": (CX - 12, S + 42, CZ - 100),
        "south gate walk": (CX, S + 20, CZ + 104),
        "harbour pier end": (HARBOUR[0] - 7, S + 1, HARBOUR[1] + 1),
        "Vesper's Spring rim": (SPRING[0] + 10, S + 1, SPRING[1]),
        "Quiet Gardens upper terrace": (16, S + 5, 60),
        # Reawakening the Heart (build_quest): where the keys wait. The
        # Soprano's cell is sealed on purpose (bookshelves to break).
        "Alto cabinet": QUEST_SPOTS["alto"],
        "Tenor perch": QUEST_SPOTS["tenor"],
        "Bass pedestal": QUEST_SPOTS["bass"],
        "Podium sockets": (CX, S + 2, CZ - 12),
    }
    bad = 0
    for name, (x, y, z) in targets.items():
        a, b = stand(x, y, z, walk), stand(x, y, z, jump)
        status = "walk" if a else ("JUMP" if b else "UNREACHABLE")
        if not a:
            bad += 1
        print(f"   {name:<30} {status}")
    roofs = [(fm, info) for (fm, info) in BUILT]
    ok_roof = 0
    for (fm, info) in roofs:
        pts = np.argwhere(erode(fm))
        if any((int(x), int(z), f) in walk for (x, z) in pts
               for f in range(2 * (info["roof_y"] + 1), 2 * (info["roof_y"] + 4))):
            ok_roof += 1
    print(f"   mid-rise roofs reachable on foot: {ok_roof} of {len(roofs)}")
    ok_house = 0
    for (x0, z0, x1, z1) in HOUSES:
        if any((x, z, 2 * (S + 6)) in walk for x in range(x0 + 1, x1) for z in range(z0 + 1, z1)):
            ok_house += 1
    print(f"   Stillhouse upper floors reachable on foot: {ok_house} of {len(HOUSES)}")
    print(f"   standable positions: {len(walk)} walking, {len(jump)} with jumps")
    return bad



# ── plaques, the city map, the rooftop secret ────────────────────────────────
def place_plaque(c: City, k: Kit, x: int, y: int, z: int, facing: str, key: str) -> bool:
    """Hang a glowing plaque (a waxed wall sign) on the face of a solid
    block: (x, y, z) is the sign's own cell, the block behind it (against
    `facing`) must be a cube. Searches a few cells along the wall if not."""
    dx, dz = STEP[facing]
    lx, lz = STEP[LEFT[facing]]
    for shift in (0, 1, -1, 2, -2, 3, -3):
        sx, sz = x + lx * shift, z + lz * shift
        behind = c.get(sx - dx, y, sz - dz)
        if c.get(sx, y, sz) in (AIR, UNSET) and behind > AIR and c.name_of(behind) in SOLID_CUBES:
            c.set(sx, y, sz, c.b("spruce_wall_sign", facing=facing, waterlogged="false"),
                  nbt=sign_nbt(LORE.SIGNS[key]))
            return True
    return False


def build_details(c: City, k: Kit) -> None:
    """The last touches: the Stillhouses' motto and the Tuners' Works plaque,
    the city's plan laid out in light on the Spire's lobby floor, and a
    chest waiting on top of the needle the sky bridge passes through."""
    # "EVERY VOICE A LANTERN" on the first Stillhouse's street face.
    if HOUSES:
        x0, z0, x1, z1 = HOUSES[0]
        place_plaque(c, k, (x0 + x1) // 2 + 2, S + 3, z1 + 1, "south", "stillhouse_motto")
    # The Tuners' Works: on the crescent's plaza face, north-east.
    for r_ in range(37, 44):
        x = int(round(CX + r_ * 0.7071)); z = int(round(CZ - r_ * 0.7071))
        if c.get(x, S + 3, z) > AIR and c.name_of(c.get(x, S + 3, z)) in SOLID_CUBES:
            if place_plaque(c, k, x - 1, S + 3, z, "west", "tuners_works") or \
                    place_plaque(c, k, x, S + 3, z + 1, "south", "tuners_works"):
                break
    # The plan of Aurelith in light on the Spire's lobby floor (1 cell = 16
    # blocks): the walls in Stave stone, the avenues as cyan light, the river
    # violet, the Heart a crystal, the towers amber, the rest pale tiles.
    sx, sz = SPIRE
    scale = 16
    half = 6
    for mx in range(-half, half + 1):
        for mz in range(-half, half + 1):
            x, z = sx + mx, sz + mz
            if c.get(x, S + 1, z) != AIR or c.name_of(c.get(x, S, z)) != f"{NS}:polished_choirstone":
                continue
            cxw = CX + mx * scale; czw = CZ + mz * scale
            if not (0 <= cxw < W and 0 <= czw < D):
                continue
            cell = k.tiles
            if WALL_BAND[max(0, min(W - 1, cxw)), max(0, min(D - 1, czw))] or \
                    (not INSIDE[cxw, czw] and OCT[cxw, czw] < WALL_OUT + 8):
                cell = k.stave
            elif mx == 0 and mz == 0:
                cell = k.crystal
            elif abs(cxw - CX) <= AVENUE_HALF or abs(czw - CZ) <= AVENUE_HALF:
                cell = k.cyan
            elif WATER_MASK[cxw, czw] or abs(RIVER_T[cxw, czw]) <= RIVER_HALF + 3:
                cell = k.violet
            elif any(math.hypot(cxw - tx, czw - tz) < 12 for (tx, tz) in (ARCHIVE, SPIRE, STARWARD)):
                cell = k.amber
            elif not INSIDE[cxw, czw]:
                continue
            c.set(x, S, z, cell)
    # The rooftop secret: a hatch through the top of needle A, a chest.
    nx, nz = NEEDLE_A
    top = S + 58
    c.set(nx, top, nz + 1, AIR)
    c.set(nx, top, nz + 1, c.b("ladder", facing="north", waterlogged="false"))
    c.set(nx - 1, top + 1, nz - 1, c.b("chest", facing="south", type="single", waterlogged="false"),
          nbt=loot_nbt("aurelith_spire"))
    c.set(nx + 1, top + 1, nz - 1, k.lamp)


# ── lived-in interiors: room kits ────────────────────────────────────────────
# Every door in the city opens onto a room somebody left in the middle of
# their life. furnish_room(kind=...) hands a floor's free cells to one of the
# kits below; each lays its furniture against the walls it finds (facing into
# the room), keeps every stair, landing and doorway clear (c.keep_clear) and
# never stacks onto another piece. The kits vary by seeded choice, so no two
# flats on a street are laid out alike.
#
#   home        a flat: bed(s) against a wall with a nightstand and a rug, a
#               kitchen run (smoker, counter, cauldron, barrel), a table laid
#               for two, shelves
#   stillhouse  a Stillhouse's ground floor: the hearth, the kitchen, the
#               table, the dresser; its upper floor is `bedroom`
#   bedroom     wardrobes (barrels, shelves), a chest at the bed's foot, rugs
#   workshop    workbenches (crafting / smithing / fletching table,
#               grindstone, stonecutter), stock racks of crystal and conduit,
#               barrels
#   tuners      the Tuners' own workshop: conduit racks, crystal stock,
#               benches with chimes and note blocks for testing
#   study       desks with chairs and candles, shelves two high, a map table,
#               a lectern where there is a book to put on it
#   shop        a counter along the street wall with goods on it, stock shelves
#               behind, the till chest
#   guard       the gatehouses' guard room: bunks, a mess table, stores
#
# The Arcade's stalls get their own goods by theme (stall_goods).

INTERIOR_PREVIEW_COLOURS = {
    "smoker": ("5A5A5E", 1, 0), "furnace": ("5A5A5E", 1, 0), "melon": ("6A9A2A", 1, 0),
    "grindstone": ("8A8A8A", 1, 0), "smithing_table": ("3A3040", 1, 0),
    "stonecutter": ("7A7A7A", 1, 0), "fletching_table": ("B89A6A", 1, 0),
    "light_blue_carpet": ("3AAFDA", 1, 0), "white_carpet": ("E0E0E0", 1, 0),
    "light_blue_bed": ("3AAFDA", 1, 0), "white_bed": ("E0E0E0", 1, 0), "blue_bed": ("35399D", 1, 0),
    "cyan_wool": ("158B94", 1, 0), "purple_wool": ("6A2A9C", 1, 0), "amethyst_cluster": ("A77CE0", 1, 1),
}
PREVIEW_COLOURS.update(INTERIOR_PREVIEW_COLOURS)


class _Room:
    """A floor's free cells with the bookkeeping the kits share: which cells
    are taken, which lie against a wall (and which way that wall is) — and a
    guard on circulation: every piece of furniture goes in as an `attempt`,
    undone if it cut the room's stairs and doorways (c.keep_clear cells) off
    from one another, so a bed or a table can never wall off a stair."""

    PASSABLE_SUFFIX = ("_carpet",)

    def __init__(self, c: City, cells, y: int) -> None:
        self.c = c
        self.y = y
        self.all_cells = set(cells)
        self.anchors = [p_ for p_ in cells if p_ in c.keep_clear]
        # Nothing on a cell that opens straight to the outside (a colonnade's
        # walk between its pillars, a doorway's threshold): that is a street,
        # not a room, and the way in must stay open.
        self.cells = [p_ for p_ in cells if p_ not in c.keep_clear and not self._opens_out(*p_)]
        self.free = set(self.cells)
        self.log: list | None = None

    def _opens_out(self, x: int, z: int) -> bool:
        return any((x + dx, z + dz) not in self.all_cells and self.c.get(x + dx, self.y, z + dz) == AIR
                   for dx, dz in N4)

    def _passable(self, x: int, z: int) -> bool:
        c, y = self.c, self.y
        i = c.get(x, y, z)
        return (i == AIR or c.name_of(i).endswith(self.PASSABLE_SUFFIX)) and c.get(x, y + 1, z) == AIR

    def connected(self) -> bool:
        """Every anchor that could walk to another before still can."""
        if len(self.anchors) < 2:
            return True
        start = next((a for a in self.anchors if self._passable(*a)), None)
        if start is None:
            return True
        seen = {start}
        todo = [start]
        while todo:
            x, z = todo.pop()
            for dx, dz in N4:
                q = (x + dx, z + dz)
                if q in self.all_cells and q not in seen and self._passable(*q):
                    seen.add(q)
                    todo.append(q)
        return all(a in seen for a in self.anchors if self._passable(*a))

    def attempt(self, fn) -> bool:
        """Run a furniture group; keep it only if circulation survives."""
        self.log = []
        ok = bool(fn())
        log, self.log = self.log, None
        if log and not self.connected():
            for (x, yy, z, old, old_nbt, was_free) in reversed(log):
                self.c.set(x, yy, z, old, old_nbt)
                if was_free:
                    self.free.add((x, z))
            return False
        return ok

    def open(self, x: int, z: int) -> bool:
        c, y = self.c, self.y
        return (x, z) in self.free and c.get(x, y, z) == AIR and c.get(x, y + 1, z) == AIR

    def wall_dir(self, x: int, z: int) -> str | None:
        """The side a solid wall stands on (the furniture faces away)."""
        for f, (dx, dz) in STEP.items():
            if self.c.name_of(self.c.get(x + dx, self.y, z + dz)) in SOLID_CUBES:
                return f
        return None

    def wallside(self, rng: random.Random) -> list[tuple[int, int, str]]:
        out = [(x, z, w) for (x, z) in self.cells if self.open(x, z) for w in [self.wall_dir(x, z)] if w]
        rng.shuffle(out)
        return out

    def centre_cells(self, rng: random.Random) -> list[tuple[int, int]]:
        out = [(x, z) for (x, z) in self.cells if self.open(x, z) and self.wall_dir(x, z) is None]
        rng.shuffle(out)
        return out

    def put(self, x: int, z: int, bid: int, dy: int = 0, nbt=None) -> None:
        if self.log is not None:
            pos = (x, self.y + dy, z)
            self.log.append((x, self.y + dy, z, self.c.get(*pos), self.c.nbt.get(pos),
                             dy == 0 and (x, z) in self.free))
        self.c.set(x, self.y + dy, z, bid, nbt)
        if dy == 0:
            self.free.discard((x, z))

    def take(self, x: int, z: int) -> None:
        self.free.discard((x, z))


def _block_nbt(block: str) -> HS.Tag:
    """The bare block-entity tag a furnace, smoker, bell or pot needs for the
    terrain library to give it its entity (TemplateEngine emits a payload only
    for template entries carrying nbt)."""
    return T_comp({"id": T_str(f"minecraft:{block}")})


def _kit_bed(room: _Room, k: Kit, rng: random.Random, colour: str | None = None) -> bool:
    """A bed with its head to a wall, a rug beside it, a nightstand with a
    candle or a small lantern."""
    c = room.c
    for (x, z, w) in room.wallside(rng):
        dx, dz = STEP[w]
        fx, fz = x - dx, z - dz                        # the foot, into the room
        if not room.open(fx, fz):
            continue
        colour = colour or rng.choice(["cyan_bed", "purple_bed", "light_blue_bed", "white_bed", "blue_bed"])
        room.put(x, z, c.b(colour, facing=w, part="head", occupied="false"))
        room.put(fx, fz, c.b(colour, facing=w, part="foot", occupied="false"))
        lx, lz = STEP[LEFT[w]]
        for side in (1, -1):
            sx, sz = x + lx * side, z + lz * side
            if room.open(sx, sz):
                room.put(sx, sz, k.ww_slab_top)
                room.put(sx, sz, rng.choice([c.b("white_candle", candles=str(rng.randint(1, 3)), lit="false",
                                                 waterlogged="false"),
                                             c.b("flower_pot"), k.echo_lantern_stand]), dy=1)
                break
        rug = c.b(rng.choice(["cyan_carpet", "purple_carpet", "light_blue_carpet", "white_carpet"]))
        for (rx, rz) in ((fx - dx, fz - dz), (fx + lx, fz + lz), (fx - lx, fz - lz)):
            if room.open(rx, rz) and rng.random() < 0.8:
                room.put(rx, rz, rug)
        return True
    return False


def _kit_run(room: _Room, rng: random.Random, pieces: list, length: int = 3) -> bool:
    """`pieces` (block ids, or callables taking the wall's facing) in a row
    along one wall — a kitchen, a workbench, a counter."""
    c = room.c
    for (x, z, w) in room.wallside(rng):
        lx, lz = STEP[LEFT[w]]
        run = [(x + lx * i, z + lz * i) for i in range(length)]
        if not all(room.open(px, pz) and room.wall_dir(px, pz) == w for (px, pz) in run):
            continue
        for (px, pz), piece in zip(run, pieces):
            bid, nbt = piece(OPP[w]) if callable(piece) else (piece, None)
            room.put(px, pz, bid, nbt=nbt)
        return True
    return False


def _kit_table(room: _Room, k: Kit, rng: random.Random, laid: bool = True) -> bool:
    """A table of two top slabs in the open floor, a chair at each end and one
    each side, laid with candles and a pot."""
    c = room.c
    for (x, z) in room.centre_cells(rng):
        along = rng.choice(["x", "z"])
        ox, oz = (1, 0) if along == "x" else (0, 1)
        if not room.open(x + ox, z + oz):
            continue
        room.put(x, z, k.ww_slab_top)
        room.put(x + ox, z + oz, k.ww_slab_top)
        if laid:
            room.put(x, z, c.b("white_candle", candles=str(rng.randint(1, 4)), lit="false",
                               waterlogged="false"), dy=1)
            if rng.random() < 0.6:
                if rng.random() < 0.5:
                    room.put(x + ox, z + oz, c.b("flower_pot"), dy=1)
                else:
                    room.put(x + ox, z + oz, c.b("decorated_pot", facing="north", cracked="false",
                                                 waterlogged="false"), dy=1, nbt=_block_nbt("decorated_pot"))
        # Chairs: stairs facing the table (a stair's back is its `facing`).
        ends = [((x - ox, z - oz), "east" if along == "x" else "south"),
                ((x + 2 * ox, z + 2 * oz), "west" if along == "x" else "north")]
        sides = [((x - oz, z - ox), "south" if along == "x" else "east"),
                 ((x + oz + ox, z + ox + oz), "north" if along == "x" else "west")]
        for (cx_, cz_), f in ends + sides:
            if room.open(cx_, cz_) and rng.random() < 0.75:
                room.put(cx_, cz_, k.stairs("ww", OPP[f]))
        return True
    return False


def _kit_lounge(room: _Room, k: Kit, rng: random.Random) -> bool:
    """A settle along a wall — two whisperwood stairs side by side, backs to
    the wall — and a low table (a bottom slab) before it with a pot or a
    candle on it."""
    c = room.c
    for (x, z, w) in room.wallside(rng):
        lx, lz = STEP[LEFT[w]]
        seats = [(x, z), (x + lx, z + lz)]
        dx, dz = STEP[w]
        tx, tz = x - dx, z - dz
        if not all(room.open(px, pz) and room.wall_dir(px, pz) == w for (px, pz) in seats):
            continue
        if not room.open(tx, tz):
            continue
        for (px, pz) in seats:
            room.put(px, pz, k.stairs("ww", w))
        room.put(tx, tz, k.slab("ww", "bottom"))
        return True
    return False


def _kit_shelves(room: _Room, k: Kit, rng: random.Random, n: int, tall: bool = True,
                 kinds=None) -> None:
    kinds = kinds or [k.bookshelf]

    def shelf(x: int, z: int, b: int, two: bool) -> bool:
        room.put(x, z, b)
        if two:
            room.put(x, z, b, dy=1)
        return True

    for (x, z, _w) in room.wallside(rng)[:n]:
        b = rng.choice(kinds)
        room.attempt(lambda x=x, z=z, b=b, two=tall and rng.random() < 0.7: shelf(x, z, b, two))


def _kit_loot(room: _Room, rng: random.Random, loot: str | None, book: str | None) -> None:
    c = room.c
    ws = room.wallside(rng)

    def one(x: int, z: int, bid: int, nbt) -> bool:
        room.put(x, z, bid, nbt=nbt)
        return True

    while loot and ws:
        x, z, w = ws.pop()
        if rng.random() < 0.7:
            item = (c.b("chest", facing=OPP[w], type="single", waterlogged="false"), loot_nbt(loot))
        else:
            item = (c.b("barrel", facing="up", open="false"), loot_nbt(loot, "minecraft:barrel"))
        if room.attempt(lambda: one(x, z, *item)):
            break
    while book and ws:
        x, z, w = ws.pop()
        item = (c.b("lectern", facing=OPP[w], has_book="true", powered="false"), lectern_nbt(book))
        if room.attempt(lambda: one(x, z, *item)):
            break


def _kit_rugs(room: _Room, rng: random.Random, share: float = 0.25) -> None:
    c = room.c
    rug = c.b(rng.choice(["cyan_carpet", "purple_carpet", "light_blue_carpet"]))
    for (x, z) in room.centre_cells(rng)[: int(len(room.cells) * share)]:
        room.put(x, z, rug)


def _kitchen(k: Kit, c: City, rng: random.Random) -> list:
    smoker = lambda f: (c.b("smoker", facing=f, lit="false"), _block_nbt("smoker"))   # noqa: E731
    counter = rng.choice([k.slab("polished", "top"), k.ww_slab_top])
    return rng.choice([[smoker, counter, c.b("cauldron")],
                       [c.b("barrel", facing="up", open="false"), smoker, counter],
                       [counter, c.b("cauldron"), smoker]])


def furnish_kit(c: City, k: Kit, cells, y: int, rng: random.Random, kind: str,
                loot: str | None, book: str | None) -> None:
    """Lay out one floor as `kind` (see the section comment)."""
    room = _Room(c, cells, y)
    if not room.cells:
        return
    big = len(room.cells) >= 40
    # A mid-rise floor is one open plan; a big one is laid out as several
    # households / benches / desks, one set per ~45 cells.
    sets = max(1, min(4, len(room.cells) // 45))
    if kind == "home":
        for n in range(sets):
            room.attempt(lambda: _kit_bed(room, k, rng))
            if (big and rng.random() < 0.6) or n > 0:
                room.attempt(lambda: _kit_bed(room, k, rng))
            room.attempt(lambda: _kit_run(room, rng, _kitchen(k, c, rng)))
            room.attempt(lambda: _kit_table(room, k, rng))
            if n % 2 == 1 or rng.random() < 0.4:
                room.attempt(lambda: _kit_lounge(room, k, rng))
        _kit_shelves(room, k, rng, 2 + 2 * sets + rng.randint(0, 2))
        _kit_loot(room, rng, loot, book)
        _kit_rugs(room, rng, 0.06)
    elif kind == "stillhouse":
        # The hearth: an amber panel set in a polished surround on a wall.
        for (x, z, w) in room.wallside(rng)[:3]:
            if room.attempt(lambda x=x, z=z: (room.put(x, z, k.polished), room.put(x, z, k.amber, dy=1))):
                break
        room.attempt(lambda: _kit_run(room, rng, _kitchen(k, c, rng)))
        room.attempt(lambda: _kit_table(room, k, rng))
        _kit_shelves(room, k, rng, 2, tall=False, kinds=[k.bookshelf, c.b("barrel", facing="up", open="false")])
        _kit_loot(room, rng, loot, book)
    elif kind == "bedroom":
        _kit_shelves(room, k, rng, 2, kinds=[c.b("barrel", facing="up", open="false"), k.bookshelf])
        _kit_loot(room, rng, loot or ("aurelith_stillhouse" if rng.random() < 0.4 else None), book)
        _kit_rugs(room, rng, 0.3)
    elif kind in ("workshop", "tuners"):
        bench = [c.b("crafting_table"), c.b("smithing_table"), c.b("fletching_table")]
        tools = [lambda f: (c.b("grindstone", face="floor", facing=f), None),
                 lambda f: (c.b("stonecutter", facing=f), None), c.b("barrel", facing="up", open="false")]
        for _ in range(sets):
            rng.shuffle(bench)
            room.attempt(lambda: _kit_run(room, rng, bench))
            room.attempt(lambda: _kit_run(room, rng, tools))
        # Stock racks: a shelf of top slabs with crystal and conduit on it.
        for _ in range(sets + (1 if kind == "tuners" else 0)):
            for (x, z, w) in room.wallside(rng)[:3]:
                stock = rng.choice([k.cluster_up, k.crystal, k.conduit_x if w in ("north", "south") else k.conduit_z,
                                    c.b("amethyst_cluster", facing="up", waterlogged="false")])
                room.attempt(lambda x=x, z=z, stock=stock: (room.put(x, z, k.slab("polished", "top")),
                                                            room.put(x, z, stock, dy=1)))
        if kind == "tuners":
            # The test bench: note blocks and a chime to tune against.
            room.attempt(lambda: _kit_run(room, rng, [k.note, k.slab("polished", "top"), k.note]))
            for (x, z) in room.centre_cells(rng)[:2]:
                room.attempt(lambda x=x, z=z: (room.put(x, z, k.chiseled), room.put(x, z, k.chime, dy=1)))
        room.attempt(lambda: _kit_table(room, k, rng, laid=False))
        _kit_loot(room, rng, loot, book)
    elif kind == "study":
        for _ in range(2 * sets if big else 1):
            for (x, z, w) in room.wallside(rng)[:1]:
                def desk(x=x, z=z, w=w) -> bool:
                    room.put(x, z, k.ww_slab_top)
                    room.put(x, z, c.b("white_candle", candles=str(rng.randint(1, 3)), lit="false",
                                       waterlogged="false"), dy=1)
                    cx_, cz_ = x - STEP[w][0], z - STEP[w][1]
                    if room.open(cx_, cz_):
                        room.put(cx_, cz_, k.stairs("ww", OPP[w]))
                    return True
                room.attempt(desk)
        _kit_shelves(room, k, rng, 3 + 3 * sets + rng.randint(0, 3))
        if rng.random() < 0.5:
            room.attempt(lambda: _kit_run(room, rng, [c.b("cartography_table"), k.ww_slab_top], length=2))
        if sets > 1:
            room.attempt(lambda: _kit_lounge(room, k, rng))
        _kit_loot(room, rng, loot, book or rng.choice([None, "four_voices", "legend"]))
        _kit_rugs(room, rng, 0.1)
    elif kind == "shop":
        goods = [k.lamp, c.b("flower_pot"), k.cluster_up, k.echo_lantern_stand,
                 c.b("white_candle", candles="3", lit="false", waterlogged="false")]
        for (x, z, w) in room.wallside(rng)[:4 + 3 * sets]:
            top = k.slab("polished", "top") if rng.random() < 0.5 else k.ww_slab_top
            good = rng.choice(goods) if rng.random() < 0.7 else None
            room.attempt(lambda x=x, z=z, top=top, good=good: (
                room.put(x, z, top), good is not None and room.put(x, z, good, dy=1)))
        _kit_shelves(room, k, rng, 2 + 2 * sets, kinds=[k.bookshelf, c.b("barrel", facing="up", open="false")])
        _kit_loot(room, rng, loot or "aurelith_arcade", book)
    elif kind == "guard":
        room.attempt(lambda: _kit_bed(room, k, rng, "blue_bed"))
        room.attempt(lambda: _kit_bed(room, k, rng, "blue_bed"))
        room.attempt(lambda: _kit_table(room, k, rng, laid=False))
        _kit_shelves(room, k, rng, 2, tall=False, kinds=[c.b("barrel", facing="up", open="false")])
        _kit_loot(room, rng, loot, book)
    # Whatever the kit, a little of the city's everyday clutter against the
    # walls — guarded like everything else, so it never closes a way through.
    clutter = [c.b("flower_pot"), k.note, k.slab("ww", "bottom"),
               c.b("white_candle", candles="1", lit="false", waterlogged="false")]
    for (x, z, _w) in room.wallside(rng)[: max(1, len(room.cells) // 25)]:
        room.attempt(lambda x=x, z=z, b=rng.choice(clutter): room.put(x, z, b) or True)


def interior_kind(district: str, floor: int, floors: int, rng: random.Random,
                  shopfront: bool) -> str:
    """What a mid-rise floor is used as: shops and workshops at street level
    on the busy streets, homes above, a study or a workshop now and then,
    and the Listeners' and the conductors' quarters bookish."""
    if floor == 0:
        if shopfront or district == "arcade":
            return "shop"
        return rng.choice(["workshop", "home", "shop", "study"] if district != "stillhouses"
                          else ["home", "home", "workshop"])
    if district in ("listeners", "conductors") and rng.random() < 0.35:
        return "study"
    if rng.random() < 0.12:
        return "workshop"
    return "home"


def archive_reading_rooms(c: City, k: Kit) -> None:
    """The Archive's reading rooms: on every gallery, reading desks set
    against the shelves (a top slab, a chair, a candle, now and then an open
    book on a lectern-desk or a stack of paper-white carpet), clear of the
    ramp's landing, the doors, the sky-bridge door and the sealed reading
    cell on the third gallery's north side."""
    ax, az = ARCHIVE
    rng = piece_rng("aurelith/archive/reading")
    top = S + 63
    floors = list(range(S, top - 6, 7))
    for n, fy in enumerate(floors):
        y = fy + 1
        for i in range(5):
            a = 55.0 + i * 58.0 + rng.uniform(-9, 9) + n * 13.0
            a %= 360.0
            # The ramp meets each gallery at bearing ~0 (east); the doors are
            # east and south; the sealed cell is north on the third gallery.
            if a < 45 or a > 330 or 75 < a < 105 or (n == 3 and 245 < a < 295):
                continue
            th = math.radians(a)
            dx, dz = math.cos(th), math.sin(th)
            x, z = int(round(ax + dx * 9.0)), int(round(az + dz * 9.0))
            cx_, cz_ = int(round(ax + dx * 8.0)), int(round(az + dz * 8.0))
            if (x, z) in c.keep_clear or c.get(x, y, z) != AIR or c.get(x, y + 1, z) != AIR:
                continue
            if c.get(cx_, y, cz_) != AIR:
                continue
            c.set(x, y, z, k.ww_slab_top)
            c.set(x, y + 1, z, rng.choice([c.b("white_candle", candles=str(rng.randint(1, 3)), lit="false",
                                                waterlogged="false"), c.b("white_carpet"), c.b("flower_pot")]))
            # The chair faces the desk (its back to the atrium).
            f = ("east" if dx > 0.7 else "west" if dx < -0.7 else "south" if dz > 0 else "north")
            c.set(cx_, y, cz_, k.stairs("ww", OPP[f]))


# The Arcade's stalls: each a theme, laid on the counter and the crate.
STALL_THEMES = ("lamps", "pottery", "crystal", "candles", "cloth", "books", "glass", "food")


def stall_goods(c: City, k: Kit, stall_no: int, sx: int, zc: int) -> None:
    """Goods on stall `stall_no`'s counter (sx) and crate (sx + 1), by the
    stall's theme; the counter and crate themselves are build_arcade's."""
    theme = STALL_THEMES[stall_no % len(STALL_THEMES)]
    y = S + 2
    pot = c.b("decorated_pot", facing="south", cracked="false", waterlogged="false")
    if theme == "lamps":
        on_counter, on_crate = k.lamp, k.echo_lantern_stand
    elif theme == "pottery":
        on_counter, on_crate = pot, c.b("flower_pot")
    elif theme == "crystal":
        on_counter, on_crate = k.cluster_up, c.b("amethyst_cluster", facing="up", waterlogged="false")
    elif theme == "candles":
        on_counter = c.b("white_candle", candles="4", lit="false", waterlogged="false")
        on_crate = c.b("white_candle", candles="2", lit="false", waterlogged="false")
    elif theme == "cloth":
        on_counter, on_crate = c.b("cyan_carpet"), c.b("purple_carpet")
    elif theme == "books":
        on_counter, on_crate = c.b("white_candle", candles="1", lit="false", waterlogged="false"), c.b("flower_pot")
        c.set(sx, S + 1, zc, k.bookshelf)                 # a shelf stall instead of a counter
    elif theme == "glass":
        on_counter, on_crate = k.glass, k.echo_lantern_stand
    else:
        on_counter, on_crate = c.b("melon"), c.b("flower_pot")
    c.set(sx, y, zc, on_counter, nbt=_block_nbt("decorated_pot") if on_counter == pot else None)
    c.set(sx + 1, y, zc, on_crate)


# ── inhabitants and spawn overrides ─────────────────────────────────────────
# The city is quiet — the lanterns still burn and nothing hostile walks its
# streets — with two exceptions: the Archive of Echoes, where echo mimics
# haunt the dark galleries (placed with the city, and spawning there through
# a spawn-override district), and the crystal golems standing watch inside
# the Four Gates. A herd of hushlings grazes the Quiet Gardens. Every mob is
# a template entity (the engine's TemplateEngine / worldgen entity path),
# placed once with the city and finalizeSpawn(STRUCTURE)'d as a pool piece's
# are, and kept (PersistenceRequired). docs/the-hush.md "Aurelith".
def _stand_spot(c: City, x: int, z: int, y: int, height: int, radius: int = 4):
    """The nearest column to (x, z) — rings outward to `radius` — where the
    block under `y` is solid ground and `height` cells from `y` up are air.
    None when the neighbourhood has no such spot."""
    for r_ in range(radius + 1):
        for dx in range(-r_, r_ + 1):
            for dz in range(-r_, r_ + 1):
                if max(abs(dx), abs(dz)) != r_:
                    continue
                xx, zz = x + dx, z + dz
                if not c.is_solid(xx, y - 1, zz):
                    continue
                if all(c.get(xx, yy, zz) == AIR for yy in range(y, y + height)):
                    return xx, zz
    return None


def _yaw_toward(x: float, z: float, tx: float, tz: float) -> float:
    """MC yaw (0 = south, 90 = west) looking from (x, z) toward (tx, tz)."""
    return math.degrees(math.atan2(-(tx - x), tz - z)) % 360.0


def _add_mob(c: City, x: int, y: int, z: int, entity_id: str, yaw: float) -> None:
    c.entities.append((x + 0.5, float(y), z + 0.5, HS.entity_nbt(entity_id, yaw)))


def place_inhabitants(c: City, k: Kit) -> None:
    placed = []
    # The gate wardens: a crystal golem just inside each gate, beside the
    # avenue, facing out through the arch. Crystal golems keep a home of 16
    # round where they were placed (finalizeSpawn) and are neutral until
    # struck — or until someone breaks resonant crystal near them.
    for direction in GATES:
        to = gate_frame(direction)
        x, z = to(AVENUE_HALF - 1, WALL_IN - 9)
        spot = _stand_spot(c, x, z, S + 1, 3)
        if spot is None:
            continue
        ox, oz = to(AVENUE_HALF - 1, WALL_IN + 10)
        _add_mob(c, spot[0], S + 1, spot[1], "minecraft:crystal_golem",
                 _yaw_toward(spot[0], spot[1], ox, oz))
        placed.append(f"golem@{direction}")
    # The Archive's echoes: a mimic on the second and the fourth gallery,
    # between the shelves and the rail, clear of the lamps round the atrium
    # (light is the mimic's weakness). They walk where you walked.
    ax, az = ARCHIVE
    floors = list(range(S, S + 63 - 6, 7))
    for n, angle in ((2, 250.0), (4, 70.0)):
        fy = floors[n]
        th = math.radians(angle)
        x = int(round(ax + math.cos(th) * 9.5))
        z = int(round(az + math.sin(th) * 9.5))
        spot = _stand_spot(c, x, z, fy + 1, 2, radius=2)
        if spot is None:
            continue
        _add_mob(c, spot[0], fy + 1, spot[1], "minecraft:echo_mimic",
                 _yaw_toward(spot[0], spot[1], ax, az))
        placed.append(f"mimic@gallery{n}")
    # The Quiet Gardens: four hushlings on the lowest terrace.
    for (x, z, yaw) in ((32, 56, 30.0), (34, 59, 200.0), (31, 62, 110.0), (35, 64, 300.0)):
        spot = _stand_spot(c, x, z, S + 1, 1, radius=3)
        if spot is None:
            continue
        _add_mob(c, spot[0], S + 1, spot[1], "minecraft:hushling", yaw)
        placed.append("hushling")
    print(f"inhabitants: {len(c.entities)} ({', '.join(placed)})")


def aurelith_spawn_overrides(pieces: list[PieceOut]) -> dict:
    """spawn_overrides for aurelith.json: no natural monsters anywhere in the
    city (its whole box — MC's `full`), except the Archive of Echoes' interior
    (the atrium and the galleries, S + 1 up to its crown), where echo mimics
    spawn in the dark. The Archive straddles four 32 x 32 columns, so the
    district is given as boxes in those pieces' own coordinates (the engine's
    "obeycraft:districts" extension, StructureSpawnOverrides.hpp): the disc
    of radius ARCHIVE_R - 2 as runs of rows with the same x span, cut per
    piece."""
    ax, az = ARCHIVE
    inner = ARCHIVE_R - 2.0
    top = S + 63
    areas = []
    for p in pieces:
        if not p.name.startswith("u_"):
            continue
        x0, y0, z0, x1, y1, z1 = p.box
        rows = []
        for z in range(max(z0, az - int(inner)), min(z1, az + int(inner)) + 1):
            half = math.sqrt(max(0.0, inner * inner - (z - az) ** 2))
            xa = max(x0, math.ceil(ax - half))
            xb = min(x1, math.floor(ax + half))
            if xa <= xb:
                rows.append((z, xa, xb))
        runs = []
        for (z, xa, xb) in rows:
            if runs and runs[-1][1] == z - 1 and runs[-1][2] == xa and runs[-1][3] == xb:
                runs[-1][1] = z
            else:
                runs.append([z, z, xa, xb])
        for (za, zb, xa, xb) in runs:
            areas.append({"template": p.location,
                          "box": [xa - x0, (S + 1) - y0, za - z0, xb - x0, min(top, y1) - y0, zb - z0]})
    return {
        "monster": {
            "bounding_box": "full",
            "spawns": [],
            "obeycraft:districts": [{
                "name": "archive_of_echoes",
                "areas": areas,
                "spawns": [{"type": "minecraft:echo_mimic", "maxCount": 2, "minCount": 1, "weight": 1}],
            }],
        },
    }


if __name__ == "__main__":
    sys.exit(main())
