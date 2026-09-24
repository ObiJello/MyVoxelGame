#!/usr/bin/env python3
"""Generate src/common/world/block/GeneratedBlockHardness.{hpp,cpp} from MC's Blocks.java.

Replaces the name-string heuristic that used to live in BlockRegistry.cpp
(`ClassifyByName` + `ApplyExplicitHardnessOverrides`). That heuristic guessed a
block's hardness from substrings of its model name — "anything with 'stone' in
it is 1.5" — which was right often enough to look fine and wrong often enough to
matter, and it had no answer at all for blast resistance, which TNT needs.

Five columns come out of here:

  destroyTime          MC BlockBehaviour.Properties.destroyTime. Seconds at the
                       player's base mining speed; -1 is unbreakable.
  explosionResistance  MC's `strength(destroyTime, explosionResistance)` SECOND
                       argument. Note `strength(a)` sets BOTH to a, so for most
                       of the registry the two are not independent.
  requiresCorrectTool  MC requiresCorrectToolForDrops().
  mapColor             The block's map colour as 0xRRGGBB. Needed because
                       ConcretePowderBlock and AnvilBlock derive their falling
                       dust colour from it (`getDustColor` ->
                       `state.getMapColor(...).col`).
  ignitedByLava        MC ignitedByLava(). Not read yet; it is the data half of
                       fire spread, and it is free to carry now.

Tool and tier are NOT in Blocks.java — they live in block tags — so those come
from data/minecraft/tags/block/{mineable/*,needs_*_tool}.json.

    python3 tools/gen_block_hardness.py
"""

import json
import os
import re
import sys

MC       = "minecraft_code_26.1-snapshot-1/decompiled_net/minecraft"
BLOCKS   = os.path.join(MC, "world/level/block/Blocks.java")
# 26.3's Blocks.java (minecraft_code_26.3-pre-2): parsed the same way, keyed on
# `BlockItemIds.X` / `BlockIds.X`, and merged setdefault-style so only the
# blocks 26.1 lacks (poplar, cinnabar, sulfur, …) come from it.
BLOCKS2  = "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/block/Blocks.java"
REFKEYS  = os.path.join(MC, "references/Blocks.java")
MAPCOLOR = os.path.join(MC, "world/level/material/MapColor.java")
DYECOLOR = os.path.join(MC, "world/item/DyeColor.java")
TAGS     = "data/minecraft/tags/block"
DEFS     = "src/common/world/block/BlockDefs.inc"
OUT_HPP  = "src/common/world/block/GeneratedBlockHardness.hpp"
OUT_CPP  = "src/common/world/block/GeneratedBlockHardness.cpp"


# ── Properties, reduced to the columns we consume ─────────────────────────────

class Props:
    __slots__ = ("destroy_time", "resistance", "requires_tool", "map_color",
                 "ignited_by_lava", "push_reaction", "redstone_conductor",
                 "instrument")

    def __init__(self):
        # MC's field initialisers: both strengths are 0.0F, not 1.0. Getting
        # this wrong makes every decorative block instant-break.
        self.destroy_time = 0.0
        self.resistance = 0.0
        self.requires_tool = False
        self.map_color = "NONE"
        self.ignited_by_lava = False
        # MC Properties.pushReaction — PUSH_PULL unless the block opts out
        # (BlockBehaviour.Properties constructor). Stored under 26.3's names.
        self.push_reaction = "PushPull"
        # MC Properties.isRedstoneConductor — the default predicate is
        # isCollisionShapeFullBlock; Blocks.java overrides it with
        # Blocks::never (glass, leaves, pistons, observers…) or Blocks::always.
        self.redstone_conductor = "Default"
        # MC Properties.instrument — what a note block on top of this plays.
        # HARP is the field default; Blocks.java sets BASEDRUM on stone,
        # BASS on wood, and so on. Stored as the enum's serialized name.
        self.instrument = "harp"

    def copy(self):
        p = Props()
        p.destroy_time = self.destroy_time
        p.resistance = self.resistance
        p.requires_tool = self.requires_tool
        p.map_color = self.map_color
        p.ignited_by_lava = self.ignited_by_lava
        p.push_reaction = self.push_reaction
        p.redstone_conductor = self.redstone_conductor
        p.instrument = self.instrument
        return p


# PushReaction was renamed wholesale in 26.3 (NORMAL→PUSH_PULL,
# PUSH_ONLY→PUSH, DESTROY→POPPED, BLOCK→IMMOVEABLE, IGNORE→IGNORE_ENTITY);
# both spellings map onto the engine's enum so either decompile parses.
PUSH_REACTIONS = {
    "NORMAL": "PushPull",   "PUSH_PULL": "PushPull",
    "PUSH_ONLY": "Push",    "PUSH": "Push",
    "DESTROY": "Popped",    "POPPED": "Popped",
    "BLOCK": "Immoveable",  "IMMOVEABLE": "Immoveable",
    "IGNORE": "IgnoreEntity", "IGNORE_ENTITY": "IgnoreEntity",
}


def push_reaction(arg):
    m = re.search(r'PushReaction\.([A-Z_]+)', arg)
    if not m or m.group(1) not in PUSH_REACTIONS:
        raise SystemExit("unknown PushReaction in: " + arg)
    return PUSH_REACTIONS[m.group(1)]


def redstone_conductor(arg):
    if "never" in arg:
        return "Never"
    if "always" in arg:
        return "Always"
    return "Default"


def strength(p, dt, res=None):
    # BlockBehaviour.Properties.strength(a)      -> strength(a, a)
    # BlockBehaviour.Properties.strength(a, b)   -> destroyTime(a).explosionResistance(b)
    # explosionResistance clamps at 0; destroyTime does NOT (bedrock is -1).
    p.destroy_time = dt
    p.resistance = max(0.0, dt if res is None else res)


# ── Java source helpers ───────────────────────────────────────────────────────

