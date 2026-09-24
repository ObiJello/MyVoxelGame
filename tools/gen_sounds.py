#!/usr/bin/env python3
"""Generate the sound-event registry and the block sound-type tables from MC's source.

Three files come out of here, all under src/common/sound/:

  GeneratedSoundEvents.inc      SOUND_EVENT(CONSTANT, "id") for every event in
                                MC's SoundEvents registry, in registration
                                order: the `register("...")` /
                                `registerForHolder("...")` literals of
                                sounds/SoundEvents.java, plus the ids its
                                helper methods build at class-init time (the
                                goat horn's eight, and the wolf / chicken /
                                cow / pig / cat sound-variant sets). The
                                position in this list is the event's WIRE id
                                (ClientboundSoundPacket's registry holder), so
                                client and server — one binary — agree by
                                construction.
  GeneratedSoundTypes.inc       SOUND_TYPE(NAME, volume, pitch, break, step,
                                place, hit, fall) — every `new SoundType(...)`
                                of world/level/block/SoundType.java, with the
                                SoundEvents constants resolved to their ids.
  GeneratedBlockSetTypes.inc    BLOCK_SET_TYPE(...) / WOOD_TYPE(...) — MC
                                BlockSetType and WoodType: the door,
                                trapdoor, pressure-plate, button and fence-gate
                                sounds, and the sound type their blocks get.
  GeneratedBlockSoundTypes.inc  BLOCK_SOUND_TYPE("slug", NAME, SET, WOOD) — MC
                                Properties.sound(...) per registered block,
                                walked out of world/level/block/Blocks.java
                                the way tools/gen_block_hardness.py walks it
                                (ofFullCopy / ofLegacyCopy copy the sound type;
                                the stair / slab / wall helpers are their base
                                block), then every engine-only slug in
                                gen_block_hardness.SLUG_ALIASES under the sound
                                type of the vanilla block it is modelled on.

The runtime resolves a block's sound type by its REGISTRY SLUG
(Game::SoundTypeOf), so a block added to BlockDefs.inc later picks its row up
by name — or falls back to SoundType::STONE when this table has never heard of
it, which is MC's own Properties default only in spirit (MC's field default is
STONE too: BlockBehaviour.Properties.soundType = SoundType.STONE).

    python3 tools/gen_sounds.py
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_block_hardness as hard  # noqa: E402  (reuses its Java walkers + SLUG_ALIASES)

MC2          = "minecraft_code_26.3-pre-2/decompiled_net/minecraft"
SOUND_EVENTS = os.path.join(MC2, "sounds/SoundEvents.java")
SOUND_TYPE   = os.path.join(MC2, "world/level/block/SoundType.java")
BLOCKS       = os.path.join(MC2, "world/level/block/Blocks.java")
BLOCKS_OLD   = "minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/world/level/block/Blocks.java"
BLOCK_SET_TYPE = os.path.join(MC2, "world/level/block/state/properties/BlockSetType.java")
WOOD_TYPE      = os.path.join(MC2, "world/level/block/state/properties/WoodType.java")
VARIANTS = [
    # (enum file, the helper's id templates) — SoundEvents.register*SoundVariants.
    (os.path.join(MC2, "world/entity/animal/wolf/WolfSoundVariants.java"),
     ["ambient", "death", "growl", "hurt", "pant", "whine"]),
    (os.path.join(MC2, "world/entity/animal/chicken/ChickenSoundVariants.java"),
     ["ambient", "hurt", "death"]),
    (os.path.join(MC2, "world/entity/animal/cow/CowSoundVariants.java"),
     ["ambient", "hurt", "death", "step"]),
    (os.path.join(MC2, "world/entity/animal/pig/PigSoundVariants.java"),
     ["ambient", "hurt", "death", "eat"]),
    (os.path.join(MC2, "world/entity/animal/feline/CatSoundVariants.java"),
     ["ambient", "stray_ambient", "hiss", "hurt", "death", "eat", "beg_for_food",
      "purr", "purreow"]),
]
OUT_DIR = "src/common/sound"


# ── SoundEvents ───────────────────────────────────────────────────────────────

FIELD_RE = re.compile(
    r'public static final (?:SoundEvent|Holder(?:\.Reference)?<SoundEvent>)\s+'
    r'([A-Z0-9_]+)\s*=\s*register(?:ForHolder)?\("([a-z0-9_./-]+)"\)\s*;')
HELPER_FIELD_RE = re.compile(
    r'public static final [^=;]+?\s+([A-Z0-9_]+)\s*=\s*(register\w+SoundVariants|registerGoatHornSoundVariants)\(\)\s*;')


def variant_ids(path, templates):
    src = open(path).read()
    body = re.search(r'enum SoundSet\s*\{(.*?);', src, re.S)
    if not body:
        raise SystemExit("SoundSet enum not found in " + path)
    ids = []
    for _name, _ident, event in re.findall(r'([A-Z_]+)\("([a-z_]+)",\s*"([a-z_]+)"\)', body.group(1)):
        for t in templates:
            ids.append("entity.%s.%s" % (event, t))
    return ids


def parse_sound_events():
    """[(CONSTANT, id)] in registration order, and CONSTANT -> id."""
    src = open(SOUND_EVENTS).read()
    helpers = {
        "registerGoatHornSoundVariants": [("", ["item.goat_horn.sound.%d" % i for i in range(8)])],
    }
    for path, templates in VARIANTS:
        stem = os.path.basename(path).replace("SoundVariants.java", "")
        helpers["register%sSoundVariants" % stem] = [("", variant_ids(path, templates))]

    events, by_const, seen = [], {}, set()
    # Walk the field declarations in source order; a helper field expands to
    # its ids at that point, which is when MC's static init registers them.
    pos = 0
    tokens = []
    for m in FIELD_RE.finditer(src):
        tokens.append((m.start(), "field", m.group(1), m.group(2)))
    for m in HELPER_FIELD_RE.finditer(src):
        tokens.append((m.start(), "helper", m.group(1), m.group(2)))
    tokens.sort()
    for _pos, kind, const, arg in tokens:
        if kind == "field":
            if arg in seen:
                continue
            seen.add(arg)
            events.append((const, arg))
            by_const[const] = arg
        else:
            for _prefix, ids in helpers.get(arg, []):
                for event_id in ids:
                    if event_id in seen:
                        continue
                    seen.add(event_id)
                    events.append((const_name(event_id), event_id))
    return events, by_const


def const_name(event_id):
    return re.sub(r'[^A-Z0-9]', '_', event_id.upper())


# ── SoundType ─────────────────────────────────────────────────────────────────

def parse_sound_types(by_const):
    src = open(SOUND_TYPE).read()
    out = []
    for name, args in re.findall(r'\n\s+([A-Z0-9_]+) = new SoundType\(([^;]*)\);', src):
        parts = [a.strip() for a in args.split(",")]
        if len(parts) != 7:
            raise SystemExit("unexpected SoundType arity for " + name)
        vol = hard.parse_float(parts[0])
        pitch = hard.parse_float(parts[1])
        events = []
        for p in parts[2:]:
            m = re.fullmatch(r'SoundEvents\.([A-Z0-9_]+)', p)
            if not m:
                raise SystemExit("SoundType %s: cannot resolve %s" % (name, p))
            const = m.group(1)
            events.append("" if const == "EMPTY" else by_const[const])
        out.append((name, vol, pitch, events))
    return out


# ── Blocks.java: Properties.sound(...) per slug ────────────────────────────────

class Snd:
    """The Properties column this generator consumes, plus the BlockSetType /
    WoodType a block's class was constructed with (doors, trapdoors, buttons,
    pressure plates, fence gates and signs take their sounds from those)."""
    __slots__ = ("sound", "set_type", "wood_type")

    def __init__(self, sound="STONE"):
        self.sound = sound            # BlockBehaviour.Properties.soundType default
        self.set_type = None
        self.wood_type = None

    def copy(self):
        c = Snd(self.sound)
        c.set_type = self.set_type
        c.wood_type = self.wood_type
        return c


def sound_of_arg(arg, ctx):
    arg = arg.strip()
    m = re.fullmatch(r'SoundType\.([A-Z0-9_]+)', arg)
    if m:
        return m.group(1)
    return ctx.get(arg)                 # a helper parameter (`soundType`)


# The local Properties factories of Blocks.java that set a sound, transcribed
# from their bodies (they are one-liners that do not move).
def helper_sound(name, raw_args):
    if name == "logProperties":        # (topColor, sideColor, soundType)
        return Snd(sound_of_arg(raw_args[2], {}) or "WOOD") if len(raw_args) >= 3 else Snd("WOOD")
    if name == "netherStemProperties":
        return Snd("STEM")
    if name == "leavesProperties":     # (soundType)
        return Snd(sound_of_arg(raw_args[0], {}) or "GRASS") if raw_args else Snd("GRASS")
    if name == "shulkerBoxProperties":
        return Snd("STONE")
    if name == "pistonProperties":
        return Snd("STONE")
    if name == "buttonProperties":
        return Snd("STONE")
    if name == "flowerPotProperties":
        return Snd("STONE")
    if name == "candleProperties":
        return Snd("CANDLE")
    if name == "wallVariant":
        return Snd("STONE")
    return None


def eval_sound(expr, fields, where):
    expr = expr.strip()
    m = re.match(r'BlockBehaviour\.Properties\.(of|ofFullCopy|ofLegacyCopy)\s*\(', expr)
    if m:
        open_idx = m.end() - 1
        close = hard.balanced(expr, open_idx)
        inner = expr[open_idx + 1:close - 1].strip()
        if m.group(1) == "of":
            p = Snd()
        else:
            base = fields.get(inner)
            if base is None:
                raise SystemExit("%s: copy of unresolved field %s" % (where, inner))
            p = base.copy()
        rest = expr[close:]
    else:
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', expr)
        if not m:
            return None
        open_idx = m.end() - 1
        close = hard.balanced(expr, open_idx)
        p = helper_sound(m.group(1), hard.split_args(expr[open_idx + 1:close - 1]))
        if p is None:
            return None
        rest = expr[close:]
    for name, args in hard.chain_calls(rest):
        if name == "sound":
            s = sound_of_arg(args, {})
            if s:
                p.sound = s
    return p


# Block classes whose constructor overrides the Properties sound from the
# BlockSetType / WoodType they are given (ButtonBlock:46, DoorBlock:51,
# BasePressurePlateBlock:31, TrapDoorBlock:49, FenceGateBlock:53, the sign
# classes). Hanging signs use WoodType.hangingSignSoundType().
SET_TYPE_CLASSES = ("ButtonBlock", "DoorBlock", "PressurePlateBlock", "WeightedPressurePlateBlock",
                    "TrapDoorBlock", "WeatheringCopperDoorBlock", "WeatheringCopperTrapDoorBlock")
WOOD_TYPE_CLASSES = ("FenceGateBlock", "StandingSignBlock", "WallSignBlock")
HANGING_SIGN_CLASSES = ("WallHangingSignBlock", "CeilingHangingSignBlock")


def apply_factory(p, factory_text, set_types, wood_types):
    m = re.search(r'new\s+([A-Za-z]+)\((?:[^()]*?,\s*)?(BlockSetType|WoodType)\.([A-Z_]+)', factory_text)
    if not m:
        return
    cls, kind, name = m.group(1), m.group(2), m.group(3)
    if kind == "BlockSetType" and cls in SET_TYPE_CLASSES:
        p.set_type = name
        p.sound = set_types[name]["sound"]
    elif kind == "WoodType" and cls in WOOD_TYPE_CLASSES + HANGING_SIGN_CLASSES:
        p.wood_type = name
        w = wood_types[name]
        p.sound = w["hanging"] if cls in HANGING_SIGN_CLASSES else w["sound"]
        if cls == "FenceGateBlock":
            p.set_type = w["set"]


def wrapper_sound(fn, args, fields, where):
    if fn == "registerBed":
        return Snd("WOOD")               # BedBlock: .sound(SoundType.WOOD)
    if fn == "registerStainedGlass":
        return Snd("GLASS")
    if fn in ("registerStair", "registerLegacyStair", "registerSlab", "registerWall"):
        base = fields.get(args[1].strip())
        if base is None:
            raise SystemExit("%s: base %s unresolved" % (where, args[1]))
        return base.copy()
    return None


COLLECTION_RE = re.compile(r'\bWeatheringCopperCollection\.registerBlocks\s*\(')


def parse_block_sounds(path, set_types, wood_types):
    src = open(path).read()
    fields, by_slug = {}, {}
    # hard.registrations: the written-out register* calls plus 26.3's
    # ColorCollection families expanded per colour, in source order.
    for _offset, fn, args, field in hard.registrations(src):
        slug = hard.slug_of(args[0])
        if slug is None:
            continue
        where = '%s("%s")' % (fn, slug)
        if fn == "register":
            p = eval_sound(args[-1], fields, where)
            if p is None:
                print("  ! %s: unrecognised Properties, defaulting to STONE" % where, file=sys.stderr)
                p = Snd()
            if len(args) >= 3:
                apply_factory(p, args[1], set_types, wood_types)
        else:
            p = wrapper_sound(fn, args, fields, where)
            if p is None:
                continue
        if field:
            fields[field] = p
        by_slug[slug] = p

    # WeatheringCopperBlocks.create(...): eight registrations off one supplier.
    for m in hard.WEATHERING_RE.finditer(src):
        open_idx = m.end() - 1
        close = hard.balanced(src, open_idx)
        args = hard.split_args(src[open_idx + 1:close - 1])
        if len(args) < 2:
            continue
        base = hard.slug_of(args[0])
        if base is None:
            continue
        supplier = args[-1]
        arrow = supplier.find("->")
        body = supplier[arrow + 2:].strip() if arrow >= 0 else supplier
        p = eval_sound(body, fields, 'WeatheringCopperBlocks.create("%s")' % base)
        if p is None:
            continue
        for factory in args[2:-1]:
            apply_factory(p, factory, set_types, wood_types)
        for prefix in hard.WEATHERING_PREFIXES:
            by_slug[prefix + base] = p.copy()

    # 26.3: WeatheringCopperCollection.registerBlocks(ids, register, waxed,
    # weathering, (state) -> Properties). The eight ids are the base with the
    # weathering prefixes (COPPER_BLOCK's irregular names come from the 26.1
    # gap fill); the collection's field stands for all eight in later copies.
    for m in COLLECTION_RE.finditer(src):
        open_idx = m.end() - 1
        close = hard.balanced(src, open_idx)
        args = hard.split_args(src[open_idx + 1:close - 1])
        if len(args) < 5:
            continue
        base = hard.slug_of(args[0])
        if base is None:
            continue
        supplier = args[-1]
        arrow = supplier.find("->")
        body = supplier[arrow + 2:].strip() if arrow >= 0 else supplier
        body = re.sub(r'\(\(BlockBehaviour\)\s*([A-Z_0-9]+)\.(?:weathering|waxed)\(\)\.pick\(state\)\)',
                      r'(\1)', body)
        p = eval_sound(body, fields, 'WeatheringCopperCollection("%s")' % base)
        if p is None:
            continue
        for factory in args[2:4]:
            apply_factory(p, factory, set_types, wood_types)
        head = src.rfind("\n", 0, m.start())
        fm = re.search(r'([A-Z][A-Z_0-9]*)\s*=\s*$', src[head + 1:m.start()])
        if fm:
            fields[fm.group(1)] = p
        for prefix in hard.WEATHERING_PREFIXES:
            by_slug.setdefault(prefix + base, p.copy())
    return by_slug


# ── BlockSetType / WoodType ───────────────────────────────────────────────────

def parse_set_types(by_const):
    src = open(BLOCK_SET_TYPE).read()
    wood = [by_const[c] for c in ("WOODEN_DOOR_CLOSE", "WOODEN_DOOR_OPEN", "WOODEN_TRAPDOOR_CLOSE",
                                  "WOODEN_TRAPDOOR_OPEN", "WOODEN_PRESSURE_PLATE_CLICK_OFF",
                                  "WOODEN_PRESSURE_PLATE_CLICK_ON", "WOODEN_BUTTON_CLICK_OFF",
                                  "WOODEN_BUTTON_CLICK_ON")]
    out = {}
    order = []
    for name, args in re.findall(r'\n\s+([A-Z_]+) = register\(new BlockSetType\(([^;]*)\)\);', src):
        parts = [a.strip() for a in args.split(",")]
        if len(parts) == 1:
            out[name] = {"sound": "WOOD", "events": list(wood)}
        else:
            snd = re.fullmatch(r'SoundType\.([A-Z_0-9]+)', parts[5]).group(1)
            events = [by_const[re.fullmatch(r'SoundEvents\.([A-Z_0-9]+)', a).group(1)] for a in parts[6:14]]
            out[name] = {"sound": snd, "events": events}
        order.append(name)
    return out, order


def parse_wood_types(by_const):
    src = open(WOOD_TYPE).read()
    out, order = {}, []
    for name, args in re.findall(r'\n\s+([A-Z_]+) = register\(new WoodType\(([^;]*)\)\);', src):
        parts = [a.strip() for a in args.split(",")]
        set_type = re.fullmatch(r'BlockSetType\.([A-Z_]+)', parts[1]).group(1)
        if len(parts) == 2:
            out[name] = {"set": set_type, "sound": "WOOD", "hanging": "HANGING_SIGN",
                         "gate": [by_const["FENCE_GATE_CLOSE"], by_const["FENCE_GATE_OPEN"]]}
        else:
            out[name] = {"set": set_type,
                         "sound": re.fullmatch(r'SoundType\.([A-Z_0-9]+)', parts[2]).group(1),
                         "hanging": re.fullmatch(r'SoundType\.([A-Z_0-9]+)', parts[3]).group(1),
                         "gate": [by_const[re.fullmatch(r'SoundEvents\.([A-Z_0-9]+)', a).group(1)]
                                  for a in parts[4:6]]}
        order.append(name)
    return out, order


# ── Output ────────────────────────────────────────────────────────────────────

HEADER = """// GENERATED by tools/gen_sounds.py from minecraft_code_26.3-pre-2 — do not edit by hand.
// Re-run the generator after a decompile update or a new SLUG_ALIASES entry.
"""


def write(path, body):
    with open(path, "w") as f:
        f.write(HEADER)
        f.write(body)
    print("wrote " + path)


def cstr(s):
    return '"' + s + '"'


def main():
    for p in (SOUND_EVENTS, SOUND_TYPE, BLOCKS, BLOCK_SET_TYPE, WOOD_TYPE):
        if not os.path.exists(p):
            raise SystemExit("missing input: " + p)

    events, by_const = parse_sound_events()
    lines = ["// SOUND_EVENT(CONSTANT, \"id\") — MC SoundEvents, registration order (= wire id).\n"]
    for const, event_id in events:
        lines.append("SOUND_EVENT(%s, %s)\n" % (const, cstr(event_id)))
    write(os.path.join(OUT_DIR, "GeneratedSoundEvents.inc"), "".join(lines))
    print("  %d sound events" % len(events))

    types = parse_sound_types(by_const)
    type_names = {t[0] for t in types}
    lines = ["// SOUND_TYPE(NAME, volume, pitch, break, step, place, hit, fall) — MC SoundType.\n"
             "// \"\" is SoundEvents.EMPTY.\n"]
    for name, vol, pitch, ev in types:
        lines.append("SOUND_TYPE(%s, %sf, %sf, %s)\n" % (
            name, repr(vol), repr(pitch), ", ".join(cstr(e) for e in ev)))
    write(os.path.join(OUT_DIR, "GeneratedSoundTypes.inc"), "".join(lines))
    print("  %d sound types" % len(types))

    set_types, set_order = parse_set_types(by_const)
    wood_types, wood_order = parse_wood_types(by_const)
    lines = ["// BLOCK_SET_TYPE(NAME, soundType, doorClose, doorOpen, trapdoorClose, trapdoorOpen,\n"
             "//                pressurePlateClickOff, pressurePlateClickOn, buttonClickOff, buttonClickOn)\n"
             "// — MC BlockSetType.\n"]
    for name in set_order:
        t = set_types[name]
        lines.append("BLOCK_SET_TYPE(%s, %s, %s)\n" % (name, t["sound"], ", ".join(cstr(e) for e in t["events"])))
    lines.append("// WOOD_TYPE(NAME, setType, soundType, hangingSignSoundType, fenceGateClose, fenceGateOpen)\n"
                 "// — MC WoodType.\n")
    for name in wood_order:
        w = wood_types[name]
        lines.append("WOOD_TYPE(%s, %s, %s, %s, %s)\n" % (name, w["set"], w["sound"], w["hanging"],
                                                          ", ".join(cstr(e) for e in w["gate"])))
    write(os.path.join(OUT_DIR, "GeneratedBlockSetTypes.inc"), "".join(lines))
    print("  %d block set types, %d wood types" % (len(set_order), len(wood_order)))

    by_slug = parse_block_sounds(BLOCKS, set_types, wood_types)
    if os.path.exists(BLOCKS_OLD):
        # 26.1 blocks 26.3 renamed or dropped — gap fill only.
        for slug, p in parse_block_sounds(BLOCKS_OLD, set_types, wood_types).items():
            by_slug.setdefault(slug, p)
    for slug, p in by_slug.items():
        if p.sound not in type_names:
            raise SystemExit("%s: unknown SoundType %s" % (slug, p.sound))

    rows = dict(by_slug)
    aliased = 0
    for engine_slug, vanilla in hard.SLUG_ALIASES.items():
        if engine_slug in rows:
            continue
        if vanilla in rows:
            rows[engine_slug] = rows[vanilla].copy()
            aliased += 1
        else:
            print("  ! alias %s -> %s: vanilla block has no sound row" % (engine_slug, vanilla),
                  file=sys.stderr)
    lines = ["// BLOCK_SOUND_TYPE(\"slug\", SOUND_TYPE, SET_TYPE(x) | NO_SET_TYPE, WOOD_TYPE_OF(x) | NO_WOOD_TYPE)\n"
             "// — MC Properties.sound per block (after the block class's constructor\n"
             "// override), and the BlockSetType / WoodType it was built with. Engine blocks\n"
             "// carry the row of the vanilla block they are modelled on.\n"]
    for slug in sorted(rows):
        p = rows[slug]
        lines.append("BLOCK_SOUND_TYPE(%s, %s, %s, %s)\n" % (
            cstr(slug), p.sound,
            ("SET_TYPE(%s)" % p.set_type) if p.set_type else "NO_SET_TYPE",
            ("WOOD_TYPE_OF(%s)" % p.wood_type) if p.wood_type else "NO_WOOD_TYPE"))
    write(os.path.join(OUT_DIR, "GeneratedBlockSoundTypes.inc"), "".join(lines))
    print("  %d block rows (%d engine aliases)" % (len(rows), aliased))


if __name__ == "__main__":
    main()
