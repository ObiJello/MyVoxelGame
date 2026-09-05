#!/usr/bin/env python3
"""Generate src/client/renderer/entity/model/GeneratedEntityModels.{hpp,cpp}
from MC's client/model/**/*Model.java.

Why a generator and not 77 hand-written meshes: every mesh is a few dozen magic
numbers whose only correctness check is "does the texture line up", and a
transcription slip is invisible in review and obvious in game. The decompiled
builders are mechanical enough to read directly, so they are.

WHAT IS GENERATED is the MESH ONLY — part hierarchy, poses, cubes, texture size.
`setupAnim` is NOT generated: it is real behaviour, expressed as arbitrary Java,
and a wrong guess there looks worse than no animation. The runtime applies a
generic limb swing keyed on MC's own part names (head/body/leg/wing/tail), which
is what the shared QuadrupedModel and HumanoidModel do anyway; anything that
needs more gets a hand-written model class, as the original eight already have.

    python3 tools/gen_entity_models.py
"""

import math
import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
MODEL_DIR = os.path.join(MC, "client/model")
# The NEWER decompile (26.3 Pre-Release 2) that the "Tiny Takeover" baby
# remodel is read from — see remodel_meshes(). Everything else still comes from
# the main tree above; the two are never mixed inside one mesh.
MC2 = "minecraft_code2/decompiled_net/minecraft"
# Mobs whose EVERY mesh comes from MC2 (remodel_meshes' explicit rows).
MC2_ONLY_MESHES = {"sulfur_cube"}
MODEL_DIR2 = os.path.join(MC2, "client/model")
OUT_HPP = "src/client/renderer/entity/model/GeneratedEntityModels.hpp"
OUT_CPP = "src/client/renderer/entity/model/GeneratedEntityModels.cpp"

# Mobs whose renderer reuses another mob's model (reskins and size variants).
# Taken from the *Renderer classes; a bare <Name>Model.java does not exist for
# any of these.
#
# `Class#method` pins a SPECIFIC layer method instead of letting ENTRY_NAMES
# pick. Needed wherever LayerDefinitions.java maps the mob's ModelLayer to
# something other than the class's default createBodyLayer — those four are
# genuinely different meshes, not reskins, and taking the default silently
# draws the wrong mob (a parched shaped like a zombie, a mule shaped like a
# horse). Cross-check with:
#   grep 'ModelLayers.<SLUG>' client/model/geom/LayerDefinitions.java
MODEL_ALIAS = {
    "camel_husk": "CamelModel",
    "cave_spider": "SpiderModel",
    "elder_guardian": "GuardianModel",
    "evoker": "IllagerModel",
    "giant": "GiantZombieModel",
    "glow_squid": "SquidModel",
    "husk": "ZombieModel",
    "illusioner": "IllagerModel",
    "mooshroom": "CowModel",
    "mule": "DonkeyModel",
    "parched": "SkeletonModel#createSingleModelDualBodyLayer",
    "piglin_brute": "PiglinModel",
    "pillager": "IllagerModel",
    "pufferfish": "PufferfishSmallModel",   # PUFF_STATE defaults to 0 -> small (PufferfishRenderer:44)
    "skeleton_horse": "HorseModel",
    "slime": "SlimeModel#createInnerBodyLayer",
    "stray": "SkeletonModel",
    "trader_llama": "LlamaModel",
    "tropical_fish": "TropicalFishSmallModel",
    "vindicator": "IllagerModel",
    "wandering_trader": "VillagerModel",
    "wither": "WitherBossModel",
    "wither_skeleton": "SkeletonModel",
    "zoglin": "HoglinModel",
    "zombie_horse": "HorseModel",
    "zombie_nautilus": "NautilusModel",
}

# MC MeshTransformer.scaling(f) is picked up automatically from
# LayerDefinitions.java (`createBodyLayer().apply(SCALE)`) by eval_transformer
# and carried as PartPose scale on the root — see apply_mesh_scale for why it
# must NOT be multiplied into the geometry (UV layout derives from the
# unscaled cube size).

# Parts MC's setupAnim hides in the DEFAULT state, keyed by slug.
#
# These are overlay parts that occupy the same space as the body and are shown
# only for a state the generic model has no way to know about. Drawing them
# unconditionally is not "an extra part somewhere harmless" — it is a second
# surface coincident with the first, i.e. z-fighting, plus cargo on a llama
# that carries nothing.
#
# Hand-verified rather than derived: MC expresses the condition as arbitrary
# Java and the sense is not mechanical (armadillo's `cube` is hidden in the
# else branch, bee's `stinger` is shown by default), so each row cites the line
# it came from. Everything else in the mesh is always drawn.
HIDDEN_PARTS = {
    # ArmadilloModel.setupAnim — `this.cube.visible = false` unless
    # state.isHidingInShell. It is the 10x10x10 rolled-up ball and it
    # intersects the body, which is the front-face z-fighting.
    "armadillo":    {"cube"},
    # FrogModel's croaking_body is NOT here: `croakingBody.visible =
    # state.croakAnimationState.isStarted()` is extracted as a GenClipVisibility
    # rule instead, so the frog's throat sac inflates when it croaks rather than
    # being hidden forever.
    # LlamaModel / DonkeyModel — the chest packs appear only when carrying.
    "llama":        {"right_chest", "left_chest"},
    "trader_llama": {"right_chest", "left_chest"},
    "donkey":       {"right_chest", "left_chest"},
    "mule":         {"right_chest", "left_chest"},
    # TurtleModel — eggBelly is the gravid bulge, shown only when hasEgg.
    "turtle":       {"egg_belly"},
    # IllagerModel.setupAnim — `this.hat.visible = false`. The hat is inherited
    # from the humanoid mesh and every illager hides it.
    "evoker":       {"hat"},
    "illusioner":   {"hat"},
    "pillager":     {"hat"},
    "vindicator":   {"hat"},
}

# Hand-written models that already exist and must NOT be replaced — they carry
# real setupAnim implementations.
HAND_WRITTEN = {"zombie", "skeleton", "creeper", "spider",
                "cow", "pig", "sheep", "chicken"}
# The hand-written mobs that CAN be babies (their classic baby is built by
# EntityModel::BecomeBaby, not a generated row).
HAND_WRITTEN_BABIES = {"zombie", "cow", "pig", "sheep", "chicken"}

# Generated-model mobs that get a `<slug>_baby` mesh — MC AgeableMobRenderer's
# babyModel, built from LayerDefinitions' `<LAYER>_BABY` rows. Only mobs that
# can actually BE babies in this port are here, cross-checked against
# src/common/entity/mobs/{Animals,AnimatedMobs,Monsters,Fish}.hpp: Animal's
# age < 0, the zombie family's m_baby, and piglin/zoglin. Deliberately absent:
#   frog / parrot / zombie_nautilus  IsBaby() is hardcoded false in the port
#                                    (and in MC);
#   piglin_brute / giant / villager  no baby form in the port (brute has none
#                                    in MC either; villager is a plain
#                                    GenericPathfinderMob here);
#   dolphin / squid / glow_squid     never age in the port (Mob-based);
#   nautilus                         MC's baby is a separate 64x64 mesh on
#                                    nautilus_baby.png, and that sheet is not
#                                    in assets/ — a baby mesh sampling the
#                                    128x128 adult sheet would be garbage, so
#                                    the uniform-shrink fallback stands until
#                                    the texture lands.
# The hand-written five (zombie, cow, pig, sheep, chicken) build their babies
# in EntityModels.cpp via EntityModel::BecomeBaby instead.
BABY_MESH_SLUGS = {
    "armadillo", "axolotl", "bee", "camel", "camel_husk", "cat", "donkey",
    "drowned", "fox", "goat", "happy_ghast", "hoglin", "horse", "husk",
    "llama", "mooshroom", "mule", "ocelot", "panda", "piglin", "polar_bear",
    "rabbit", "skeleton_horse", "sniffer", "strider", "trader_llama",
    "turtle", "wolf", "zoglin", "zombie_horse", "zombie_villager",
    "zombified_piglin",
    # MC's AgeableWaterCreatures and the villager are AgeableMobs in the
    # port now (Fish.hpp, GenericAgeableMob) — their *_BABY rows apply.
    "dolphin", "squid", "glow_squid", "villager",
}

# ── The 26.x baby remodel (read from MC2) ─────────────────────────────────
#
# MC 26.1 ("Tiny Takeover") started replacing the BabyModelTransform babies
# with dedicated meshes on their own textures, and 26.2 finished the job: in
# 26.3 EVERY `<LAYER>_BABY` row is a Baby*Model class (BabyCowModel,
# BabyFoxModel, BabyHorseModel, SniffletModel, ...), and the adult rabbit was
# remodeled alongside (AdultRabbitModel + two keyframe clips). The port keeps
# BOTH looks: the `<slug>_baby` rows stay the CLASSIC babies, and the `_new`
# rows below are the remodel, chosen per world by the Baby Models world
# setting (MobRenderer::BabyModelLook).
#
# remodel_meshes() derives the rows rather than listing them: for every mob
# that can be a baby in this port, MC2's `<LAYER>_BABY` row is evaluated
# against MC2's own model classes and LayerDefinitions, and the class whose
# createBodyLayer builds it (through LayerDefinitions' locals) is the one
# gen_setup_anim.py compiles setupAnim from, under the SAME out slug — so a
# GeneratedModel("fox_baby_new") finds both its mesh and its program. The
# cow/pig/chicken biome variants and the mooshroom all share one baby mesh
# per species (the rows differ only by texture), which is why there is no
# cold_/warm_ entry.
REMODEL_SUFFIX = "_baby_new"
# Baby forms that exist ONLY under the remodel: the nautilus baby needs
# nautilus_baby.png, which the 26.3 jar ships and the classic asset set
# never had.
REMODEL_ONLY_BABIES = {"nautilus"}
_REMODEL_CACHE = None


def camel_name(s):
    return "".join(p.capitalize() for p in s.split("_"))


def remodel_meshes():
    """out slug -> (MC2 ModelLayers row, MC2 class whose setupAnim animates it)."""
    global _REMODEL_CACHE
    if _REMODEL_CACHE is not None:
        return _REMODEL_CACHE
    if not os.path.isdir(MODEL_DIR2):
        _REMODEL_CACHE = {}
        return _REMODEL_CACHE
    sources2, _ = load_sources(MODEL_DIR2)
    layers2, lvars2 = load_layer_defs(MODEL_DIR2)

    def anim_class(expr, depth=0):
        base, _ = split_applies(expr.strip())
        if base in lvars2 and depth < 6:
            return anim_class(lvars2[base], depth + 1)
        m = (re.match(r"LayerDefinition\.create\(\s*(\w+)\.\w+\(", base)
             or re.match(r"(\w+)\.\w+\(", base))
        if m and m.group(1) in sources2:
            return m.group(1)
        return None

    out = {}

    def add(slug, layer, fallback_cls):
        if layer not in layers2:
            return
        cls = anim_class(layers2[layer]) or fallback_cls
        if cls in sources2:
            out[slug] = (layer, cls)

    baby_slugs = set(BABY_MESH_SLUGS) | HAND_WRITTEN_BABIES | REMODEL_ONLY_BABIES
    # MC's CamelHuskRenderer is a plain MobRenderer on AdultCamelModel +
    # camel_husk.png — there is no baby husk mesh or sheet, so CAMEL_BABY's
    # new geometry would sample the husk sheet as garbage. The husk keeps
    # the classic transform baby (its UVs are the adult's) in both looks.
    baby_slugs.discard("camel_husk")
    for slug in sorted(baby_slugs):
        layer = SLUG_LAYER.get(slug, slug.upper()) + "_BABY"
        adult = MODEL_ALIAS.get(slug, camel_name(slug) + "Model").split("#")[0]
        add(slug + REMODEL_SUFFIX, layer, adult)
    # The remodeled ADULT rabbit, the baby sheep's wool layer (MC
    # SheepWoolLayer.babyModel — the same mesh again, drawn dyed on
    # sheep_wool_baby.png) and the baby drowned's outer layer.
    add("rabbit_new", "RABBIT", "RabbitModel")
    add("sheep_wool_baby_new", "SHEEP_BABY_WOOL", "SheepModel")
    add("drowned_outer_baby_new", "DROWNED_BABY_OUTER_LAYER", "DrownedModel")
    # The sulfur cube (26.3-only mob, ported standalone): SulfurCubeRenderer
    # draws the outer shell (SULFUR_CUBE, 128x128 sulfur_cube_outer.png)
    # with SulfurCubeInnerLayer's inner cube (SULFUR_CUBE_INNER) at order
    # -1 under it; the baby swaps both for SmallSulfurCubeModel's rows on
    # 64x64 sheets. `_baby` (not `_baby_new`) so BOTH looks use it — the
    # small model IS the mob's only baby model.
    add("sulfur_cube", "SULFUR_CUBE", "SulfurCubeModel")
    add("sulfur_cube_inner", "SULFUR_CUBE_INNER", "SulfurCubeModel")
    add("sulfur_cube_baby", "SULFUR_CUBE_SMALL", "SmallSulfurCubeModel")
    add("sulfur_cube_baby_inner", "SULFUR_CUBE_SMALL_INNER", "SmallSulfurCubeModel")
    _REMODEL_CACHE = out
    return out


