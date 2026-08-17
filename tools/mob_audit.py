#!/usr/bin/env python3
"""Audit every mob against Minecraft, mechanically.

    python3 tools/mob_audit.py              # summary + the worst offenders
    python3 tools/mob_audit.py --all        # every mob
    python3 tools/mob_audit.py cow frog     # named mobs, in full
    python3 tools/mob_audit.py --json out.json
    python3 tools/mob_audit.py --html out.html

WHY THIS EXISTS. Every gap found in this port so far was found by a human
looking at one mob in the game and noticing something missing: a frog that would
not croak, a camel whose ears did not move, an armadillo that never rolled up, a
sniffer whose whole body swung when it turned its head. That is not a process —
it is 90 mobs times a dozen systems, and nobody is going to catch a keyframe
animation that is baked but never triggered by watching a frog for long enough.

So this compares MC's own source, per mob, against what this port actually
builds, and prints what is missing. Every check is mechanical: "MC's class
overrides mobInteract and we have no class for this type" is a fact, not a
judgement. Nothing here says a mob is FINE — it says which specific things MC
has that we do not, so that a gap has to be dismissed deliberately rather than
never noticed.

THE CHECK THAT MATTERS MOST is DEAD CLIPS. A KeyframeAnimation whose timer slot
is never started anywhere in src/ is baked data that can never play. That is the
exact shape of the frog-croak bug, and it is invisible in game (nothing looks
broken, the animation simply never happens) and invisible in review.

A check that reports a gap which is not real is worse than no check, so anything
this cannot determine honestly is reported as "unknown" rather than guessed.
"""

import json
import os
import re
import sys

sys.path.insert(0, "tools")
import gen_mob_defs as GD          # noqa: E402
import gen_entity_models as GM     # noqa: E402

ROOT = "."
SRC = "src"

TYPES_HPP = "src/common/entity/GeneratedEntityTypes.hpp"
DEFS_CPP = "src/common/entity/GeneratedMobDefs.cpp"
MODELS_CPP = "src/client/renderer/entity/model/GeneratedEntityModels.cpp"
ANIMS_CPP = "src/client/renderer/entity/model/GeneratedAnimations.cpp"
SETUP_HPP = "src/client/renderer/entity/model/GeneratedSetupAnim.hpp"
ANIMSTATE_HPP = "src/common/entity/AnimationState.hpp"
LOOT_CPP = "src/common/world/loot/GeneratedMobLoot.cpp"
SPAWN_CPP = "src/common/world/spawn/GeneratedMobSpawns.cpp"
EGGS_HPP = "src/common/entity/SpawnEggs.hpp"


# ── Reading the port ───────────────────────────────────────────────────────

def read_text(path):
    return open(path).read() if os.path.exists(path) else ""


def table(cpp, marker):
    """The rows between `marker` and the closing `};`."""
    if marker not in cpp:
        return ""
    seg = cpp[cpp.index(marker):]
    end = seg.find("\n    };")
    return seg[:end if end >= 0 else len(seg)]