def balanced(text, open_idx):
    """Index just past the ')' that closes the '(' at open_idx."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':                      # skip string literals
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced parentheses")


def split_args(arg_text):
    """Top-level comma split of an argument list (without the outer parens)."""
    out, depth, start = [], 0, 0
    i = 0
    n = len(arg_text)
    while i < n:
        c = arg_text[i]
        if c == '"':
            i += 1
            while i < n and arg_text[i] != '"':
                i += 2 if arg_text[i] == '\\' else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(arg_text[start:i].strip())
            start = i + 1
        i += 1
    tail = arg_text[start:].strip()
    if tail:
        out.append(tail)
    return out


def chain_calls(expr):
    """Yield (name, args_text) for each top-level `.name(...)` in `expr`.

    Depth-aware so `.mapColor((Function)((state) -> ...))` is one call and the
    lambda inside it is not mistaken for more chain links.
    """
    i, n, depth = 0, len(expr), 0
    while i < n:
        c = expr[i]
        if c == '"':
            i += 1
            while i < n and expr[i] != '"':
                i += 2 if expr[i] == '\\' else 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == '.' and depth == 0:
            m = re.match(r'\.([A-Za-z_][A-Za-z0-9_]*)\s*\(', expr[i:])
            if m:
                open_idx = i + m.end() - 1
                close = balanced(expr, open_idx)
                yield m.group(1), expr[open_idx + 1:close - 1]
                i = close
                continue
        i += 1


def parse_float(tok):
    tok = tok.strip().rstrip("Ff")
    # Handles "(double)0.5F" style casts the decompiler sometimes emits.
    tok = re.sub(r'^\([a-z]+\)', '', tok).strip()
    return float(tok)


# ── Colour tables ─────────────────────────────────────────────────────────────

def load_map_colors():
    src = open(MAPCOLOR).read()
    out = {}
    for name, _id, rgb in re.findall(
            r'public static final MapColor ([A-Z_0-9]+) = new MapColor\((\d+),\s*(\d+)\)', src):
        out[name] = int(rgb) & 0xFFFFFF
    if "NONE" not in out:
        raise SystemExit("MapColor.NONE missing — MapColor.java layout changed")
    return out


def load_dye_to_map_color():
    src = open(DYECOLOR).read()
    out = {}
    for name, mc in re.findall(
            r'^\s+([A-Z_]+)\(\d+,\s*"[a-z_]+",\s*\d+,\s*MapColor\.([A-Z_0-9]+)', src, re.M):
        out[name] = mc
    return out


def load_reference_keys():
    """net.minecraft.references.Blocks — the ResourceKey constants a handful of
    registrations use instead of a string literal (the stem/melon family, whose
    keys have to exist before the blocks that reference each other)."""
    if not os.path.exists(REFKEYS):
        return {}
    src = open(REFKEYS).read()
    return dict(re.findall(
        r'ResourceKey<Block>\s+([A-Z_0-9]+)\s*=\s*createKey\("([a-z0-9_]+)"\)', src))


MAP_COLORS = load_map_colors()
DYE_TO_MAP = load_dye_to_map_color()
REF_KEYS   = load_reference_keys()


def resolve_map_color(arg):
    """The argument of .mapColor(...) -> a MapColor NAME, or None if dynamic."""
    arg = arg.strip()
    m = re.fullmatch(r'MapColor\.([A-Z_0-9]+)', arg)
    if m:
        return m.group(1)
    m = re.fullmatch(r'DyeColor\.([A-Z_]+)', arg)
    if m:
        return DYE_TO_MAP.get(m.group(1))
    # A bare parameter name (`color`, `mapColor`) inside a helper, or a
    # state-dependent lambda. Both are resolved by the caller or, for the
    # lambda, approximated by the first MapColor mentioned inside it — which is
    # what a log's top face and a bed's foot end use.
    m = re.search(r'MapColor\.([A-Z_0-9]+)', arg)
    if m:
        return m.group(1)
    m = re.search(r'([a-z][A-Za-z]*)\.getMapColor\(\)', arg)
    if m:
        return "@param:" + m.group(1)
    if re.fullmatch(r'[a-z][A-Za-z]*', arg):
        return "@param:" + arg
    return None


# ── Properties-expression evaluation ──────────────────────────────────────────

# The nine local factories in Blocks.java that build a Properties of their own.
# Transcribed from their bodies rather than parsed, because each is a one-liner
# that will not move and a parser for them would be more code than the table.
def helper_props(name, args, fields):
    p = Props()
    if name == "logProperties":            # (topColor, sideColor, soundType)
        strength(p, 2.0)
        p.ignited_by_lava = True
        p.map_color = args[0] if args else "NONE"
        p.instrument = "bass"
    elif name == "netherStemProperties":   # (mapColor)
        strength(p, 2.0)
        p.map_color = args[0] if args else "NONE"
        p.instrument = "bass"
    elif name == "leavesProperties":       # (soundType)
        strength(p, 0.2)
        p.ignited_by_lava = True
        p.map_color = "PLANT"
        p.push_reaction = "Popped"
        p.redstone_conductor = "Never"
    elif name == "shulkerBoxProperties":   # (mapColor)
        strength(p, 2.0)
        p.map_color = args[0] if args else "NONE"
        p.push_reaction = "Popped"
    elif name == "pistonProperties":
        strength(p, 1.5)
        p.map_color = "STONE"
        p.push_reaction = "Immoveable"
        p.redstone_conductor = "Never"
    elif name == "buttonProperties":
        strength(p, 0.5)
        p.push_reaction = "Popped"
    elif name == "flowerPotProperties":
        strength(p, 0.0)                   # instabreak()
        p.push_reaction = "Popped"
    elif name == "candleProperties":       # (color)
        strength(p, 0.1)
        p.map_color = args[0] if args else "NONE"
        p.push_reaction = "Popped"
    elif name == "wallVariant":
        # Returns Properties.of() with only a loot-table override — every wall
        # chains its own strength on top, so the base is the plain default.
        pass
    else:
        return None
    return p


def eval_props(expr, fields, ctx=None, where=""):
    """Evaluate a Properties expression to a Props. `ctx` maps helper parameter
    names (`color`, `mapColor`, `topColor`) to MapColor names."""
    expr = expr.strip()
    ctx = ctx or {}

    # 1. Find the base.
    p = None
    m = re.match(r'BlockBehaviour\.Properties\.(of|ofFullCopy|ofLegacyCopy)\s*\(', expr)
    if m:
        kind = m.group(1)
        open_idx = m.end() - 1
        close = balanced(expr, open_idx)
        inner = expr[open_idx + 1:close - 1].strip()
        if kind == "of":
            p = Props()
        else:
            # Both copy destroyTime, explosionResistance, mapColor,
            # ignitedByLava and requiresCorrectToolForDrops (ofFullCopy is
            # ofLegacyCopy plus fields we do not consume), so one path serves.
            base = fields.get(inner)
            if base is None:
                raise SystemExit(
                    "%s: %s(%s) references an unresolved block field. "
                    "Blocks.java must be walked in source order." % (where, kind, inner))
            p = base.copy()
        rest = expr[close:]
    else:
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', expr)
        if not m:
            return None
        open_idx = m.end() - 1
        close = balanced(expr, open_idx)
        raw_args = split_args(expr[open_idx + 1:close - 1])
        args = []
        for a in raw_args:
            c = resolve_map_color(a)
            args.append(c if c and not c.startswith("@param:") else
                        ctx.get(c[7:], "NONE") if c else "NONE")
        p = helper_props(m.group(1), args, fields)
        if p is None:
            return None
        rest = expr[close:]

    # 2. Apply the chain, in order. LAST WINS — bamboo chains
    #    `.instabreak().strength(1.0F)` and the later call is the real one.
    for name, args in chain_calls(rest):
        if name == "strength":
            parts = split_args(args)
            if len(parts) == 1:
                strength(p, parse_float(parts[0]))
            else:
                strength(p, parse_float(parts[0]), parse_float(parts[1]))
        elif name == "instabreak":
            strength(p, 0.0)
        elif name == "destroyTime":
            p.destroy_time = parse_float(split_args(args)[0])
        elif name == "explosionResistance":
            p.resistance = max(0.0, parse_float(split_args(args)[0]))
        elif name == "requiresCorrectToolForDrops":
            p.requires_tool = True
        elif name == "ignitedByLava":
            p.ignited_by_lava = True
        elif name == "pushReaction":
            p.push_reaction = push_reaction(args)
        elif name == "isRedstoneConductor":
            p.redstone_conductor = redstone_conductor(args)
        elif name == "instrument":
            m = re.search(r'NoteBlockInstrument\.([A-Z_]+)', args)
            if m:
                p.instrument = m.group(1).lower()
        elif name == "mapColor":
            c = resolve_map_color(args)
            if c and c.startswith("@param:"):
                c = ctx.get(c[7:], "NONE")
            if c:
                p.map_color = c
    return p


# ── Blocks.java walk ──────────────────────────────────────────────────────────

# The four register wrappers, transcribed from their bodies (see the file
# header of Blocks.java around line 1226).
def wrapper_props(fn, args, fields, where):
    if fn == "registerBed":
        p = Props()
        strength(p, 0.2)
        p.ignited_by_lava = True
        dye = re.fullmatch(r'DyeColor\.([A-Z_]+)', args[1].strip())
        p.map_color = DYE_TO_MAP.get(dye.group(1), "WOOL") if dye else "WOOL"
        p.push_reaction = "Popped"
        return p
    if fn == "registerStainedGlass":
        p = Props()
        strength(p, 0.3)
        dye = re.fullmatch(r'DyeColor\.([A-Z_]+)', args[1].strip())
        p.map_color = DYE_TO_MAP.get(dye.group(1), "NONE") if dye else "NONE"
        p.redstone_conductor = "Never"
        p.instrument = "hat"
        return p
    if fn in ("registerStair", "registerLegacyStair", "registerSlab", "registerWall"):
        # Properties.ofFullCopy(base) / ofLegacyCopy(base) — both copy every
        # column we consume, so a stair (and 26.3's slab and wall helpers)
        # simply IS its base block.
        base = fields.get(args[1].strip())
        if base is None:
            raise SystemExit("%s: stair base %s unresolved" % (where, args[1]))
        p = base.copy()
        # 26.3's registerSlab(id, base, destroyTime) chains .destroyTime(t) and
        # registerSlab(id, base, destroyTime, resistance) .strength(t, r) on
        # the copy (Blocks.java registerSlab overloads).
        if fn == "registerSlab" and len(args) == 3:
            p.destroy_time = parse_float(args[2])
        elif fn == "registerSlab" and len(args) == 4:
            strength(p, parse_float(args[2]), parse_float(args[3]))
        return p
    return None


def slug_of(arg):
    """The registry slug an argument names: a string literal, or one of the
    net.minecraft.references.Blocks ResourceKey constants."""
    arg = arg.strip()
    m = re.fullmatch(r'"([a-z0-9_]+)"', arg)
    if m:
        return m.group(1)
    m = re.fullmatch(r'(?:net\.minecraft\.references\.)?Blocks\.([A-Z_0-9]+)', arg)
    if m:
        return REF_KEYS.get(m.group(1))
    # 26.3: the key constant IS the slug, upper-cased.
    m = re.fullmatch(r'Block(?:Item)?Ids\.([A-Z_0-9]+)', arg)
    if m:
        return m.group(1).lower()
    return None


# WeatheringCopperBlocks.create(id, register, waxedFactory, weatheringFactory,
# propertiesSupplier) registers EIGHT blocks off one call — the four weather
# stages and their waxed twins — all sharing whatever the supplier returns.
# Missing this is what left the copper bar / chain / lantern family with no
# hardness at all.
WEATHERING_RE = re.compile(r'\bWeatheringCopperBlocks\.create\s*\(')
WEATHERING_PREFIXES = ["", "exposed_", "weathered_", "oxidized_",
                       "waxed_", "waxed_exposed_", "waxed_weathered_", "waxed_oxidized_"]


def parse_weathering(src, fields, by_slug, order):
    """The copper family. Returns how many slugs it added."""
    added = 0
    for m in WEATHERING_RE.finditer(src):
        open_idx = m.end() - 1
        close = balanced(src, open_idx)
        args = split_args(src[open_idx + 1:close - 1])
        if len(args) < 2:
            continue
        base = slug_of(args[0])
        if base is None:
            continue

        # The last argument is `(state) -> <Properties expression>`. Every
        # current supplier ignores the weather state, so one evaluation serves
        # all eight registrations.
        supplier = args[-1]
        arrow = supplier.find("->")
        body = supplier[arrow + 2:].strip() if arrow >= 0 else supplier
        p = eval_props(body, fields,
                       where='WeatheringCopperBlocks.create("%s")' % base)
        if p is None:
            print("  ! weathering copper '%s': unrecognised Properties, skipped" % base,
                  file=sys.stderr)
            continue

        for prefix in WEATHERING_PREFIXES:
            slug = prefix + base
            if slug not in by_slug:
                order.append(slug)
            by_slug[slug] = p.copy()
            added += 1
    return added


CALL_RE = re.compile(
    r'\b(register|registerBed|registerStainedGlass|registerStair|registerLegacyStair'
    r'|registerSlab|registerWall)\s*\(')


# ── ColorCollection (26.3) ───────────────────────────────────────────────────
# 26.3 registers every dyed family off ONE call per family instead of sixteen
# `register("white_wool", …)` lines:
#
#   WOOL = ColorCollection.registerBlocks(BlockItemIds.WOOL, Blocks::register,
#            (var0, p) -> { return new Block(p); },
#            (color) -> { return Properties.of().mapColor(color.getMapColor())…; });
#   WOOL_STAIRS = ColorCollection.zipMap(ColorCollection.VALUES, BlockItemIds.WOOL_STAIRS,
#            (color, id) -> { return registerStair(id, (Block)WOOL.pick(color)); });
#
# ColorCollection.registerBlocks is itself zipMap(VALUES, ids, (color, id) ->
# register(id, factory, supplier(color))) (ColorCollection.java:42), and the
# ids are BlockItemIds/BlockIds.createSimpleColored("wool") — "<colour>_wool"
# for each colour (ColorCollection.prefixWithColor). So each call is expanded
# here into the sixteen ordinary registrations it performs, with the lambda's
# colour parameter bound to each DyeColor in turn, and those go through the
# same evaluation as a written-out `register(...)`. The colour collection's
# field becomes sixteen fields named FIELD@colour, which is what a later
# `(Block)FIELD.pick(color)` resolves to.
COLOR_COLLECTION_RE = re.compile(
    r'\b([A-Z][A-Z_0-9]*)\s*=\s*ColorCollection\.(registerBlocks|zipMap)\s*\(')
REFS2       = "minecraft_code_26.3-pre-2/decompiled_net/minecraft/references"
COLORCOLL2  = "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/block/ColorCollection.java"
DYECOLOR2   = "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/item/DyeColor.java"


def load_dye_colors():
    """ColorCollection.VALUES as [(DyeColor constant, serialized name)], in
    VALUES order (ColorCollection.java: `VALUES = new ColorCollection(
    DyeColor.WHITE, …)`); the name is DyeColor's own, e.g. LIGHT_BLUE ->
    "light_blue"."""
    names = dict(re.findall(r'^\s+([A-Z_]+)\(\d+,\s*"([a-z_]+)"', open(DYECOLOR).read(), re.M))
    if not os.path.exists(COLORCOLL2):
        return []
    m = re.search(r'VALUES\s*=\s*new ColorCollection\(([^;]*)\);', open(COLORCOLL2).read())
    if not m:
        raise SystemExit("ColorCollection.VALUES not found — ColorCollection.java layout changed")
    order = re.findall(r'DyeColor\.([A-Z_]+)', m.group(1))
    if len(order) != 16 or any(c not in names for c in order):
        raise SystemExit("ColorCollection.VALUES does not list the sixteen DyeColors")
    return [(c, names[c]) for c in order]


def load_colored_ids():
    """`BlockItemIds.WOOL` / `BlockIds.WALL_BANNER` -> the base name its
    createSimpleColored("…") prefixes with each colour."""
    out = {}
    for cls in ("BlockItemIds", "BlockIds"):
        path = os.path.join(REFS2, cls + ".java")
        if not os.path.exists(path):
            continue
        for const, base in re.findall(
                r'\b([A-Z][A-Z_0-9]*)\s*=\s*createSimpleColored\("([a-z0-9_]+)"\)', open(path).read()):
            out[cls + "." + const] = base
    return out


DYE_COLORS  = load_dye_colors()
# 26.3's DyeColor(id, name, textureDiffuseColor, mapColor, terracottaColor, …):
# the second MapColor, which DyeColor.getTerracottaColor() returns (26.1's
# DyeColor has no such argument; its terracottas name the colour directly).
DYE_TO_TERRACOTTA = dict(re.findall(
    r'^\s+([A-Z_]+)\(\d+,\s*"[a-z_]+",\s*\d+,\s*MapColor\.[A-Z_0-9]+,\s*MapColor\.([A-Z_0-9]+)',
    open(DYECOLOR2).read(), re.M)) if os.path.exists(DYECOLOR2) else {}
COLORED_IDS = load_colored_ids()


def lambda_parts(text):
    """`(a, b) -> expr` or `(a, b) -> { return expr; }` -> ([a, b], expr)."""
    m = re.match(r'\s*\(([^)]*)\)\s*->\s*', text, re.S)
    if not m:
        m = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*->\s*', text, re.S)
        if not m:
            return None, None
    params = [p.strip() for p in m.group(1).split(",") if p.strip()]
    body = text[m.end():].strip()
    if body.startswith("{"):
        rm = re.fullmatch(r'\{\s*return\s+(.*?);\s*\}', body, re.S)
        if not rm:
            return params, None           # a multi-statement body: not a plain expression
        body = rm.group(1).strip()
    return params, body


def bind_color(expr, param, dye):
    """`expr` with the lambda's colour parameter bound to DyeColor `dye`."""
    p = re.escape(param)
    # (Block)FIELD.pick(color) -> the per-colour field FIELD@name.
    name = dict(DYE_COLORS)[dye]
    expr = re.sub(r'\(\s*(?:Block|BlockBehaviour)\s*\)\s*([A-Z][A-Z_0-9]*)\.pick\(\s*%s\s*\)' % p,
                  lambda m: "%s@%s" % (m.group(1), name), expr)
    # color.getMapColor() -> DyeColor's map colour (its constructor's MapColor).
    expr = re.sub(r'\b%s\.getMapColor\(\)' % p, "MapColor." + DYE_TO_MAP[dye], expr)
    # color.getTerracottaColor() -> its terracotta MapColor (the next argument).
    expr = re.sub(r'\b%s\.getTerracottaColor\(\)' % p, "MapColor." + DYE_TO_TERRACOTTA[dye], expr)
    # `color == DyeColor.X ? A : B`, once both arms are constants.
    expr = re.sub(r'\b%s\s*==\s*DyeColor\.([A-Z_]+)\s*\?\s*(MapColor\.[A-Z_0-9]+)\s*:\s*(MapColor\.[A-Z_0-9]+)' % p,
                  lambda m: m.group(2) if m.group(1) == dye else m.group(3), expr)
    return re.sub(r'\b%s\b' % p, "DyeColor." + dye, expr)


