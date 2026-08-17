#!/usr/bin/env python3
"""Generate src/common/entity/GeneratedMobDefs.{hpp,cpp} from MC's mob classes.

One row per mob: which base to build on (Monster / Animal / plain PathfinderMob),
its attribute overrides, and the texture its renderer uses. That is everything a
generic mob needs in order to exist, move, be hit and be drawn — the parts of MC
that ARE just data.

What is NOT here is behaviour. Goals stay in code: MC expresses them as arbitrary
Java, and a mob with the wrong goals is worse than a mob with the default set.
Every generated mob gets the shared PathfinderMob goal set (float, panic/melee,
stroll, look), and anything wanting more gets a hand-written class the way the
original eight have.

    python3 tools/gen_mob_defs.py
"""

import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
ENT_DIR = os.path.join(MC, "world/entity")
REN_DIR = os.path.join(MC, "client/renderer/entity")
TYPES_HPP = "src/common/entity/GeneratedEntityTypes.hpp"
OUT_HPP = "src/common/entity/GeneratedMobDefs.hpp"
OUT_CPP = "src/common/entity/GeneratedMobDefs.cpp"

HAND_WRITTEN = {"zombie", "skeleton", "creeper", "spider",
                "cow", "pig", "sheep", "chicken"}

# MC attribute -> our Attribute enum. Anything not listed has no effect in this
# engine yet (armour, luck, jump strength …) and is dropped rather than faked.
ATTRS = {
    "MAX_HEALTH": "MaxHealth",
    "MOVEMENT_SPEED": "MovementSpeed",
    "ATTACK_DAMAGE": "AttackDamage",
    "ATTACK_KNOCKBACK": "AttackKnockback",
    "FOLLOW_RANGE": "FollowRange",
    "KNOCKBACK_RESISTANCE": "KnockbackResistance",
    "STEP_HEIGHT": "StepHeight",
}

# Our four C++ bases ARE four of MC's own classes, so the mapping is just "the
# most derived one that appears in this mob's chain" — no hand-maintained list
# of intermediate classes, which is what previously put Bat on Animal
# (AmbientCreature extends Mob, not Animal) and every fish on Animal
# (AbstractFish -> WaterAnimal -> PathfinderMob, also not Animal).
#
# The Mob rung matters: a MC Mob that is not a PathfinderMob has no
# GroundPathNavigation and no stroll goal — bats, ghasts, phantoms, slimes, the
# ender dragon. It is also exactly the set MC never gives MOVEMENT_SPEED, so
# they sit at the registry default of 0.7; handing them the strolling goal set
# makes them tear across the terrain at three times a cow's speed.
BASE_ORDER = ["Monster", "Animal", "PathfinderMob", "Mob"]


def cf(v):
    """A C++ float literal that always has a decimal point.

    `%g` renders 8.0 as "8", and "8f" is not a number in C++ — it is an invalid
    digit sequence. Every emitted float suffix goes through here.
    """
    s = repr(round(float(v), 6))
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


def camel(slug):
    return "".join(p.capitalize() for p in slug.split("_"))


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def index_sources(root):
    out = {}
    for r, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(".java"):
                out.setdefault(f[:-5], os.path.join(r, f))
    return out


def superclass(src, cls):
    m = re.search(r"class\s+" + re.escape(cls) + r"(?:<[^>]*>)?\s+extends\s+(\w+)", src)
    return m.group(1) if m else None


def chain(cls, files, seen=None):
    seen = seen or set()
    if cls in seen or cls not in files:
        return []
    seen.add(cls)
    src = strip_comments(open(files[cls], encoding="utf-8").read())
    sup = superclass(src, cls)
    return [cls] + (chain(sup, files, seen) if sup else [])


# The four builders our C++ models directly as MobBase — CreateLivingAttributes,
# CreateMobAttributes, CreateMonsterAttributes, CreateAnimalAttributes in
# Attributes.cpp. Recursion stops at these: whatever they add is already applied
# before ApplyDef runs, so re-emitting it would be noise.
BASE_BUILDERS = {
    "createLivingAttributes",
    "createMobAttributes",
    "createMonsterAttributes",
    "createAnimalAttributes",
}

# `.add(Attributes.X, <value>)`. The value may carry a `(double)` or `(float)`
# cast and an F/D suffix — MC's decompile writes the same number four different
# ways: `0.14`, `(double)0.14F`, `0.14F`, `35.0`.
ADD_CALL = re.compile(r"\.add\s*\(\s*Attributes\.([A-Z_]+)\s*(?:,([^;]*?))?\)(?=\s*[.;)])")
NUMBER = re.compile(r"^\s*(?:\(\s*(?:double|float)\s*\)\s*)*(-?[0-9]*\.?[0-9]+)[FfDd]?\s*$")

