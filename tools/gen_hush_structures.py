#!/usr/bin/env python3
# tools/gen_hush_structures.py
#
# Emits the Hush's structure templates and template pools: the Echo Vaults
# jigsaw (data/minecraft/structure/echo_vaults/*.nbt + worldgen/template_pool/
# echo_vaults/*.json), the Whisper Shrine (structure/whisper_shrine/
# shrine.nbt + template_pool/whisper_shrine/start.json), the Choir Hall,
# the Hush Lighthouse, the Warden's Tomb (tomb + ladder shaft) and the Sunken
# Library (structure/<name>/*.nbt + template_pool/<name>/*.json). Pools come from here
# so piece names and weights cannot drift from the templates. The structure,
# structure_set, processor_list, biome tag and loot-table JSONs are hand-written
# next to them; this script owns only what has to agree with the NBT bytes.
#
#   python3 tools/gen_hush_structures.py                # write templates + pools
#   python3 tools/gen_hush_structures.py --dump         # ... and re-read them back
#   python3 tools/gen_hush_structures.py --check-blocks # ... and cross-check every
#       # palette block against the terrain library's registrations (Blocks.cpp)
#   python3 tools/gen_hush_structures.py --no-shaft     # centre without the sinkhole shaft
#   python3 tools/gen_hush_structures.py --vanilla-stand-ins
#       # vanilla block names in place of the Hush blocks, for testing the
#       # pipeline against a terrain library that has not registered them
#
# Loader contract (src/my_terrain_library/src/levelgen/structure/*):
#   * JigsawTemplates / TemplateEngine read size, palette, blocks{pos,state,nbt}
#     and entities{pos,blockPos,nbt} (placed as MC's StructureTemplate
#     .placeEntities does; pool pieces finalizeSpawn them); DataVersion is
#     ignored but written vanilla-shaped.
#   * Every palette Name must be a block the library registers, with Properties
#     the registered block has (unknown Name or property THROWS and kills all
#     structures - TemplateEngine::resolvePaletteEntry). structure_void is NOT
#     registered, so every piece is a full box: rooms are hollowed out of solid
#     masonry, never floated in void; the shrine carries its own ground layer.
#   * Jigsaw blocks: Properties.orientation gives front/top; nbt name/target/
#     pool/joint/final_state exactly like ancient_city/city_center_1.nbt.
#   * Chests: vanilla-style {LootTable, id} block entities (the same shape
#     ancient_city templates carry); the engine rolls the table on first open
#     (BaseContainerBlockEntity::UnpackLootTable). Pass --bake-items to write
#     fixed Items instead.
#   * Fences are real FenceBlocks (north/east/south/west/waterlogged); nothing
#     re-evaluates connections after placement, so they are baked here from
#     the piece's own neighbours (to_nbt).
#   * A jigsaw start piece projected to the heightmap lands with its y=0 ON the
#     surface's top block (JigsawLayout: bottomY - groundLevelDelta 1), so a
#     surface piece's bottom layer replaces the ground, not the air above it.
#
# Jigsaw convention: a horizontal link sits in the FLOOR row of the doorway
# cell on the piece's outer face, front pointing out, name == target ==
# minecraft:echo_vaults/link, joint aligned, final_state hushstone_bricks. The
# child piece's own link (same convention) lands one block outside, so floors
# line up and every doorway is a 3-wide by 3-tall opening.

from __future__ import annotations

import argparse
import gzip
import io
import json
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
PROCESSORS = "minecraft:echo_vaults_degradation"

LINK = "minecraft:echo_vaults/link"
SHAFT_LINK = "minecraft:echo_vaults/shaft_link"
POOL_CENTER = "minecraft:echo_vaults/center"
POOL_HALLS = "minecraft:echo_vaults/halls"
POOL_HALL_NEXT = "minecraft:echo_vaults/hall_next"
POOL_ENDS = "minecraft:echo_vaults/ends"
POOL_SHAFT = "minecraft:echo_vaults/shaft"

VAULT_LOOT = "minecraft:chests/echo_vaults"
SHRINE_LOOT = "minecraft:chests/whisper_shrine"
CHOIR_LOOT = "minecraft:chests/choir_hall"
LIGHTHOUSE_LOOT = "minecraft:chests/hush_lighthouse"
TOMB_LOOT = "minecraft:chests/wardens_tomb"
LIBRARY_LOOT = "minecraft:chests/sunken_library"
TOMB_SHAFT_LINK = "minecraft:wardens_tomb/shaft_link"
POOL_TOMB_SHAFT = "minecraft:wardens_tomb/shaft"


# ── NBT (the subset templates use), same encoding as redstone_computer/nbtread.py
class Tag:
    __slots__ = ("t", "v")

    def __init__(self, t: int, v):
        self.t = t
        self.v = v

    def __repr__(self) -> str:
        return f"Tag({self.t},{self.v!r})"


def wname(s: str) -> bytes:
    e = s.encode("utf8")
    return struct.pack(">H", len(e)) + e


def wpayload(tag: Tag) -> bytes:
    t, v = tag.t, tag.v
    if t == 1: return struct.pack(">b", v)
    if t == 2: return struct.pack(">h", v)
    if t == 3: return struct.pack(">i", v)
    if t == 4: return struct.pack(">q", v)
    if t == 5: return struct.pack(">f", v)
    if t == 6: return struct.pack(">d", v)
    if t == 7: return struct.pack(">i", len(v)) + struct.pack(">%db" % len(v), *v)
    if t == 8: return wname(v)
    if t == 9:
        et, items = v
        return struct.pack(">bi", et, len(items)) + b"".join(wpayload(Tag(et, i)) for i in items)
    if t == 10:
        out = b""
        for k, tg in v.items():
            out += struct.pack(">b", tg.t) + wname(k) + wpayload(tg)
        return out + b"\x00"
    if t == 11: return struct.pack(">i", len(v)) + struct.pack(">%di" % len(v), *v)
    if t == 12: return struct.pack(">i", len(v)) + struct.pack(">%dq" % len(v), *v)
    raise ValueError(t)


def serialize(name: str, tag: Tag) -> bytes:
    return struct.pack(">b", tag.t) + wname(name) + wpayload(tag)


def rd(b: bytes, o: int, fmt: str):
    v = struct.unpack_from(">" + fmt, b, o)
    return v[0], o + struct.calcsize(fmt)


def rname(b: bytes, o: int):
    n, o = rd(b, o, "H")
    return b[o:o + n].decode("utf8", "replace"), o + n


def payload(b: bytes, o: int, t: int):
    if t == 1: return rd(b, o, "b")
    if t == 2: return rd(b, o, "h")
    if t == 3: return rd(b, o, "i")
    if t == 4: return rd(b, o, "q")
    if t == 5: return rd(b, o, "f")
    if t == 6: return rd(b, o, "d")
    if t == 7:
        n, o = rd(b, o, "i")
        return list(struct.unpack_from(">%db" % n, b, o)), o + n
    if t == 8: return rname(b, o)
    if t == 9:
        et, o = rd(b, o, "b")
        n, o = rd(b, o, "i")
        out = []
        for _ in range(n):
            v, o = payload(b, o, et)
            out.append(v)
        return (et, out), o
    if t == 10:
        d = {}
        while True:
            tt, o = rd(b, o, "b")
            if tt == 0:
                break
            nm, o = rname(b, o)
            v, o = payload(b, o, tt)
            d[nm] = Tag(tt, v)
        return d, o
    if t == 11:
        n, o = rd(b, o, "i")
        return list(struct.unpack_from(">%di" % n, b, o)), o + 4 * n
    if t == 12:
        n, o = rd(b, o, "i")
        return list(struct.unpack_from(">%dq" % n, b, o)), o + 8 * n
    raise ValueError("tag %d" % t)


def parse(b: bytes):
    t, o = rd(b, 0, "b")
    nm, o = rname(b, o)
    v, o = payload(b, o, t)
    return nm, Tag(t, v)


def T_str(s: str) -> Tag: return Tag(8, s)
def T_int(i: int) -> Tag: return Tag(3, i)
def T_byte(i: int) -> Tag: return Tag(1, i)
def T_comp(d: dict) -> Tag: return Tag(10, d)


def T_list(et: int, items: list) -> Tag:
    return Tag(9, (et, items))


def T_short(i: int) -> Tag: return Tag(2, i)


# ── template entities (StructureTemplate "entities") ─────────────────────────
# Vanilla shape: {pos: [x, y, z] doubles (template-local, fractional),
# blockPos: [x, y, z] ints (the block the entity stands in), nbt: the entity's
# saved compound}. The loader (TemplateEngine) re-bases Pos and Rotation to
# the placement, drops UUID, and the engine builds the mob when the chunk is
# promoted; pieces placed through a pool element get finalizeSpawn(STRUCTURE)
# (SinglePoolElement's setFinalizeEntities), exactly like a village's
# villagers. Only the saved compound's own keys reach the mob: id, Rotation
# and, for a mob that must stay, PersistenceRequired (the village animals'
# templates carry it; a Hush structure's inhabitants do too).
def entity_nbt(entity_id: str, yaw: float = 0.0, persistent: bool = True,
               extra: dict | None = None) -> Tag:
    d = {"id": T_str(entity_id), "Rotation": Tag(9, (5, [float(yaw), 0.0]))}
    if persistent:
        d["PersistenceRequired"] = T_byte(1)
    if extra:
        d.update(extra)
    return T_comp(d)


def template_entity(x: float, y: float, z: float, nbt: Tag) -> dict:
    return {
        "pos": Tag(9, (6, [float(x), float(y), float(z)])),
        "blockPos": T_list(3, [math.floor(x), math.floor(y), math.floor(z)]),
        "nbt": nbt,
    }


# A monster spawner's template block entity (BaseSpawner.save's fields;
# TemplateEngine carries them into the placed spawner): SpawnData names the
# mob, the rest are BaseSpawner's limits, written only where they differ from
# vanilla's defaults.
def spawner_nbt(entity_id: str, **limits: int) -> Tag:
    d = {
        "SpawnData": T_comp({"entity": T_comp({"id": T_str(entity_id)})}),
        "id": T_str("minecraft:mob_spawner"),
    }
    for key, value in limits.items():
        d[key] = T_short(value)
    return T_comp(d)


def gzip_bytes(raw: bytes) -> bytes:
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as z:
        z.write(raw)
    return buf.getvalue()


