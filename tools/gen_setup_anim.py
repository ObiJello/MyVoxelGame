#!/usr/bin/env python3
"""Compile MC's setupAnim methods to data.

src/client/renderer/entity/model/GeneratedSetupAnim.{hpp,cpp}

WHY A COMPILER. The 74 mobs without a KeyframeAnimation animate through
`setupAnim`, which is arbitrary Java — but in practice it is arbitrary Java of a
very narrow shape: float locals, `Mth.cos/sin/clamp/lerp/...`, and assignments
onto `this.<part>.<xyz|xRot|yRot|zRot>`. 1268 statements across 65 classes, and
118 of them are a cosine. Hand-porting that is 1268 chances to fat-finger a
constant, and a wrong constant in an animation is invisible in review and
obvious in game. So it is parsed and emitted as a tiny postfix program, the same
way the meshes and keyframes already are.

WHAT IT CANNOT DO, and why that is a real limit rather than a shortcut: MC's
LivingEntityRenderState carries ~140 fields, and the server never sends most of
them. `WolfModel.setupAnim` reads isSitting, headRollAngle, tailAngle and
shakeAnim; `PandaModel` reads sneezeTime, sitAmount and lieOnBackAmount. Those
are not animation constants that can be looked up — they are entity state this
port does not replicate. A statement reading one is SKIPPED and counted, so the
coverage number below is honest rather than optimistic.

Control flow: `if` is compiled when its condition is a supported boolean, and
both branches are emitted with a guard. `for` loops (7 across all models,
all tentacle/segment loops) are skipped.

    python3 tools/gen_setup_anim.py
"""

import math
import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
MODEL_DIR = os.path.join(MC, "client/model")
MODELS_CPP = "src/client/renderer/entity/model/GeneratedEntityModels.cpp"
OUT_HPP = "src/client/renderer/entity/model/GeneratedSetupAnim.hpp"
OUT_CPP = "src/client/renderer/entity/model/GeneratedSetupAnim.cpp"

# Render-state fields this port actually carries. Anything else makes the
# statement unportable — see the docstring.
STATE_FLOAT = {
    "walkAnimationPos": "WalkPos",
    "walkAnimationSpeed": "WalkSpeed",
    "ageInTicks": "AgeInTicks",
    "xRot": "XRot",
    "yRot": "YRot",
    "attackTime": "AttackTime",
    # IllagerRenderState.attackAnim = entity.getAttackAnim(partialTicks) —
    # the same 0..1 swing progress attackTime carries everywhere else.
    "attackAnim": "AttackTime",
    "ageScale": "AgeScale",
    "flapAngle": "Flap",
    "flap": "Flap",
    "flapSpeed": "FlapSpeed",
    # HumanoidMobRenderer:62 sets this to 1.0 and only changes it while
    # fall-flying, which no mob does. It divides every humanoid limb swing, so
    # treating it as unsupported dropped 48 statements over a constant.
    "speedValue": "SpeedValue",
    "swimAmount": "SwimAmount",
    # ArmPose as its ordinal; our C++ enum is declared in MC's order.
    "rightArmPose": "RightArmPose",
    "leftArmPose": "LeftArmPose",
    # HumanoidArm ordinal (LEFT=0, RIGHT=1). Every mob in this port is
    # right-handed, which is what MC's default is too.
    "mainArm": "MainArm",
    "attackArm": "MainArm",

    # ── Per-mob render-state floats (see EntityRenderState) ────────────────
    #
    # Each is populated by MobRenderer from real entity state where the game
    # tracks it, and otherwise carries the truthful default for a mob whose
    # behaviour this port does not run yet (a horse that never rears has
    # standAnimation 0 — that is MC's value for it, not a stand-in).
    "squish": "Squish",                            # slime family
    "flapTime": "FlapTime",                        # phantom
    "tentacleAngle": "TentacleAngle",              # squid
    "eatAnimation": "EatAnim",                     # equines
    "standAnimation": "StandAnim",
    "feedingAnimation": "FeedingAnim",
    "playingDeadFactor": "PlayingDead",            # axolotl
    "inWaterFactor": "InWaterFactor",
    "onGroundFactor": "OnGroundFactor",
    "movingFactor": "MovingFactor",
    "standScale": "StandScale",                    # polar bear
    "rammingXHeadRot": "RammingXHeadRot",          # goat
    "attackTicksRemaining": "AttackTicksRemaining",# iron golem, ravager
    "attackAnimationRemainingTicks": "AttackAnimRemaining",  # hoglin
    "stunnedTicksRemaining": "StunnedTicks",       # ravager
    "jumpCompletion": "JumpCompletion",            # rabbit
    "holdingAnimationProgress": "HoldingProgress", # allay
    "tendrilAnimation": "TendrilAnim",             # warden
    "spikesAnimation": "SpikesAnim",               # guardian
    "tailAnimation": "TailAnim",                   # guardian
    "entityId": "EntityId",                        # witch nose wiggle
    "jumpCooldown": "JumpCooldown",                # camel
    "headRollAngle": "HeadRollAngle",              # wolf
    "tailAngle": "TailAngle",                      # wolf
    "lieDownAmount": "LieDown",                    # cat/ocelot
    "relaxStateOneAmount": "RelaxOne",
    "sitAmount": "SitAmount",                      # panda
    "lieOnBackAmount": "LieOnBack",
    "rollAmount": "RollAmount",                    # panda roll / bee hover roll
    "crouchAmount": "CrouchAmount",                # fox
    "peekAmount": "PeekAmount",                    # shulker
    "spinningProgress": "SpinningProgress",        # allay
    "sneezeTime": "SneezeTime",                    # panda
    "lieDownAmountTail": "LieDownTail",            # cat/ocelot
    "offerFlowerTick": "OfferFlowerTick",          # iron golem
    "roarAnimation": "RoarAnim",                   # ravager
    # Shulker head math wants ABSOLUTE head/body yaw, unlike yRot which is
    # already head-relative-to-body.
    "yHeadRot": "YHeadRotAbs",
    "yBodyRot": "YBodyRotAbs",
    # Per-mob pose enums as ordinals (see ENUM_ORDINALS): armPose is the
    # illager/piglin arm pose, pose is ParrotModel.Pose.
    "armPose": "MobArmPose",
    "pose": "MobPose",
    # SwingAnimationType ordinal; runtime default WHACK (1) — the empty-hand
    # swing, which is what every mob in this port attacks with.
    "swingAnimationType": "SwingAnimType",
}

# `state.<method>(...)` calls whose value is a compile-time constant in this
# port, each because the behaviour that would change it does not run yet:
# getBodyRollAngle is the wolf's shake roll (never shakes -> 0), and bodyItem
# is the happy ghast's harness slot (never equipped -> isEmpty() true).
STATE_CONST_CALLS = {
    "getBodyRollAngle": 0.0,
}

ARM_SIDE = {"LEFT": 0, "RIGHT": 1}

# HumanoidModel.ArmPose's two boolean columns, in declaration order. MC calls
# them as methods (`leftArmPose.isTwoHanded()`), so they compile to a lookup
# against the pose ordinal already on the tape.
ARM_POSE_TWO_HANDED = [False, False, False, True, False, True, True,
                       False, False, False, False]
ARM_POSE_AFFECTS_OFFHAND = [False, False, False, True, True, True, True,
                            False, False, False, True]