# Attribute builders called by name rather than being one of the four bases —
# MC's per-family helpers. Recursed into so their values land on the mob.
HELPER_CALL = re.compile(r"(?:(\w+)\s*\.\s*)?(create[A-Za-z]*Attributes)\s*\(\s*\)")


def method_body(src, name):
    """The text between the braces of `static ... name(...) {` — brace-balanced.

    The old regex terminated on a line that was exactly three spaces and a
    brace, which silently truncated any body the decompiler indented
    differently.
    """
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end():i - 1]


def attributes(cls, files, method="createAttributes", seen=None, unparsed=None):
    """Every attribute MC ends up setting on `cls`, base-first.

    MC composes attributes by chaining: a mob's createAttributes() calls a
    family helper (createBaseHorseAttributes, AbstractFish.createAttributes,
    Zombie.createAttributes …) and then .add()s its own overrides on top. Only
    reading the mob's own method — which is what this used to do — loses every
    value the helper set, and reading it without following the class chain
    loses mobs that define no createAttributes at all (donkey, mule).

    A missed MOVEMENT_SPEED is not a small error: nothing overrides it, so the
    mob keeps the registry default of 0.7, roughly triple a cow's 0.25.
    """
    seen = set() if seen is None else seen
    if unparsed is None:
        unparsed = []

    # Walk up the class chain — a mob with no createAttributes of its own
    # inherits its parent's (Donkey -> AbstractChestedHorse -> AbstractHorse).
    body, home = None, None
    for c in chain(cls, files):
        src = strip_comments(open(files[c], encoding="utf-8").read())
        body = method_body(src, method)
        if body is not None:
            home, home_src = c, src
            break
    if body is None:
        return {}

    key = (home, method)
    if key in seen:
        return {}
    seen.add(key)

    out = {}

    # Base first, so the derived class's own .add() wins on a conflict — which
    # is exactly what MC's builder does (a later .add replaces the earlier).
    for owner, helper in HELPER_CALL.findall(body):
        if helper in BASE_BUILDERS:
            continue
        target = owner if owner and owner in files else home
        out.update(attributes(target, files, helper, seen, unparsed))

    for attr, value in ADD_CALL.findall(body):
        if attr not in ATTRS:
            continue
        if value is None or not value.strip():
            continue          # `.add(Attributes.X)` — registry default, no override
        num = NUMBER.match(value)
        if num:
            out[ATTRS[attr]] = float(num.group(1))
        else:
            unparsed.append((home, attr, " ".join(value.split())))
    return out


# MC registry defaults for the attributes we model (Attributes.java), and what
# each of MC's four base builders leaves them at. `.add(X)` with no value keeps
# the registry default, which is why createMonsterAttributes' ATTACK_DAMAGE is
# 2.0 without the number appearing anywhere near it.
#
# These MUST mirror CreateLivingAttributes / CreateMobAttributes /
# CreateMonsterAttributes / CreateAnimalAttributes in src/common/entity/
# Attributes.cpp. They are what a row is diffed against, so a drift here shows
# up as every mob carrying a redundant override — or, worse, missing one.
REGISTRY_DEFAULTS = {
    "MaxHealth": 20.0,
    "MovementSpeed": 0.7,
    "AttackDamage": 2.0,
    "AttackKnockback": 0.0,
    "FollowRange": 32.0,
    "KnockbackResistance": 0.0,
    "StepHeight": 0.6,
}

_LIVING = {a: REGISTRY_DEFAULTS[a] for a in
           ("MaxHealth", "MovementSpeed", "KnockbackResistance",
            "StepHeight", "AttackKnockback")}
_MOB = dict(_LIVING, FollowRange=16.0)

BUILDER_DEFAULTS = {
    "createLivingAttributes": _LIVING,
    "createMobAttributes": _MOB,
    "createMonsterAttributes": dict(_MOB, AttackDamage=REGISTRY_DEFAULTS["AttackDamage"]),
    "createAnimalAttributes": _MOB,          # adds TEMPT_RANGE, which we don't model
}

OUR_BASE_BUILDER = {
    "Mob": "createMobAttributes",
    "PathfinderMob": "createMobAttributes",
    "Monster": "createMonsterAttributes",
    "Animal": "createAnimalAttributes",
}