# ── block vocabulary ─────────────────────────────────────────────────────────
# logical key -> (Hush block, vanilla stand-in). Stand-ins are blocks the
# library registers with the same block class (deepslate itself is a pillar
# there, so cobbled_deepslate stands in for hushstone).
BLOCKS = {
    "stone":     ("minecraft:hushstone",                 "minecraft:cobbled_deepslate"),
    "polished":  ("minecraft:polished_hushstone",        "minecraft:polished_deepslate"),
    "bricks":    ("minecraft:hushstone_bricks",          "minecraft:deepslate_bricks"),
    "cracked":   ("minecraft:cracked_hushstone_bricks",  "minecraft:cracked_deepslate_bricks"),
    "chiseled":  ("minecraft:chiseled_hushstone_bricks", "minecraft:chiseled_deepslate"),
    "loam":      ("minecraft:sculk_loam",                "minecraft:sculk"),
    "moss":      ("minecraft:hush_moss",                 "minecraft:moss_block"),
    "ore":       ("minecraft:echo_ore",                  "minecraft:deepslate_diamond_ore"),
    "crystal":   ("minecraft:resonant_crystal",          "minecraft:amethyst_block"),
    "core":      ("minecraft:echo_core",                 "minecraft:reinforced_deepslate"),
    "bloom":     ("minecraft:resonance_bloom",           "minecraft:dandelion"),
    "grass":     ("minecraft:hush_grass",                "minecraft:short_grass"),
    "log":       ("minecraft:whisperwood_log",           "minecraft:oak_log"),
    "leaves":    ("minecraft:lantern_leaves",            "minecraft:oak_leaves"),
    "fence":     ("minecraft:whisperwood_fence",         "minecraft:oak_fence"),
    "air":       ("minecraft:air",                       "minecraft:air"),
    "sculk":     ("minecraft:sculk",                     "minecraft:sculk"),
    "reinforced": ("minecraft:reinforced_deepslate",     "minecraft:reinforced_deepslate"),
    "lantern":   ("minecraft:echo_lantern",              "minecraft:soul_lantern"),
    "chest":     ("minecraft:chest",                     "minecraft:chest"),
    "jigsaw":    ("minecraft:jigsaw",                    "minecraft:jigsaw"),
    # Choir Hall / Hush Lighthouse / Warden's Tomb / Sunken Library
    "planks":    ("minecraft:whisperwood_planks",        "minecraft:oak_planks"),
    "ww_stairs": ("minecraft:whisperwood_stairs",        "minecraft:oak_stairs"),
    "brick_stairs": ("minecraft:hushstone_brick_stairs", "minecraft:deepslate_brick_stairs"),
    "brick_slab": ("minecraft:hushstone_brick_slab",     "minecraft:deepslate_brick_slab"),
    "tiles":     ("minecraft:deepslate_tiles",           "minecraft:deepslate_tiles"),
    "soul_lantern": ("minecraft:soul_lantern",           "minecraft:soul_lantern"),
    "chain":     ("minecraft:iron_chain",                "minecraft:iron_chain"),
    "sensor":    ("minecraft:sculk_sensor",              "minecraft:sculk_sensor"),
    "shrieker":  ("minecraft:sculk_shrieker",            "minecraft:sculk_shrieker"),
    "catalyst":  ("minecraft:sculk_catalyst",            "minecraft:sculk_catalyst"),
    "ladder":    ("minecraft:ladder",                    "minecraft:ladder"),
    "bookshelf": ("minecraft:bookshelf",                 "minecraft:bookshelf"),
    "lectern":   ("minecraft:lectern",                   "minecraft:lectern"),
    "water":     ("minecraft:water",                     "minecraft:water"),
    "kelp":      ("minecraft:kelp",                      "minecraft:kelp"),
    "kelp_plant": ("minecraft:kelp_plant",               "minecraft:kelp_plant"),
    "pickle":    ("minecraft:sea_pickle",                "minecraft:sea_pickle"),
    "resonite":  ("minecraft:resonite_block",            "minecraft:diamond_block"),
    "cobweb":    ("minecraft:cobweb",                    "minecraft:cobweb"),
    # The Choir Mother's puzzle blocks (the Hush mobs' block set). Placed by
    # slug with no Properties (their default state).
    "altar":     ("minecraft:choir_altar",               "minecraft:chiseled_deepslate"),
    "chime":     ("minecraft:resonant_chime",            "minecraft:amethyst_block"),
    # The Hush Lighthouse: its rotating lamp (a block entity: the template
    # carries {id} nbt so the generator attaches one, as vanilla templates
    # do for every block entity), the lantern-room glazing and the sculk
    # veins creeping up its base. cyan_stained_glass is registered in
    # Blocks.cpp for this; sea_lantern / glass stand in (plain cubes).
    "lamp":      ("minecraft:hush_lighthouse_lamp",      "minecraft:sea_lantern"),
    "glass":     ("minecraft:cyan_stained_glass",        "minecraft:glass"),
    "vein":      ("minecraft:sculk_vein",                "minecraft:sculk_vein"),
    # The Echo Vaults' reliquary spawner (a monster spawner block entity:
    # spawner_nbt).
    "spawner":   ("minecraft:spawner",                   "minecraft:spawner"),
}
# Full cubes a fence connects to (FenceBlock.connectsTo: solid faces).
FENCE_SOLID = {"stone", "polished", "bricks", "cracked", "chiseled", "loam", "moss", "ore",
               "crystal", "core", "log", "sculk", "reinforced", "fence", "planks", "tiles",
               "bookshelf", "resonite", "catalyst", "glass", "lamp"}
STAND_INS = False


def block_id(key: str) -> str:
    real, stand_in = BLOCKS[key]
    return stand_in if STAND_INS else real


# Property sets the library's block classes carry (RotatedPillarBlock,
# LeavesBlock, LanternBlockImpl, ChestBlockImpl, FenceBlock); simple blocks
# and bushes take none. Fence connections are filled in by Piece.to_nbt.
def props_for(key: str, **extra) -> dict | None:
    wl = "true" if extra.get("waterlogged", False) else "false"
    if key in ("ww_stairs", "brick_stairs"):
        return {"facing": extra["facing"], "half": extra.get("half", "bottom"),
                "shape": "straight", "waterlogged": wl}
    if key == "brick_slab":
        return {"type": extra.get("type", "bottom"), "waterlogged": wl}
    if key == "soul_lantern":
        return {"hanging": "true" if extra.get("hanging", True) else "false", "waterlogged": wl}
    if key == "chain":
        return {"axis": extra.get("axis", "y"), "waterlogged": wl}
    if key == "sensor":
        return {"power": "0", "sculk_sensor_phase": "inactive", "waterlogged": wl}
    if key == "shrieker":
        # can_summon false: a Hush structure's shrieker shrieks, it does not
        # call a warden (the ancient city's are the summoners).
        return {"can_summon": "false", "shrieking": "false", "waterlogged": wl}
    if key == "catalyst":
        return {"bloom": "false"}
    if key == "ladder":
        return {"facing": extra["facing"], "waterlogged": wl}
    if key == "lectern":
        return {"facing": extra["facing"],
                "has_book": "true" if extra.get("has_book", False) else "false", "powered": "false"}
    if key == "water":
        return {"level": "0"}
    if key == "kelp":
        return {"age": str(extra.get("age", 20))}
    if key == "pickle":
        return {"pickles": str(extra.get("pickles", 1)),
                "waterlogged": "true" if extra.get("waterlogged", True) else "false"}
    if key == "log":
        return {"axis": extra.get("axis", "y")}
    if key == "leaves":
        return {"distance": "7", "persistent": "true", "waterlogged": "false"}
    if key == "lantern":
        return {"hanging": "true" if extra.get("hanging", True) else "false", "waterlogged": "false"}
    if key == "chest":
        return {"facing": extra["facing"], "type": "single", "waterlogged": wl}
    if key == "fence":
        return {"north": "false", "east": "false", "south": "false", "west": "false",
                "waterlogged": "false"}
    if key == "vein":
        # SculkVeinBlock (MultifaceBlock): one boolean per face it covers.
        faces = set(extra.get("faces", ()))
        props = {d: "true" if d in faces else "false"
                 for d in ("down", "east", "north", "south", "up", "west")}
        props["waterlogged"] = wl
        return props
    return None


ORIENTATION = {  # front direction -> jigsaw orientation (front_top)
    "north": "north_up", "south": "south_up", "east": "east_up", "west": "west_up",
    "up": "up_north", "down": "down_north",
}
FRONT_OF = {v: k for k, v in ORIENTATION.items()}

# ── baked chest loot ─────────────────────────────────────────────────────────
# (item id, min, max, weight, components) - mirrors the chest loot tables.
LOOT = {
    VAULT_LOOT: [
        ("minecraft:echo_shard", 2, 4, 5, None),
        ("minecraft:disc_fragment_5", 1, 2, 3, None),
        ("minecraft:sculk", 2, 5, 4, None),
        ("minecraft:resonant_crystal", 1, 3, 4, None),
        ("minecraft:hushstone_bricks", 4, 12, 4, None),
        ("minecraft:raw_resonite", 2, 4, 6, None),
        ("minecraft:resonite_ingot", 1, 2, 2, None),
        ("minecraft:soul_lantern", 1, 2, 2, None),
        ("minecraft:enchanted_book", 1, 1, 2, "book"),
        ("minecraft:diamond", 1, 1, 1, None),
    ],
    SHRINE_LOOT: [
        ("minecraft:echo_shard", 1, 3, 10, None),
        ("minecraft:resonance_bloom", 1, 2, 6, None),
        ("minecraft:hush_moss", 2, 4, 6, None),
        ("minecraft:whisperwood_sapling", 1, 2, 5, None),
        ("minecraft:raw_resonite", 1, 2, 4, None),
        ("minecraft:iron_ingot", 1, 3, 4, None),
        ("minecraft:resonant_crystal", 1, 1, 3, None),
        ("minecraft:enchanted_book", 1, 1, 2, "book"),
    ],
    CHOIR_LOOT: [
        ("minecraft:echo_shard", 2, 4, 8, None),
        ("minecraft:resonance_bloom", 2, 4, 6, None),
        ("minecraft:resonant_crystal", 1, 3, 5, None),
        ("minecraft:amethyst_shard", 2, 6, 5, None),
        ("minecraft:disc_fragment_5", 1, 2, 3, None),
        ("minecraft:resonite_ingot", 1, 2, 3, None),
        ("minecraft:echo_lantern", 1, 2, 3, None),
        ("minecraft:enchanted_book", 1, 1, 3, "book"),
        ("minecraft:golden_apple", 1, 1, 1, None),
    ],
    LIGHTHOUSE_LOOT: [
        ("minecraft:echo_shard", 1, 3, 8, None),
        ("minecraft:glow_ink_sac", 1, 3, 6, None),
        ("minecraft:paper", 2, 5, 6, None),
        ("minecraft:arrow", 4, 10, 5, None),
        ("minecraft:raw_resonite", 1, 2, 4, None),
        ("minecraft:resonant_crystal", 1, 2, 3, None),
        ("minecraft:spyglass", 1, 1, 2, None),
        ("minecraft:sculk_sensor", 1, 1, 2, None),
        ("minecraft:hush_lighthouse_lamp", 1, 1, 1, None),
    ],
    TOMB_LOOT: [
        ("minecraft:echo_shard", 3, 6, 8, None),
        ("minecraft:resonite_ingot", 2, 4, 6, None),
        ("minecraft:raw_resonite", 3, 6, 6, None),
        ("minecraft:disc_fragment_5", 2, 3, 5, None),
        ("minecraft:resonant_crystal", 2, 4, 5, None),
        ("minecraft:diamond", 1, 3, 4, None),
        ("minecraft:golden_apple", 1, 2, 3, None),
        ("minecraft:enchanted_book", 1, 1, 4, "book"),
        ("minecraft:music_disc_5", 1, 1, 1, None),
        ("minecraft:enchanted_golden_apple", 1, 1, 1, None),
    ],
    LIBRARY_LOOT: [
        ("minecraft:book", 1, 3, 10, None),
        ("minecraft:paper", 2, 7, 8, None),
        ("minecraft:echo_shard", 1, 3, 6, None),
        ("minecraft:disc_fragment_5", 1, 2, 4, None),
        ("minecraft:glow_ink_sac", 1, 3, 4, None),
        ("minecraft:resonant_crystal", 1, 2, 3, None),
        ("minecraft:enchanted_book", 1, 1, 4, "book"),
        ("minecraft:recovery_compass", 1, 1, 1, None),
    ],
}
BOOK_ENCHANTS = [("minecraft:unbreaking", 2), ("minecraft:efficiency", 3),
                 ("minecraft:mending", 1), ("minecraft:silk_touch", 1)]

