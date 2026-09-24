#!/usr/bin/env python3
# tools/hush_creature_meshes.py
#
# The Hush creatures' meshes (docs/the-hush.md, "Creatures of the deep
# Hush"): the lumen moth, the crystal golem, the hush leviathan, the echo
# mimic, the Choir Mother and Aurelith's Unsung. Original designs with no MC or mod model class
# behind them, so they are written here as data rather than parsed out of a
# decompile.
#
# ONE definition, two consumers:
#   * tools/gen_entity_models.py turns every mesh into GeneratedEntityModels
#     rows (after the MC and mod meshes, so no existing index moves);
#   * tools/gen_hush_entity_textures.py paints each cube's UV box on the
#     sheet from the same packing, so a mesh edit can never leave the art
#     misaligned.
#
# Conventions are MC model space: pixels, Y DOWN, y = 24 on the ground (the
# renderer's -1.501 block offset puts model y 24 at the entity's feet at any
# modelScale), the face (-Z) forward. A cube is (origin, size) in its part's
# frame; a part's pose is (x, y, z, xRot, yRot, zRot) in its parent's frame.
# Zero-thickness cubes are MC's flat planes (bat wings, the allay's wings).
#
# Texture offsets are NOT written by hand: `pack` shelf-packs every cube's MC
# UV footprint (2*(sx+sz) wide, sz+sy tall — ModelPart.Cube's unwrap) in
# declaration order onto the sheet, and both generators read the result.
#
#   python3 tools/hush_creature_meshes.py     # print the packing report

from __future__ import annotations

import math
from dataclasses import dataclass, field


@dataclass
class Cube:
    o: tuple[float, float, float]           # origin, pixels (part frame)
    s: tuple[float, float, float]           # size, pixels
    mat: str                                # material key (texture painter)
    grow: float = 0.0                       # CubeDeformation, uniform
    # Per-face decorations for the painter: {"front": "eyes", ...}. Faces
    # are named as seen on the posed model: top, bottom, front (-Z), back,
    # right (-X side of the sheet's first column) and left.
    feats: dict[str, str] = field(default_factory=dict)
    # Filled by pack().
    tu: int = 0
    tv: int = 0


@dataclass
class Part:
    name: str
    parent: str | None                      # None = child of the root
    pose: tuple[float, float, float, float, float, float]
    cubes: list[Cube]


@dataclass
class Mesh:
    slug: str
    texw: int
    texh: int
    parts: list[Part]
    # The part the look direction turns (GenModel::headPart), "" for none.
    head: str = ""
    # Drawn back-face culled (GenModel::cull). Every Hush mesh has flat
    # planes (wings, veils, fins) that must show both sides, so none cull.
    cull: bool = False


def footprint(c: Cube) -> tuple[int, int]:
    """MC ModelPart.Cube's UV unwrap size for a cube: 2*(x+z) by z+y."""
    sx, sy, sz = (int(math.ceil(v)) for v in c.s)
    return 2 * (sx + sz), sz + sy


def pack(mesh: Mesh) -> None:
    """Shelf-pack every cube's footprint in declaration order; sets tu/tv."""
    x = y = shelf = 0
    for part in mesh.parts:
        for c in part.cubes:
            w, h = footprint(c)
            if w > mesh.texw:
                raise SystemExit(f"{mesh.slug}.{part.name}: cube {w}px wider than the "
                                 f"{mesh.texw}px sheet")
            if x + w > mesh.texw:
                x, y, shelf = 0, y + shelf, 0
            c.tu, c.tv = x, y
            x += w
            shelf = max(shelf, h)
    if y + shelf > mesh.texh:
        raise SystemExit(f"{mesh.slug}: packing needs {y + shelf}px, the sheet is "
                         f"{mesh.texh}px tall")


def face_rects(c: Cube) -> dict[str, tuple[int, int, int, int]]:
    """The six UV rectangles (u0, v0, u1, v1; exclusive end) of a packed cube,
    named as they appear on the posed model. MC's unwrap from texOffs (u, v)
    with size (x, y, z): top at (u+z, v) x-by-z, bottom beside it, then the
    band at v+z: right side (z wide), front (x), left side (z), back (x)."""
    sx, sy, sz = (int(math.ceil(v)) for v in c.s)
    u, v = c.tu, c.tv
    return {
        "top":    (u + sz,          v,      u + sz + sx,          v + sz),
        "bottom": (u + sz + sx,     v,      u + sz + 2 * sx,      v + sz),
        "right":  (u,               v + sz, u + sz,               v + sz + sy),
        "front":  (u + sz,          v + sz, u + sz + sx,          v + sz + sy),
        "left":   (u + sz + sx,     v + sz, u + 2 * sz + sx,      v + sz + sy),
        "back":   (u + 2 * sz + sx, v + sz, u + 2 * sz + 2 * sx,  v + sz + sy),
    }


