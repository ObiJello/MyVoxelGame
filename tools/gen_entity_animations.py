#!/usr/bin/env python3
"""Generate src/client/renderer/entity/model/GeneratedAnimations.{hpp,cpp}
from MC's client/animation/definitions/*Animation.java.

Modern MC mobs do NOT animate through setupAnim's limb swing. Their motion is a
KeyframeAnimation — a declarative AnimationDefinition of per-part position /
rotation / scale keyframes that `applyWalk` samples against walkAnimationPos.
A frog, an armadillo, a camel, a sniffer, a creaking and a copper golem all move
this way, which is why the generic `cos(pos * 0.6662)` swing did nothing
recognisable for them: MC never swings those limbs, it plays a curve.

The definitions are pure data — timestamps, vectors, an interpolation kind — so
they generate exactly like the meshes do. The RUNTIME (baking part names to
ModelPart pointers, sampling, catmull-rom) is ported by hand in
KeyframeAnimation.{hpp,cpp}; only the data lives here.

    python3 tools/gen_entity_animations.py
"""

import math
import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
DEF_DIR = os.path.join(MC, "client/animation/definitions")
OUT_HPP = "src/client/renderer/entity/model/GeneratedAnimations.hpp"
OUT_CPP = "src/client/renderer/entity/model/GeneratedAnimations.cpp"

TARGETS = {"POSITION": "Position", "ROTATION": "Rotation", "SCALE": "Scale"}
INTERPS = {"LINEAR": "Linear", "CATMULLROM": "CatmullRom"}


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def balanced(text, start, open_ch="(", close_ch=")"):
    depth, i = 0, start
    while i < len(text):
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    return text[start + 1:], len(text)


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def num(expr):
    e = expr.strip().rstrip("Ff").rstrip("Dd")
    e = re.sub(r"\(\s*(?:float|double|int)\s*\)", "", e)
    try:
        return float(e)
    except ValueError:
        return 0.0


def vec(expr):
    """KeyframeAnimations.posVec / degreeVec / scaleVec -> the stored vector.

    Each helper pre-converts (KeyframeAnimations.java): posVec NEGATES Y because
    model space is Y-down, degreeVec converts to radians, and scaleVec stores
    the DELTA from 1. Emitting the raw literals instead would give an upside
    down bounce, a 57x rotation and a mob that vanishes.
    """
    m = re.search(r"KeyframeAnimations\.(posVec|degreeVec|scaleVec)\s*\(", expr)
    if not m:
        return (0.0, 0.0, 0.0)
    inner, _ = balanced(expr, m.end() - 1)
    a = [num(x) for x in split_args(inner)]
    while len(a) < 3:
        a.append(0.0)
    kind = m.group(1)
    if kind == "posVec":
        return (a[0], -a[1], a[2])
    if kind == "degreeVec":
        return tuple(v * math.pi / 180.0 for v in a[:3])
    return (a[0] - 1.0, a[1] - 1.0, a[2] - 1.0)


def cf(v):
    s = repr(round(float(v), 7))
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


def parse_definitions(src):
    """`NAME = AnimationDefinition.Builder.withLength(x)...build();` -> rows."""
    out = []
    for m in re.finditer(r"\b([A-Z][A-Z0-9_]*)\s*=\s*AnimationDefinition\.Builder\.withLength\s*\(", src):
        name = m.group(1)
        inner, end = balanced(src, m.end() - 1)
        length = num(inner)

        # The whole chained expression, up to the terminating semicolon.
        semi = src.find(";", end)
        chain = src[end:semi]
        looping = ".looping()" in chain

        channels = []
        for a in re.finditer(r"\.addAnimation\s*\(", chain):
            args_text, _ = balanced(chain, a.end() - 1)
            args = split_args(args_text)
            if len(args) < 2:
                continue
            part = args[0].strip().strip('"')

            ch = args[1]
            tm = re.search(r"Targets\.([A-Z]+)", ch)
            if not tm or tm.group(1) not in TARGETS:
                continue
            target = TARGETS[tm.group(1)]

            keys = []
            for k in re.finditer(r"new Keyframe\s*\(", ch):
                kt, _ = balanced(ch, k.end() - 1)
                ka = split_args(kt)
                if len(ka) < 3:
                    continue
                im = re.search(r"Interpolations\.([A-Z]+)", ka[-1])
                keys.append((num(ka[0]), vec(ka[1]),
                             INTERPS.get(im.group(1) if im else "LINEAR", "Linear")))
            if keys:
                channels.append((part, target, keys))
        if channels:
            out.append((name, length, looping, channels))
    return out