BAKE_ITEMS = False


def bake_loot(seed_key: str, table: str) -> list[dict]:
    """Distinct stacks per chest (weighted draw without replacement); the
    vault chests also guarantee an echo-shard stack: the vaults are the
    shard's home."""
    rng = random.Random(zlib.crc32(seed_key.encode("utf8")))
    entries = LOOT[table]
    rolls = rng.randint(4, 6) if table in (VAULT_LOOT, TOMB_LOOT) else rng.randint(2, 4)
    remaining = list(entries)
    picked = []
    while remaining and len(picked) < rolls:
        entry = rng.choices(remaining, weights=[e[3] for e in remaining])[0]
        remaining.remove(entry)
        picked.append(entry)
    if table in (VAULT_LOOT, TOMB_LOOT) and not any(e[0] == "minecraft:echo_shard" for e in picked):
        picked[0] = entries[0]
    slots = sorted(rng.sample(range(27), len(picked)))
    items = []
    for slot, (item_id, lo, hi, _w, comp) in zip(slots, picked):
        entry = {"Slot": T_byte(slot), "id": T_str(item_id), "count": T_int(rng.randint(lo, hi))}
        if comp == "book":
            ench, level = rng.choice(BOOK_ENCHANTS)
            entry["components"] = T_comp({
                "minecraft:stored_enchantments": T_comp({ench: T_int(level)})})
        items.append(entry)
    return items


# ── the Sunken Library's lectern books ───────────────────────────────────────
# Written by the library's keeper, Wren of the Listeners (the author of the
# Sunken Library's chest books, chests/sunken_library), in the canon of
# docs/hush-lore.md: the Sunken Choir district drowned after the Great Rest.
# Pages are plain text, each within a book page (14 lines of ~19 characters).
LIBRARY_LECTERN_BOOKS = [
    {"title": "The Drowned Stacks", "author": "Wren of the Listeners", "pages": [
        "The water came up\nthe stair one step\neach year after\nthe Rest.\n\nI moved the hymnals\nfirst. Then the\nlitanies. Then\nanything I could\ncarry.",
        "What is under the\nwater now is the\nlower catalogue:\ntuning records,\nthe Hall's old\naccounts, the\nchildren's scores.\n\nThe glue lets go.\nThe song does not.",
        "If you dive for\nthem, bring a light.\nThe pages still\nhum a little when\nyou hold them near\na crystal.\n\nThey remember what\nthey were for.",
    ]},
    {"title": "A Listener's Rule", "author": "Wren of the Listeners", "pages": [
        "RULES OF THE\nLIBRARY\n\n1. Listen first.\n2. Read aloud only\nin the Hall.\n3. Never close a\nhymnal on a held\nnote.",
        "4. A book is\nreturned to the\nshelf it sang from.\n5. Echo shards are\nnot bookmarks.\n6. Do not answer\nanything you hear\nin the stacks.",
        "7. The seventh\nrule was never\nwritten down. Ask\na Listener.\n\nThere are no\nListeners left.\nIt was: stay\nuntil the song\nis done.",
    ]},
    {"title": "Last Entry", "author": "Wren of the Listeners", "pages": [
        "The gallery is dry\nfor now. The\nwarden walked\nthrough the reading\nroom tonight and\ndid not stop.\n\nI kept very still.\nI am good at that.",
        "The others went\nthrough the gates\nwith the Last\nProcession. I told\nthem someone had\nto keep the books\nquiet.\n\nThat was a lie.",
        "I stayed because\nthe water sings\nwhen it rises, the\nway the Heart used\nto. I wanted to\nhear it one more\ntime.\n\nIt is very close\nnow.",
    ]},
]


def lectern_book_nbt(book: dict) -> Tag:
    """LecternBlockEntity: Book (a written_book ItemStack with its
    WRITTEN_BOOK_CONTENT in MC's saved Filterable {raw: ...} form) and Page."""
    content = T_comp({
        "title": T_comp({"raw": T_str(book["title"])}),
        "author": T_str(book["author"]),
        "pages": T_list(10, [{"raw": T_str(page)} for page in book["pages"]]),
        "resolved": T_byte(1),
    })
    item = T_comp({
        "id": T_str("minecraft:written_book"),
        "count": T_int(1),
        "components": T_comp({"minecraft:written_book_content": content}),
    })
    return T_comp({"Book": item, "Page": T_int(0), "id": T_str("minecraft:lectern")})


# ── piece builder ────────────────────────────────────────────────────────────
class Piece:
    def __init__(self, folder: str, name: str, sx: int, sy: int, sz: int):
        self.folder = folder
        self.name = name
        self.size = (sx, sy, sz)
        # (x, y, z) -> (block key, props dict | None, nbt Tag | None)
        self.cells: dict[tuple[int, int, int], tuple[str, dict | None, Tag | None]] = {}
        # Template entities, in placement order (template_entity dicts).
        self.entities: list[dict] = []

    @property
    def location(self) -> str:
        return f"minecraft:{self.folder}/{self.name}"

    def inside(self, x: int, y: int, z: int) -> bool:
        sx, sy, sz = self.size
        return 0 <= x < sx and 0 <= y < sy and 0 <= z < sz

    def key_at(self, x: int, y: int, z: int) -> str | None:
        cell = self.cells.get((x, y, z))
        return None if cell is None else cell[0]

    def set(self, x: int, y: int, z: int, key: str, props: dict | None = None,
            nbt: Tag | None = None, **extra) -> None:
        assert self.inside(x, y, z), (self.name, x, y, z)
        if props is None:
            props = props_for(key, **extra)
        self.cells[(x, y, z)] = (key, props, nbt)

    def fill(self, x0: int, y0: int, z0: int, x1: int, y1: int, z1: int,
             key: str, **extra) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            for y in range(min(y0, y1), max(y0, y1) + 1):
                for z in range(min(z0, z1), max(z0, z1) + 1):
                    self.set(x, y, z, key, **extra)

    def carve(self, x0: int, y0: int, z0: int, x1: int, y1: int, z1: int) -> None:
        self.fill(x0, y0, z0, x1, y1, z1, "air")

    def shell(self, interior: set[tuple[int, int, int]], wall: str, mass: str) -> None:
        """Air in `interior`; `wall` on every box cell touching it (6-neigh);
        `mass` everywhere else, so the box is completely specified."""
        sx, sy, sz = self.size
        for x in range(sx):
            for y in range(sy):
                for z in range(sz):
                    if (x, y, z) in interior:
                        self.set(x, y, z, "air")
                        continue
                    touching = any((x + dx, y + dy, z + dz) in interior
                                   for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0),
                                                      (0, -1, 0), (0, 0, 1), (0, 0, -1)))
                    self.set(x, y, z, wall if touching else mass)

    def jigsaw(self, x: int, y: int, z: int, front: str, *, name: str, target: str,
               pool: str, final_state: str, joint: str) -> None:
        nbt = T_comp({
            "joint": T_str(joint),
            "final_state": T_str(final_state),
            "name": T_str(name),
            "pool": T_str(pool),
            "id": T_str("minecraft:jigsaw"),
            "target": T_str(target),
        })
        self.set(x, y, z, "jigsaw", {"orientation": ORIENTATION[front]}, nbt)

    def link(self, x: int, y: int, z: int, front: str, pool: str) -> None:
        self.jigsaw(x, y, z, front, name=LINK, target=LINK, pool=pool,
                    final_state=block_id("bricks"), joint="aligned")

    def chest(self, x: int, y: int, z: int, facing: str, seed_key: str,
              table: str = VAULT_LOOT, waterlogged: bool = False) -> None:
        if BAKE_ITEMS:
            items = bake_loot(seed_key, table)
            nbt = T_comp({
                "Items": T_list(10, [T_comp(i).v for i in items]),
                "id": T_str("minecraft:chest"),
            })
        else:
            # RandomizableContainer.setBlockEntityLootTable: the seed is
            # drawn at placement (TemplateEngine appends LootTableSeed).
            nbt = T_comp({
                "LootTable": T_str(table),
                "id": T_str("minecraft:chest"),
            })
        self.set(x, y, z, "chest", nbt=nbt, facing=facing, waterlogged=waterlogged)

    def lantern(self, x: int, y: int, z: int, hanging: bool = True) -> None:
        self.set(x, y, z, "lantern", hanging=hanging)

    def entity(self, x: float, y: float, z: float, entity_id: str, yaw: float = 0.0,
               persistent: bool = True, extra: dict | None = None) -> None:
        """A mob placed with the template (see template_entity). (x, y, z)
        is template-local and may lie outside the template's box — the
        loader only requires the block to be in the chunk being decorated."""
        self.entities.append(template_entity(x, y, z, entity_nbt(entity_id, yaw, persistent, extra)))

    def spawner(self, x: int, y: int, z: int, entity_id: str, **limits: int) -> None:
        self.set(x, y, z, "spawner", nbt=spawner_nbt(entity_id, **limits))

    def soul_lantern(self, x: int, y: int, z: int, hanging: bool = True) -> None:
        self.set(x, y, z, "soul_lantern", hanging=hanging)

    def _fence_props(self, x: int, y: int, z: int) -> dict:
        sides = {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}
        props = {}
        for side, (dx, dz) in sides.items():
            nk = self.key_at(x + dx, y, z + dz)
            props[side] = "true" if nk in FENCE_SOLID else "false"
        props["waterlogged"] = "false"
        return props

    def to_nbt(self) -> bytes:
        sx, sy, sz = self.size
        assert len(self.cells) == sx * sy * sz, \
            f"{self.name}: {len(self.cells)} of {sx * sy * sz} cells set"
        palette: list[tuple[str, tuple]] = []
        index: dict[tuple[str, tuple], int] = {}
        blocks = []
        for (x, y, z) in sorted(self.cells, key=lambda p: (p[1], p[2], p[0])):
            key_name, props, nbt = self.cells[(x, y, z)]
            if key_name == "fence":
                props = self._fence_props(x, y, z)
            bid = block_id(key_name)
            key = (bid, tuple(sorted((props or {}).items())))
            if key not in index:
                index[key] = len(palette)
                palette.append(key)
            entry = {"pos": T_list(3, [x, y, z]), "state": T_int(index[key])}
            if nbt is not None:
                entry["nbt"] = nbt
            blocks.append(entry)
        palette_tags = []
        for bid, props in palette:
            entry = {}
            if props:
                entry["Properties"] = T_comp({k: T_str(v) for k, v in props})
            entry["Name"] = T_str(bid)
            palette_tags.append(entry)
        root = T_comp({
            "size": T_list(3, [sx, sy, sz]),
            "entities": T_list(10, self.entities) if self.entities else T_list(0, []),
            "blocks": T_list(10, blocks),
            "palette": T_list(10, palette_tags),
            "DataVersion": T_int(DATA_VERSION),
        })
        return gzip_bytes(serialize("", root))


