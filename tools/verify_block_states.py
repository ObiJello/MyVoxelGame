#!/usr/bin/env python3
"""Independently verify GeneratedBlockStates.cpp against vanilla.

It parses the emitted C++ back out and re-derives every fact from
minecraft-data, so an emission bug — a window offset off by one, a value-pool
alias pointing at the wrong strings, a property interned under the wrong key —
fails here rather than silently producing a different state space at runtime.

The independence that matters is in the ARITHMETIC and the EMISSION: none of
the counting, indexing or window resolution below shares a line with the
generator. The one thing it does import is the alias/explicit POLICY (which
new slug borrows which older slug's row), because a second copy of that would
be a thing that drifts rather than a thing that checks.

That failure mode is the whole reason this exists. A wrong `valueBegin` does
not crash; it makes `facing=north` decode as `half=top`, and the first symptom
is blocks rendering in the wrong orientation three subsystems away.

Run: python3 tools/verify_block_states.py
"""

import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The alias/explicit POLICY is imported rather than restated. Independence here
# is meant to cover the arithmetic and the emission — a second copy of "which
# slug borrows which" would just be a thing that silently drifts.
from gen_block_states import alias_for, explicit_for, NOT_A_BLOCK  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCDATA = os.path.join(
    ROOT, "tools", "protocol-gen", "node_modules", "minecraft-data",
    "minecraft-data", "data", "pc", "1.21.6", "blocks.json")
GEN = os.path.join(ROOT, "src", "common", "world", "block", "GeneratedBlockStates.cpp")
BLOCKDEFS = os.path.join(ROOT, "src", "common", "world", "block", "BlockDefs.inc")


def section(src, name):
    """Body text of `const ... name[] = { ... };`"""
    m = re.search(re.escape(name) + r'\[\]\s*=\s*\{(.*?)\n    \};', src, re.S)
    if not m:
        raise SystemExit(f"could not find array {name} in the generated source")
    return m.group(1)


