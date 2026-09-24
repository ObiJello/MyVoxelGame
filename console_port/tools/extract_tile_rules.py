#!/usr/bin/env python3
"""Extract original placement rules; keep the complete classes as references."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
rules = {
    "Tile": ["mayPlace(Level *level, int x, int y, int z, int face)", "mayPlace(Level *level, int x, int y, int z)"],
    "Bush": ["mayPlace", "mayPlaceOn", "canSurvive"],
    "DeadBushTile": ["mayPlaceOn"],
    "Mushroom": ["mayPlace", "mayPlaceOn", "canSurvive"],
    "CactusTile": ["mayPlace", "canSurvive"],
    "ReedTile": ["mayPlace", "canSurvive"],
    "WaterlilyTile": ["mayPlaceOn", "canSurvive"],
    "PumpkinTile": ["mayPlace"],
    "VineTile": ["mayPlace", "isAcceptableNeighbor"],
    "Level": ["isTopSolidBlocking", "shouldFreezeIgnoreNeighbors(int", "shouldFreeze(int x, int y, int z)", "shouldFreeze(int x, int y, int z, bool checkNeighbors)"],
}
output = ['// Original placement methods extracted by tools/extract_tile_rules.py.\n',
          '#include "WorldGenLevel.h"\n']
for cls, methods in rules.items():
    source = (root / f"original/reference-only/{cls}.cpp").read_text()
    for method in methods:
        start = source.index(f"bool {cls}::{method}")
        opening = source.index("{", start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        output.append(source[start:end] + "\n\n")
Path(sys.argv[1]).write_text("".join(output))
