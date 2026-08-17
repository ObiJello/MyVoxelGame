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

import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
MODEL_DIR = os.path.join(MC, "client/model")
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

# MC MeshTransformer.scaling(f), baked in rather than carried as a per-part
# scale. LayerDefinitions maps these mobs to `createBodyLayer().apply(SCALE)`,
# where scaling(f) is
#     pose -> pose.scaled(f).translated(0, 24.016 * (1 - f), 0)
# A uniform scale of a rigid hierarchy is the same whether it is applied at the
# pose or multiplied into the geometry — no part carries its own scale, and the
# rotations are untouched — so multiplying every offset, origin and size by f
# and shifting the root by yOffset reproduces it exactly, with no scale field
# on GenPart.
MESH_SCALE = {
    "elder_guardian": 2.35,   # GuardianModel.ELDER_GUARDIAN_SCALE
}

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
    # `.mirror(true)` and `.mirror(mirrorLeftLeg)` both come through here, and
    # a bare `true` is not a Python expression — it evaluated to the 0.0
    # fallback, i.e. NOT mirrored, silently flipping those cubes' UVs.
    e = re.sub(r"\btrue\b", "1", e)
    e = re.sub(r"\bfalse\b", "0", e)
    for k, v in env.items():
        e = re.sub(r"\b" + re.escape(k) + r"\b", repr(v), e)
    try:
        return float(eval(e, {"__builtins__": {}}, {}))
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
        self.cubes = []               # dicts


DEFORM_HINT = re.compile(r"CubeDeformation|\.extend\s*\(|deformation|inflate|fudge",
                         re.I)


def is_deform(expr, denv):
    """Does this addBox argument denote a CubeDeformation?"""
    e = expr.strip()
    return bool(DEFORM_HINT.search(e)) or e in denv


def evaldeform(expr, env, denv):
    """A CubeDeformation expression -> its uniform grow, in pixels.

    MC inflates a cube by this on every axis (ModelPart.Cube's constructor).
    Two coincident boxes with DIFFERENT deformations are how MC layers a shell
    over a body — armadillo, wolf, sheep, every 'outer layer'. Dropping the
    value collapses them onto each other and they z-fight, which is exactly
    what this used to do: `evalnum("new CubeDeformation(0.3F)")` cannot be
    eval'd as Python, so it returned the 0.0 fallback for all 138 of them.

    Only the uniform (single-argument) constructor appears in MC's model
    sources — the growX/growY/growZ form lives inside CubeDeformation itself —
    so one float is enough and GenCube keeps a single `grow`.
    """
    e = expr.strip()
    if e in denv:
        return denv[e]
    if e.endswith("NONE") and "CubeDeformation" in e:
        return 0.0

    # `<base>.extend(f)` -> base + f, recursively.
    m = re.search(r"\.extend\s*\(", e)
    if m:
        inner, end = balanced(e, m.end() - 1)
        if end >= len(e.rstrip()):
            args = split_args(inner)
            base = evaldeform(e[:m.start()], env, denv)
            return base + evalnum(args[0], env)

    m = re.match(r"new\s+CubeDeformation\s*\(", e)
    if m:
        inner, _ = balanced(e, m.end() - 1)
        args = split_args(inner)
        # The 3-arg form is per-axis; MC's models never use it, but if a
        # snapshot introduces one, taking X keeps the common case right.
        return evalnum(args[0], env) if args else 0.0

    return evalnum(e, env)


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
                grow, cube_tex = 0.0, tex

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