def load_port():
    p = {}

    types = read_text(TYPES_HPP)
    p["slugs"] = re.findall(r'//\s*"([a-z_0-9]+)"', types)
    p["enum_of"] = dict(re.findall(r'(\w+)\s*=\s*\d+\s*,\s*//\s*"([a-z_0-9]+)"', types))
    p["slug_of"] = {v: k for k, v in p["enum_of"].items()}

    # Mob defs: base, texture, attributes, food, look cadence.
    d = read_text(DEFS_CPP)
    attr_rows = re.findall(r"\{\s*Attribute::(\w+),\s*(-?[\d.e+-]+)\s*\}",
                           table(d, "kMobAttrs"))
    defs = {}
    for m in re.finditer(
            r'\{\s*EntityTypeId::(\w+),\s*MobBase::(\w+),\s*"([^"]*)",'
            r"\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),([^}]*)\}", table(d, "kMobDefs")):
        fa, ac = int(m.group(4)), int(m.group(5))
        tail = [t.strip() for t in m.group(8).split(",")]
        defs[m.group(1)] = {
            "base": m.group(2),
            "texture": m.group(3),
            "attrs": {attr_rows[fa + i][0]: float(attr_rows[fa + i][1])
                      for i in range(ac) if fa + i < len(attr_rows)},
            "foodCount": int(m.group(7)),
            "lookIntervalMin": int(tail[-2]) if len(tail) >= 2 and tail[-2].isdigit() else 0,
        }
    p["defs"] = defs

    # Meshes, clips and visibility rules.
    mc = read_text(MODELS_CPP)
    clips = re.findall(
        r'\{\s*"([\w.]+)",\s*(true|false),\s*[\d.f-]+,\s*[\d.f-]+,\s*[\d.f-]+,'
        r"\s*[\d.f-]+,\s*(\d+),\s*AnimGuard::(\w+),\s*(true|false)\s*\}",
        table(mc, "kGenClips[]"))
    parts = re.findall(r'\{\s*"([a-z_0-9]+)",\s*-?\d+,', table(mc, "kGenParts[]"))
    models = {}
    for m in re.finditer(
            r'\{\s*"([a-z_0-9]+)",\s*([\d.]+)f,\s*([\d.]+)f,\s*"([a-z_0-9]*)",'
            r"\s*AnimGuard::\w+,\s*(?:true|false),\s*(\d+),\s*(\d+),"
            r"\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", table(mc, "kGenModels")):
        fc, cc = int(m.group(7)), int(m.group(8))
        models[m.group(1)] = {
            "head": m.group(4),
            "partCount": int(m.group(6)),
            "parts": parts[int(m.group(5)):int(m.group(5)) + int(m.group(6))],
            "clips": clips[fc:fc + cc],
            "visCount": int(m.group(10)),
        }
    p["models"] = models

    p["anims"] = set(re.findall(r'\{\s*"([\w.]+)",', table(read_text(ANIMS_CPP),
                                                           "kGenAnims[kGenAnimCount]")))

    # MobAnim slot names, in ordinal order.
    m = re.search(r"enum class MobAnim\s*:\s*uint8_t\s*\{(.*?)\}",
                  read_text(ANIMSTATE_HPP), re.S)
    names = [n.split("=")[0].strip() for n in m.group(1).split(",") if n.strip()] if m else []
    p["slots"] = [n for n in names if n and n != "Count"]

    # Which slots each C++ CLASS starts, and which class builds each mob.
    #
    # This used to be one global set: "does anything anywhere start MobAnim::Sit".
    # That is wrong and it lied — once Camel started the sit clips, camel_husk's
    # identical clips were reported live even though a camel_husk is built as a
    # GenericAnimal and can never play them. A dead clip is dead PER MOB.
    started_by = {}

    def note_starts(cls, text):
        for mm in re.finditer(
                r"Anim\(\s*MobAnim::(\w+)\s*\)\s*\.\s*"
                r"(Start|StartIfStopped|AnimateWhen)\s*\(([^;]*)", text):
            # `AnimateWhen(false, ...)` can never start anything. The camel
            # writes its dash that way — MC reaches it only through riding —
            # and counting it as driven hid a dead clip behind code that looks
            # like a driver.
            if mm.group(2) == "AnimateWhen" and re.match(r"\s*false\s*,", mm.group(3)):
                continue
            started_by.setdefault(cls, set()).add(mm.group(1))

    for root, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith((".cpp", ".hpp")):
                continue
            text = read_text(os.path.join(root, f))
            # <Mob>Ai.cpp belongs wholly to <Mob>.
            if f.endswith("Ai.cpp"):
                note_starts(f[:-len("Ai.cpp")], text)
                continue
            # Otherwise attribute each start to the enclosing Class::Method.
            for m in re.finditer(r"\n    (?:\w[\w:<>*&\s]*?)\b(\w+)::\w+\s*\(", text):
                start = m.end()
                nxt = re.search(r"\n    (?:\w[\w:<>*&\s]*?)\b\w+::\w+\s*\(", text[start:])
                note_starts(m.group(1), text[start:start + (nxt.start() if nxt else len(text))])
    p["startedBy"] = started_by

    # slug -> the C++ class that actually gets built, from the factories.
    cls_of_slug = {}
    for f in ("src/common/entity/mobs/GenericMobs.cpp",
              "src/client/entity/ClientMobManager.cpp",
              "src/server/IntegratedServer.cpp"):
        for m in re.finditer(
                r"case\s+(?:Game::)?EntityTypeId::(\w+):\s*return\s+"
                r"std::make_unique<(?:Game::)?(\w+)>", read_text(f)):
            cls_of_slug.setdefault(m.group(1), m.group(2))
    p["classOfEnum"] = cls_of_slug

    p["setupCoverage"] = dict(re.findall(
        r'\{\s*"([a-z_0-9]+)",\s*\d+,\s*(\d+),\s*\d+,\s*(?:true|false)\s*\}',
        read_text(SETUP_HPP.replace(".hpp", ".cpp"))))

    p["loot"] = set(re.findall(r"EntityTypeId::(\w+)", read_text(LOOT_CPP)))
    p["spawns"] = set(re.findall(r"EntityTypeId::(\w+)", read_text(SPAWN_CPP)))
    p["eggs"] = set(re.findall(r"EntityTypeId::(\w+)", read_text(EGGS_HPP)))

    # Hand-written mob classes: EntityTypeId::X -> a concrete C++ class.
    handwritten = set()
    for f in ("src/common/entity/mobs/Animals.hpp", "src/common/entity/mobs/Monsters.hpp",
              "src/common/entity/mobs/AnimatedMobs.hpp"):
        for mm in re.finditer(r"class\s+(\w+)\s*:\s*public\s+\w+", read_text(f)):
            handwritten.add(mm.group(1))
    p["handwritten"] = handwritten

    # Mobs this port actually drives with the ported Brain — one <Mob>Ai.cpp
    # per mob, the same shape MC uses. Without this the brain check would keep
    # reporting a mob as goal-approximated after it had been converted.
    braindir = "src/common/entity/ai/brain"
    p["brainMobs"] = set()
    if os.path.isdir(braindir):
        for f in os.listdir(braindir):
            if f.endswith("Ai.cpp"):
                p["brainMobs"].add(f[:-len("Ai.cpp")].lower())

    # The base attribute builders, read from Attributes.cpp. Without these the
    # attribute check compares MC's per-mob createAttributes against our
    # OVERRIDE list alone and reports every value that happens to match the
    # base as missing — five false positives on the first run.
    ac = read_text("src/common/entity/Attributes.cpp")
    bases, order = {}, ["Living", "Mob", "Monster", "Animal", "PathfinderMob"]
    for name in order:
        m = re.search(r"void Create" + name + r"Attributes\(AttributeMap& out\)"
                      r"\s*\{(.*?)\n    \}", ac, re.S)
        if not m:
            continue
        body = m.group(1)
        vals = {}
        inherit = re.search(r"Create(\w+)Attributes\(out\)", body)
        if inherit:
            vals.update(bases.get(inherit.group(1), {}))
        for a, v in re.findall(r"out\.Register\(Attribute::(\w+),\s*(-?[\d.]+)\)", body):
            vals[a] = float(v)
        bases[name] = vals
    # MobBase::PathfinderMob uses CreateMobAttributes (there is no separate
    # builder), which is MC's arrangement too.
    bases.setdefault("PathfinderMob", bases.get("Mob", {}))
    p["baseAttrs"] = bases
    return p