STATE_BOOL = {
    "isAggressive": "IsAggressive",
    "isBaby": "IsBaby",
    "isCrouching": "IsCrouching",
    "isSprinting": "IsSprinting",
    "isInWater": "IsInWater",
    "isOnGround": "IsOnGround",
    # State a generated mob genuinely never enters. Supplying them as false is
    # not a stand-in: a cow IS not a passenger and IS not using an item, so the
    # branch MC would take is the one we take.
    "isFallFlying": "IsFallFlying",
    "isPassenger": "IsPassenger",
    "isRiding": "IsPassenger",
    "isUsingItem": "IsUsingItem",
    "isSitting": "IsSitting",
    "isHoldingBow": "IsHoldingBow",
    # Armadillo. The whole rolled-up look — body hidden, legs and tail off, the
    # 10x10x10 shell cube shown — hangs off this one boolean, so without it the
    # armadillo plays the roll-up animation and stays armadillo-shaped.
    "isHidingInShell": "IsHidingInShell",

    # ── Per-mob render-state booleans ──────────────────────────────────────
    "isResting": "IsResting",          # bat
    "canMove": "CanMove",              # creaking
    "isSearching": "IsSearching",      # sniffer
    "isHoldingItem": "IsHoldingItem",  # witch / copper golem
    "isMoving": "IsMoving",            # dolphin
    "animateTail": "AnimateTail",      # equines
    "hasChest": "HasChest",            # donkey/mule/llama
    "hasLeftHorn": "HasLeftHorn",      # goat
    "hasRightHorn": "HasRightHorn",
    "hasEgg": "HasEgg",                # turtle
    "isOnLand": "IsOnLand",
    "isAngry": "IsAngry",              # bee/wolf
    "hasStinger": "HasStinger",        # bee
    "isSheared": "IsSheared",          # bogged
    "isUnhappy": "IsUnhappy",          # villager
    "isCharging": "IsCharging",        # vex
    "isRidden": "IsRidden",            # strider
    "isCreepy": "IsCreepy",            # enderman
    "isDancing": "IsDancing",          # allay/piglin
    "isFaceplanted": "IsFaceplanted",  # fox
    "isSwimming": "IsSwimming",        # drowned
    "isSleeping": "IsSleeping",        # fox/cat
    "isSpinning": "IsSpinning",        # allay
    "isLayingEgg": "IsLayingEgg",      # turtle
    "isSneezing": "IsSneezing",        # panda
    "isEating": "IsEating",            # panda
    "isScared": "IsScared",            # panda
}

# MC HumanoidModel.ArmPose ordinals, mirrored by Render::ArmPose.
ARM_POSE = {n: i for i, n in enumerate([
    "EMPTY", "ITEM", "BLOCK", "BOW_AND_ARROW", "THROW_TRIDENT",
    "CROSSBOW_CHARGE", "CROSSBOW_HOLD", "SPYGLASS", "TOOT_HORN",
    "BRUSH", "SPEAR"])}

# Enum constants that appear in setupAnim comparisons/switches, as ordinals.
# Each list is read from its MC declaration order — the switch statements
# compare against `.ordinal()`, so order is load-bearing. `mobArmPose` is the
# per-mob ArmPose field (IllagerArmPose / PiglinArmPose — different enums, but
# the same `state.armPose` field, populated per-mob by the renderer).
ENUM_ORDINALS = {
    # AbstractIllager.IllagerArmPose
    "IllagerArmPose": ["CROSSED", "ATTACKING", "SPELLCASTING", "BOW_AND_ARROW",
                       "CROSSBOW_HOLD", "CROSSBOW_CHARGE", "CELEBRATING",
                       "NEUTRAL"],
    # PiglinArmPose
    "PiglinArmPose": ["ATTACKING_WITH_MELEE_WEAPON", "CROSSBOW_HOLD",
                      "CROSSBOW_CHARGE", "ADMIRING_ITEM", "DANCING", "DEFAULT"],
    # ParrotModel.Pose
    "Pose": ["FLYING", "STANDING", "SITTING", "PARTY", "ON_SHOULDER"],
    # world/item/SwingAnimationType — the melee swing flavour. The render
    # state default (ArmedEntityRenderState) is WHACK, which is what an empty
    # hand swings with, so that is the runtime default too.
    "SwingAnimationType": ["NONE", "WHACK", "STAB"],
}

# Mth / Math calls we implement. argc is fixed per name.
FUNCS = {
    "cos": ("Cos", 1), "sin": ("Sin", 1), "abs": ("Abs", 1),
    "sqrt": ("Sqrt", 1), "floor": ("Floor", 1), "signum": ("Signum", 1),
    "min": ("Min", 2), "max": ("Max", 2),
    "clamp": ("Clamp", 3), "lerp": ("Lerp", 3),
    "rotLerpRad": ("RotLerpRad", 3), "rotLerp": ("RotLerp", 3),
    "wrapDegrees": ("WrapDegrees", 1),
    "triangleWave": ("TriangleWave", 2),
    "square": ("Square", 1),
    "cube": ("Cube", 1),
    "degreesDifferenceAbs": ("DegDiffAbs", 2),
}

PART_FIELDS = {"x": "X", "y": "Y", "z": "Z",
               "xRot": "XRot", "yRot": "YRot", "zRot": "ZRot",
               "xScale": "XScale", "yScale": "YScale", "zScale": "ZScale",
               # Assigned a boolean; the runtime stores `value != 0`.
               "visible": "Visible",
               # MC ModelPart.skipDraw — the armadillo's rolled-up body.
               "skipDraw": "SkipDraw"}

OPS = {"+": "Add", "-": "Sub", "*": "Mul", "/": "Div", "%": "Mod"}
CMP = {">": "Gt", "<": "Lt", ">=": "Ge", "<=": "Le", "==": "Eq", "!=": "Ne"}
LOGIC = {"&&": "And", "||": "Or"}


class Unsupported(Exception):
    pass


# ── Java source helpers ────────────────────────────────────────────────────

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def balanced(text, start, open_ch="{", close_ch="}"):
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


def method_body(src, name):
    m = re.search(r"(?:public|private|protected)\s+(?:static\s+)?void\s+"
                  + re.escape(name) + r"\s*\(([^)]*)\)\s*\{", src)
    if not m:
        return None
    body, _ = balanced(src, m.end() - 1)
    return body


def value_signature(src, name):
    """(params, body) for a method returning a value."""
    m = re.search(r"(?:public|private|protected)\s+(?:static\s+)?"
                  r"(?:float|double|int)\s+" + re.escape(name)
                  + r"\s*\(([^)]*)\)\s*\{", src)
    if not m:
        return None
    params = []
    for d in m.group(1).split(","):
        d = d.strip().replace("final ", "")
        parts = d.split()
        if len(parts) >= 2:
            params.append(parts[-1])
    body, _ = balanced(src, m.end() - 1)
    return params, body


def method_signature(src, name):
    """(params, body) for a void method — used to inline setupAnim's helpers.

    The optional `<T extends ...>` admits generic statics — AnimationUtils's
    `static <T extends UndeadRenderState> void animateZombieArms`, which is
    the whole zombie-family arm pose."""
    m = re.search(r"(?:public|private|protected)\s+(?:static\s+)?"
                  r"(?:<[^>]*>\s*)?void\s+"
                  + re.escape(name) + r"\s*\(([^)]*)\)\s*\{", src)
    if not m:
        return None
    params = []
    for d in m.group(1).split(","):
        d = d.strip().replace("final ", "")
        parts = d.split()
        if len(parts) >= 2:
            params.append(parts[-1])
    body, _ = balanced(src, m.end() - 1)
    return params, body


def superclass(src, cls):
    m = re.search(r"class\s+" + re.escape(cls) + r"(?:<[^>]*>)?\s+extends\s+(\w+)", src)
    return m.group(1) if m else None


def class_chain(cls, sources, seen=None):
    seen = seen or set()
    if cls in seen or cls not in sources:
        return []
    seen.add(cls)
    sup = superclass(sources[cls], cls)
    return [cls] + (class_chain(sup, sources, seen) if sup else [])


# ── Expression parsing ─────────────────────────────────────────────────────

TOKEN = re.compile(r"""
    \s*(?:
      (?P<num>[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?[FfDdLl]?)
    | (?P<name>[A-Za-z_][A-Za-z_0-9]*)
    | (?P<op><=|>=|==|!=|&&|\|\||\(|\)|\[|\]|\.|,|\+|-|\*|/|%|\?|:|!|<|>)
    )""", re.X)


def tokenize(s):
    out, i = [], 0
    while i < len(s):
        m = TOKEN.match(s, i)
        if not m:
            if s[i].isspace():
                i += 1
                continue
            raise Unsupported("token %r" % s[i:i + 12])
        i = m.end()
        for k in ("num", "name", "op"):
            if m.group(k) is not None:
                out.append((k, m.group(k)))
                break
    return out


