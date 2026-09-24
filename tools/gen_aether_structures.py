#!/usr/bin/env python3
"""Generate the Aether dungeon structure templates (ORIGINAL designs).

The Aether's own templates (src/main/resources/data/aether/structure/**) are
All Rights Reserved and may not be shipped, so this script builds our own at
the same template ids, with the sizes, anchor rows and DATA markers the ported
piece code (src/my_terrain_library/src/levelgen/structure/AetherStructures.cpp)
relies on. Everything else — layout, materials, decoration — is designed here.

    python3 tools/gen_aether_structures.py                 # write data/aether/structure/**
    python3 tools/gen_aether_structures.py --dump          # ... and print a summary of each
    python3 tools/gen_aether_structures.py --check-blocks  # ... and cross-check every palette
                                                           #     block against Blocks.cpp

Output is deterministic (per-template RNG seeded by crc32 of the template id,
gzip mtime 0).

What the piece code needs from each template (sizes are x*y*z):

  bronze_dungeon/
    boss_room      16x14x16  entry opening on the local south face (z 15) at
                             x 6..9, y 3..6 (where the square tunnel lands);
                             "Treasure Chest" marker one above the reward chest;
                             the Slider would stand at local (8, 3, 8)
    chest_room     12x8x12   floor y 0, interior from y 1 (tunnels cut the
                             walls); "Chest" marker
    lobby          12x12x12  same floor convention
    square_tunnel  6x6x6     air core x 1..4, y 1..4, open at both ends
    entrance       6x8x1     doorway x 1..4, y 1..4
    end_corridor   6x8x5     air core x 1..4, y 1..4
  silver_dungeon/
    rear           32x30x31  temple back, encloses the boss room
    boss_room      22x17x26  floor top y 1; door cut at local z 25, y 2..3;
                             "Treasure Chest" marker; Valkyrie Queen at (11, 2, 12)
    skeleton       32x27x26  the shell around the 3x3x3 room grid (grid at
                             x 5..26, y 4..19, z -1..20 of this template)
    floor 6x1x6, wall 6x4x1, door 6x4x1, boss_door 2x2x1,
    staircase 4x9x4, tall_staircase 4x14x4, chest_room 2x2x2 ("Chest" marker)
  gold_dungeon/
    island         38x43x38  grass top about y 34 (golden oaks look in the top
                             quarter of the box), solid round the centre where
                             the boss room is set
    stub           15x15x15  small islet, grass top y 11
    tunnel         7x5x16    air core x 1..5, y 1..3
    boss_room      23x9x28   door on local south face (z 27) x 9..13, y 2..4;
                             "Treasure Chest" marker; Sun Spirit at (11, 1, 14)

Blocks are the engine's Aether slugs in the minecraft namespace; chests are
minecraft:chest carrying the mod's loot-table ids (the piece code re-seeds the
reward chests from their markers).
"""

from __future__ import annotations

import argparse
import gzip
import io
import math
import random
import re
import struct
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BLOCKS_CPP = REPO_ROOT / "src" / "my_terrain_library" / "src" / "world" / "level" / "block" / "Blocks.cpp"
DATA_VERSION = 4764

BRONZE_LOOT = "aether:chests/dungeon/bronze/bronze_dungeon"
BRONZE_REWARD = "aether:chests/dungeon/bronze/bronze_dungeon_reward"
SILVER_REWARD = "aether:chests/dungeon/silver/silver_dungeon_reward"
GOLD_REWARD = "aether:chests/dungeon/gold/gold_dungeon_reward"


# ── NBT writer/reader (the subset templates use; same encoding as
#    tools/gen_hush_structures.py) ─────────────────────────────────────────────
class Tag:
    __slots__ = ("t", "v")

    def __init__(self, t: int, v):
        self.t = t
        self.v = v


def wname(s: str) -> bytes:
    e = s.encode("utf8")
    return struct.pack(">H", len(e)) + e


