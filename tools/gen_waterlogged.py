#!/usr/bin/env python3
"""Extract MC's waterlogging tables from the decompiled source.

WHY THIS EXISTS
---------------
Two different mechanisms in MC put water inside a non-water block, and the
engine needs both:

1. The `waterlogged` BlockState property, declared by every block whose class
   chain reaches `SimpleWaterloggedBlock` (stairs, slabs, fences, walls,
   trapdoors, signs, leaves, ladders, chests, candles, rails, coral, ...).
   `getFluidState` on those returns WATER when the property is set.

2. Blocks with no property at all whose `getFluidState` returns
   `Fluids.WATER.getSource(false)` UNCONDITIONALLY — kelp, kelp_plant,
   seagrass, tall_seagrass, bubble_column. These are always water-filled and
   there is nothing to store per voxel.

Hardcoding either list by name pattern is a trap: `LeavesBlock` is
waterloggable and `oak_planks` is not, but `IronBarsBlock` is and
`GlassBlock` is not, and the pattern that separates them is the Java class
hierarchy rather than the name. So we read the hierarchy.

WHAT IS RESOLVED
----------------
* Every .java in world/level/block is parsed for its `extends`/`implements`
  clause, and the transitive closure of `SimpleWaterloggedBlock` is taken.
* `Blocks.java` maps registry slug -> constructor class (including the
  `registerStair` / `registerLegacyStair` helpers), which turns the class set
  into a slug set.
* The default value matters and is NOT always false: the coral families and
  sea pickle call `registerDefaultState(... setValue(WATERLOGGED, true))`, and
  conduit does too. The engine's state storage requires state index 0 to be
  the block's default state, so the generated `defaultWaterlogged` flag is
  what decides the value ORDER when the property is declared.

Output: src/common/world/block/GeneratedWaterlogged.{hpp,cpp}
Run:    python3 tools/gen_waterlogged.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLOCK_DIR = os.path.join(ROOT, "minecraft_code_26.1-snapshot-1/decompiled_net/minecraft/world/level/block")
BLOCKS_JAVA = os.path.join(BLOCK_DIR, "Blocks.java")
# 26.3 gap fill: classes and registrations 26.1 lacks (poplar, sulfur, …).
BLOCK_DIR2 = os.path.join(ROOT, "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/block")
BLOCKS_JAVA2 = os.path.join(BLOCK_DIR2, "Blocks.java")


def class_files():
    """(class name, path) for every block class: 26.1's, plus 26.3's for the
    classes 26.1 does not have."""
    out = {}
    for fn in os.listdir(BLOCK_DIR):
        if fn.endswith(".java"):
            out[fn[:-5]] = os.path.join(BLOCK_DIR, fn)
    if os.path.isdir(BLOCK_DIR2):
        for fn in os.listdir(BLOCK_DIR2):
            if fn.endswith(".java"):
                out.setdefault(fn[:-5], os.path.join(BLOCK_DIR2, fn))
    return sorted(out.items())
BLOCK_DEFS = os.path.join(ROOT, "src/common/world/block/BlockDefs.inc")
OUT_HPP = os.path.join(ROOT, "src/common/world/block/GeneratedWaterlogged.hpp")
OUT_CPP = os.path.join(ROOT, "src/common/world/block/GeneratedWaterlogged.cpp")

WATERLOGGED_IFACE = "SimpleWaterloggedBlock"


def parse_hierarchy():
    """class name -> [direct supertypes], for every block class."""
    info = {}
    for cls, path in class_files():
        src = open(path, errors="ignore").read()
        m = re.search(
            r"\n(?:public |abstract |final |)*(?:class|interface)\s+"
            + re.escape(cls)
            + r"\b([^{]*)\{",
            src,
        )
        parents = []
        if m:
            # Strip generics (twice, for one level of nesting) so the split on
            # ',' cannot land inside a type argument list.
            tail = re.sub(r"<[^<>]*>", "", re.sub(r"<[^<>]*>", "", m.group(1)))
            for kw in ("extends", "implements"):
                mm = re.search(
                    kw + r"\s+([^{]*?)(?=\s+(?:extends|implements)\s|$)", tail
                )
                if mm:
                    parents += [
                        p.strip().split(".")[-1]
                        for p in mm.group(1).split(",")
                        if p.strip()
                    ]
        info[cls] = parents
    return info


def closure(info, roots):
    """Classes whose supertype chain reaches any of `roots`."""
    memo = {}

    def hit(c, seen):
        if c in roots:
            return True
        if c in memo:
            return memo[c]
        if c in seen or c not in info:
            return False
        seen.add(c)
        memo[c] = False           # break cycles pessimistically
        r = any(hit(p, seen) for p in info[c])
        memo[c] = r
        return r

    return {c for c in info if hit(c, set())}


def default_true_classes():
    """Classes whose registerDefaultState sets WATERLOGGED true."""
    out = set()
    for cls, path in class_files():
        src = open(path, errors="ignore").read()
        for m in re.finditer(r"registerDefaultState\((.*?)\);", src, re.S):
            if re.search(r"setValue\(\s*WATERLOGGED\s*,\s*true\s*\)", m.group(1)):
                out.add(cls)
                break
    return out


def unconditional_water_classes():
    """Classes whose getFluidState returns WATER with no state test."""
    out = set()
    for fn in os.listdir(BLOCK_DIR):
        if not fn.endswith(".java"):
            continue
        src = open(os.path.join(BLOCK_DIR, fn), errors="ignore").read()
        if re.search(
            r"FluidState\s+getFluidState\(final BlockState \w+\)\s*\{\s*"
            r"return\s+Fluids\.WATER\.getSource\(false\);\s*\}",
            src,
        ):
            out.add(fn[:-5])
    return out


def parse_registry():
    """registry slug -> constructor class, from Blocks.java."""
    src = open(BLOCKS_JAVA, errors="ignore").read()
    names = {}
    for m in re.finditer(
        r'register\(\s*"([a-z0-9_]+)"\s*,\s*'
        r"(?:\(p\w*\)\s*->\s*new\s+(\w+)|(\w+)::new)",
        src,
    ):
        names[m.group(1)] = m.group(2) or m.group(3)
    # register("id", Properties...) with no factory is a plain Block.
    for m in re.finditer(r'register\(\s*"([a-z0-9_]+)"\s*,\s*BlockBehaviour', src):
        names.setdefault(m.group(1), "Block")
    # Helpers that hide the class behind a wrapper.
    for m in re.finditer(r'register(?:Legacy)?Stair\(\s*"([a-z0-9_]+)"', src):
        names[m.group(1)] = "StairBlock"
    for m in re.finditer(r'registerBed\(\s*"([a-z0-9_]+)"', src):
        names[m.group(1)] = "BedBlock"
    for m in re.finditer(r'registerStainedGlass\(\s*"([a-z0-9_]+)"', src):
        names[m.group(1)] = "StainedGlassBlock"
    # 26.3's registrations 26.1 lacks (keyed BlockItemIds.X / BlockIds.X,
    # multi-line lambda factories, registerSlab/registerWall helpers).
    if os.path.isfile(BLOCKS_JAVA2):
        src2 = open(BLOCKS_JAVA2, errors="ignore").read()
        for m in re.finditer(
                r'register\(\s*Block\w*Ids\.([A-Z0-9_]+)\s*,\s*'
                r"(?:\(\s*\w+\s*\)\s*->\s*\{?\s*(?:return\s+)?new\s+(\w+)|(\w+)::new)", src2):
            names.setdefault(m.group(1).lower(), m.group(2) or m.group(3))
        for m in re.finditer(r'register\(\s*Block\w*Ids\.([A-Z0-9_]+)\s*,\s*BlockBehaviour', src2):
            names.setdefault(m.group(1).lower(), "Block")
        for fn, cls in (("registerSlab", "SlabBlock"), ("registerWall", "WallBlock"),
                        ("registerStair", "StairBlock"), ("registerLegacyStair", "StairBlock"),
                        ("registerBed", "BedBlock")):
            for m in re.finditer(fn + r'\(\s*Block\w*Ids\.([A-Z0-9_]+)', src2):
                names.setdefault(m.group(1).lower(), cls)
        # ColorCollection.registerBlocks / zipMap register a whole dyed family
        # (the wool and concrete stairs and slabs, …) off one call;
        # gen_block_hardness expands each into its sixteen registrations.
        for _offset, fn, args, _field in color_collection_calls(src2):
            slug = args[0].strip('"')
            if fn in WRAPPER_CLASS:
                names.setdefault(slug, WRAPPER_CLASS[fn])
                continue
            m = (re.search(r'(\w+)::new', args[1]) or
                 re.search(r'\bnew\s+(\w+)\s*\(', args[1])) if len(args) >= 3 else None
            names.setdefault(slug, m.group(1) if m else "Block")
    return names


# The register* helpers that fix the block class (Blocks.java).
WRAPPER_CLASS = {"registerSlab": "SlabBlock", "registerWall": "WallBlock",
                 "registerStair": "StairBlock", "registerLegacyStair": "StairBlock",
                 "registerBed": "BedBlock", "registerStainedGlass": "StainedGlassBlock"}


def color_collection_calls(src):
    """gen_block_hardness.color_collection_calls: every ColorCollection
    registration in `src` expanded to [(offset, fn, args, field)], args[0]
    being the colour's slug literal. Imported lazily with the repo root as the
    working directory, which is what that module's input paths are relative to."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    cwd = os.getcwd()
    os.chdir(ROOT)
    try:
        import gen_block_hardness as hard
        return hard.color_collection_calls(src)
    finally:
        os.chdir(cwd)