def color_collection_calls(src):
    """Every ColorCollection registration in `src`, expanded to the ordinary
    calls it makes: [(source offset, fn, args, field)] where `args[0]` is the
    colour's slug as a string literal and `field` is FIELD@colour."""
    out = []
    for m in COLOR_COLLECTION_RE.finditer(src):
        field, kind = m.group(1), m.group(2)
        open_idx = m.end() - 1
        close = balanced(src, open_idx)
        args = split_args(src[open_idx + 1:close - 1])
        where = "%s = ColorCollection.%s" % (field, kind)
        if kind == "registerBlocks":
            # (ids, register, factory, propertiesSupplier)
            if len(args) != 4:
                raise SystemExit("%s: expected 4 arguments" % where)
            ids, factory, supplier = args[0], args[2], args[3]
        else:
            # (ColorCollection.VALUES, ids, (color, id) -> registration)
            if len(args) != 3 or args[0] != "ColorCollection.VALUES":
                raise SystemExit("%s: only zipMap(ColorCollection.VALUES, ids, …) is understood" % where)
            ids, supplier = args[1], args[2]
        base = COLORED_IDS.get(ids.strip())
        if base is None:
            raise SystemExit("%s: %s is not a createSimpleColored id" % (where, ids))
        params, body = lambda_parts(supplier)
        if not params or body is None:
            raise SystemExit("%s: unrecognised lambda %s" % (where, supplier[:80]))

        for dye, name in DYE_COLORS:
            slug = '"%s_%s"' % (name, base)
            if kind == "registerBlocks":
                call_fn = "register"
                call_args = [slug, factory, bind_color(body, params[0], dye)]
            else:
                expr = bind_color(body, params[0], dye)
                if len(params) > 1:
                    expr = re.sub(r'\b%s\b' % re.escape(params[1]), slug, expr)
                cm = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', expr)
                if not cm or not CALL_RE.match(expr):
                    raise SystemExit("%s: body is not a register call: %s" % (where, expr[:80]))
                call_fn = cm.group(1)
                call_close = balanced(expr, cm.end() - 1)
                call_args = split_args(expr[cm.end():call_close - 1])
            out.append((m.start(), call_fn, call_args, "%s@%s" % (field, name)))
    return out