def wpayload(tag: Tag) -> bytes:
    t, v = tag.t, tag.v
    if t == 1: return struct.pack(">b", v)
    if t == 3: return struct.pack(">i", v)
    if t == 8: return wname(v)
    if t == 9:
        et, items = v
        return struct.pack(">bi", et, len(items)) + b"".join(wpayload(Tag(et, i)) for i in items)
    if t == 10:
        out = b""
        for k, tg in v.items():
            out += struct.pack(">b", tg.t) + wname(k) + wpayload(tg)
        return out + b"\x00"
    raise ValueError(t)


def serialize(name: str, tag: Tag) -> bytes:
    return struct.pack(">b", tag.t) + wname(name) + wpayload(tag)


def parse(b: bytes):
    pos = [0]

    def u(fmt):
        v = struct.unpack_from(">" + fmt, b, pos[0])[0]
        pos[0] += struct.calcsize(fmt)
        return v

    def s():
        n = u("H")
        v = b[pos[0]:pos[0] + n].decode("utf8", "replace")
        pos[0] += n
        return v

    def payload(t):
        if t == 1: return u("b")
        if t == 2: return u("h")
        if t == 3: return u("i")
        if t == 4: return u("q")
        if t == 5: return u("f")
        if t == 6: return u("d")
        if t == 8: return s()
        if t == 9:
            et = u("b"); n = u("i")
            return [payload(et) for _ in range(n)]
        if t == 10:
            d = {}
            while True:
                tt = u("b")
                if tt == 0: return d
                k = s(); d[k] = payload(tt)
        raise ValueError(t)

    t = u("b"); s()
    return payload(t)


def T_str(s: str) -> Tag: return Tag(8, s)
def T_int(i: int) -> Tag: return Tag(3, i)
def T_comp(d: dict) -> Tag: return Tag(10, d)
def T_list(et: int, items: list) -> Tag: return Tag(9, (et, items))


def gzip_bytes(raw: bytes) -> bytes:
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as z:
        z.write(raw)
    return buf.getvalue()


# ── block vocabulary: key -> (engine id, default props) ─────────────────────
BLOCKS = {
    "air": ("minecraft:air", None),
    "holystone": ("minecraft:holystone", None),
    "mossy": ("minecraft:mossy_holystone", None),
    "hbricks": ("minecraft:holystone_bricks", None),
    "dirt": ("minecraft:aether_dirt", None),
    "grass": ("minecraft:aether_grass_block", {"snowy": "false"}),
    "carved": ("minecraft:carved_stone", None),
    "sentry": ("minecraft:sentry_stone", None),
    "locked_carved": ("minecraft:locked_carved_stone", None),
    "locked_sentry": ("minecraft:locked_sentry_stone", None),
    "angelic": ("minecraft:angelic_stone", None),
    "light_angelic": ("minecraft:light_angelic_stone", None),
    "locked_angelic": ("minecraft:locked_angelic_stone", None),
    "locked_light_angelic": ("minecraft:locked_light_angelic_stone", None),
    "hellfire": ("minecraft:hellfire_stone", None),
    "light_hellfire": ("minecraft:light_hellfire_stone", None),
    "locked_hellfire": ("minecraft:locked_hellfire_stone", None),
    "locked_light_hellfire": ("minecraft:locked_light_hellfire_stone", None),
    "pillar": ("minecraft:pillar", {"axis": "y"}),
    "pillar_top": ("minecraft:pillar_top", {"facing": "up"}),
    "planks": ("minecraft:skyroot_planks", None),
    "torch": ("minecraft:ambrosium_torch", None),
    "chest": ("minecraft:chest", {"facing": "north", "type": "single", "waterlogged": "false"}),
    "marker": ("minecraft:structure_block", {"mode": "data"}),
}