def box(x0, y0, z0, x1, y1, z1) -> set[tuple[int, int, int]]:
    return {(x, y, z) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1) for z in range(z0, z1 + 1)}


def piece_rng(location: str) -> random.Random:
    return random.Random(zlib.crc32(location.encode("utf8")))


# ── the Echo Vaults pieces ───────────────────────────────────────────────────
def vault_piece(name: str, sx: int, sy: int, sz: int) -> Piece:
    return Piece("echo_vaults", name, sx, sy, sz)


def build_center(with_shaft: bool) -> Piece:
    p = vault_piece("center_1", 15, 11, 15)
    rng = piece_rng(f"echo_vaults/{p.name}")
    interior = box(1, 1, 1, 13, 9, 13)
    interior |= box(6, 1, 0, 8, 3, 0) | box(6, 1, 14, 8, 3, 14)      # N / S doorways
    interior |= box(0, 1, 6, 0, 3, 8) | box(14, 1, 6, 14, 3, 8)      # W / E doorways
    if with_shaft:
        interior |= box(6, 10, 6, 8, 10, 8)                          # ceiling hole
    p.shell(interior, wall="bricks", mass="bricks")
    # Polished band around the walls at eye height, and a polished floor ring.
    for x in range(15):
        for z in range(15):
            if x in (0, 14) or z in (0, 14):
                if (x, 5, z) not in interior:
                    p.set(x, 5, z, "polished")
            if (x in (1, 13) or z in (1, 13)) and 1 <= x <= 13 and 1 <= z <= 13:
                p.set(x, 0, z, "polished")
    # Reinforced-deepslate corner pillars - the builders' signature material -
    # on a chiseled base with a chiseled capital.
    for cx, cz in ((1, 1), (13, 1), (1, 13), (13, 13)):
        p.set(cx, 1, cz, "chiseled")
        p.fill(cx, 2, cz, cx, 8, cz, "reinforced")
        p.set(cx, 9, cz, "chiseled")
    # Sculk creeping over the floor in three patches.
    for _ in range(3):
        cx, cz = rng.randint(3, 11), rng.randint(3, 11)
        for x in range(cx - 2, cx + 3):
            for z in range(cz - 2, cz + 3):
                if abs(x - cx) + abs(z - cz) <= 2 and rng.random() < 0.8 and 2 <= x <= 12 and 2 <= z <= 12:
                    if not (5 <= x <= 9 and 5 <= z <= 9):
                        p.set(x, 0, z, "sculk")
    # Plinth, the resonant-crystal column and the echo core at its top: the
    # vault's heart (breaking it wakes the Silent Warden).
    p.fill(6, 1, 6, 8, 1, 8, "polished")
    p.fill(7, 2, 7, 7, 4, 7, "crystal")
    p.set(7, 5, 7, "core")
    for x, z in ((6, 7), (8, 7), (7, 6), (7, 8)):
        p.set(x, 2, z, "crystal")
    # Four echo lanterns hanging from the ceiling.
    for x, z in ((3, 3), (11, 3), (3, 11), (11, 11)):
        p.lantern(x, 9, z)
    # Two chests in the corners by the pillars.
    p.chest(2, 1, 2, "east", "center_1/chest_a")
    p.chest(12, 1, 12, "west", "center_1/chest_b")
    # Doorway links (floor row, outer face, front outward).
    p.link(7, 0, 0, "north", POOL_HALLS)
    p.link(7, 0, 14, "south", POOL_HALLS)
    p.link(0, 0, 7, "west", POOL_HALLS)
    p.link(14, 0, 7, "east", POOL_HALLS)
    if with_shaft:
        # Up-facing link in the ceiling hole: the sinkhole shaft hangs off it.
        p.jigsaw(7, 10, 7, "up", name=SHAFT_LINK, target=SHAFT_LINK, pool=POOL_SHAFT,
                 final_state="minecraft:air", joint="rollable")
    return p


def build_shaft() -> Piece:
    # 5x4x5 collar with a 3x3 open core; sits on the vault ceiling and reaches
    # about two blocks above the local surface (start_height -12 puts the
    # vault floor 13 below the first free block).
    p = vault_piece("shaft", 5, 4, 5)
    interior = box(1, 0, 1, 3, 3, 3)
    p.shell(interior, wall="bricks", mass="bricks")
    for x, z in ((0, 0), (4, 0), (0, 4), (4, 4)):
        p.set(x, 3, z, "chiseled")
    p.jigsaw(2, 0, 2, "down", name=SHAFT_LINK, target=SHAFT_LINK, pool="minecraft:empty",
             final_state="minecraft:air", joint="rollable")
    return p


def pilasters(p: Piece, zs: tuple[int, ...]) -> None:
    # Polished wall pilasters with a chiseled block at eye height.
    for z in zs:
        for x in (0, 4):
            p.fill(x, 1, z, x, 3, z, "polished")
            p.set(x, 2, z, "chiseled")


def build_hall_straight() -> Piece:
    p = vault_piece("hall_straight", 5, 5, 9)
    p.shell(box(1, 1, 0, 3, 3, 8), wall="bricks", mass="bricks")
    pilasters(p, (2, 6))
    p.fill(2, 0, 1, 2, 0, 7, "polished")
    p.lantern(2, 3, 4)
    p.link(2, 0, 0, "north", POOL_HALL_NEXT)
    p.link(2, 0, 8, "south", POOL_HALL_NEXT)
    return p


def build_hall_arch() -> Piece:
    p = vault_piece("hall_arch", 5, 6, 9)
    interior = box(1, 1, 0, 3, 3, 8) | box(2, 4, 1, 2, 4, 7)   # raised centre channel
    p.shell(interior, wall="bricks", mass="bricks")
    p.fill(2, 5, 1, 2, 5, 7, "polished")
    pilasters(p, (2, 6))
    p.lantern(2, 4, 2)
    p.lantern(2, 4, 6)
    p.link(2, 0, 0, "north", POOL_HALL_NEXT)
    p.link(2, 0, 8, "south", POOL_HALL_NEXT)
    return p


def build_hall_bend() -> Piece:
    # Compact L corner: in from the south face, out through the east face.
    p = vault_piece("hall_bend", 5, 5, 5)
    interior = box(1, 1, 1, 3, 3, 4) | box(1, 1, 1, 4, 3, 3)
    p.shell(interior, wall="bricks", mass="bricks")
    p.fill(0, 1, 0, 0, 3, 0, "polished")
    p.set(0, 2, 0, "chiseled")
    p.set(2, 0, 2, "polished")
    p.lantern(2, 3, 2)
    p.link(2, 0, 4, "south", POOL_HALL_NEXT)
    p.link(4, 0, 2, "east", POOL_HALL_NEXT)
    return p


def build_room_reliquary() -> Piece:
    p = vault_piece("room_reliquary", 9, 7, 9)
    rng = piece_rng(f"echo_vaults/{p.name}")
    interior = box(1, 1, 1, 7, 5, 7) | box(3, 1, 8, 5, 3, 8)
    p.shell(interior, wall="bricks", mass="bricks")
    for x in range(9):
        for z in range(9):
            if (x in (0, 8) or z in (0, 8)) and (x, 3, z) not in interior:
                p.set(x, 3, z, "polished")
    # Echo-ore pillars with chiseled caps: the ore the builders mined.
    for cx, cz in ((2, 2), (6, 2), (2, 6), (6, 6)):
        p.fill(cx, 1, cz, cx, 4, cz, "ore")
        p.set(cx, 5, cz, "chiseled")
    for _ in range(6):
        x, z = rng.randint(1, 7), rng.randint(1, 7)
        if (x, z) not in ((4, 4),):
            p.set(x, 0, z, "sculk")
    # The reliquary's keeper: an echo-wraith spawner as the chest's plinth
    # (a vanilla dungeon's arrangement — the loot sits on the danger). Gentler
    # than vanilla's defaults: two wraiths a cycle, at most three about, a
    # longer rest between cycles, and it wakes only for a player within 12.
    p.spawner(4, 1, 4, "minecraft:echo_wraith", SpawnCount=2, MaxNearbyEntities=3,
              MinSpawnDelay=300, MaxSpawnDelay=900, RequiredPlayerRange=12)
    p.chest(4, 2, 4, "south", "room_reliquary/chest")
    p.lantern(4, 5, 2)
    p.lantern(4, 5, 6)
    p.link(4, 0, 8, "south", POOL_HALL_NEXT)
    return p


def build_room_garden() -> Piece:
    p = vault_piece("room_garden", 9, 7, 9)
    rng = piece_rng(f"echo_vaults/{p.name}")
    interior = box(1, 1, 1, 7, 5, 7) | box(3, 1, 8, 5, 3, 8)
    p.shell(interior, wall="bricks", mass="bricks")
    # Living floor: loam with moss islands.
    for x in range(1, 8):
        for z in range(1, 8):
            p.set(x, 0, z, "moss" if rng.random() < 0.35 else "loam")
    p.fill(3, 0, 8, 5, 0, 8, "bricks")
    # A whisperwood tree in the middle with a lantern-leaf crown.
    p.fill(4, 1, 4, 4, 3, 4, "log", axis="y")
    for x in range(3, 6):
        for z in range(3, 6):
            p.set(x, 4, z, "leaves")
    for x, z in ((2, 4), (6, 4), (4, 2), (4, 6)):
        p.set(x, 3, z, "leaves")
    p.set(4, 5, 4, "leaves")
    # Lantern-leaf ring around the ceiling edge.
    for x in range(1, 8):
        for z in range(1, 8):
            if x in (1, 7) or z in (1, 7):
                p.set(x, 5, z, "leaves")
    # Crystal spires in the corners.
    p.fill(1, 1, 1, 1, 3, 1, "crystal")
    p.fill(7, 1, 1, 7, 2, 1, "crystal")
    p.fill(1, 1, 7, 1, 2, 7, "crystal")
    p.set(7, 1, 7, "crystal")
    # Whisperwood-fence railing around the tree bed, open on the north and
    # south sides so the bed can be walked into.
    for x in range(2, 7):
        for z in range(2, 7):
            if (x in (2, 6) or z in (2, 6)) and not (x == 4 and z in (2, 6)):
                p.set(x, 1, z, "fence")
    # Resonance blooms on the open ground.
    placed = 0
    while placed < 6:
        x, z = rng.randint(2, 6), rng.randint(2, 6)
        if p.key_at(x, 1, z) == "air" and (x, z) != (4, 4):
            p.set(x, 1, z, "bloom")
            placed += 1
    p.link(4, 0, 8, "south", POOL_HALL_NEXT)
    return p