def load_sources(model_dir):
    """Every *.java under a client/model tree, keyed by class name."""
    sources, paths = {}, {}
    for r, _, fs in os.walk(model_dir):
        for f in fs:
            if f.endswith(".java"):
                p = os.path.join(r, f)
                sources[f[:-5]] = strip_comments(open(p, encoding="utf-8").read())
                paths[f[:-5]] = p
    return sources, paths


# Projectiles: the sprite-rendered ones (snowball, egg, potion, fireballs)
# have no MC model class at all, and the modelled ones (trident, wind charge,
# shulker bullet, llama spit, wither skull) have hand-written classes in
# EntityModels.cpp because each needs a real setupAnim (spin / pitch-to-flight).
PROJECTILES = {
    "arrow", "snowball", "egg", "splash_potion",
    "small_fireball", "fireball", "dragon_fireball", "wither_skull",
    "shulker_bullet", "llama_spit", "trident",
    "wind_charge", "breeze_wind_charge",
}

NUM = r"[-+]?[0-9]*\.?[0-9]+"


# ── Java value evaluation ──────────────────────────────────────────────────

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def evalnum(expr, env):
    """Evaluate a numeric Java expression: literals, (float) casts, PI, vars."""
    e = expr.strip()
    e = re.sub(r"\(\s*(?:float|double|int)\s*\)", "", e)
    e = e.replace("F", "").replace("f", "").replace("D", "")
    e = e.replace("(Math.PI", "(3.141592653589793")
    e = e.replace("Math.PI", "3.141592653589793")
    # Mth.cos/sin (and the Math spellings) appear in loop-local position maths —
    # the squid's tentacle ring, the blaze's rod circles. Without them the whole
    # expression failed to eval and took the 0.0 fallback, which stacked all
    # eight squid tentacles at the origin.
    e = re.sub(r"\b(?:Mth|Math)\s*\.\s*(cos|sin)\b", r"\1", e)
    # `.mirror(true)` and `.mirror(mirrorLeftLeg)` both come through here, and
    # a bare `true` is not a Python expression — it evaluated to the 0.0
    # fallback, i.e. NOT mirrored, silently flipping those cubes' UVs.
    e = re.sub(r"\btrue\b", "1", e)
    e = re.sub(r"\bfalse\b", "0", e)
    for k, v in env.items():
        e = re.sub(r"\b" + re.escape(k) + r"\b", repr(v), e)
    try:
        return float(eval(e, {"__builtins__": {}},
                          {"cos": math.cos, "sin": math.sin}))
    except Exception:
        return 0.0