class Piece:
    def __init__(self, folder: str, name: str, sx: int, sy: int, sz: int, fill: str = "air"):
        self.folder = folder
        self.name = name
        self.size = (sx, sy, sz)
        self.cells: dict[tuple[int, int, int], tuple[str, dict | None, Tag | None]] = {}
        self.rng = random.Random(zlib.crc32(f"aether:{folder}/{name}".encode("utf8")))
        self.fill(0, 0, 0, sx - 1, sy - 1, sz - 1, fill)

    def inside(self, x, y, z) -> bool:
        sx, sy, sz = self.size
        return 0 <= x < sx and 0 <= y < sy and 0 <= z < sz

    def set(self, x, y, z, key: str, props: dict | None = None, nbt: Tag | None = None) -> None:
        assert self.inside(x, y, z), (self.name, x, y, z)
        if props is None:
            props = BLOCKS[key][1]
        self.cells[(x, y, z)] = (key, props, nbt)

    def get(self, x, y, z) -> str:
        return self.cells[(x, y, z)][0]

    def fill(self, x0, y0, z0, x1, y1, z1, key: str, props: dict | None = None) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            for y in range(min(y0, y1), max(y0, y1) + 1):
                for z in range(min(z0, z1), max(z0, z1) + 1):
                    self.set(x, y, z, key, props)

    def hollow(self, x0, y0, z0, x1, y1, z1, wall: str) -> None:
        """A box of `wall` with an air interior."""
        self.fill(x0, y0, z0, x1, y1, z1, wall)
        self.fill(x0 + 1, y0 + 1, z0 + 1, x1 - 1, y1 - 1, z1 - 1, "air")

    def speckle(self, keys: dict[str, list[tuple[str, float]]]) -> None:
        """Replace a fraction of each listed block with variants (deterministic)."""
        for pos in sorted(self.cells, key=lambda p: (p[1], p[2], p[0])):
            key = self.cells[pos][0]
            if key in keys:
                roll = self.rng.random()
                for variant, chance in keys[key]:
                    if roll < chance:
                        self.set(*pos, variant)
                        break
                    roll -= chance

    def marker(self, x, y, z, metadata: str) -> None:
        self.set(x, y, z, "marker", nbt=T_comp({
            "mode": T_str("DATA"),
            "metadata": T_str(metadata),
            "name": T_str(""),
            "id": T_str("minecraft:structure_block"),
        }))

    def chest(self, x, y, z, facing: str, loot: str) -> None:
        self.set(x, y, z, "chest", {"facing": facing, "type": "single", "waterlogged": "false"},
                 T_comp({"LootTable": T_str(loot), "id": T_str("minecraft:chest")}))

    def to_nbt(self) -> bytes:
        sx, sy, sz = self.size
        assert len(self.cells) == sx * sy * sz
        palette: list[tuple[str, tuple]] = []
        index: dict[tuple[str, tuple], int] = {}
        blocks = []
        for (x, y, z) in sorted(self.cells, key=lambda p: (p[1], p[2], p[0])):
            key, props, nbt = self.cells[(x, y, z)]
            bid = BLOCKS[key][0]
            k = (bid, tuple(sorted((props or {}).items())))
            if k not in index:
                index[k] = len(palette)
                palette.append(k)
            entry = {"pos": T_list(3, [x, y, z]), "state": T_int(index[k])}
            if nbt is not None:
                entry["nbt"] = nbt
            blocks.append(entry)
        palette_tags = []
        for bid, props in palette:
            e = {}
            if props:
                e["Properties"] = T_comp({pk: T_str(pv) for pk, pv in props})
            e["Name"] = T_str(bid)
            palette_tags.append(e)
        root = T_comp({
            "size": T_list(3, [sx, sy, sz]),
            "entities": T_list(0, []),
            "blocks": T_list(10, blocks),
            "palette": T_list(10, palette_tags),
            "DataVersion": T_int(DATA_VERSION),
        })
        return gzip_bytes(serialize("", root))