# ── Goal / move-control parameters ────────────────────────────────────────
#
# MC's effective walk speed is `speedModifier * MOVEMENT_SPEED`, where the
# modifier comes from the GOAL, not the attribute — and it is never the 1.0 this
# port used to hardcode for everyone. Thirteen mobs stroll between 0.35 and 0.8,
# and the brain-driven ones reach 2.0. Camel is the extreme case: 0.09 x 2.0 in
# MC against 0.09 x 1.0 here, i.e. half speed, which is exactly what it looked
# like.
GOAL_SPEED = {
    "stroll": r"new (?:WaterAvoiding)?RandomStroll\w*Goal\(\s*this\s*,\s*([^,)]+)",
    "panic":  r"new \w*PanicGoal\(\s*this\s*,\s*([^,)]+)",
    "melee":  r"new \w*AttackGoal\(\s*this\s*,\s*([^,)]+)",
}
LOOK_DIST = r"new LookAtPlayerGoal\(\s*this\s*,\s*[\w.]+\s*,\s*([^,)]+)"

# The brain equivalent of RandomStrollGoal. Mobs with an empty (or absent)
# registerGoals drive everything from <Mob>Ai.java instead.
BRAIN_STROLL = r"RandomStroll\.stroll\(\s*([^,)]+)"

# ── Who a mob hunts, and who it flees ─────────────────────────────────────
#
# MC parameterises NearestAttackableTargetGoal and AvoidEntityGoal on a
# `Class<T>`. Twenty mobs pass Player.class — which our goals already handled —
# but the rest name IronGolem, Turtle, AbstractVillager, Wolf, Creaking and a
# dozen others, and a zombie that hunts players while ignoring villagers and
# iron golems is not the same mob.
TARGET_GOAL = re.compile(r"new NearestAttackableTargetGoal<?>?\(\s*this\s*,\s*(\w+)\.class")
AVOID_GOAL = re.compile(r"new AvoidEntityGoal<?>?\(\s*this\s*,\s*(\w+)\.class")
# MC gives RestrictSunGoal/FleeSunGoal to AbstractSkeleton ONLY — not to every
# mob that burns in daylight. A zombie caught out just burns; a skeleton dives
# for shade. Reading it per mob keeps that difference.
FLEE_SUN = re.compile(r"new (?:RestrictSunGoal|FleeSunGoal)\(")

# MC's abstract target classes, flattened to the concrete types this port has.
ABSTRACT_TARGETS = {
    "AbstractVillager":  ["villager", "wandering_trader"],
    "AbstractSkeleton":  ["skeleton", "stray", "wither_skeleton", "bogged", "parched"],
    "AbstractIllager":   ["evoker", "illusioner", "pillager", "vindicator"],
    "AbstractPiglin":    ["piglin", "piglin_brute"],
}

# Deliberately NOT flattened. MC pairs these with a runtime selector — an iron
# golem takes `Mob.class` plus `LivingEntity::attackable`, a guardian takes
# `LivingEntity.class` plus a squid test — and expanding the class without the
# selector would have golems attacking cows. Reported by the audit instead.
UNMAPPABLE_TARGETS = {"LivingEntity", "Mob", "Animal", "AbstractFish"}
# SetEntityLookTargetSometimes.create(EntityType.PLAYER, 6.0F, UniformInt.of(30, 60))
BRAIN_LOOK = (r"SetEntityLookTargetSometimes\.create\(\s*EntityType\.PLAYER\s*,"
              r"\s*([\d.]+)F?\s*,\s*UniformInt\.of\(\s*(\d+)\s*,\s*(\d+)\s*\)")

# MC SmoothSwimmingMoveControl(mob, maxTurnX, maxTurnY, inWater, outsideWater,
# gravity). `outsideWater` is why a frog with MOVEMENT_SPEED 1.0 does not tear
# across the ground: MC multiplies its land speed by 0.1. Without it the frog
# walks at five times a cow.
SMOOTH_SWIM = (r"moveControl\s*=\s*new SmoothSwimmingMoveControl\(\s*this\s*,"
               r"\s*[^,]+,\s*[^,]+,\s*([^,]+),\s*([^,]+),")


def num(expr):
    m = NUMBER.match(expr or "")
    return float(m.group(1)) if m else None


def target_types(cls, files, slug_of_class):
    """(hunted slugs, fled slugs, hunts players, flees players, skipped).

    Scans registerGoals AND addBehaviourGoals: MC's Zombie puts its FloatGoal in
    the first and every target goal in the second, so reading only registerGoals
    reported the most-targeted mob in the game as hunting nothing.
    """
    hunt, flee, skipped = [], [], []
    hunt_players = flee_players = seeks_shade = False

    for c in chain(cls, files):
        src = strip_comments(open(files[c], encoding="utf-8").read()) if c in files else ""
        bodies = [b for b in (method_body(src, "registerGoals"),
                              method_body(src, "addBehaviourGoals")) if b]
        if not bodies:
            continue
        for body in bodies:
            if FLEE_SUN.search(body):
                seeks_shade = True
            for pat, out, key in ((TARGET_GOAL, hunt, "target"),
                                  (AVOID_GOAL, flee, "avoid")):
                for arg in pat.findall(body):
                    if arg == "Player":
                        if key == "target":
                            hunt_players = True
                        else:
                            flee_players = True
                        continue
                    if arg in UNMAPPABLE_TARGETS:
                        note = "%s %s (MC pairs it with a runtime selector)" % (key, arg)
                        if note not in skipped:
                            skipped.append(note)
                        continue
                    for slug in ABSTRACT_TARGETS.get(arg, slug_of_class.get(arg, [])):
                        if slug not in out:
                            out.append(slug)
        # A registerGoals that does not chain up replaces its parent's set.
        rg = method_body(src, "registerGoals")
        if rg is not None and "super.registerGoals()" not in rg:
            break

    return hunt, flee, hunt_players, flee_players, seeks_shade, skipped


