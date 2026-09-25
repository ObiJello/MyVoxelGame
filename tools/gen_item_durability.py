#!/usr/bin/env python3
"""Generate src/common/entity/GeneratedItemDurability.{hpp,cpp} from MC 26.3's
Items.java + ToolMaterial.java + ArmorMaterials.java.

WHAT IT PRODUCES, per item that carries any of them — the Item.Properties
component defaults the durability and enchanting systems read:

  maxDamage       MAX_DAMAGE (Properties.durability: MAX_DAMAGE n, DAMAGE 0,
                  MAX_STACK_SIZE 1). Tools and spears take their material's
                  durability (ToolMaterial.applyCommonProperties), humanoid
                  armour ArmorType.getDurability(material.durability) — the
                  piece's unit (helmet 11, chestplate 16, leggings 15, boots 13,
                  body 16) times the material's multiplier.
  enchantable     ENCHANTABLE's value (Properties.enchantable; the material's
                  enchantmentValue for tools and humanoid armour).
  repairable      REPAIRABLE's HolderSet as one entry: "#minecraft:<tag>" for
                  a material's repair tag, "minecraft:<item>" for
                  .repairable(ITEM).
  weapon          WEAPON's item_damage_per_attack (-1 = no WEAPON): 1 for
                  swords, spears, the mace and the trident, 2 for every other
                  tool (ToolMaterial.applyToolProperties' Weapon(2, ...)), and
                  the axe's 5 s disable-blocking.
  toolPerBlock    TOOL's damage_per_block where it is not the default 1:
                  swords 2 (applySwordProperties), the mace and trident 2
                  (their createToolProperties). -1 = keep the default.
  breakSound      BREAK_SOUND where it is not entity.item.break (the shield's
                  item.shield.break, wolf armour's item.wolf_armor.break).
  damageResistant DAMAGE_RESISTANT's tag (Properties.fireResistant() →
                  #minecraft:is_fire) — what keeps netherite armour from
                  wearing in lava.

WHY GENERATED. The numbers live in three files and two of them are multiplied
together (armour); copying ~120 rows by hand is how an iron chestplate ends up
at 250 instead of 240.

    python3 tools/gen_item_durability.py
"""

import os
import re
import sys

MC = "minecraft_code_26.3-pre-2/decompiled_net/minecraft"
ITEMS = os.path.join(MC, "world/item/Items.java")
ITEM_IDS = os.path.join(MC, "references/ItemIds.java")
ITEM_TAGS = os.path.join(MC, "tags/ItemTags.java")
TOOL_MATERIAL = os.path.join(MC, "world/item/ToolMaterial.java")
ARMOR_MATERIALS = os.path.join(MC, "world/item/equipment/ArmorMaterials.java")
ARMOR_TYPE = os.path.join(MC, "world/item/equipment/ArmorType.java")
OUT_HPP = "src/common/entity/GeneratedItemDurability.hpp"
OUT_CPP = "src/common/entity/GeneratedItemDurability.cpp"
ITEM_LIST_HPP = "src/common/entity/GeneratedItemList.hpp"

# One registration statement: `NAME = registerItem(ItemIds.NAME, ...);`.
STATEMENT = re.compile(r"\n\s+([A-Z_0-9]+) = register(?:Item|Block)\w*\(")
# `ToolMaterial WOOD = new ToolMaterial(BlockTags.X, durability, speed, bonus,
# enchantmentValue, ItemTags.REPAIR)` — the static-block assignment form.
TOOL_MATERIAL_ROW = re.compile(
    r"([A-Z]+) = new ToolMaterial\(\s*BlockTags\.[A-Z_]+\s*,\s*(\d+)\s*,\s*[\d.]+F?\s*,"
    r"\s*[-\d.]+F?\s*,\s*(\d+)\s*,\s*ItemTags\.([A-Z_]+)\s*\)")
ARMOR_MATERIAL_ROW = re.compile(
    r"ArmorMaterial\s+([A-Z_]+)\s*=\s*new ArmorMaterial\(\s*(\d+)\s*,\s*makeDefense\([^)]*\)\s*,"
    r"\s*(\d+)\s*,\s*SoundEvents\.[A-Z_]+\s*,\s*[-\d.]+F?\s*,\s*[-\d.]+F?\s*,\s*ItemTags\.([A-Z_]+)")
ARMOR_TYPE_ROW = re.compile(r"([A-Z]+)\(EquipmentSlot\.[A-Z]+,\s*(\d+),")
ITEM_ID_ROW = re.compile(r"ResourceKey<Item>\s+([A-Z_0-9]+)\s*=\s*create\(\"([a-z_0-9]+)\"\)")
ITEM_TAG_ROW = re.compile(r"([A-Z_0-9]+)\s*=\s*bind\(\"([a-z_0-9/]+)\"\)")