# ── bronze dungeon: carved stone halls in holystone ─────────────────────────
def bronze_boss_room() -> Piece:
    p = Piece("bronze_dungeon", "boss_room", 16, 14, 16)
    # Thick locked shell; the arena floor tops out at y 2.
    p.hollow(0, 0, 0, 15, 13, 15, "locked_carved")
    p.fill(1, 1, 1, 14, 2, 14, "locked_carved")
    # A sunken ring round a raised centre dais, lit by sentry studs.
    p.fill(3, 2, 3, 12, 2, 12, "air")
    p.fill(5, 2, 5, 10, 2, 10, "locked_carved")
    for x, z in ((1, 1), (1, 14), (14, 1), (14, 14)):
        p.fill(x, 3, z, x, 12, z, "locked_sentry")
    for i in range(2, 14, 3):
        p.set(i, 12, 0, "locked_sentry"); p.set(i, 12, 15, "locked_sentry")
        p.set(0, 12, i, "locked_sentry"); p.set(15, 12, i, "locked_sentry")
    # Entry opening where the square tunnel meets the south wall.
    p.fill(6, 3, 15, 9, 6, 15, "air")
    # Reward alcove in the north wall, chest one below its marker.
    p.fill(6, 3, 1, 9, 5, 1, "air")
    p.chest(7, 3, 1, "south", BRONZE_REWARD)
    p.marker(7, 4, 1, "Treasure Chest")
    return p


def bronze_chest_room() -> Piece:
    p = Piece("bronze_dungeon", "chest_room", 12, 8, 12)
    p.hollow(0, 0, 0, 11, 7, 11, "carved")
    # Buttresses in the corners and a central plinth for the loot.
    for x, z in ((1, 1), (1, 10), (10, 1), (10, 10)):
        p.fill(x, 1, z, x, 6, z, "sentry")
    p.fill(5, 1, 5, 6, 1, 6, "carved")
    p.marker(5, 2, 5, "Chest")
    p.set(1, 6, 5, "torch"); p.set(10, 6, 6, "torch")
    return p


def bronze_lobby() -> Piece:
    p = Piece("bronze_dungeon", "lobby", 12, 12, 12)
    p.hollow(0, 0, 0, 11, 11, 11, "carved")
    # A gallery ring at y 5 with a well down its middle.
    p.fill(1, 5, 1, 10, 5, 10, "carved")
    p.fill(3, 5, 3, 8, 5, 8, "air")
    for x, z in ((3, 3), (3, 8), (8, 3), (8, 8)):
        p.fill(x, 1, z, x, 10, z, "sentry")
    # Steps up to the gallery along the west wall.
    for i in range(4):
        p.fill(1, 1, 2 + i, 2, 1 + i, 2 + i, "carved")
    p.fill(1, 5, 2, 2, 5, 5, "air")
    p.fill(1, 1, 6, 2, 4, 6, "carved")
    p.set(5, 10, 5, "torch")
    return p


def bronze_square_tunnel() -> Piece:
    p = Piece("bronze_dungeon", "square_tunnel", 6, 6, 6, "holystone")
    p.fill(1, 1, 0, 4, 4, 5, "air")
    p.speckle({"holystone": [("mossy", 0.12)]})
    return p


def bronze_entrance() -> Piece:
    p = Piece("bronze_dungeon", "entrance", 6, 8, 1, "hbricks")
    p.fill(1, 1, 0, 4, 4, 0, "air")
    p.set(0, 5, 0, "carved"); p.set(5, 5, 0, "carved")
    return p


def bronze_end_corridor() -> Piece:
    p = Piece("bronze_dungeon", "end_corridor", 6, 8, 5, "holystone")
    p.fill(1, 1, 0, 4, 4, 4, "air")
    p.fill(1, 0, 0, 4, 0, 4, "hbricks")
    p.speckle({"holystone": [("mossy", 0.18)]})
    return p


# ── silver dungeon: angelic stone temple ────────────────────────────────────
def silver_floor() -> Piece:
    p = Piece("silver_dungeon", "floor", 6, 1, 6, "locked_angelic")
    for x in range(6):
        for z in range(6):
            if (x + z) % 4 == 0:
                p.set(x, 0, z, "locked_light_angelic")
    return p


def silver_wall() -> Piece:
    p = Piece("silver_dungeon", "wall", 6, 4, 1, "locked_angelic")
    p.set(2, 2, 0, "locked_light_angelic"); p.set(3, 2, 0, "locked_light_angelic")
    return p


