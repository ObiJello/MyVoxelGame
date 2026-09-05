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

MC       = "minecraft_code/decompiled_net/minecraft"
BLOCKS   = os.path.join(MC, "world/level/block/Blocks.java")
# 26.3's Blocks.java (minecraft_code2): parsed the same way, keyed on
# `BlockItemIds.X` / `BlockIds.X`, and merged setdefault-style so only the
# blocks 26.1 lacks (poplar, cinnabar, sulfur, …) come from it.
BLOCKS2  = "minecraft_code2/decompiled_net/minecraft/world/level/block/Blocks.java"
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
                 "ignited_by_lava")

    def __init__(self):
        # MC's field initialisers: both strengths are 0.0F, not 1.0. Getting
        # this wrong makes every decorative block instant-break.
        self.destroy_time = 0.0
        self.resistance = 0.0
        self.requires_tool = False
        self.map_color = "NONE"
        self.ignited_by_lava = False

    def copy(self):
        p = Props()
        p.destroy_time = self.destroy_time
        p.resistance = self.resistance
        p.requires_tool = self.requires_tool
        p.map_color = self.map_color
        p.ignited_by_lava = self.ignited_by_lava
        return p


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
    elif name == "netherStemProperties":   # (mapColor)
        strength(p, 2.0)
        p.map_color = args[0] if args else "NONE"
    elif name == "leavesProperties":       # (soundType)
        strength(p, 0.2)
        p.ignited_by_lava = True
        p.map_color = "PLANT"
    elif name == "shulkerBoxProperties":   # (mapColor)
        strength(p, 2.0)
        p.map_color = args[0] if args else "NONE"
    elif name == "pistonProperties":
        strength(p, 1.5)
        p.map_color = "STONE"
    elif name == "buttonProperties":
        strength(p, 0.5)
    elif name == "flowerPotProperties":
        strength(p, 0.0)                   # instabreak()
    elif name == "candleProperties":       # (color)
        strength(p, 0.1)
        p.map_color = args[0] if args else "NONE"
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
        return p
    if fn == "registerStainedGlass":
        p = Props()
        strength(p, 0.3)
        dye = re.fullmatch(r'DyeColor\.([A-Z_]+)', args[1].strip())
        p.map_color = DYE_TO_MAP.get(dye.group(1), "NONE") if dye else "NONE"
        return p
    if fn in ("registerStair", "registerLegacyStair", "registerSlab", "registerWall"):
        # Properties.ofFullCopy(base) / ofLegacyCopy(base) — both copy every
        # column we consume, so a stair (and 26.3's slab and wall helpers)
        # simply IS its base block.
        base = fields.get(args[1].strip())
        if base is None:
            raise SystemExit("%s: stair base %s unresolved" % (where, args[1]))
        return base.copy()
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


def parse_blocks(path=None):
    src = open(path or BLOCKS).read()
    fields = {}     # Java field name -> Props
    by_slug = {}    # registry slug   -> Props
    order = []

    for m in CALL_RE.finditer(src):
        fn = m.group(1)
        open_idx = m.end() - 1
        try:
            close = balanced(src, open_idx)
        except ValueError:
            continue
        args = split_args(src[open_idx + 1:close - 1])
        if not args:
            continue

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
        head = src.rfind("\n", 0, m.start())
        line = src[head + 1:m.start()]
        fm = re.search(r'([A-Z][A-Z_0-9]*)\s*=\s*$', line)
        if fm:
            fields[fm.group(1)] = p

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
TIER_TAGS = [("needs_diamond_tool", "Diamond"), ("needs_iron_tool", "Iron"),
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

        tool = "None"
        for tag, cpp in TOOL_TAGS:
            if vanilla in tools[tag]:
                tool = cpp
                break
        tier = "Wood"
        for tag, cpp in TIER_TAGS:
            if vanilla in tiers[tag]:
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
            f.write('        { "%s", %sf, %sf, %s, 0x%06X, %s, ToolType::%s, MiningTier::%s },\n'
                    % (slug, fmt(p.destroy_time), fmt(p.resistance),
                       "true" if p.requires_tool else "false",
                       MAP_COLORS.get(p.map_color, 0),
                       "true" if p.ignited_by_lava else "false",
                       tool, tier))
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