class Parser:
    """Recursive descent over the Java expression grammar setupAnim uses."""

    def __init__(self, toks, ctx):
        self.t, self.i, self.ctx = toks, 0, ctx

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else (None, None)

    def eat(self, val=None):
        k, v = self.peek()
        if v is None or (val is not None and v != val):
            raise Unsupported("expected %r got %r" % (val, v))
        self.i += 1
        return v

    def parse(self):
        e = self.logical_or()
        if self.i != len(self.t):
            raise Unsupported("trailing %r" % (self.t[self.i:],))
        return e

    def logical_or(self):
        n = self.logical_and()
        while self.peek()[1] == "||":
            self.eat()
            n = ("logic", "||", n, self.logical_and())
        return n

    def logical_and(self):
        n = self.comparison()
        while self.peek()[1] == "&&":
            self.eat()
            n = ("logic", "&&", n, self.comparison())
        return n

    def comparison(self):
        n = self.ternary()
        while self.peek()[1] in CMP:
            op = self.eat()
            n = ("cmp", op, n, self.ternary())
        return n

    def ternary(self):
        c = self.additive()
        if self.peek()[1] == "?":
            self.eat("?")
            a = self.logical_or()
            self.eat(":")
            b = self.logical_or()
            return ("sel", c, a, b)
        return c

    def additive(self):
        n = self.multiplicative()
        while self.peek()[1] in ("+", "-"):
            op = self.eat()
            n = ("bin", op, n, self.multiplicative())
        return n

    def multiplicative(self):
        n = self.unary()
        while self.peek()[1] in ("*", "/", "%"):
            op = self.eat()
            n = ("bin", op, n, self.unary())
        return n

    def unary(self):
        k, v = self.peek()
        if v == "-":
            self.eat()
            return ("neg", self.unary())
        if v == "+":
            self.eat()
            return self.unary()
        if v == "!":
            self.eat()
            return ("not", self.unary())
        return self.primary()

    def primary(self):
        k, v = self.peek()

        if v == "(":
            # A cast — `(float)x`, `(double)x` — or a parenthesised expression.
            if self.peek(1)[0] == "name" and self.peek(1)[1] in (
                    "float", "double", "int", "long") and self.peek(2)[1] == ")":
                self.eat("(")
                self.eat()
                self.eat(")")
                return self.unary()
            self.eat("(")
            e = self.logical_or()
            self.eat(")")
            return e

        if k == "num":
            self.eat()
            return ("const", float(v.rstrip("FfDdLl")))

        if k != "name":
            raise Unsupported("primary %r" % (v,))

        # A dotted path: Math.PI, Mth.cos(...), state.xRot, this.head.xRot, local
        path = [self.eat()]
        while self.peek()[1] == ".":
            self.eat(".")
            path.append(self.eat())

        if self.peek()[1] == "(":
            return self.call(path)

        # Indexed reads: `SPIKE_X[spike]` (a static float table — constant
        # once the index is), and `this.tailParts[0].x` (an indexed part).
        if self.peek()[1] == "[":
            self.eat("[")
            idx = self.logical_or()
            self.eat("]")
            if idx[0] != "const":
                raise Unsupported("index %s" % ".".join(path))
            k = int(idx[1])
            arrays = self.ctx.get("arrays") or {}
            if len(path) <= 2 and path[-1] in arrays:
                table = arrays[path[-1]]
                if 0 <= k < len(table):
                    return ("const", table[k])
                raise Unsupported("index %s[%d]" % (path[-1], k))
            if path[0] == "this" and len(path) == 2 and self.peek()[1] == ".":
                self.eat(".")
                field = self.eat()
                if field in PART_FIELDS:
                    return ("part", self.ctx["part_name"](path[1], k),
                            PART_FIELDS[field])
            raise Unsupported("index %s" % ".".join(path))

        return self.value(path)

    def call(self, path):
        self.eat("(")
        args = []
        if self.peek()[1] != ")":
            while True:
                args.append(self.logical_or())
                if self.peek()[1] == ",":
                    self.eat(",")
                    continue
                break
        self.eat(")")

        name = path[-1]
        owner = path[-2] if len(path) > 1 else ""

        # `state.<method>()` whose value is constant in this port — see
        # STATE_CONST_CALLS. `state.bodyItem.isEmpty()` and friends: no mob
        # here carries an item, so any state item-stack isEmpty() is true.
        if path[0] == "state" and name in STATE_CONST_CALLS:
            return ("const", STATE_CONST_CALLS[name])
        if path[0] == "state" and name == "isEmpty" and len(path) == 3:
            return ("const", 1.0)
        # `state.getMainHandItemState().isEmpty()` — an item-state getter whose
        # result is only ever tested for emptiness. The MAIN hand is a real
        # render-state boolean now (vindicator's axe drives the swingWeaponDown
        # branch of IllagerModel); the off hand stays constant-empty — nothing
        # in this port dual-wields. Any other use is unsupported.
        if path[0] == "state" and name in ("getMainHandItemState",
                                           "getOffhandItemState",
                                           "getOffHandItemState"):
            if self.peek()[1] == "." and self.peek(1)[1] == "isEmpty":
                self.eat(".")
                self.eat()
                self.eat("(")
                self.eat(")")
                if name == "getMainHandItemState":
                    return ("not", ("bstate", "HasMainHandItem"))
                return ("const", 1.0)
            raise Unsupported("call state.%s" % name)

        # `<armPoseExpr>.isTwoHanded()` / `.affectsOffhandPose()`.
        table = {"isTwoHanded": ARM_POSE_TWO_HANDED,
                 "affectsOffhandPose": ARM_POSE_AFFECTS_OFFHAND}.get(name)
        if table is not None and len(path) >= 2 and not args:
            base = self.value(path[:-1])
            expr = ("const", 0.0)
            for ordinal, flag in enumerate(table):
                if not flag:
                    continue
                test = ("cmp", "==", base, ("const", float(ordinal)))
                expr = ("logic", "||", expr, test)
            return expr

        # A value-returning helper — same-class (`this.quadraticArmUpdate(x)`,
        # bare static `getSpikeX(0, ...)`) or class-qualified
        # (`Ease.outQuart(x)`). Inlined with its arguments substituted as
        # SUBTREES, so operator precedence survives. Value helpers may call
        # each other (guardian's getSpikeX -> getSpikeOffset); the depth guard
        # in value_helper stops runaway recursion.
        if self.ctx.get("fnbody") and len(path) <= 2 \
                and owner not in ("Mth", "Math", "state"):
            inline = self.ctx["fnbody"](
                name, owner if owner not in ("this", "") else None)
            if inline:
                return inline(args)

        if owner not in ("Mth", "Math", ""):
            raise Unsupported("call %s" % ".".join(path))
        if name not in FUNCS:
            raise Unsupported("fn %s" % name)
        fn, argc = FUNCS[name]
        if len(args) != argc:
            raise Unsupported("fn %s argc %d" % (name, len(args)))
        return ("call", fn, args)

    def value(self, path):
        if path == ["Math", "PI"]:
            return ("const", math.pi)
        # `HumanoidModel.ArmPose.SPYGLASS` and friends -> their ordinal.
        if len(path) >= 2 and path[-2] == "ArmPose" and path[-1] in ARM_POSE:
            return ("const", float(ARM_POSE[path[-1]]))
        if len(path) >= 2 and path[-2] == "HumanoidArm" and path[-1] in ARM_SIDE:
            return ("const", float(ARM_SIDE[path[-1]]))
        # `AbstractIllager.IllagerArmPose.ATTACKING`, `PiglinArmPose.DANCING`,
        # `ParrotModel.Pose.PARTY` -> their ordinal, in MC declaration order.
        if len(path) >= 2 and path[-2] in ENUM_ORDINALS \
                and path[-1] in ENUM_ORDINALS[path[-2]]:
            return ("const", float(ENUM_ORDINALS[path[-2]].index(path[-1])))
        if len(path) == 2 and path[0] == "state":
            f = path[1]
            if f in STATE_FLOAT:
                return ("state", STATE_FLOAT[f])
            if f in STATE_BOOL:
                return ("bstate", STATE_BOOL[f])
            raise Unsupported("state.%s" % f)
        if len(path) == 1:
            n = path[0]
            at = self.ctx.get("argtrees")
            if at and n in at:
                return at[n]
            if n in self.ctx["locals"]:
                return ("local", self.ctx["locals"][n])
            if n in ("true", "false"):
                return ("const", 1.0 if n == "true" else 0.0)
            # A boolean local has no slot on the tape — it is re-expanded from
            # the expression it was declared with, so `rightHanded !=
            # twoHandedOffhand` resolves to the two state comparisons.
            src = self.ctx["bools"].get(n)
            if src:
                return self.ctx["expand"](src)
            raise Unsupported("name %s" % n)
        # `this.<part>.<field>` read — a part's current value.
        if path[0] == "this" and len(path) == 3 and path[2] in PART_FIELDS:
            return ("part", self.ctx["part_name"](path[1]), PART_FIELDS[path[2]])
        # `<alias>.<field>` read — a part reached through a local ModelPart
        # variable (`arm.xRot`, `holdingArm.xRot`).
        if len(path) == 2 and path[1] in PART_FIELDS:
            try:
                return ("part", self.ctx["part_name"](path[0]),
                        PART_FIELDS[path[1]])
            except Unsupported:
                pass
        # `this.<scalar>` read — a model field the bee stores state into
        # (`this.rollAmount = state.rollAmount`), compiled as a local slot.
        if path[0] == "this" and len(path) == 2 \
                and ("this." + path[1]) in self.ctx["locals"]:
            return ("local", self.ctx["locals"]["this." + path[1]])
        raise Unsupported("value %s" % ".".join(path))