def registrations(src):
    """Every block registration in `src`, in source order: [(offset, fn, args,
    field)] — the written-out register* calls (their field is the assignment
    target, or None) and the expanded ColorCollection families."""
    out = []
    for m in CALL_RE.finditer(src):
        open_idx = m.end() - 1
        try:
            close = balanced(src, open_idx)
        except ValueError:
            continue
        args = split_args(src[open_idx + 1:close - 1])
        if not args:
            continue
        head = src.rfind("\n", 0, m.start())
        line = src[head + 1:m.start()]
        fm = re.search(r'([A-Z][A-Z_0-9]*)\s*=\s*$', line)
        out.append((m.start(), m.group(1), args, fm.group(1) if fm else None))
    out += color_collection_calls(src)
    out.sort(key=lambda r: r[0])
    return out


def parse_blocks(path=None):
    src = open(path or BLOCKS).read()
    fields = {}     # Java field name -> Props
    by_slug = {}    # registry slug   -> Props
    order = []

    for _offset, fn, args, field in registrations(src):
        slug = slug_of(args[0])
        if slug is None:
            continue                       # the private register(...) definitions
        where = "%s(\"%s\")" % (fn, slug)

        if fn == "register":
            # register(id, props) or register(id, factory, props): the
            # Properties expression is always the LAST argument.
            p = eval_props(args[-1], fields, where=where)
            if p is None:
                print("  ! %s: unrecognised Properties expression, skipped" % where,
                      file=sys.stderr)
                continue
        else:
            p = wrapper_props(fn, args, fields, where)
            if p is None:
                continue

        # The assignment target, so ofFullCopy(FIELD) can find it later.
        if field:
            fields[field] = p

        if slug not in by_slug:
            order.append(slug)
        by_slug[slug] = p

    parse_weathering(src, fields, by_slug, order)
    return by_slug, order


# ── Block tags: tool + tier ───────────────────────────────────────────────────

def load_tag(name, _seen=None):
    """Flatten a block tag to a set of slugs, following nested `#tag` entries.

    The nesting is not decoration: mineable/pickaxe lists `#minecraft:anvil`,
    `#minecraft:walls`, `#minecraft:chains` and nine more rather than their
    members, so a reader that skips `#` entries decides that anvils and every
    wall in the game need no tool — which then reads as "unbreakable", because
    they also carry requiresCorrectToolForDrops.
    """
    if _seen is None:
        _seen = set()
    if name in _seen:                     # tags may not cycle, but do not trust it
        return set()
    _seen.add(name)

    path = os.path.join(TAGS, name + ".json")
    if not os.path.exists(path):
        print("  ! tag not found: %s" % name, file=sys.stderr)
        return set()
    with open(path) as f:
        data = json.load(f)

    out = set()
    for v in data.get("values", []):
        if isinstance(v, dict):
            v = v.get("id", "")
        if not isinstance(v, str) or not v:
            continue
        if v.startswith("#"):
            out |= load_tag(v[1:].split(":")[-1], _seen)
        else:
            out.add(v.split(":")[-1])
    return out


