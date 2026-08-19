#!/usr/bin/env python3
"""Generate every block's BlockState property set, exactly as vanilla declares it.

WHY THIS EXISTS
---------------
The engine used to hand-write its property tables in `BlockRegistry.cpp`'s
`InitBlockStates`, keyed off a `StateKind` enum classified from model names.
That approach has three problems that no amount of care fixes:

1. It only covers the families someone got round to writing. 701 of the 1086
   blocks this engine defines carry properties in vanilla; the hand-written
   table knew about a couple of dozen.

2. The value ORDER was guessed, and the guesses are wrong. MC's `facing` is
   north,south,west,east — not compass order. `half` is top,bottom — not
   bottom,top. A slab's `type` is top,bottom,double. The order decides the
   numeric state index, so a wrong order is a silently different state space.

3. Property IDENTITY is (name, value-set), not name. `type` means three
   different things (slab top/bottom/double, chest single/left/right, piston
   normal/sticky), and so do `age` (eight distinct ranges), `half`, `shape`,
   `mode`, `axis`, `level`, `distance`, and the four side properties. A
   string-keyed table cannot tell them apart. There are 120 distinct
   properties across 13 reused names.

So we read the property sets from data instead of writing them.

WHERE THE DATA COMES FROM
-------------------------
`tools/protocol-gen/node_modules/minecraft-data/.../blocks.json`, already
vendored for the protocol generator. Per block it gives `minStateId`,
`maxStateId`, `defaultState`, and a `states[]` array of
`{name, type, num_values, values[]}`.

Two properties of that data make it directly usable:

* `states[]` is already in MC's sorted-by-NAME order, which is the order
  `StateDefinition` uses (`ImmutableSortedMap.copyOf`) and therefore the order
  the state index is a mixed-radix number over — alphabetically-last property
  varying fastest.
* `values[]` is in MC's own `getPossibleValues()` order, which is what
  `Property.getInternalIndex` returns an index into.

Both are asserted below against `maxStateId - minStateId + 1` rather than
trusted.

THE DEFAULT STATE IS NOT INDEX 0
--------------------------------
This is the fact that breaks the engine's old storage assumption, so it is
carried explicitly as `defaultIndex`. MC's `StateDefinition.any()` takes the
FIRST value of every property, and `BooleanProperty` lists `true` before
`false` — so `any()` is usually a nonsense state (waterlogged, powered, lit
all true) and every block then calls `registerDefaultState` to move off it.
567 of the 1086 blocks here have a default that is not index 0; `oak_stairs`
defaults to index 11 of 80, `acacia_fence` to 31 of 32.

BLOCKS THAT POSTDATE THE VENDORED DATA
--------------------------------------
65 rows in BlockDefs.inc are newer than this copy of minecraft-data — the
1.21.9 Copper Age set, the shelves, the oxidised lightning rods.
`minecraft_code/decompiled_net/` IS that newer version, so the property sets
are in the repo; they just are not in `blocks.json`.

`Blocks.java` shows what class registers each one, and six of the eight
families reuse a class that an older block already uses:

    *copper_bars    -> IronBarsBlock          (same set as iron_bars)
    *copper_chain   -> ChainBlock             (chain)
    iron_chain      -> ChainBlock             (chain)
    *copper_chest   -> CopperChestBlock       (chest)
    *copper_lantern -> LanternBlock           (lantern)
    copper_torch    -> TorchBlock             (torch)
    copper_wall_torch -> WallTorchBlock       (wall_torch)
    *lightning_rod  -> LightningRodBlock      (lightning_rod)

Those are handled as ALIASES onto the older slug's upstream row, so their
value orders and — critically — their default indices come from the same
verified data as everything else rather than from arithmetic done by hand.

Only two families have no older analogue and are written out explicitly:
ShelfBlock (`builder.add(FACING, POWERED, SIDE_CHAIN_PART, WATERLOGGED)`,
ShelfBlock.java:86) and CopperGolemStatueBlock (`FACING, POSE, WATERLOGGED`,
CopperGolemStatueBlock.java:66). Even for those the DEFAULT INDEX is computed
below from the declared default values, not written down — hand-computing a
mixed-radix index is exactly the kind of silent error this file exists to
avoid.

An engine slug that resolves in NEITHER source is a hard failure. Emitting it
with no properties would be a silent degradation of precisely the kind this
codebase has already been bitten by, and a new block added to BlockDefs.inc
should stop the generator until someone says what its states are.

Output: src/common/world/block/GeneratedBlockStates.{hpp,cpp}
Run:    python3 tools/gen_block_states.py
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCDATA = os.path.join(
    ROOT, "tools", "protocol-gen", "node_modules", "minecraft-data",
    "minecraft-data", "data", "pc", "1.21.6", "blocks.json")
BLOCKDEFS = os.path.join(ROOT, "src", "common", "world", "block", "BlockDefs.inc")
BLOCKS_HPP = os.path.join(ROOT, "src", "common", "world", "block", "Blocks.hpp")
OUT_DIR = os.path.join(ROOT, "src", "common", "world", "block")

KIND = {"bool": 0, "int": 1, "enum": 2}

# ── Blocks newer than the vendored blocks.json ──────────────────────────────
# See the module docstring. Aliases borrow an older slug's upstream row because
# MC registers them with the same block class, so the property set, the value
# orders and the default are identical by construction.
ALIAS_SUFFIX = [
    # (suffix or exact name, slug whose upstream row to copy)
    ("copper_bars",    "iron_bars"),
    ("copper_chain",   "chain"),
    ("copper_chest",   "chest"),
    ("copper_lantern", "lantern"),
    ("lightning_rod",  "lightning_rod"),
]
ALIAS_EXACT = {
    "iron_chain":        "chain",
    "copper_torch":      "torch",
    "copper_wall_torch": "wall_torch",
}

# Not blocks: artifacts of the block-list generator. No properties, on purpose.
NOT_A_BLOCK = {"model_name", "set_spawn", "ominous_banner"}

# The two families with no older analogue. Values are MC's getPossibleValues()
# order; `default` is the value the class's registerDefaultState installs. The
# INDEX is computed from these, never written down.
EXPLICIT = {
    # ShelfBlock.java:86 + :66
    "*_shelf": [
        ("facing",      "enum", ["north", "south", "west", "east"],            "north"),
        ("powered",     "bool", ["true", "false"],                             "false"),
        ("side_chain",  "enum", ["unconnected", "right", "center", "left"],    "unconnected"),
        ("waterlogged", "bool", ["true", "false"],                             "false"),
    ],
    # CopperGolemStatueBlock.java:66 + :61
    "*copper_golem_statue": [
        ("copper_golem_pose", "enum", ["standing", "sitting", "running", "star"], "standing"),
        ("facing",            "enum", ["north", "south", "west", "east"],         "north"),
        ("waterlogged",       "bool", ["true", "false"],                          "false"),
    ],
}


# ── Property identity -> MC's own constant name ─────────────────────────────
# 13 property NAMES are reused with different value sets, so a name alone
# cannot identify a property (`type` means three different things). MC solves
# this by giving each identity its own constant in BlockStateProperties.java,
# and those names are transcribed here verbatim so the C++ constant a reader
# sees is the one they would find in the decompiled source.
#
# Keyed on the exact value tuple, never on the value COUNT: MODE_COMPARATOR
# and TEST_BLOCK_MODE both have four values and are different properties.
AMBIGUOUS_CONSTANT = {
    ("facing", ("north", "south", "west", "east")):               "HORIZONTAL_FACING",
    ("facing", ("north", "east", "south", "west", "up", "down")): "FACING",
    ("facing", ("down", "north", "south", "west", "east")):       "FACING_HOPPER",
    ("axis",   ("x", "y", "z")):                                  "AXIS",
    ("axis",   ("x", "z")):                                       "HORIZONTAL_AXIS",
    ("half",   ("top", "bottom")):                                "HALF",
    ("half",   ("upper", "lower")):                               "DOUBLE_BLOCK_HALF",
    ("type",   ("top", "bottom", "double")):                      "SLAB_TYPE",
    ("type",   ("single", "left", "right")):                      "CHEST_TYPE",
    ("type",   ("normal", "sticky")):                             "PISTON_TYPE",
    ("mode",   ("compare", "subtract")):                          "MODE_COMPARATOR",
    ("mode",   ("save", "load", "corner", "data")):               "STRUCTUREBLOCK_MODE",
    ("mode",   ("start", "log", "fail", "accept")):               "TEST_BLOCK_MODE",
    ("shape",  ("straight", "inner_left", "inner_right",
                "outer_left", "outer_right")):                    "STAIRS_SHAPE",
    ("distance", tuple(str(i) for i in range(1, 8))):             "DISTANCE",
    ("distance", tuple(str(i) for i in range(0, 8))):             "STABILITY_DISTANCE",
    ("level",  ("1", "2", "3")):                                  "LEVEL_CAULDRON",
    ("level",  tuple(str(i) for i in range(0, 9))):               "LEVEL_COMPOSTER",
    ("level",  tuple(str(i) for i in range(1, 9))):               "LEVEL_FLOWING",
    ("level",  tuple(str(i) for i in range(0, 16))):              "LEVEL",
}
# The four side properties share one pattern across three value sets.
for _side in ("north", "east", "south", "west"):
    AMBIGUOUS_CONSTANT[(_side, ("true", "false"))] = _side.upper()
    AMBIGUOUS_CONSTANT[(_side, ("none", "low", "tall"))] = _side.upper() + "_WALL"
    AMBIGUOUS_CONSTANT[(_side, ("up", "side", "none"))] = _side.upper() + "_REDSTONE"
# `age` and the rail shapes disambiguate on size alone.
for _hi in (1, 2, 3, 4, 5, 7, 15, 25):
    AMBIGUOUS_CONSTANT[("age", tuple(str(i) for i in range(_hi + 1)))] = f"AGE_{_hi}"
_RAIL = ("north_south", "east_west", "ascending_east", "ascending_west",
         "ascending_north", "ascending_south")
AMBIGUOUS_CONSTANT[("shape", _RAIL)] = "RAIL_SHAPE_STRAIGHT"
AMBIGUOUS_CONSTANT[("shape", _RAIL + ("south_east", "south_west",
                                      "north_west", "north_east"))] = "RAIL_SHAPE"


def constant_name(name, values, ambiguous_names):
    """MC's BlockStateProperties constant for this (name, value-set)."""
    if name not in ambiguous_names:
        return name.upper()
    key = (name, tuple(values))
    if key not in AMBIGUOUS_CONSTANT:
        raise SystemExit(
            f"property {name!r} with values {list(values)} shares its name with another\n"
            f"  property but has no entry in AMBIGUOUS_CONSTANT. Auto-naming it would\n"
            f"  produce two constants a reader cannot tell apart — add its\n"
            f"  BlockStateProperties.java constant name to the map.")
    return AMBIGUOUS_CONSTANT[key]