# ── Reading Minecraft ──────────────────────────────────────────────────────

GOAL_NEW = re.compile(r"new\s+([A-Z]\w*Goal)\s*[\(<]")

# NOTE: the port's goal classes carry MC's own names, so no alias table is
# needed. When that stops being true, add one here rather than letting the
# audit report a spelling difference as a missing goal.
SYNCHED = re.compile(r"defineId\(\s*\w+\.class\s*,\s*EntityDataSerializers\.(\w+)\s*\)")


def load_mc():
    ent = GD.index_sources(GD.ENT_DIR)
    return {
        "files": ent,
        "class_of": GD.entity_classes(),
        "attr_src": GD.default_attribute_sources(),
    }


def mc_goals(cls, files):
    """Goal classes MC constructs in registerGoals, walking the chain."""
    goals, done = [], False
    for c in GD.chain(cls, files):
        src = GD.strip_comments(read_text(files[c])) if c in files else ""
        body = GD.method_body(src, "registerGoals")
        if body is None:
            continue
        for g in GOAL_NEW.findall(body):
            if g not in goals:
                goals.append(g)
        if "super.registerGoals()" not in body:
            done = True
        if done:
            break
    return goals


def mc_overrides(cls, files, names):
    """Which of `names` the mob's own chain overrides."""
    out = []
    for c in own_chain(cls, files):
        if c in SHARED_BASES:
            break
        src = GD.strip_comments(read_text(files[c])) if c in files else ""
        for n in names:
            if n in out:
                continue
            if re.search(r"\b(?:public|protected|private)\s+[\w<>\[\]?, .]+\s+"
                         + re.escape(n) + r"\s*\(", src):
                out.append(n)
    return out