# ── The meshes ──────────────────────────────────────────────────────────────

def _lumen_moth() -> Mesh:
    # A hand-sized moth (0.5 x 0.5 box) hovering with its thorax at y 19:
    # a furred thorax and head, feathered antennae, a luminous abdomen and
    # two pairs of flat wings (MC's zero-thickness wing planes, flapped about
    # Z by the render module).
    return Mesh("lumen_moth", 64, 32, head="head", parts=[
        Part("body", None, (0, 19, 0, 0, 0, 0), [
            Cube((-1.5, -1.5, -2), (3, 3, 4), "moth_fur")]),
        Part("head", "body", (0, -0.5, -2, 0, 0, 0), [
            Cube((-1.5, -1.5, -3), (3, 3, 3), "moth_fur", feats={"front": "moth_eyes"})]),
        Part("left_antenna", "head", (1, -1.5, -2.5, -0.5, 0, 0.35), [
            Cube((0, -4, -2), (0, 4, 2), "moth_antenna")]),
        Part("right_antenna", "head", (-1, -1.5, -2.5, -0.5, 0, -0.35), [
            Cube((0, -4, -2), (0, 4, 2), "moth_antenna")]),
        Part("abdomen", "body", (0, 0, 2, 0, 0, 0), [
            Cube((-1, -1, 0), (2, 2, 5), "moth_glow")]),
        Part("left_wing", "body", (1.5, -1.5, -1, 0, 0, 0), [
            Cube((0, 0, -2), (9, 0, 7), "moth_wing", feats={"top": "wing_eye"})]),
        Part("right_wing", "body", (-1.5, -1.5, -1, 0, 0, 0), [
            Cube((-9, 0, -2), (9, 0, 7), "moth_wing", feats={"top": "wing_eye"})]),
        Part("left_hind_wing", "body", (1.5, -1, 1.5, 0, 0, 0), [
            Cube((0, 0, 0), (7, 0, 5), "moth_hindwing")]),
        Part("right_hind_wing", "body", (-1.5, -1, 1.5, 0, 0, 0), [
            Cube((-7, 0, 0), (7, 0, 5), "moth_hindwing")]),
    ])


def _crystal_golem() -> Mesh:
    # Iron-golem-sized (1.4 x 2.7 box, 43 px tall): a hushstone body grown
    # through with resonant crystal — a crown, shoulder and back shards, and
    # crystal fists at the end of long arms.
    return Mesh("crystal_golem", 128, 128, head="head", parts=[
        Part("body", None, (0, -7, 0, 0, 0, 0), [
            Cube((-9, -2, -6), (18, 12, 11), "golem_stone", feats={"front": "golem_core"}),
            Cube((-5, 10, -4), (10, 8, 8), "golem_stone")]),
        Part("head", None, (0, -9, -2, 0, 0, 0), [
            Cube((-4, -10, -4), (8, 10, 8), "golem_stone", feats={"front": "golem_face"})]),
        Part("crown", "head", (0, -10, 0, 0, 0.785398, 0), [
            Cube((-1.5, -6, -1.5), (3, 6, 3), "crystal")]),
        Part("crown_left", "head", (2.5, -9, 1, 0, 0, -0.5), [
            Cube((-1, -4, -1), (2, 4, 2), "crystal")]),
        Part("crown_right", "head", (-2.5, -9, 1, 0, 0, 0.5), [
            Cube((-1, -4, -1), (2, 4, 2), "crystal")]),
        Part("left_shard", "body", (7, -2, 0, 0, 0, -0.35), [
            Cube((-2, -8, -2), (4, 8, 4), "crystal")]),
        Part("right_shard", "body", (-7, -2, 1, 0, 0, 0.4), [
            Cube((-2, -7, -2), (4, 7, 4), "crystal")]),
        Part("back_shard", "body", (0, 2, 5, 0.55, 0, 0), [
            Cube((-1.5, -9, -1.5), (3, 9, 3), "crystal")]),
        Part("right_arm", None, (-11, -7, 0, 0, 0, 0), [
            Cube((-4, -2, -3), (5, 26, 6), "golem_stone")]),
        Part("left_arm", None, (11, -7, 0, 0, 0, 0), [
            Cube((-1, -2, -3), (5, 26, 6), "golem_stone")]),
        Part("right_fist", "right_arm", (-1.5, 18, 0, 0, 0, 0), [
            Cube((-3.5, 0, -3.5), (6, 6, 7), "crystal", grow=0.3)]),
        Part("left_fist", "left_arm", (1.5, 18, 0, 0, 0, 0), [
            Cube((-2.5, 0, -3.5), (6, 6, 7), "crystal", grow=0.3)]),
        Part("right_leg", None, (-4, 11, 0, 0, 0, 0), [
            Cube((-3, 0, -3), (6, 13, 6), "golem_stone")]),
        Part("left_leg", None, (4, 11, 0, 0, 0, 0), [
            Cube((-3, 0, -3), (6, 13, 6), "golem_stone")]),
    ])