# ── Statement compilation ──────────────────────────────────────────────────

ASSIGN_RE = re.compile(
    r"^(?:this\.)?(\w+)(?:\[\s*(\d+)\s*\])?\.(\w+)\s*(=|\+=|-=|\*=)\s*(.+)$", re.S)
LOCAL_RE = re.compile(r"^(?:final\s+)?[\w.]+(?:<[^>]*>)?\s+(\w+)\s*=\s*(.+)$", re.S)
LOCAL_OP_RE = re.compile(r"^(\w+)\s*(\+=|-=|\*=)\s*(.+)$", re.S)
ALIAS_RE = re.compile(r"^(?:final\s+)?(?:ModelPart\s+)?(\w+)\s*=\s*this\.(\w+)$", re.S)
BOOL_LOCAL_RE = re.compile(r"^(?:final\s+)?boolean\s+(\w+)\s*=\s*(.+)$", re.S)
# Statements with no effect on the pose. resetPose is the runtime's job (it runs
# before the program), and it appears in all 74 models.
IGNORE_RE = re.compile(r"^(?:this\.)?(?:resetPose|loadPose)\s*\(")


def split_args(text):
    """Split a call's argument text on top-level commas."""
    out, depth, cur = [], 0, ""
    for c in text:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def split_statements(body):
    """Top-level statements, keeping `if (...) { ... } else { ... }` whole.

    A block statement ends at its closing brace, NOT at a semicolon. Splitting
    only on `;` made every `if/else` swallow the statement that followed it —
    which is how HumanoidModel's `float animationPos = state.walkAnimationPos`
    disappeared, taking every arm and leg swing that referenced it with it.
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
        # A `for (init; cond; step)` header carries semicolons INSIDE parens.
        # Splitting on them tore every loop into three fragments, so the loop
        # bodies — blaze rods, wither heads, the dragon's neck — never compiled.
        if c == ";" and depth == 0 and paren == 0:
            out.append(cur[:-1].strip())
            cur = ""
        elif c == "}" and depth == 0 and paren == 0:
            # `} else ...` continues the same statement; anything else ends it.
            rest = body[i + 1:].lstrip()
            if not rest.startswith("else"):
                out.append(cur.strip())
                cur = ""
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return [s for s in out if s]


class Compiler:
    def __init__(self, fields, helpers=None, value_fns=None, arrays=None,
                 parts=None, mpart_fns=None):
        self.helpers = helpers
        self.value_fns = value_fns
        self.mpart_fns = mpart_fns  # ModelPart-returning helper -> body text
        self.fields = fields        # java field name -> mesh part name
        self.arrays = arrays or {}  # static float table name -> [values]
        self.parts = parts or set() # mesh part names, for getChild() chains
        self.locals = {}
        self.bools = {}
        self.aliases = {}
        self.nodes = []             # postfix expression nodes
        self.stmts = []             # (kind, ...)
        self.skipped = []
        self._inline_depth = 0
        # While inlining a helper's body, the class it was found in — so an
        # unqualified call inside AnimationUtils.bobArms resolves to
        # AnimationUtils.bobModelPart, not to a method of the model class.
        self.helper_owners = []
        # Locals bound from a LITERAL helper argument (`animateCrossbowHold(
        # ..., true)`) — the one case where a ModelPart ternary
        # (`holdingArm = holdingInRightArm ? rightArm : leftArm`) can be
        # resolved statically. name -> float value.
        self.const_locals = {}

    def value_helper(self, name, owner=None):
        """A `private float f(args) { return <expr>; }` helper, as an inliner."""
        if self.value_fns is None:
            return None
        got = self.value_fns(name, owner)
        if not got:
            return None
        params, body = got
        m = re.search(r"return\s+([^;]+);", body)
        if not m:
            return None
        src = m.group(1)

        def inline(args, _params=params, _src=src):
            if self._inline_depth > 6:
                raise Unsupported("helper recursion")
            self._inline_depth += 1
            try:
                ctx = {"locals": self.locals, "part_name": self.part_name,
                       "bools": self.bools, "expand": self.expand_bool,
                       "arrays": self.arrays,
                       "fnbody": self.value_helper,
                       "argtrees": dict(zip(_params, args))}
                return Parser(tokenize(_src), ctx).parse()
            finally:
                self._inline_depth -= 1

        return inline

    def expand_bool(self, text, depth=[0]):
        if depth[0] > 4:
            raise Unsupported("bool recursion")
        depth[0] += 1
        try:
            ctx = {"locals": self.locals, "part_name": self.part_name,
                   "bools": self.bools, "expand": self.expand_bool,
                   "arrays": self.arrays, "fnbody": self.value_helper}
            return Parser(tokenize(text), ctx).parse()
        finally:
            depth[0] -= 1

    def part_name(self, java_name, index=None):
        n = self.aliases.get(java_name, java_name)
        if index is not None:
            key = "%s[%d]" % (n, index)
            if key in self.fields:
                return self.fields[key]
            raise Unsupported("part %s[%s]" % (java_name, index))
        if n not in self.fields:
            raise Unsupported("part %s" % java_name)
        return self.fields[n]

    def compile_expr(self, text):
        ctx = {"locals": self.locals, "part_name": self.part_name,
               "bools": self.bools, "expand": self.expand_bool,
               "arrays": self.arrays,
               "fnbody": self.value_helper}
        tree = Parser(tokenize(text), ctx).parse()
        first = len(self.nodes)
        self.emit(tree)
        return first, len(self.nodes) - first

    def emit(self, n):
        kind = n[0]
        if kind == "const":
            self.nodes.append(("Const", n[1], 0, ""))
        elif kind == "state":
            self.nodes.append(("State", 0.0, 0, n[1]))
        elif kind == "bstate":
            self.nodes.append(("BState", 0.0, 0, n[1]))
        elif kind == "local":
            self.nodes.append(("Local", 0.0, n[1], ""))
        elif kind == "part":
            self.nodes.append(("Part", 0.0, 0, n[1] + "|" + n[2]))
        elif kind == "neg":
            self.emit(n[1])
            self.nodes.append(("Neg", 0.0, 0, ""))
        elif kind == "bin":
            self.emit(n[2])
            self.emit(n[3])
            self.nodes.append((OPS[n[1]], 0.0, 0, ""))
        elif kind == "cmp":
            self.emit(n[2])
            self.emit(n[3])
            self.nodes.append((CMP[n[1]], 0.0, 0, ""))
        elif kind == "logic":
            self.emit(n[2])
            self.emit(n[3])
            self.nodes.append((LOGIC[n[1]], 0.0, 0, ""))
        elif kind == "not":
            self.emit(n[1])
            self.nodes.append(("Not", 0.0, 0, ""))
        elif kind == "sel":
            self.emit(n[1])
            self.emit(n[2])
            self.emit(n[3])
            self.nodes.append(("Select", 0.0, 0, ""))
        elif kind == "call":
            for a in n[2]:
                self.emit(a)
            self.nodes.append((n[1], 0.0, len(n[2]), ""))
        else:
            raise Unsupported(kind)

    def run(self, body, guard=None):
        for s in split_statements(body):
            try:
                self.statement(s, guard)
            except Unsupported as e:
                self.skipped.append((s.split("\n")[0].strip()[:90], str(e)))

    def statement(self, s, guard):
        s = s.strip()
        if not s or s.startswith("super.") or s.startswith("//"):
            return
        if IGNORE_RE.match(s):
            return
        if s.startswith("for"):
            self.for_statement(s, guard)
            return
        if s.startswith("switch"):
            self.switch_statement(s, guard)
            return
        if s.startswith("while"):
            raise Unsupported("control flow")

        if s.startswith("if"):
            self.if_statement(s, guard)
            return

        m = BOOL_LOCAL_RE.match(s)
        if m:
            self.bools[m.group(1)] = m.group(2).strip()
            return

        m = ALIAS_RE.match(s)
        if m and (s.startswith("ModelPart") or s.startswith("final ModelPart")
                  or m.group(1) in self.aliases):
            # The decompiler's `ModelPart var10000 = this.head;` aliasing, and
            # the bare re-assignments that follow it.
            self.aliases[m.group(1)] = m.group(2)
            return

        # `ModelPart holdingArm = holdingInRightArm ? rightArm : leftArm` —
        # a part ternary whose condition is a compile-time constant (a
        # literal helper argument). AnimationUtils.animateCrossbowHold and
        # friends select their arms this way.
        m = re.fullmatch(r"(?:final\s+)?ModelPart\s+(\w+)\s*=\s*(\w+)\s*"
                         r"\?\s*(\w+)\s*:\s*(\w+)", s)
        if m and m.group(2) in self.const_locals:
            pick = m.group(3) if self.const_locals[m.group(2)] != 0.0 \
                              else m.group(4)
            self.aliases[m.group(1)] = self.aliases.get(pick, pick)
            return

        # `ModelPart attackArm = this.getArm(state.attackArm)` — a part-
        # returning helper whose body is `return <arm> == HumanoidArm.LEFT ?
        # this.A : this.B`. Every mob in this port is right-handed (mainArm is
        # the RIGHT constant — see STATE_FLOAT), so the RIGHT branch is the
        # statically-resolved alias.
        m = re.fullmatch(r"(?:final\s+)?ModelPart\s+(\w+)\s*=\s*"
                         r"this\.(\w+)\(\s*state\.(?:attackArm|mainArm)\s*\)", s)
        if m and self.mpart_fns is not None:
            body = self.mpart_fns(m.group(2))
            if body:
                bm = re.search(r"return\s+\w+\s*==\s*HumanoidArm\.LEFT\s*\?"
                               r"\s*this\.(\w+)\s*:\s*this\.(\w+)\s*;", body)
                if bm:
                    self.aliases[m.group(1)] = bm.group(2)
                    return
            raise Unsupported("call this.%s" % m.group(2))

        m = LOCAL_RE.match(s)
        if m:
            first, count = self.compile_expr(m.group(2))
            idx = len(self.locals)
            self.locals[m.group(1)] = idx
            self.stmts.append(("SetLocal", idx, "", "", first, count, guard))
            return

        # Bare re-assignment of an existing local — the decompiler's
        # `bounce = bounce * bounce ...`, salmon's `amplitudeMultiplier = 1.3F`,
        # the camel's `yRot = Mth.clamp(yRot, -30, 30)`.
        m = re.fullmatch(r"(\w+)\s*=\s*(.+)", s, re.S)
        if m and m.group(1) in self.locals:
            first, count = self.compile_expr(m.group(2))
            self.stmts.append(("SetLocal", self.locals[m.group(1)], "", "",
                               first, count, guard))
            return

        # A void helper call: `this.setupSpikes(...)`, a bare static
        # `setupHeadRotation(state, this.rightHead, 0)`, or a cross-class
        # `GhastModel.animateTentacles(state, this.tentacles)` /
        # `AnimationUtils.bobModelPart(this.rightArm, ...)`. ModelPart and
        # ModelPart[] arguments bind as part aliases so the inlined body's
        # writes land on the real parts.
        m = re.fullmatch(r"(?:(\w+)\.)?(\w+)\s*\((.*)\)", s, re.S)
        if m and self.helpers is not None:
            qual, name, argtext = m.groups()
            if qual == "this":
                qual = None
            owner = qual or (self.helper_owners[-1] if self.helper_owners else None)
            got = self.helpers(name, owner)
            if got:
                params, body, owner_cls = got
                args = split_args(argtext)
                saved = dict(self.locals)
                savedb = dict(self.bools)
                saveda = dict(self.aliases)
                savedf = dict(self.fields)
                savedc = dict(self.const_locals)
                # `state` is passed through by name; ModelPart (array) args
                # bind as aliases; the rest positionally as locals.
                for pname, arg in zip(params, args):
                    if pname == "state" or arg == "state":
                        continue
                    # `this.rightArm`, or a bare name that is itself a bound
                    # alias/param of the enclosing helper (bobArms passing its
                    # own ModelPart params down to bobModelPart).
                    pm = re.fullmatch(r"(?:this\.)?(\w+)", arg)
                    if pm:
                        fname = self.aliases.get(pm.group(1), pm.group(1))
                        if fname in self.fields:
                            self.aliases[pname] = fname
                            continue
                        arr = [k for k in self.fields
                               if re.fullmatch(re.escape(fname) + r"\[\d+\]", k)]
                        if arr:
                            for k in arr:
                                self.fields[pname + k[len(fname):]] = self.fields[k]
                            continue
                    try:
                        first, count = self.compile_expr(arg)
                    except Unsupported:
                        continue
                    idx = len(self.locals)
                    self.locals[pname] = idx
                    self.stmts.append(("SetLocal", idx, "", "", first, count, guard))
                    # A literal argument is also a compile-time constant —
                    # the ModelPart-ternary resolver keys off it.
                    if arg in ("true", "false"):
                        self.const_locals[pname] = 1.0 if arg == "true" else 0.0
                    else:
                        try:
                            self.const_locals[pname] = float(arg.rstrip("FfDdLl"))
                        except ValueError:
                            pass
                self.helper_owners.append(owner_cls)
                try:
                    self.run(body, guard)
                finally:
                    self.helper_owners.pop()
                    self.locals, self.bools = saved, savedb
                    self.aliases, self.fields = saveda, savedf
                    self.const_locals = savedc
                return
            if qual is None:
                raise Unsupported("call this.%s" % name)
            raise Unsupported("call %s.%s" % (qual, name))

        # `++angle` / `angle++`. Loop-carried accumulators: the blaze's rod
        # angle advances once per iteration, so dropping the increment gave all
        # twelve rods the same position.
        m = re.fullmatch(r"(?:\+\+|--)\s*(\w+)|(\w+)\s*(?:\+\+|--)", s)
        if m:
            name = m.group(1) or m.group(2)
            if name in self.locals:
                delta = "+ 1" if "++" in s else "- 1"
                first, count = self.compile_expr("%s %s" % (name, delta))
                self.stmts.append(("SetLocal", self.locals[name], "", "",
                                   first, count, guard))
                return
            raise Unsupported("name %s" % name)

        m = LOCAL_OP_RE.match(s)
        if m and m.group(1) in self.locals:
            idx = self.locals[m.group(1)]
            op = {"+=": "+", "-=": "-", "*=": "*"}[m.group(2)]
            first, count = self.compile_expr("%s %s (%s)" % (m.group(1), op, m.group(3)))
            self.stmts.append(("SetLocal", idx, "", "", first, count, guard))
            return

        # `this.nose.setPos(x, y, z)` — three axis writes in one call (witch
        # nose, shulker lid, wither tail).
        m = re.fullmatch(r"(?:this\.)?(\w+)\.setPos\s*\((.*)\)", s, re.S)
        if m:
            part = self.part_name(m.group(1))
            axes = split_args(m.group(2))
            if len(axes) != 3:
                raise Unsupported("setPos argc %d" % len(axes))
            for expr, field in zip(axes, ("X", "Y", "Z")):
                first, count = self.compile_expr(expr)
                self.stmts.append(("Set", 0, part, field, first, count, guard))
            return

        # `--this.root.y` / `this.root.y++` — the turtle's shell drop.
        m = re.fullmatch(r"(--|\+\+)\s*this\.(\w+)\.(\w+)"
                         r"|this\.(\w+)\.(\w+)\s*(--|\+\+)", s)
        if m:
            op = m.group(1) or m.group(6)
            recv = m.group(2) or m.group(4)
            field = m.group(3) or m.group(5)
            if field not in PART_FIELDS:
                raise Unsupported("field %s" % field)
            part = self.part_name(recv)
            first, count = self.compile_expr("1")
            self.stmts.append(("AddTo" if op == "++" else "SubFrom",
                               0, part, PART_FIELDS[field], first, count, guard))
            return

        # `this.head.getChild("left_horn").visible = ...` — the goat's horns.
        # The child name is a mesh part name directly (they are unique), so it
        # bypasses the java-field map.
        m = re.fullmatch(r"this\.\w+((?:\.getChild\(\s*\"[a-z_0-9]+\"\s*\))+)"
                         r"\.(\w+)\s*(=|\+=|-=|\*=)\s*(.+)", s, re.S)
        if m:
            chain, field, op, rhs = m.groups()
            child = re.findall(r'getChild\(\s*"([a-z_0-9]+)"\s*\)', chain)[-1]
            if field not in PART_FIELDS:
                raise Unsupported("field %s" % field)
            if child not in self.parts:
                raise Unsupported("part %s" % child)
            first, count = self.compile_expr(rhs)
            self.stmts.append((
                {"=": "Set", "+=": "AddTo", "-=": "SubFrom", "*=": "MulBy"}[op],
                0, child, PART_FIELDS[field], first, count, guard))
            return

        # `this.rollAmount = state.rollAmount` — a scalar MODEL field the bee
        # stores into and reads back. Compiled as a local slot keyed
        # "this.<name>"; Parser.value resolves reads against the same key.
        # Must run before ASSIGN_RE, whose backtracking would otherwise parse
        # it as part "this" field "rollAmount".
        m = re.fullmatch(r"this\.(\w+)\s*=\s*(.+)", s, re.S)
        if m and m.group(1) not in self.fields \
                and m.group(1) not in self.aliases:
            first, count = self.compile_expr(m.group(2))
            key = "this." + m.group(1)
            if key not in self.locals:
                self.locals[key] = len(self.locals)
            self.stmts.append(("SetLocal", self.locals[key], "", "",
                               first, count, guard))
            return

        m = ASSIGN_RE.match(s)
        if m:
            recv, index, field, op, rhs = m.groups()
            if field not in PART_FIELDS:
                raise Unsupported("field %s" % field)
            part = self.part_name(recv, int(index) if index is not None else None)
            first, count = self.compile_expr(rhs)
            self.stmts.append((
                {"=": "Set", "+=": "AddTo", "-=": "SubFrom", "*=": "MulBy"}[op],
                0, part, PART_FIELDS[field], first, count, guard))
            return

        raise Unsupported("statement")

    def for_statement(self, s, guard):
        """Unroll `for (int i = A; i < B; ++i) { ... }`.

        Blaze rods, wither heads and the dragon's neck are all segment loops
        over an array of parts. The bounds are always literal, so unrolling is
        exact — and it is the only way to reach the parts at all, since each
        iteration writes a different one.
        """
        m = re.match(r"for\s*\(", s)
        head, end = balanced(s, m.end() - 1, "(", ")")
        body_text = s[end:].lstrip()
        if not body_text.startswith("{"):
            raise Unsupported("control flow")
        body, _ = balanced(body_text, 0)

        def array_len(arr):
            return len([k for k in self.fields
                        if re.fullmatch(re.escape(arr) + r"\[\d+\]", k)])

        # Enhanced for over a part array — the squid's
        # `for (ModelPart tentacle : this.tentacles)`. Each iteration binds
        # the loop variable to that iteration's part.
        me = re.fullmatch(r"\s*(?:final\s+)?ModelPart\s+(\w+)\s*:\s*"
                          r"this\.(\w+)\s*", head)
        if me:
            var, arr = me.groups()
            n = array_len(arr)
            if n == 0:
                raise Unsupported("part %s[]" % arr)
            saved = dict(self.fields)
            try:
                for k in range(n):
                    self.fields[var] = self.fields["%s[%d]" % (arr, k)]
                    self.run(body, guard)
            finally:
                self.fields = saved
            return

        parts = head.split(";")
        if len(parts) != 3:
            raise Unsupported("control flow")
        mi = re.fullmatch(r"\s*(?:int\s+)?(\w+)\s*=\s*(-?\d+)\s*", parts[0])
        mc_ = re.fullmatch(r"\s*(\w+)\s*<\s*(-?\d+)\s*", parts[1])
        # `i < this.bodyCubes.length` — the bound is the number of indexed
        # entries the field map has for that array (magma cube, endermite,
        # and the ghast's inlined tentacle helper).
        ml = re.fullmatch(r"\s*(\w+)\s*<\s*(?:this\.)?(\w+)\s*\.\s*length\s*",
                          parts[1])
        if not mi or (not mc_ and not ml):
            raise Unsupported("control flow")
        if mc_ and mi.group(1) == mc_.group(1):
            hi = int(mc_.group(2))
        elif ml and mi.group(1) == ml.group(1):
            hi = array_len(ml.group(2))
            if hi == 0:
                raise Unsupported("part %s[]" % ml.group(2))
        else:
            raise Unsupported("control flow")
        var, lo = mi.group(1), int(mi.group(2))
        if hi - lo > 64:
            raise Unsupported("control flow")

        for k in range(lo, hi):
            # Substitute the induction variable, including inside `parts[i]`.
            it = re.sub(r"\b" + re.escape(var) + r"\b", str(k), body)
            self.run(it, guard)

    def switch_statement(self, s, guard):
        """Compile `switch (<expr>.ordinal()) { case N: ... }` to guarded runs.

        Java fall-through is preserved: a chunk with no top-level `break` lets
        the previous chunk's entry labels reach the next one, which is exactly
        how ParrotModel's STANDING case adds the leg swing and then falls into
        the shared body bob. `default` compiles to "none of the explicit
        labels". A `break` nested under an `if` would break the switch
        conditionally — unsupported, none of the models do it.
        """
        m = re.match(r"switch\s*\(", s)
        head, end = balanced(s, m.end() - 1, "(", ")")
        rest = s[end:].lstrip()
        if not rest.startswith("{"):
            raise Unsupported("control flow")
        body, _ = balanced(rest, 0)
        sel = re.sub(r"\.ordinal\(\)\s*$", "", head.strip())

        # Chunk the body at depth-0 `case N:` / `case NAME:` / `default:`
        # labels. Named labels are resolved to ordinals afterwards.
        chunks, cur_labels, cur_text = [], [], ""
        i, depth = 0, 0
        while i < len(body):
            if depth == 0:
                mlab = re.match(r"\s*(?:case\s+(\w+)|default)\s*:", body[i:])
                if mlab:
                    if cur_text.strip():
                        chunks.append((cur_labels, cur_text))
                        cur_labels, cur_text = [], ""
                    cur_labels.append(mlab.group(1)
                                      if mlab.group(1) is not None else "default")
                    i += mlab.end()
                    continue
            c = body[i]
            if c in "{(":
                depth += 1
            elif c in "})":
                depth -= 1
            cur_text += c
            i += 1
        if cur_text.strip() or cur_labels:
            chunks.append((cur_labels, cur_text))

        # Statements before the first label would run unguarded — malformed.
        if chunks and chunks[0][0] == [] :
            raise Unsupported("control flow")

        # Resolve labels: digits are ordinals already; names resolve against
        # the ONE enum in ENUM_ORDINALS containing every named label. A label
        # that resolves nowhere rejects the whole switch — anything less would
        # run its statements with the wrong guard.
        named = {l for labs, _ in chunks for l in labs
                 if l != "default" and not l.isdigit()}
        enum = None
        if named:
            candidates = [vals for vals in ENUM_ORDINALS.values()
                          if named <= set(vals)]
            if len(candidates) != 1:
                raise Unsupported("control flow")
            enum = candidates[0]

        def resolve(l):
            if l == "default":
                return l
            return int(l) if l.isdigit() else enum.index(l)

        chunks = [([resolve(l) for l in labs], text) for labs, text in chunks]
        all_labels = sorted({l for labs, _ in chunks for l in labs
                             if l != "default"})

        def chunk_breaks(text):
            # Top-level break only; a nested one is conditional control flow.
            d = 0
            for stmt in split_statements(text):
                if stmt == "break":
                    return True
                d += stmt.count("{") - stmt.count("}")
                if "break" in stmt and re.search(r"\bbreak\b", stmt):
                    raise Unsupported("control flow")
            return False

        entry = set()
        prev_falls = False
        for labs, text in chunks:
            entry = (entry if prev_falls else set()) | set(labs)
            prev_falls = not chunk_breaks(text)
            stripped = re.sub(r"\bbreak\s*;", "", text)
            if not stripped.strip():
                continue
            conds = []
            for l in sorted(entry, key=str):
                if l == "default":
                    if all_labels:
                        conds.append("(" + " && ".join(
                            "%s != %d" % (sel, x) for x in all_labels) + ")")
                    else:
                        conds = None
                        break
                else:
                    conds.append("(%s == %d)" % (sel, l))
            if conds is None or not conds:
                cg = None
            else:
                cg = self.guard_expr(" || ".join(conds))
            self.run(stripped, self.and_guard(guard, cg) if cg else guard)

    def if_statement(self, s, guard):
        m = re.match(r"if\s*\(", s)
        cond_text, end = balanced(s, m.end() - 1, "(", ")")
        rest = s[end:].lstrip()
        if not rest.startswith("{"):
            raise Unsupported("if without block")
        then_body, e2 = balanced(rest, 0)
        tail = rest[e2:].lstrip()
        else_body, else_chain = None, None
        if tail.startswith("else"):
            t2 = tail[4:].lstrip()
            if t2.startswith("{"):
                else_body, _ = balanced(t2, 0)
            elif t2.startswith("if"):
                else_chain = t2          # `else if (...) {...}` — compiled below
            else:
                raise Unsupported("else without block")

        # The condition is compiled like any other expression, so comparisons
        # (`swimAmount > 0.0F`), enum tests (`armPose != SPYGLASS`) and `&&`
        # chains all work. A nested if ANDs its guard onto the enclosing one.
        cond = self.guard_expr(cond_text)
        self.run(then_body, self.and_guard(guard, cond))
        inv = self.not_guard(cond)
        if else_body is not None:
            self.run(else_body, self.and_guard(guard, inv))
        elif else_chain is not None:
            self.if_statement(else_chain, self.and_guard(guard, inv))

    def guard_expr(self, text):
        """Compile a boolean expression; returns (firstNode, nodeCount)."""
        c = text.strip()
        while c.startswith("(") and c.endswith(")"):
            inner = c[1:-1]
            depth = 0
            ok = True
            for ch in inner:
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth < 0:
                        ok = False
                        break
            if not ok or depth:
                break
            c = inner.strip()
        # A bare boolean local resolves to whatever it was declared from.
        if c in self.bools:
            if not self.bools[c]:
                raise Unsupported("cond %s" % text.strip()[:40])
            c = self.bools[c]
        return self.compile_expr(c)

    def and_guard(self, a, b):
        if a is None:
            return b
        first = len(self.nodes)
        self.copy_nodes(a)
        self.copy_nodes(b)
        self.nodes.append(("And", 0.0, 0, ""))
        return (first, len(self.nodes) - first)

    def not_guard(self, g):
        first = len(self.nodes)
        self.copy_nodes(g)
        self.nodes.append(("Not", 0.0, 0, ""))
        return (first, len(self.nodes) - first)

    def copy_nodes(self, span):
        # Guard spans are pure — no local writes — so duplicating them is safe
        # and keeps the tape a flat array with no jumps.
        first, count = span
        self.nodes.extend(self.nodes[first:first + count])


def cf(v):
    s = repr(round(float(v), 7))
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


def main():
    sources = {}
    for r, _, fs in os.walk(MODEL_DIR):
        for f in fs:
            if f.endswith(".java"):
                sources.setdefault(f[:-5], strip_comments(open(os.path.join(r, f), encoding="utf-8").read()))
    # Non-model classes setupAnim calls into: Ease (the WHACK swing curve).
    for extra in (os.path.join(MC, "util/Ease.java"),):
        if os.path.exists(extra):
            name = os.path.basename(extra)[:-5]
            sources.setdefault(name, strip_comments(
                open(extra, encoding="utf-8").read()))

    # Which model class each generated mob uses, and the parts its mesh has.
    sys.path.insert(0, "tools")
    import gen_entity_models as GM

    cpp = open(MODELS_CPP, encoding="utf-8").read()
    mseg = cpp[cpp.index("kGenModels[kGenModelCount] = {"):]
    mseg = mseg[:mseg.index("\n    };")]
    rows = re.findall(
        r'\{\s*"([a-z_0-9]+)",\s*[\d.]+f,\s*[\d.]+f,\s*"([a-z_0-9]*)",\s*AnimGuard::\w+,'
        r'\s*(?:true|false),\s*(\d+),\s*(\d+),', mseg)
    pseg = cpp[cpp.index("kGenParts["):]
    pseg = pseg[:pseg.index("\n    };")]
    pnames = re.findall(r'\{\s*"([a-z_0-9]+)",\s*-?\d+', pseg)

    def camel(s):
        return "".join(p.capitalize() for p in s.split("_"))

    programs, all_nodes, all_stmts = [], [], []
    total_stmts = total_skipped = 0
    report = []

    for slug, headpart, fp, pc in rows:
        cls = GM.MODEL_ALIAS.get(slug, camel(slug) + "Model").split("#")[0]
        if cls not in sources:
            continue
        parts = {pnames[i] for i in range(int(fp), int(fp) + int(pc))}

        # `this.head = root.getChild("head")` — java field name -> mesh part.
        fields = {}
        G_CHAIN = class_chain(cls, sources)
        for c in G_CHAIN:
            # GREEDY on purpose. Five models walk down the hierarchy —
            # `this.head = root.getChild("bone").getChild("body").getChild("head")`
            # — and a non-greedy match takes the FIRST name in the chain. That
            # bound the sniffer's `head` to `bone`, its root-most part, so the
            # compiled look rotation swung the entire animal instead of its head.
            for m in re.finditer(
                    r'this\.(\w+)\s*=\s*[\w.()"]*getChild\(\s*"([a-z_0-9]+)"\s*\)\s*;',
                    sources[c]):
                fields.setdefault(m.group(1), m.group(2))
            for m in re.finditer(r'this\.(\w+)\s*=\s*root\s*;', sources[c]):
                fields.setdefault(m.group(1), "root")
            # Arrays of parts, filled either by an indexed loop or by an
            # initialiser list. Blaze, wither and the dragon all do this.
            # The charclass admits brackets so a chained receiver like
            # `this.tailParts[0].getChild("tail1")` (guardian) still matches.
            for m in re.finditer(
                    r'this\.(\w+)\[\s*(\d+)\s*\]\s*=\s*[\w.\[\]()"+ ]*?getChild\('
                    r'\s*"([a-z_0-9]+)"\s*\)', sources[c]):
                fields.setdefault("%s[%s]" % (m.group(1), m.group(2)), m.group(3))
            # Name-helper resolution shared by the two indexed-fill forms
            # below. The helper may be class-qualified — the ghast's
            # `root.getChild(PartNames.tentacle(i))` lives in PartNames.java,
            # not the model chain — so a qualifier routes the search there.
            def helper_prefix(fn):
                search_chain = G_CHAIN
                if "." in fn:
                    owner, fn = fn.rsplit(".", 1)
                    search_chain = [owner] if owner in sources else []
                for c2 in search_chain:
                    fm = re.search(r'\b' + re.escape(fn) + r'\s*\([^)]*\)\s*\{'
                                   r'\s*return\s+"([a-z_0-9]*)"', sources.get(c2, ""))
                    if fm:
                        return fm.group(1)
                return None

            def fill_indexed(arr, fn):
                prefix = helper_prefix(fn)
                if prefix is None:
                    return
                for k in range(64):
                    name = "%s%d" % (prefix, k)
                    if name in parts:
                        fields.setdefault("%s[%d]" % (arr, k), name)

            # `for (i...) this.spikeParts[i] = this.head.getChild(createSpikeName(i))`
            # — an indexed fill through a name helper (guardian's spikes,
            # ghast tentacles via PartNames).
            for m in re.finditer(
                    r'this\.(\w+)\[\s*\w+\s*\]\s*=\s*[\w.\[\]()" ]*?getChild\('
                    r'\s*([\w.]+)\s*\(', sources[c]):
                fill_indexed(m.group(1), m.group(2))
            # `Arrays.setAll(this.rods, (i) -> root.getChild(getPartName(i)))`
            # where the name helper is `return "prefix" + i;`. Blaze rods,
            # wither heads, squid tentacles and the dragon's neck all index
            # their parts this way, and every loop body writes through it.
            for m in re.finditer(
                    r'Arrays\.setAll\(\s*this\.(\w+)\s*,\s*\(\s*\w+\s*\)\s*->'
                    r'\s*[\w.]*getChild\(\s*([\w.]+)\s*\(', sources[c]):
                fill_indexed(m.group(1), m.group(2))

            for m in re.finditer(
                    r'this\.(\w+)\s*=\s*new ModelPart\[\]\s*\{([^}]*)\}', sources[c]):
                for i, piece in enumerate(m.group(2).split(",")):
                    g = re.search(r'getChild\(\s*"([a-z_0-9]+)"\s*\)', piece)
                    if g:
                        fields.setdefault("%s[%d]" % (m.group(1), i), g.group(1))
            fields = {k: v for k, v in fields.items() if v in parts or v == "root"}

        chain = class_chain(cls, sources)

        def helpers(name, owner=None, _chain=chain):
            """Resolve a void helper. `owner` is an explicit qualifier
            (`GhastModel.animateTentacles`, `AnimationUtils.bobModelPart`) or
            the class an enclosing inlined body was found in — searched first
            so nested unqualified calls resolve in their own class."""
            search = []
            if owner and owner in sources:
                search.extend(class_chain(owner, sources))
            search.extend(c for c in _chain if c not in search)
            for c in search:
                got = method_signature(sources[c], name)
                if got:
                    return got[0], got[1], c
            return None

        def value_fns(name, owner=None, _chain=chain):
            search = []
            if owner and owner in sources:
                search.extend(class_chain(owner, sources))
            search.extend(c for c in _chain if c not in search)
            for c in search:
                got = value_signature(sources[c], name)
                if got:
                    return got
            return None

        def mpart_fns(name, _chain=chain):
            """Body text of a `ModelPart <name>(...)` helper — getArm."""
            for c in _chain:
                m2 = re.search(r"(?:public|private|protected)\s+(?:static\s+)?"
                               r"ModelPart\s+" + re.escape(name)
                               + r"\s*\([^)]*\)\s*\{", sources[c])
                if m2:
                    body2, _ = balanced(sources[c], m2.end() - 1)
                    return body2
            return None

        # Static float tables — GuardianModel's SPIKE_X/Y/Z and friends.
        # Indexed with a constant (the unrolled loop's induction value), they
        # fold to constants.
        arrays = {}
        for c in chain:
            for am in re.finditer(
                    r'float\[\]\s+(\w+)\s*=\s*(?:new\s+float\[\]\s*)?\{([^}]*)\}',
                    sources[c]):
                try:
                    arrays.setdefault(am.group(1), [
                        float(v.strip().rstrip("FfDd"))
                        for v in am.group(2).split(",") if v.strip()])
                except ValueError:
                    continue

        comp = Compiler(fields, helpers, value_fns, arrays, parts, mpart_fns)
        for c in reversed(chain):          # base first, mirroring super.setupAnim
            b = method_body(sources[c], "setupAnim")
            if b:
                comp.run(b)

        if not comp.stmts:
            report.append((slug, cls, 0, len(comp.skipped), comp.skipped))
            continue

        first_stmt = len(all_stmts)
        base_node = len(all_nodes)
        for kind, li, part, field, nf, nc, guard in comp.stmts:
            gf, gc = (base_node + guard[0], guard[1]) if guard else (0, 0)
            all_stmts.append((kind, li, part, field, base_node + nf, nc, gf, gc))
        for n in comp.nodes:
            all_nodes.append(n)
        # Does the program pose the head itself? The runtime applies MC's plain
        # head turn only when it does not — a model whose compiled setupAnim
        # already writes the head (usually with MC's own clamps, which the plain
        # turn has no way to know about) would otherwise be posed twice.
        writes_head = bool(headpart) and any(
            st[2] == headpart for st in comp.stmts)
        programs.append((slug, first_stmt, len(comp.stmts), len(comp.locals),
                         "true" if writes_head else "false"))
        total_stmts += len(comp.stmts)
        total_skipped += len(comp.skipped)
        report.append((slug, cls, len(comp.stmts), len(comp.skipped), comp.skipped))

    # ── Emit ───────────────────────────────────────────────────────────────
    node_rows = []
    for kind, val, arg, s in all_nodes:
        node_rows.append('    {{ AnimOp::{}, {}, {}, "{}" }},'.format(
            kind, cf(val), arg, s))

    stmt_rows = []
    for kind, li, part, field, nf, nc, gf, gc in all_stmts:
        stmt_rows.append('    {{ AnimStmt::{}, {}, "{}", PartField::{}, {}, {}, {}, {} }},'.format(
            kind, li, part, field or "XRot", nf, nc, gf, gc))

    prog_rows = ['    {{ "{}", {}, {}, {}, {} }},'.format(*p) for p in programs]

    hpp = f"""// GENERATED by tools/gen_setup_anim.py — do not edit by hand.