def build_end_cap() -> Piece:
    p = vault_piece("end_cap", 5, 5, 1)
    p.fill(0, 0, 0, 4, 4, 0, "bricks")
    p.set(2, 2, 0, "polished")
    p.link(2, 0, 0, "south", "minecraft:empty")
    return p


def build_vaults(with_shaft: bool) -> list[Piece]:
    pieces = [build_center(with_shaft), build_hall_straight(), build_hall_arch(),
              build_hall_bend(), build_room_reliquary(), build_room_garden(), build_end_cap()]
    if with_shaft:
        pieces.append(build_shaft())
    return pieces


# ── the Whisper Shrine ───────────────────────────────────────────────────────
SHRINE_PILLARS = ((1, 4), (7, 4), (4, 1), (4, 7), (2, 2), (6, 2), (2, 6), (6, 6))


def build_shrine() -> Piece:
    # One surface piece, 9x6x9. y=0 replaces the ground's top layer (see the
    # loader contract): a polished-hushstone disc with loam and moss pockets
    # in its outer ring, sculk loam outside the disc so the corners stay
    # ground. A ring of eight hushstone-brick pillars (two bricks + chiseled
    # cap), two of them fallen; an echo lantern standing on a central plinth;
    # a chest beside it; hush grass and blooms on the soil pockets.
    p = Piece("whisper_shrine", "shrine", 9, 6, 9)
    rng = piece_rng(p.location)
    for x in range(9):
        for z in range(9):
            r2 = (x - 4) ** 2 + (z - 4) ** 2
            p.set(x, 0, z, "polished" if r2 <= 18 else "loam")
    p.fill(0, 1, 0, 8, 5, 8, "air")
    soil = []
    for x in range(9):
        for z in range(9):
            r2 = (x - 4) ** 2 + (z - 4) ** 2
            if 5 <= r2 <= 18 and (x, z) not in SHRINE_PILLARS and rng.random() < 0.45:
                p.set(x, 0, z, "moss" if rng.random() < 0.5 else "loam")
                soil.append((x, z))
    for n, (x, z) in enumerate(SHRINE_PILLARS):
        if n == 4:                       # fallen: only the base is left
            p.set(x, 1, z, "cracked")
        elif n == 7:                     # fallen: no cap
            p.set(x, 1, z, "bricks")
            p.set(x, 2, z, "cracked")
        else:
            p.set(x, 1, z, "bricks")
            p.set(x, 2, z, "cracked" if rng.random() < 0.3 else "bricks")
            p.set(x, 3, z, "chiseled")
    p.set(4, 1, 4, "polished")
    p.lantern(4, 2, 4, hanging=False)
    p.chest(5, 1, 4, "east", "whisper_shrine/chest", table=SHRINE_LOOT)
    # Two lumen moths keep the lantern company (they circle the nearest
    # light). Kept (PersistenceRequired), so the shrine never loses them.
    p.entity(3.4, 3.3, 4.6, "minecraft:lumen_moth", yaw=40.0)
    p.entity(5.3, 3.8, 3.4, "minecraft:lumen_moth", yaw=220.0)
    grass = blooms = 0
    for (x, z) in soil:
        if p.key_at(x, 1, z) != "air":
            continue
        roll = rng.random()
        if roll < 0.55:
            p.set(x, 1, z, "grass")
            grass += 1
        elif roll < 0.80:
            p.set(x, 1, z, "bloom")
            blooms += 1
    assert grass >= 3 and blooms >= 1, (grass, blooms)
    return p


# ── the Choir Hall ───────────────────────────────────────────────────────────
CHOIR_CHIMES = ((16, 12), (14, 16), (10, 16), (8, 12), (10, 8), (14, 8))
CHOIR_PILLARS = ((19, 15), (15, 19), (9, 19), (5, 15), (5, 9), (9, 5), (15, 5), (19, 9))


