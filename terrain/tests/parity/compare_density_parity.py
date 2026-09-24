#!/usr/bin/env python3
"""Diff two DensityParityTest dumps (Java reference vs C++).

usage: compare_density_parity.py <java.txt> <java.bin> <cpp.txt> <cpp.bin> [--detail N]

Reports, per (settings, function), mismatching uncached points (P), cached
points (C) and volume cells (V), with the first few differences.
"""
import collections
import struct
import sys


def load(text_path, bin_path):
    points = []   # (kind, settings, fn, x, y, z, hex)
    volumes = []  # (settings, fn, cx, cz, shape, values)
    with open(bin_path, 'rb') as b, open(text_path) as t:
        for line in t:
            parts = line.split()
            if parts[0] in ('P', 'C'):
                points.append(tuple(parts))
            elif parts[0] == 'V':
                n = int(parts[6])
                data = b.read(4 * n)
                volumes.append((parts[1], parts[2], parts[3], parts[4], parts[5], struct.unpack('>%dI' % n, data)))
    return points, volumes


def as_float(bits):
    return struct.unpack('>f', struct.pack('>I', bits))[0]


def main():
    args = sys.argv[1:]
    detail = 3
    if '--detail' in args:
        i = args.index('--detail')
        detail = int(args[i + 1])
        del args[i:i + 2]
    jp, jv = load(args[0], args[1])
    cp, cv = load(args[2], args[3])
    if len(jp) != len(cp) or len(jv) != len(cv):
        print(f"stream length differs: points {len(jp)} vs {len(cp)}, volumes {len(jv)} vs {len(cv)}")
    bad = collections.Counter()
    total = collections.Counter()
    shown = collections.Counter()
    for a, b in zip(jp, cp):
        key = (a[0], a[1], a[2])
        total[key] += 1
        if a[:6] != b[:6]:
            print("point stream out of step:", a, b)
            return 1
        if a[6] != b[6]:
            bad[key] += 1
            if shown[key] < detail:
                shown[key] += 1
                print(f"  {a[0]} {a[1]} {a[2]} ({a[3]},{a[4]},{a[5]}): java {as_float(int(a[6], 16))!r} cpp {as_float(int(b[6], 16))!r}")
    for a, b in zip(jv, cv):
        key = ('V', a[0], a[1])
        if a[:5] != b[:5]:
            print("volume stream out of step:", a[:5], b[:5])
            return 1
        diffs = [i for i, (x, y) in enumerate(zip(a[5], b[5])) if x != y]
        total[key] += len(a[5])
        if diffs:
            bad[key] += len(diffs)
            if shown[key] < detail:
                shown[key] += 1
                i = diffs[0]
                print(f"  V {a[0]} {a[1]} chunk ({a[2]},{a[3]}) {a[4]}: {len(diffs)} cells, first index {i}: "
                      f"java {as_float(a[5][i])!r} cpp {as_float(b[5][i])!r}")
    print()
    for key in sorted(total):
        if bad[key]:
            print(f"MISMATCH {key[0]} {key[1]:18s} {key[2]:20s} {bad[key]}/{total[key]}")
    ok = sum(1 for k in total if not bad[k])
    print(f"{ok}/{len(total)} (kind, settings, function) groups exact")
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
