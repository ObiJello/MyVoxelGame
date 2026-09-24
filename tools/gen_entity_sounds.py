#!/usr/bin/env python3
"""Generate src/common/sound/GeneratedEntitySounds.inc — every entity type's
sounds, read out of the Java classes that define them.

For each entity type the engine has (GeneratedEntityTypes.cpp), the class that
builds it is found through its registration (`Builder.of(Class::new ...)`) in

    minecraft_code_26.3-pre-2 (vanilla 26.3)       EntityTypes.java
    mods_reference/twilightforest        TFEntities.java      (TFSounds.X)
    mods_reference/aether                AetherEntityTypes.java (AetherSoundEvents.X)

and the class's sound methods are resolved up its `extends` chain, exactly as
Java dispatch would:

    getAmbientSound / getHurtSound / getDeathSound      the three voice events
    playStepSound                                       step event, volume, pitch
                                                        (or the Entity default:
                                                        the block's SoundType)
    getSwimSound / getSwimSplashSound / getSwimHighSpeedSplashSound
    getSoundSource / getSoundVolume / getAmbientSoundInterval

A method body is understood when it is a single `return` of a sound constant,
of null, or of `this.<predicate>() ? A : B` for a predicate the engine can
evaluate (isBaby, isInWater, isUnderWater, isEyeInFluid(WATER), onGround).
Anything else — a sound chosen from AI state, a variant sound set, a block
tag — is left to the parent's answer here and HAND-CODED as a virtual override
on the engine class (see the unresolved list printed at the end, and the
`// unresolved:` comment on each row).

The Hush's creatures have no Java source; their rows name engine events
(obeycraft:entity.<slug>.*), which assets/sound_overlays/obeycraft/
entities.json maps onto vanilla sounds.

    python3 tools/gen_entity_sounds.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MC = os.path.join(ROOT, "minecraft_code_26.3-pre-2/decompiled_net/minecraft")
TF = os.path.join(ROOT, "mods_reference/twilightforest/src/main/java")
AE = os.path.join(ROOT, "mods_reference/aether/src/main/java")
ENGINE_TYPES = os.path.join(ROOT, "src/common/entity/GeneratedEntityTypes.cpp")
OUT = os.path.join(ROOT, "src/common/sound/GeneratedEntitySounds.inc")

# Engine slugs whose mod registry name differs (none today; the engine uses
# the mods' own registry names).
SLUG_TO_MOD = {}

# The Hush's creatures (docs/the-hush.md): engine events, mapped in
# assets/sound_overlays/obeycraft/entities.json. (step None = no footsteps:
# a flier, a swimmer, a crawler with no feet.)
HUSH = {
    #  slug             source     volume  step?   ambient interval
    "echo_wraith":    ("Hostile", 1.0,   False, 80),
    "hushling":       ("Hostile", 1.0,   True,  80),
    "silent_warden":  ("Hostile", 4.0,   True,  80),
    "choir_mother":   ("Hostile", 4.0,   False, 80),
    "crystal_golem":  ("Neutral", 1.0,   True,  80),
    "echo_mimic":     ("Hostile", 1.0,   True,  80),
    "hush_leviathan": ("Hostile", 5.0,   False, 80),
    "lumen_moth":     ("Ambient", 0.6,   False, 80),
    # Aurelith's boss (TheUnsung.hpp): a hovering conductor, no feet.
    "the_unsung":     ("Hostile", 4.0,   False, 100),
}


# ── Java helpers ──────────────────────────────────────────────────────────────

def balanced_braces(text, open_idx):
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


CLASS_RE = re.compile(r'(?:public|protected|private|abstract|final|static|\s)*class\s+(\w+)(?:<[^{]*?>)?'
                      r'(?:\s+extends\s+([\w.]+)(?:<[^{]*?>)?)?[^{]*\{')

METHODS = {
    "ambient": re.compile(r'SoundEvent\s+getAmbientSound\s*\(\s*\)\s*\{'),
    "hurt":    re.compile(r'SoundEvent\s+getHurtSound\s*\([^)]*\)\s*\{'),
    "death":   re.compile(r'SoundEvent\s+getDeathSound\s*\(\s*\)\s*\{'),
    "step":    re.compile(r'void\s+playStepSound\s*\([^)]*\)\s*\{'),
    "stepSound": re.compile(r'SoundEvent\s+getStepSound\s*\(\s*\)\s*\{'),
    "swim":    re.compile(r'SoundEvent\s+getSwimSound\s*\(\s*\)\s*\{'),
    "splash":  re.compile(r'SoundEvent\s+getSwimSplashSound\s*\(\s*\)\s*\{'),
    "splashHigh": re.compile(r'SoundEvent\s+getSwimHighSpeedSplashSound\s*\(\s*\)\s*\{'),
    "source":  re.compile(r'SoundSource\s+getSoundSource\s*\(\s*\)\s*\{'),
    "volume":  re.compile(r'float\s+getSoundVolume\s*\(\s*\)\s*\{'),
    "interval": re.compile(r'int\s+getAmbientSoundInterval\s*\(\s*\)\s*\{'),
    "emission": re.compile(r'MovementEmission\s+getMovementEmission\s*\(\s*\)\s*\{'),
}


class JClass:
    def __init__(self, name, parent, root):
        self.name, self.parent, self.root = name, parent, root
        self.bodies = {}


def load_classes(root):
    """name -> JClass for every top-level class under `root`."""
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            if not f.endswith(".java"):
                continue
            src = open(os.path.join(dirpath, f), encoding="utf-8", errors="replace").read()
            m = CLASS_RE.search(src)
            if not m or m.group(1) != f[:-5]:
                continue
            parent = m.group(2).split(".")[-1] if m.group(2) else None
            jc = JClass(m.group(1), parent, root)
            body_start = m.end() - 1
            body_end = balanced_braces(src, body_start)
            body = src[body_start:body_end]
            for key, rx in METHODS.items():
                mm = rx.search(body)
                if mm:
                    b0 = mm.end() - 1
                    b1 = balanced_braces(body, b0)
                    jc.bodies[key] = body[b0 + 1:b1 - 1].strip()
            out[jc.name] = jc
    return out


# ── Sound constant registries ─────────────────────────────────────────────────

def vanilla_consts():
    src = open(os.path.join(MC, "sounds/SoundEvents.java")).read()
    d = dict(re.findall(r'public static final [^=;]+?\s+([A-Z0-9_]+)\s*=\s*register(?:ForHolder)?\("([a-z0-9_./-]+)"\)', src))
    return d


def mod_consts(path, fn, ns):
    if not os.path.exists(path):
        return {}
    src = open(path).read()
    return {k: ns + ":" + v for k, v in re.findall(
        r'public static final [^=;]+?\s+([A-Z0-9_]+)\s*=\s*' + fn + r'\("([a-z0-9_./-]+)"\)', src)}


VANILLA = vanilla_consts()
# Every vanilla event id, variant sets included (the generated registry).
VANILLA_IDS = set(re.findall(r'SOUND_EVENT\(\w+, "([^"]+)"\)',
                             open(os.path.join(ROOT, "src/common/sound/GeneratedSoundEvents.inc")).read()))
TFS = mod_consts(os.path.join(TF, "twilightforest/init/TFSounds.java"), "createEvent", "twilightforest")
AES = mod_consts(os.path.join(AE, "com/aetherteam/aether/client/AetherSoundEvents.java"), "register", "aether")


def sound_const(expr):
    """A sound expression -> event id, "" for null, or None if not a constant."""
    e = expr.strip().rstrip(";").strip()
    e = re.sub(r'^\((?:SoundEvent|Holder)\)\s*', '', e)
    if e == "null":
        return ""
    m = re.fullmatch(r'SoundEvents\.([A-Z0-9_]+)(?:\.value\(\))?', e)
    if m:
        return VANILLA.get(m.group(1))
    m = re.fullmatch(r'TFSounds\.([A-Z0-9_]+)\.(?:get|value)\(\)', e)
    if m:
        return TFS.get(m.group(1))
    m = re.fullmatch(r'AetherSoundEvents\.([A-Z0-9_]+)\.(?:get|value)\(\)', e)
    if m:
        return AES.get(m.group(1))
    return None


# The classic (default-variant) sound sets of the variant animals, and their
# baby sets: MC <Animal>.getSoundSet() is `isBaby() ? babySounds : adultSounds`
# (the cow has no baby set). The engine plays the CLASSIC variant — no sound
# variant is synced — so the set collapses to a Baby predicate.
SOUND_SET_CLASSES = {
    # class         adult prefix  step constant   has baby set
    "Pig":         ("pig",     "PIG_STEP",     True),
    "AbstractCow": ("cow",     None,           False),
    "Chicken":     ("chicken", "CHICKEN_STEP", True),
    "Wolf":        ("wolf",    "WOLF_STEP",    True),
    "Cat":         ("cat",     None,           True),
}
CURRENT_CLASS = [None]


def sound_set(word):
    """this.getSoundSet().<word>Sound().value() in CURRENT_CLASS -> (pred, t, f)."""
    info = SOUND_SET_CLASSES.get(CURRENT_CLASS[0])
    if not info:
        return None
    prefix, step_const, has_baby = info
    snake = re.sub(r'([A-Z])', lambda m: '_' + m.group(1).lower(), word)
    adult = VANILLA.get(step_const) if (snake == "step" and step_const) else "entity.%s.%s" % (prefix, snake)
    if not has_baby:
        return ("None", adult, adult)
    baby = VANILLA.get("%s_%s_BABY" % (prefix.upper(), snake.upper()))
    if not baby:
        return ("None", adult, adult)
    return ("Baby", baby, adult)


PREDICATES = {
    "this.isBaby()": "Baby",
    "this.isInWater()": "InWater",
    "this.isUnderWater()": "UnderWater",
    "this.isEyeInFluid(FluidTags.WATER)": "UnderWater",
    "this.onGround()": "OnGround",
}


def eval_sound_expr(expr):
    """-> (pred, whenTrue, whenFalse) or None."""
    ss = re.fullmatch(r'\s*(?:\(SoundEvent\))?this\.getSoundSet\(\)\.(\w+)Sound\(\)\.value\(\)\s*;?\s*', expr)
    if ss:
        return sound_set(ss.group(1))
    if expr.strip().rstrip(";").strip() == "this.getStepSound()":
        return STEP_SOUND[0]
    c = sound_const(expr)
    if c is not None:
        return ("None", c, c)
    m = re.fullmatch(r'\s*(.+?)\s*\?\s*(.+?)\s*:\s*(.+?)\s*;?\s*', expr, re.S)
    if m and m.group(1).strip() in PREDICATES:
        a, b = sound_const(m.group(2)), sound_const(m.group(3))
        if a is not None and b is not None:
            return (PREDICATES[m.group(1).strip()], a, b)
    return None


def eval_return(body):
    m = re.fullmatch(r'return\s+(.+);', body.strip(), re.S)
    if not m:
        return None
    return eval_sound_expr(m.group(1))


def eval_step(body):
    """-> ("None"|"Event", pred, t, f, vol, pitch) or None."""
    b = body.strip()
    if b == "":
        return ("None", "None", "", "", 0.0, 0.0)
    # `SoundEvent sound = <expr>; this.playSound(sound, v, p);`
    local = re.fullmatch(r'SoundEvent\s+(\w+)\s*=\s*(.+?);\s*this\.playSound\(\s*\1\s*,\s*([\d.]+)F\s*,\s*([\d.]+)F\s*\);', b, re.S)
    if local:
        r = eval_sound_expr(local.group(2))
        if r:
            return ("Event", r[0], r[1], r[2], float(local.group(3)), float(local.group(4)))
        return None
    m = re.fullmatch(r'this\.playSound\(\s*(.+)\s*,\s*([\d.]+)F\s*,\s*([\d.]+)F\s*\);', b, re.S)
    if m:
        r = eval_sound_expr(m.group(1))
        if r:
            return ("Event", r[0], r[1], r[2], float(m.group(2)), float(m.group(3)))
    return None


STEP_SOUND = [None]

# Mod events the engine's hand-written mob code plays beyond the rows (a yeti's
# grab, a zephyr's shot) — every "twilightforest:..." / "aether:..." literal
# under src/common/entity, pulled into the overlays with the rest.
def engine_mod_events():
    found = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src/common/entity")):
        for name in files:
            if name.endswith((".cpp", ".hpp", ".inc")):
                with open(os.path.join(dirpath, name), errors="replace") as f:
                    found += re.findall(r'"((?:twilightforest|aether):entity\.[a-z0-9_.]+)"', f.read())
    return sorted(set('"%s"' % e for e in found))


EXTRA_MOD_EVENTS = engine_mod_events()


def resolve(classes_by_root, jc, key, unresolved):
    """Walk the extends chain; returns the first understood body's value, and
    records an unresolved override on the way."""
    cur = jc
    seen = set()
    if key == "step":
        # `this.playSound(this.getStepSound(), ...)` (Zombie, AbstractSkeleton):
        # the getStepSound override the chain resolves to.
        STEP_SOUND[0] = resolve(classes_by_root, jc, "stepSound", [])
    while cur and cur.name not in seen:
        seen.add(cur.name)
        CURRENT_CLASS[0] = cur.name
        if key in cur.bodies:
            body = cur.bodies[key]
            if key == "step":
                v = eval_step(body)
                # Entity's own playStepSound is the block default.
                if cur.name == "Entity":
                    return ("Block", "None", "", "", 0.15, 1.0)
            elif key == "source":
                m = re.fullmatch(r'return\s+SoundSource\.([A-Z_]+);', body)
                v = m.group(1) if m else None
            elif key == "volume":
                m = re.fullmatch(r'return\s+([\d.]+)F;', body)
                v = float(m.group(1)) if m else None
            elif key == "emission":
                m = re.fullmatch(r'return\s+(?:Entity\.)?MovementEmission\.([A-Z_]+);', body)
                v = m.group(1) if m else None
            elif key == "interval":
                m = re.fullmatch(r'return\s+(\d+);', body)
                v = int(m.group(1)) if m else None
            else:
                v = eval_return(body)
            if v is not None:
                return v
            unresolved.append("%s.%s" % (cur.name, key))
        cur = find_class(classes_by_root, cur.parent, cur.root)
    return None


def find_class(classes_by_root, name, root):
    if not name:
        return None
    if root in classes_by_root and name in classes_by_root[root]:
        return classes_by_root[root][name]
    return classes_by_root[MC].get(name)


def registrations(path, pattern):
    if not os.path.exists(path):
        return {}
    src = open(path).read()
    out = {}
    for m in re.finditer(pattern, src, re.S):
        out[m.group(1).lower()] = m.group(2)
    return out


SOURCE_NAMES = {"MASTER": "Master", "MUSIC": "Music", "RECORDS": "Records", "WEATHER": "Weather",
                "BLOCKS": "Blocks", "HOSTILE": "Hostile", "NEUTRAL": "Neutral", "PLAYERS": "Players",
                "AMBIENT": "Ambient", "VOICE": "Voice", "UI": "Ui"}


def cstr(s):
    return '"%s"' % s


# ── The mod overlays ──────────────────────────────────────────────────────────
#
# assets/sound_overlays/<mod>/entities.json: the mods' own sounds.json entries
# for every event a row names. An entry built from vanilla files or `type:
# event` references is copied as the mod wrote it (TF maps its boar straight
# onto the pig, for instance). An entry that plays the MOD'S OWN audio — which
# this engine does not ship (The Aether's is all-rights-reserved) — is
# replaced by the vanilla creature the mob is modelled on, pitch-shaped:
MOD_AUDIO_STAND_INS = {
    # twilightforest: event suffix pattern -> (vanilla event base, pitch)
    "twilightforest:entity.twilightforest.blockchain_goblin.": ("entity.piglin_brute.", 1.4),
    "twilightforest:entity.twilightforest.goblin_knight.":     ("entity.piglin_brute.", 1.25),
    "twilightforest:entity.twilightforest.redcap.":            ("entity.piglin.", 1.4),
    "twilightforest:entity.twilightforest.kobold.":            ("entity.piglin.", 1.6),
    "twilightforest:entity.twilightforest.deer.":              ("entity.goat.", 1.15),
    "twilightforest:entity.twilightforest.fire_beetle.":       ("entity.silverfish.", 0.7),
    "twilightforest:entity.twilightforest.pinch_beetle.":      ("entity.silverfish.", 0.6),
    "twilightforest:entity.twilightforest.slime_beetle.":      ("entity.silverfish.", 0.8),
    "twilightforest:entity.twilightforest.helmet_crab.":       ("entity.silverfish.", 1.2),
    "twilightforest:entity.twilightforest.hostile_wolf.":      ("entity.wolf_angry.", 1.0),
    "twilightforest:entity.twilightforest.mist_wolf.":         ("entity.wolf_angry.", 0.8),
    "twilightforest:entity.twilightforest.winter_wolf.":       ("entity.wolf_angry.", 0.9),
    "twilightforest:entity.twilightforest.penguin.":           ("entity.chicken.", 0.8),
    "twilightforest:entity.twilightforest.squirrel.":          ("entity.rabbit.", 1.3),
    "twilightforest:entity.twilightforest.tiny_bird.":         ("entity.parrot.", 1.4),
    "twilightforest:entity.twilightforest.troll.":             ("entity.ravager.", 1.2),
    "twilightforest:entity.twilightforest.wraith.":            ("entity.vex.", 0.6),
    "twilightforest:entity.twilightforest.yeti.":              ("entity.polar_bear.", 0.9),
    "twilightforest:entity.twilightforest.mosquito.":          ("entity.bee.", 1.8),
    "twilightforest:entity.twilightforest.raven.":             ("entity.parrot.", 0.6),
    # aether
    "aether:entity.aerbunny.":    ("entity.rabbit.", 1.2),
    "aether:entity.aerwhale.":    ("entity.dolphin.", 0.4),
    "aether:entity.cockatrice.":  ("entity.parrot.", 0.7),
    "aether:entity.fire_minion.": ("entity.blaze.", 1.2),
    "aether:entity.moa.":         ("entity.chicken.", 0.7),
    "aether:entity.valkyrie.":    ("entity.player.", 1.3),
    "aether:entity.zephyr.":      ("entity.ghast.", 1.5),
}
# The vanilla event a stand-in suffix lands on when the vanilla creature names
# it differently (a wolf "growls" where the TF wolf is "ambient").
SUFFIX_REMAP = {
    ("entity.wolf_angry.", "ambient"): "growl",
    ("entity.bee.", "ambient"): "loop",
    ("entity.parrot.", "caw"): "ambient",
    ("entity.parrot.", "squawk"): "hurt",
    ("entity.polar_bear.", "growl"): "warning",
    ("entity.polar_bear.", "grab"): "warning",
    ("entity.polar_bear.", "throw"): "ambient",
    ("entity.dolphin.", "ambient"): "ambient_water",
    ("entity.player.", "hurt"): "hurt",
}


def mod_sounds_json(ns):
    import json
    path = os.path.join(ROOT, "mods_reference", ns, "src/generated/resources/assets", ns, "sounds.json")
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def uses_own_audio(entry, ns):
    for s in entry.get("sounds", []):
        name = s if isinstance(s, str) else s.get("name", "")
        kind = "file" if isinstance(s, str) else s.get("type", "file")
        if kind == "file" and name.startswith(ns + ":"):
            return True
    return False


def write_mod_overlay(ns, events):
    import json
    src = mod_sounds_json(ns)
    out, pending, done = {}, sorted(events), set()
    while pending:
        full = pending.pop()
        if full in done:
            continue
        done.add(full)
        key = full.split(":", 1)[1]
        entry = src.get(key)
        if entry is None:
            print("  ! %s: not in the mod's sounds.json — left silent, as in the mod" % full)
            continue
        if not uses_own_audio(entry, ns):
            out[key] = entry
            # A reference to another event of the mod comes along too.
            for s in entry.get("sounds", []):
                if isinstance(s, dict) and s.get("type") == "event" and s["name"].startswith(ns + ":"):
                    pending.append(s["name"])
            continue
        stand = None
        for prefix, (base, pitch) in MOD_AUDIO_STAND_INS.items():
            if full.startswith(prefix):
                suffix = full[len(prefix):]
                suffix = SUFFIX_REMAP.get((base, suffix), suffix)
                stand = (base + suffix, pitch)
                break
        if stand is None:
            print("  ! %s: plays the mod's own audio and has no stand-in" % full)
            continue
        target, pitch = stand
        if target not in VANILLA_IDS:
            print("  ! %s -> %s: not a vanilla event" % (full, target))
        sub = {"sounds": [{"name": target, "type": "event"} if pitch == 1.0
                          else {"name": target, "type": "event", "pitch": pitch}]}
        if "subtitle" in entry:
            sub["subtitle"] = entry["subtitle"]
        out[key] = sub
    path = os.path.join(ROOT, "assets/sound_overlays", ns, "entities.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(dict(sorted(out.items())), f, indent=2)
        f.write("\n")
    print("wrote %s: %d events" % (os.path.relpath(path, ROOT), len(out)))


def main():
    classes_by_root = {MC: load_classes(os.path.join(MC, "world/entity"))}
    if os.path.isdir(TF):
        classes_by_root[TF] = load_classes(TF)
    if os.path.isdir(AE):
        classes_by_root[AE] = load_classes(AE)

    reg = {}
    reg[MC] = registrations(os.path.join(MC, "world/entity/EntityTypes.java"),
                            r'register\(EntityTypeIds\.([A-Z0-9_]+),\s*EntityType\.Builder\.of\(\s*(\w+)::new')
    reg[TF] = registrations(os.path.join(TF, "twilightforest/init/TFEntities.java"),
                            r'register\w*\(\s*"([a-z0-9_]+)"[^;]*?Builder\.of\(\s*(\w+)::new')
    reg[AE] = registrations(os.path.join(AE, "com/aetherteam/aether/entity/AetherEntityTypes.java"),
                            r'register\(\s*"([a-z0-9_]+)"[^;]*?Builder\.of\(\s*(\w+)::new')

    engine = re.findall(r'\{ "([a-z0-9_]+)",', open(ENGINE_TYPES).read())

    rows, missing, all_unresolved = [], [], []
    for slug in engine:
        if slug in HUSH:
            src, vol, has_step, interval = HUSH[slug]
            base = "obeycraft:entity.%s." % slug
            step = ("Event", "None", base + "step", base + "step", 0.15, 1.0) if has_step else ("None", "None", "", "", 0.0, 0.0)
            rows.append((slug, src, vol, interval,
                         ("None", base + "ambient", base + "ambient"),
                         ("None", base + "hurt", base + "hurt"),
                         ("None", base + "death", base + "death"),
                         step,
                         "entity.generic.swim", "entity.generic.splash", "entity.generic.splash", [], "hush"))
            continue
        jc, root = None, None
        for r in (MC, TF, AE):
            name = reg.get(r, {}).get(SLUG_TO_MOD.get(slug, slug) if r != MC else slug)
            if name and r in classes_by_root and name in classes_by_root[r]:
                jc, root = classes_by_root[r][name], r
                break
        if jc is None:
            missing.append(slug)
            continue
        unresolved = []
        amb = resolve(classes_by_root, jc, "ambient", unresolved) or ("None", "", "")
        hurt = resolve(classes_by_root, jc, "hurt", unresolved) or ("None", VANILLA["GENERIC_HURT"], VANILLA["GENERIC_HURT"])
        death = resolve(classes_by_root, jc, "death", unresolved) or ("None", VANILLA["GENERIC_DEATH"], VANILLA["GENERIC_DEATH"])
        step = resolve(classes_by_root, jc, "step", unresolved) or ("Block", "None", "", "", 0.15, 1.0)
        swim = resolve(classes_by_root, jc, "swim", unresolved) or ("None", VANILLA["GENERIC_SWIM"], "")
        splash = resolve(classes_by_root, jc, "splash", unresolved) or ("None", VANILLA["GENERIC_SPLASH"], "")
        splash_hi = resolve(classes_by_root, jc, "splashHigh", unresolved) or ("None", VANILLA["GENERIC_SPLASH"], "")
        source = resolve(classes_by_root, jc, "source", unresolved) or "NEUTRAL"
        volume = resolve(classes_by_root, jc, "volume", unresolved)
        interval = resolve(classes_by_root, jc, "interval", unresolved) or 80
        # MC Entity.getMovementEmission: ALL unless overridden; only ALL and
        # SOUNDS play footsteps (EVENTS is the vibration half alone).
        emission = resolve(classes_by_root, jc, "emission", unresolved) or "ALL"
        if emission not in ("ALL", "SOUNDS"):
            step = ("None", "None", "", "", 0.0, 0.0)
        rows.append((slug, SOURCE_NAMES.get(source, "Neutral"), volume if volume is not None else 1.0, interval,
                     amb, hurt, death, step, swim[1], splash[1], splash_hi[1], unresolved,
                     {MC: "vanilla", TF: "twilightforest", AE: "aether"}[root] + " " + jc.name))
        all_unresolved += ["%s: %s" % (slug, u) for u in unresolved]

    lines = [
        "// GENERATED by tools/gen_entity_sounds.py — do not edit by hand.\n",
        "//\n",
        "// ENTITY_SOUNDS(slug, source, soundVolume, ambientInterval,\n",
        "//               ambientPred, ambientTrue, ambientFalse,\n",
        "//               hurtPred, hurtTrue, hurtFalse,\n",
        "//               deathPred, deathTrue, deathFalse,\n",
        "//               stepMode, stepPred, stepTrue, stepFalse, stepVolume, stepPitch,\n",
        "//               swim, splash, splashHighSpeed)\n",
        "// Pred: None | Baby | InWater | UnderWater | OnGround (True/False are the\n",
        "// ternary's two arms; equal when None). stepMode: Block = Entity's default\n",
        "// (the block's SoundType), Event = the named event, None = silent feet.\n",
        "// \"\" = no sound (MC null). Rows marked `unresolved` have an override the\n",
        "// generator could not read; the engine class hand-codes it.\n",
    ]
    for (slug, src, vol, interval, amb, hurt, death, step, swim, splash, splash_hi, unresolved, origin) in rows:
        note = "  // %s" % origin
        if unresolved:
            note += "; unresolved: " + ", ".join(unresolved)
        lines.append("ENTITY_SOUNDS(%s, %s, %sf, %d,%s\n    %s, %s, %s,\n    %s, %s, %s,\n    %s, %s, %s,\n    %s, %s, %s, %s, %sf, %sf,\n    %s, %s, %s)\n" % (
            cstr(slug), src, repr(float(vol)), interval, note,
            amb[0], cstr(amb[1]), cstr(amb[2]),
            hurt[0], cstr(hurt[1]), cstr(hurt[2]),
            death[0], cstr(death[1]), cstr(death[2]),
            step[0], step[1], cstr(step[2]), cstr(step[3]), repr(float(step[4])), repr(float(step[5])),
            cstr(swim), cstr(splash), cstr(splash_hi)))
    with open(OUT, "w") as f:
        f.write("".join(lines))
    print("wrote %s: %d rows" % (os.path.relpath(OUT, ROOT), len(rows)))

    text = "".join(lines) + "\n".join(EXTRA_MOD_EVENTS)
    for ns in ("twilightforest", "aether"):
        events = set(re.findall(r'"(%s:[^"]+)"' % ns, text))
        if events:
            write_mod_overlay(ns, events)
    if missing:
        print("no class found for: " + " ".join(missing))
    if all_unresolved:
        print("unresolved overrides (hand-coded on the engine classes):")
        for u in all_unresolved:
            print("  " + u)


if __name__ == "__main__":
    main()