def _hush_leviathan() -> Mesh:
    # Drawn at modelScale 2 (HushCreatureRender): 51 px long and 24 px tall
    # natively, 6.4 x 3 blocks in the world. A whale-like body that tapers
    # into a three-segment tail and a flat fluke; pectoral fins, a dorsal
    # fin and glowing crystal nodes along the spine.
    return Mesh("hush_leviathan", 128, 128, parts=[
        Part("body", None, (0, 12, 0, 0, 0, 0), [
            Cube((-8, -8, -8), (16, 16, 16), "lev_hide",
                 feats={"bottom": "lev_belly", "left": "lev_lights", "right": "lev_lights"})]),
        Part("head", "body", (0, 1, -8, 0, 0, 0), [
            Cube((-7, -7, -8), (14, 13, 8), "lev_hide",
                 feats={"front": "lev_face", "bottom": "lev_belly",
                        "left": "lev_eye", "right": "lev_eye"})]),
        Part("snout", "head", (0, 1, -8, 0, 0, 0), [
            Cube((-5, -4, -3), (10, 8, 3), "lev_hide",
                 feats={"front": "lev_mouth", "bottom": "lev_belly"})]),
        Part("tail1", "body", (0, -1, 8, 0, 0, 0), [
            Cube((-6, -6, 0), (12, 12, 8), "lev_hide", feats={"bottom": "lev_belly"})]),
        Part("tail2", "tail1", (0, 0, 8, 0, 0, 0), [
            Cube((-4, -4, 0), (8, 8, 7), "lev_hide", feats={"bottom": "lev_belly"})]),
        Part("tail3", "tail2", (0, 0, 7, 0, 0, 0), [
            Cube((-2.5, -2.5, 0), (5, 5, 5), "lev_hide")]),
        Part("fluke", "tail3", (0, 0, 5, 0, 0, 0), [
            Cube((-9, -0.5, 0), (18, 1, 5), "lev_fin")]),
        Part("left_fin", "body", (8, 4, -3, 0, 0, 0), [
            Cube((0, -0.5, -3), (12, 1, 7), "lev_fin")]),
        Part("right_fin", "body", (-8, 4, -3, 0, 0, 0), [
            Cube((-12, -0.5, -3), (12, 1, 7), "lev_fin")]),
        Part("dorsal", "body", (0, -8, 1, 0, 0, 0), [
            Cube((-0.5, -5, -4), (1, 5, 9), "lev_fin")]),
        Part("head_node", "head", (0, -7, -4, 0, 0, 0), [
            Cube((-1, -2, -1), (2, 2, 2), "lev_glow")]),
        Part("tail_node", "tail1", (0, -6, 4, 0, 0, 0), [
            Cube((-1, -2, -1), (2, 2, 2), "lev_glow")]),
    ])


