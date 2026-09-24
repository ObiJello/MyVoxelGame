#!/usr/bin/env python3
"""Extract the archived console's two-layer creative spawn egg palette."""
from pathlib import Path
import difflib
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
source = (root / "original/reference-only/EntityIO.cpp").read_text()
colours = {node.attrib["name"]: int(node.attrib["value"], 16)
           for node in ET.parse(root / "assets/colours.xml").getroot().findall("colour")}
palette = {}
for match in re.finditer(
        r"setId\([^;]*?L\"([^\"]+)\",\s*(\d+),\s*eMinecraftColour_(\w+),\s*"
        r"eMinecraftColour_(\w+),\s*IDS_[A-Z0-9_]+\);", source):
    entity_id = int(match[2])
    palette[entity_id] = (colours[match[3]], colours[match[4]], match[1])

catalog = (root / "ported/CreativeCatalog.cpp").read_text()
egg_ids = {int(aux) for aux in re.findall(r"\{383,(\d+)\}", catalog)}
assert egg_ids and egg_ids <= palette.keys(), sorted(egg_ids - palette.keys())
assert len(palette) == 21 and len(egg_ids) == 21

lines = [
    "// Generated from EntityIO::staticCtor and the original colours.xml.",
    '#include "SpawnEggColors.h"',
    "namespace console {",
    "SpawnEggColors consoleSourceEggColors(int entityId){",
    "    switch(entityId){",
]
for entity_id, (base, spots, name) in sorted(palette.items()):
    lines.append(f"    case {entity_id}:return {{0x{base:06x},0x{spots:06x},true,L\"{name}\"}};")
lines += ["    default:return {};", "    }", "}", "}", ""]
output = root / "ported/SpawnEggColors.cpp"
generated = "\n".join(lines)
if sys.argv[1:] == ["--check"]:
    old = output.read_text() if output.exists() else ""
    if old != generated:
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), generated.splitlines(True),
                                                  fromfile=str(output), tofile="source extraction"))
        sys.exit(1)
    print(f"Verified {len(palette)} source spawn-egg color pairs.")
else:
    output.write_text(generated)
    print(f"Extracted {len(palette)} source spawn-egg color pairs.")