def explicit_for(slug):
    """The EXPLICIT entry whose pattern this slug matches, or None."""
    for pattern, props in EXPLICIT.items():
        bare = pattern.lstrip("*")
        if slug.endswith(bare):
            return props
    return None


def alias_for(slug):
    """The older slug whose upstream row this one should copy, or None."""
    if slug in ALIAS_EXACT:
        return ALIAS_EXACT[slug]
    for suffix, target in ALIAS_SUFFIX:
        if slug.endswith(suffix):
            return target
    return None


def load_engine_slugs():
    """Column 2 of BlockDefs.inc, in declaration order."""
    src = open(BLOCKDEFS).read()
    return [m.group(2) for m in re.finditer(r'BLOCK_DEF\(\s*(\w+)\s*,\s*"([^"]+)"', src)]


def count_manual_block_ids():
    """Enumerators Blocks.hpp declares BEYOND the BlockDefs.inc include.

    These are the synthetic variants — *SlabTop, *SlabDouble, SnowGrass, the
    double-plant tops, leaf_litter_N — that spend a whole BlockID on one
    property value. They carry no properties, so each is exactly one state, but
    they are still part of the global id space and kBlockStateCount has to
    include them or the palette width is derived from the wrong number.

    They are slated to collapse back into real properties, at which point this
    returns 0 on its own.
    """
    src = open(BLOCKS_HPP).read()
    body = src.split("#undef BLOCK_DEF", 1)[1].split("Count", 1)[0]
    return len(re.findall(r'^\s*([A-Za-z]\w*)\s*,', body, re.M))