def silver_door() -> Piece:
    p = Piece("silver_dungeon", "door", 6, 4, 1, "locked_angelic")
    p.fill(2, 0, 0, 3, 2, 0, "air")
    p.set(1, 3, 0, "locked_light_angelic"); p.set(4, 3, 0, "locked_light_angelic")
    return p


def silver_boss_door() -> Piece:
    # The opening cut through the temple back and the boss room's south wall.
    return Piece("silver_dungeon", "boss_door", 2, 2, 1, "air")


def spiral(p: Piece, height: int) -> None:
    """A tight spiral round a 2x2 pillar core: one step per block of rise."""
    p.fill(1, 0, 1, 2, height - 1, 2, "pillar")
    ring = [(0, 0), (1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (3, 3), (2, 3), (1, 3), (0, 3), (0, 2), (0, 1)]
    for y in range(height - 1):
        x, z = ring[y % len(ring)]
        p.set(x, y, z, "angelic")
        x2, z2 = ring[(y + 1) % len(ring)]
        p.set(x2, y, z2, "angelic")
    p.set(1, height - 1, 1, "pillar_top"); p.set(2, height - 1, 2, "pillar_top")


def silver_staircase() -> Piece:
    p = Piece("silver_dungeon", "staircase", 4, 9, 4)
    spiral(p, 9)
    return p


def silver_tall_staircase() -> Piece:
    p = Piece("silver_dungeon", "tall_staircase", 4, 14, 4)
    spiral(p, 14)
    return p


def silver_chest_room() -> Piece:
    # The chest lands on a random spot of this 2x2 box at the marker's height.
    p = Piece("silver_dungeon", "chest_room", 2, 2, 2)
    p.marker(0, 0, 0, "Chest")
    return p


def silver_boss_room() -> Piece:
    p = Piece("silver_dungeon", "boss_room", 22, 17, 26)
    p.hollow(0, 0, 0, 21, 16, 25, "locked_angelic")
    p.fill(1, 1, 1, 20, 1, 24, "locked_angelic")
    # Colonnade down both long sides, light bands in the vault.
    for z in range(3, 24, 4):
        for x in (3, 18):
            p.fill(x, 2, z, x, 14, z, "pillar")
            p.set(x, 15, z, "pillar_top")
    for x in range(1, 21):
        if x % 3 == 0:
            p.fill(x, 16, 1, x, 16, 24, "locked_light_angelic")
    # A checker aisle down the middle.
    for z in range(2, 24):
        for x in range(9, 13):
            if (x + z) % 2 == 0:
                p.set(x, 1, z, "locked_light_angelic")
    # Reward dais at the north end.
    p.fill(8, 1, 1, 13, 2, 3, "locked_angelic")
    p.chest(11, 3, 2, "south", SILVER_REWARD)
    p.marker(11, 4, 2, "Treasure Chest")
    return p


def silver_rear() -> Piece:
    p = Piece("silver_dungeon", "rear", 32, 30, 31)
    # Foundation and a hollow sanctuary block; the boss room is set inside it
    # at (5, 3, 5).
    p.fill(0, 0, 0, 31, 2, 30, "holystone")
    p.hollow(2, 2, 2, 29, 21, 30, "angelic")
    for x in range(2, 30, 5):
        p.fill(x, 3, 1, x, 22, 1, "pillar")
        p.set(x, 23, 1, "pillar_top")
    for z in range(2, 31, 5):
        p.fill(1, 3, z, 1, 22, z, "pillar"); p.set(1, 23, z, "pillar_top")
        p.fill(30, 3, z, 30, 22, z, "pillar"); p.set(30, 23, z, "pillar_top")
    # A stepped roof.
    for step in range(6):
        p.fill(2 + step * 2, 22 + step, 2 + step, 29 - step * 2, 22 + step, 30, "angelic")
    p.fill(12, 28, 7, 19, 28, 30, "light_angelic")
    p.speckle({"holystone": [("mossy", 0.1)]})
    return p


def silver_skeleton() -> Piece:
    p = Piece("silver_dungeon", "skeleton", 32, 27, 26)
    # Plinth under the room grid (grid floors sit at y 4, 9, 14).
    p.fill(4, 0, 0, 27, 3, 21, "holystone")
    # Outer shell: east wall x 26, south wall z 20, roof y 19 (the grid's
    # north and west walls are its own wall pieces).
    p.fill(26, 4, 0, 27, 19, 20, "angelic")
    p.fill(4, 4, 20, 27, 19, 21, "angelic")
    p.fill(4, 4, 0, 4, 19, 21, "angelic")
    p.fill(4, 19, 0, 27, 19, 21, "angelic")
    # Ground-floor entrance through the south wall into the middle cell.
    p.fill(14, 5, 20, 17, 7, 21, "air")
    # Front porch: columns and steps down from the plinth.
    p.fill(4, 0, 22, 27, 3, 25, "holystone")
    for x in (5, 11, 20, 26):
        p.fill(x, 4, 24, x, 18, 24, "pillar")
        p.set(x, 19, 24, "pillar_top")
    p.fill(4, 20, 22, 27, 20, 25, "angelic")
    for i, z in enumerate(range(22, 26)):
        # Each step one lower than the last, open above.
        p.fill(13, 3 - i, z, 18, 3 - i, z, "hbricks")
        if i > 0:
            p.fill(13, 4 - i, z, 18, 3, z, "air")
    # Pitched roof over the hall.
    for step in range(7):
        y = 20 + step
        if y > 26:
            break
        p.fill(4 + step * 2, y, 0, 27 - step * 2, y, 21, "angelic")
    p.speckle({"holystone": [("mossy", 0.1)]})
    return p


# ── gold dungeon: a hellfire vault inside a floating island ─────────────────
def island_shape(p: Piece, cx: float, cz: float, top: int, radius: float) -> None:
    """An inverted, ragged cone: grass on top, three dirt, holystone below."""
    sx, sy, sz = p.size
    rng = p.rng
    lobes = [(rng.uniform(0, math.tau), rng.uniform(0.06, 0.14), rng.randint(2, 5)) for _ in range(3)]
    for x in range(sx):
        for z in range(sz):
            dx, dz = x + 0.5 - cx, z + 0.5 - cz
            ang = math.atan2(dz, dx)
            wobble = 1.0 + sum(a * math.sin(k * ang + ph) for ph, a, k in lobes)
            dist = math.hypot(dx, dz) / wobble
            if dist > radius:
                continue
            # Surface height sags a little towards the rim; the underside is
            # a deep bowl, full under the middle (where the gold dungeon's
            # boss room is set) and closing to a point at y 0.
            t = dist / radius
            surface = top - int(2.5 * t * t)
            bottom = max(0, int(top * (1.0 - math.sqrt(max(0.0, 1.0 - t * t)))))
            for y in range(bottom, surface + 1):
                if y == surface:
                    p.set(x, y, z, "grass")
                elif y >= surface - 3:
                    p.set(x, y, z, "dirt")
                else:
                    p.set(x, y, z, "holystone")


def gold_island() -> Piece:
    p = Piece("gold_dungeon", "island", 38, 43, 38)
    island_shape(p, 19.0, 19.0, top=34, radius=18.5)
    return p


def gold_stub() -> Piece:
    p = Piece("gold_dungeon", "stub", 15, 15, 15)
    island_shape(p, 7.5, 7.5, top=11, radius=7.0)
    return p


def gold_tunnel() -> Piece:
    p = Piece("gold_dungeon", "tunnel", 7, 5, 16, "holystone")
    p.fill(1, 1, 0, 5, 3, 15, "air")
    for z in range(2, 16, 4):
        p.set(1, 3, z, "torch")
    p.speckle({"holystone": [("mossy", 0.15)]})
    return p


def gold_boss_room() -> Piece:
    p = Piece("gold_dungeon", "boss_room", 23, 9, 28)
    p.hollow(0, 0, 0, 22, 8, 27, "locked_hellfire")
    # Glowing seams in the floor and ceiling.
    for x in range(1, 22):
        for z in range(1, 27):
            if x % 4 == 3 and z % 4 == 3:
                p.set(x, 0, z, "locked_light_hellfire")
                p.set(x, 8, z, "locked_light_hellfire")
    # Pillars round the arena.
    for x in (4, 18):
        for z in range(5, 24, 6):
            p.fill(x, 1, z, x, 7, z, "locked_hellfire")
    # Door where the tunnel meets the south face.
    p.fill(9, 2, 26, 13, 4, 27, "air")
    # Reward niche at the north end.
    p.fill(9, 1, 1, 13, 1, 2, "locked_light_hellfire")
    p.chest(11, 2, 1, "south", GOLD_REWARD)
    p.marker(11, 3, 1, "Treasure Chest")
    return p


BUILDERS = [
    bronze_boss_room, bronze_chest_room, bronze_lobby, bronze_square_tunnel,
    bronze_entrance, bronze_end_corridor,
    silver_floor, silver_wall, silver_door, silver_boss_door, silver_staircase,
    silver_tall_staircase, silver_chest_room, silver_boss_room, silver_rear, silver_skeleton,
    gold_island, gold_stub, gold_tunnel, gold_boss_room,
]


# ── dump / cross-check ───────────────────────────────────────────────────────
def dump(path: Path) -> None:
    d = parse(gzip.decompress(path.read_bytes()))
    palette = d["palette"]
    counts: dict[str, int] = {}
    for b in d["blocks"]:
        name = palette[b["state"]]["Name"]
        counts[name] = counts.get(name, 0) + 1
    print(f"{path.parent.name}/{path.name}: size {d['size']}, {len(d['blocks'])} blocks")
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"   {n:6d} {name}")
    for b in d["blocks"]:
        if "nbt" in b:
            nbt = b["nbt"]
            extra = nbt.get("metadata") or nbt.get("LootTable")
            print(f"   nbt @{tuple(b['pos'])} {palette[b['state']]['Name']}: {extra}")


