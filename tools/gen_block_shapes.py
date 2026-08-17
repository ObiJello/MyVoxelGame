#!/usr/bin/env python3
"""Extract MC block outline shapes (VoxelShape) into a C++ table.

WHY THIS EXISTS
---------------
This engine used to derive a block's outline/raycast box by unioning the AABBs
of its MODEL elements. That is not what Minecraft does, and for a whole family
of blocks it is badly wrong: a sapling's model is `block/cross`, two diagonal
planes spanning the FULL 16x16 cell, while `SaplingBlock.SHAPE` is
`Block.column(12, 0, 12)` — a 12-wide box only 12 tall. Mushrooms are worse
(model full-width, shape `column(6, 0, 6)`). The model and the shape are
independent in MC; the shape is hardcoded per block class in Java.

So we read the shapes out of the decompiled source. Blocks whose shape we
cannot resolve statically are simply omitted, and the engine keeps its
model-derived box for them — strictly better than before, never worse.

WHAT IS RESOLVED
----------------
A class qualifies when `getShape` (or an inherited `getShape`) is a plain
`return <CONST>;` and <CONST> resolves through Block/Shapes helpers to a set of
boxes. Per-state shapes (lambdas, direction maps, `getShapeForEachState`) are
deliberately NOT chased — they need the state to evaluate, which this table has
no room for.

The engine stores ONE AABB per state, so multi-box shapes are unioned. That
matches what the model-derived path already did and what the outline render
expects.

Output: src/common/world/block/GeneratedBlockShapes.{hpp,cpp}
Run:    python3 tools/gen_block_shapes.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLOCK_DIR = os.path.join(ROOT, "minecraft_code/decompiled_net/minecraft/world/level/block")
BLOCKS_JAVA = os.path.join(BLOCK_DIR, "Blocks.java")
OUT_HPP = os.path.join(ROOT, "src/common/world/block/GeneratedBlockShapes.hpp")
OUT_CPP = os.path.join(ROOT, "src/common/world/block/GeneratedBlockShapes.cpp")

FULL_CUBE = (0.0, 0.0, 0.0, 16.0, 16.0, 16.0)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# ── Java source helpers ─────────────────────────────────────────────────────

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def split_args(s):
    """Split a Java argument list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def match_call(src, start):
    """Given index of '(', return (inner, index_after_close)."""
    assert src[start] == "("
    depth, i = 0, start
    while i < len(src):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return src[start + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced parens")


NUM_RE = re.compile(r"^-?\d+(\.\d+)?$")


def as_num(tok):
    """Evaluate a numeric literal, tolerating Java casts and F/D suffixes."""
    t = tok.strip()
    t = re.sub(r"\((?:double|float|int)\)", "", t)
    t = t.strip()
    t = re.sub(r"[FfDdLl]$", "", t)
    if NUM_RE.match(t):
        return float(t)
    # Simple arithmetic like "16.0 - 2.0"
    if re.fullmatch(r"[-+*/(). 0-9]+", t):
        try:
            return float(eval(t, {"__builtins__": {}}, {}))
        except Exception:
            return None
    return None


# ── Shape expression evaluation ─────────────────────────────────────────────

class Unresolved(Exception):
    pass


def boxes_from_helper(name, args):
    """Convert one Block.* helper call into a list of boxes (pixel space)."""
    n = [as_num(a) for a in args]
    if any(v is None for v in n):
        raise Unresolved(f"non-numeric args to {name}: {args}")

    if name == "box" and len(n) == 6:
        return [tuple(n)]
    if name == "column" and len(n) == 3:
        size, lo, hi = n
        return boxes_from_helper("column", [str(size), str(size), str(lo), str(hi)])
    if name == "column" and len(n) == 4:
        sx, sz, lo, hi = n
        return [(8 - sx / 2, lo, 8 - sz / 2, 8 + sx / 2, hi, 8 + sz / 2)]
    if name == "cube" and len(n) == 1:
        return boxes_from_helper("cube", [str(n[0])] * 3)
    if name == "cube" and len(n) == 3:
        sx, sy, sz = n
        return boxes_from_helper("column", [str(sx), str(sz), str(8 - sy / 2), str(8 + sy / 2)])
    if name == "boxZ" and len(n) == 3:
        s, lo, hi = n
        return boxes_from_helper("boxZ", [str(s), str(s), str(lo), str(hi)])
    if name == "boxZ" and len(n) == 4:
        sx, sy, lo, hi = n
        return boxes_from_helper("boxZ", [str(sx), str(8 - sy / 2), str(8 + sy / 2), str(lo), str(hi)])
    if name == "boxZ" and len(n) == 5:
        sx, ylo, yhi, zlo, zhi = n
        return [(8 - sx / 2, ylo, zlo, 8 + sx / 2, yhi, zhi)]
    raise Unresolved(f"unhandled helper {name}/{len(n)}")


def strip_trailing_wrappers(e):
    """Drop a trailing `.move(...)` / `.optimize()` applied to the whole shape.

    `.move` is MC applying the per-position scatter offset
    (FlowerBlock.getShape = SHAPE.move(state.getOffset(pos))). This engine
    already applies that offset itself, from BlockRegistry::GetBlockOffset, so
    the value we want to bake is the UNMOVED shape — baking the moved one would
    double up the scatter. `.optimize()` is a pure VoxelShape simplification and
    never changes the bounds.
    """
    changed = True
    while changed:
        changed = False
        for meth in ("move", "optimize"):
            token = "." + meth
            idx, depth = 0, 0
            while idx < len(e):
                ch = e[idx]
                if ch in "([":
                    depth += 1
                elif ch in ")]":
                    depth -= 1
                elif depth == 0 and e.startswith(token, idx) and \
                        e[idx + len(token):].lstrip().startswith("("):
                    open_paren = e.index("(", idx + len(token))
                    _, after = match_call(e, open_paren)
                    if not e[after:].strip():
                        e = e[:idx].strip()
                        changed = True
                        break
                idx += 1
            if changed:
                break
    return e


def eval_shape(expr, consts, depth=0):
    """Evaluate a VoxelShape expression to a list of pixel-space boxes."""
    if depth > 12:
        raise Unresolved("expression too deep")
    e = expr.strip()
    e = re.sub(r"^\((?:VoxelShape)\)\s*", "", e).strip()
    e = strip_trailing_wrappers(e)

    # Parenthesised whole expression
    if e.startswith("(") and match_call(e, 0)[1] == len(e):
        return eval_shape(match_call(e, 0)[0], consts, depth + 1)

    m = re.match(r"^(?:Block|Shapes)\.([A-Za-z]+)\s*\(", e)
    if m:
        inner, after = match_call(e, e.index("(", m.start(1)))
        if e[after:].strip():
            raise Unresolved(f"trailing expression: {e[after:]!r}")
        name = m.group(1)
        if name == "block":
            return [FULL_CUBE]
        if name == "empty":
            return []
        if name in ("or", "join"):
            args = split_args(inner)
            if name == "join":
                # join(a, b, BooleanOp.OR) — only OR is a plain union.
                if len(args) != 3 or "OR" not in args[2]:
                    raise Unresolved("join with non-OR operator")
                args = args[:2]
            boxes = []
            for a in args:
                boxes.extend(eval_shape(a, consts, depth + 1))
            return boxes
        return boxes_from_helper(name, split_args(inner))

    # A bare constant name
    if re.fullmatch(r"[A-Z_][A-Z0-9_]*", e):
        if e not in consts:
            raise Unresolved(f"unknown constant {e}")
        return eval_shape(consts[e], consts, depth + 1)

    raise Unresolved(f"unparsed expression {e[:60]!r}")


def collect_constants(src):
    """Map CONSTANT_NAME -> initialiser expression, inline or via static block."""
    consts = {}
    for m in re.finditer(
            r"(?:private|protected|public)?\s*(?:static\s+)?(?:final\s+)?VoxelShape\s+"
            r"([A-Z_][A-Z0-9_]*)\s*=\s*", src):
        tail = src[m.end():]
        end = tail.find(";")
        if end != -1:
            consts[m.group(1)] = tail[:end].strip()
    # Assignments inside `static { ... }`
    for m in re.finditer(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*([^;]+);", src, flags=re.M):
        consts.setdefault(m.group(1), m.group(2).strip())
    return consts


def shape_for_class(cls, cache, depth=0):
    """Resolve a class's getShape() to a list of boxes, or None."""
    if cls in cache:
        return cache[cls]
    if depth > 6:
        return None
    path = os.path.join(BLOCK_DIR, cls + ".java")
    if not os.path.isfile(path):
        cache[cls] = None
        return None
    src = strip_comments(read(path))

    m = re.search(r"VoxelShape\s+getShape\s*\([^)]*\)\s*\{([^}]*)\}", src, flags=re.S)
    if m:
        body = m.group(1).strip()
        rm = re.fullmatch(r"return\s+([^;]+);", body)
        if rm:
            try:
                boxes = eval_shape(rm.group(1), collect_constants(src))
                cache[cls] = boxes
                return boxes
            except Unresolved:
                cache[cls] = None
                return None
        cache[cls] = None      # per-state / conditional shape
        return None

    # No override — inherit.
    em = re.search(r"class\s+" + re.escape(cls) + r"\b[^{]*?\bextends\s+([A-Za-z0-9_]+)", src)
    if em:
        boxes = shape_for_class(em.group(1), cache, depth + 1)
        cache[cls] = boxes
        return boxes
    cache[cls] = None
    return None


def union(boxes):
    xs0 = min(b[0] for b in boxes); ys0 = min(b[1] for b in boxes); zs0 = min(b[2] for b in boxes)
    xs1 = max(b[3] for b in boxes); ys1 = max(b[4] for b in boxes); zs1 = max(b[5] for b in boxes)
    return (xs0, ys0, zs0, xs1, ys1, zs1)


def fmt(v):
    s = repr(round(v / 16.0, 6))
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def main():
    blocks_src = strip_comments(read(BLOCKS_JAVA))
    slug2cls = {}
    for m in re.finditer(
            r'register\(\s*"([a-z0-9_]+)"\s*,\s*'
            r'(?:\([a-z]+\)\s*->\s*new\s+([A-Za-z0-9_]+)|([A-Za-z0-9_]+)::new)',
            blocks_src):
        slug2cls[m.group(1)] = m.group(2) or m.group(3)

    cache, rows, skipped = {}, [], {}
    for slug, cls in sorted(slug2cls.items()):
        boxes = shape_for_class(cls, cache)
        if not boxes:
            skipped.setdefault(cls, 0)
            skipped[cls] += 1
            continue
        box = union(boxes)
        if box == FULL_CUBE:
            continue          # engine default; no row needed
        rows.append((slug, box))

    hpp = f"""// GENERATED by tools/gen_block_shapes.py — DO NOT EDIT BY HAND.
//
// MC outline shapes (BlockBehaviour.getShape) for blocks whose shape is a
// static VoxelShape in the decompiled source. Values are in BLOCK units
// (0..1), already divided by 16 from MC's pixel space.
//
// Blocks absent from this table either are a full cube or have a per-state
// shape this generator deliberately does not chase; both keep the engine's
// model-derived box. See the script header for why the model cannot be used
// as the shape in the first place.
#pragma once

#include <cstddef>

namespace Game {{

    struct GeneratedBlockShapeRow {{
        const char* slug;
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
    }};

    extern const GeneratedBlockShapeRow kBlockShapeTable[];
    extern const size_t kBlockShapeTableSize;

}} // namespace Game
"""

    lines = []
    for slug, b in rows:
        lines.append(
            f'        {{"{slug}", {fmt(b[0])}, {fmt(b[1])}, {fmt(b[2])}, '
            f'{fmt(b[3])}, {fmt(b[4])}, {fmt(b[5])}}},')
    cpp = f"""// GENERATED by tools/gen_block_shapes.py — DO NOT EDIT BY HAND.
#include "GeneratedBlockShapes.hpp"

namespace Game {{

    const GeneratedBlockShapeRow kBlockShapeTable[] = {{
{chr(10).join(lines)}
    }};

    const size_t kBlockShapeTableSize =
        sizeof(kBlockShapeTable) / sizeof(kBlockShapeTable[0]);

}} // namespace Game
"""

    with open(OUT_HPP, "w") as f:
        f.write(hpp)
    with open(OUT_CPP, "w") as f:
        f.write(cpp)

    print(f"slugs with a class: {len(slug2cls)}")
    print(f"emitted non-cube shapes: {len(rows)}")
    print(f"classes left to the model fallback: {len(skipped)} "
          f"({sum(skipped.values())} slugs)")
    top = sorted(skipped.items(), key=lambda kv: -kv[1])[:12]
    for cls, n in top:
        print(f"    {n:4d}  {cls}")
    for probe in ("oak_sapling", "red_mushroom", "brown_mushroom", "dandelion",
                  "short_grass", "torch", "lever"):
        hit = next((r for r in rows if r[0] == probe), None)
        print(f"    probe {probe:16s} -> {hit[1] if hit else 'FALLBACK'}")


if __name__ == "__main__":
    main()