def goal_params(cls, files):
    """MC's per-mob goal speeds and land-speed factor, walking up the chain."""
    out = {"stroll": None, "strollwater": None, "panic": None, "melee": None,
           "look": None, "land": None, "lookmin": 0, "lookmax": 0,
           "__goals_done": False}
    for c in chain(cls, files):
        src = strip_comments(open(files[c], encoding="utf-8").read())

        m = re.search(SMOOTH_SWIM, src)
        if m and out["land"] is None:
            out["land"] = num(m.group(2))

        if not out["__goals_done"]:
            body = method_body(src, "registerGoals")
            if body is not None:
                for key, pat in GOAL_SPEED.items():
                    g = re.search(pat, body)
                    if g and out[key] is None:
                        out[key] = num(g.group(1))
                if out["strollwater"] is None:
                    # MC picks the plain RandomStrollGoal for 8 mobs; the
                    # water-avoiding variant is the common case, not the only one.
                    if re.search(r"new WaterAvoidingRandomStrollGoal\(", body):
                        out["strollwater"] = True
                    elif re.search(r"new RandomStrollGoal\(", body):
                        out["strollwater"] = False
                g = re.search(LOOK_DIST, body)
                if g and out["look"] is None:
                    out["look"] = num(g.group(1))
                # An override that does NOT call super.registerGoals() replaces
                # its parent's goals outright. Camel's is empty precisely to
                # drop AbstractHorse's — so reading through to the parent gave
                # it the horse's 0.7 stroll instead of the 2.0 its brain uses.
                if "super.registerGoals()" not in body:
                    out["__goals_done"] = True

    # MC's brain mobs do not use LookAtPlayerGoal at all — they use
    # SetEntityLookTargetSometimes, which fires on a UNIFORM 30-60 tick timer
    # instead of the goal's 2%-per-evaluation roll. The two are not equivalent:
    # a 2% roll is geometric with a ~100-tick mean and a long tail, so a camel
    # that MC has glance at you every 45 ticks on average would go several
    # seconds at a time without doing it. Twelve mobs are affected.
    for c in chain(cls, files):
        ai = files.get(c + "Ai")
        if not ai:
            continue
        g = re.search(BRAIN_LOOK, strip_comments(open(ai, encoding="utf-8").read()))
        if g:
            out["look"] = num(g.group(1))
            out["lookmin"] = int(g.group(2))
            out["lookmax"] = int(g.group(3))
            break

    if out["stroll"] is None:
        # Brain-driven: the stroll modifier lives in <Mob>Ai.java.
        for c in chain(cls, files):
            ai = files.get(c + "Ai")
            if not ai:
                continue
            g = re.search(BRAIN_STROLL, strip_comments(open(ai, encoding="utf-8").read()))
            if g:
                out["stroll"] = num(g.group(1))
                break
    return out


# ── Breeding / taming food ────────────────────────────────────────────────
#
# MC states every animal's food as an item tag — `isFood(stack)` is
# `stack.is(ItemTags.COW_FOOD)` for all 26 of them — and this repo already ships
# those tag files under data/minecraft/tags/item/. So the food IS generatable,
# and without it TemptGoal and BreedGoal can never fire: a breed goal with no
# food is a goal that does nothing, which is why they were left out entirely.
ITEM_LIST = "src/common/entity/GeneratedItemList.hpp"
TAG_DIR = "data/minecraft/tags/item"


def load_item_identifiers():
    """slug -> Items::Identifier, from the generated item list."""
    pat = re.compile(r'ItemID\s+(\w+)\s+=\s+PURE_ITEM_BASE\s+\+\s+\d+;\s*//\s*"([a-z0-9_]+)"')
    out = {}
    for line in open(ITEM_LIST, encoding="utf-8"):
        m = pat.search(line)
        if m:
            out[m.group(2)] = m.group(1)
    return out