REGISTER_RE = re.compile(r'(?:create\w*Block|registerBlock|setId)\(\s*"(minecraft:[a-z0-9_]+)"')
ID_RE = re.compile(r'"(minecraft:[a-z0-9_]+)"')


def registered_blocks() -> set[str]:
    text = BLOCKS_CPP.read_text(encoding="utf-8", errors="replace")
    known = set(REGISTER_RE.findall(text))
    for m in re.finditer(r':\s*\{(.*?)\}\s*\)\s*\{', text, re.S):
        known.update(ID_RE.findall(m.group(1)))
    return known


def check_blocks(paths: list[Path]) -> int:
    known = registered_blocks()
    missing = 0
    for path in paths:
        d = parse(gzip.decompress(path.read_bytes()))
        for entry in d["palette"]:
            if entry["Name"] not in known:
                print(f"   MISSING {entry['Name']} (used by {path.parent.name}/{path.name})")
                missing += 1
    print("block check: " + ("all palette blocks registered" if missing == 0 else f"{missing} unregistered"))
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", action="store_true", help="re-read the written templates and summarize them")
    ap.add_argument("--check-blocks", action="store_true",
                    help="cross-check every palette block against Blocks.cpp; exit 1 on a miss")
    ap.add_argument("--outdir", type=Path, default=REPO_ROOT / "data", help="data root (default: data/)")
    args = ap.parse_args()

    written: list[Path] = []
    for build in BUILDERS:
        piece = build()
        out = args.outdir / "aether" / "structure" / piece.folder / f"{piece.name}.nbt"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(piece.to_nbt())
        written.append(out)
        sx, sy, sz = piece.size
        print(f"wrote {out.relative_to(REPO_ROOT) if out.is_relative_to(REPO_ROOT) else out} ({sx}x{sy}x{sz})")
    if args.dump:
        print()
        for path in written:
            dump(path)
    if args.check_blocks:
        return 1 if check_blocks(written) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