# Engine slugs that name a vanilla block under a different id. `chain` was
# renamed `iron_chain` when the copper chain family landed and BlockDefs.inc
# still carries the old name; the other two engine slugs (ominous_banner,
# set_spawn) are not blocks in vanilla at all and correctly have no row.
SLUG_ALIASES = {
    "chain": "iron_chain",
    # engine blocks (redstone_plus): the zero-delay torch breaks like the redstone torch
    "blue_redstone_torch": "redstone_torch",
    "blue_redstone_wall_torch": "redstone_wall_torch",
    "display_block": "redstone_lamp",
    # The Hush (engine dimension, 2026-09-21): each block breaks like the
    # vanilla block it is modelled on, and inherits its mineable/needs-tool tags.
    "hushstone": "deepslate",
    "polished_hushstone": "polished_deepslate",
    "hushstone_bricks": "deepslate_bricks",
    "sculk_loam": "grass_block",
    "echo_ore": "deepslate_diamond_ore",
    "resonant_crystal": "amethyst_block",
    "resonance_bloom": "dandelion",
    "whisperwood_log": "oak_log",
    "whisperwood_planks": "oak_planks",
    "lantern_leaves": "oak_leaves",
    "hush_moss": "moss_block",
    "hush_portal": "nether_portal",
    # The Hush, second drop (2026-09-22). Same rule: break like the vanilla
    # block each is modelled on. echo_core aliases obsidian for its numbers
    # but its TIER comes from needs_resonite_tool.json under its OWN slug —
    # see the engine-slug tier lookup in main().
    "stripped_whisperwood_log": "stripped_oak_log",
    "whisperwood_stairs": "oak_stairs",
    "whisperwood_slab": "oak_slab",
    "whisperwood_fence": "oak_fence",
    "whisperwood_fence_gate": "oak_fence_gate",
    "whisperwood_door": "oak_door",
    "whisperwood_trapdoor": "oak_trapdoor",
    "whisperwood_sapling": "oak_sapling",
    "hushstone_stairs": "cobbled_deepslate_stairs",
    "hushstone_slab": "cobbled_deepslate_slab",
    "polished_hushstone_stairs": "polished_deepslate_stairs",
    "polished_hushstone_slab": "polished_deepslate_slab",
    "hushstone_brick_stairs": "deepslate_brick_stairs",
    "hushstone_brick_slab": "deepslate_brick_slab",
    "hushstone_brick_wall": "deepslate_brick_wall",
    "cracked_hushstone_bricks": "cracked_deepslate_bricks",
    "chiseled_hushstone_bricks": "chiseled_deepslate",
    "hush_grass": "short_grass",
    "resonant_cluster": "amethyst_cluster",
    "resonite_ore": "deepslate_diamond_ore",      # iron tier, pickaxe
    "resonite_block": "iron_block",               # stone tier, pickaxe
    "echo_lantern": "lantern",
    "echo_core": "obsidian",                      # tier overridden: Resonite
    # The Choir Hall puzzle (2026-09-22): the chime breaks like the crystal
    # it is tuned from; the altar like the lodestone (3.5, pickaxe).
    "resonant_chime": "amethyst_block",
    "choir_altar": "lodestone",
    # The tools of the deep (2026-09-22): the whisperfruit breaks like the
    # cocoa pod it is modelled on (0.2, axe); the echo heart like the beacon
    # it stands in for (3.0, no tool needed).
    "hanging_whisperfruit": "cocoa",
    "echo_heart": "beacon",
    # The Hush lighthouse (2026-09-22): the lamp, glass and metal, breaks
    # like the beacon it is classed as (3.0, no tool needed).
    "hush_lighthouse_lamp": "beacon",
    # Aurelith, the Lantern City (2026-09-22): choirstone breaks like the
    # stone it was sung from (1.5/6, pickaxe), the tiles and pillar like the
    # deepslate tiles / quartz pillar they echo; stave stone like polished
    # deepslate. Nightglass is tinted glass (drops itself, no tool); the
    # grate is a copper grate. The lumen panels and strip are sea lanterns
    # (0.3, glass); the conduit breaks like a chain, the lamp like a lantern.
    # The river's water is water (unbreakable, 100). The resonance engine and
    # the voice beacon are beacons (3.0, no tool).
    "choirstone": "stone", "polished_choirstone": "polished_andesite",
    "choirstone_bricks": "stone_bricks", "cracked_choirstone_bricks": "cracked_stone_bricks",
    "chiseled_choirstone": "chiseled_stone_bricks", "choirstone_tiles": "deepslate_tiles",
    "choirstone_pillar": "quartz_pillar",
    "polished_choirstone_stairs": "polished_andesite_stairs",
    "polished_choirstone_slab": "polished_andesite_slab",
    "choirstone_brick_stairs": "stone_brick_stairs", "choirstone_brick_slab": "stone_brick_slab",
    "choirstone_brick_wall": "stone_brick_wall",
    "choirstone_tile_stairs": "deepslate_tile_stairs", "choirstone_tile_slab": "deepslate_tile_slab",
    "stave_stone": "polished_deepslate",
    "nightglass": "tinted_glass",
    "resonite_grate": "copper_grate",
    "cyan_lumen_panel": "sea_lantern", "violet_lumen_panel": "sea_lantern",
    "amber_lumen_panel": "sea_lantern", "lumen_strip": "sea_lantern",
    "crystal_conduit": "iron_chain",
    "choir_lamp": "lantern",
    "resonant_water": "water",
    "resonance_engine": "beacon",
    "voice_beacon": "beacon",
    # Aurelith, reawakening the Heart (2026-09-22): the dim lights break like
    # their lit twins; the chord socket and the pedestal like the lodestone
    # the choir altar is (3.5, pickaxe); the cabinet like a barrel (2.5, axe).
    "dim_cyan_lumen_panel": "sea_lantern", "dim_violet_lumen_panel": "sea_lantern",
    "dim_amber_lumen_panel": "sea_lantern", "dim_lumen_strip": "sea_lantern",
    "dim_stave_stone": "polished_deepslate",
    "dim_choir_lamp": "lantern",
    "chord_socket": "lodestone",
    "voice_pedestal": "lodestone",
    "choir_cabinet": "barrel",
    # Aurelith's flickering windows: sea lanterns, like the lumen panels.
    "guttering_amber_window": "sea_lantern",
    "waking_amber_window": "sea_lantern",
    "restless_amber_window": "sea_lantern",
    "guttering_cyan_window": "sea_lantern",
    "waking_cyan_window": "sea_lantern",
    "guttering_violet_window": "sea_lantern",
    "waking_violet_window": "sea_lantern",
    # ── Twilight Forest (pass one, docs/mod-ports.md). Break like the vanilla
    # block each stands in for, tool/tier tags included. Deliberate pass-one
    # numbers, NOT TF's: the mazestone family is TF strength(100, 5) and
    # meant for the Mazebreaker pickaxe (pass two) — here it breaks like
    # stone bricks; the logs/planks/leaves use oak's (TF's leaves are 0.2 like
    # oak; dark_leaves' 2.0/10 is pass two).
    "twilight_oak_log": "oak_log", "canopy_log": "oak_log",
    "tf_mangrove_log": "oak_log", "dark_log": "oak_log",
    "stripped_twilight_oak_log": "stripped_oak_log", "stripped_canopy_log": "stripped_oak_log",
    "stripped_tf_mangrove_log": "stripped_oak_log", "stripped_dark_log": "stripped_oak_log",
    "twilight_oak_leaves": "oak_leaves", "canopy_leaves": "oak_leaves",
    "tf_mangrove_leaves": "oak_leaves", "dark_leaves": "oak_leaves",
    "hardened_dark_leaves": "oak_leaves",
    "twilight_oak_planks": "oak_planks", "canopy_planks": "oak_planks",
    "tf_mangrove_planks": "oak_planks", "dark_planks": "oak_planks",
    "twilight_oak_sapling": "oak_sapling", "canopy_sapling": "oak_sapling",
    "tf_mangrove_sapling": "oak_sapling", "darkwood_sapling": "oak_sapling",
    "root": "oak_planks",                         # TF strength(2, 3), #mineable/axe
    "liveroot_block": "oak_planks",               # TF strength(2, 3), #mineable/axe
    "hedge": "oak_leaves",
    "firefly": "end_rod", "cicada": "end_rod", "moonworm": "end_rod",   # instabreak
    "mushgloom": "dandelion", "mayapple": "dandelion", "torchberry_plant": "dandelion",
    "fiddlehead": "short_grass",
    "clover_patch": "pink_petals", "moss_patch": "pink_petals",   # TF instabreak
    # Not `snow` (its state alias): SNOW is requiresCorrectToolForDrops, so
    # fallen leaves would drop nothing without a shovel. TF: instabreak.
    "fallen_leaves": "leaf_litter",
    "mazestone": "stone_bricks", "mazestone_brick": "stone_bricks",
    "cracked_mazestone": "stone_bricks", "mossy_mazestone": "stone_bricks",
    "mazestone_mosaic": "stone_bricks", "mazestone_border": "stone_bricks",
    "aurora_block": "stone",
    "twilight_portal": "nether_portal",           # unbreakable
    # ── The Aether (pass one). Same rule.
    "aether_grass_block": "grass_block",
    "aether_dirt": "dirt",
    "quicksoil": "sand",
    "holystone": "stone", "mossy_holystone": "stone",
    "holystone_bricks": "stone_bricks",
    "cold_aercloud": "glass", "blue_aercloud": "glass", "golden_aercloud": "glass",
    "icestone": "stone",
    "ambrosium_ore": "iron_ore", "zanite_ore": "iron_ore", "gravitite_ore": "iron_ore",
    "skyroot_log": "oak_log", "golden_oak_log": "oak_log",
    "stripped_skyroot_log": "stripped_oak_log",
    "skyroot_leaves": "oak_leaves", "golden_oak_leaves": "oak_leaves",
    "skyroot_planks": "oak_planks",
    "skyroot_sapling": "oak_sapling", "golden_oak_sapling": "oak_sapling",
    "aether_portal": "nether_portal",             # unbreakable
    "berry_bush": "oak_leaves",                   # Aether strength(0.2)
    "berry_bush_stem": "oak_leaves",              # Aether strength(0.2)
    "white_flower": "dandelion", "purple_flower": "dandelion",
    # ── The Aether, pass two. The alias supplies the map colour, push
    # reaction and instrument; the mod's own strength numbers are applied on
    # top from MOD_STRENGTH below, and the tool/tier from the Aether's
    # mineable/needs_* tag entries, which data/minecraft/tags/block carries
    # under the engine slugs (the tool and tier lookups consult the engine
    # slug before the alias).
    "holystone_stairs": "stone_stairs", "holystone_slab": "stone_slab",
    "holystone_wall": "stone_brick_wall",
    "mossy_holystone_stairs": "mossy_stone_brick_stairs",
    "mossy_holystone_slab": "mossy_stone_brick_slab",
    "mossy_holystone_wall": "mossy_stone_brick_wall",
    "holystone_brick_stairs": "stone_brick_stairs", "holystone_brick_slab": "stone_brick_slab",
    "holystone_brick_wall": "stone_brick_wall",
    "holystone_button": "stone_button", "holystone_pressure_plate": "stone_pressure_plate",
    "skyroot_wood": "oak_wood", "golden_oak_wood": "oak_wood",
    "stripped_skyroot_wood": "stripped_oak_wood",
    "skyroot_stairs": "oak_stairs", "skyroot_slab": "oak_slab", "skyroot_fence": "oak_fence",
    "skyroot_fence_gate": "oak_fence_gate", "skyroot_door": "oak_door",
    "skyroot_trapdoor": "oak_trapdoor", "skyroot_button": "oak_button",
    "skyroot_pressure_plate": "oak_pressure_plate",
    "aerogel": "glass", "quicksoil_glass": "glass",
    "icestone_stairs": "stone_stairs", "icestone_slab": "stone_slab",
    "icestone_wall": "stone_brick_wall",
    "ambrosium_block": "gold_block", "zanite_block": "amethyst_block",
    "enchanted_gravitite": "iron_block",
    "ambrosium_torch": "torch", "ambrosium_wall_torch": "wall_torch",
    "carved_stone": "stone", "sentry_stone": "stone",
    "angelic_stone": "stone", "light_angelic_stone": "stone",
    "hellfire_stone": "stone", "light_hellfire_stone": "stone",
    "trapped_carved_stone": "stone", "trapped_sentry_stone": "stone",
    "trapped_angelic_stone": "stone", "trapped_light_angelic_stone": "stone",
    "trapped_hellfire_stone": "stone", "trapped_light_hellfire_stone": "stone",
    "locked_carved_stone": "bedrock", "locked_sentry_stone": "bedrock",
    "locked_angelic_stone": "bedrock", "locked_light_angelic_stone": "bedrock",
    "locked_hellfire_stone": "bedrock", "locked_light_hellfire_stone": "bedrock",
    "boss_doorway_carved_stone": "bedrock", "boss_doorway_sentry_stone": "bedrock",
    "boss_doorway_angelic_stone": "bedrock", "boss_doorway_light_angelic_stone": "bedrock",
    "boss_doorway_hellfire_stone": "bedrock", "boss_doorway_light_hellfire_stone": "bedrock",
    "treasure_doorway_carved_stone": "bedrock", "treasure_doorway_sentry_stone": "bedrock",
    "treasure_doorway_angelic_stone": "bedrock",
    "treasure_doorway_light_angelic_stone": "bedrock",
    "treasure_doorway_hellfire_stone": "bedrock",
    "treasure_doorway_light_hellfire_stone": "bedrock",
    "pillar": "quartz_pillar", "pillar_top": "quartz_pillar",
    "carved_stairs": "stone_stairs", "carved_slab": "stone_slab", "carved_wall": "stone_brick_wall",
    "angelic_stairs": "stone_stairs", "angelic_slab": "stone_slab", "angelic_wall": "stone_brick_wall",
    "hellfire_stairs": "stone_stairs", "hellfire_slab": "stone_slab",
    "hellfire_wall": "stone_brick_wall",
    # ── Twilight Forest, pass two. Same scheme: alias for colour/push/
    # instrument, TF's strength in MOD_STRENGTH, tool/tier from TF's
    # mineable/needs_* entries carried under the engine slugs.
    **{s: "oak_planks" for s in ("towerwood", "encased_towerwood", "cracked_towerwood",
                                 "mossy_towerwood", "infested_towerwood")},
    **{s: "stone_bricks" for s in ("castle_brick", "worn_castle_brick", "cracked_castle_brick", "castle_roof_tile", "mossy_castle_brick", "thick_castle_brick", "encased_castle_brick_tile", "bold_castle_brick_tile", "pink_castle_rune_brick", "blue_castle_rune_brick", "yellow_castle_rune_brick", "violet_castle_rune_brick")},
    **{s: "stone_brick_stairs" for s in ("castle_brick_stairs", "worn_castle_brick_stairs", "cracked_castle_brick_stairs", "mossy_castle_brick_stairs", "encased_castle_brick_stairs", "bold_castle_brick_stairs")},
    "encased_castle_brick_pillar": "quartz_pillar", "bold_castle_brick_pillar": "quartz_pillar",
    "deadrock": "stone", "cracked_deadrock": "stone", "weathered_deadrock": "stone",
    "trollsteinn": "stone",
    "brown_thorns": "glass", "green_thorns": "glass", "burnt_thorns": "dead_bush",   # not axe-mineable in TF
    "huge_water_lily": "lily_pad",
    "uncrafting_table": "crafting_table",
    "cinder_log": "oak_log", "cinder_wood": "oak_wood",
    "ironwood_block": "iron_block", "steeleaf_block": "iron_block",
    "knightmetal_block": "iron_block", "fiery_block": "iron_block",
}