DURABILITY = re.compile(r"\.durability\((\d+)\)")
ENCHANTABLE = re.compile(r"\.enchantable\((\d+)\)")
REPAIR_TAG = re.compile(r"\.repairable\(ItemTags\.([A-Z_0-9]+)\)")
REPAIR_ITEM = re.compile(r"\.repairable\(([A-Z_0-9]+)\)")
TOOL_FORM = re.compile(r"\.(sword|pickaxe|axe|hoe|shovel|spear)\(\s*ToolMaterial\.([A-Z]+)")
HUMANOID_ARMOR = re.compile(r"\.humanoidArmor\(\s*ArmorMaterials\.([A-Z_]+)\s*,\s*ArmorType\.([A-Z]+)\s*\)")
WOLF_ARMOR = re.compile(r"\.wolfArmor\(\s*ArmorMaterials\.([A-Z_]+)\s*\)")
WEAPON = re.compile(r"new Weapon\((\d+)(?:\s*,\s*([\d.]+)F?)?\)")
TOOL_CLASS = re.compile(r"DataComponents\.TOOL,\s*(\w+)\.createToolProperties\(\)")
NEW_TOOL = re.compile(r"new Tool\(List\.of\([^;]*?\)\s*,\s*[\d.]+F?\s*,\s*(\d+)\s*,")
BREAK_SOUND = re.compile(r"DataComponents\.BREAK_SOUND,\s*SoundEvents\.([A-Z_0-9]+)")
FIRE_RESISTANT = re.compile(r"\.fireResistant\(\)")

# SoundEvents constant -> event id, for the handful of break sounds.
SOUND_IDS = {
    "SHIELD_BREAK": "item.shield.break",
    "WOLF_ARMOR_BREAK": "item.wolf_armor.break",
    "ITEM_BREAK": "entity.item.break",
}

# ── Engine-only durable items ──────────────────────────────────────────────
# Items no Items.java has. slug -> (maxDamage, enchantable, repairable, kind),
# kind being the tool form ("sword", "pickaxe", ...) or "armor". Sources:
#   The Hush (docs/the-hush.md): resonite is netherite's twin with 1800
#     durability; enchantability and weapon rules follow netherite. The echo
#     blade, resonance bow and the other Hush tools document no durability,
#     so they stay unbreakable (no MAX_DAMAGE) until they do.
#   The Aether (AetherItemTiers / AetherItems): skyroot 59 / 15, holystone
#     131 / 5, zanite 250 / 14, gravitite 1561 / 10; zanite armour x15 / 9,
#     gravitite armour x33 / 10. The repairing tags are single items here.
#   Twilight Forest (TFToolMaterials / TFArmorMaterials / TFItems): ironwood
#     512 / 25, fiery 1024 / 10, steeleaf 131 / 9, knightmetal 512 / 8 (the
#     steeleaf sword is registered on KNIGHTMETAL); armour naga x21 / 15,
#     ironwood x20 / 15, fiery x25 / 10, steeleaf x10 / 9, knightmetal x20 / 8.
#     Every fiery piece is .fireResistant() in TFItems.
ARMOR_UNITS = {"helmet": 11, "chestplate": 16, "leggings": 15, "boots": 13}


def _tools(prefix, durability, enchant, repair, forms):
    return {"%s_%s" % (prefix, f): (durability, enchant, repair, f) for f in forms}


def _armor(prefix, multiplier, enchant, repair, pieces=("helmet", "chestplate", "leggings", "boots")):
    return {"%s_%s" % (prefix, p): (ARMOR_UNITS[p] * multiplier, enchant, repair, "armor") for p in pieces}