def property_values(state):
    """MC's getPossibleValues() for one property, as strings.

    A bool has no `values` array in the data because MC's BooleanProperty is a
    fixed [true, false] — and the order matters, so it is spelled out here
    rather than inferred.
    """
    if state["type"] == "bool":
        return ["true", "false"]
    vals = state.get("values")
    if not vals:
        raise SystemExit(f"property {state['name']!r} has no values array")
    return [str(v) for v in vals]


def build_explicit(intern_property, slug, props):
    """Intern an EXPLICIT property list and COMPUTE its default index.

    The index is derived here rather than written into the table because a
    mixed-radix digit-by-stride sum is the single easiest thing to get wrong by
    hand, and getting it wrong produces a block that renders and behaves as
    some other perfectly valid state of itself.
    """
    names = [p[0] for p in props]
    if names != sorted(names):
        raise SystemExit(f"{slug}: explicit property list {names} is not sorted by name; "
                         "MC's StateDefinition uses an ImmutableSortedMap and the sort "
                         "order IS the state index layout")

    refs, radices, digits = [], [], []
    for name, kind, values, default in props:
        if default not in values:
            raise SystemExit(f"{slug}.{name}: default {default!r} not in {values}")
        refs.append(intern_property(name, KIND[kind], values))
        radices.append(len(values))
        digits.append(values.index(default))

    # Odometer: last property varies fastest (MC's flatMap accumulation order).
    count, default_index, stride = 1, 0, 1
    for r, d in zip(reversed(radices), reversed(digits)):
        default_index += d * stride
        stride *= r
        count *= r
    return refs, count, default_index