//
// MC's setupAnim, compiled. Each mob's program is a list of statements over a
// postfix expression tape; the runtime walks it once per frame. See the
// generator's docstring for what is and is not portable — statements reading
// render state this port does not replicate are dropped at generation time and
// counted, so the coverage figure is real.
#pragma once

#include <cstdint>
#include <string_view>

namespace Render {{

    enum class AnimOp : uint8_t {{
        Const, State, BState, Local, Part,
        Neg, Add, Sub, Mul, Div, Select,
        Cos, Sin, Abs, Sqrt, Floor, Signum, Min, Max,
        Clamp, Lerp, RotLerpRad, RotLerp, WrapDegrees, TriangleWave,
        Square, Cube, DegDiffAbs, Mod,
        Gt, Lt, Ge, Le, Eq, Ne, And, Or, Not,
    }};

    enum class AnimStmt : uint8_t {{ SetLocal, Set, AddTo, SubFrom, MulBy }};

    enum class PartField : uint8_t {{
        X, Y, Z, XRot, YRot, ZRot, XScale, YScale, ZScale, Visible, SkipDraw }};

    struct AnimNode {{
        AnimOp           op;
        float            value;      // Const
        int              arg;        // Local index / call arity
        std::string_view name;       // state field, or "part|field"
    }};