ALL_TOOLS = ("sword", "pickaxe", "axe", "shovel", "hoe")
ENGINE_ONLY = {
    **_tools("resonite", 1800, 15, "minecraft:resonite_ingot", ALL_TOOLS),
    **_tools("skyroot", 59, 15, "minecraft:skyroot_planks", ALL_TOOLS),
    **_tools("holystone", 131, 5, "minecraft:holystone", ALL_TOOLS),
    **_tools("zanite", 250, 14, "minecraft:zanite_gemstone", ALL_TOOLS),
    **_tools("gravitite", 1561, 10, "minecraft:enchanted_gravitite", ALL_TOOLS),
    **_tools("ironwood", 512, 25, "minecraft:ironwood_ingot", ALL_TOOLS),
    **_tools("steeleaf", 131, 9, "minecraft:steeleaf_ingot", ("pickaxe", "axe", "shovel", "hoe")),
    "steeleaf_sword": (512, 8, "minecraft:steeleaf_ingot", "sword"),
    **_tools("knightmetal", 512, 8, "minecraft:knightmetal_ingot", ("sword", "pickaxe", "axe")),
    **_tools("fiery", 1024, 10, "minecraft:fiery_ingot", ("sword", "pickaxe")),
    **_armor("zanite", 15, 9, "minecraft:zanite_gemstone"),
    **_armor("gravitite", 33, 10, "minecraft:enchanted_gravitite"),
    **_armor("naga", 21, 15, "minecraft:naga_scale", ("chestplate", "leggings")),
    **_armor("ironwood", 20, 15, "minecraft:ironwood_ingot"),
    **_armor("fiery", 25, 10, "minecraft:fiery_ingot"),
    **_armor("steeleaf", 10, 9, "minecraft:steeleaf_ingot"),
    **_armor("knightmetal", 20, 8, "minecraft:knightmetal_ingot"),
}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def weapon_for(form):
    """ToolMaterial.applyToolProperties / applySwordProperties / Properties.spear."""
    if form in ("sword", "spear"):
        return 1, 0.0
    return 2, (5.0 if form == "axe" else 0.0)