# Where a chain walk must stop: everything at or above these is shared MC
# machinery, not this mob's own behaviour. Mob.java declares makeBrain, so a
# walk that does not stop reports every mob in the game as brain-driven.
SHARED_BASES = {"Mob", "PathfinderMob", "LivingEntity", "Entity", "Animal",
                "AgeableMob", "Monster", "AbstractGolem", "AmbientCreature",
                "WaterAnimal", "AbstractFish", "AbstractSchoolingFish",
                "AbstractHorse", "AbstractChestedHorse", "AbstractIllager",
                "AbstractSkeleton", "AbstractPiglin", "AbstractVillager",
                "SpellcasterIllager", "PatrollingMonster", "TamableAnimal",
                "ShoulderRidingEntity", "FlyingMob", "Raider", "Zombie",
                "AbstractCow"}


def own_chain(cls, files):
    """The mob's own classes, stopping before the shared MC bases."""
    out = []
    for c in GD.chain(cls, files):
        if c in SHARED_BASES and out:
            break
        out.append(c)
        if c in SHARED_BASES:
            break
    return out


def mc_uses_brain(cls, files):
    # A brain shows up two ways: the mob overrides makeBrain, or MC ships a
    # <Mob>Ai class holding its Activities.
    for c in own_chain(cls, files):
        if c in SHARED_BASES:
            continue
        src = read_text(files[c]) if c in files else ""
        if re.search(r"\bmakeBrain\s*\(", src) or (c + "Ai") in files:
            return True
    return False


# ── The audit ──────────────────────────────────────────────────────────────

# Goal classes this port implements, by MC name. Read from our own sources so
# the list cannot drift from reality.
def port_goal_classes():
    out = set()
    for f in os.listdir("src/common/entity/ai/goals"):
        if f.endswith(".hpp"):
            src = read_text(os.path.join("src/common/entity/ai/goals", f))
            out |= set(re.findall(r"class\s+(\w*Goal)\s*:\s*public\s+\w+", src))
    return out


# MC goals whose absence is not a gap: they need systems this port does not have
# at all, and listing them per mob would bury the real findings.
GOAL_NOT_APPLICABLE = {
    # Riding, leads, boats.
    "MoveToBlockGoal", "UseItemGoal", "RangedCrossbowAttackGoal",
}