def build_choir_hall() -> Piece:
    # One surface piece, 25x18x25: a square hall under a domed groin vault.
    # y=0 is the floor and replaces the ground's top layer. Walls rise to 10;
    # the vault climbs from 10 at the walls to 16 over the centre, ribbed in
    # polished hushstone along the axes and diagonals. Eight polished piers
    # on a ring of radius 8 (rotated off the axes so the nave from the south
    # door is clear) carry it. At the centre a 5x5 polished dais holds the
    # choir_altar, soul lanterns on its corners; around it, on the floor, a
    # ring of six resonant_chime blocks at radius ~4.5 (the Choir Mother's
    # note puzzle). Whisperwood-stair choir stalls face in along the east and
    # west walls; two chests stand in the north corners; a rose window opens
    # the north wall; echo lanterns hang on chains from the vault.
    w, h = 25, 18
    c = 12
    p = Piece("choir_hall", "hall", w, h, w)
    rng = piece_rng(p.location)
    p.fill(0, 0, 0, w - 1, h - 1, w - 1, "air")

    def vault(x: int, z: int) -> int:
        d = math.hypot(x - c, z - c)
        return 10 + int(round(6 * max(0.0, 1.0 - (d / 12.0) ** 2)))

    def inside(x: int, z: int) -> bool:
        return 1 <= x <= w - 2 and 1 <= z <= w - 2

    # Floor: polished, a chiseled ring round the dais, a brick ring further
    # out, sculk creeping in along the walls.
    for x in range(w):
        for z in range(w):
            d = math.hypot(x - c, z - c)
            key = "polished"
            if 5.5 <= d < 6.5:
                key = "chiseled"
            elif 9.5 <= d < 10.5:
                key = "bricks"
            elif d > 10.5 and inside(x, z) and rng.random() < 0.18:
                key = "sculk"
            p.set(x, 0, z, key)
    # Walls to y 10.
    for x in range(w):
        for z in range(w):
            if not inside(x, z):
                p.fill(x, 1, z, x, 10, z, "bricks")
    # Vault: each interior column's roof from its own height up to one below
    # its tallest neighbour's, so steps between columns stay closed.
    for x in range(1, w - 1):
        for z in range(1, w - 1):
            top = vault(x, z)
            nb = max(vault(x + dx, z + dz) if inside(x + dx, z + dz) else 10
                     for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            rib = x == c or z == c or abs(x - c) == abs(z - c)
            for y in range(top, max(top, nb - 1) + 1):
                p.set(x, y, z, "polished" if rib else "bricks")
    # Wall band at eye height and window slits on the east and west walls.
    for i in range(w):
        for (x, z) in ((0, i), (w - 1, i), (i, 0), (i, w - 1)):
            p.set(x, 4, z, "polished")
    for i in (4, 8, 16, 20):
        for x in (0, w - 1):
            p.fill(x, 5, i, x, 7, i, "air")
        p.fill(i, 5, 0, i, 7, 0, "air")
    # Rose window in the north wall.
    for x in range(c - 3, c + 4):
        for y in range(4, 11):
            r2 = (x - c) ** 2 + (y - 7) ** 2
            if r2 <= 4:
                p.set(x, y, 0, "air")
            elif r2 <= 9:
                p.set(x, y, 0, "chiseled")
    # South door, 3 wide and 5 tall, under a chiseled lintel.
    p.fill(c - 1, 1, w - 1, c + 1, 5, w - 1, "air")
    p.fill(c - 2, 6, w - 1, c + 2, 6, w - 1, "chiseled")
    # Piers: chiseled base and capital, polished shaft up to the vault.
    for x, z in CHOIR_PILLARS:
        top = vault(x, z) - 1
        p.set(x, 1, z, "chiseled")
        p.fill(x, 2, z, x, top - 1, z, "polished")
        p.set(x, top, z, "chiseled")
    # Dais, altar and the chime ring.
    p.fill(c - 2, 1, c - 2, c + 2, 1, c + 2, "polished")
    p.set(c, 2, c, "altar")
    for x, z in ((c - 2, c - 2), (c + 2, c - 2), (c - 2, c + 2), (c + 2, c + 2)):
        p.soul_lantern(x, 2, z, hanging=False)
    for x, z in CHOIR_CHIMES:
        p.set(x, 1, z, "chime")
    # Choir stalls: backs to the walls, seats facing the dais.
    for z in range(7, 18):
        if z == c:
            continue
        p.set(2, 1, z, "ww_stairs", facing="west")
        p.set(w - 3, 1, z, "ww_stairs", facing="east")
    # Echo lanterns hanging on two links of chain from the vault, one over
    # the altar and four over the ring.
    for x, z in ((c, c), (c - 5, c), (c + 5, c), (c, c - 5), (c, c + 5)):
        roof = vault(x, z)
        p.set(x, roof - 1, z, "chain", axis="y")
        p.set(x, roof - 2, z, "chain", axis="y")
        p.lantern(x, roof - 3, z)
    # Chests in the north corners.
    p.chest(2, 1, 2, "south", "choir_hall/chest_a", table=CHOIR_LOOT)
    p.chest(w - 3, 1, 2, "south", "choir_hall/chest_b", table=CHOIR_LOOT)
    return p


# ── the Hush Lighthouse ──────────────────────────────────────────────────────
LIGHTHOUSE_W = 11                  # footprint (x and z)
LIGHTHOUSE_H = 32                  # y 0..31
LIGHTHOUSE_C = 5                   # the tower's axis, x = z = 5
LIGHTHOUSE_LAMP_Y = 25             # the lamp's cell; the beams leave at y 25.5


def build_hush_lighthouse() -> Piece:
    # One surface piece, 11x32x11: a weathered, tapering hushstone-brick
    # lighthouse whose lamp sweeps two beams over the Hush (the lamp is a
    # block entity; HushLighthouseRenderer draws the sweep). Octagonal
    # courses (a square with its corners chamfered) around the axis:
    #   y 0       the ground layer: a polished foundation disc, a sculk ring
    #             round it, sculk loam out to the corners (the jigsaw start
    #             lands with y 0 ON the surface block, and beard_thin builds
    #             an islet under it when the site is water)
    #   y 1..5    the keeper's room (outer radius 4): chest, bookshelves,
    #             a lectern, a chair, an echo lantern; south door, three
    #             cyan-glass windows; the ladder starts against a polished
    #             pilaster on the west wall
    #   y 6       its ceiling (whisperwood planks inside a polished band)
    #   y 7..10   the lower shaft through thick masonry, polished course
    #             at y 10; a brick-slab ledge at y 11 where the tower steps in
    #   y 11..22  the upper shaft (outer radius 3): glass slits and two
    #             sculk-sensor "listening" windows (the old posts' echo),
    #             polished bands at 16 and 22
    #   y 22      upside-down brick-stair corbels carrying the gallery,
    #             an echo lantern hung under each diagonal corbel
    #   y 23      the gallery floor (radius 4), whisperwood-fence rail at 24
    #   y 24..27  the lantern room: a 5x5 ring of cyan glass on a chiseled
    #             sill between four polished mullions; the ladder arrives
    #             through its west side, a door opens east onto the gallery;
    #             the lamp on a chiseled pedestal at its centre
    #   y 28..31  an octagonal roof of brick stairs, a resonant-crystal
    #             finial and an iron-chain spike
    # Sculk grows up from the base (blocks in the walls, veins on the
    # outside faces), thinning with height; cracked bricks throughout (and
    # the degradation processor cracks more per placement).
    w, h, c = LIGHTHOUSE_W, LIGHTHOUSE_H, LIGHTHOUSE_C
    p = Piece("hush_lighthouse", "lighthouse", w, h, w)
    rng = piece_rng(p.location)
    p.fill(0, 0, 0, w - 1, h - 1, w - 1, "air")

    def octagon(x: int, z: int, r: int, chamfer: int) -> bool:
        dx, dz = x - c, z - c
        return abs(dx) <= r and abs(dz) <= r and abs(dx) + abs(dz) <= 2 * r - chamfer

    def out_a(x, z): return octagon(x, z, 4, 2)     # keeper's room / lower tower
    def in_a(x, z):  return octagon(x, z, 3, 2)
    def out_b(x, z): return octagon(x, z, 3, 2)     # upper tower
    def in_b(x, z):  return octagon(x, z, 2, 1)     # the shaft

    # The gallery: the lower tower's octagon, its outline railed. A chamfer's
    # outline steps DIAGONALLY, and fences only join orthogonally (and a
    # player slips between two diagonal posts), so each step's outer notch
    # joins the gallery as a rail cell: the rail is one closed 4-connected
    # loop round a walkway one block wide.
    orth = ((1, 0), (-1, 0), (0, 1), (0, -1))
    gallery_cells = {(x, z) for x in range(w) for z in range(w) if out_a(x, z)}
    rail = {(x, z) for (x, z) in gallery_cells
            if any((x + dx, z + dz) not in gallery_cells for dx, dz in orth)}
    outline = frozenset(rail)                        # notches never beget notches
    for x in range(w):
        for z in range(w):
            if (x, z) in gallery_cells:
                continue
            if (any((x + dx, z) in outline for dx in (1, -1)) and
                    any((x, z + dz) in outline for dz in (1, -1))):
                rail.add((x, z))
    gallery_cells |= rail

    def gallery(x, z): return (x, z) in gallery_cells

    def weathered(y: int) -> str:
        # Sculk thins from ~45 % at the foot to nothing by y 15; ~16 % of
        # what is left is cracked.
        creep = max(0.0, 0.45 * (1.0 - (y - 1) / 14.0))
        roll = rng.random()
        if roll < creep:
            return "sculk"
        return "cracked" if roll < creep + 0.16 else "bricks"

    def toward_centre(x: int, z: int) -> str:
        dx, dz = x - c, z - c
        if abs(dx) >= abs(dz):
            return "west" if dx > 0 else "east"
        return "north" if dz > 0 else "south"

    cells = [(x, z) for x in range(w) for z in range(w)]
    ladder_x, ladder_z = c - 2, c                    # against the west wall of the shaft

    # ── ground ────────────────────────────────────────────────────────────
    for x, z in cells:
        r2 = (x - c) ** 2 + (z - c) ** 2
        if out_a(x, z):
            p.set(x, 0, z, "polished" if not in_a(x, z) or rng.random() < 0.75 else "sculk")
        elif r2 <= 30:
            p.set(x, 0, z, "sculk" if rng.random() < 0.6 else "loam")
        else:
            p.set(x, 0, z, "loam")
    p.set(c, 0, w - 1, "polished")                   # the step outside the door

    # ── the keeper's room, y 1..5 ─────────────────────────────────────────
    for y in range(1, 6):
        for x, z in cells:
            if out_a(x, z) and not in_a(x, z):
                p.set(x, y, z, "polished" if y == 1 else weathered(y))
    p.fill(ladder_x - 1, 1, ladder_z, ladder_x - 1, 5, ladder_z, "polished")   # the ladder's pilaster
    p.fill(c, 1, w - 2, c, 2, w - 2, "air")          # south door
    p.set(c, 3, w - 2, "chiseled")
    for x, y, z in ((c, 3, 1), (w - 2, 3, c), (1, 3, c - 1)):   # N, E, W (beside the pilaster)
        p.set(x, y, z, "glass")
    p.chest(c + 3, 1, c - 1, "west", "hush_lighthouse/chest", table=LIGHTHOUSE_LOOT)
    p.set(c + 3, 1, c + 1, "ww_stairs", facing="east")           # a chair against the east wall
    p.set(c + 1, 1, c + 3, "lectern", facing="north")
    p.fill(c - 2, 1, c + 2, c - 2, 2, c + 2, "bookshelf")
    p.set(c - 2, 5, c - 2, "cobweb")
    p.set(c + 2, 5, c + 2, "cobweb")
    p.lantern(c, 5, c)

    # ── the room's ceiling, y 6 ───────────────────────────────────────────
    for x, z in cells:
        if out_a(x, z):
            p.set(x, 6, z, "planks" if in_a(x, z) else "polished")

    # ── the lower shaft, y 7..10, and the ledge at 11 ─────────────────────
    for y in range(7, 11):
        for x, z in cells:
            if not out_a(x, z) or in_b(x, z):
                continue
            if y == 10 and not in_a(x, z):
                p.set(x, y, z, "polished")
            else:
                p.set(x, y, z, weathered(y) if not in_a(x, z) else
                      ("cracked" if rng.random() < 0.2 else "bricks"))
    for x, z in cells:
        if out_a(x, z) and not out_b(x, z):
            p.set(x, 11, z, "brick_slab")

    # ── the upper shaft, y 11..22 ─────────────────────────────────────────
    for y in range(11, 23):
        for x, z in cells:
            if out_b(x, z) and not in_b(x, z):
                p.set(x, y, z, "polished" if y in (16, 22) else weathered(y))
    for y in (13, 14):                               # glass slits, north and south
        p.set(c, y, 2, "glass")
        p.set(c, y, w - 3, "glass")
    # The listening windows: a sculk sensor in the sill, open above it.
    p.set(w - 3, 14, c, "sensor")
    p.set(w - 3, 15, c, "air")
    p.set(c, 19, 2, "sensor")
    p.set(c, 20, 2, "air")

    # ── the gallery: corbels at 22, floor at 23, rail at 24 ───────────────
    for x, z in cells:
        if gallery(x, z) and not out_b(x, z):
            p.set(x, 22, z, "brick_stairs", facing=toward_centre(x, z), half="top")
    for dx, dz in ((-3, -3), (3, -3), (-3, 3), (3, 3)):
        p.lantern(c + dx, 21, c + dz)                # hung under the diagonal corbels
    for x, z in cells:
        if gallery(x, z):
            p.set(x, 23, z, "polished")
    p.set(c, 23, c, "chiseled")
    for x, z in rail:
        p.set(x, 24, z, "fence")

    # ── the lantern room, y 24..27 ────────────────────────────────────────
    for x, z in cells:
        dx, dz = x - c, z - c
        if max(abs(dx), abs(dz)) != 2:
            continue
        if abs(dx) == 2 and abs(dz) == 2:
            p.fill(x, 24, z, x, 27, z, "polished")   # mullions
            continue
        p.set(x, 24, z, "chiseled")
        p.fill(x, 25, z, x, 27, z, "glass")
    p.fill(c - 2, 24, c, c - 2, 25, c, "air")        # where the ladder arrives
    p.fill(c + 2, 24, c, c + 2, 25, c, "air")        # the door onto the gallery
    p.set(c, 24, c, "chiseled")                      # the lamp's pedestal
    p.set(c, LIGHTHOUSE_LAMP_Y, c, "lamp",
          nbt=T_comp({"id": T_str(block_id("lamp"))}))

    # ── the roof, y 28..31 ────────────────────────────────────────────────
    for x, z in cells:
        dx, dz = x - c, z - c
        ring = max(abs(dx), abs(dz))
        if ring == 3 and out_b(x, z):
            p.set(x, 28, z, "brick_stairs", facing=toward_centre(x, z))   # the eave
        elif ring <= 2:
            p.set(x, 28, z, "polished")
        if ring == 2:
            if abs(dx) == 2 and abs(dz) == 2:
                p.set(x, 29, z, "polished")
            else:
                p.set(x, 29, z, "brick_stairs", facing=toward_centre(x, z))
        elif ring <= 1:
            p.set(x, 29, z, "polished")
        if ring == 1:
            p.set(x, 30, z, "brick_slab")
    p.set(c, 30, c, "crystal")                       # the glowing finial
    p.set(c, 31, c, "chain", axis="y")

    # ── the ladder, y 1..23, through the room ceiling and the gallery ─────
    for y in range(1, 24):
        p.set(ladder_x, y, ladder_z, "ladder", facing="east")

    # ── the lurker: an echo mimic at the foot of the dark shaft, on the
    # keeper's-room ceiling, facing the ladder. It follows whoever climbs;
    # the keeper's lantern below the planks is near enough to weaken it
    # (double damage within 4) but not to burn it (within 2).
    p.entity(c + 1.5, 7.0, c - 0.5, "minecraft:echo_mimic", yaw=90.0)

    # ── sculk veins creeping up the outside of the lower tower ────────────
    face_of = {(1, 0): "east", (-1, 0): "west", (0, 1): "south", (0, -1): "north"}
    for y in range(1, 10):
        chance = 0.55 * (1.0 - (y - 1) / 9.0)
        for x, z in cells:
            if out_a(x, z) or p.key_at(x, y, z) != "air":
                continue
            if (x, z) == (c, w - 1):
                continue                             # keep the doorstep clear
            faces = [side for (dx, dz), side in face_of.items()
                     if out_a(x + dx, z + dz) and p.key_at(x + dx, y, z + dz) not in ("air", "glass")]
            if y == 1:
                faces.append("down")
            if faces and rng.random() < chance:
                p.set(x, y, z, "vein", faces=faces)
    return p


# ── the Warden's Tomb ────────────────────────────────────────────────────────
TOMB_CENTER = (13, 22)
TOMB_RADIUS = 12.0
TOMB_HEIGHT = 11.5


def build_tomb() -> Piece:
    # One buried piece, 27x14x45, plus the ladder shaft on a jigsaw.
    #   z 1..6   entrance hall (x 10..16) with the ladder shaft over it
    #   z 7..9   corridor into the dome, framed in reinforced deepslate
    #   z 9..35  the arena: an ellipsoidal dome, radius 12, 12 high, shell of
    #            hushstone bricks with reinforced-deepslate ribs on the axes
    #            and diagonals; polished floor with deepslate-tile rings; at
    #            the centre a two-step dais with the echo core on top (the
    #            Silent Warden wakes when it breaks); soul lanterns on the
    #            dais and on four plinths; echo lanterns on chains from the
    #            dome
    #   z 35..44 the treasure room behind the arena (only reachable through
    #            it): chests, a resonite-block plinth with an echo lantern
    # Everything outside the rooms is hushstone (the piece is a full box and
    # the structure buries it).
    w, h, l = 27, 14, 45
    cx, cz = TOMB_CENTER
    p = Piece("wardens_tomb", "tomb", w, h, l)
    rng = piece_rng(p.location)
    dome = set()
    for x in range(w):
        for z in range(l):
            for y in range(1, h):
                if ((x - cx) ** 2 + (z - cz) ** 2) / TOMB_RADIUS ** 2 + ((y - 1) / TOMB_HEIGHT) ** 2 <= 1.0:
                    dome.add((x, y, z))
    hall = box(10, 1, 2, 16, 4, 6)
    shaft = box(12, 5, 2, 14, 13, 4)
    # Corridor and passage run two blocks into the dome: at its z extremes
    # the ellipsoid is a single cell wide.
    corridor = box(12, 1, 7, 14, 3, 11)
    passage = box(12, 1, 33, 14, 3, 37)
    treasure = box(8, 1, 38, 18, 5, 43)
    interior = dome | hall | shaft | corridor | passage | treasure
    p.shell(interior, wall="bricks", mass="stone")
    # Ribs on the dome shell.
    for (x, y, z), (key, _props, _nbt) in list(p.cells.items()):
        if key != "bricks" or not (9 <= z <= 35) or y < 1:
            continue
        if (x, y, z) in interior:
            continue
        dx, dz = x - cx, z - cz
        if dx == 0 or dz == 0 or abs(dx) == abs(dz):
            p.set(x, y, z, "reinforced")
    # Arena floor.
    for x in range(w):
        for z in range(9, 36):
            if (x, 1, z) not in dome:
                continue
            d = math.hypot(x - cx, z - cz)
            if 6 <= d < 7 or 10 <= d < 11:
                p.set(x, 0, z, "tiles")
            elif d >= 11.5:
                p.set(x, 0, z, "reinforced")
            else:
                p.set(x, 0, z, "sculk" if rng.random() < 0.12 else "polished")
    # Door frames.
    for z in (9, 35):
        p.fill(11, 1, z, 11, 4, z, "reinforced")
        p.fill(15, 1, z, 15, 4, z, "reinforced")
        p.fill(12, 4, z, 14, 4, z, "reinforced")
    # Dais and the echo core.
    p.fill(cx - 2, 1, cz - 2, cx + 2, 1, cz + 2, "polished")
    p.fill(cx - 1, 2, cz - 1, cx + 1, 2, cz + 1, "chiseled")
    p.set(cx, 3, cz, "core")
    for x, z in ((cx - 2, cz - 2), (cx + 2, cz - 2), (cx - 2, cz + 2), (cx + 2, cz + 2)):
        p.soul_lantern(x, 2, z, hanging=False)
    # Soul lanterns on four plinths round the arena.
    for x, z in ((cx + 7, cz + 7), (cx - 7, cz + 7), (cx - 7, cz - 7), (cx + 7, cz - 7)):
        p.set(x, 1, z, "chiseled")
        p.soul_lantern(x, 2, z, hanging=False)
    # Echo lanterns on chains from the dome, eight on a ring of radius 7.
    for k in range(8):
        a = k * math.pi / 4
        x = cx + int(round(7 * math.cos(a)))
        z = cz + int(round(7 * math.sin(a)))
        roof = max(y for y in range(1, h) if (x, y, z) in dome)
        p.set(x, roof, z, "chain", axis="y")
        p.set(x, roof - 1, z, "chain", axis="y")
        p.lantern(x, roof - 2, z)
    # Entrance hall: polished floor, a soul lantern, the ladder up the north
    # wall of the shaft (z 1 is solid all the way up), the shaft link.
    p.fill(10, 0, 2, 16, 0, 6, "polished")
    p.soul_lantern(15, 4, 5)
    for y in range(1, 14):
        p.set(13, y, 2, "ladder", facing="south")
    p.jigsaw(13, 13, 3, "up", name=TOMB_SHAFT_LINK, target=TOMB_SHAFT_LINK,
             pool=POOL_TOMB_SHAFT, final_state="minecraft:air", joint="aligned")
    # Treasure room.
    p.fill(8, 0, 38, 18, 0, 43, "polished")
    for x in range(7, 20):
        for z in range(37, 45):
            if (x, 3, z) not in interior and p.key_at(x, 3, z) == "bricks":
                p.set(x, 3, z, "chiseled")
    p.fill(11, 1, 37, 11, 4, 37, "reinforced")
    p.fill(15, 1, 37, 15, 4, 37, "reinforced")
    p.fill(12, 4, 37, 14, 4, 37, "reinforced")
    p.set(cx, 1, 43, "resonite")
    p.lantern(cx, 2, 43, hanging=False)
    p.chest(10, 1, 43, "north", "wardens_tomb/chest_a", table=TOMB_LOOT)
    p.chest(16, 1, 43, "north", "wardens_tomb/chest_b", table=TOMB_LOOT)
    p.chest(8, 1, 40, "east", "wardens_tomb/chest_c", table=TOMB_LOOT)
    p.set(8, 1, 38, "catalyst")
    p.set(18, 1, 38, "catalyst")
    p.soul_lantern(10, 5, 40)
    p.soul_lantern(16, 5, 40)
    p.set(18, 5, 43, "cobweb")
    p.set(8, 5, 43, "cobweb")
    return p


def build_tomb_shaft() -> Piece:
    # 5x4x5 collar with a 3x3 open core and the ladder continuing up its
    # north wall; sits on the tomb's top row and reaches about two blocks
    # above the local surface (start_height -15 puts the tomb's top 2 below
    # the first free block). Joint aligned so the ladder keeps its wall.
    p = Piece("wardens_tomb", "shaft", 5, 4, 5)
    p.shell(box(1, 0, 1, 3, 3, 3), wall="bricks", mass="bricks")
    for x, z in ((0, 0), (4, 0), (0, 4), (4, 4)):
        p.set(x, 3, z, "chiseled")
    for y in range(0, 4):
        p.set(2, y, 1, "ladder", facing="south")
    p.jigsaw(2, 0, 2, "down", name=TOMB_SHAFT_LINK, target=TOMB_SHAFT_LINK, pool="minecraft:empty",
             final_state="minecraft:air", joint="aligned")
    return p


# ── the Sunken Library ───────────────────────────────────────────────────────
def build_library() -> Piece:
    # One piece, 17x11x13, set 4 blocks into the ground (start_height -4 on
    # WORLD_SURFACE_WG: over the Sunken Choir's water its floor is 4 under
    # the water line). The lower hall (y 1..4) is flooded: submerged
    # bookshelves line the walls and stand in two broken rows, kelp and sea
    # pickles grow from a silted floor, one chest lies in the water. A
    # whisperwood gallery (y 5) runs round the walls above the water with a
    # fence rail, three lecterns and a dry chest; more shelves line the upper
    # walls. The door is on the north wall at gallery level (the brick
    # threshold is one block above the water line, so a swimmer can climb
    # in). The ceiling has fallen through in three places and the east wall
    # is broken at the top. Echo lanterns hang from what is left.
    w, h, l = 17, 11, 13
    p = Piece("sunken_library", "library", w, h, l)
    rng = piece_rng(p.location)
    interior = box(1, 1, 1, w - 2, h - 2, l - 2)
    p.shell(interior, wall="bricks", mass="bricks")
    # Open well over the water; the gallery round it is three wide: shelves
    # on the outer row, the walkway, the rail on the inner row.
    atrium = {(x, z) for x in range(4, w - 4) for z in range(4, l - 4)}

    def wall_adjacent(x: int, z: int) -> bool:
        return x in (1, w - 2) or z in (1, l - 2)

    # Silted floor.
    for x in range(1, w - 1):
        for z in range(1, l - 1):
            p.set(x, 0, z, "sculk" if rng.random() < 0.25 else "loam" if rng.random() < 0.2 else "polished")
    # Shelves along the lower walls (under the gallery) and upper walls.
    for x in range(1, w - 1):
        for z in range(1, l - 1):
            if not wall_adjacent(x, z):
                continue
            for y in range(1, 5):
                if rng.random() < 0.9:
                    p.set(x, y, z, "bookshelf")
            for y in range(6, 9):
                p.set(x, y, z, "bookshelf")
    # Two broken shelf rows in the flooded atrium.
    for x in (6, w - 7):
        for z in range(5, l - 5):
            top = rng.randint(1, 3)
            for y in range(1, top + 1):
                p.set(x, y, z, "bookshelf")
    # Gallery floor and its rail.
    for x in range(1, w - 1):
        for z in range(1, l - 1):
            if (x, z) not in atrium:
                p.set(x, 5, z, "planks")
    for x in range(1, w - 1):
        for z in range(1, l - 1):
            if (x, z) in atrium:
                continue
            if any((x + dx, z + dz) in atrium for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                p.set(x, 6, z, "fence")
    # The three gallery lecterns still hold the Listeners' open books
    # (LIBRARY_LECTERN_BOOKS; MC's LecternBlockEntity Book + Page nbt).
    for (x, y, z, facing), book in zip(((3, 6, 6, "east"), (w - 4, 6, 6, "west"), (8, 6, l - 4, "north")),
                                       LIBRARY_LECTERN_BOOKS):
        p.set(x, y, z, "lectern", nbt=lectern_book_nbt(book), facing=facing, has_book=True)
    # Door on the north wall at gallery level, over the brick threshold.
    p.fill(7, 6, 0, 9, 8, 0, "air")
    p.fill(7, 6, 1, 9, 8, 1, "air")                  # through the upper shelves
    # Windows in the upper east and west walls.
    for z in (4, 8):
        for x in (0, w - 1):
            p.fill(x, 7, z, x, 8, z, "air")
            p.fill(x + (1 if x == 0 else -1), 7, z, x + (1 if x == 0 else -1), 8, z, "air")
    # Collapse: three holes in the ceiling, a broken top on the east wall.
    for cx, cz in ((5, 4), (10, 7), (12, 3)):
        p.fill(cx, h - 1, cz, cx + 1, h - 1, cz + 1, "air")
    p.fill(w - 1, 8, 5, w - 1, 9, 7, "air")
    # Lanterns.
    p.lantern(5, h - 2, 8)
    p.lantern(11, h - 2, 5)
    p.soul_lantern(2, 6, 2, hanging=False)
    p.soul_lantern(w - 3, 6, l - 3, hanging=False)
    # Chests: one dry on the gallery, two in the water.
    p.chest(w - 3, 6, 2, "west", "sunken_library/chest_gallery", table=LIBRARY_LOOT)
    p.chest(8, 1, 6, "south", "sunken_library/chest_atrium", table=LIBRARY_LOOT, waterlogged=True)
    p.chest(2, 1, l - 3, "east", "sunken_library/chest_corner", table=LIBRARY_LOOT, waterlogged=True)
    # Kelp and pickles in the atrium, then everything still open below the
    # water line floods.
    free = [(x, z) for (x, z) in sorted(atrium) if p.key_at(x, 1, z) == "air"]
    rng.shuffle(free)
    for x, z in free[:6]:
        top = rng.randint(2, 4)
        for y in range(1, top):
            p.set(x, y, z, "kelp_plant")
        p.set(x, top, z, "kelp")
    for x, z in free[6:11]:
        p.set(x, 1, z, "pickle", pickles=rng.randint(1, 3))
    for x in range(1, w - 1):
        for z in range(1, l - 1):
            for y in range(1, 5):
                if p.key_at(x, y, z) == "air":
                    p.set(x, y, z, "water")
    # A Hush leviathan drifting over the drowned choir's library, twenty-odd
    # blocks above the water line (the loader places an entity wherever its
    # block falls in the decorating chunk, above the template as well). It
    # roams from here as any leviathan does; kept, so the first visit finds it.
    p.entity(w / 2, 26.0, l / 2, "minecraft:hush_leviathan", yaw=135.0)
    return p


# ── template pools ───────────────────────────────────────────────────────────
def pool_json(entries: list[tuple[str, int]], fallback: str, processors: str) -> dict:
    return {
        "elements": [
            {
                "element": {
                    "element_type": "minecraft:single_pool_element",
                    "location": location,
                    "processors": processors,
                    "projection": "rigid",
                },
                "weight": weight,
            }
            for location, weight in entries
        ],
        "fallback": fallback,
    }


def vault_pool(entries: list[tuple[str, int]], fallback: str) -> dict:
    return pool_json([(f"minecraft:echo_vaults/{n}", w) for n, w in entries], fallback, PROCESSORS)


# The single-piece structures (and the tomb's shaft): folder -> pool name ->
# (entries, fallback, processors).
EXTRA_POOLS = {
    "choir_hall": {"start": ([("minecraft:choir_hall/hall", 1)], "minecraft:empty", "minecraft:empty")},
    # The lighthouse takes the vaults' degradation (20 % of its bricks crack
    # per placement), so no two stand quite alike.
    "hush_lighthouse": {"start": ([("minecraft:hush_lighthouse/lighthouse", 1)], "minecraft:empty",
                                  PROCESSORS)},
    "wardens_tomb": {
        "start": ([("minecraft:wardens_tomb/tomb", 1)], "minecraft:empty", PROCESSORS),
        "shaft": ([("minecraft:wardens_tomb/shaft", 1)], "minecraft:empty", PROCESSORS),
    },
    "sunken_library": {"start": ([("minecraft:sunken_library/library", 1)], "minecraft:empty",
                                 PROCESSORS)},
}


def build_extra() -> list[Piece]:
    return [build_choir_hall(), build_hush_lighthouse(), build_tomb(), build_tomb_shaft(),
            build_library()]


VAULT_POOLS = {
    "center": ([("center_1", 1)], "minecraft:empty"),
    "halls": ([("hall_straight", 3), ("hall_arch", 2), ("hall_bend", 2)], POOL_ENDS),
    "hall_next": ([("room_reliquary", 3), ("room_garden", 3), ("hall_straight", 2),
                   ("hall_arch", 1), ("hall_bend", 2)], POOL_ENDS),
    "ends": ([("end_cap", 1)], "minecraft:empty"),
    "shaft": ([("shaft", 1)], "minecraft:empty"),
}


# ── dump / cross-check ───────────────────────────────────────────────────────
def read_template(path: Path) -> dict:
    _, root = parse(gzip.decompress(path.read_bytes()))
    return root.v


def dump(path: Path) -> None:
    d = read_template(path)
    size = d["size"].v[1]
    palette = d["palette"].v[1]
    blocks = d["blocks"].v[1]
    print(f"{path.parent.name}/{path.name}: size {size[0]}x{size[1]}x{size[2]}, {len(blocks)} blocks, "
          f"{len(palette)} palette entries, DataVersion {d['DataVersion'].v}, "
          f"entities {len(d['entities'].v[1])}")
    for i, entry in enumerate(palette):
        props = entry.get("Properties")
        props_s = "" if props is None else " " + json.dumps({k: v.v for k, v in props.v.items()}, sort_keys=True)
        print(f"   palette[{i}] {entry['Name'].v}{props_s}")
    for b in blocks:
        if "nbt" not in b:
            continue
        nbt = b["nbt"].v
        pos = b["pos"].v[1]
        pal = palette[b["state"].v]
        name = pal["Name"].v
        if name == "minecraft:jigsaw":
            orientation = pal["Properties"].v["orientation"].v
            print(f"   jigsaw @{tuple(pos)} {orientation:<10} front={FRONT_OF[orientation]:<5} "
                  f"name={nbt['name'].v} target={nbt['target'].v} pool={nbt['pool'].v} "
                  f"joint={nbt['joint'].v} final={nbt['final_state'].v}")
        elif name == "minecraft:chest":
            facing = pal["Properties"].v["facing"].v
            if "LootTable" in nbt:
                print(f"   chest @{tuple(pos)} facing {facing}: LootTable {nbt['LootTable'].v}")
                continue
            items = nbt["Items"].v[1]
            desc = ", ".join(
                f"{i['Slot'].v}:{i['id'].v.split(':')[1]}x{i['count'].v}"
                + ("(" + ",".join(f"{k.split(':')[1]}{v.v}" for k, v in i['components'].v['minecraft:stored_enchantments'].v.items()) + ")"
                   if 'components' in i else "")
                for i in items)
            print(f"   chest  @{tuple(pos)} facing={facing} items=[{desc}]")
        else:
            print(f"   nbt    @{tuple(pos)} {name} keys={list(nbt)}")


REGISTER_RE = re.compile(
    r'(?:create\w*Block|registerBlock|setId)\(\s*"(minecraft:[a-z0-9_]+)"')
ID_RE = re.compile(r'"(minecraft:[a-z0-9_]+)"')


def registered_blocks() -> set[str]:
    """Block ids the terrain library registers: every create*Block /
    registerBlock / setId call in Blocks.cpp, plus the ids in the brace
    initializer lists its `for (const char* x : {...})` registration loops
    iterate (reinforced_deepslate, chiseled_deepslate, ... live there)."""
    text = BLOCKS_CPP.read_text(encoding="utf-8", errors="replace")
    known = set(REGISTER_RE.findall(text))
    for m in re.finditer(r':\s*\{(.*?)\}\s*\)\s*\{', text, re.S):
        known.update(ID_RE.findall(m.group(1)))
    return known


def check_blocks(paths: list[Path]) -> int:
    known = registered_blocks()
    print(f"\nBlocks.cpp registers {len(known)} block ids")
    missing = 0
    for path in paths:
        d = read_template(path)
        for entry in d["palette"].v[1]:
            name = entry["Name"].v
            if name not in known:
                print(f"   MISSING {name} (used by {path.parent.name}/{path.name})")
                missing += 1
    print("block check: " + ("all palette blocks registered" if missing == 0
                             else f"{missing} unregistered palette entr{'y' if missing == 1 else 'ies'}"))
    return missing


def main() -> int:
    global STAND_INS, BAKE_ITEMS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vanilla-stand-ins", action="store_true",
                    help="use vanilla block names instead of the Hush blocks")
    ap.add_argument("--no-shaft", action="store_true",
                    help="seal the vault ceiling; no sinkhole shaft piece or pool")
    ap.add_argument("--dump", action="store_true", help="re-read the written templates and print them")
    ap.add_argument("--check-blocks", action="store_true",
                    help="cross-check every palette block against Blocks.cpp registrations; exit 1 on a miss")
    ap.add_argument("--bake-items", action="store_true",
                    help="write fixed Items into chests instead of a LootTable key")
    ap.add_argument("--outdir", type=Path, default=REPO_ROOT / "data",
                    help="data root to write under (default: the repo's data/)")
    args = ap.parse_args()
    BAKE_ITEMS = args.bake_items
    STAND_INS = args.vanilla_stand_ins

    def rel(path: Path) -> str:
        return str(path.relative_to(REPO_ROOT)) if path.is_relative_to(REPO_ROOT) else str(path)

    with_shaft = not args.no_shaft
    written: list[Path] = []
    for piece in build_vaults(with_shaft) + [build_shrine()] + build_extra():
        struct_dir = args.outdir / "minecraft" / "structure" / piece.folder
        struct_dir.mkdir(parents=True, exist_ok=True)
        path = struct_dir / f"{piece.name}.nbt"
        path.write_bytes(piece.to_nbt())
        written.append(path)
        print(f"wrote {rel(path)} ({piece.size[0]}x{piece.size[1]}x{piece.size[2]}, {len(piece.cells)} blocks)")
    vault_dir = args.outdir / "minecraft" / "structure" / "echo_vaults"
    if not with_shaft:
        stale = vault_dir / "shaft.nbt"
        if stale.exists():
            stale.unlink()
            print(f"removed {stale}")

    pool_root = args.outdir / "minecraft" / "worldgen" / "template_pool"
    vault_pool_dir = pool_root / "echo_vaults"
    vault_pool_dir.mkdir(parents=True, exist_ok=True)
    for name, (entries, fallback) in VAULT_POOLS.items():
        path = vault_pool_dir / f"{name}.json"
        if name == "shaft" and not with_shaft:
            if path.exists():
                path.unlink()
                print(f"removed {path}")
            continue
        path.write_text(json.dumps(vault_pool(entries, fallback), indent=2) + "\n", encoding="utf-8")
        print(f"wrote {rel(path)}")
    shrine_pool_dir = pool_root / "whisper_shrine"
    shrine_pool_dir.mkdir(parents=True, exist_ok=True)
    path = shrine_pool_dir / "start.json"
    path.write_text(json.dumps(pool_json([("minecraft:whisper_shrine/shrine", 1)], "minecraft:empty",
                                         "minecraft:empty"), indent=2) + "\n", encoding="utf-8")
    print(f"wrote {rel(path)}")
    for folder, pools in EXTRA_POOLS.items():
        folder_dir = pool_root / folder
        folder_dir.mkdir(parents=True, exist_ok=True)
        for name, (entries, fallback, processors) in pools.items():
            path = folder_dir / f"{name}.json"
            path.write_text(json.dumps(pool_json(entries, fallback, processors), indent=2) + "\n",
                            encoding="utf-8")
            print(f"wrote {rel(path)}")

    if STAND_INS:
        print("NOTE: templates use vanilla stand-in blocks; re-run without "
              "--vanilla-stand-ins before shipping.")
    if args.dump:
        print()
        for path in written:
            dump(path)
    if args.check_blocks:
        return 1 if check_blocks(written) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