def main():
    tool_materials = {m.group(1): (int(m.group(2)), int(m.group(3)), m.group(4))
                      for m in TOOL_MATERIAL_ROW.finditer(read(TOOL_MATERIAL))}
    armor_materials = {m.group(1): (int(m.group(2)), int(m.group(3)), m.group(4))
                       for m in ARMOR_MATERIAL_ROW.finditer(read(ARMOR_MATERIALS))}
    armor_units = {m.group(1): int(m.group(2)) for m in ARMOR_TYPE_ROW.finditer(read(ARMOR_TYPE))}
    item_ids = {m.group(1): m.group(2) for m in ITEM_ID_ROW.finditer(read(ITEM_IDS))}
    item_tags = {m.group(1): m.group(2) for m in ITEM_TAG_ROW.finditer(read(ITEM_TAGS))}
    for name, table in (("ToolMaterial", tool_materials), ("ArmorMaterials", armor_materials),
                        ("ArmorType", armor_units), ("ItemIds", item_ids), ("ItemTags", item_tags)):
        if not table:
            raise SystemExit("no %s rows parsed — the file changed shape" % name)

    src = read(ITEMS)
    starts = [(m.start(), m.group(1)) for m in STATEMENT.finditer(src)]
    rows = {}
    for i, (pos, const) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(src)
        stmt = src[pos:end]
        slug = item_ids.get(const, const.lower())

        max_damage = 0
        enchant = 0
        repair = ""
        weapon, disable = -1, 0.0
        per_block = -1
        break_sound = ""
        resistant = "#minecraft:is_fire" if FIRE_RESISTANT.search(stmt) else ""

        if m := DURABILITY.search(stmt):
            max_damage = int(m.group(1))
        if m := TOOL_FORM.search(stmt):
            form, mat = m.groups()
            dur, ench, tag = tool_materials[mat]
            max_damage, enchant, repair = dur, ench, "#minecraft:" + item_tags[tag]
            weapon, disable = weapon_for(form)
            if form == "sword":
                per_block = 2
        if m := HUMANOID_ARMOR.search(stmt):
            mat, atype = m.groups()
            mult, ench, tag = armor_materials[mat]
            max_damage, enchant, repair = armor_units[atype] * mult, ench, "#minecraft:" + item_tags[tag]
        if m := WOLF_ARMOR.search(stmt):
            mult, _ench, tag = armor_materials[m.group(1)]
            # Properties.wolfArmor: durability + repairable, no enchantable.
            max_damage, repair = armor_units["BODY"] * mult, "#minecraft:" + item_tags[tag]
            break_sound = SOUND_IDS["WOLF_ARMOR_BREAK"]
        if m := ENCHANTABLE.search(stmt):
            enchant = int(m.group(1))
        if m := REPAIR_TAG.search(stmt):
            repair = "#minecraft:" + item_tags[m.group(1)]
        elif m := REPAIR_ITEM.search(stmt):
            repair = "minecraft:" + item_ids.get(m.group(1), m.group(1).lower())
        if m := WEAPON.search(stmt):
            weapon = int(m.group(1))
            disable = float(m.group(2)) if m.group(2) else 0.0
        if m := TOOL_CLASS.search(stmt):
            cls = os.path.join(MC, "world/item", m.group(1) + ".java")
            if os.path.exists(cls):
                if t := NEW_TOOL.search(read(cls)):
                    if int(t.group(1)) != 1:
                        per_block = int(t.group(1))
        if m := BREAK_SOUND.search(stmt):
            sound = SOUND_IDS.get(m.group(1))
            if sound is None:
                raise SystemExit("%s: unknown break sound %s" % (slug, m.group(1)))
            if sound != SOUND_IDS["ITEM_BREAK"]:
                break_sound = sound

        if max_damage or enchant or repair or weapon >= 0 or per_block >= 0 or break_sound:
            # Only durable rows carry the resistance: its one reader is wear.
            rows[slug] = (max_damage, enchant, repair, weapon, disable, per_block, break_sound,
                          resistant if max_damage else "")

    if not rows:
        raise SystemExit("no durable items parsed — Items.java changed shape")

    for slug, (dur, ench, repair, kind) in ENGINE_ONLY.items():
        if slug in rows:
            raise SystemExit("engine-only item '%s' collides with an MC item" % slug)
        weapon, disable, per_block = -1, 0.0, -1
        if kind != "armor":
            weapon, disable = weapon_for(kind)
            per_block = 2 if kind == "sword" else -1
        rows[slug] = (dur, ench, repair, weapon, disable, per_block, "",
                      "#minecraft:is_fire" if slug.startswith("fiery_") else "")

    # Resolve against the generated item table so an item MC has and this port
    # does not is dropped loudly instead of emitting a row nothing reads.
    known = set(re.findall(r'//\s*"([a-z0-9_]+)"', read(ITEM_LIST_HPP)))
    missing = sorted(s for s in rows if s not in known)
    emitted = [(s, rows[s]) for s in sorted(rows) if s in known]

    def cf(v):
        s = repr(round(float(v), 4))
        return (s if "." in s else s + ".0") + "f"

    body = ['        {{ "{}", {}, {}, "{}", {}, {}, {}, "{}", "{}" }},'.format(
        slug, dur, ench, repair, weapon, cf(disable), per_block, sound, resistant)
        for slug, (dur, ench, repair, weapon, disable, per_block, sound, resistant) in emitted]

    hpp = """// GENERATED by tools/gen_item_durability.py — do not edit by hand.
//
// MC 26.3's per-item durability and enchanting defaults (Items.java combined
// with ToolMaterial.java, ArmorMaterials.java and ArmorType.java): the
// MAX_DAMAGE, ENCHANTABLE, REPAIRABLE, WEAPON, TOOL damage_per_block,
// BREAK_SOUND and DAMAGE_RESISTANT an Item.Properties chain gives each item. ItemDurability.cpp
// turns every row into the item's defaultComponents at registration.
#pragma once

#include <cstdint>
#include <string_view>

namespace Game {

    struct ItemDurabilityRow {
        std::string_view slug;
        int32_t          maxDamage;             // 0 = not durable (no MAX_DAMAGE / DAMAGE)
        int32_t          enchantable;           // 0 = no ENCHANTABLE component
        std::string_view repairable;            // "" = none; "#ns:tag" or "ns:item"
        int8_t           weaponDamagePerAttack; // -1 = no WEAPON component
        float            disableBlockingSeconds;
        int8_t           toolDamagePerBlock;    // -1 = the TOOL default (1)
        std::string_view breakSound;            // "" = entity.item.break
        std::string_view damageResistant;       // "" = none; a damage-type tag
    };

    inline constexpr int kItemDurabilityCount = %d;
    extern const ItemDurabilityRow kItemDurability[kItemDurabilityCount];

} // namespace Game
""" % len(emitted)

    cpp = "\n".join([
        "// GENERATED by tools/gen_item_durability.py — do not edit by hand.",
        '#include "common/entity/GeneratedItemDurability.hpp"',
        "",
        "namespace Game {",
        "",
        "    const ItemDurabilityRow kItemDurability[kItemDurabilityCount] = {",
        *body,
        "    };",
        "",
        "} // namespace Game",
        "",
    ])

    with open(OUT_HPP, "w", encoding="utf-8") as f:
        f.write(hpp)
    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write(cpp)

    print("%s: %d rows" % (OUT_HPP, len(emitted)))
    if missing:
        print("  %d items this port has no item for: %s" % (len(missing), ", ".join(missing)))
    if "-v" in sys.argv:
        for slug, row in emitted:
            print("    %-28s %s" % (slug, row))


if __name__ == "__main__":
    main()
