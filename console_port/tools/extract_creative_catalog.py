#!/usr/bin/env python3
"""Extract the PS3 creative tab entries from the archived console sources."""
from pathlib import Path
import ast
import difflib
import operator
import re
import sys
from functools import reduce

root = Path(__file__).resolve().parents[1]
refs = root / "original/reference-only"
output = root / "ported/CreativeCatalog.cpp"

sources = {name: (refs / (name + ".h")).read_text() for name in (
    "Tile", "Item", "LeafTile", "QuartzBlockTile", "SandStoneTile", "Sapling",
    "SkullTileEntity", "SmoothStoneBrickTile", "StoneMonsterTile", "StoneSlabTile",
    "TallGrass", "TreeTile", "WallTile")}
constants = {}
for classname, source in sources.items():
    for name, expression in re.findall(r"static\s+const\s+int\s+(\w+)\s*=\s*([^;]+);", source):
        constants[classname + "::" + name] = expression.strip()
for name, value in re.findall(r"^#define\s+(MASK_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b",
                              (refs / "Potion_Macros.h").read_text(), re.M):
    constants[name] = value


def evaluate(expression: str) -> int:
    expression = expression.strip()
    if expression.startswith("MACRO_MAKEPOTION_AUXVAL("):
        assert expression.endswith(")")
        arguments = expression[len("MACRO_MAKEPOTION_AUXVAL("):-1].split(",")
        return reduce(operator.or_, (evaluate(term) for term in arguments), 0)
    if expression in constants:
        return evaluate(constants[expression])
    tree = ast.parse(expression, mode="eval").body
    if isinstance(tree, ast.Constant) and isinstance(tree.value, int):
        return tree.value
    raise ValueError(f"Unresolved source constant: {expression}")


def arguments(text: str) -> list[str]:
    parts, depth, start = [], 0, 0
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    parts.append(text[start:].strip())
    return parts


source = (refs / "IUIScene_CreativeMenu.cpp").read_text()
body = source.split("void IUIScene_CreativeMenu::staticCtor()", 1)[1].split("// Top Row", 1)[0]
groups: dict[str, list[tuple[int, int]]] = {}
current = None
for line in body.splitlines():
    line = line.split("//", 1)[0].strip()
    match = re.fullmatch(r"DEF\((\w+)\)\s*;?", line)
    if match:
        current = match.group(1)
        groups[current] = []
        continue
    match = re.fullmatch(r"ITEM(_AUX)?\((.*)\)\s*;?", line)
    if not match:
        continue
    assert current is not None, line
    fields = arguments(match.group(2))
    assert len(fields) == (2 if match.group(1) else 1), line
    identity = fields[0]
    if identity in ("Tile::clay", "Tile::vine"):
        identity += "_Id"
    id_value = evaluate(identity)
    damage = evaluate(fields[1]) if len(fields) == 2 else 0
    assert 1 <= id_value <= 32767 and 0 <= damage <= 32767, line
    groups[current].append((id_value, damage))

tabs = (
    ("Building Blocks", ("eCreativeInventory_BuildingBlocks",)),
    ("Decoration", ("eCreativeInventory_Decoration",)),
    ("Redstone & Transport", ("eCreativeInventory_Transport", "eCreativeInventory_Redstone")),
    ("Materials", ("eCreativeInventory_Materials",)),
    ("Food", ("eCreativeInventory_Food",)),
    ("Tools & Armour", ("eCreativeInventory_ToolsArmourWeapons",)),
    ("Brewing", ("eCreativeInventory_Brewing", "eCreativeInventory_Potions_Level2_Extended",
                 "eCreativeInventory_Potions_Extended", "eCreativeInventory_Potions_Level2",
                 "eCreativeInventory_Potions_Basic")),
    ("Miscellaneous", ("eCreativeInventory_Misc",)),
)
assert len(groups) == 13 and len(tabs) == 8 and sum(map(len, groups.values())) >= 400
lines = [
    "// Generated from IUIScene_CreativeMenu::staticCtor and source item constants.",
    '#include "CreativeCatalog.h"',
    "#include <stdexcept>",
    "namespace console {",
    "namespace {",
]
for index, (title, names) in enumerate(tabs):
    entries = [entry for name in names for entry in groups[name]]
    lines.append(f"constexpr CreativeEntry tab{index}[]={{")
    for id_value, damage in entries:
        lines.append(f"    {{{id_value},{damage}}},")
    lines.append("};")
lines += ["}", f"int creativeTabCount(){{return {len(tabs)};}}", "CreativeTab creativeTab(int index){", "    switch(index){"]
for index, (title, _) in enumerate(tabs):
    lines.append(f'    case {index}:return {{"{title}",tab{index}}};')
lines += ["    default:throw std::out_of_range(\"Creative tab\");", "    }", "}", "}", ""]
generated = "\n".join(lines)
if len(sys.argv) > 1 and sys.argv[1] == "--check":
    previous = output.read_text() if output.exists() else ""
    if previous != generated:
        sys.stdout.writelines(difflib.unified_diff(previous.splitlines(True), generated.splitlines(True),
                                                  fromfile=str(output), tofile="source extraction"))
        sys.exit(1)
    print(f"Verified {sum(map(len, groups.values()))} creative entries in {len(tabs)} tabs.")
else:
    output.write_text(generated)
    print(f"Extracted {sum(map(len, groups.values()))} creative entries in {len(tabs)} tabs.")