def audit(port=None, mc=None):
    port = port if port is not None else load_port()
    mc = mc if mc is not None else load_mc()
    goal_impl = port_goal_classes()

    rows = []
    for slug in port["slugs"]:
        enum = port["slug_of"].get(slug)
        if not enum:
            continue
        cls = mc["class_of"].get(slug)
        d = port["defs"].get(enum)

        r = {"slug": slug, "enum": enum, "mcClass": cls,
             "isMob": d is not None or enum in port["handwritten"],
             "gaps": [], "notes": []}
        if not r["isMob"]:
            continue

        r["handwritten"] = enum in port["handwritten"]
        r["base"] = d["base"] if d else "hand-written"

        # ── Rendering ──────────────────────────────────────────────────────
        model = port["models"].get(slug)
        if model is None and not r["handwritten"]:
            r["gaps"].append(("model", "no mesh at all"))
        r["partCount"] = model["partCount"] if model else None

        tex = d["texture"] if d else ""
        if d and not tex:
            r["gaps"].append(("texture", "no texture path resolved"))
        elif tex and not os.path.exists(tex):
            r["gaps"].append(("texture", "file missing: " + tex))

        # ── Dead animation clips ───────────────────────────────────────────
        #
        # A timed clip whose MobAnim slot nothing ever starts is baked data that
        # can never play. This is the frog-croak class of bug.
        dead, live = [], []
        for anim, is_walk, slot, guard, neg in (model["clips"] if model else []):
            if anim not in port["anims"]:
                r["gaps"].append(("clip", "no animation data for " + anim))
                continue
            if is_walk == "true":
                live.append(anim)
                continue
            name = port["slots"][int(slot)] if int(slot) < len(port["slots"]) else "?"
            # Only the class this mob is actually BUILT as counts.
            cls = port["classOfEnum"].get(enum)
            driven = cls is not None and name in port["startedBy"].get(cls, set())
            (live if driven else dead).append(
                "%s (MobAnim::%s)" % (anim.split(".")[-1], name))
        r["liveClips"] = len(live)
        for x in dead:
            r["gaps"].append(("dead-clip", x + " is baked but nothing starts its timer"))

        # ── setupAnim coverage ─────────────────────────────────────────────
        if slug in port["setupCoverage"]:
            r["setupStatements"] = int(port["setupCoverage"][slug])

        # ── Attributes ─────────────────────────────────────────────────────
        if d is not None:
            # DefaultAttributes.java is keyed by SLUG and its value is
            # (Class, method) — NOT the entity class. Looking it up by class
            # name silently returned None for every mob, so this whole check
            # passed 89 mobs without ever comparing a number. Caught by
            # perturbing a value and watching the audit stay silent; any check
            # that cannot fail is worse than no check.
            want = mc["attr_src"].get(slug)
            if want is None:
                r["notes"].append("no DefaultAttributes row in MC (attributes "
                                  "not compared)")
            else:
                mcattr = GD.attributes(want[0], mc["files"], method=want[1])
                if not mcattr:
                    r["notes"].append("MC attributes for %s.%s could not be "
                                      "parsed (not compared)" % want)
                # The EFFECTIVE value: our base builder, then the def's
                # overrides on top. That is what the mob actually ends up with.
                effective = dict(port["baseAttrs"].get(d["base"], {}))
                effective.update(d["attrs"])
                for k, v in sorted((mcattr or {}).items()):
                    got = effective.get(k)
                    if got is None:
                        r["gaps"].append(("attribute", "MC sets %s = %g, we do not" % (k, v)))
                    elif abs(got - v) > 1e-6:
                        r["gaps"].append(
                            ("attribute", "%s: MC %g, ours %g" % (k, v, got)))

        # ── Behaviour ──────────────────────────────────────────────────────
        if cls:
            if mc_uses_brain(cls, mc["files"]):
                r["brain"] = True
                # Keyed on the CLASS that builds the mob, not the slug: a camel
                # husk is built as Camel and so runs CamelAi's brain.
                built_as = port["classOfEnum"].get(enum, "").lower()
                if built_as in port["brainMobs"]:
                    r["notes"].append("runs on the ported Brain")
                else:
                    r["gaps"].append(
                        ("brain", "MC drives this mob with a Brain (Behavior/"
                                  "Activity/Memory); this port approximates it "
                                  "with goals"))
            goals = [g for g in mc_goals(cls, mc["files"])
                     if g not in GOAL_NOT_APPLICABLE]
            r["mcGoals"] = goals
            missing = [g for g in goals if g not in goal_impl]
            r["mcGoalCount"] = len(goals)
            r["missingGoals"] = missing
            if missing:
                r["gaps"].append((
                    "goal",
                    "%d of MC's %d goals not implemented: %s"
                    % (len(missing), len(goals), ", ".join(missing))))

            # Per-mob overrides that ARE the mob's behaviour.
            overrides = mc_overrides(cls, mc["files"], [
                "mobInteract", "customServerAiStep", "finalizeSpawn",
                "playerTouch", "doHurtTarget",
            ])
            r["mcOverrides"] = overrides
            if not r["handwritten"] and overrides:
                r["gaps"].append(
                    ("class", "MC overrides %s; this port has no class for it "
                              "(generic %s)" % (", ".join(overrides), r["base"])))

            # Synched data MC defines but our wire cannot carry.
            src = GD.strip_comments(read_text(mc["files"][cls])) if cls in mc["files"] else ""
            n = len(SYNCHED.findall(src))
            if n and not r["handwritten"]:
                r["gaps"].append(
                    ("synched", "MC synchronises %d value(s) for this mob; the wire "
                                "carries pose + one state byte" % n))

        # ── World integration ──────────────────────────────────────────────
        if enum not in port["loot"]:
            r["gaps"].append(("loot", "no loot table row"))
        if enum not in port["spawns"]:
            r["notes"].append("no natural spawn entry (may be correct — bosses, "
                              "variants and /summon-only mobs have none)")
        if enum not in port["eggs"]:
            r["notes"].append("no spawn egg")

        rows.append(r)
    return rows