def main():
    if not os.path.exists(MCDATA):
        raise SystemExit(f"missing {MCDATA} — run npm install under tools/protocol-gen")

    upstream = {b["name"]: b for b in json.load(open(MCDATA))}
    slugs = load_engine_slugs()

    # ── Dedupe properties by (name, kind, values) — MC's property identity ──
    prop_index = {}          # key -> index into the emitted property table
    prop_rows = []           # (name, kind, values)
    value_pool = []          # flat string pool
    value_begin = {}         # tuple(values) -> offset into value_pool

    def intern_property(name, kind, values):
        key = (name, kind, tuple(values))
        if key in prop_index:
            return prop_index[key]
        vt = tuple(values)
        if vt not in value_begin:
            value_begin[vt] = len(value_pool)
            value_pool.extend(values)
        idx = len(prop_rows)
        prop_index[key] = idx
        prop_rows.append((name, kind, vt))
        return idx

    block_rows = []          # (slug, [propIdx...], defaultIndex, stateCount)
    aliased = []
    explicit = []
    total_states = 0

    for slug in slugs:
        if slug in NOT_A_BLOCK:
            total_states += 1
            continue

        b = upstream.get(slug)
        source = "upstream"

        if b is None:
            target = alias_for(slug)
            if target is not None:
                b = upstream.get(target)
                if b is None:
                    raise SystemExit(f"{slug} aliases {target!r}, which is not upstream either")
                source = f"alias of {target}"
                aliased.append(slug)
            else:
                props = explicit_for(slug)
                if props is None:
                    # Silently emitting a propertyless block here is how a real
                    # state space quietly becomes a wrong one. Stop instead.
                    raise SystemExit(
                        f"{slug}: no upstream row, no alias and no explicit property set.\n"
                        f"  Add it to ALIAS_SUFFIX/ALIAS_EXACT if MC registers it with a class\n"
                        f"  an older block already uses, or to EXPLICIT with its\n"
                        f"  createBlockStateDefinition list from minecraft_code/.")
                refs, count, default_index = build_explicit(intern_property, slug, props)
                block_rows.append((slug, refs, default_index, count))
                total_states += count
                explicit.append(slug)
                continue

        states = b.get("states") or []
        if not states:
            total_states += 1
            continue

        refs = []
        count = 1
        for st in states:
            values = property_values(st)
            if len(values) < 2:
                raise SystemExit(f"{slug}.{st['name']} has {len(values)} value(s); "
                                 "MC forbids single-valued properties")
            refs.append(intern_property(st["name"], KIND[st["type"]], values))
            count *= len(values)

        # Self-check: the cartesian product must equal the id range upstream
        # allocated. If this trips, either the value lists or the property list
        # is not what MC enumerated, and every state index would be wrong.
        span = b["maxStateId"] - b["minStateId"] + 1
        if count != span:
            raise SystemExit(f"{slug}: product of value counts {count} != "
                             f"upstream state span {span} ({source})")

        default_index = b["defaultState"] - b["minStateId"]
        if not (0 <= default_index < count):
            raise SystemExit(f"{slug}: default index {default_index} out of range {count}")

        block_rows.append((slug, refs, default_index, count))
        total_states += count

    import collections
    name_counts = collections.Counter(n for n, _k, _v in prop_rows)
    ambiguous = {n for n, c in name_counts.items() if c > 1}
    const_names = [constant_name(n, v, ambiguous) for n, _k, v in prop_rows]
    dupes = [c for c, n in collections.Counter(const_names).items() if n > 1]
    if dupes:
        raise SystemExit(f"two properties resolved to the same constant name: {dupes}")

    manual = count_manual_block_ids()
    total_states += manual
    write_header(prop_rows, const_names, total_states, len(block_rows))
    write_source(prop_rows, value_pool, value_begin, block_rows)

    stateful = len(block_rows)
    print(f"gen_block_states: {len(slugs)} blocks, {stateful} with properties, "
          f"{total_states} states total, {len(prop_rows)} distinct properties, "
          f"{len(value_pool)} pooled value strings")
    nondefault = sum(1 for _, _, d, _ in block_rows if d != 0)
    print(f"                  {nondefault} blocks whose default state is not index 0")
    aliased_rows = sum(1 for s_, _, _, _ in block_rows if s_ in set(aliased))
    print(f"                  {manual} synthetic BlockIDs from Blocks.hpp "
          f"(1 state each, folded into the total)")
    print(f"                  supplement: {len(aliased)} aliased "
          f"({aliased_rows} with properties), {len(explicit)} explicit, "
          f"{len(NOT_A_BLOCK)} non-blocks skipped")