def _echo_mimic() -> Mesh:
    # A player-sized (0.6 x 1.95) figure of the one it copies: a humanoid of
    # player proportions with thin, over-long arms, a shimmer shell over the
    # head (MC's hat layer shape) and a wisp trailing from its back.
    return Mesh("echo_mimic", 64, 64, head="head", parts=[
        Part("head", None, (0, 0, 0, 0, 0, 0), [
            Cube((-4, -8, -4), (8, 8, 8), "mimic_skin", feats={"front": "mimic_face"}),
            Cube((-4, -8, -4), (8, 8, 8), "mimic_shell", grow=0.5)]),
        Part("body", None, (0, 0, 0, 0, 0, 0), [
            Cube((-4, 0, -2), (8, 12, 4), "mimic_skin", feats={"front": "mimic_chest"})]),
        Part("right_arm", None, (-5, 2, 0, 0, 0, 0), [
            Cube((-2, -2, -1.5), (3, 14, 3), "mimic_skin")]),
        Part("left_arm", None, (5, 2, 0, 0, 0, 0), [
            Cube((-1, -2, -1.5), (3, 14, 3), "mimic_skin")]),
        Part("right_leg", None, (-1.9, 12, 0, 0, 0, 0), [
            Cube((-2, 0, -2), (4, 12, 4), "mimic_skin")]),
        Part("left_leg", None, (1.9, 12, 0, 0, 0, 0), [
            Cube((-2, 0, -2), (4, 12, 4), "mimic_skin")]),
        Part("wisp", "body", (0, 8, 2, 0.3, 0, 0), [
            Cube((-3, 0, 0), (6, 8, 0), "mimic_wisp")]),
    ])


def _choir_mother() -> Mesh:
    # Drawn at modelScale 1.5: 40 px native, 3.75 blocks in the world. A
    # floating sculk matriarch — a slight torso with a glowing heart, a
    # veiled face under two tendril horns, conducting arms, a flared
    # four-panel veil for a skirt and six hanging tendrils beneath it.
    parts = [
        Part("torso", None, (0, 4, 0, 0, 0, 0), [
            Cube((-4, -6, -3), (8, 12, 6), "sculk_flesh", feats={"front": "mother_heart"})]),
        Part("head", None, (0, -2, 0, 0, 0, 0), [
            Cube((-3.5, -7, -3.5), (7, 7, 7), "sculk_flesh", feats={"front": "mother_face"})]),
        Part("face_veil", "head", (0, -3, -3.6, 0, 0, 0), [
            Cube((-3.5, 0, 0), (7, 9, 0), "mother_veil")]),
        Part("left_horn", "head", (3.5, -6, 0, 0, 0, -0.35), [
            Cube((0, -8, 0), (7, 8, 0), "sculk_tendril_glow")]),
        Part("right_horn", "head", (-3.5, -6, 0, 0, 0, 0.35), [
            Cube((-7, -8, 0), (7, 8, 0), "sculk_tendril_glow")]),
        Part("left_arm", None, (4.5, -1, 0, 0, 0, -0.2), [
            Cube((-1, -1, -1), (2, 10, 2), "sculk_flesh")]),
        Part("right_arm", None, (-4.5, -1, 0, 0, 0, 0.2), [
            Cube((-1, -1, -1), (2, 10, 2), "sculk_flesh")]),
        Part("left_forearm", "left_arm", (0, 9, 0, 0, 0, 0), [
            Cube((-1, 0, -1), (2, 9, 2), "sculk_tendril")]),
        Part("right_forearm", "right_arm", (0, 9, 0, 0, 0, 0), [
            Cube((-1, 0, -1), (2, 9, 2), "sculk_tendril")]),
        Part("veil_front", None, (0, 9, -3, -0.22, 0, 0), [
            Cube((-6, 0, 0), (12, 15, 0), "mother_veil")]),
        Part("veil_back", None, (0, 9, 3, 0.22, 0, 0), [
            Cube((-6, 0, 0), (12, 15, 0), "mother_veil")]),
        Part("veil_left", None, (4, 9, 0, 0, 0, -0.22), [
            Cube((0, 0, -5), (0, 15, 10), "mother_veil")]),
        Part("veil_right", None, (-4, 9, 0, 0, 0, 0.22), [
            Cube((0, 0, -5), (0, 15, 10), "mother_veil")]),
    ]
    for i in range(6):
        a = math.radians(30 + 60 * i)
        x, z = round(3.5 * math.cos(a), 4), round(3.5 * math.sin(a), 4)
        parts.append(Part(f"tendril_{i}", None, (x, 9, z, 0, 0, 0), [
            Cube((-1, 0, -1), (2, 8, 2), "sculk_tendril")]))
    for i in range(6):
        parts.append(Part(f"tendril_{i}_tip", f"tendril_{i}", (0, 8, 0, 0, 0, 0), [
            Cube((-0.5, 0, -0.5), (1, 7, 1), "sculk_tendril_glow")]))
    return Mesh("choir_mother", 128, 64, head="head", parts=parts)