def resolve_item_tag(tag, seen=None):
    """Flatten a `#minecraft:` item tag to slugs, following nested tags."""
    import json
    seen = set() if seen is None else seen
    if tag in seen:
        return []
    seen.add(tag)
    path = os.path.join(TAG_DIR, tag + ".json")
    if not os.path.exists(path):
        return []
    out = []
    for v in json.load(open(path, encoding="utf-8")).get("values", []):
        v = v["id"] if isinstance(v, dict) else v
        if v.startswith("#"):
            out += resolve_item_tag(v.removeprefix("#minecraft:"), seen)
        else:
            out.append(v.removeprefix("minecraft:"))
    return out


def food_items(cls, files, items, unknown):
    """The item slugs MC's isFood accepts."""
    for c in chain(cls, files):
        src = strip_comments(open(files[c], encoding="utf-8").read())
        body = method_body(src, "isFood")
        if body is None:
            continue
        tags = re.findall(r"ItemTags\.([A-Z_0-9]+)", body)
        if not tags:
            return []
        out = []
        for t in tags:
            # A taming item is not a breeding item; MC's nautilus checks
            # NAUTILUS_TAMING_ITEMS first and only feeds on NAUTILUS_FOOD.
            if "TAMING" in t:
                continue
            for slug in resolve_item_tag(t.lower()):
                if slug not in out:
                    out.append(slug)
        return out
    return []


# MC LivingEntity.updateWalkAnimation, and the three mobs that override it.
#
# The multiplier decides how fast the walk animation advances per block
# travelled, and it is NOT a detail: a frog uses 25x where the default is 4x, so
# running the shared version plays its walk cycle at a sixth of MC's rate — the
# keyframes are right and the motion still looks nothing like the game.
WALK_ANIM = re.compile(
    r"protected void updateWalkAnimation\(final float (\w+)\)\s*\{")


def walk_anim(cls, files):
    """(distanceScale, cap, smoothing, babyPositionScale) for this mob."""
    for c in chain(cls, files):
        src = strip_comments(open(files[c], encoding="utf-8").read())
        m = WALK_ANIM.search(src)
        if not m:
            continue
        depth, i = 1, m.end()
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        body = " ".join(src[m.end():i - 1].split())

        # `Math.min(distance * S, CAP)`
        mm = re.search(r"Math\.min\(\s*\w+\s*\*\s*([\d.]+)F?\s*,\s*([\d.]+)F?\s*\)", body)
        scale, cap = (float(mm.group(1)), float(mm.group(2))) if mm else (4.0, 1.0)

        # `walkAnimation.update(target, FACTOR, POSITION_SCALE)`
        mu = re.search(r"walkAnimation\.update\([^,]+,\s*([\d.]+)F?\s*,\s*([^)]+)\)", body)
        factor = float(mu.group(1)) if mu else 0.4
        babyscale = 3.0
        if mu and "isBaby" not in mu.group(2):
            n = NUMBER.match(mu.group(2).strip())
            babyscale = float(n.group(1)) if n else 1.0
        return scale, cap, factor, babyscale
    return 4.0, 1.0, 0.4, 3.0


def entity_classes():
    """slug -> the entity's Java class, from `Class::new` in EntityType.java."""
    src = strip_comments(open(os.path.join(ENT_DIR, "EntityType.java"), encoding="utf-8").read())
    return {m.group(1): m.group(2) for m in re.finditer(
        r'register\(\s*"([a-z_0-9]+)"\s*,\s*EntityType\.Builder\.'
        r'(?:<[^>]*>)?of\(\s*(\w+)::new', src)}


def default_attribute_sources():
    """slug -> (Class, method) from DefaultAttributes.java's SUPPLIERS map."""
    src = strip_comments(open(os.path.join(
        ENT_DIR, "ai/attributes/DefaultAttributes.java"), encoding="utf-8").read())
    return {const.lower(): (cls, meth) for const, cls, meth in re.findall(
        r"put\(\s*EntityType\.([A-Z_0-9]+)\s*,\s*(\w+)\.(\w+)\(\)", src)}


def base_builder_reached(cls, files, method, seen=None):
    """Which of MC's four base builders this chain bottoms out at."""
    seen = set() if seen is None else seen
    if method in BASE_BUILDERS:
        return method
    body, home = None, None
    for c in chain(cls, files):
        body = method_body(strip_comments(open(files[c], encoding="utf-8").read()), method)
        if body is not None:
            home = c
            break
    if body is None or (home, method) in seen:
        return None
    seen.add((home, method))
    for owner, helper in HELPER_CALL.findall(body):
        got = base_builder_reached(
            owner if owner and owner in files else home, files, helper, seen)
        if got:
            return got
    return None