def main():
    if not os.path.isdir(DEF_DIR):
        sys.exit(f"missing {DEF_DIR} — run from the repo root")

    anims = []
    for f in sorted(os.listdir(DEF_DIR)):
        if not f.endswith("Animation.java"):
            continue
        src = strip_comments(open(os.path.join(DEF_DIR, f), encoding="utf-8").read())
        for name, length, looping, channels in parse_definitions(src):
            anims.append((f[:-5] + "." + name, length, looping, channels))

    key_rows, ch_rows, anim_rows = [], [], []
    for name, length, looping, channels in anims:
        first_ch = len(ch_rows)
        for part, target, keys in channels:
            first_key = len(key_rows)
            for t, v, interp in keys:
                key_rows.append(
                    "    {{ {}, {}, {}, {}, AnimInterp::{} }},".format(
                        cf(t), cf(v[0]), cf(v[1]), cf(v[2]), interp))
            ch_rows.append(
                '    {{ "{}", AnimTarget::{}, {}, {} }},'.format(
                    part, target, first_key, len(keys)))
        anim_rows.append(
            '    {{ "{}", {}, {}, {}, {} }},'.format(
                name, cf(length), "true" if looping else "false",
                first_ch, len(channels)))

    hpp = f"""// GENERATED by tools/gen_entity_animations.py — do not edit by hand.
//
// MC's AnimationDefinitions, flattened. See the generator's docstring for why
// these exist at all: modern mobs animate from keyframes, not from setupAnim's
// limb swing, so without them a frog or a camel simply stands still while it
// walks.
//
// The vectors are already in MC's STORED form — posVec has negated Y, degreeVec
// is radians, scaleVec is the delta from 1 — exactly as KeyframeAnimations
// produces them, so the runtime adds them straight onto the part.
#pragma once

#include <string_view>

namespace Render {{

    enum class AnimTarget : uint8_t {{ Position, Rotation, Scale }};
    enum class AnimInterp : uint8_t {{ Linear, CatmullRom }};

    struct GenAnimKey {{
        float t;              // timestamp, seconds
        float x, y, z;
        AnimInterp interp;
    }};

    struct GenAnimChannel {{
        std::string_view part;
        AnimTarget target;
        int firstKey, keyCount;
    }};

    struct GenAnim {{
        std::string_view name;   // e.g. "FrogAnimation.FROG_WALK"
        float length;            // seconds
        bool  looping;
        int   firstChannel, channelCount;
    }};

    inline constexpr int kGenAnimCount = {len(anim_rows)};
    extern const GenAnim        kGenAnims[kGenAnimCount];
    extern const GenAnimChannel kGenAnimChannels[];
    extern const GenAnimKey     kGenAnimKeys[];

    const GenAnim* FindGenAnim(std::string_view name);

}} // namespace Render
"""

    cpp = "\n".join([
        "// GENERATED by tools/gen_entity_animations.py — do not edit by hand.",
        '#include "client/renderer/entity/model/GeneratedAnimations.hpp"',
        "",
        "namespace Render {",
        "",
        "    const GenAnimKey kGenAnimKeys[] = {",
        *key_rows,
        "    };",
        "",
        "    const GenAnimChannel kGenAnimChannels[] = {",
        *ch_rows,
        "    };",
        "",
        "    const GenAnim kGenAnims[kGenAnimCount] = {",
        *anim_rows,
        "    };",
        "",
        "    const GenAnim* FindGenAnim(std::string_view name) {",
        "        for (const GenAnim& a : kGenAnims) {",
        "            if (a.name == name) return &a;",
        "        }",
        "        return nullptr;",
        "    }",
        "",
        "} // namespace Render",
        "",
    ])

    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp)
    print(f"{OUT_HPP}: {len(anim_rows)} animations, "
          f"{len(ch_rows)} channels, {len(key_rows)} keyframes")


if __name__ == "__main__":
    main()