HEADER = '''// GENERATED by tools/gen_block_states.py — DO NOT EDIT BY HAND.
//
// Every block's BlockState property set, taken from vanilla rather than
// guessed. See the script header for why this is generated: MC's property
// VALUE ORDER decides the state index, and MC's property IDENTITY is
// (name, value-set) rather than name — `type` alone means three different
// things.
//
// Layout mirrors the other generated tables here (kBlockShapeTable,
// kWaterloggable): flat arrays with (begin, count) windows, keyed on the
// vanilla registry slug, which is column 2 of BlockDefs.inc.
//
// Properties are DEDUPED across blocks, exactly as vanilla shares one
// `BlockStateProperties.FACING` object between every block that uses it —
// which is what makes identity comparison valid.
//
// A block absent from kBlockStates has no properties and exactly one state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Game {

    // MC's three Property subclasses.
    enum class GeneratedPropertyKind : uint8_t { Bool = 0, Int = 1, Enum = 2 };

    struct GeneratedPropertyRow {
        const char* name;
        uint8_t     kind;         // GeneratedPropertyKind
        uint16_t    valueBegin;   // window into kPropertyValues
        uint16_t    valueCount;   // MC getPossibleValues().size(), always >= 2
    };

    struct GeneratedBlockStateRow {
        const char* slug;
        uint16_t    propBegin;    // window into kBlockPropertyRefs
        uint16_t    propCount;
        // Index of the block's default state within its OWN state list. NOT
        // always 0 — MC's any() takes the first value of every property and
        // BooleanProperty lists true first, so most blocks register a default
        // somewhere else entirely. 567 of these are non-zero.
        uint16_t    defaultIndex;
        // Product of the property value counts. Carried so the loader can
        // assert its own arithmetic against what vanilla allocated.
        uint16_t    stateCount;
    };

    // Value-name pool, shared by properties with identical value lists.
    extern const char* const            kPropertyValues[];
    extern const size_t                 kPropertyValueCount;

    // The 118 distinct (name, kind, values) properties.
    extern const GeneratedPropertyRow   kProperties[];
    extern const size_t                 kPropertyCount;

    // Flat property-index windows, one run per block, in MC's sorted-by-name
    // order — which is the order the state index is a mixed-radix number over,
    // with the LAST property varying fastest.
    extern const uint16_t               kBlockPropertyRefs[];
    extern const size_t                 kBlockPropertyRefCount;

    extern const GeneratedBlockStateRow kBlockStates[];
    extern const size_t                 kBlockStateRowCount;

__PROPERTY_IDS__

__TOTALS__

} // namespace Game
'''