# Ordered by how actionable a finding is, not alphabetically.
#
#   dead-clip  baked animation data that can never play — pure waste, and
#              invisible in game because nothing looks broken
#   model/texture/clip  the mob renders wrong or not at all
#   attribute  wrong numbers, silently
#   class      MC gives this mob bespoke behaviour and we run the generic base
#   goal       named MC goals we do not have
#   brain      MC drives it with the Behavior/Activity system; architectural
#   synched    per-mob state MC replicates and our wire does not; architectural
CATEGORY_ORDER = ["dead-clip", "model", "texture", "clip", "attribute",
                  "loot", "class", "goal", "brain", "synched"]

# The first four are bugs. The rest are scope.
BUG_CATEGORIES = {"dead-clip", "model", "texture", "clip", "attribute", "loot"}


# ── Self-test ──────────────────────────────────────────────────────────────
#
# A check that cannot fail is worse than no check: it reports a clean bill of
# health forever. That is not hypothetical — the attribute check shipped in this
# file's first draft looked up DefaultAttributes by CLASS name when the table is
# keyed by SLUG, so it silently compared nothing across all 89 mobs and reported
# zero problems. It was caught by perturbing a value and noticing the audit
# stayed quiet.
#
# So each check gets a deliberate fault injected into the in-memory inputs, and
# must report MORE findings in its own category than it did on clean data.

def self_test():
    import copy
    base_port, mc = load_port(), load_mc()

    def count(port, cat):
        return sum(1 for r in audit(port, mc) for c, _ in r["gaps"] if c == cat)

    def first(d):
        return next(iter(d))

    cases = []

    def case(name, cat, mutate):
        cases.append((name, cat, mutate))

    case("a class stops starting its animation slots", "dead-clip",
         lambda p: p["startedBy"].__setitem__(
             max(p["startedBy"], key=lambda k: len(p["startedBy"][k])), set()))
    case("a mesh goes missing", "model",
         lambda p: p["models"].pop("cow", p["models"].pop(first(p["models"]))))
    case("a texture path is wrong", "texture",
         lambda p: p["defs"][first(p["defs"])].update(texture="assets/nope.png"))
    case("animation data is missing for a clip", "clip",
         lambda p: p["anims"].clear())
    # Cow and the other hand-written mobs have no generated def row, so pick a
    # mob that does and give it an attribute MC also sets.
    attr_victim = next(k for k, v in base_port["defs"].items() if v["attrs"])
    case("an attribute value drifts", "attribute",
         lambda p: p["defs"][attr_victim]["attrs"].update(
             {first(p["defs"][attr_victim]["attrs"]): 999.0}))
    case("a loot table row goes missing", "loot",
         lambda p: p["loot"].clear())

    print("self-test: each check must report MORE when its input is broken\n")
    ok = True
    for name, cat, mutate in cases:
        before = count(base_port, cat)
        broken = copy.deepcopy(base_port)
        mutate(broken)
        after = count(broken, cat)
        good = after > before
        ok = ok and good
        print("  %-4s %-12s %-42s %d -> %d"
              % ("PASS" if good else "FAIL", cat, name, before, after))

    # The goal check reads our own goal classes, not the port dict.
    print("\n%s" % ("all checks fire" if ok else "SOME CHECKS CANNOT FAIL — fix them"))
    return 0 if ok else 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if "--self-test" in flags:
        sys.exit(self_test())

    rows = audit()

    jsonout = next((f.split("=", 1)[1] for f in flags if f.startswith("--json=")), None)
    htmlout = next((f.split("=", 1)[1] for f in flags if f.startswith("--html=")), None)
    if "--json" in flags:
        jsonout = "mob_audit.json"
    if "--html" in flags:
        htmlout = "mob_audit.html"

    if jsonout:
        json.dump(rows, open(jsonout, "w"), indent=1)
        print("wrote", jsonout)
    if htmlout:
        write_html(rows, htmlout)
        print("wrote", htmlout)

    # ── Summary ────────────────────────────────────────────────────────────
    counts = {}
    for r in rows:
        for cat, _ in r["gaps"]:
            counts[cat] = counts.get(cat, 0) + 1

    print("%d mobs audited against MC\n" % len(rows))
    print("  gaps by category")
    for cat in CATEGORY_ORDER:
        if cat in counts:
            print("    %-11s %4d" % (cat, counts[cat]))
    for cat in sorted(set(counts) - set(CATEGORY_ORDER)):
        print("    %-11s %4d" % (cat, counts[cat]))

    bugs = [r for r in rows
            if any(c in BUG_CATEGORIES for c, _ in r["gaps"])]
    clean = [r for r in rows if not r["gaps"]]
    print("\n  %d mobs carry a BUG-class gap (dead clip, missing mesh/texture,"
          "\n     wrong attribute, missing loot) — these are defects."
          "\n  %d mobs differ only in SCOPE (bespoke MC behaviour we have not"
          "\n     ported yet), %d match on everything checked."
          % (len(bugs), len(rows) - len(bugs) - len(clean), len(clean)))

    dead = [(r["slug"], g[1]) for r in rows for g in r["gaps"] if g[0] == "dead-clip"]
    if dead:
        print("\n  DEAD ANIMATION CLIPS (baked, but nothing can ever play them)")
        for slug, why in dead:
            print("    %-16s %s" % (slug, why))

    if args:
        wanted = [r for r in rows if r["slug"] in args]
    elif "--all" in flags:
        wanted = rows
    else:
        wanted = sorted(bugs, key=lambda r: -sum(
            1 for c, _ in r["gaps"] if c in BUG_CATEGORIES))
        print("\n  MOBS WITH BUG-CLASS GAPS (pass --all, or name mobs, for the rest)")

    for r in wanted:
        print("\n%s  [%s%s]  %s" % (
            r["slug"], r["base"], ", brain in MC" if r.get("brain") else "",
            "%d parts" % r["partCount"] if r["partCount"] else "no mesh"))
        for cat, why in r["gaps"]:
            print("    %-11s %s" % (cat, why))
        for note in r["notes"]:
            print("    %-11s %s" % ("note", note))