def unroll_loops(body, src, env):
    """Expand literal-bounded for loops, resolving `"prefix" + i` part names."""
    name_fns = {m.group(1): m.group(2) for m in NAME_FN.finditer(src)}

    out, i = "", 0
    while True:
        m = re.compile(r"\bfor\s*\(").search(body, i)
        if not m:
            out += body[i:]
            break
        out += body[i:m.start()]
        head, end = balanced(body, m.end() - 1)
        rest = body[end:].lstrip()
        if not rest.startswith("{"):
            out += body[m.start():end]
            i = end
            continue
        inner, e2 = balanced(rest, 0, "{", "}")
        consumed = end + (len(body[end:]) - len(rest)) + e2

        parts = head.split(";")
        mi = re.fullmatch(r"\s*(?:int\s+)?(\w+)\s*=\s*(-?\d+)\s*", parts[0]) if len(parts) == 3 else None
        mc = re.fullmatch(r"\s*(\w+)\s*<\s*(-?\d+)\s*", parts[1]) if mi else None
        if not mi or not mc or mi.group(1) != mc.group(1) or int(mc.group(2)) - int(mi.group(2)) > 64:
            out += body[m.start():consumed]
            i = consumed
            continue

        var, lo, hi = mi.group(1), int(mi.group(2)), int(mc.group(2))
        # Loop-carried accumulators (`++angle`) are evaluated per iteration and
        # folded into the emitted text, since the unrolled copies are otherwise
        # independent and would all read the initial value.
        carried = {}
        for cm in re.finditer(r"(?:\+\+\s*(\w+)|(\w+)\s*\+\+)\s*;", inner):
            nm = cm.group(1) or cm.group(2)
            carried[nm] = 1.0
        for k in range(lo, hi):
            it = inner
            # `getPartName(i)` -> the literal name it would return.
            for fn, prefix in name_fns.items():
                it = re.sub(r"\b" + re.escape(fn) + r"\s*\(\s*" + re.escape(var)
                            + r"\s*\)", '"%s%d"' % (prefix, k), it)
            it = re.sub(r"\b" + re.escape(var) + r"\b", str(k), it)
            for nm, step in carried.items():
                # Replace reads of the accumulator with `nm + <n steps>`.
                bump = (k - lo) * step
                it = re.sub(r"\b" + re.escape(nm) + r"\b(?!\s*(?:\+\+|=))",
                            "(%s + %g)" % (nm, bump), it)
            out += it + "\n"
        i = consumed
    return out


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
    search = [owner] if owner else list(chain)
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
        return run_mesh(body, src, sources, chain, cenv, cdenv)
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
    for m in re.finditer(
            r"PartDefinition\s+(\w+)\s*=\s*(\w+)((?:\.getChild\(\s*\"[a-z_0-9]+\"\s*\))+)", body):
        cur = vars_.get(m.group(2), root)
        for name in re.findall(r'getChild\(\s*"([a-z_0-9]+)"', m.group(3)):
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
    body = unroll_loops(body, src, env)

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
    """Bake MC MeshTransformer.scaling(f) into the parsed mesh. See MESH_SCALE."""
    y_offset = 24.016 * (1.0 - f)
    root = model["root"]
    for p in model["parts"]:
        for i in range(3):
            p.pose[i] *= f
        for c in p.cubes:
            c["o"] = [v * f for v in c["o"]]
            c["s"] = [v * f for v in c["s"]]
            c["grow"] *= f
    # PartDefinition.transformed rewrites only ITS OWN pose (children are copied
    # verbatim), so the translate lands on the root alone. The root itself is
    # dropped from the emitted table when it has no cubes, so carry the shift to
    # its direct children — one level down is the same transform, two would move
    # every grandchild twice.
    #
    # It is added AFTER the scaling because MC's ModelPart translates before it
    # scales: the root's offset is in unscaled units and only the subtree below
    # it is multiplied.
    if root in model["parts"]:
        root.pose[1] += y_offset
    else:
        # The root is dropped from the emitted table when it carries no cubes;
        # carry the shift to its direct children instead. Doing BOTH moves every
        # child twice.
        for p in model["parts"]:
            if p.parent is None or p.parent is root:
                p.pose[1] += y_offset


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
    "idleAnimationState":            "Idle",
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
               "IsHoldingItem"]


ANIM_STATE_HPP = "src/common/entity/AnimationState.hpp"


def read_mob_anim_slots():
    """Game::MobAnim's ordinals, read from the C++ header.

    The generated clip rows carry the slot as a raw ordinal, so the two enums
    must agree. Reading the real one — instead of re-deriving the order here —
    turns a rename or an inserted slot into a generator error rather than every
    animation silently playing on the wrong timer.
    """
    src = open(ANIM_STATE_HPP, encoding="utf-8").read()
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
            key = norm(cond)
            if key not in CLIP_GUARD:
                # Not a clip guard — recurse anyway so a clip nested under an
                # unrelated condition (the armadillo's scared branch) is still
                # found, with the OUTER guard.
                scan_clips(then_body, bake, guard, out, vis)
                if tail.startswith("else"):
                    t2 = tail[4:].lstrip()
                    if t2.startswith("{"):
                        scan_clips(balanced(t2, 0, "{", "}")[0], bake, guard, out, vis)
                continue
            name, negate = CLIP_GUARD[key]
            scan_clips(then_body, bake, (name, negate), out, vis)
            if tail.startswith("else"):
                t2 = tail[4:].lstrip()
                if t2.startswith("{"):
                    scan_clips(balanced(t2, 0, "{", "}")[0], bake,
                               (name, not negate), out, vis)
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
        bake = dict(BAKE_FIELD.findall(sources[c]))
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