    struct AnimStatement {{
        AnimStmt         kind;
        int              localIndex;
        std::string_view part;
        PartField        field;
        int              firstNode, nodeCount;
        // A boolean expression on the same tape; nodeCount 0 = always runs.
        int firstGuardNode, guardNodeCount;
    }};

    struct AnimProgram {{
        std::string_view slug;
        int firstStatement, statementCount, localCount;
        // True when the program poses the model's head part itself, so the
        // runtime must NOT also apply the plain look rotation on top.
        bool writesHead;
    }};

    inline constexpr int kAnimProgramCount = {len(prog_rows)};
    extern const AnimProgram   kAnimPrograms[kAnimProgramCount];
    extern const AnimStatement kAnimStatements[];
    extern const AnimNode      kAnimNodes[];

    const AnimProgram* FindAnimProgram(std::string_view slug);

}} // namespace Render
"""

    cpp_out = "\n".join([
        "// GENERATED by tools/gen_setup_anim.py — do not edit by hand.",
        '#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"',
        "",
        "namespace Render {",
        "",
        "    const AnimNode kAnimNodes[] = {",
        *node_rows,
        "    };",
        "",
        "    const AnimStatement kAnimStatements[] = {",
        *stmt_rows,
        "    };",
        "",
        "    const AnimProgram kAnimPrograms[kAnimProgramCount] = {",
        *prog_rows,
        "    };",
        "",
        "    const AnimProgram* FindAnimProgram(std::string_view slug) {",
        "        for (const AnimProgram& p : kAnimPrograms) {",
        "            if (p.slug == slug) return &p;",
        "        }",
        "        return nullptr;",
        "    }",
        "",
        "} // namespace Render",
        "",
    ])

    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp_out)

    pct = 100.0 * total_stmts / max(1, total_stmts + total_skipped)
    print(f"{OUT_HPP}: {len(prog_rows)} programs, {len(stmt_rows)} statements, "
          f"{len(node_rows)} expression nodes")
    print(f"  ported {total_stmts} of {total_stmts + total_skipped} statements ({pct:.0f}%)")

    if "-v" in sys.argv:
        for slug, cls, n, sk, skipped in sorted(report, key=lambda r: -r[3]):
            if not sk:
                continue
            print(f"\n  {slug} ({cls}): {n} ported, {sk} skipped")
            seen = set()
            for stmt, why in skipped:
                if why in seen:
                    continue
                seen.add(why)
                print(f"      {why:34s} {stmt}")
    else:
        reasons = {}
        for _, _, _, _, skipped in report:
            for _, why in skipped:
                key = why.split()[0]
                reasons[key] = reasons.get(key, 0) + 1
        print("  skipped by reason:",
              ", ".join(f"{k}={v}" for k, v in sorted(reasons.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main()