def split_args(s):
    """Split a Java argument list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def balanced(text, start, open_ch="(", close_ch=")"):
    """Content between the paren at `start` and its match. Returns (body, end)."""
    assert text[start] == open_ch
    depth, i = 0, start
    while i < len(text):
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    return text[start + 1:], len(text)


# ── The mesh model ─────────────────────────────────────────────────────────

class Part:
    def __init__(self, name, parent):
        self.name = name
        self.parent = parent          # Part or None
        self.pose = [0.0] * 6         # x y z xRot yRot zRot
        self.scale = [1.0] * 3        # PartPose xScale yScale zScale
        self.cubes = []               # dicts


DEFORM_HINT = re.compile(r"CubeDeformation|\.extend\s*\(|deformation|inflate|fudge",
                         re.I)


def is_deform(expr, denv):
    """Does this addBox argument denote a CubeDeformation?"""
    e = expr.strip()
    return bool(DEFORM_HINT.search(e)) or e in denv


def evaldeform(expr, env, denv):
    """A CubeDeformation expression -> its per-axis grow (gx, gy, gz), pixels.

    MC inflates a cube by this per axis (ModelPart.Cube's constructor).
    Two coincident boxes with DIFFERENT deformations are how MC layers a shell
    over a body — armadillo, wolf, sheep, every 'outer layer'. Dropping the
    value collapses them onto each other and they z-fight, which is exactly
    what this used to do: `evalnum("new CubeDeformation(0.3F)")` cannot be
    eval'd as Python, so it returned the 0.0 fallback for all 138 of them.

    Per-axis matters in exactly one place in MC's model sources:
    AbstractEquineModel.createFullScaleBabyMesh stretches the baby legs with
    `g.extend(0, 5.5, 0)` — collapsed to one float, baby equine legs come out
    5.5px short of the ground.
    """
    e = expr.strip()
    if e in denv:
        return denv[e]
    if e.endswith("NONE") and "CubeDeformation" in e:
        return (0.0, 0.0, 0.0)

    # `<base>.extend(f)` / `.extend(x, y, z)` -> base + factor, recursively.
    m = re.search(r"\.extend\s*\(", e)
    if m:
        inner, end = balanced(e, m.end() - 1)
        if end >= len(e.rstrip()):
            args = split_args(inner)
            base = evaldeform(e[:m.start()], env, denv)
            if len(args) >= 3:
                return tuple(b + evalnum(a, env)
                             for b, a in zip(base, args[:3]))
            f = evalnum(args[0], env)
            return (base[0] + f, base[1] + f, base[2] + f)

    m = re.match(r"new\s+CubeDeformation\s*\(", e)
    if m:
        inner, _ = balanced(e, m.end() - 1)
        args = split_args(inner)
        if len(args) >= 3:
            return tuple(evalnum(a, env) for a in args[:3])
        v = evalnum(args[0], env) if args else 0.0
        return (v, v, v)

    v = evalnum(e, env)
    return (v, v, v)


def parse_cubes(expr, env, denv=None):
    """A CubeListBuilder chain -> list of cube dicts.

    Dispatches CubeListBuilder's addBox overloads (CubeListBuilder.java:32-74).
    They are NOT positionally uniform: four of them take a leading String id,
    so args[0] is the box's x0 in some and a comment in others. Reading the
    first six as numbers unconditionally — which is what this used to do —
    turned `addBox("body", -4, 0, -2, 8, 12, 4, 64, 0)` into a cube at
    (0, -4, 0) sized (-2, 8, 12).
    """
    denv = denv or {}
    cubes = []
    tex = (0.0, 0.0)
    mirror = False
    i = 0
    while i < len(expr):
        m = re.compile(r"\.(texOffs|addBox|mirror)\s*\(").search(expr, i)
        if not m:
            break
        body, end = balanced(expr, m.end() - 1)
        call = m.group(1)
        args = split_args(body)
        if call == "texOffs":
            tex = (evalnum(args[0], env), evalnum(args[1], env))
        elif call == "mirror":
            mirror = evalnum(args[0], env) != 0.0 if args else True
        elif call == "addBox":
            has_id = bool(args) and args[0].strip().startswith('"')
            a = args[1:] if has_id else args
            if len(a) >= 6:
                vals = [evalnum(x, env) for x in a[:6]]
                extra = a[6:]
                grow, cube_tex = (0.0, 0.0, 0.0), tex

                if len(extra) == 1:
                    # CubeDeformation | boolean mirror | Set<Direction>.
                    if is_deform(extra[0], denv):
                        grow = evaldeform(extra[0], env, denv)
                    # `mirror` and `visibleSides` need no change here: the
                    # former is handled by the chain-level .mirror() in every
                    # model that matters, the latter only hides faces.
                elif len(extra) == 2:
                    # (xTexOffs, yTexOffs) — a per-cube override of texOffs.
                    cube_tex = (evalnum(extra[0], env), evalnum(extra[1], env))
                elif len(extra) >= 3:
                    grow = evaldeform(extra[0], env, denv)
                    if has_id:
                        # CubeListBuilder.java:32 — the trailing pair is
                        # xTexOffs/yTexOffs. Without the id it is
                        # xTexScale/yTexScale (:69), which this port does not
                        # model; leaving tex alone is the right no-op there.
                        cube_tex = (evalnum(extra[1], env), evalnum(extra[2], env))

                cubes.append(dict(o=vals[0:3], s=vals[3:6],
                                  tu=cube_tex[0], tv=cube_tex[1],
                                  grow=grow, mirror=mirror))
        i = end
    return cubes


NAME_FN = re.compile(r'\b(\w+)\s*\([^)]*\)\s*\{\s*return\s+"([a-z_0-9]*)"\s*\+')


class JavaRandom:
    """java.util.Random's 48-bit LCG. `RandomSource.create(seed)` is
    LegacyRandomSource, which is exactly this generator (cross-checked against
    src/my_terrain_library/include/random/LegacyRandomSource.h). GhastModel
    seeds one with 1660L and draws every tentacle's length from it, so the mesh
    is only right if this reproduces the sequence bit for bit."""
    MASK = (1 << 48) - 1
    MULT = 0x5DEECE66D        # 25214903917
    INC = 0xB

    def __init__(self, seed):
        self.seed = (seed ^ self.MULT) & self.MASK

    def next(self, bits):
        self.seed = (self.seed * self.MULT + self.INC) & self.MASK
        return self.seed >> (48 - bits)

    def next_int(self, bound):
        if bound & (bound - 1) == 0:              # power-of-two shortcut
            return (bound * self.next(31)) >> 31
        while True:
            bits = self.next(31)
            val = bits % bound
            # Retry while `bits - val + (bound-1)` overflows a signed int32 —
            # Java writes this as `< 0` after the wrap.
            if bits - val + (bound - 1) < (1 << 31):
                return val


def parse_static_tables(src):
    """Class-level `static final int[][] / int[] / float[]` data tables.

    SilverfishModel and EndermiteModel keep every segment's box size and
    texOffs in `BODY_SIZES` / `BODY_TEXS`, GuardianModel keeps its spike ring
    in six float[]s. Values from an int table stay Python ints so that Java's
    integer `/` and `%` still fold correctly after substitution."""
    tables = {}
    for m in re.finditer(r"static\s+final\s+(int|float)\s*\[\]\s*\[\]\s+(\w+)"
                         r"\s*=\s*new\s+(?:int|float)\s*\[\]\s*\[\]\s*\{", src):
        outer, _ = balanced(src, m.end() - 1, "{", "}")
        rows, i = [], 0
        while True:
            j = outer.find("{", i)
            if j < 0:
                break
            inner, i = balanced(outer, j, "{", "}")
            rows.append([_table_num(x, m.group(1)) for x in inner.split(",") if x.strip()])
        tables[m.group(2)] = rows
    for m in re.finditer(r"static\s+final\s+(int|float)\s*\[\]\s+(\w+)"
                         r"\s*=\s*new\s+(?:int|float)\s*\[\]\s*\{", src):
        inner, _ = balanced(src, m.end() - 1, "{", "}")
        tables[m.group(2)] = [_table_num(x, m.group(1)) for x in inner.split(",") if x.strip()]
    return tables


def _table_num(text, jtype):
    v = float(text.strip().rstrip("FfDdLl"))
    return int(v) if jtype == "int" else v


def numeric_return_fns(src):
    """Static numeric helpers whose whole body is `return <expr>;`.

    GuardianModel's getSpikeX/Y/Z/getSpikeOffset are the reason: the spike
    positions are computed through them, and leaving the calls unresolved made
    every spike evaluate to the origin. They are inlined (with parameters
    bound) during simulation so evalnum sees plain arithmetic."""
    out = {}
    for m in re.finditer(r"static\s+(?:int|long|float|double)\s+(\w+)\s*\(", src):
        got = method_signature(src, m.group(1))
        if not got:
            continue
        params, mbody = got
        r = re.fullmatch(r"\s*return\s+(.+?);\s*", mbody, re.S)
        if r:
            out[m.group(1)] = ([p[1] for p in params], r.group(1).strip())
    return out


def fold_int_ops(text):
    """Fold `intLiteral / intLiteral` and `%` with JAVA int semantics.

    After the loop variable is substituted, GhastModel's `(float)(i / 3 % 2)`
    reads e.g. `4 / 3 % 2` — Java int division gives 1, Python float division
    1.333, and the difference moves tentacles between rows. Only folds when
    BOTH operands are bare int literals (so `2.0F / 8.0F` is untouched) and the
    preceding token is not `*`/`/`/`%` (same precedence, would bind first)."""
    pat = re.compile(r"(?<![\w.])(\d+)\s*([/%])\s*(\d+)(?![\w.])")
    while True:
        for m in pat.finditer(text):
            if text[:m.start()].rstrip().endswith(("*", "/", "%")):
                continue
            a, b = int(m.group(1)), int(m.group(3))
            if b == 0:
                continue
            v = a // b if m.group(2) == "/" else a % b
            text = text[:m.start()] + str(v) + text[m.end():]
            break
        else:
            return text


def eval_cond(cond):
    """A fully-literal Java condition -> True/False, or None when it still
    references anything symbolic (in which case the caller keeps the source
    text and both branches are scanned, as before)."""
    e = re.sub(r"\btrue\b", "1", cond)
    e = re.sub(r"\bfalse\b", "0", e)
    e = re.sub(r"\(\s*(?:float|double|int|long)\s*\)", "", e)
    e = e.replace("F", "").replace("f", "").replace("D", "")
    e = e.replace("&&", " and ").replace("||", " or ")
    e = re.sub(r"!(?!=)", " not ", e)
    if re.search(r"[A-Za-z_]", re.sub(r"\b(?:and|or|not)\b", "", e)):
        return None
    try:
        return bool(eval(e, {"__builtins__": {}}, {}))
    except Exception:
        return None


def unroll_loops(body, src, env, sources=None, chain=()):
    """Symbolically execute a mesh-builder body into straight-line text.

    The scans that follow (addOrReplaceChild, PartPose, cubes) read literals,
    so everything Java computes along the way has to be computed HERE and
    substituted into the text:

      - literal-bounded `for` loops are unrolled (blaze rods, squid tentacles,
        wither heads, the dragon's neck — left folded, a whole loop collapses
        into one part);
      - part-name helpers (`getPartName(i)` -> "part3") are resolved for BOTH
        loop-variable and literal arguments, including the cross-class
        `PartNames.tentacle(i)` — the qualifier is consumed too, or the
        substitution left broken `PartNames."tentacle0"` text behind;
      - numeric locals are simulated statement by statement, so loop-carried
        accumulators (silverfish's `placement += ...`, blaze's `++angle`) and
        per-iteration locals (ghast's `int len = random.nextInt(7) + 8`) read
        their value AT that point in the execution, not their initial value;
      - `float[]` locals filled inside the loop (silverfish's zPlacement) are
        recorded and their reads after the loop substituted;
      - static int[][] / float[] data tables (BODY_SIZES) are folded in;
      - `RandomSource.create(seed)` draws are replayed with the real Java LCG,
        in execution order;
      - `if` conditions that reduce to literals are decided (drops the
        HappyGhast baby-only inner_body and PlayerModel's slim-arm branch);
        anything symbolic keeps its source text, which is the old behaviour.
    """
    sources = sources or {}
    name_fns = {m.group(1): m.group(2)
                for m in NAME_FN.finditer(sources.get("PartNames", ""))}
    # The name helper may live UP the chain: 26.3's BabySquidModel builds
    # its tentacles through SquidModel.createTentacleName(i). Base classes
    # first so the class's own helper wins a collision.
    for c in reversed(list(chain)):
        if c in sources and sources[c] is not src:
            name_fns.update({m.group(1): m.group(2) for m in NAME_FN.finditer(sources[c])})
    name_fns.update({m.group(1): m.group(2) for m in NAME_FN.finditer(src)})
    tables = parse_static_tables(src)
    numfns = numeric_return_fns(src)
    rngs = {}                 # java var -> JavaRandom
    farrays = {}              # java float[] local -> {index: value}
    sim = dict(env)           # numeric locals, simulated in statement order

    def jnum(v):
        if isinstance(v, int):
            return str(v)
        r = repr(round(float(v), 6))
        # Parenthesise negatives so `-x` cannot turn into `--3.5`.
        return "(" + r + ")" if r.startswith("-") else r

    def subst_arrays(text):
        pos = 0
        while True:
            m = re.compile(r"\b(\w+)\s*\[").search(text, pos)
            if not m:
                return text
            data = tables.get(m.group(1), farrays.get(m.group(1)))
            if data is None:
                pos = m.end()
                continue
            idx_txt, end = balanced(text, text.index("[", m.end() - 1), "[", "]")
            if not re.fullmatch(r"[\d\s()+\-*/%.]+", idx_txt):
                pos = m.end()
                continue
            i1 = int(evalnum(fold_int_ops(idx_txt), {}))
            val = None
            if isinstance(data, dict):                       # simulated float[]
                val = data.get(i1)
            elif 0 <= i1 < len(data):
                row = data[i1]
                if isinstance(row, list):                    # int[][] — need [j]
                    m2 = re.match(r"\s*\[", text[end:])
                    if m2:
                        idx2, e2 = balanced(text, end + m2.end() - 1, "[", "]")
                        if re.fullmatch(r"[\d\s()+\-*/%.]+", idx2):
                            j = int(evalnum(fold_int_ops(idx2), {}))
                            if 0 <= j < len(row):
                                val = row[j]
                                end = e2
                else:
                    val = row
            if val is None:
                pos = m.end()
                continue
            lit = jnum(val)
            text = text[:m.start()] + lit + text[end:]
            pos = m.start() + len(lit)

    def subst(text):
        # Numeric locals -> their current value. NOT when the name is a member
        # access or a call: VillagerModel declares a (dead) local `float offset
        # = 0.5F`, and an unguarded substitution rewrote every
        # `PartPose.offset(...)` into `PartPose.0.5(...)`, zeroing the poses.
        for k, v in sim.items():
            text = re.sub(r"(?<![.\w])" + re.escape(k) + r"\b(?!\s*\()",
                          jnum(v), text)
        # Single-return static numeric helpers -> their expression, parameters
        # bound. Repeated for nested helpers (getSpikeX -> getSpikeOffset).
        for _ in range(4):
            hit = False
            for fn, (params, expr) in numfns.items():
                m = re.search(r"\b" + re.escape(fn) + r"\s*\(", text)
                while m:
                    args, aend = balanced(text, m.end() - 1)
                    e = expr
                    for p, a in zip(params, split_args(args)):
                        e = re.sub(r"\b" + re.escape(p) + r"\b",
                                   "(" + a.strip() + ")", e)
                    text = text[:m.start()] + "(" + e + ")" + text[aend:]
                    hit = True
                    m = re.search(r"\b" + re.escape(fn) + r"\s*\(", text)
            if not hit:
                break
        text = subst_arrays(text)
        # Part-name helpers with (now-)literal arguments; the optional
        # `PartNames.` / class qualifier is consumed with the call.
        for fn, prefix in name_fns.items():
            text = re.sub(r"(?:\b\w+\s*\.\s*)?\b" + re.escape(fn)
                          + r"\s*\(\s*(-?\d+)\s*\)",
                          lambda mm: '"%s%s"' % (prefix, mm.group(1)), text)
        # Seeded random draws — each textual occurrence is one draw, and text
        # order IS execution order once the loops are unrolled.
        def draw(mm):
            r = rngs.get(mm.group(1))
            return mm.group(0) if r is None else str(r.next_int(int(mm.group(2))))
        text = re.sub(r"\b(\w+)\s*\.\s*nextInt\s*\(\s*(\d+)\s*\)", draw, text)
        return fold_int_ops(text)

    def process_stmt(s):
        s = s.strip()
        if not s:
            return ""

        if re.match(r"for\s*\(", s):
            head, hend = balanced(s, s.index("("))
            rest = s[hend:].lstrip()
            if rest.startswith("{"):
                inner, _ = balanced(rest, 0, "{", "}")
                p = head.split(";")
                mi = re.fullmatch(r"\s*(?:int\s+)?(\w+)\s*=\s*(-?\d+)\s*",
                                  p[0]) if len(p) == 3 else None
                mc = re.fullmatch(r"\s*(\w+)\s*<\s*(-?\d+)\s*", p[1]) if mi else None
                if mi and mc and mi.group(1) == mc.group(1) \
                        and int(mc.group(2)) - int(mi.group(2)) <= 64:
                    var = mi.group(1)
                    saved = sim.pop(var, None)
                    out = []
                    for k in range(int(mi.group(2)), int(mc.group(2))):
                        sim[var] = k
                        out.append(process_block(inner))
                    if saved is None:
                        sim.pop(var, None)
                    else:
                        sim[var] = saved
                    return "\n".join(out)
            return subst(s)

        if re.match(r"if\s*\(", s):
            cond, cend = balanced(s, s.index("("))
            rest = s[cend:].lstrip()
            if rest.startswith("{"):
                then_body, tend = balanced(rest, 0, "{", "}")
                tail = rest[tend:].lstrip()
                verdict = eval_cond(subst(cond))
                if verdict is True:
                    return process_block(then_body)
                if verdict is False:
                    if tail.startswith("else"):
                        t2 = tail[4:].lstrip()
                        if t2.startswith("{"):
                            return process_block(balanced(t2, 0, "{", "}")[0])
                        if t2.startswith("if"):
                            return process_stmt(t2)
                    return ""
            return subst(s)

        m = re.fullmatch(r"RandomSource\s+(\w+)\s*=\s*RandomSource\s*\.\s*create"
                         r"\(\s*(-?\d+)L?\s*\)", s)
        if m:
            rngs[m.group(1)] = JavaRandom(int(m.group(2)))
            return ""
        m = re.fullmatch(r"float\s*\[\]\s*(\w+)\s*=\s*new\s+float\s*\[\s*\d+\s*\]", s)
        if m:
            farrays[m.group(1)] = {}
            return ""
        m = re.fullmatch(r"(?:final\s+)?(int|long|float|double)\s+(\w+)\s*=\s*(.+)",
                         s, re.S)
        if m:
            v = evalnum(subst(m.group(3)), {})
            sim[m.group(2)] = int(v) if m.group(1) in ("int", "long") else v
            return ""
        m = re.fullmatch(r"(\w+)\s*\[([^\]]+)\]\s*=\s*(.+)", s, re.S)
        if m and m.group(1) in farrays:
            idx = int(evalnum(subst(m.group(2)), {}))
            farrays[m.group(1)][idx] = evalnum(subst(m.group(3)), {})
            return ""
        m = re.fullmatch(r"(\w+)\s*([+\-*/])?=\s*([^=].*)", s, re.S)
        if m and m.group(1) in sim:
            v = evalnum(subst(m.group(3)), {})
            old = sim[m.group(1)]
            op = m.group(2)
            nv = v if not op else {"+": old + v, "-": old - v, "*": old * v,
                                   "/": old / v if v else 0.0}[op]
            if isinstance(old, int) and float(nv).is_integer():
                nv = int(nv)
            sim[m.group(1)] = nv
            return ""
        m = re.fullmatch(r"(?:\+\+\s*(\w+)|(\w+)\s*\+\+)", s)
        if m:
            nm = m.group(1) or m.group(2)
            if nm in sim:
                sim[nm] = sim[nm] + 1
            return ""

        out = subst(s)
        return out if out.endswith("}") else out + ";"

    def process_block(text):
        return "\n".join(filter(None, (process_stmt(x)
                                       for x in split_statements(text))))

    return process_block(body)


def parse_pose(expr, env):
    pose = [0.0] * 6
    m = re.search(r"PartPose\.(offsetAndRotation|offset|rotation)\s*\(", expr)
    if not m:
        return pose
    body, _ = balanced(expr, m.end() - 1)
    args = [evalnum(a, env) for a in split_args(body)]
    kind = m.group(1)
    if kind == "offset":
        pose[0:3] = args[:3]
    elif kind == "rotation":
        pose[3:6] = args[:3]
    else:
        pose[0:6] = (args + [0.0] * 6)[:6]
    return pose


def method_body(src, name):
    """The `{ ... }` body of a static method by name, or None."""
    got = method_signature(src, name)
    return got[1] if got else None


def method_signature(src, name):
    """(params, body) for a static method, params as [(type, name), …].

    The names matter: MC threads a CubeDeformation through mesh helpers as a
    parameter — `createBodyMesh(CubeDeformation g)`, `createMesh(float scale,
    CubeDeformation deformation)` — and the caller passes the literal. Without
    the binding, `g` inside addBox resolves to nothing and the deformation is
    lost for the 72 cubes that use that spelling.
    """
    m = re.search(r"static\s+\w+(?:<[^>]*>)?\s+" + re.escape(name) + r"\s*\(", src)
    if not m:
        return None
    arglist, end = balanced(src, m.end() - 1)
    brace = src.find("{", end)
    if brace < 0:
        return None
    inner, _ = balanced(src, brace, "{", "}")

    params = []
    for decl in split_args(arglist):
        d = decl.strip().replace("final ", "")
        parts = d.split()
        if len(parts) >= 2:
            params.append((parts[-2], parts[-1]))
    return params, inner


def base_mesh(call, env, denv, sources, chain):
    """Evaluate `Cls.method(...)` or a bare local `method(...)` as a mesh."""
    m = re.match(r"(?:(\w+)\s*\.\s*)?(\w+)\s*\(", call)
    if not m:
        return None
    owner, meth = m.group(1), m.group(2)
    # An owner-qualified call resolves along the OWNER's superclass chain —
    # Java static-method inheritance. `PiglinModel.createMesh(...)` (the
    # LayerDefinitions row for all three piglins) is defined on
    # AbstractPiglinModel; searching the named class alone found nothing and
    # the whole layer evaluation failed over to the wrong fallback mesh.
    if owner:
        search = class_chain(owner, sources) or [owner]
    else:
        search = list(chain)
    for cls in search:
        src = sources.get(cls)
        if not src:
            continue
        got = method_signature(src, meth)
        if got is None:
            continue
        params, body = got
        args = split_args(balanced(call, call.index("("))[0])

        # Bind the callee's parameters from the caller's arguments, evaluated
        # in the CALLER's scope.
        cenv, cdenv = {}, {}
        for (ptype, pname), arg in zip(params, args):
            if ptype == "CubeDeformation" or is_deform(arg, denv):
                cdenv[pname] = evaldeform(arg, env, denv)
            else:
                cenv[pname] = evalnum(arg, env)

        # `return BABY_TRANSFORMER.apply(createFullScaleBabyMesh(g));` —
        # AbstractEquineModel.createBabyMesh. The body builds nothing itself,
        # so run_mesh would come back empty; evaluate the inner call and run
        # the class's transformer constant over it instead.
        cchain = class_chain(cls, sources) or list(chain)
        rm = re.fullmatch(r"\s*return\s+([A-Z][A-Z0-9_]*)\s*\.\s*apply\s*\("
                          r"\s*((?:\w+\s*\.\s*)?\w+\s*\(.*\))\s*\)\s*;\s*",
                          body, re.S)
        if rm:
            decl = None
            for c2 in cchain:
                decl = re.search(r"MeshTransformer\s+" + rm.group(1)
                                 + r"\s*=\s*([^;]+);", sources.get(c2, ""))
                if decl:
                    break
            if decl:
                got = base_mesh(rm.group(2), cenv, cdenv, sources, cchain)
                if got:
                    model = dict(root=got[0], parts=got[1])
                    ctx = {"sources": sources, "lvars": {}, "home": cls}
                    apply_transformer(model,
                                      eval_transformer_in(decl.group(1), cls, ctx),
                                      ctx)
                    return model["root"], model["parts"]

        # The callee's body runs in the scope of the class that DEFINES it.
        return run_mesh(body, src, sources, cchain, cenv, cdenv)
    return None


def run_mesh(body, src, sources, chain=(), env=None, denv=None, into=None):
    """Execute a mesh-building method body. Returns (root Part, parts list).

    `into` continues an EXISTING (root, parts) instead of starting a new mesh —
    that is how MC's MeshTransformer lambdas work (DonkeyModel's
    `modifyMesh(mesh.getRoot())` bolts the chest packs and the long ears onto a
    mesh AbstractEquineModel already built).
    """
    env = dict(env or {})
    denv = dict(denv or {})

    # CubeDeformation locals (`CubeDeformation g = new CubeDeformation(0.25F);`).
    for m in re.finditer(r"CubeDeformation\s+(\w+)\s*=\s*([^;]+);", body):
        denv[m.group(1)] = evaldeform(m.group(2), env, denv)

    if into is not None:
        root, parts = into
    else:
        root = Part("root", None)
        parts = [root]
    vars_ = {}          # java var -> Part
    cubelists = {}      # java var -> CubeListBuilder expression

    # Numeric locals (`int legSize = 6;`).
    for m in re.finditer(r"\b(?:int|float|double)\s+(\w+)\s*=\s*([^;]+);", body):
        env[m.group(1)] = evalnum(m.group(2), env)

    # A mesh built on top of another one — either a shared base
    # (QuadrupedModel.createBodyMesh) or a private local helper that
    # createBodyLayer wraps (Hoglin's createMesh, Cow's createBaseCowModel).
    mbase = re.search(r"MeshDefinition\s+\w+\s*=\s*(?!new\b)((?:\w+\s*\.\s*)?\w+)\s*\(", body)
    if mbase:
        call_start = mbase.end() - 1
        inner, _ = balanced(body, call_start)
        got = base_mesh(mbase.group(1) + "(" + inner + ")", env, denv, sources, chain)
        if got:
            root, parts = got

    # CubeListBuilder locals, reused across parts.
    for m in re.finditer(r"CubeListBuilder\s+(\w+)\s*=\s*([^;]+);", body):
        cubelists[m.group(1)] = m.group(2)

    # `PartDefinition x = mesh.getRoot();` and `... = root.getChild("body");`
    for m in re.finditer(r"PartDefinition\s+(\w+)\s*=\s*\w+\.getRoot\(\)", body):
        vars_[m.group(1)] = root
    # `.getRoot()` is allowed inside the chain: HoglinModel.createBabyLayer
    # writes `PartDefinition body = mesh.getRoot().getChild("body")`, and
    # without it the mane replacement landed under the ROOT as a duplicate.
    for m in re.finditer(
            r"PartDefinition\s+(\w+)\s*=\s*(\w+)"
            r"((?:\s*\.\s*get(?:Root\s*\(\s*\)|Child\s*\(\s*\"[a-z_0-9]+\"\s*\)))+)", body):
        cur = vars_.get(m.group(2), root)
        for name in re.findall(r'getChild\s*\(\s*"([a-z_0-9]+)"', m.group(3)):
            cur = next((p for p in parts if p.parent is cur and p.name == name), cur)
        vars_[m.group(1)] = cur

    # Rewrite `<expr>.getRoot()` / `<expr>.getChild("x")` receivers of an
    # addOrReplaceChild into a temporary variable. The receiver pattern below
    # needs a bare identifier, so a chained call — `mesh.getRoot()
    # .addOrReplaceChild(...)` (shulker) or `root.getChild("head")
    # .addOrReplaceChild(...)` (bogged) — matched nothing at all and the part
    # was silently dropped from the mesh.
    def _hoist(mm):
        recv, chainexpr = mm.group(1), mm.group(2)
        cur = recv
        key = "__recv%d__" % len(vars_)
        base = vars_.get(recv, root)
        for name in re.findall(r'getChild\(\s*"([a-z_0-9]+)"', chainexpr):
            base = next((p for p in parts if p.parent is base and p.name == name), base)
        vars_[key] = base
        return key + ".addOrReplaceChild("
    body = re.sub(r"(\w+)((?:\.get(?:Root\(\)|Child\(\s*\"[a-z_0-9]+\"\s*\)))+)"
                  r"\s*\.addOrReplaceChild\s*\(", _hoist, body)

    # Unroll `for (int i = A; i < B; ++i) { ... }` before scanning for parts.
    #
    # Blaze rods, squid tentacles, wither heads and the dragon's neck are all
    # built inside a loop, with the part NAME computed by a helper
    # (`getPartName(i)` -> `"part" + i`). Left folded, the whole loop collapses
    # to a single child called `part` — a blaze rendered with one rod instead of
    # twelve, and a squid with no tentacles at all.
    body = unroll_loops(body, src, env, sources, chain)

    # `PartDefinition head = addHead(g, mesh);` — a static helper that builds
    # parts INTO the mesh and returns one of them. AbstractPiglinModel.addHead
    # is the only occurrence in MC's model sources: it replaces the humanoid
    # head with the piglin head (snout, both ears as children) — skipping it
    # left every piglin ear-less with a plain player head. The assigned
    # variable is bound to the part the helper's `return` names, so a later
    # `head.clearChild(...)` resolves.
    for m in re.finditer(r"PartDefinition\s+(\w+)\s*=\s*(?:(\w+)\s*\.\s*)?(\w+)\s*\(", body):
        hvar, howner, hmeth = m.group(1), m.group(2), m.group(3)
        if hmeth in ("getRoot", "getChild", "addOrReplaceChild"):
            continue
        if howner:
            search = class_chain(howner, sources) or [howner]
        else:
            search = list(chain)
        for cls in search:
            csrc = sources.get(cls)
            if not csrc:
                continue
            got = method_signature(csrc, hmeth)
            if got is None:
                continue
            hparams, hbody = got
            hargs = split_args(balanced(body, m.end() - 1)[0])
            henv, hdenv = {}, {}
            for (ptype, pname), arg in zip(hparams, hargs):
                if ptype == "CubeDeformation" or is_deform(arg, denv):
                    hdenv[pname] = evaldeform(arg, env, denv)
                else:
                    henv[pname] = evalnum(arg, env)
            run_mesh(hbody, csrc, sources, class_chain(cls, sources) or chain,
                     henv, hdenv, into=(root, parts))
            ret = re.search(r"return\s+(\w+)\s*;", hbody)
            if ret:
                rd = re.search(r"PartDefinition\s+" + re.escape(ret.group(1))
                               + r"\s*=\s*\w+\.addOrReplaceChild\(\s*\"([a-z_0-9]+)\"",
                               hbody)
                if rd:
                    part = next((p for p in parts if p.name == rd.group(1)), None)
                    if part is not None:
                        vars_[hvar] = part
            break

    # `createDefaultSkeletonMesh(root);` — a bare VOID helper that builds
    # parts into the mesh. SkeletonModel (and through it stray, bogged,
    # wither skeleton) REPLACES the humanoid's fat limbs with the slim 2px
    # bones this way; skipping it shipped fat-armed skeletons. Only helpers
    # whose body actually adds parts are inlined, and the PartDefinition
    # argument falls back to the mesh root inside run_mesh — the only shape
    # MC's model sources use for this. Inlined here, after the assigned-
    # helper pass and before the main scan; MC always places these calls
    # after the base mesh is built, so replace-order is preserved.
    for m in re.finditer(r"(?:^|;)\s*(?:(\w+)\s*\.\s*)?(\w+)\s*\(\s*(\w+)\s*\)\s*;",
                         body):
        howner, hmeth = m.group(1), m.group(2)
        if hmeth in ("getRoot", "getChild", "addOrReplaceChild", "clearChild"):
            continue
        if howner:
            search = class_chain(howner, sources) or [howner]
        else:
            search = list(chain)
        for cls in search:
            csrc = sources.get(cls)
            if not csrc:
                continue
            got = method_signature(csrc, hmeth)
            if got is None:
                continue
            hparams, hbody = got
            if "addOrReplaceChild" not in hbody:
                break
            run_mesh(hbody, csrc, sources, class_chain(cls, sources) or chain,
                     dict(env), dict(denv), into=(root, parts))
            break

    # Every addOrReplaceChild, in source order.
    for m in re.finditer(r"(\w+)((?:\.addOrReplaceChild\s*\()+)", body):
        pass
    pos = 0
    while True:
        m = re.compile(r"(?:(\w+)\s*=\s*)?(\w+)\.addOrReplaceChild\s*\(").search(body, pos)
        if not m:
            break
        inner, end = balanced(body, m.end() - 1)
        assign, recv = m.group(1), m.group(2)
        parent = vars_.get(recv, root)

        args3 = split_args(inner)
        if len(args3) >= 2:
            name = re.search(r'"([^"]*)"', args3[0])
            name = name.group(1) if name else "part"
            cube_expr = args3[1]
            for v, expr in cubelists.items():
                cube_expr = re.sub(r"\b" + re.escape(v) + r"\b", expr, cube_expr)
            pose_expr = args3[2] if len(args3) > 2 else ""

            # Replace an existing part of the same name under this parent.
            part = next((p for p in parts
                         if p.parent is parent and p.name == name), None)
            if part is None:
                part = Part(name, parent)
                parts.append(part)
            part.cubes = parse_cubes(cube_expr, env, denv)
            part.pose = parse_pose(pose_expr, env)

            if assign:
                vars_[assign] = part
            # `root.addOrReplaceChild(...).addOrReplaceChild(...)` chains onto
            # the part just created.
            tail = body[end:end + 40]
            if tail.lstrip().startswith(".addOrReplaceChild"):
                vars_["__chain__"] = part
                body = body[:end] + body[end:].replace(".addOrReplaceChild",
                                                       "__chain__.addOrReplaceChild", 1)
        pos = end

    # `head.clearChild("hat")` — MC empties the child's CUBES but keeps the
    # part, its pose and its children (PartDefinition.clearChild re-adds the
    # name with CubeListBuilder.create()). AbstractPiglinModel uses it to
    # delete the humanoid hat overlay from the piglin head; HumanoidModel's
    # constructor still getChild()s "hat", so the part must survive, empty.
    for m in re.finditer(r'(\w+)\s*\.\s*clearChild\(\s*"([a-z_0-9]+)"\s*\)', body):
        parent = vars_.get(m.group(1), root)
        part = next((p for p in parts
                     if p.parent is parent and p.name == m.group(2)), None)
        if part is not None:
            part.cubes = []

    # MC PartDefinition.retainPartsAndChildren / retainExactParts /
    # clearRecursively — the part-FILTER family. BreezeModel builds its three
    # LayerDefinitions from one base mesh with it (`mesh.getRoot()
    # .retainPartsAndChildren(Set.of("head", "rods"))` for the body,
    # {"wind_body"} for the 128x128 wind sheet, {"eyes"} for the eyes);
    # CreakingModel / WardenModel / CopperGolemModel cut their emissive
    # layers the same way and the villagers their no-hat meshes
    # (`clearChild("head").clearRecursively()`). All three keep every PART —
    # name, pose and children survive, so a bake-time getChild still
    # resolves — and only empty its cubes:
    #   retainPartsAndChildren(S): a child named in S is kept whole, subtree
    #     included; any other child loses its cubes and recurses.
    #   retainExactParts(S): a child named in S keeps its own cubes but its
    #     subtree is cleared; any other child loses its cubes and recurses.
    #   clearRecursively(): every descendant loses its cubes; the receiver
    #     keeps its own.
    # Before this pass existed the breeze's BODY row carried the wind rings
    # at their 128x128 texOffs on the 32x32 sheet (garbage texels wrapped
    # over the body) and the wind and eyes rows carried the head and rods on
    # THEIR sheets — three complete meshes stacked on one another.
    def _kids(p):
        return [c for c in parts if c.parent is p]

    def _clear_rec(p):
        for c in _kids(p):
            c.cubes = []
            _clear_rec(c)

    def _retain(p, names, exact):
        for c in _kids(p):
            if c.name in names:
                if exact:
                    _clear_rec(c)
            else:
                c.cubes = []
                _retain(c, names, exact)

    for m in re.finditer(
            r"(\w+)((?:\s*\.\s*(?:getRoot\s*\(\s*\)"
            r"|(?:getChild|clearChild)\s*\(\s*\"[A-Za-z_0-9]+\"\s*\)))*)"
            r"\s*\.\s*(retainPartsAndChildren|retainExactParts|clearRecursively)"
            r"\s*\(([^)]*)\)", body):
        recv = vars_.get(m.group(1), root)
        for name in re.findall(r'(?:getChild|clearChild)\s*\(\s*"([A-Za-z_0-9]+)"',
                               m.group(2)):
            recv = next((p for p in parts
                         if p.parent is recv and p.name == name), recv)
        op = m.group(3)
        names = set(re.findall(r'"([A-Za-z_0-9]+)"', m.group(4)))
        if op == "clearRecursively":
            _clear_rec(recv)
        else:
            _retain(recv, names, op == "retainExactParts")

    return root, parts


# Mesh entry points, most specific first. MC is not consistent about the name:
# most models expose createBodyLayer, but Wolf uses createMeshDefinition,
# Villager createBodyModel, Spider createSpiderBodyLayer, Slime
# createOuterBodyLayer, and several keep the real mesh in a private createMesh
# that createBodyLayer merely wraps.
ENTRY_NAMES = (
    "createBodyLayer", "createBodyMesh", "createMeshDefinition", "createMesh",
    "createBaseMesh", "createBodyModel", "createSpiderBodyLayer",
    "createOuterBodyLayer", "createLayer",
)


def class_chain(cls, sources, seen=None):
    """A class and its superclasses, nearest first.

    Half of MC's models hold no mesh of their own: HorseModel extends
    AbstractEquineModel, ZombieModel extends AbstractZombieModel extends
    HumanoidModel, CatModel extends FelineModel. Without the walk those come
    out empty, which reads as 'no model' rather than 'look one level up'.
    """
    seen = seen or set()
    if cls in seen or cls not in sources:
        return []
    seen.add(cls)
    out = [cls]
    m = re.search(r"class\s+" + re.escape(cls) + r"(?:<[^>]*>)?\s+extends\s+(\w+)", sources[cls])
    if m:
        out += class_chain(m.group(1), sources, seen)
    return out


def resolve_forward(body, src, sources, chain):
    """`return LayerDefinition.create(helper(...), W, H);` -> the helper's body."""
    m = re.search(r"LayerDefinition\.create\s*\(", body)
    if not m:
        return body
    inner, _ = balanced(body, m.end() - 1)
    args = split_args(inner)
    if not args:
        return body
    # Either `helper(...)` or `SomeClass.helper(...)` — Donkey forwards to
    # AbstractEquineModel.createBodyMesh, Hoglin to a private local createMesh.
    call = re.match(r"\s*(?:(\w+)\s*\.\s*)?(\w+)\s*\(", args[0])
    if not call:
        return body
    owner, meth = call.group(1), call.group(2)
    search = [owner] if owner else list(chain)
    for cls in search:
        if cls in sources:
            hb = method_body(sources[cls], meth)
            if hb is not None:
                return hb
    return body


def apply_mesh_scale(model, f):
    """MC MeshTransformer.scaling(f), carried on the ROOT PartPose.

    scaling(f) is `pose -> pose.scaled(f).translated(0, 24.016*(1-f), 0)`,
    and MeshDefinition.transformed rewrites only the root's own pose —
    PartPose.scaled multiplies the offsets AND the pose scale, translated
    then adds the re-anchor. The scale must stay ON THE POSE rather than be
    multiplied into the geometry: MC computes every cube's UV layout from
    the UNSCALED pixel size (a Cube never carries scale), so baking f into
    the sizes remaps every face's texels by f — the smeared wither-skeleton
    / cave-spider / villager-family textures. GenPart carries the three
    PartPose scale fields for exactly this."""
    y_offset = 24.016 * (1.0 - f)
    root = model["root"]
    for i in range(3):
        root.pose[i] *= f
        root.scale[i] *= f
    root.pose[1] += y_offset
    if root not in model["parts"]:
        # Never happens on the eval_layer path (run_mesh seeds parts with the
        # root), but a scaled mesh whose root got dropped would silently lose
        # the transform — make that loud instead.
        raise RuntimeError("apply_mesh_scale: root not in parts")


def parse_baby_transform(expr):
    """`new BabyModelTransform(...)` -> the record's seven fields.

    Resolves the two convenience constructors (BabyModelTransform.java:12-18)
    to the canonical (scaleHead, babyYHeadOffset, babyZHeadOffset,
    babyHeadScale, babyBodyScale, bodyYOffset, headParts) form. The trailing
    argument is always the Set.of(...) of head part names.
    """
    inner, _ = balanced(expr, expr.index("("))
    args = split_args(inner)
    head_parts = frozenset(re.findall(r'"([a-z_0-9]+)"', args[-1]))
    nums = [evalnum(a, {}) for a in args[:-1]]
    if not nums:                       # (headParts)
        nums = [0.0, 5.0, 2.0]
    if len(nums) == 3:                 # (scaleHead, yHead, zHead, headParts)
        nums += [2.0, 2.0, 24.0]
    return (nums[0] != 0.0, nums[1], nums[2], nums[3], nums[4], nums[5],
            head_parts)


# LlamaModel.transformToBaby (LlamaModel.java:55-79) — the one baby transform
# that is a hand-rolled loop instead of a BabyModelTransform record, so its
# literal per-part rules are transcribed. Rows are (part names or None for
# the default arm of the switch, translate xyz, scale xyz), applied in order,
# first match wins — exactly the Java switch.
LLAMA_BABY_POSES = (
    (frozenset(("head",)), (0.0, 21.0, 3.52),
     (0.71428573, 0.64935064, 0.7936508)),
    (frozenset(("body",)), (0.0, 33.0, 0.0),
     (0.625, 0.45454544, 0.45454544)),
    (None, (0.0, 33.0, 0.0),
     (0.45454544, 0.41322312, 0.45454544)),
)


def apply_baby_poses(model, rules):
    """BabyModelTransform.apply / LlamaModel.transformToBaby: rewrite each
    ROOT CHILD's PartPose as translated(t) then scaled(s) — PartPose.scaled
    multiplies the offsets AND the pose scale (PartPose.java:30-32), and
    parts below the root keep their own poses. MC rebuilds the mesh under a
    fresh ZERO root, so anything the old root's pose carried is dropped.
    """
    root = model["root"]
    root.pose = [0.0] * 6
    root.scale = [1.0] * 3
    for p in model["parts"]:
        if p.parent is not root:
            continue
        for names, t, s in rules:
            if names is None or p.name in names:
                for i in range(3):
                    p.pose[i] = (p.pose[i] + t[i]) * s[i]
                    p.scale[i] *= s[i]
                break


def parse_model(cls, sources, entry_pin=None):
    chain = class_chain(cls, sources)
    if not chain:
        return None

    # A pinned entry (MODEL_ALIAS "Class#method") wins outright — it is there
    # precisely because the default pick is the wrong mesh.
    names = (entry_pin,) if entry_pin else ENTRY_NAMES

    body, home = None, None
    for c in chain:
        for entry in names:
            body = method_body(sources[c], entry)
            if body is not None:
                home = c
                break
        if body is not None:
            break
    if body is None:
        return None

    # Texture size comes from the ENTRY METHOD's own LayerDefinition.create
    # first. Taking the first one in the FILE is wrong for the eight classes
    # that expose layers at two sheet sizes — SkeletonModel is 64x32 for the
    # skeleton and 64x64 for the parched dual-body layer, and picking the wrong
    # one halves every V coordinate, which garbles the texture rather than
    # merely offsetting it. Only when the entry has none of its own does it
    # fall back to the derived class and then up the chain.
    def layer_size(text):
        m = re.search(r"LayerDefinition\.create\s*\(", text)
        if not m:
            return None
        a = split_args(balanced(text, m.end() - 1)[0])
        if len(a) < 3:
            return None
        return int(evalnum(a[1], {})), int(evalnum(a[2], {}))

    texw, texh = 64, 32
    got = layer_size(body)
    if got is None:
        for c in chain:
            got = layer_size(sources[c])
            if got:
                break
    if got:
        texw, texh = got

    body = resolve_forward(body, sources[home], sources, chain)
    root, parts = run_mesh(body, sources[home], sources, chain)
    parts = [p for p in parts if p is not root or p.cubes]
    return dict(texw=texw, texh=texh, root=root, parts=parts)




# ── LayerDefinitions.java: MC's own description of how each layer is built ──
#
# This file, not the model class, is the authority. `ModelLayers.CAVE_SPIDER` is
# `spiderBodyLayer.apply(MeshTransformer.scaling(0.7F))` and `ModelLayers.HUSK`
# is `humanoidBodyLayer.apply(huskScale)` — resolve the model class alone and
# you get a cave spider drawn at spider size and a husk at zombie size. Thirteen
# mobs carry a scale transformer here, and donkey and mule get theirs from a
# parameter (0.87 / 0.92) plus a modifyMesh that adds their long ears.
#
# Slugs whose ModelLayer is not simply the uppercased slug. MC has no
# ModelLayers row for these three, so the renderer's own choice is pinned:
#   CamelHuskRenderer   -> reuses ModelLayers.CAMEL
#   PufferfishRenderer  -> PUFF_STATE defaults to 0, i.e. the SMALL layer
#   TropicalFishRenderer-> base model is TROPICAL_FISH_SMALL
SLUG_LAYER = {
    "camel_husk":    "CAMEL",
    "pufferfish":    "PUFFERFISH_SMALL",
    "tropical_fish": "TROPICAL_FISH_SMALL",
}


# What each model's setupAnim ACTUALLY does, read out of the model class.
#
# Two things the generic animation used to assume for everyone and MC does for
# neither:
#
#  - HEAD TRACKING. `this.head.yRot = state.yRot * DEG_TO_RAD` appears in most
#    models, but NOT in frog, camel, breeze, warden, nautilus or bat — and the
#    part is not always called "head" (dolphin and strider turn `body`, the
#    wither turns `centerHead`). Rotating a frog's flat head plate that MC
#    keeps rigid is what made its head shear away from its body.
#
#  - THE WALK CYCLE. Six models animate from a KeyframeAnimation instead of a
#    limb swing, with their own speed and scale factors.
HEAD_ASSIGN = re.compile(r"this\.(\w+)\.[xy]Rot\s*=\s*([^;]*);")
# Locals derived from the look angles. AbstractEquineModel writes
# `float clampedYRot = Mth.clamp(state.yRot, -20, 20);` and then assigns
# `this.headParts.yRot = clampedYRot * DEG`, so requiring `state.yRot` in the
# assignment itself missed every horse, donkey and mule.
HEAD_LOCAL = re.compile(r"\bfloat\s+(\w+)\s*=\s*([^;]*state\.[xy]Rot[^;]*);")
BAKE_FIELD = re.compile(r"this\.(\w+)\s*=\s*(\w+Animation\.\w+)\.bake\(")

# ── The animation CLIPS a model plays ──────────────────────────────────────
#
# Nine models drive KeyframeAnimations rather than writing limb rotations. Two
# kinds appear:
#
#   applyWalk(pos, speed, sf, cf)   the walk cycle, driven by distance walked
#   apply(state.<X>, ageInTicks)    an episodic clip, driven by an AnimationState
#
# The second kind is the whole point of Game::AnimationState: the keyframes are
# already baked, but without a timer telling the model WHEN the clip started
# there is nothing to sample, so a frog's croak and a warden's roar are dead
# data. Extracting the calls here is what connects the two.
WALK_APPLY = re.compile(
    r"this\.(\w+)\.applyWalk\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,"
    r"\s*([-\d.]+)F?\s*,\s*([-\d.]+)F?\s*\)")
STATE_APPLY = re.compile(r"this\.(\w+)\.apply\(\s*state\.(\w+)\s*,\s*state\.ageInTicks\s*\)")
VIS_STARTED = re.compile(
    r"this\.(\w+)\.visible\s*=\s*state\.(\w+)\.isStarted\(\)")

# MC's per-class AnimationState field name -> the shared MobAnim slot.
# Two mobs sharing a name share the slot; no entity has two of them, so the
# collision is a naming coincidence rather than a conflict. Kept explicit
# (rather than derived by stripping "AnimationState") so that adding a mob whose
# field name does not follow the convention fails loudly instead of silently
# inventing a slot the C++ enum does not have.
ANIM_SLOT = {
    "attackAnimationState":          "Attack",
    "croakAnimationState":           "Croak",
    "dashAnimationState":            "Dash",
    "deathAnimationState":           "Death",
    "diggingAnimationState":         "Digging",
    "emergeAnimationState":          "Emerge",
    "feelingHappyAnimationState":    "FeelingHappy",
    "flyAnimationState":             "Fly",
    "hopAnimationState":             "Hop",
    "idleAnimationState":            "Idle",
    "idleHeadTiltAnimationState":    "IdleHeadTilt",
    # The 26.2 baby axolotl (AxolotlRenderState: `swimAnimation` is the one
    # field without the -State suffix).
    "swimAnimation":                 "Swim",
    "swimAnimationState":            "Swim",
    "walkAnimationState":            "Walk",
    "walkUnderWaterAnimationState":  "WalkUnderWater",
    "idleUnderWaterAnimationState":  "IdleUnderWater",
    "idleUnderWaterOnGroundAnimationState": "IdleUnderWaterOnGround",
    "idleOnGroundAnimationState":    "IdleOnGround",
    "playDeadAnimationState":        "PlayDead",
    "idle":                          "Idle",
    "inhale":                        "Inhale",
    "interactionDropItem":           "InteractionDropItem",
    "interactionDropNoItem":         "InteractionDropNoItem",
    "interactionGetItem":            "InteractionGetItem",
    "interactionGetNoItem":          "InteractionGetNoItem",
    "invulnerabilityAnimationState": "Invulnerability",
    "jumpAnimationState":            "Jump",
    "longJump":                      "LongJump",
    "peekAnimationState":            "Peek",
    "restAnimationState":            "Rest",
    "risingAnimationState":          "Rising",
    "roarAnimationState":            "Roar",
    "rollOutAnimationState":         "RollOut",
    "rollUpAnimationState":          "RollUp",
    "scentingAnimationState":        "Scenting",
    "shoot":                         "Shoot",
    "sitAnimationState":             "Sit",
    "sitPoseAnimationState":         "SitPose",
    "sitUpAnimationState":           "SitUp",
    "slide":                         "Slide",
    "slideBack":                     "SlideBack",
    "sniffAnimationState":           "Sniff",
    "sniffingAnimationState":        "Sniffing",
    "sonicBoomAnimationState":       "SonicBoom",
    "swimIdleAnimationState":        "SwimIdle",
    "tongueAnimationState":          "Tongue",
}

# The `if` conditions that gate a clip, normalised to single spaces.
#
# Only five exist across all nine models, and each is a distinct render-state
# boolean, so a table beats a general expression compiler: an unrecognised
# condition raises rather than quietly dropping the clip. `(guard, negate)`.
CLIP_GUARD = {
    # FrogRenderer:25 — `state.isSwimming = entity.isInWater()`.
    "state.isSwimming": ("IsInWater", False),
    "state.isSearching": ("IsSearching", False),
    "state.canMove": ("CanMove", False),
    "state.isResting": ("IsResting", False),
    # CopperGolemModel:127 — empty hands take the plain walk.
    "state.rightHandItemState.isEmpty() && state.leftHandItemState.isEmpty()":
        ("IsHoldingItem", True),
}

GUARD_NAMES = ["None", "IsInWater", "IsSearching", "CanMove", "IsResting",
               "IsHoldingItem",
               # `state.<x>AnimationState.isStarted()` — the guard carries the
               # MobAnim slot (GenClip.guardSlot); BabyAxolotlModel gates its
               # walk clip on the walk state running.
               "AnimStarted"]

# `[!]state.<field>.isStarted()` as a clip guard -> (name, negate, slot).
STARTED_GUARD = re.compile(r"^(!?)\s*state\.(\w+)\.isStarted\(\)$")


def guard_of(cond):
    """A clip-guard condition -> (name, negate, slot-or-None), or None."""
    key = norm(cond)
    if key in CLIP_GUARD:
        name, negate = CLIP_GUARD[key]
        return (name, negate, None)
    m = STARTED_GUARD.match(key)
    if m and m.group(2) in ANIM_SLOT:
        return ("AnimStarted", m.group(1) == "!", ANIM_SLOT[m.group(2)])
    return None


ANIM_STATE_HPP = "src/common/entity/AnimationState.hpp"


def read_mob_anim_slots():
    """Game::MobAnim's ordinals, read from the C++ header.

    The generated clip rows carry the slot as a raw ordinal, so the two enums
    must agree. Reading the real one — instead of re-deriving the order here —
    turns a rename or an inserted slot into a generator error rather than every
    animation silently playing on the wrong timer.
    """
    src = strip_comments(open(ANIM_STATE_HPP, encoding="utf-8").read())
    m = re.search(r"enum class MobAnim\s*:\s*uint8_t\s*\{(.*?)\}", src, re.S)
    if not m:
        raise SystemExit("could not find MobAnim in " + ANIM_STATE_HPP)
    names = [n.split("=")[0].strip()
             for n in m.group(1).split(",") if n.strip()]
    names = [n for n in names if n and n != "Count"]
    index = {n: i for i, n in enumerate(names)}
    missing = sorted(set(ANIM_SLOT.values()) - set(index))
    if missing:
        raise SystemExit("MobAnim is missing slots: " + ", ".join(missing))
    return index


def snake(name):
    """MC field name -> the mesh part name (centerHead -> center_head)."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def norm(text):
    return " ".join(text.split())


PRIVATE_HELPER = re.compile(
    r"private\s+void\s+(\w+)\s*\(([^)]*)\)\s*\{")


def head_derived_names(src):
    """Local/parameter names that carry the look angles into a head assignment.

    Four models never write `state.yRot` at the assignment at all — they pass it
    into a helper (`applyHeadRotation(state.yRot)` on the bat, `applyBodyRotation`
    on the nautilus, `animateHeadLookTarget` on the warden, and the camel's
    clamping three-argument version) and the helper writes the part from its own
    PARAMETER. Matching only on the literal `state.yRot` left those four with no
    tracked head at all, so a resting bat stared straight ahead.

    Also picks up MC's `float clampedYRot = Mth.clamp(state.yRot, -20, 20);`
    pattern, which every horse-like model uses.
    """
    names = {m.group(1) for m in HEAD_LOCAL.finditer(src)}

    body = instance_method_body(src, "setupAnim")
    if not body:
        return names

    helpers = {}
    for m in PRIVATE_HELPER.finditer(src):
        params = []
        for d in split_args(m.group(2)):
            d = d.strip().replace("final ", "").split()
            if len(d) >= 2:
                params.append(d[-1])
        helpers[m.group(1)] = params

    for m in re.finditer(r"this\.(\w+)\s*\(", body):
        name = m.group(1)
        if name not in helpers:
            continue
        args = split_args(balanced(body, m.end() - 1)[0])
        for param, arg in zip(helpers[name], args):
            if "state.yRot" in arg or "state.xRot" in arg:
                names.add(param)
    return names


def instance_method_body(src, name):
    """The body of a non-static method. `method_body` above requires `static`,
    which every mesh builder is and no `setupAnim` is."""
    m = re.search(r"public\s+void\s+" + re.escape(name) + r"\s*\(", src)
    if not m:
        return None
    _, end = balanced(src, m.end() - 1)
    brace = src.find("{", end)
    if brace < 0:
        return None
    return balanced(src, brace, "{", "}")[0]


def split_statements(body):
    """Top-level statements, keeping `if (...) { ... } else { ... }` whole.

    Same shape as the one in gen_setup_anim.py: a block statement ends at its
    closing brace, not at a semicolon, and a `for` header carries semicolons
    inside its parens.
    """
    out, i, depth, paren, cur = [], 0, 0, 0, ""
    while i < len(body):
        c = body[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        cur += c
        if c == ";" and depth == 0 and paren == 0:
            out.append(cur[:-1].strip())
            cur = ""
        elif c == "}" and depth == 0 and paren == 0:
            rest = body[i + 1:].lstrip()
            if not rest.startswith("else"):
                out.append(cur.strip())
                cur = ""
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return [s for s in out if s]


def walk_arg_extras(pos_arg, speed_arg):
    """(posAgeScale, speedBias) for applyWalk's first two arguments.

    Every model but one passes `state.walkAnimationPos` and
    `state.walkAnimationSpeed` untouched. NautilusModel adds
    `state.ageInTicks / 5` and `0.2` — which is what makes a nautilus keep
    swimming while it hovers instead of freezing mid-stroke.
    """
    pos, speed = norm(pos_arg), norm(speed_arg)
    age_scale, bias = 0.0, 0.0
    if pos != "state.walkAnimationPos":
        m = re.fullmatch(
            r"state\.walkAnimationPos \+ state\.ageInTicks / ([\d.]+)F?", pos)
        if not m:
            raise ValueError("applyWalk pos arg %r" % pos)
        age_scale = 1.0 / float(m.group(1))
    if speed != "state.walkAnimationSpeed":
        m = re.fullmatch(r"state\.walkAnimationSpeed \+ ([\d.]+)F?", speed)
        if not m:
            raise ValueError("applyWalk speed arg %r" % speed)
        bias = float(m.group(1))
    return age_scale, bias


def scan_clips(body, bake, guard, out, vis):
    """Walk one setupAnim body, recording clips with the guard in force."""
    for stmt in split_statements(body):
        s = stmt.strip()
        if s.startswith("if"):
            m = re.match(r"if\s*\(", s)
            cond, end = balanced(s, m.end() - 1, "(", ")")
            rest = s[end:].lstrip()
            if not rest.startswith("{"):
                continue
            then_body, e2 = balanced(rest, 0, "{", "}")
            tail = rest[e2:].lstrip()
            g = guard_of(cond)
            if g is None:
                # Not a clip guard — recurse anyway so a clip nested under an
                # unrelated condition (the armadillo's scared branch) is still
                # found, with the OUTER guard.
                scan_clips(then_body, bake, guard, out, vis)
                if tail.startswith("else"):
                    t2 = tail[4:].lstrip()
                    if t2.startswith("{"):
                        scan_clips(balanced(t2, 0, "{", "}")[0], bake, guard, out, vis)
                continue
            name, negate, slot = g
            scan_clips(then_body, bake, (name, negate, slot), out, vis)
            if tail.startswith("else"):
                t2 = tail[4:].lstrip()
                if t2.startswith("{"):
                    scan_clips(balanced(t2, 0, "{", "}")[0], bake,
                               (name, not negate, slot), out, vis)
            continue

        m = WALK_APPLY.fullmatch(s)
        if m:
            anim = bake.get(m.group(1))
            if anim:
                age, bias = walk_arg_extras(m.group(2), m.group(3))
                out.append(("Walk", anim, "Attack", float(m.group(4)),
                            float(m.group(5)), guard, age, bias))
            continue

        m = STATE_APPLY.fullmatch(s)
        if m:
            anim = bake.get(m.group(1))
            if anim is None:
                continue
            field = m.group(2)
            if field not in ANIM_SLOT:
                raise ValueError("no MobAnim slot for state.%s" % field)
            out.append(("Timed", anim, ANIM_SLOT[field], 0.0, 0.0, guard, 0.0, 0.0))
            continue

        m = VIS_STARTED.fullmatch(s)
        if m:
            field = m.group(2)
            if field not in ANIM_SLOT:
                raise ValueError("no MobAnim slot for state.%s" % field)
            vis.append((snake(m.group(1)), ANIM_SLOT[field]))


# `this.hopAnimation = hop.bake(root)` where `hop` is a CONSTRUCTOR PARAMETER
# — 26.3's RabbitModel takes its two AnimationDefinitions from the subclass:
# `AdultRabbitModel(root) { super(root, RabbitAnimation.HOP, RabbitAnimation.
# IDLE_HEAD_TILT); }`. BAKE_FIELD only sees the `Xxx.NAME.bake(` spelling, so
# the parameter names are matched positionally against the most-derived
# class's super(...) call.
BAKE_PARAM = re.compile(r"this\.(\w+)\s*=\s*(\w+)\.bake\(")


def bake_fields(cls, chain, sources):
    """field name -> 'XxxAnimation.NAME' for every baked clip on the chain."""
    bake = {}
    for c in chain:
        bake.update(dict(BAKE_FIELD.findall(sources[c])))
    # Parameter-passed definitions: find the class that bakes a parameter,
    # read its constructor's parameter list, and bind each name to the
    # argument the leaf class's `super(...)` passes at that position.
    for c in chain:
        params_used = BAKE_PARAM.findall(sources[c])
        if not params_used:
            continue
        m = re.search(r"\b" + re.escape(c) + r"\s*\(([^)]*)\)\s*\{", sources[c])
        if not m:
            continue
        pnames = [d.strip().replace("final ", "").split()[-1]
                  for d in split_args(m.group(1)) if d.strip()]
        sup = re.search(r"\bsuper\s*\(([^;]*)\)\s*;", sources[cls])
        if not sup:
            continue
        sargs = [a.strip() for a in split_args(sup.group(1))]
        for field, pname in params_used:
            if pname in pnames:
                k = pnames.index(pname)
                if k < len(sargs) and re.fullmatch(r"\w+Animation\.\w+", sargs[k]):
                    bake.setdefault(field, sargs[k])
    return bake


# MC Model.renderType: which RenderType a model class asks for, and whether
# that type's pipeline culls back faces (RenderPipelines.java — `withCull
# (false)` on the no-cull ones, the builder default otherwise). EntityModel
# defaults to the NO-cull cutout type: entity models are not closed, and
# vanilla shows the far side of a skeleton's ribs through the gaps. A few
# classes opt into a culling type in their constructor (BatModel, ArrowModel,
# BeeStingerModel, TridentModel + the solid objects; 26.3's BabyTurtleModel)
# — exactly the ones whose zero-thickness planes would otherwise fight their
# own back face. entityTranslucent (allay, vex, breeze, piglins, player) is
# NOT culled in either tree.
#
# 26.3 renamed the family: entityCutoutNoCull became the default
# `entityCutout`, and the culling one became `entityCutoutCull` — hence a
# table per decompile rather than a name rule.
RENDER_TYPE_PICK = re.compile(
    r"(?:super|this)\s*\(\s*root(?:\.getChild\([^)]*\))?\s*,\s*RenderTypes::(\w+)")
CULLS_MC1 = {   # 26.1 Snapshot 1
    "entityCutout": True, "entityCutoutNoCull": False,
    "entityCutoutNoCullZOffset": False, "entitySolid": True,
    "entityTranslucent": False,
}
CULLS_MC2 = {   # 26.3 Pre-Release 2
    "entityCutout": False, "entityCutoutCull": True,
    "entityCutoutZOffset": False, "entitySolid": True,
    "entityTranslucent": False,
}


def model_culls(cls, sources, culls=CULLS_MC1):
    """Does MC draw this model class back-face culled? The nearest class in
    the chain that names a RenderType decides; none named = the default."""
    for c in class_chain(cls, sources):
        m = RENDER_TYPE_PICK.search(sources[c])
        if m:
            if m.group(1) not in culls:
                raise SystemExit("unknown RenderType %s in %s" % (m.group(1), c))
            return culls[m.group(1)]
    return False


def setup_anim_info(cls, sources):
    """(head part, head guard, clips, visibility rules) for a model class."""
    head, head_guard = "", ("None", False)
    clips, vis = [], []
    chain = class_chain(cls, sources)
    for c in chain:
        src = sources[c]
        if not head:
            locals_ = head_derived_names(src)
            for m in HEAD_ASSIGN.finditer(src):
                rhs = m.group(2)
                if "state.yRot" in rhs or "state.xRot" in rhs \
                        or any(re.search(r"\b" + re.escape(v) + r"\b", rhs) for v in locals_):
                    head = snake(m.group(1))
                    break

    # Clips come from the most-derived setupAnim that has any; a subclass that
    # overrides setupAnim replaces its parent's clip list wholesale (it calls
    # super.setupAnim, but EntityModel's base does no animation).
    for c in chain:
        body = instance_method_body(sources[c], "setupAnim")
        if body is None:
            continue
        bake = bake_fields(cls, chain, sources)
        got, gotvis = [], []
        scan_clips(body, bake, ("None", False), got, gotvis)
        if got or gotvis:
            clips, vis = got, gotvis
            # BatModel guards its head turn on `state.isResting`; every other
            # model turns unconditionally. Read here rather than assumed,
            # because a bat that tracks you while flying looks broken.
            m = re.search(r"if\s*\(\s*state\.(\w+)\s*\)\s*\{\s*this\.applyHeadRotation",
                          body)
            if m and "state." + m.group(1) in CLIP_GUARD:
                head_guard = CLIP_GUARD["state." + m.group(1)]
            break

    return head, head_guard, clips, vis


def load_layer_defs(model_dir=MODEL_DIR):
    """(layer name -> expression, local name -> expression)."""
    src = strip_comments(open(os.path.join(model_dir,
                                           "geom/LayerDefinitions.java"), encoding="utf-8").read())
    lvars = {}
    for m in re.finditer(r"\b(?:LayerDefinition|MeshTransformer)\s+(\w+)\s*=\s*([^;]+);",
                         src):
        lvars[m.group(1)] = m.group(2).strip()

    layers = {}
    for m in re.finditer(r"result\.put\(\s*ModelLayers\.([A-Z0-9_]+)\s*,\s*", src):
        body, _ = balanced(src, src.index("(", m.start()))
        args = split_args(body)
        if len(args) >= 2:
            layers[m.group(1)] = args[1].strip()
    return layers, lvars


def split_applies(expr):
    """`base.apply(a).apply(b)` -> ("base", ["a", "b"]), respecting nesting."""
    applies = []
    while True:
        depth, cut = 0, -1
        for i, ch in enumerate(expr):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif depth == 0 and expr.startswith(".apply(", i):
                cut = i
        if cut < 0:
            return expr.strip(), applies[::-1]
        inner, end = balanced(expr, cut + len(".apply") )
        if end < len(expr.rstrip()):
            return expr.strip(), applies[::-1]
        applies.append(inner.strip())
        expr = expr[:cut]


def top_level_ternary(expr):
    """`cond ? a : b` split at depth 0, or None. No MC transformer expression
    nests ternaries, so the first '?' and its ':' are the split points."""
    depth, qpos = 0, None
    for i, ch in enumerate(expr):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "?" and depth == 0:
            qpos = i
        elif ch == ":" and depth == 0 and qpos is not None:
            return expr[:qpos], expr[qpos + 1:i], expr[i + 1:]
    return None


def eval_transformer(expr, ctx):
    """A MeshTransformer expression -> ("scale", f) | ("meshfn", cls, meth)
    | ("baby", params) | ("posemap", rules) | None."""
    e = expr.strip()
    sources, lvars = ctx["sources"], ctx["lvars"]

    if e in lvars:
        return eval_transformer(lvars[e], ctx)

    # `baby ? BABY_TRANSFORMER : MeshTransformer.IDENTITY` — RabbitModel and
    # PolarBearModel thread a boolean through createBodyLayer. Folded BEFORE
    # the IDENTITY early-out below, which would otherwise swallow the whole
    # ternary on its trailing token.
    q = top_level_ternary(e)
    if q:
        cond, then_e, else_e = q
        for k, v in ctx.get("env", {}).items():
            cond = re.sub(r"\b" + re.escape(k) + r"\b", repr(v), cond)
        verdict = eval_cond(cond)
        if verdict is None:
            return None
        return eval_transformer(then_e if verdict else else_e, ctx)

    if e.endswith("IDENTITY"):
        return None

    m = re.match(r"MeshTransformer\s*\.\s*scaling\s*\(", e)
    if m:
        inner, _ = balanced(e, m.end() - 1)
        return ("scale", evalnum(inner, ctx.get("env", {})))

    # `Cls.SOME_TRANSFORMER` — resolve the constant in that class.
    m = re.fullmatch(r"(\w+)\s*\.\s*([A-Z][A-Z0-9_]*)", e)
    if m and m.group(1) in sources:
        cls, const = m.group(1), m.group(2)
        d = re.search(r"MeshTransformer\s+" + const + r"\s*=\s*([^;]+);", sources[cls])
        if d:
            return eval_transformer_in(d.group(1), cls, ctx)

    # A bare constant, written inside the class whose layer method we are in —
    # DonkeyModel's `.apply(DONKEY_TRANSFORMER)`. Resolving it needs that class,
    # so `home` must survive the call. The search walks the superclass chain:
    # DonkeyModel.createBabyLayer applies BABY_TRANSFORMER, which lives on
    # AbstractEquineModel.
    home = ctx.get("home")
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", e):
        for cls in class_chain(home, ctx["sources"]) or ():
            d = re.search(r"MeshTransformer\s+" + e + r"\s*=\s*([^;]+);",
                          sources[cls])
            if d:
                return eval_transformer_in(d.group(1), cls, ctx)
    return eval_transformer_in(e, home, ctx)


def eval_transformer_in(expr, cls, ctx):
    """As eval_transformer, but for an expression written inside `cls`."""
    e = expr.strip()
    m = re.match(r"MeshTransformer\s*\.\s*scaling\s*\(", e)
    if m:
        inner, _ = balanced(e, m.end() - 1)
        return ("scale", evalnum(inner, ctx.get("env", {})))
    # The baby-mesh record — see BabyModelTransform.java and apply_baby_poses.
    if re.match(r"new\s+BabyModelTransform\s*\(", e):
        return ("baby", parse_baby_transform(e))
    # LlamaModel::transformToBaby — the method reference behind
    # LlamaModel.BABY_TRANSFORMER; its rules are the transcribed table.
    if e.endswith("::transformToBaby"):
        return ("posemap", LLAMA_BABY_POSES)
    # `(mesh) -> { modifyMesh(mesh.getRoot()); return mesh; }` — a lambda that
    # bolts extra parts on. DonkeyModel's is the only one reachable from a mob
    # layer, and it is what adds the chest packs and the long ears.
    # `(mesh) -> { mesh.getRoot().retainExactParts(Set.of("head")); return
    # mesh; }` — a lambda that only FILTERS parts (WardenModel's and
    # CopperGolemModel's emissive layers, ZombieVillagerModel's no-hat
    # layer). Its body runs through run_mesh, whose retain/clear pass
    # applies it to the mesh already built.
    m = re.match(r"\(?\s*\w+\s*\)?\s*->\s*\{(.*)\}\s*$", e, re.S)
    if m and re.search(r"retainPartsAndChildren|retainExactParts|clearRecursively",
                       m.group(1)):
        return ("meshbody", m.group(1))
    m = re.search(r"(\w+)\s*\(\s*\w+\s*\.\s*getRoot\s*\(\s*\)\s*\)", e)
    if m and cls:
        return ("meshfn", cls, m.group(1))
    return None


def apply_transformer(model, tf, ctx):
    if tf is None:
        return
    if tf[0] == "scale":
        apply_mesh_scale(model, tf[1])
    elif tf[0] == "baby":
        scale_head, y_head, z_head, head_scale, body_scale, body_y, heads = tf[1]
        # BabyModelTransform.apply:21-24.
        hs = (1.5 / head_scale) if scale_head else 1.0
        bs = 1.0 / body_scale
        apply_baby_poses(model, (
            (heads, (0.0, y_head, z_head), (hs, hs, hs)),
            (None, (0.0, body_y, 0.0), (bs, bs, bs)),
        ))
    elif tf[0] == "posemap":
        apply_baby_poses(model, tf[1])
    elif tf[0] == "meshfn":
        _, cls, meth = tf
        sources = ctx["sources"]
        got = method_signature(sources.get(cls, ""), meth)
        if not got:
            return
        _, body = got
        run_mesh(body, sources[cls], sources, class_chain(cls, sources),
                 into=(model["root"], model["parts"]))
    elif tf[0] == "meshbody":
        run_mesh(tf[1], "", ctx["sources"], ctx.get("chain", ()),
                 into=(model["root"], model["parts"]))


def eval_layer(expr, ctx, depth=0):
    """A LayerDefinition expression -> dict(texw, texh, root, parts)."""
    if depth > 8:
        return None
    sources, lvars = ctx["sources"], ctx["lvars"]
    base, applies = split_applies(expr.strip())

    model = None
    if base in lvars:
        model = eval_layer(lvars[base], ctx, depth + 1)
    else:
        m = re.match(r"(?:(\w+)\s*\.\s*)?(\w+)\s*\(", base)
        if m:
            owner, meth = m.group(1), m.group(2)
            inner, _ = balanced(base, base.index("("))
            args = split_args(inner)
            if owner == "LayerDefinition" and meth == "create":
                model = eval_create(args, ctx, depth)
            else:
                model = eval_method_layer(owner, meth, args, ctx, depth)

    if model:
        for a in applies:
            apply_transformer(model, eval_transformer(a, ctx), ctx)
    return model


def eval_create(args, ctx, depth):
    """`LayerDefinition.create(<mesh>, w, h)`."""
    texw = int(evalnum(args[1], ctx.get("env", {}))) if len(args) > 2 else 64
    texh = int(evalnum(args[2], ctx.get("env", {}))) if len(args) > 2 else 32
    mesh_expr = args[0].strip()
    home, body = ctx.get("home"), ctx.get("body")
    sources = ctx["sources"]

    if re.match(r"(?:\w+\s*\.\s*)?\w+\s*\(", mesh_expr):
        got = base_mesh(mesh_expr, ctx.get("env", {}), ctx.get("denv", {}),
                        sources, ctx.get("chain", ()))
        if got:
            root, parts = got
            return dict(texw=texw, texh=texh, root=root, parts=parts)
    if body is not None and home:
        b = resolve_forward(body, sources[home], sources, ctx.get("chain", ()))
        root, parts = run_mesh(b, sources[home], sources, ctx.get("chain", ()),
                               ctx.get("env"), ctx.get("denv"))
        return dict(texw=texw, texh=texh, root=root, parts=parts)
    return None


def eval_method_layer(owner, meth, args, ctx, depth):
    """`Cls.createXxxLayer(args)` -> the layer it returns."""
    sources = ctx["sources"]
    if owner in sources:
        # Walk the owner's superclass chain too — Java static-method
        # inheritance, same rule as base_mesh.
        search = class_chain(owner, sources) or [owner]
    else:
        search = list(ctx.get("chain", ())) or list(sources)
    for cls in search:
        got = method_signature(sources.get(cls, ""), meth)
        if not got:
            continue
        params, body = got
        env, denv = {}, {}
        for (ptype, pname), arg in zip(params, args):
            if ptype == "CubeDeformation":
                denv[pname] = evaldeform(arg, ctx.get("env", {}), ctx.get("denv", {}))
            else:
                env[pname] = evalnum(arg, ctx.get("env", {}))
        chain = class_chain(cls, sources)
        sub = dict(ctx, home=cls, body=body, env=env, denv=denv, chain=chain)
        ret = re.search(r"return\s+([^;]+);", body)
        if ret:
            return eval_layer(ret.group(1), sub, depth + 1)
    return None


def cf(v):
    """A C++ float literal that always has a decimal point.

    `%g` renders 5.0 as "5", and "5f" is not a number in C++ — it is an
    invalid digit sequence. Every emitted float goes through here.
    """
    t = repr(round(float(v), 6))
    if "." not in t and "e" not in t and "E" not in t:
        t += ".0"
    return t + "f"


def main():
    if not os.path.isdir(MODEL_DIR):
        sys.exit(f"missing {MODEL_DIR} — run from the repo root")

    sources, paths = load_sources(MODEL_DIR)

    # Which mobs need a model.
    from subprocess import run as _run
    ent = open("src/common/entity/GeneratedEntityTypes.hpp", encoding="utf-8").read()
    slugs = re.findall(r'//\s*"([a-z_]+)"', ent)

    def camel(s):
        return "".join(p.capitalize() for p in s.split("_"))

    layers, lvars = load_layer_defs()
    ctx0 = {"sources": sources, "lvars": lvars}

    models, missing = {}, []
    for slug in slugs:
        if slug in HAND_WRITTEN or slug in PROJECTILES:
            continue
        # A 26.3-only mob's meshes come from the MC2 rows below (the sulfur
        # cube); 26.1 has nothing to look for.
        if slug in MC2_ONLY_MESHES:
            continue
        # MC's own build description, transformers and all. Falling back to the
        # model class is only for the handful with no ModelLayers row.
        layer = SLUG_LAYER.get(slug, slug.upper())
        got = None
        if layer in layers:
            got = eval_layer(layers[layer], dict(ctx0))
        if not got or not any(p.cubes for p in got["parts"]):
            cls = MODEL_ALIAS.get(slug, camel(slug) + "Model")
            cls, _, pin = cls.partition("#")
            if cls in paths:
                got = parse_model(cls, sources, pin or None)
        if not got or not any(p.cubes for p in got["parts"]):
            missing.append((slug, layer))
            continue
        cls_for_anim = MODEL_ALIAS.get(slug, camel(slug) + "Model").split("#")[0]
        (got["head"], got["headguard"],
         got["clips"], got["vis"]) = setup_anim_info(cls_for_anim, sources)
        got["cull"] = model_culls(cls_for_anim, sources)
        models[slug] = got

    # ── Baby meshes ────────────────────────────────────────────────────────
    #
    # MC renders a baby through a SEPARATE mesh, not a uniform shrink:
    # AgeableMobRenderer swaps to babyModel, whose LayerDefinitions row is
    # (usually) the adult layer run through the class's BABY_TRANSFORMER —
    # BabyModelTransform rewrites each root child's PartPose (big head, half
    # body). Part names survive, so a baby runs the SAME setupAnim program and
    # clips as its adult; only the mesh row differs.
    baby_skipped = []
    for slug in sorted(BABY_MESH_SLUGS):
        if slug not in models:
            baby_skipped.append((slug, "no adult mesh"))
            continue
        layer = SLUG_LAYER.get(slug, slug.upper()) + "_BABY"
        got = eval_layer(layers[layer], dict(ctx0)) if layer in layers else None
        if not got or not any(p.cubes for p in got["parts"]):
            baby_skipped.append((slug, "could not evaluate ModelLayers." + layer))
            continue
        cls_for_anim = MODEL_ALIAS.get(slug, camel(slug) + "Model").split("#")[0]
        (got["head"], got["headguard"],
         got["clips"], got["vis"]) = setup_anim_info(cls_for_anim, sources)
        got["cull"] = model_culls(cls_for_anim, sources)
        models[slug + "_baby"] = got

    # ── The 26.x baby remodel, from the second decompile ───────────────────
    remodel = remodel_meshes()
    if os.path.isdir(MODEL_DIR2):
        sources2, _ = load_sources(MODEL_DIR2)
        layers2, lvars2 = load_layer_defs(MODEL_DIR2)
        ctx2 = {"sources": sources2, "lvars": lvars2}
        for slug in sorted(remodel):
            layer, cls = remodel[slug]
            got = eval_layer(layers2[layer], dict(ctx2)) if layer in layers2 else None
            if not got or not any(p.cubes for p in got["parts"]):
                print(f"  WARNING: no remodel mesh for {slug} "
                      f"(MC2 ModelLayers.{layer})")
                continue
            (got["head"], got["headguard"],
             got["clips"], got["vis"]) = setup_anim_info(cls, sources2)
            got["cull"] = model_culls(cls, sources2, CULLS_MC2)
            models[slug] = got
    else:
        print(f"  WARNING: {MODEL_DIR2} missing — the baby remodel meshes "
              f"were not generated")

    # ── Extra layer meshes ─────────────────────────────────────────────────
    #
    # MC's clothing/decor RenderLayers draw a SECOND model over (or instead
    # of) the mob's body mesh: DrownedOuterLayer, SkeletonClothingLayer
    # (stray/bogged), and the pufferfish's mid/big puff stages
    # (PufferfishRenderer swaps the whole model by puff state). Each has its
    # own LayerDefinitions row, evaluated here exactly like a body layer.
    # The layer meshes keep the body's part names, so MobRenderer can run the
    # base mob's compiled setupAnim program over them (GeneratedModel's
    # animSlug); the setup_anim_info class below only supplies the head/clip
    # metadata for the row.
    EXTRA_LAYER_MESHES = (
        # (slug,                ModelLayers row,            setupAnim class)
        ("drowned_outer",       "DROWNED_OUTER_LAYER",      "DrownedModel"),
        ("drowned_outer_baby",  "DROWNED_BABY_OUTER_LAYER", "DrownedModel"),
        ("stray_clothes",       "STRAY_OUTER_LAYER",        "SkeletonModel"),
        ("bogged_clothes",      "BOGGED_OUTER_LAYER",       "SkeletonModel"),
        ("pufferfish_mid",      "PUFFERFISH_MEDIUM",        "PufferfishMidModel"),
        ("pufferfish_big",      "PUFFERFISH_BIG",           "PufferfishBigModel"),
        # MC BreezeWindLayer / BreezeEyesLayer: the breeze mesh again on its
        # own sheets — the wind layer is the SAME parts at a 128x128 sheet
        # (BreezeModel.createWindLayer), so a redraw of the body's vertices
        # cannot serve it; the UVs differ.
        ("breeze_wind",         "BREEZE_WIND",              "BreezeModel"),
        ("breeze_eyes",         "BREEZE_EYES",              "BreezeModel"),
    )
    for slug, layer, cls in EXTRA_LAYER_MESHES:
        got = eval_layer(layers[layer], dict(ctx0)) if layer in layers else None
        if not got or not any(p.cubes for p in got["parts"]):
            print(f"  WARNING: no mesh for extra layer {slug} "
                  f"(ModelLayers.{layer})")
            continue
        (got["head"], got["headguard"],
         got["clips"], got["vis"]) = setup_anim_info(cls, sources)
        got["cull"] = model_culls(cls, sources)
        models[slug] = got

    # ── Emit ───────────────────────────────────────────────────────────────
    guard_enum = ", ".join(GUARD_NAMES)
    hpp = f"""// GENERATED by tools/gen_entity_models.py — do not edit by hand.
//
// Mesh data for every mob without a hand-written model class: the part
// hierarchy, each part's rest pose, and its cubes, straight out of MC's
// `createBodyLayer`. Coordinates are MC MODEL SPACE — pixels, Y DOWN — so they
// match the decompile line for line and Render::ModelPart consumes them as-is.
//
// setupAnim is deliberately NOT here; see the generator's docstring.
#pragma once

#include <cstdint>
#include <string_view>

namespace Render {{

    struct GenCube {{
        float ox, oy, oz;      // origin, pixels
        float sx, sy, sz;      // size, pixels
        float tu, tv;          // texOffs
        // CubeDeformation, per axis — uniform everywhere except the equine
        // baby legs (AbstractEquineModel's g.extend(0, 5.5, 0)).
        float growX, growY, growZ;
        bool  mirror;
    }};

    struct GenPart {{
        std::string_view name;
        int   parent;          // index into the model's part span, -1 = root
        float x, y, z;         // PartPose offset, pixels
        float xRot, yRot, zRot;
        // PartPose scale — MC MeshTransformer.scaling lands here (on the
        // root), never in the cube geometry: UV layout derives from the
        // unscaled cube size, exactly as in MC.
        float xScale, yScale, zScale;
        int   firstCube, cubeCount;
        bool  visible;         // false = MC hides it in the default state
    }};

    // A boolean on the render state that gates a clip. Only five conditions
    // appear across all of MC's animated models, so this is a closed enum
    // rather than a compiled expression — an unrecognised one fails the build.
    enum class AnimGuard : uint8_t {{ {guard_enum} }};

    // One KeyframeAnimation application from a model's setupAnim.
    struct GenClip {{
        std::string_view anim;      // row in kGenAnims
        bool  isWalk;               // applyWalk (distance-driven) vs apply (timed)

        // applyWalk: MC's speed/scale factors, plus NautilusModel's two
        // argument tweaks (every other model passes the raw walk values).
        float speedFactor, scaleFactor;
        float posAgeScale, speedBias;

        // apply(AnimationState): which Game::MobAnim timer drives it. The
        // ordinal is Game::MobAnim's; the client stores the timers on the mob.
        uint8_t animSlot;

        AnimGuard guard;
        bool      guardNegate;
        // AnimGuard::AnimStarted — the Game::MobAnim slot whose running bit
        // is the guard.
        uint8_t   guardSlot;
    }};

    // MC `this.<part>.visible = state.<X>AnimationState.isStarted()` — the
    // frog's croaking throat sac. The part only exists while the clip runs.
    struct GenClipVisibility {{
        std::string_view part;
        uint8_t          animSlot;
    }};

    struct GenModel {{
        std::string_view slug;
        float texWidth, texHeight;

        // The part MC's setupAnim turns with the look direction, or "" when it
        // turns none. Not always literally "head".
        std::string_view headPart;
        // BatModel turns its head only while resting; everything else is
        // unconditional.
        AnimGuard headGuard;
        bool      headGuardNegate;

        int   firstPart, partCount;
        int   firstClip, clipCount;
        int   firstVis,  visCount;

        // MC Model.renderType picked a culling RenderType (entityCutout /
        // entitySolid / entityTranslucent) rather than the entityCutoutNoCull
        // default — see the generator's model_culls. The renderer draws this
        // mesh back-face culled, which is what keeps a bat's zero-thickness
        // ears from fighting their own back faces.
        bool  cull;
    }};

    inline constexpr int kGenModelCount = {len(models)};
    extern const GenModel kGenModels[kGenModelCount];
    extern const GenPart  kGenParts[];
    extern const GenCube  kGenCubes[];
    extern const GenClip  kGenClips[];
    extern const GenClipVisibility kGenClipVis[];

    // Mesh for a mob slug, or nullptr when it has a hand-written model class
    // (or none at all). Linear over ~{len(models)} entries, called once per type.
    const GenModel* FindGenModel(std::string_view slug);

}} // namespace Render
"""

    parts_rows, cubes_rows, model_rows = [], [], []
    clip_rows, vis_rows = [], []
    slot_index = read_mob_anim_slots()
    for slug in sorted(models):
        md = models[slug]
        # A baby mesh hides the same default-state parts as its adult — the
        # llama's chest packs and the turtle's egg belly exist in the baby
        # rows too.
        base_slug = slug[:-5] if slug.endswith("_baby") else slug
        if slug in remodel:
            base_slug = slug[:-4].removesuffix("_baby").removesuffix("_wool")
        ordered = [p for p in md["parts"]]
        index = {id(p): i for i, p in enumerate(ordered)}
        first_part = len(parts_rows)
        for p in ordered:
            fc = len(cubes_rows)
            for c in p.cubes:
                cubes_rows.append(
                    "    {{ {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {} }},".format(
                        cf(c["o"][0]), cf(c["o"][1]), cf(c["o"][2]),
                        cf(c["s"][0]), cf(c["s"][1]), cf(c["s"][2]),
                        cf(c["tu"]), cf(c["tv"]),
                        cf(c["grow"][0]), cf(c["grow"][1]), cf(c["grow"][2]),
                        "true" if c["mirror"] else "false"))
            parent = -1 if p.parent is None else index.get(id(p.parent), -1)
            visible = p.name not in HIDDEN_PARTS.get(base_slug, ())
            parts_rows.append(
                '    {{ "{}", {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {} }},'.format(
                    p.name, parent, cf(p.pose[0]), cf(p.pose[1]), cf(p.pose[2]),
                    cf(p.pose[3]), cf(p.pose[4]), cf(p.pose[5]),
                    cf(p.scale[0]), cf(p.scale[1]), cf(p.scale[2]),
                    fc, len(p.cubes),
                    "true" if visible else "false"))
        first_clip = len(clip_rows)
        for kind, anim, slot, sf, sc, guard, age, bias in md["clips"]:
            guard_slot = guard[2] if len(guard) > 2 and guard[2] else None
            clip_rows.append(
                '    {{ "{}", {}, {}, {}, {}, {}, {}, AnimGuard::{}, {}, {} }},'.format(
                    anim, "true" if kind == "Walk" else "false",
                    cf(sf), cf(sc), cf(age), cf(bias), slot_index[slot],
                    guard[0], "true" if guard[1] else "false",
                    slot_index[guard_slot] if guard_slot else 0))
        first_vis = len(vis_rows)
        for part, slot in md["vis"]:
            vis_rows.append('    {{ "{}", {} }},'.format(part, slot_index[slot]))

        hg = md["headguard"]
        model_rows.append(
            '    {{ "{}", {}, {}, "{}", AnimGuard::{}, {}, {}, {}, {}, {}, {}, {}, {} }},'.format(
                slug, cf(md["texw"]), cf(md["texh"]), md["head"],
                hg[0], "true" if hg[1] else "false",
                first_part, len(ordered),
                first_clip, len(md["clips"]), first_vis, len(md["vis"]),
                "true" if md.get("cull") else "false"))

    cpp = "\n".join([
        "// GENERATED by tools/gen_entity_models.py — do not edit by hand.",
        '#include "client/renderer/entity/model/GeneratedEntityModels.hpp"',
        "",
        "namespace Render {",
        "",
        "    const GenCube kGenCubes[] = {",
        *cubes_rows,
        "    };",
        "",
        "    const GenPart kGenParts[] = {",
        *parts_rows,
        "    };",
        "",
        "    const GenClip kGenClips[] = {",
        *clip_rows,
        "    };",
        "",
        "    const GenClipVisibility kGenClipVis[] = {",
        *vis_rows,
        "    };",
        "",
        "    const GenModel kGenModels[kGenModelCount] = {",
        *model_rows,
        "    };",
        "",
        "    const GenModel* FindGenModel(std::string_view slug) {",
        "        for (const GenModel& m : kGenModels) {",
        "            if (m.slug == slug) return &m;",
        "        }",
        "        return nullptr;",
        "    }",
        "",
        "} // namespace Render",
        "",
    ])

    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp)
    baby_count = sum(1 for s in models if s.endswith("_baby") or s in remodel)
    print(f"{OUT_HPP}: {len(models)} meshes ({baby_count} baby), "
          f"{len(parts_rows)} parts, {len(cubes_rows)} cubes")
    if missing:
        print(f"  no mesh for {len(missing)}:")
        for slug, cls in missing:
            print(f"    {slug:24s} (looked for {cls})")
    for slug, why in baby_skipped:
        print(f"  WARNING: no baby mesh for {slug}: {why}")


if __name__ == "__main__":
    main()