# Mod blocks whose own `strength(destroyTime, resistance)` and
# requiresCorrectToolForDrops differ from their alias's: (destroyTime,
# explosionResistance, requiresCorrectTool), verbatim from AetherBlocks /
# TFBlocks. Applied to a COPY of the alias's row. Pass one's Aether terrain
# blocks are listed too, so a holystone stair and the holystone it is cut
# from break alike.
MOD_STRENGTH = {
    # AetherBlocks: HOLYSTONE strength(0.5) + requiresCorrectToolForDrops, and
    # everything ofFullCopy(HOLYSTONE / MOSSY_HOLYSTONE).
    **{s: (0.5, 0.5, True) for s in (
        "holystone", "mossy_holystone", "holystone_stairs", "holystone_slab", "holystone_wall",
        "mossy_holystone_stairs", "mossy_holystone_slab", "mossy_holystone_wall")},
    # HOLYSTONE_BRICKS strength(2.0, 6.0).
    **{s: (2.0, 6.0, True) for s in (
        "holystone_bricks", "holystone_brick_stairs", "holystone_brick_slab",
        "holystone_brick_wall")},
    # ICESTONE strength(0.5) (IcestoneStairs/Slab/Wall copy it).
    **{s: (0.5, 0.5, True) for s in ("icestone", "icestone_stairs", "icestone_slab",
                                      "icestone_wall")},
    "aether_dirt": (0.2, 0.2, False), "aether_grass_block": (0.2, 0.2, False),
    "quicksoil": (0.5, 0.5, False),
    "ambrosium_ore": (3.0, 3.0, True), "zanite_ore": (3.0, 3.0, True),
    "gravitite_ore": (3.0, 3.0, True),
    "aerogel": (1.0, 2000.0, True),
    "quicksoil_glass": (0.2, 0.2, False),
    "ambrosium_block": (5.0, 6.0, True), "zanite_block": (5.0, 6.0, True),
    "enchanted_gravitite": (5.0, 6.0, True),
    # CARVED_STONE / ANGELIC_STONE / HELLFIRE_STONE strength(0.5, 6.0), and
    # every light / trapped / stairs / slab / wall copy of them.
    **{s: (0.5, 6.0, True) for s in (
        "carved_stone", "sentry_stone", "angelic_stone", "light_angelic_stone",
        "hellfire_stone", "light_hellfire_stone",
        "trapped_carved_stone", "trapped_sentry_stone", "trapped_angelic_stone",
        "trapped_light_angelic_stone", "trapped_hellfire_stone", "trapped_light_hellfire_stone",
        "carved_stairs", "carved_slab", "carved_wall", "angelic_stairs", "angelic_slab",
        "angelic_wall", "hellfire_stairs", "hellfire_slab", "hellfire_wall")},
    # PILLAR / PILLAR_TOP strength(0.5).
    "pillar": (0.5, 0.5, True), "pillar_top": (0.5, 0.5, True),
    # TFBlocks. The towerwood, castle and deadrock numbers are TF's own
    # progression walls (the dark tower, the final castle, the highlands),
    # not typos: 40 / 100 / 100.
    "towerwood": (40.0, 6.0, False), "encased_towerwood": (40.0, 6.0, False),
    "cracked_towerwood": (40.0, 6.0, False), "mossy_towerwood": (40.0, 6.0, False),
    "infested_towerwood": (2.0, 6.0, False),
    **{s: (100.0, 50.0, True) for s in ("castle_brick", "worn_castle_brick", "cracked_castle_brick", "castle_roof_tile", "mossy_castle_brick", "thick_castle_brick", "encased_castle_brick_tile", "bold_castle_brick_tile", "pink_castle_rune_brick", "blue_castle_rune_brick", "yellow_castle_rune_brick", "violet_castle_rune_brick", "castle_brick_stairs", "worn_castle_brick_stairs", "cracked_castle_brick_stairs", "mossy_castle_brick_stairs", "encased_castle_brick_stairs", "bold_castle_brick_stairs", "encased_castle_brick_pillar", "bold_castle_brick_pillar")},
    "deadrock": (100.0, 6000000.0, True), "cracked_deadrock": (100.0, 6000000.0, True),
    "weathered_deadrock": (100.0, 6000000.0, True),
    "trollsteinn": (2.0, 6.0, True),
    "brown_thorns": (50.0, 2000.0, False), "green_thorns": (50.0, 2000.0, False),
    "burnt_thorns": (0.0, 0.0, False), "huge_water_lily": (0.0, 0.0, False),
    "uncrafting_table": (2.5, 2.5, False),
    "cinder_log": (1.0, 1.0, False), "cinder_wood": (1.0, 1.0, False),
    "ironwood_block": (5.0, 6.0, False), "steeleaf_block": (5.0, 6.0, False),
    "knightmetal_block": (5.0, 40.0, True), "fiery_block": (5.0, 6.0, True),
}


