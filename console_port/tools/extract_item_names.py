#!/usr/bin/env python3
"""Generate creative-item display names from the archived PS3 registrations."""
from pathlib import Path
import difflib
import json
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
refs = root / "original/reference-only"
output = root / "ported/ItemNames.cpp"

localized = {
    data.attrib["name"]: "".join(data.find("value").itertext()).strip()
    for data in ET.parse(refs / "strings.resx").getroot().findall("data")
    if data.find("value") is not None
}
catalog = [(int(item), int(damage)) for item, damage in
           re.findall(r"\{(\d+),(\d+)\}", (root / "ported/CreativeCatalog.cpp").read_text())]
catalog_ids = {item for item, _ in catalog}

base_keys = {}
for kind in ("Tile", "Item"):
    ids = {name: int(value) for name, value in
           re.findall(r"static const int (\w+)_Id\s*=\s*(\d+)", (refs / f"{kind}.h").read_text())}
    for statement in (refs / f"{kind}.cpp").read_text().split(";"):
        match = re.search(rf"{kind}::(\w+)\s*=.*?setDescriptionId\((IDS_[A-Z0-9_]+)\)",
                          statement, re.S)
        if match and match[1] in ids:
            base_keys[ids[match[1]]] = match[2]

# These registrations use numeric constructors, so their IDs are not resolved
# by the static *_Id declarations above. Their names are explicit in source.
base_keys.update({77: "IDS_TILE_BUTTON", 134: "IDS_TILE_STAIRS_SPRUCEWOOD",
                  135: "IDS_TILE_STAIRS_BIRCHWOOD", 136: "IDS_TILE_STAIRS_JUNGLEWOOD",
                  389: "IDS_ITEM_ITEMFRAME"})
assert catalog_ids == catalog_ids & base_keys.keys(), sorted(catalog_ids - base_keys.keys())


def source_array(class_name: str, array_name: str) -> list[str]:
    source = (refs / f"{class_name}.cpp").read_text()
    match = re.search(rf"{class_name}::{array_name}\s*\[[^]]*\]\s*=\s*\{{([^}}]*)\}}", source, re.S)
    assert match, (class_name, array_name)
    keys = re.findall(r"IDS_[A-Z0-9_]+", match[1])
    assert keys and all(key in localized for key in keys), (class_name, array_name)
    return keys


variants = {}
variant_arrays = (
    (5, "WoodTile", "WOOD_NAMES", lambda d: d),
    (6, "Sapling", "SAPLING_NAMES", lambda d: d),
    (17, "TreeTile", "TREE_NAMES", lambda d: d & 3),
    (18, "LeafTile", "LEAF_NAMES", lambda d: d & 3),
    (24, "SandStoneTile", "SANDSTONE_NAMES", lambda d: d),
    (35, "ClothTileItem", "COLOR_DESCS", lambda d: 15 - (d & 15)),
    (43, "StoneSlabTile", "SLAB_NAMES", lambda d: d),
    (44, "StoneSlabTile", "SLAB_NAMES", lambda d: d),
    (97, "StoneMonsterTile", "STONE_MONSTER_NAMES", lambda d: d),
    (98, "SmoothStoneBrickTile", "SMOOTH_STONE_BRICK_NAMES", lambda d: d),
    (139, "WallTile", "COBBLE_NAMES", lambda d: d),
    (155, "QuartzBlockTile", "BLOCK_NAMES", lambda d: d),
    (171, "ClothTileItem", "CARPET_COLOR_DESCS", lambda d: 15 - (d & 15)),
    (351, "DyePowderItem", "COLOR_DESCS", lambda d: d),
    (397, "SkullItem", "NAMES", lambda d: d),
)
for item, class_name, array_name, index_for_damage in variant_arrays:
    names = source_array(class_name, array_name)
    for _, damage in (entry for entry in catalog if entry[0] == item):
        index = index_for_damage(damage)
        assert 0 <= index < len(names), (item, damage, len(names))
        variants[item, damage] = localized[names[index]]

# CoalItem::getDescriptionId chooses charcoal for auxiliary value 1.
assert "IDS_ITEM_CHARCOAL" in (refs / "CoalItem.cpp").read_text()
variants[263, 1] = localized["IDS_ITEM_CHARCOAL"]

# MonsterPlacerItem::getHoverName substitutes EntityIO's localized name into
# the source "Spawn {*CREATURE*}" template for each creative egg.
assert '{*CREATURE*}' in (refs / "MonsterPlacerItem.cpp").read_text()
egg_template = localized["IDS_ITEM_MONSTER_SPAWNER"]
assert "{*CREATURE*}" in egg_template
egg_names = {int(mob_id): description for mob_id, description in re.findall(
    r"setId\([^;]*?,\s*(\d+),\s*eMinecraftColour_[^,]+,\s*"
    r"eMinecraftColour_[^,]+,\s*(IDS_[A-Z0-9_]+)\);",
    (refs / "EntityIO.cpp").read_text())}
for _, damage in (entry for entry in catalog if entry[0] == 383):
    assert damage in egg_names and egg_names[damage] in localized, damage
    variants[383, damage] = egg_template.replace("{*CREATURE*}", localized[egg_names[damage]])

# The potion scene composes these placeholders separately; main.cpp uses its
# source-derived potionDisplayName instead of this fallback.
base_names = {item: localized[base_keys[item]] for item in catalog_ids}
base_names[373] = "Potion"
base_names[383] = egg_template.replace("{*CREATURE*}", "").strip()
assert all(key in localized for key in (base_keys[item] for item in catalog_ids))

lines = [
    "// Generated from the archived console Item/Tile registrations,",
    "// variant description arrays, and Common/Media/strings.resx.",
    '#include "ItemNames.h"',
    "namespace console {",
    "const char* consoleSourceItemName(int id,int damage){",
    "    switch((id<<16)|(damage&0xffff)){",
]
for (item, damage), name in sorted(variants.items()):
    lines.append(f"    case {item * 65536 + damage}:return {json.dumps(name, ensure_ascii=False)};")
lines += ["    default:break;", "    }", "    switch(id){"]
for item in sorted(catalog_ids):
    lines.append(f"    case {item}:return {json.dumps(base_names[item], ensure_ascii=False)};")
lines += ["    default:return nullptr;", "    }", "}", "}", ""]
generated = "\n".join(lines)
if sys.argv[1:] == ["--check"]:
    old = output.read_text() if output.exists() else ""
    if old != generated:
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), generated.splitlines(True),
                                                  fromfile=str(output), tofile="source extraction"))
        sys.exit(1)
    print(f"Verified names for {len(catalog_ids)} creative IDs and {len(variants)} source variants.")
else:
    output.write_text(generated)
    print(f"Extracted names for {len(catalog_ids)} creative IDs and {len(variants)} source variants.")