def main():
    src = open(GEN).read()
    upstream = {b["name"]: b for b in json.load(open(MCDATA))}
    slugs = [m.group(2) for m in re.finditer(
        r'BLOCK_DEF\(\s*(\w+)\s*,\s*"([^"]+)"', open(BLOCKDEFS).read())]

    values = re.findall(r'"((?:[^"\\]|\\.)*)"', section(src, "kPropertyValues"))
    props = [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
             for m in re.finditer(r'\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',
                                  section(src, "kProperties"))]
    refs = [int(x) for x in re.findall(r'(\d+),', section(src, "kBlockPropertyRefs"))]
    blocks = [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
              for m in re.finditer(
                  r'\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',
                  section(src, "kBlockStates"))]

    fail = []

    def check(cond, msg):
        if not cond:
            fail.append(msg)

    # Every property window must land inside the pool and hold >= 2 values.
    for i, (name, kind, vb, vc) in enumerate(props):
        check(vc >= 2, f"property {name!r} (#{i}) has {vc} value(s); MC forbids < 2")
        check(vb + vc <= len(values),
              f"property {name!r} (#{i}) window {vb}+{vc} runs past the {len(values)}-entry pool")
        if kind == 0:
            check(values[vb:vb + vc] == ["true", "false"],
                  f"bool property {name!r} values are {values[vb:vb+vc]}, expected [true, false]")

    seen, aliased, explicit = set(), set(), set()
    total = 0
    for slug, pb, pc, default_index, state_count in blocks:
        check(slug not in seen, f"{slug} appears twice in kBlockStates")
        seen.add(slug)
        b = upstream.get(slug)
        if b is None:
            target = alias_for(slug)
            if target is not None:
                b = upstream.get(target)
                check(b is not None, f"{slug} aliases {target!r}, absent upstream")
                aliased.add(slug)
            elif explicit_for(slug) is not None:
                verify_explicit(slug, pb, pc, default_index, state_count,
                                refs, props, values, check)
                explicit.add(slug)
                total += state_count
                continue
            else:
                check(False, f"{slug} is in the generated table but has no upstream row, "
                             f"no alias and no explicit entry")
                continue
        check(pb + pc <= len(refs),
              f"{slug} property window {pb}+{pc} runs past the {len(refs)}-entry ref table")

        got_names, product = [], 1
        for r in refs[pb:pb + pc]:
            check(r < len(props), f"{slug} references property #{r}, only {len(props)} exist")
            if r >= len(props):
                continue
            name, kind, vb, vc = props[r]
            got_names.append(name)
            product *= vc
            want = (["true", "false"] if b_state_type(b, name) == "bool"
                    else [str(v) for v in b_state_values(b, name)])
            check(values[vb:vb + vc] == want,
                  f"{slug}.{name}: emitted values {values[vb:vb+vc]} != upstream {want}")

        want_names = [s["name"] for s in b.get("states", [])]
        check(got_names == want_names,
              f"{slug}: property order {got_names} != upstream sorted-by-name {want_names}")

        span = b["maxStateId"] - b["minStateId"] + 1
        check(product == state_count, f"{slug}: stateCount {state_count} != product {product}")
        check(state_count == span, f"{slug}: stateCount {state_count} != upstream span {span}")
        check(default_index == b["defaultState"] - b["minStateId"],
              f"{slug}: defaultIndex {default_index} != upstream "
              f"{b['defaultState'] - b['minStateId']}")
        total += state_count

    # Blocks with properties upstream must not be silently missing from the table.
    for slug in slugs:
        if slug in NOT_A_BLOCK:
            continue
        if slug not in upstream and alias_for(slug) is None and explicit_for(slug) is None:
            fail.append(f"{slug} resolves in no source; the generator should have refused it")
        b = upstream.get(slug)
        if b and b.get("states") and slug not in seen:
            fail.append(f"{slug} has {len(b['states'])} properties upstream but no emitted row")

    stateless = len(slugs) - len(blocks)
    runtime_total = total + stateless
    bits = math.ceil(math.log2(runtime_total))

    print(f"properties      {len(props)} distinct, {len(values)} pooled value strings")
    print(f"blocks          {len(slugs)} declared, {len(blocks)} with properties")
    print(f"states          {total} from stateful blocks + {stateless} single-state "
          f"= {runtime_total}")
    print(f"palette width   {bits} bits/voxel")
    print(f"default != 0    {sum(1 for _, _, _, d, _ in blocks if d != 0)} blocks")
    print(f"supplement      {len(aliased)} aliased, {len(explicit)} explicit, "
          f"{len(NOT_A_BLOCK)} non-blocks")
    print(f"headroom        {(1 << bits) - runtime_total} states before {bits + 1} bits")

    if fail:
        print(f"\nFAILED — {len(fail)} problem(s):", file=sys.stderr)
        for m in fail[:40]:
            print(f"  {m}", file=sys.stderr)
        if len(fail) > 40:
            print(f"  … and {len(fail) - 40} more", file=sys.stderr)
        sys.exit(1)
    print("\nOK — generated table reproduces vanilla's state space exactly")


def verify_explicit(slug, pb, pc, default_index, state_count, refs, props, values, check):
    """An explicit entry has no upstream row, so check it against itself:
    sorted-by-name order, radix product, and default index in range."""
    names, product = [], 1
    for r in refs[pb:pb + pc]:
        if r >= len(props):
            check(False, f"{slug} references property #{r}")
            return
        name, _kind, _vb, vc = props[r]
        names.append(name)
        product *= vc
    check(names == sorted(names),
          f"{slug}: explicit property order {names} is not sorted by name")
    check(product == state_count, f"{slug}: stateCount {state_count} != product {product}")
    check(0 <= default_index < state_count,
          f"{slug}: defaultIndex {default_index} outside [0,{state_count})")


def b_state_type(b, name):
    for s in b.get("states", []):
        if s["name"] == name:
            return s["type"]
    return None


def b_state_values(b, name):
    for s in b.get("states", []):
        if s["name"] == name:
            return s.get("values") or []
    return []


if __name__ == "__main__":
    main()