def load_engine_slugs():
    """The registry slugs this engine actually has, from BlockDefs.inc."""
    slugs = []
    with open(DEFS) as f:
        for line in f:
            m = re.match(r'\s*BLOCK_DEF\(\s*\w+\s*,\s*"([a-z0-9_]+)"', line)
            if m:
                slugs.append(m.group(1))
    return slugs


TOOL_TAGS = [("pickaxe", "Pickaxe"), ("axe", "Axe"),
             ("shovel", "Shovel"), ("hoe", "Hoe")]
# Highest tier first: the first tag that lists the block wins.
# needs_resonite_tool is an engine tag (The Hush's echo core) and names the
# engine slug, so the tier lookup below consults the ENGINE slug as well as
# the aliased vanilla one; MiningTier::Resonite lives in MiningTier.hpp.
TIER_TAGS = [("needs_resonite_tool", "Resonite"),
             ("needs_diamond_tool", "Diamond"), ("needs_iron_tool", "Iron"),
             ("needs_stone_tool", "Stone")]


def main():
    for path in (BLOCKS, MAPCOLOR, DYECOLOR, DEFS):
        if not os.path.exists(path):
            raise SystemExit("missing input: " + path)

    by_slug, _order = parse_blocks()
    print("parsed %d block registrations from Blocks.java" % len(by_slug))
    if os.path.exists(BLOCKS2):
        by_slug2, _ = parse_blocks(BLOCKS2)
        added = 0
        for slug, props in by_slug2.items():
            if slug not in by_slug:
                by_slug[slug] = props
                added += 1
        print("  + %d from 26.3's Blocks.java (gap fill)" % added)

    tools = {tag: load_tag("mineable/" + tag) for tag, _ in TOOL_TAGS}
    tiers = {tag: load_tag(tag) for tag, _ in TIER_TAGS}
    for tag, _ in TOOL_TAGS:
        if not tools[tag]:
            print("  ! mineable/%s.json is empty or missing" % tag, file=sys.stderr)

    engine = load_engine_slugs()
    rows, missing = [], []
    for slug in sorted(set(engine)):
        # The alias applies to the TAG lookups too, not just the properties:
        # vanilla's tags list `iron_chain`, so a `chain` row that resolved its
        # hardness through the alias but its tool through the raw slug would
        # come out unbreakable (requiresCorrectTool with no usable tool).
        vanilla = SLUG_ALIASES.get(slug, slug)
        p = by_slug.get(vanilla)
        if p is None:
            missing.append(slug)
            continue
        if slug in MOD_STRENGTH:
            p = p.copy()
            p.destroy_time, p.resistance, p.requires_tool = MOD_STRENGTH[slug]

        tool = "None"
        for tag, cpp in TOOL_TAGS:
            # Engine slug first (a mod block listed in a mineable tag under
            # its own name), then the alias's vanilla slug.
            if slug in tools[tag] or vanilla in tools[tag]:
                tool = cpp
                break
        tier = "Wood"
        for tag, cpp in TIER_TAGS:
            # The engine slug first: an engine-only tag (needs_resonite_tool)
            # lists the engine block, and it must beat the alias's own tier
            # (echo_core copies obsidian's numbers, not its diamond tier).
            if slug in tiers[tag] or vanilla in tiers[tag]:
                tier = cpp
                break

        rows.append((slug, p, tool, tier))

    print("matched %d / %d engine blocks (%d unmatched)"
          % (len(rows), len(engine), len(missing)))
    if missing:
        print("  unmatched: " + ", ".join(missing[:12])
              + (" ..." if len(missing) > 12 else ""))

    emit(rows, len(missing))