# Renderers that pick their texture from a variant map rather than a constant.
# The generator cannot read those, so the DEFAULT variant is pinned here — the
# one vanilla shows most often, which is what an untextured mob should look
# like until variants are modelled.
TEXTURE_FALLBACK = {
    "axolotl":         "assets/textures/entity/axolotl/axolotl_lucy.png",
    "cat":             "assets/textures/entity/cat/tabby.png",
    # CopperGolemRenderer picks from CopperGolemOxidationLevels; UNAFFECTED is
    # the default. The PNG is NOT in the shipped asset dump (which lags the
    # decompile — see "Items that exist in Items.java but lack assets" in
    # CLAUDE.md), so this warns once at load and draws untextured until the
    # dump is refreshed. Pinning the real path means it starts working the
    # moment the file appears, with no code change.
    "copper_golem":    "assets/textures/entity/copper_golem/copper_golem.png",
    "frog":            "assets/textures/entity/frog/temperate_frog.png",
    "giant":           "assets/textures/entity/zombie/zombie.png",
    "mooshroom":       "assets/textures/entity/cow/red_mooshroom.png",
    "mule":            "assets/textures/entity/horse/mule.png",
    "piglin_brute":    "assets/textures/entity/piglin/piglin_brute.png",
    "shulker":         "assets/textures/entity/shulker/shulker.png",
    "skeleton_horse":  "assets/textures/entity/horse/horse_skeleton.png",
    "trader_llama":    "assets/textures/entity/llama/brown.png",
    "wither":          "assets/textures/entity/wither/wither.png",
    "wolf":            "assets/textures/entity/wolf/wolf.png",
    "zombie_horse":    "assets/textures/entity/horse/horse_zombie.png",
    "zombie_nautilus": "assets/textures/entity/zombie/zombie.png",
}


def texture_for(slug, files_ren):
    """The renderer's texture path, as a repo-relative asset path."""
    cls = camel(slug) + "Renderer"
    for c in (cls, camel(slug) + "EntityRenderer"):
        if c in files_ren:
            src = open(files_ren[c], encoding="utf-8").read()
            m = re.search(r'withDefaultNamespace\(\s*"(textures/entity/[^"]+)"', src)
            if m:
                return "assets/" + m.group(1)
    return TEXTURE_FALLBACK.get(slug, "")


