#!/usr/bin/env python3
"""Generate src/common/world/block/GeneratedBlockLight.inc from MC's Blocks.java.

The light engine (src/common/world/lighting/) needs, per block STATE, what
MC's BlockBehaviour computes in BlockStateBase.initCache:

  lightEmission            Properties.lightLevel(...)  — a function of the
                           state for ~20 blocks (lit furnaces, candles,
                           sea pickles, respawn anchors, the light block...)
  canOcclude               false after Properties.noOcclusion() / noCollision()
  propagatesSkylightDown   Block.propagatesSkylightDown, overridden by ~15
                           classes (glass, walls, vegetation, liquids...)
  getLightDampening        Block.getLightDampening (leaves 1, tinted glass 15)
  useShapeForLightOcclusion  stairs, slabs, snow layers, farmland, paths...

The per-state evaluation happens in C++ (BlockLightProperties.cpp), which has
the shapes and state properties; this script only carries the RULE per block,
keyed on the registry slug (column 2 of BlockDefs.inc):

  BLOCK_LIGHT("slug", EmissionRule, value, canOcclude, SkyDownRule, dampening, ShapeRule)

Engine blocks resolve through gen_block_hardness.SLUG_ALIASES (a Hush lantern
behaves like the vanilla lantern it is modelled on). Engine blocks that GLOW
beyond what their alias does get their emission from the hand-maintained
src/common/world/block/EngineBlockLight.inc, which overrides this table at
load — edit that one, not this output.

    python3 tools/gen_block_light.py
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_block_hardness as hard  # noqa: E402

BLOCKS  = hard.BLOCKS
BLOCKS2 = hard.BLOCKS2
BLOCK_DIR = "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/block"
OUT = "src/common/world/block/GeneratedBlockLight.inc"


# ── Class-level overrides (BlockBehaviour subclasses) ─────────────────────────
#
# Transcribed from the decompile; the scan in check_overrides() fails the run
# when a class defines one of these methods and is missing here, so a snapshot
# bump cannot silently drop one.

DAMPENING = {                 # getLightDampening
    "LeavesBlock": 1,
    "TintedGlassBlock": 15,
}
SKYDOWN = {                   # propagatesSkylightDown
    "BambooStalkBlock": "True",
    "BarrierBlock": "FluidEmpty",
    "CrossCollisionBlock": "NotWaterlogged",
    "GlowLichenBlock": "FluidEmpty",
    "HangingMossBlock": "True",
    "LightBlock": "FluidEmpty",
    "LiquidBlock": "False",
    "MossyCarpetBlock": "True",
    "PipeBlock": "False",
    "ShulkerBoxBlock": "False",
    "TintedGlassBlock": "False",
    "TransparentBlock": "True",
    "VegetationBlock": "FluidEmpty",
    "VineBlock": "True",
    "WallBlock": "NotWaterlogged",
}
SHAPE = {                     # useShapeForLightOcclusion
    "DaylightDetectorBlock": "True",
    "EnchantingTableBlock": "True",
    "EndPortalFrameBlock": "True",
    "FarmlandBlock": "True",
    "LecternBlock": "True",
    "PathBlock": "True",
    "SculkSensorBlock": "True",
    "ShelfBlock": "True",
    "SculkShriekerBlock": "True",
    "SnowLayerBlock": "True",
    "StairBlock": "True",
    "StonecutterBlock": "True",
    "SlabBlock": "NotDouble",
    "PistonHeadBlock": "True",
    "PistonBaseBlock": "Extended",
}


def load_hierarchy():
    """class name -> superclass name, over every block class in the decompile."""
    parent = {}
    for root, _dirs, files in os.walk(BLOCK_DIR):
        for fn in files:
            if not fn.endswith(".java"):
                continue
            src = open(os.path.join(root, fn)).read()
            for m in re.finditer(r'\bclass\s+([A-Z][A-Za-z0-9_]*)\s*(?:<[^>{]*>)?\s+extends\s+([A-Z][A-Za-z0-9_]*)', src):
                parent.setdefault(m.group(1), m.group(2))
    return parent


def check_overrides():
    """Every class that defines one of the three methods must be in the maps."""
    missing = []
    for root, _dirs, files in os.walk(BLOCK_DIR):
        for fn in files:
            if not fn.endswith(".java"):
                continue
            cls = fn[:-5]
            src = open(os.path.join(root, fn)).read()
            if re.search(r'protected int getLightDampening\s*\(', src) and cls not in DAMPENING \
                    and cls != "BlockBehaviour":
                missing.append(cls + ".getLightDampening")
            if re.search(r'protected boolean propagatesSkylightDown\s*\(', src) and cls not in SKYDOWN \
                    and cls != "BlockBehaviour":
                missing.append(cls + ".propagatesSkylightDown")
            if re.search(r'protected boolean useShapeForLightOcclusion\s*\(', src) and cls not in SHAPE \
                    and cls != "BlockBehaviour":
                missing.append(cls + ".useShapeForLightOcclusion")
    if missing:
        raise SystemExit("light overrides not transcribed: " + ", ".join(missing))


def resolve_class_rule(cls, table, parent, default):
    seen = set()
    while cls and cls not in seen:
        seen.add(cls)
        if cls in table:
            return table[cls]
        cls = parent.get(cls)
    return default


# ── Properties: only the light columns ────────────────────────────────────────

class LightProps:
    __slots__ = ("rule", "value", "can_occlude")

    def __init__(self):
        self.rule = "None"
        self.value = 0
        self.can_occlude = True

    def copy(self):
        p = LightProps()
        p.rule, p.value, p.can_occlude = self.rule, self.value, self.can_occlude
        return p


def parse_light_level(arg):
    """The argument of .lightLevel(...) -> (rule, value)."""
    a = re.sub(r'\s+', ' ', arg)
    m = re.search(r'litBlockEmission\((\d+)\)', a)
    if m:
        return "Lit", int(m.group(1))
    if "litBlockEmission(" in a:
        return "Lit", -1          # copper bulb: resolved per weather stage below
    if "CandleBlock.LIGHT_EMISSION" in a:
        return "Candles", 3
    if "LightBlock.LIGHT_EMISSION" in a:
        return "LightLevel", 0
    m = re.search(r'GlowLichenBlock\.emission\((\d+)\)', a)
    if m:
        return "AnyFace", int(m.group(1))
    m = re.search(r'CaveVines\.emission\((\d+)\)', a)
    if m:
        return "Berries", int(m.group(1))
    if "SeaPickleBlock" in a:
        return "SeaPickle", 3
    if "RespawnAnchorBlock.getScaledChargeLevel" in a:
        return "RespawnAnchor", 15
    if "TrialSpawnerState" in a:
        return "TrialSpawner", 0
    if "VaultState" in a:
        return "Vault", 0
    m = re.fullmatch(r'\s*\(\s*\w+\s*\)\s*->\s*\{?\s*return\s+(\d+)\s*;?\s*\}?\s*', a)
    if m:
        return "Constant", int(m.group(1))
    m = re.fullmatch(r'\s*\(\s*\w+\s*\)\s*->\s*(\d+)\s*', a)
    if m:
        return "Constant", int(m.group(1))
    raise SystemExit("unrecognised lightLevel expression: " + a)


def helper_light(name):
    """Local Properties factories in Blocks.java (see gen_block_hardness.helper_props)."""
    p = LightProps()
    if name in ("leavesProperties", "shulkerBoxProperties", "flowerPotProperties",
                "candleProperties"):
        p.can_occlude = False
    if name == "candleProperties":
        p.rule, p.value = "Candles", 3
    if name == "buttonProperties":
        p.can_occlude = False                 # noCollision()
    if name in ("logProperties", "netherStemProperties", "pistonProperties",
                "wallVariant", "leavesProperties", "shulkerBoxProperties",
                "flowerPotProperties", "candleProperties", "buttonProperties"):
        return p
    return None


def eval_light(expr, fields):
    expr = expr.strip()
    p = None
    m = re.match(r'BlockBehaviour\.Properties\.(of|ofFullCopy|ofLegacyCopy)\s*\(', expr)
    if m:
        open_idx = m.end() - 1
        close = hard.balanced(expr, open_idx)
        inner = expr[open_idx + 1:close - 1].strip()
        if m.group(1) == "of":
            p = LightProps()
        else:
            # Both copies carry lightEmission and canOcclude
            # (BlockBehaviour.Properties.ofLegacyCopy).
            base = fields.get(inner)
            p = base.copy() if base else LightProps()
        rest = expr[close:]
    else:
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', expr)
        if not m:
            return None
        open_idx = m.end() - 1
        close = hard.balanced(expr, open_idx)
        p = helper_light(m.group(1))
        if p is None:
            return None
        rest = expr[close:]
    for name, args in hard.chain_calls(rest):
        if name == "lightLevel":
            p.rule, p.value = parse_light_level(args)
        elif name in ("noOcclusion", "noCollision"):
            p.can_occlude = False
    return p


def factory_class(fn, args):
    """The Block subclass a registration instantiates."""
    if fn in ("registerStair", "registerLegacyStair"):
        return "StairBlock"
    if fn == "registerSlab":
        return "SlabBlock"
    if fn == "registerWall":
        return "WallBlock"
    if fn == "registerBed":
        return "BedBlock"
    if fn == "registerStainedGlass":
        return "StainedGlassBlock"
    if len(args) >= 3:
        f = args[1]
        m = re.search(r'([A-Z][A-Za-z0-9_]*)::new', f)
        if m:
            return m.group(1)
        m = re.search(r'\bnew\s+([A-Z][A-Za-z0-9_]*)\s*\(', f)
        if m:
            return m.group(1)
    return "Block"


def wrapper_light(fn, args, fields):
    if fn == "registerBed":
        p = LightProps()
        p.can_occlude = False
        return p
    if fn == "registerStainedGlass":
        p = LightProps()
        p.can_occlude = False
        return p
    if fn in ("registerStair", "registerLegacyStair", "registerSlab", "registerWall"):
        base = fields.get(args[1].strip())
        return base.copy() if base else LightProps()
    return None


COPPER_BULB = {"": 15, "exposed_": 12, "weathered_": 8, "oxidized_": 4}


def parse(path):
    src = open(path).read()
    fields, by_slug = {}, {}
    # hard.registrations: the written-out register* calls plus 26.3's
    # ColorCollection families expanded per colour, in source order.
    for _offset, fn, args, field in hard.registrations(src):
        slug = hard.slug_of(args[0])
        if slug is None:
            continue
        if fn == "register":
            p = eval_light(args[-1], fields)
            if p is None:
                continue
        else:
            p = wrapper_light(fn, args, fields)
            if p is None:
                continue
        cls = factory_class(fn, args)
        if field:
            fields[field] = p
        by_slug[slug] = (p, cls)

    # WeatheringCopperBlocks / WeatheringCopperCollection: eight blocks off one
    # call, the supplier's Properties shared (copper bulbs vary the light by
    # weather stage, transcribed in COPPER_BULB).
    for rx in (hard.WEATHERING_RE, re.compile(r'\bWeatheringCopperCollection\.registerBlocks\s*\(')):
        for m in rx.finditer(src):
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
            body = body.strip()
            if body.startswith("{"):
                rm = re.search(r'return\s+(.*?);\s*\}?\s*$', body, re.S)
                body = rm.group(1) if rm else body
                # `var10000 = BlockBehaviour.Properties.of()...;` then
                # `return var10000.lightLevel(...)`: stitch the two.
                vm = re.search(r'var10000\s*=\s*(BlockBehaviour\.Properties\..*?);', supplier, re.S)
                if vm and body.startswith("var10000"):
                    body = vm.group(1) + body[len("var10000"):]
            p = eval_light(body, fields)
            if p is None:
                p = LightProps()
            cls = "Block"
            for a in args[1:-1]:
                cm = re.search(r'([A-Z][A-Za-z0-9_]*)::new', a)
                if cm:
                    cls = cm.group(1)
            for prefix in hard.WEATHERING_PREFIXES:
                q = p.copy()
                if base == "copper_bulb":
                    q.rule, q.value = "Lit", COPPER_BULB[prefix.replace("waxed_", "")]
                by_slug[prefix + base] = (q, cls)
    return by_slug


def main():
    check_overrides()
    parent = load_hierarchy()
    by_slug = parse(BLOCKS)
    print("parsed %d registrations from %s" % (len(by_slug), BLOCKS))
    if os.path.exists(BLOCKS2):
        added = 0
        for slug, v in parse(BLOCKS2).items():
            if slug not in by_slug:
                by_slug[slug] = v
                added += 1
        print("  + %d from 26.3's Blocks.java (gap fill)" % added)

    engine = hard.load_engine_slugs()
    rows, missing = [], []
    for slug in sorted(set(engine)):
        vanilla = hard.SLUG_ALIASES.get(slug, slug)
        entry = by_slug.get(vanilla)
        if entry is None:
            missing.append(slug)
            continue
        p, cls = entry
        if p.rule == "Lit" and p.value < 0:
            raise SystemExit("unresolved litBlockEmission for " + slug)
        damp = resolve_class_rule(cls, DAMPENING, parent, -1)
        sky = resolve_class_rule(cls, SKYDOWN, parent, "Default")
        shape = resolve_class_rule(cls, SHAPE, parent, "False")
        rows.append((slug, p, damp, sky, shape, cls))

    print("matched %d / %d engine blocks (%d fall back to the runtime defaults)"
          % (len(rows), len(set(engine)), len(missing)))
    if missing:
        print("  unmatched: " + ", ".join(missing[:16]) + (" ..." if len(missing) > 16 else ""))

    with open(OUT, "w") as f:
        f.write("// GENERATED by tools/gen_block_light.py — DO NOT EDIT BY HAND.\n")
        f.write("// Engine-only glow values live in EngineBlockLight.inc (hand-maintained).\n")
        f.write("//\n")
        f.write("// BLOCK_LIGHT(slug, EmissionRule, value, canOcclude, SkyDownRule, dampening, ShapeRule)\n")
        f.write("//   EmissionRule/value: MC Properties.lightLevel; SkyDownRule: propagatesSkylightDown;\n")
        f.write("//   dampening: getLightDampening override (-1 = MC default); ShapeRule:\n")
        f.write("//   useShapeForLightOcclusion. Sorted by slug.\n")
        for slug, p, damp, sky, shape, cls in rows:
            f.write('BLOCK_LIGHT("%s", %s, %d, %s, %s, %d, %s)  // %s\n'
                    % (slug, p.rule, p.value, "true" if p.can_occlude else "false",
                       sky, damp, shape, cls))
    print("wrote %s (%d rows)" % (OUT, len(rows)))


if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    main()