def _the_unsung() -> Mesh:
    # Aurelith's boss (common/entity/mobs/TheUnsung.hpp), drawn at modelScale
    # 1.6: 39 px native, ~3.9 blocks in the world (a 128 x 128 sheet). The Undersong given a
    # body in the only shape it ever heard: a conductor's. A gaunt robed
    # figure of layered sculk plates over a dark robe that flares into
    # hanging plates and tendrils; a pale blank mask (the statues' "blank so
    # anyone may sing in it") split by a crack of light, under a sculk hood
    # and two swept crest tendrils; a HOLLOW crystal ring where a throat
    # should be (four bars round an empty core — you see through it); long
    # arms, the right hand holding a dark-crystal baton. Four voice shards —
    # the voices it learned — orbit it (the render module places them).
    parts = [
        Part("robe", None, (0, 7, 0, 0, 0, 0), [
            Cube((-4.5, 0, -3), (9, 15, 6), "unsung_robe", feats={"front": "unsung_robe_notes"})]),
        Part("robe_front", None, (0, 7, -3.1, -0.24, 0, 0), [
            Cube((-6, 0, 0), (12, 15, 0), "unsung_robe_plate")]),
        Part("robe_back", None, (0, 7, 3.1, 0.24, 0, 0), [
            Cube((-6, 0, 0), (12, 15, 0), "unsung_robe_plate")]),
        Part("robe_left", None, (4.6, 7, 0, 0, 0, -0.24), [
            Cube((0, 0, -4.5), (0, 15, 9), "unsung_robe_plate")]),
        Part("robe_right", None, (-4.6, 7, 0, 0, 0, 0.24), [
            Cube((0, 0, -4.5), (0, 15, 9), "unsung_robe_plate")]),
        Part("mantle_front", None, (0, 6, -3.6, -0.36, 0, 0), [
            Cube((-5, 0, 0), (10, 8, 0), "unsung_mantle")]),
        Part("mantle_back", None, (0, 6, 3.6, 0.36, 0, 0), [
            Cube((-5, 0, 0), (10, 8, 0), "unsung_mantle")]),
        Part("mantle_left", None, (5.0, 6, 0, 0, 0, -0.36), [
            Cube((0, 0, -4.5), (0, 8, 9), "unsung_mantle")]),
        Part("mantle_right", None, (-5.0, 6, 0, 0, 0, 0.36), [
            Cube((0, 0, -4.5), (0, 8, 9), "unsung_mantle")]),
        Part("body", None, (0, -4, 0, 0, 0, 0), [
            Cube((-4.5, 0, -2.5), (9, 11, 5), "unsung_plate", feats={"front": "unsung_chest"})]),
        # The hollow throat: four crystal bars round an open core.
        Part("throat", "body", (0, 0, 0, 0, 0, 0), [
            Cube((-2, -4, -2), (4, 4, 1), "unsung_throat"),
            Cube((-2, -4, 1), (4, 4, 1), "unsung_throat"),
            Cube((1, -4, -1), (1, 4, 2), "unsung_throat"),
            Cube((-2, -4, -1), (1, 4, 2), "unsung_throat")]),
        Part("left_pauldron", "body", (4.5, 0.5, 0, 0, 0, -0.35), [
            Cube((0, -1, -3), (4, 2, 6), "unsung_plate")]),
        Part("right_pauldron", "body", (-4.5, 0.5, 0, 0, 0, 0.35), [
            Cube((-4, -1, -3), (4, 2, 6), "unsung_plate")]),
        Part("head", None, (0, -8, 0, 0, 0, 0), [
            Cube((-3, -7, -3), (6, 7, 6), "unsung_mask", feats={"front": "unsung_face"})]),
        Part("hood_back", "head", (0, -7.5, 3.3, 0.12, 0, 0), [
            Cube((-3.5, 0, 0), (7, 9, 0), "unsung_hood")]),
        Part("hood_left", "head", (3.4, -7.5, 0, 0, 0, -0.1), [
            Cube((0, 0, -3.5), (0, 9, 7), "unsung_hood")]),
        Part("hood_right", "head", (-3.4, -7.5, 0, 0, 0, 0.1), [
            Cube((0, 0, -3.5), (0, 9, 7), "unsung_hood")]),
        Part("crest_left", "head", (2, -6.5, 1.5, -0.95, 0.3, -0.4), [
            Cube((0, -9, 0), (0, 9, 5), "unsung_tendril_glow")]),
        Part("crest_right", "head", (-2, -6.5, 1.5, -0.95, -0.3, 0.4), [
            Cube((0, -9, 0), (0, 9, 5), "unsung_tendril_glow")]),
        Part("left_arm", None, (7.0, -2.5, -0.5, 0, 0, -0.16), [
            Cube((-1, -1, -1), (2, 12, 2), "unsung_sculk")]),
        Part("left_forearm", "left_arm", (0, 11, 0, 0, 0, 0), [
            Cube((-1, 0, -1), (2, 11, 2), "unsung_sculk")]),
        Part("left_hand", "left_forearm", (0, 11, 0, 0, 0, 0), [
            Cube((-1.5, 0, -1), (3, 3, 2), "unsung_sculk"),
            Cube((-1.5, 3, 0), (3, 4, 0), "unsung_tendril_glow")]),
        Part("right_arm", None, (-7.0, -2.5, -0.5, 0, 0, 0.16), [
            Cube((-1, -1, -1), (2, 12, 2), "unsung_sculk")]),
        Part("right_forearm", "right_arm", (0, 11, 0, 0, 0, 0), [
            Cube((-1, 0, -1), (2, 11, 2), "unsung_sculk")]),
        Part("right_hand", "right_forearm", (0, 11, 0, 0, 0, 0), [
            Cube((-1.5, 0, -1), (3, 3, 2), "unsung_sculk"),
            Cube((-1.5, 3, 0), (3, 4, 0), "unsung_tendril_glow")]),
        # The baton points forward out of the right fist.
        Part("baton", "right_hand", (0, 2, 0, 0, 0, 0), [
            Cube((-0.5, -0.5, -13), (1, 1, 14), "unsung_baton", feats={"front": "baton_tip"})]),
    ]
    for i in range(5):
        a = math.radians(18 + 72 * i)
        x, z = round(3.2 * math.cos(a), 4), round(2.4 * math.sin(a), 4)
        parts.append(Part(f"tendril_{i}", None, (x, 21, z, 0, 0, 0), [
            Cube((-0.5, 0, -0.5), (1, 5, 1), "unsung_sculk")]))
    for i in range(5):
        parts.append(Part(f"tendril_{i}_tip", f"tendril_{i}", (0, 5, 0, 0, 0, 0), [
            Cube((-0.5, 0, -0.5), (1, 4, 1), "unsung_tendril_glow")]))
    # The four voices it learned (Soprano, Alto, Tenor, Bass — the Voice
    # enum's order); orbit positions are set per frame by the render module.
    for i, voice in enumerate(("soprano", "alto", "tenor", "bass")):
        parts.append(Part(f"shard_{i}", None, (0, -2, -9, 0, 0.785398, 0), [
            Cube((-1, -2.5, -1), (2, 5, 2), f"shard_{voice}")]))
    return Mesh("the_unsung", 128, 128, head="head", parts=parts)


def meshes() -> list[Mesh]:
    """Every Hush creature mesh, packed, in emit order (append-only: a new
    mesh goes at the end so the generated part/cube indices never move)."""
    out = [_lumen_moth(), _crystal_golem(), _hush_leviathan(), _echo_mimic(),
           _choir_mother(), _the_unsung()]
    for m in out:
        names = {p.name for p in m.parts}
        for p in m.parts:
            if p.parent is not None and p.parent not in names:
                raise SystemExit(f"{m.slug}.{p.name}: unknown parent {p.parent}")
        pack(m)
    return out


if __name__ == "__main__":
    for m in meshes():
        cubes = sum(len(p.cubes) for p in m.parts)
        bottom = max(c.tv + footprint(c)[1] for p in m.parts for c in p.cubes)
        print(f"{m.slug:16s} {len(m.parts):2d} parts {cubes:2d} cubes, sheet "
              f"{m.texw}x{m.texh}, packed to v={bottom}")
