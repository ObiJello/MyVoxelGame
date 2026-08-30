#!/usr/bin/env python3
"""coverage_report.py - biome (and thereby feature) coverage of the golden set.

A biome appearing in a golden region means that region's decoration ran the
biome's entire configured-feature list RNG-draw-for-RNG-draw, so per-biome
coverage is the coverage criterion for feature parity. ("Feature attempted" is
what parity depends on; individual placements may still be probabilistically
absent in any one region.)

Target set: build/biome_search --list-possible (the C++ overworld source).
Cross-checked against the vanilla datapack biome JSONs in data/minecraft.
Covered set: streamed from the Q (biome) lines of every golden dump.

Usage: coverage_report.py [--goldens DIR] [--json OUT] [--quiet]
Exit 0 if every possible biome is covered, 1 otherwise.
"""
import argparse
import gzip
import json
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(os.path.dirname(SCRIPT_DIR))

p = argparse.ArgumentParser()
p.add_argument("--goldens", default=os.path.join(SCRIPT_DIR, "goldens"))
p.add_argument("--json", default=os.path.join(SCRIPT_DIR, "coverage.json"))
p.add_argument("--quiet", action="store_true")
args = p.parse_args()

# ---- target set ----
tool = os.path.join(PROJECT_DIR, "build", "biome_search")
if not os.path.isfile(tool):
    print("build/biome_search missing — run: make biome_search", file=sys.stderr)
    sys.exit(2)
possible = set(subprocess.run([tool, "--list-possible"], capture_output=True,
                              text=True, check=True).stdout.split())

# cross-check: overworld biomes present in the vanilla datapack
datapack_dir = os.path.join(PROJECT_DIR, "..", "data", "minecraft", "worldgen", "biome")
if not os.path.isdir(datapack_dir):
    datapack_dir = os.path.join(PROJECT_DIR, "data", "minecraft", "worldgen", "biome")
datapack = {f"minecraft:{f[:-5]}" for f in os.listdir(datapack_dir)
            if f.endswith(".json")} if os.path.isdir(datapack_dir) else set()

# ---- biome -> feature count (from the vanilla datapack, informational) ----
def feature_count(biome):
    if not datapack_dir:
        return None
    path = os.path.join(datapack_dir, biome.split(":", 1)[1] + ".json")
    try:
        with open(path) as f:
            steps = json.load(f).get("features", [])
        return sum(len(s) if isinstance(s, list) else 1 for s in steps)
    except OSError:
        return None

# ---- covered set ----
manifest_path = os.path.join(args.goldens, "MANIFEST.json")
if not os.path.isfile(manifest_path):
    print(f"missing {manifest_path}", file=sys.stderr)
    sys.exit(2)
manifest = json.load(open(manifest_path))

covered = {}  # biome -> [golden tags]
for g in manifest["goldens"]:
    tag = g["tag"]
    seen = set()
    with gzip.open(os.path.join(args.goldens, f"java_{tag}.txt.gz"), "rt") as f:
        for line in f:
            if line.startswith("Q,"):
                seen.add(line.rstrip("\n").rsplit(",", 1)[-1])
    for b in seen:
        covered.setdefault(b, []).append(tag)

missing = sorted(possible - set(covered))
extra = sorted(set(covered) - possible)  # dumped but not in possible set (unexpected)

report = {
    "possible": len(possible),
    "covered": len(possible) - len(missing),
    "missing": missing,
    "unexpected_in_dumps": extra,
    "biomes": {b: {"goldens": covered.get(b, []),
                   "configured_features": feature_count(b)}
               for b in sorted(possible)},
}
json.dump(report, open(args.json, "w"), indent=1)

if not args.quiet:
    print(f"Overworld biome coverage: {report['covered']}/{report['possible']}")
    for b in sorted(possible):
        tags = covered.get(b, [])
        nfeat = feature_count(b)
        mark = "OK  " if tags else "MISS"
        detail = f"{len(tags)} golden(s)" if tags else "NOT COVERED"
        print(f"  {mark} {b:<40} {detail}  [{nfeat} features]")
    if extra:
        print(f"  NOTE: biomes in dumps but not in possible set: {extra}")
    if datapack and (datapack_overworld_diff := (possible - datapack)):
        print(f"  NOTE: possible biomes missing from datapack dir: {sorted(datapack_overworld_diff)}")
print(f"coverage.json written -> {args.json}")
sys.exit(0 if not missing else 1)