def main():
    if not os.path.isdir(ENT_DIR):
        sys.exit(f"missing {ENT_DIR} — run from the repo root")

    ent_files = index_sources(ENT_DIR)
    ren_files = index_sources(REN_DIR)

    slugs = re.findall(r'//\s*"([a-z_]+)"', open(TYPES_HPP, encoding="utf-8").read())

    entity_class = entity_classes()
    # Java class -> our slugs, for the target/avoid lists. Several MC classes
    # back more than one entity type (SkeletonModel-style reskins).
    slug_of_class = {}
    for _slug, _c in entity_class.items():
        slug_of_class.setdefault(_c, []).append(_slug)
    attr_source = default_attribute_sources()

    item_ids = load_item_identifiers()
    rows, notex, unparsed, noattr, unknown_food = [], [], [], [], []
    for slug in slugs:
        if slug in HAND_WRITTEN or slug == "arrow":
            continue

        # The entity's OWN class, from its `Class::new` in EntityType.java —
        # not camel(slug), which is wrong for enderman (EnderMan), wither
        # (WitherBoss) and mooshroom (MushroomCow) and quietly produced an
        # empty chain, hence base PathfinderMob and zero attributes.
        cls = entity_class.get(slug) or camel(slug)
        ch = chain(cls, ent_files)
        base = next((b for b in BASE_ORDER if b in ch), "Mob")

        # DefaultAttributes.java is the authoritative EntityType -> builder map.
        # Reading the mob class's own createAttributes() instead gets several
        # rows wrong: cave_spider's builder is createCaveSpider(), horse/donkey/
        # mule have no method of their own, and phantom/slime/wandering_trader
        # use a bare base builder.
        src = attr_source.get(slug)
        if src:
            mc = attributes(src[0], ent_files, src[1], unparsed=unparsed)
            mc_base = base_builder_reached(src[0], ent_files, src[1])
        else:
            noattr.append(slug)
            mc, mc_base = {}, None

        # Emit the DIFFERENCE against what our C++ base already registers, so a
        # mob whose MobBase disagrees with MC's builder (phantom and slime are
        # Mob subclasses built from createMonsterAttributes) still ends up with
        # MC's exact numbers.
        effective = dict(BUILDER_DEFAULTS[mc_base or "createMobAttributes"])
        effective.update(mc)
        ours = BUILDER_DEFAULTS[OUR_BASE_BUILDER[base]]
        attrs = {a: v for a, v in effective.items() if ours.get(a) != v}

        # MC omitting MOVEMENT_SPEED is a statement, not an oversight: every
        # mob that walks sets it, and the ones that don't are the swimmers,
        # fliers and sitters whose registerGoals contains no ground stroll —
        # fish, squid, bats, ghasts, phantoms, slimes, shulkers. They keep the
        # registry default of 0.7, so handing them the strolling goal set makes
        # them cross the landscape at three times a cow's pace.
        #
        # Since per-mob registerGoals is not generated (see the docstring), the
        # nearest faithful stand-in is to drop them to the Mob base, which
        # floats and looks around but never strolls. Monsters are exempt: their
        # goal set is what makes them attack, and a vex that cannot chase is a
        # worse error than a fast one.
        if "MovementSpeed" not in attrs and base in ("Animal", "PathfinderMob"):
            base = "Mob"
            attrs = {a: v for a, v in attrs.items()
                     if BUILDER_DEFAULTS["createMobAttributes"].get(a) != v}

        tex = texture_for(slug, ren_files)
        if not tex:
            notex.append(slug)
        rows.append((slug, base, attrs, tex, goal_params(cls, ent_files),
                     food_items(cls, ent_files, item_ids, unknown_food),
                     walk_anim(cls, ent_files),
                     target_types(cls, ent_files, slug_of_class)))

    hpp = f"""// GENERATED by tools/gen_mob_defs.py — do not edit by hand.
//
// One row per mob that has no hand-written class: the base to build on, its
// attribute overrides from MC's createAttributes(), and its texture.
//
// Behaviour is NOT here — see the generator's docstring. Every generated mob
// gets the shared goal set from MakeGenericMob().
#pragma once

#include "common/entity/Attributes.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"

#include <string_view>

namespace Game {{

    enum class MobBase : uint8_t {{ Mob, PathfinderMob, Monster, Animal }};

    struct MobAttrOverride {{
        Attribute attribute;
        double    value;
    }};

    struct MobDef {{
        EntityTypeId     type;
        MobBase          base;
        std::string_view texture;      // "" when the renderer's path was not found
        int              firstAttr, attrCount;
        int              firstFood, foodCount;

        // MC goal speed modifiers. The mob's real walk speed is
        // `modifier * MOVEMENT_SPEED` (MoveControl.java:99), and MC picks the
        // modifier per goal per mob — 0.35 for a wandering trader, 2.0 for a
        // camel. Assuming 1.0 for everyone made 13 mobs too fast and the
        // brain-driven ones half speed.
        double strollSpeed;            // 0 = MC gives this mob no stroll goal
        double panicSpeed;
        double meleeSpeed;
        float  lookDistance;

        // MC SmoothSwimmingMoveControl's outsideWaterSpeedModifier. 1.0 for
        // every mob with the ordinary MoveControl; 0.1 for frog/dolphin/
        // tadpole and 0.0 for the nautilus, which is what keeps a mob with
        // MOVEMENT_SPEED 1.0 from sprinting once it is on land.
        float  landSpeedFactor;

        // MC uses the plain RandomStrollGoal for 8 mobs and the water-avoiding
        // variant for the rest; they are different goals, not a detail.
        bool   strollAvoidsWater;

        // MC LivingEntity.updateWalkAnimation, overridden by camel, frog and
        // creaking. `walkAnimScale` is blocks-travelled -> animation rate; the
        // default is 4, a frog is 25. Getting it wrong plays the right
        // keyframes at the wrong speed.
        float  walkAnimScale, walkAnimCap, walkAnimFactor, walkAnimBabyScale;


        // MC SetEntityLookTargetSometimes' UniformInt interval, for the twelve
        // mobs whose brain looks at the player on a TIMER rather than through
        // LookAtPlayerGoal's 2% roll. 0 = use the goal's probability, which is
        // what MC does for every mob that has a registerGoals.
        int    lookIntervalMin, lookIntervalMax;
        // MC NearestAttackableTargetGoal / AvoidEntityGoal, generated from the
        // `Class<T>` each mob passes. `targetsPlayers` is MC's Player.class
        // case; the spans are everything else, flattened through MC's abstract
        // classes (AbstractVillager -> villager + wandering_trader).
        int    firstTargetType, targetTypeCount;
        int    firstAvoidType,  avoidTypeCount;
        bool   targetsPlayers, avoidsPlayers;

        // MC RestrictSunGoal + FleeSunGoal. AbstractSkeleton only — a zombie
        // caught in daylight just burns, a skeleton runs for shade.
        bool   seeksShade;
    }};

    inline constexpr int kMobDefCount = {len(rows)};
    extern const MobDef          kMobDefs[kMobDefCount];
    extern const MobAttrOverride kMobAttrs[];

    // Who each mob hunts and flees, indexed by MobDef's spans.
    extern const EntityTypeId    kMobTargetTypes[];
    extern const EntityTypeId    kMobAvoidTypes[];

    // The items MC's isFood accepts, flattened from the item tag it names.
    // TemptGoal and BreedGoal read this; a mob with none of them gets neither
    // goal, which is MC's behaviour for anything whose isFood returns false.
    //
    // Slugs rather than ItemIDs because a food may be a BLOCK item — cactus,
    // bamboo, seagrass and thirty-odd flowers — whose ItemID is derived from
    // its BlockID and so has no constant in GeneratedItemList. Resolved once at
    // startup through RecipeManager::ItemFromSlug, the same way recipes and
    // loot tables already do it.
    extern const char* const      kMobFoodSlugs[];

    // Null for the eight hand-written mobs and for anything not a mob.
    const MobDef* FindMobDef(EntityTypeId type);

}} // namespace Game
"""

    attr_rows, def_rows, food_rows = [], [], []
    target_rows, avoid_rows = [], []
    for slug, base, attrs, tex, gp, food, wa, tt in rows:
        hunt, flee, hunt_players, flee_players, seeks_shade, _skipped = tt
        ft, fv = len(target_rows), len(avoid_rows)
        target_rows += ["    EntityTypeId::%s," % camel(x) for x in hunt]
        avoid_rows += ["    EntityTypeId::%s," % camel(x) for x in flee]
        ff = len(food_rows)
        food_rows += [f'    "{s}",' for s in food]
        first = len(attr_rows)
        for a in sorted(attrs):
            attr_rows.append(f"    {{ Attribute::{a}, {attrs[a]:g} }},")
        # A mob MC never gives a stroll goal keeps 0 and gets none here either.
        stroll = gp["stroll"] if gp["stroll"] is not None else (
            0.0 if base == "Mob" else 1.0)
        def_rows.append(
            f'    {{ EntityTypeId::{camel(slug)}, MobBase::{base}, '
            f'"{tex}", {first}, {len(attrs)}, {ff}, {len(food)}, '
            f'{stroll:g}, {gp["panic"] or 2.0:g}, {gp["melee"] or 1.0:g}, '
            f'{cf(gp["look"] or 8.0)}, '
            f'{cf(gp["land"] if gp["land"] is not None else 1.0)}, '
            f'{"false" if gp["strollwater"] is False else "true"}, '
            f'{cf(wa[0])}, {cf(wa[1])}, {cf(wa[2])}, {cf(wa[3])}, '
            f'{gp["lookmin"]}, {gp["lookmax"]}, '
            f'{ft}, {len(hunt)}, {fv}, {len(flee)}, '
            f'{"true" if hunt_players else "false"}, '
            f'{"true" if flee_players else "false"}, '
            f'{"true" if seeks_shade else "false"} }},')

    cpp = "\n".join([
        "// GENERATED by tools/gen_mob_defs.py — do not edit by hand.",
        '#include "common/entity/GeneratedMobDefs.hpp"',
        "",
        "namespace Game {",
        "",
        "    const char* const kMobFoodSlugs[] = {",
        *(food_rows or ['    "",']),
        "    };",
        "",
        "    const EntityTypeId kMobTargetTypes[] = {",
        *(target_rows or ["    EntityTypeId::Count,"]),
        "    };",
        "",
        "    const EntityTypeId kMobAvoidTypes[] = {",
        *(avoid_rows or ["    EntityTypeId::Count,"]),
        "    };",
        "",
        "    const MobAttrOverride kMobAttrs[] = {",
        *attr_rows,
        "    };",
        "",
        "    const MobDef kMobDefs[kMobDefCount] = {",
        *def_rows,
        "    };",
        "",
        "    const MobDef* FindMobDef(EntityTypeId type) {",
        "        for (const MobDef& d : kMobDefs) {",
        "            if (d.type == type) return &d;",
        "        }",
        "        return nullptr;",
        "    }",
        "",
        "} // namespace Game",
        "",
    ])

    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp)

    by_base = {}
    for _, b, *_ in rows:
        by_base[b] = by_base.get(b, 0) + 1
    print(f"{OUT_HPP}: {len(rows)} mobs, {len(attr_rows)} attribute overrides")
    print("  bases: " + ", ".join(f"{k}={v}" for k, v in sorted(by_base.items())))
    if notex:
        print(f"  no texture found for {len(notex)}: {', '.join(notex)}")
    if noattr:
        print(f"  no DefaultAttributes row for {len(noattr)}: {', '.join(noattr)}")
    if unparsed:
        # A non-literal value MC computes at runtime. Left unset deliberately —
        # a wrong number is worse than the documented base default — but printed
        # so it is a known gap rather than a silent one.
        print(f"  non-literal attribute values ({len(unparsed)}), left at base default:")
        for cls, attr, expr in unparsed:
            print(f"    {cls}.{attr} = {expr}")


if __name__ == "__main__":
    main()