HPP = '''// GENERATED by tools/gen_block_hardness.py — DO NOT EDIT BY HAND.
//
// Per-block mining and blast data, transcribed from MC's Blocks.java rather
// than guessed from the block's name.
//
// This table replaced `ClassifyByName` + `ApplyExplicitHardnessOverrides` in
// BlockRegistry.cpp, which inferred hardness from substrings of a model name
// ("anything containing 'stone' is 1.5"). That was right often enough to look
// correct and wrong often enough to matter, and it had no answer at all for
// explosion resistance — which is the number an explosion's ray march spends.
//
// Keyed on the vanilla registry SLUG (column 2 of BlockDefs.inc), not the
// BlockID, so adding a block to BlockDefs.inc picks its real values up for
// free and reordering the enum cannot silently shuffle the table.
#pragma once

#include "common/entity/MiningTier.hpp"
#include "common/world/block/PushReaction.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Game {

    struct GeneratedBlockHardnessRow {
        std::string_view slug;
        // MC Properties.destroyTime — seconds at base mining speed.
        // -1 is unbreakable (bedrock, barrier, the portal frames).
        float       destroyTime;
        // MC Properties.explosionResistance. `strength(a)` sets this to `a` as
        // well, so for most of the registry it equals destroyTime; the
        // exceptions are the blast-proof set (obsidian and the anvils at 1200,
        // bedrock at 3600000) and stone-family blocks, which are 1.5 to mine
        // and 6.0 to blow up.
        float       explosionResistance;
        bool        requiresCorrectTool;
        // 0xRRGGBB. MC's map colour, carried because ConcretePowderBlock and
        // AnvilBlock derive their FALLING DUST colour from it
        // (getDustColor -> state.getMapColor(...).col).
        uint32_t    mapColor;
        // MC Properties.ignitedByLava. The data half of fire spread, which
        // does not exist yet — carried now so that when FireBlock's tick lands
        // it does not also need a new generator run.
        bool        ignitedByLava;
        // From data/minecraft/tags/block/mineable/*.json — NOT from Blocks.java,
        // which does not know about tools at all.
        ToolType    preferredTool;
        // From data/minecraft/tags/block/needs_*_tool.json.
        MiningTier  minTier;
        // MC Properties.pushReaction — what a piston does to this block.
        PushReaction      pushReaction;
        // MC Properties.isRedstoneConductor — Blocks.java's never/always
        // overrides of the "is my collision shape a full cube" default.
        RedstoneConductor redstoneConductor;
        // MC Properties.instrument, as NoteBlockInstrument's serialized name
        // ("harp", "basedrum", "bass"…) — what a note block on top plays.
        std::string_view  instrument;
    };

    extern const GeneratedBlockHardnessRow kBlockHardnessTable[];
    extern const size_t                    kBlockHardnessCount;

    // Row for a registry slug, or null when the block is not in vanilla's
    // Blocks.java (this engine carries a handful that are not, and promoted
    // state variants share their base block's slug).
    const GeneratedBlockHardnessRow* FindBlockHardness(std::string_view slug);

} // namespace Game
'''


def emit(rows, missing_count):
    with open(OUT_HPP, "w") as f:
        f.write(HPP)

    with open(OUT_CPP, "w") as f:
        f.write('// GENERATED by tools/gen_block_hardness.py — DO NOT EDIT BY HAND.\n')
        f.write('#include "common/world/block/GeneratedBlockHardness.hpp"\n\n')
        f.write('#include <algorithm>\n\n')
        f.write('namespace Game {\n\n')
        f.write('    // Sorted by slug so FindBlockHardness can binary-search.\n')
        f.write('    const GeneratedBlockHardnessRow kBlockHardnessTable[] = {\n')
        for slug, p, tool, tier in rows:
            f.write('        { "%s", %sf, %sf, %s, 0x%06X, %s, ToolType::%s, MiningTier::%s, '
                    'PushReaction::%s, RedstoneConductor::%s, "%s" },\n'
                    % (slug, fmt(p.destroy_time), fmt(p.resistance),
                       "true" if p.requires_tool else "false",
                       MAP_COLORS.get(p.map_color, 0),
                       "true" if p.ignited_by_lava else "false",
                       tool, tier, p.push_reaction, p.redstone_conductor,
                       p.instrument))
        f.write('    };\n\n')
        f.write('    const size_t kBlockHardnessCount =\n'
                '        sizeof(kBlockHardnessTable) / sizeof(kBlockHardnessTable[0]);\n\n')
        f.write('''    const GeneratedBlockHardnessRow* FindBlockHardness(std::string_view slug) {
        const auto* begin = kBlockHardnessTable;
        const auto* end   = begin + kBlockHardnessCount;
        const auto* it = std::lower_bound(
            begin, end, slug,
            [](const GeneratedBlockHardnessRow& row, std::string_view key) {
                return row.slug < key;
            });
        return (it != end && it->slug == slug) ? it : nullptr;
    }

} // namespace Game
''')
    print("wrote %s (%d rows) and %s" % (OUT_CPP, len(rows), OUT_HPP))


def fmt(v):
    s = ("%g" % v)
    return s if "." in s or "e" in s else s + ".0"


if __name__ == "__main__":
    main()