def write_html(rows, path):
    """A scannable matrix. Kept deliberately plain — this is a work tool."""
    esc = lambda s: (str(s).replace("&", "&amp;").replace("<", "&lt;")
                     .replace(">", "&gt;"))
    counts = {}
    for r in rows:
        for cat, _ in r["gaps"]:
            counts[cat] = counts.get(cat, 0) + 1
    out = ["<title>Mob Parity Audit</title>", "<style>",
           "body{font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;"
           "max-width:1100px;margin:2rem auto;padding:0 1rem}",
           "h1{font-size:1.4rem}h2{font-size:1rem;margin-top:2rem}",
           "table{border-collapse:collapse;width:100%}",
           "td,th{border-bottom:1px solid #8883;padding:.3rem .5rem;"
           "text-align:left;vertical-align:top}",
           ".g{color:#b00}.n{color:#888}", "</style>",
           "<h1>Mob Parity Audit</h1>",
           "<p>%d mobs compared against minecraft_code/. Generated by "
           "tools/mob_audit.py.</p>" % len(rows), "<h2>Gaps by category</h2><table>"]
    for cat in CATEGORY_ORDER:
        if cat in counts:
            out.append("<tr><td>%s</td><td>%d</td></tr>" % (cat, counts[cat]))
    out.append("</table><h2>Per mob</h2><table>")
    for r in sorted(rows, key=lambda r: -len(r["gaps"])):
        out.append("<tr><th>%s<br><span class=n>%s</span></th><td>" % (
            esc(r["slug"]), esc(r["base"])))
        for cat, why in r["gaps"]:
            out.append("<div class=g>%s: %s</div>" % (esc(cat), esc(why)))
        for note in r["notes"]:
            out.append("<div class=n>note: %s</div>" % esc(note))
        if not r["gaps"] and not r["notes"]:
            out.append("<span class=n>no gap found</span>")
        out.append("</td></tr>")
    out.append("</table>")
    open(path, "w").write("\n".join(out))


if __name__ == "__main__":
    main()