def our_slugs():
    """Column 2 of BlockDefs.inc — the vanilla registry names this engine has."""
    src = open(BLOCK_DEFS, errors="ignore").read()
    return set(re.findall(r'BLOCK_DEF\(\s*\w+\s*,\s*"([^"]+)"', src))


def main():
    if not os.path.isdir(BLOCK_DIR):
        print(f"missing {BLOCK_DIR}", file=sys.stderr)
        return 1

    info = parse_hierarchy()
    wl_classes = closure(info, {WATERLOGGED_IFACE})
    dt_classes = closure(info, default_true_classes())
    uncond = unconditional_water_classes()

    registry = parse_registry()
    # Engine-only blocks that copy a vanilla class (same rule as the
    # ALIAS_EXACT rows in gen_block_states.py): The Hush's lantern_leaves is
    # a LeavesBlock and so declares `waterlogged` exactly like oak_leaves.
    # The second drop's cluster and lantern, plus the whisperwood/hushstone
    # families whose classes (SlabBlock, StairBlock, FenceBlock, FenceGateBlock,
    # TrapDoorBlock, WallBlock) all declare `waterlogged`; doors do not.
    for engine_slug, vanilla in (("lantern_leaves", "oak_leaves"),
                                 ("resonant_cluster", "amethyst_cluster"),
                                 ("echo_lantern", "lantern"),
                                 ("whisperwood_stairs", "oak_stairs"),
                                 ("whisperwood_slab", "oak_slab"),
                                 ("whisperwood_fence", "oak_fence"),
                                 ("whisperwood_fence_gate", "oak_fence_gate"),
                                 ("whisperwood_trapdoor", "oak_trapdoor"),
                                 ("hushstone_stairs", "oak_stairs"),
                                 ("hushstone_slab", "oak_slab"),
                                 ("polished_hushstone_stairs", "oak_stairs"),
                                 ("polished_hushstone_slab", "oak_slab"),
                                 ("hushstone_brick_stairs", "oak_stairs"),
                                 ("hushstone_brick_slab", "oak_slab"),
                                 ("hushstone_brick_wall", "cobblestone_wall"),
                                 # Aurelith (2026-09-22): the choirstone stairs/slabs/wall,
                                 # the grate (WaterloggedTransparentBlock), the conduit
                                 # (ChainBlock) and the lamp (LanternBlock) declare
                                 # `waterlogged`; the river's resonant water is water
                                 # unconditionally, bubble_column's rule.
                                 ("polished_choirstone_stairs", "oak_stairs"),
                                 ("polished_choirstone_slab", "oak_slab"),
                                 ("choirstone_brick_stairs", "oak_stairs"),
                                 ("choirstone_brick_slab", "oak_slab"),
                                 ("choirstone_brick_wall", "cobblestone_wall"),
                                 ("choirstone_tile_stairs", "oak_stairs"),
                                 ("choirstone_tile_slab", "oak_slab"),
                                 ("resonite_grate", "copper_grate"),
                                 ("crystal_conduit", "iron_chain"),
                                 ("choir_lamp", "lantern"),
                                 ("resonant_water", "bubble_column"),
                                 # Twilight Forest + The Aether (pass one): every leaves block
                                 # is a LeavesBlock and declares `waterlogged` like oak_leaves.
                                 # Critters/saplings alias end_rod/oak_sapling, which do not.
                                 ("twilight_oak_leaves", "oak_leaves"),
                                 ("canopy_leaves", "oak_leaves"),
                                 ("tf_mangrove_leaves", "oak_leaves"),
                                 ("dark_leaves", "oak_leaves"),
                                 ("skyroot_leaves", "oak_leaves"),
                                 ("golden_oak_leaves", "oak_leaves"),
                                 # The Aether, pass two: the stairs/slab/wall/fence/gate/trapdoor
                                 # families (StairBlock, SlabBlock, WallBlock, FenceBlock,
                                 # FenceGateBlock, TrapDoorBlock all declare `waterlogged`).
                                 ("holystone_stairs", "oak_stairs"), ("holystone_slab", "oak_slab"),
                                 ("mossy_holystone_stairs", "oak_stairs"), ("mossy_holystone_slab", "oak_slab"),
                                 ("holystone_brick_stairs", "oak_stairs"), ("holystone_brick_slab", "oak_slab"),
                                 ("skyroot_stairs", "oak_stairs"), ("skyroot_slab", "oak_slab"),
                                 ("icestone_stairs", "oak_stairs"), ("icestone_slab", "oak_slab"),
                                 ("carved_stairs", "oak_stairs"), ("carved_slab", "oak_slab"),
                                 ("angelic_stairs", "oak_stairs"), ("angelic_slab", "oak_slab"),
                                 ("hellfire_stairs", "oak_stairs"), ("hellfire_slab", "oak_slab"),
                                 ("holystone_wall", "cobblestone_wall"), ("mossy_holystone_wall", "cobblestone_wall"),
                                 ("holystone_brick_wall", "cobblestone_wall"), ("icestone_wall", "cobblestone_wall"),
                                 ("carved_wall", "cobblestone_wall"), ("angelic_wall", "cobblestone_wall"),
                                 ("hellfire_wall", "cobblestone_wall"), ("skyroot_fence", "oak_fence"),
                                 ("skyroot_fence_gate", "oak_fence_gate"), ("skyroot_trapdoor", "oak_trapdoor"),
                                 # Twilight Forest, pass two: the castle stairs (StairBlock).
                                 ("castle_brick_stairs", "oak_stairs"), ("worn_castle_brick_stairs", "oak_stairs"),
                                 ("cracked_castle_brick_stairs", "oak_stairs"), ("mossy_castle_brick_stairs", "oak_stairs"),
                                 ("encased_castle_brick_stairs", "oak_stairs"), ("bold_castle_brick_stairs", "oak_stairs")):
        if vanilla in registry:
            registry.setdefault(engine_slug, registry[vanilla])
    ours = our_slugs()

    waterloggable = sorted(
        s for s, c in registry.items() if c in wl_classes and s in ours
    )
    always = sorted(s for s, c in registry.items() if c in uncond and s in ours)
    default_true = {s for s in waterloggable if registry[s] in dt_classes}

    missing = sorted(
        s for s, c in registry.items() if c in wl_classes and s not in ours
    )

    hpp = '''// GENERATED by tools/gen_waterlogged.py - DO NOT EDIT BY HAND.
//
// MC's two ways of putting water inside a non-water block.
//
// `kWaterloggableTable` lists every block whose class chain reaches
// SimpleWaterloggedBlock, i.e. every block that declares the `waterlogged`
// BlockState property. `defaultWaterlogged` is the value its
// registerDefaultState installs - true for the coral families, sea pickle and
// conduit, false for everything else. It decides the ORDER the property's two
// values are declared in, because this engine requires state index 0 to be the
// block's default state.
//
// `kAlwaysWaterloggedTable` lists the blocks that carry no property and whose
// getFluidState returns WATER unconditionally (KelpBlock.java:62 and friends).
// Nothing is stored per voxel for those - they are water wherever they are.
#pragma once

#include <cstddef>

namespace Game {

    struct GeneratedWaterloggedRow {
        const char* slug;
        bool        defaultWaterlogged;
    };

    extern const GeneratedWaterloggedRow kWaterloggableTable[];
    extern const size_t kWaterloggableTableSize;

    extern const char* const kAlwaysWaterloggedTable[];
    extern const size_t kAlwaysWaterloggedTableSize;

} // namespace Game
'''

    rows = "\n".join(
        '        { "%s", %s },' % (s, "true" if s in default_true else "false")
        for s in waterloggable
    )
    always_rows = "\n".join('        "%s",' % s for s in always)

    # MSVC rejects a zero-length array (C2466); emit nullptr + 0 instead.
    if waterloggable:
        wl_body = "    const GeneratedWaterloggedRow kWaterloggableTable[] = {\n%s\n    };\n\n    const size_t kWaterloggableTableSize =\n        sizeof(kWaterloggableTable) / sizeof(kWaterloggableTable[0]);" % rows
    else:
        wl_body = "    const GeneratedWaterloggedRow* const kWaterloggableTable = nullptr;\n\n    const size_t kWaterloggableTableSize = 0;"
    if always:
        aw_body = "    const char* const kAlwaysWaterloggedTable[] = {\n%s\n    };\n\n    const size_t kAlwaysWaterloggedTableSize =\n        sizeof(kAlwaysWaterloggedTable) / sizeof(kAlwaysWaterloggedTable[0]);" % always_rows
    else:
        aw_body = "    const char* const* kAlwaysWaterloggedTable = nullptr;\n\n    const size_t kAlwaysWaterloggedTableSize = 0;"

    cpp = '''// GENERATED by tools/gen_waterlogged.py - DO NOT EDIT BY HAND.
#include "GeneratedWaterlogged.hpp"

namespace Game {

%s

%s

}} // namespace Game
'''.replace("}}", "}") % (wl_body, aw_body)

    with open(OUT_HPP, "w", encoding="utf-8") as f:
        f.write(hpp)
    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write(cpp)

    print(f"waterloggable blocks emitted: {len(waterloggable)}")
    print(f"  of which default waterlogged=true: {len(default_true)}")
    print(f"always-waterlogged blocks emitted: {len(always)}  {always}")
    print(f"MC waterloggable slugs this engine does not have: {len(missing)}")
    for probe in ("oak_stairs", "oak_slab", "oak_fence", "oak_leaves", "lantern",
                  "sea_pickle", "tube_coral_fan", "conduit", "glass", "oak_planks"):
        mark = "YES" if probe in waterloggable else "no "
        dv = " (default true)" if probe in default_true else ""
        print(f"    probe {probe:16s} -> {mark}{dv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
