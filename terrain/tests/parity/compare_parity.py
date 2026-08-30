#!/usr/bin/env python3
"""
compare_parity.py - canonical parity dump comparator (see FORMAT.md).

Usage:
  compare_parity.py <java_dump> <cpp_dump> [--first N] [--summary-json FILE] [--quiet]

Exit codes:
  0  dumps identical
  1  mismatches found
  2  structural error (chunk-set mismatch, unparseable line, missing file)

Mismatch classes (per position):
  missing          Java has a block, C++ has air
  extra            C++ has a block, Java has air
  wrong-block      both non-air, different block name
  wrong-properties same block name, different property string
  biome            Q-line biome differs
  heightmap        H-line height differs
  structure-layout S/P line sequence differs (starts/pieces; order-sensitive)
  structure-refs   R-line reference lists differ
  block-entity     E-line canonical NBT differs
"""

import argparse
import json
import sys
from collections import Counter, defaultdict

AIR = "minecraft:air"


def block_name(state):
    i = state.find("[")
    return state if i < 0 else state[:i]


def parse_chunks(path):
    """Yield (chunk_pos, {'B','Q','H','E': dict, 'SP': list, 'R': dict}) in file order.

    'SP' keeps S and P lines verbatim in file order (order is parity-load-bearing,
    see FORMAT.md); 'R' maps structure_id -> reference-list string.
    """
    chunk_pos = None
    data = None
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            kind, _, rest = line.partition(",")
            if kind == "C":
                if chunk_pos is not None:
                    yield chunk_pos, data
                cx, cz = rest.split(",")
                chunk_pos = (int(cx), int(cz))
                data = {"B": {}, "Q": {}, "H": {}, "E": {}, "SP": [], "R": {}}
            elif kind == "B":
                x, y, z, state = rest.split(",", 3)
                data["B"][(int(x), int(y), int(z))] = state
            elif kind == "Q":
                qx, qy, qz, biome = rest.split(",", 3)
                data["Q"][(int(qx), int(qy), int(qz))] = biome
            elif kind == "H":
                t, x, z, h = rest.split(",", 3)
                data["H"][(t, int(x), int(z))] = int(h)
            elif kind in ("S", "P"):
                data["SP"].append(line)
            elif kind == "R":
                structure_id, _, refs = rest.partition(",")
                data["R"][structure_id] = refs
            elif kind == "E":
                x, y, z, nbt = rest.split(",", 3)
                data["E"][(int(x), int(y), int(z))] = nbt
            else:
                raise ValueError(f"{path}:{lineno}: unparseable line: {line!r}")
    if chunk_pos is not None:
        yield chunk_pos, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("java_dump")
    ap.add_argument("cpp_dump")
    ap.add_argument("--first", type=int, default=5,
                    help="sample coordinates kept per (class, block type)")
    ap.add_argument("--summary-json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    totals = Counter()                      # class -> count
    by_type = Counter()                     # (class, type) -> count
    samples = defaultdict(list)             # (class, type) -> [sample strings]
    per_chunk = Counter()                   # chunk -> total mismatches
    chunks_compared = 0

    def record(cls, typ, chunk, detail):
        totals[cls] += 1
        by_type[(cls, typ)] += 1
        if len(samples[(cls, typ)]) < args.first:
            samples[(cls, typ)].append(f"chunk{chunk} {detail}")
        per_chunk[chunk] += 1

    try:
        java_iter = parse_chunks(args.java_dump)
        cpp_iter = parse_chunks(args.cpp_dump)
        while True:
            j = next(java_iter, None)
            c = next(cpp_iter, None)
            if j is None and c is None:
                break
            if j is None or c is None or j[0] != c[0]:
                print(f"STRUCTURAL ERROR: chunk sequence mismatch "
                      f"(java={j and j[0]}, cpp={c and c[0]})", file=sys.stderr)
                return 2
            chunk, (jd, cd) = j[0], (j[1], c[1])
            chunks_compared += 1

            for pos in jd["B"].keys() | cd["B"].keys():
                js = jd["B"].get(pos, AIR)
                cs = cd["B"].get(pos, AIR)
                if js == cs:
                    continue
                wx = chunk[0] * 16 + pos[0]
                wz = chunk[1] * 16 + pos[2]
                where = f"local({pos[0]},{pos[1]},{pos[2]}) world({wx},{pos[1]},{wz})"
                if cs == AIR:
                    record("missing", block_name(js), chunk, f"{where} java={js}")
                elif js == AIR:
                    record("extra", block_name(cs), chunk, f"{where} cpp={cs}")
                elif block_name(js) == block_name(cs):
                    record("wrong-properties", block_name(js), chunk,
                           f"{where} java={js} cpp={cs}")
                else:
                    record("wrong-block", block_name(js), chunk,
                           f"{where} java={js} cpp={cs}")

            for pos in jd["Q"].keys() | cd["Q"].keys():
                jb = jd["Q"].get(pos, "<absent>")
                cb = cd["Q"].get(pos, "<absent>")
                if jb != cb:
                    record("biome", jb, chunk, f"quart{pos} java={jb} cpp={cb}")

            for pos in jd["H"].keys() | cd["H"].keys():
                jh = jd["H"].get(pos, "<absent>")
                ch = cd["H"].get(pos, "<absent>")
                if jh != ch:
                    record("heightmap", pos[0], chunk, f"{pos} java={jh} cpp={ch}")

            # S/P: order-sensitive positional comparison (FORMAT.md).
            for i in range(max(len(jd["SP"]), len(cd["SP"]))):
                jl = jd["SP"][i] if i < len(jd["SP"]) else "<absent>"
                cl = cd["SP"][i] if i < len(cd["SP"]) else "<absent>"
                if jl != cl:
                    typ = (jl if jl != "<absent>" else cl).split(",")[1]
                    record("structure-layout", typ, chunk,
                           f"line {i}: java={jl} cpp={cl}")

            for sid in jd["R"].keys() | cd["R"].keys():
                jr = jd["R"].get(sid, "<absent>")
                cr = cd["R"].get(sid, "<absent>")
                if jr != cr:
                    record("structure-refs", sid, chunk, f"java={jr} cpp={cr}")

            for pos in jd["E"].keys() | cd["E"].keys():
                je = jd["E"].get(pos, "<absent>")
                ce = cd["E"].get(pos, "<absent>")
                if je != ce:
                    record("block-entity", "E", chunk, f"{pos} java={je} cpp={ce}")
    except (FileNotFoundError, ValueError) as e:
        print(f"STRUCTURAL ERROR: {e}", file=sys.stderr)
        return 2

    total = sum(totals.values())
    if not args.quiet:
        print(f"Chunks compared: {chunks_compared}")
        print(f"Total mismatches: {total}")
        if total:
            print()
            print("By class:")
            for cls, n in totals.most_common():
                print(f"  {cls:18s} {n}")
            print()
            print("By (class, type), worst first:")
            for (cls, typ), n in by_type.most_common(50):
                print(f"  {n:8d}  {cls:18s} {typ}")
                for s in samples[(cls, typ)]:
                    print(f"             {s}")
            print()
            worst = per_chunk.most_common(10)
            print("Worst chunks:")
            for chunk, n in worst:
                print(f"  {chunk}: {n}")
        else:
            print("PERFECT MATCH at full block-state + biome + heightmap granularity.")

    if args.summary_json:
        with open(args.summary_json, "w") as f:
            json.dump({
                "chunks_compared": chunks_compared,
                "total_mismatches": total,
                "by_class": dict(totals),
                "by_type": [{"class": cls, "type": typ, "count": n}
                            for (cls, typ), n in by_type.most_common()],
                "samples": {f"{cls}|{typ}": v for (cls, typ), v in samples.items()},
                "worst_chunks": [{"chunk": list(chunk), "count": n}
                                 for chunk, n in per_chunk.most_common(50)],
            }, f, indent=2)

    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