def write_header(prop_rows, const_names, total_states, stateful_blocks):
    bits = 0
    while (1 << bits) < total_states:
        bits += 1

    ids = ['    // MC BlockStateProperties constant names, verbatim. A property is',
           '    // identified by (name, value-set), not by name — `type` alone is three',
           '    // different properties — so the 13 reused names carry MC\'s own',
           '    // disambiguating constant (SLAB_TYPE / CHEST_TYPE / PISTON_TYPE, …).',
           '    enum class PropertyId : uint16_t {']
    for i, (c, (name, _kind, values)) in enumerate(zip(const_names, prop_rows)):
        preview = ",".join(values[:3]) + ("…" if len(values) > 3 else "")
        ids.append(f'        {c} = {i},'.ljust(42) + f'// "{name}" = {preview}')
    ids.append(f'        Count = {len(const_names)}')
    ids.append('    };')

    totals = [
        '    // Total distinct block states across every block this engine defines —',
        '    // the size of the global state id space, and MC\'s',
        '    // Block.BLOCK_STATE_REGISTRY.size(). Generated, never a literal: the',
        '    // palette width is derived from it and must move when it does.',
        f'    inline constexpr uint32_t kBlockStateCount = {total_states};',
        f'    inline constexpr int      kBlockStateBits  = {bits};',
        f'    inline constexpr size_t   kStatefulBlockCount = {stateful_blocks};',
        '',
        '    static_assert(kBlockStateCount <= (1u << kBlockStateBits),',
        '                  "state id space does not fit the generated palette width");',
        '    static_assert(kBlockStateCount > (1u << (kBlockStateBits - 1)),',
        '                  "generated palette width is wider than the state space needs");',
    ]

    body = HEADER.replace("__PROPERTY_IDS__", "\n".join(ids))
    body = body.replace("__TOTALS__", "\n".join(totals))
    with open(os.path.join(OUT_DIR, "GeneratedBlockStates.hpp"), "w") as f:
        f.write(body)


def write_source(prop_rows, value_pool, value_begin, block_rows):
    out = ['// GENERATED by tools/gen_block_states.py — DO NOT EDIT BY HAND.',
           '#include "GeneratedBlockStates.hpp"',
           '',
           'namespace Game {',
           '']

    out.append('    const char* const kPropertyValues[] = {')
    for i in range(0, len(value_pool), 8):
        out.append('        ' + ' '.join(f'"{v}",' for v in value_pool[i:i + 8]))
    out.append('    };')
    out.append(f'    const size_t kPropertyValueCount = {len(value_pool)};')
    out.append('')

    kind_name = {0: "Bool", 1: "Int ", 2: "Enum"}
    out.append('    const GeneratedPropertyRow kProperties[] = {')
    for name, kind, values in prop_rows:
        begin = value_begin[values]
        preview = ",".join(values[:4]) + ("…" if len(values) > 4 else "")
        out.append(f'        {{ "{name}", {kind}, {begin}, {len(values)} }},'
                   f'  // {kind_name[kind]} {preview}')
    out.append('    };')
    out.append(f'    const size_t kPropertyCount = {len(prop_rows)};')
    out.append('')

    refs_flat = []
    windows = {}
    for slug, refs, _, _ in block_rows:
        windows[slug] = len(refs_flat)
        refs_flat.extend(refs)

    out.append('    const uint16_t kBlockPropertyRefs[] = {')
    for i in range(0, len(refs_flat), 16):
        out.append('        ' + ' '.join(f'{v},' for v in refs_flat[i:i + 16]))
    out.append('    };')
    out.append(f'    const size_t kBlockPropertyRefCount = {len(refs_flat)};')
    out.append('')

    out.append('    const GeneratedBlockStateRow kBlockStates[] = {')
    for slug, refs, default_index, count in block_rows:
        out.append(f'        {{ "{slug}", {windows[slug]}, {len(refs)}, '
                   f'{default_index}, {count} }},')
    out.append('    };')
    out.append(f'    const size_t kBlockStateRowCount = {len(block_rows)};')
    out.append('')
    out.append('} // namespace Game')
    out.append('')

    with open(os.path.join(OUT_DIR, "GeneratedBlockStates.cpp"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