def load_layer_defs():
    """(layer name -> expression, local name -> expression)."""
    src = strip_comments(open(os.path.join(MODEL_DIR,
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


def eval_transformer(expr, ctx):
    """A MeshTransformer expression -> ("scale", f) | ("meshfn", cls, meth) | None."""
    e = expr.strip()
    sources, lvars = ctx["sources"], ctx["lvars"]

    if e in lvars:
        return eval_transformer(lvars[e], ctx)
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
    # so `home` must survive the call.
    home = ctx.get("home")
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", e) and home in sources:
        d = re.search(r"MeshTransformer\s+" + e + r"\s*=\s*([^;]+);", sources[home])
        if d:
            return eval_transformer_in(d.group(1), home, ctx)
    return eval_transformer_in(e, home, ctx)


def eval_transformer_in(expr, cls, ctx):
    """As eval_transformer, but for an expression written inside `cls`."""
    e = expr.strip()
    m = re.match(r"MeshTransformer\s*\.\s*scaling\s*\(", e)
    if m:
        inner, _ = balanced(e, m.end() - 1)
        return ("scale", evalnum(inner, ctx.get("env", {})))
    # `(mesh) -> { modifyMesh(mesh.getRoot()); return mesh; }` — a lambda that
    # bolts extra parts on. DonkeyModel's is the only one reachable from a mob
    # layer, and it is what adds the chest packs and the long ears.
    m = re.search(r"(\w+)\s*\(\s*\w+\s*\.\s*getRoot\s*\(\s*\)\s*\)", e)
    if m and cls:
        return ("meshfn", cls, m.group(1))
    return None


def apply_transformer(model, tf, ctx):
    if tf is None:
        return
    if tf[0] == "scale":
        apply_mesh_scale(model, tf[1])
    elif tf[0] == "meshfn":
        _, cls, meth = tf
        sources = ctx["sources"]
        got = method_signature(sources.get(cls, ""), meth)
        if not got:
            return
        _, body = got
        run_mesh(body, sources[cls], sources, class_chain(cls, sources),
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
    search = [owner] if owner in sources else list(ctx.get("chain", ())) or list(sources)
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

    sources = {}
    paths = {}
    for r, _, fs in os.walk(MODEL_DIR):
        for f in fs:
            if f.endswith(".java"):
                p = os.path.join(r, f)
                sources[f[:-5]] = strip_comments(open(p, encoding="utf-8").read())
                paths[f[:-5]] = p

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
        if slug in HAND_WRITTEN or slug == "arrow":
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
        float grow;            // CubeDeformation
        bool  mirror;
    }};

    struct GenPart {{
        std::string_view name;
        int   parent;          // index into the model's part span, -1 = root
        float x, y, z;         // PartPose offset, pixels
        float xRot, yRot, zRot;
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
        ordered = [p for p in md["parts"]]
        index = {id(p): i for i, p in enumerate(ordered)}
        first_part = len(parts_rows)
        for p in ordered:
            fc = len(cubes_rows)
            for c in p.cubes:
                cubes_rows.append(
                    "    {{ {}, {}, {}, {}, {}, {}, {}, {}, {}, {} }},".format(
                        cf(c["o"][0]), cf(c["o"][1]), cf(c["o"][2]),
                        cf(c["s"][0]), cf(c["s"][1]), cf(c["s"][2]),
                        cf(c["tu"]), cf(c["tv"]), cf(c["grow"]),
                        "true" if c["mirror"] else "false"))
            parent = -1 if p.parent is None else index.get(id(p.parent), -1)
            visible = p.name not in HIDDEN_PARTS.get(slug, ())
            parts_rows.append(
                '    {{ "{}", {}, {}, {}, {}, {}, {}, {}, {}, {}, {} }},'.format(
                    p.name, parent, cf(p.pose[0]), cf(p.pose[1]), cf(p.pose[2]),
                    cf(p.pose[3]), cf(p.pose[4]), cf(p.pose[5]), fc, len(p.cubes),
                    "true" if visible else "false"))
        first_clip = len(clip_rows)
        for kind, anim, slot, sf, sc, guard, age, bias in md["clips"]:
            clip_rows.append(
                '    {{ "{}", {}, {}, {}, {}, {}, {}, AnimGuard::{}, {} }},'.format(
                    anim, "true" if kind == "Walk" else "false",
                    cf(sf), cf(sc), cf(age), cf(bias), slot_index[slot],
                    guard[0], "true" if guard[1] else "false"))
        first_vis = len(vis_rows)
        for part, slot in md["vis"]:
            vis_rows.append('    {{ "{}", {} }},'.format(part, slot_index[slot]))

        hg = md["headguard"]
        model_rows.append(
            '    {{ "{}", {}, {}, "{}", AnimGuard::{}, {}, {}, {}, {}, {}, {}, {} }},'.format(
                slug, cf(md["texw"]), cf(md["texh"]), md["head"],
                hg[0], "true" if hg[1] else "false",
                first_part, len(ordered),
                first_clip, len(md["clips"]), first_vis, len(md["vis"])))

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
    print(f"{OUT_HPP}: {len(models)} meshes, "
          f"{len(parts_rows)} parts, {len(cubes_rows)} cubes")
    if missing:
        print(f"  no mesh for {len(missing)}:")
        for slug, cls in missing:
            print(f"    {slug:24s} (looked for {cls})")


if __name__ == "__main__":
    main()
