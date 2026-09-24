#!/usr/bin/env python3
"""Generate src/common/entity/GeneratedItemAttributes.{hpp,cpp} from MC's
Items.java + ToolMaterial.java.

WHY GENERATED. A weapon's damage in MC is not a number in a table — it is
`attackDamageBaseline + material.attackDamageBonus`, split across two files, and
the split is deliberate: every sword shares the 3.0 baseline and differs only by
its material's bonus. Hand-copying 35 pairs is 70 chances to typo one, and a
wrong weapon damage is invisible until somebody counts hits on a zombie.

WHAT IT PRODUCES, per item:
  attackDamage   the ATTACK_DAMAGE modifier the item ADDS in the main hand
  attackSpeed    the ATTACK_SPEED modifier it ADDS (always negative — a weapon
                 makes you swing SLOWER than a bare hand)

Both are ADD_VALUE modifiers onto the player's base attributes (1.0 damage,
4.0 speed), which is why an iron sword reads "6 damage" in game: 1 + (3 + 2).

    python3 tools/gen_item_attributes.py
"""

import os
import re
import sys

MC = "minecraft_code_26.1-snapshot-1/decompiled_net/minecraft"
ITEMS = os.path.join(MC, "world/item/Items.java")
MATERIAL = os.path.join(MC, "world/item/ToolMaterial.java")
OUT_HPP = "src/common/entity/GeneratedItemAttributes.hpp"
OUT_CPP = "src/common/entity/GeneratedItemAttributes.cpp"

# `.sword(ToolMaterial.IRON, 3.0F, -2.4F)` and `.pickaxe(...)`.
PROP_FORM = re.compile(
    r'registerItem\(\s*"([a-z_0-9]+)"\s*,\s*\(new Item\.Properties\(\)\)'
    r'\.(sword|pickaxe)\(\s*ToolMaterial\.([A-Z]+)\s*,\s*([-\d.]+)F?\s*,\s*([-\d.]+)F?\s*\)')

# Armour: `.humanoidArmor(ArmorMaterials.IRON, ArmorType.HELMET)`,
# `.wolfArmor(ArmorMaterials.ARMADILLO_SCUTE)`, `.horseArmor(ArmorMaterials.IRON)`
# (the last two are ArmorType.BODY in Item.Properties). The registration key is
# a string slug in 26.1's Items.java and `ItemIds.NAME` in 26.3's; both forms
# are read.
ARMOR_FORM = re.compile(
    r'registerItem\(\s*(?:"([a-z_0-9]+)"|ItemIds\.([A-Z_0-9]+))\s*,\s*\(new Item\.Properties\(\)\)'
    r'\.(humanoidArmor|wolfArmor|horseArmor)\(\s*ArmorMaterials\.([A-Z_]+)\s*(?:,\s*ArmorType\.([A-Z]+))?\s*\)')
ARMOR_MATERIALS = os.path.join(MC, "world/item/equipment/ArmorMaterials.java")
# `ArmorMaterial IRON = new ArmorMaterial(15, makeDefense(2, 5, 6, 2, 5), 9,
#  SoundEvents.ARMOR_EQUIP_IRON, 0.0F, 0.0F, ...)`: makeDefense(boots, legs,
# chest, helm, body), then enchantment value, sound, TOUGHNESS, KNOCKBACK
# RESISTANCE.
ARMOR_MATERIAL_ROW = re.compile(
    r'ArmorMaterial\s+([A-Z_]+)\s*=\s*new ArmorMaterial\(\s*\d+\s*,\s*makeDefense\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\s*,'
    r'\s*\d+\s*,\s*SoundEvents\.[A-Z_]+\s*,\s*([-\d.]+)F?\s*,\s*([-\d.]+)F?')
# ArmorType -> (defense index in makeDefense order, EquipmentSlotGroup name)
ARMOR_TYPES = {
    "BOOTS":      (0, "feet"),
    "LEGGINGS":   (1, "legs"),
    "CHESTPLATE": (2, "chest"),
    "HELMET":     (3, "head"),
    "BODY":       (4, "body"),
}

# Spears (26.x): `.spear(ToolMaterial.IRON, attackDuration, damageMultiplier,
# …)`. Item.Properties.spear builds the modifiers itself — ATTACK_DAMAGE is
# `0.0 + material.attackDamageBonus()`, ATTACK_SPEED is
# `1 / attackDuration - 4.0` — so only the material and the first float
# matter here.
SPEAR_FORM = re.compile(
    r'registerItem\(\s*(?:"([a-z_0-9]+)"|ItemIds\.([A-Z_0-9]+))\s*,\s*\(new Item\.Properties\(\)\)'
    r'(?:\.[a-zA-Z]+\([^()]*\))*?'
    r'\.spear\(\s*ToolMaterial\.([A-Z]+)\s*,\s*([-\d.]+)F?\s*,')

# `new AxeItem(ToolMaterial.WOOD, 6.0F, -3.2F, p)` and Shovel/Hoe.
CTOR_FORM = re.compile(
    r'registerItem\(\s*"([a-z_0-9]+)"\s*,[^;]*?'
    r'new (Axe|Shovel|Hoe)Item\(\s*ToolMaterial\.([A-Z]+)\s*,'
    r'\s*([-\d.]+)F?\s*,\s*([-\d.]+)F?\s*,')

# `.attributes(TridentItem.createAttributes())` — the weapons that are not made
# of a ToolMaterial (trident, mace) build their modifiers in their own class
# instead, so their numbers live in that file, not in Items.java. Missing these
# is silent and expensive: an unlisted weapon falls through to (0, 0), so a
# trident would swing for the bare-hand 1.0 rather than 9.
ATTR_CLASS_FORM = re.compile(
    r'registerItem\(\s*"([a-z_0-9]+)"\s*,[^;]*?'
    r'\.attributes\((\w+)\.createAttributes\(\)\)')

# Inside that class: ItemAttributeModifiers.builder().add(Attributes.ATTACK_X,
# new AttributeModifier(ID, (double)5.0F, ...), EquipmentSlotGroup.MAINHAND)
ATTR_BUILDER_FORM = re.compile(
    r'Attributes\.(ATTACK_DAMAGE|ATTACK_SPEED),\s*new AttributeModifier\('
    r'[^,]+,\s*\(double\)([-\d.]+)F?')

MATERIAL_BONUS = re.compile(
    r"(\w+) = new ToolMaterial\([^,]+,\s*\d+,\s*[\d.]+F?,\s*([-\d.]+)F?")


# ── Engine-only weapons ───────────────────────────────────────────────────
# Items no Items.java has (The Hush, docs/the-hush.md). Same shape as a parsed
# row: (kind, material, FINAL attackDamage row, attackSpeed row). Resonite is
# netherite's twin (baseline + 4.0 bonus, so "+1 damage over diamond" for
# every tool that has damage; a hoe is 0 in every material). The echo blade
# reads 10 in game (1 base + 9) and swings like a sword.
ENGINE_ONLY_WEAPONS = {
    "resonite_sword":   ("sword",   "RESONITE", 3.0 + 4.0, -2.4),
    "resonite_pickaxe": ("pickaxe", "RESONITE", 1.0 + 4.0, -2.8),
    "resonite_axe":     ("axe",     "RESONITE", 5.0 + 4.0, -3.0),
    "resonite_shovel":  ("shovel",  "RESONITE", 1.5 + 4.0, -3.0),
    "resonite_hoe":     ("hoe",     "RESONITE", -4.0 + 4.0, 0.0),
    "echo_blade":       ("sword",   "-",        9.0,       -2.4),
    # The Aether, pass two (AetherItemTiers attack bonus: skyroot 0,
    # holystone 1, zanite 2, gravitite 3; each item's createAttributes(tier,
    # base, speed) from its *Item class — FINAL = base + bonus).
    "skyroot_sword": ("sword", "SKYROOT", 3.0, -2.4),
    "skyroot_pickaxe": ("pickaxe", "SKYROOT", 1.0, -2.8),
    "skyroot_shovel": ("shovel", "SKYROOT", 1.5, -3.0),
    "skyroot_axe": ("axe", "SKYROOT", 6.0, -3.2),
    "skyroot_hoe": ("hoe", "SKYROOT", 0.0, -3.0),
    "holystone_sword": ("sword", "HOLYSTONE", 4.0, -2.4),
    "holystone_pickaxe": ("pickaxe", "HOLYSTONE", 2.0, -2.8),
    "holystone_shovel": ("shovel", "HOLYSTONE", 2.5, -3.0),
    "holystone_axe": ("axe", "HOLYSTONE", 8.0, -3.2),
    "holystone_hoe": ("hoe", "HOLYSTONE", 0.0, -2.0),
    "zanite_sword": ("sword", "ZANITE", 5.0, -2.4),
    "zanite_pickaxe": ("pickaxe", "ZANITE", 3.0, -2.8),
    "zanite_shovel": ("shovel", "ZANITE", 3.5, -3.0),
    "zanite_axe": ("axe", "ZANITE", 8.0, -3.1),
    "zanite_hoe": ("hoe", "ZANITE", 0.0, -1.0),
    "gravitite_sword": ("sword", "GRAVITITE", 6.0, -2.4),
    "gravitite_pickaxe": ("pickaxe", "GRAVITITE", 4.0, -2.8),
    "gravitite_shovel": ("shovel", "GRAVITITE", 4.5, -3.0),
    "gravitite_axe": ("axe", "GRAVITITE", 8.0, -3.0),
    "gravitite_hoe": ("hoe", "GRAVITITE", 0.0, 0.0),
    # Twilight Forest, pass two (TFToolMaterials bonus: ironwood 2, fiery 4,
    # steeleaf 3, knightmetal 3; TFItems' base/speed per item. The steeleaf
    # sword really is registered on TFToolMaterials.KNIGHTMETAL.)
    "ironwood_sword": ("sword", "IRONWOOD", 5.0, -2.4),
    "ironwood_shovel": ("shovel", "IRONWOOD", 3.5, -3.0),
    "ironwood_pickaxe": ("pickaxe", "IRONWOOD", 3.0, -2.8),
    "ironwood_axe": ("axe", "IRONWOOD", 8.0, -3.1),
    "ironwood_hoe": ("hoe", "IRONWOOD", 0.0, -1.0),
    "steeleaf_sword": ("sword", "KNIGHTMETAL", 6.0, -2.4),
    "steeleaf_shovel": ("shovel", "STEELEAF", 4.5, -3.0),
    "steeleaf_pickaxe": ("pickaxe", "STEELEAF", 4.0, -2.8),
    "steeleaf_axe": ("axe", "STEELEAF", 9.0, -3.0),
    "steeleaf_hoe": ("hoe", "STEELEAF", 0.0, -0.5),
    "knightmetal_sword": ("sword", "KNIGHTMETAL", 6.0, -2.4),
    "knightmetal_pickaxe": ("pickaxe", "KNIGHTMETAL", 4.0, -2.8),
    "knightmetal_axe": ("axe", "KNIGHTMETAL", 9.0, -3.2),
    "fiery_sword": ("sword", "FIERY", 7.0, -2.4),
    "fiery_pickaxe": ("pickaxe", "FIERY", 5.0, -2.8),
}

# ── Engine-only armour ────────────────────────────────────────────────────
# Mod armour no ArmorMaterials.java has: slug -> (slot group, material,
# defense, toughness, knockback resistance), from AetherArmorMaterials /
# TFArmorMaterials (defense per ArmorType, then the material's toughness and
# knockback resistance).
def _armor_set(prefix, material, boots, legs, chest, helm, toughness, kb=0.0, pieces=None):
    out = {}
    for piece, group, d in (("helmet", "head", helm), ("chestplate", "chest", chest),
                            ("leggings", "legs", legs), ("boots", "feet", boots)):
        if pieces is None or piece in pieces:
            out["%s_%s" % (prefix, piece)] = (group, material, float(d), toughness, kb)
    return out


ENGINE_ONLY_ARMOR = {
    **_armor_set("zanite", "ZANITE", 2, 5, 6, 2, 0.0),
    **_armor_set("gravitite", "GRAVITITE", 3, 6, 8, 3, 2.0),
    **_armor_set("ironwood", "IRONWOOD", 2, 5, 7, 2, 0.0),
    **_armor_set("steeleaf", "STEELEAF", 3, 6, 8, 3, 0.0),
    **_armor_set("knightmetal", "KNIGHTMETAL", 3, 6, 8, 3, 1.0),
    **_armor_set("fiery", "FIERY", 4, 7, 9, 4, 1.5),
    **_armor_set("naga", "NAGA", 3, 6, 7, 2, 0.5, pieces=("chestplate", "leggings")),
}

# The kinds worth naming in C++. `sword` is the one with behaviour attached —
# MC's sweep attack is gated on ItemTags.SWORDS, whose contents are exactly the
# items registered through `.sword(...)`, so the parse IS the tag.
KINDS = ("sword", "axe", "pickaxe", "shovel", "hoe", "trident", "mace", "spear")


def camel(slug):
    return "".join(p.capitalize() for p in slug.split("_"))


def cf(v):
    s = repr(round(float(v), 4))
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def main():
    mat_src = open(MATERIAL, encoding="utf-8").read()
    bonus = {m.group(1): float(m.group(2)) for m in MATERIAL_BONUS.finditer(mat_src)}
    if not bonus:
        raise SystemExit("no ToolMaterial rows parsed — ToolMaterial.java changed shape")

    items_src = open(ITEMS, encoding="utf-8").read()

    rows = {}
    for m in PROP_FORM.finditer(items_src):
        slug, kind, mat, dmg, spd = m.groups()
        rows[slug] = (kind, mat, float(dmg) + bonus.get(mat, 0.0), float(spd))
    for m in CTOR_FORM.finditer(items_src):
        slug, kind, mat, dmg, spd = m.groups()
        rows[slug] = (kind.lower(), mat, float(dmg) + bonus.get(mat, 0.0), float(spd))

    for m in SPEAR_FORM.finditer(items_src):
        slug_lit, slug_id, mat, duration = m.groups()
        slug = slug_lit or slug_id.lower()
        rows[slug] = ("spear", mat, 0.0 + bonus.get(mat, 0.0), 1.0 / float(duration) - 4.0)

    # Trident and mace: modifiers built in their own class, already final (no
    # material bonus to add).
    for m in ATTR_CLASS_FORM.finditer(items_src):
        slug, cls = m.groups()
        path = os.path.join(MC, "world/item", cls + ".java")
        if not os.path.exists(path):
            print("  %s: no source for %s.createAttributes()" % (slug, cls))
            continue
        found = dict(ATTR_BUILDER_FORM.findall(open(path, encoding="utf-8").read()))
        if "ATTACK_DAMAGE" not in found:
            continue   # a non-weapon that still ships attributes
        rows[slug] = (slug, "-",
                      float(found["ATTACK_DAMAGE"]),
                      float(found.get("ATTACK_SPEED", 0.0)))

    if not rows:
        raise SystemExit("no weapon rows parsed — Items.java changed shape")
    for slug, row in ENGINE_ONLY_WEAPONS.items():
        if slug in rows:
            raise SystemExit("engine-only weapon '%s' collides with an MC item" % slug)
        rows[slug] = row

    # ── Armour (ArmorMaterial.createAttributes): ARMOR = defense for the
    # type, ARMOR_TOUGHNESS = the material's, KNOCKBACK_RESISTANCE only when
    # > 0. All ADD_VALUE on the piece's own slot group. ──
    materials = {}
    for m in ARMOR_MATERIAL_ROW.finditer(open(ARMOR_MATERIALS, encoding="utf-8").read()):
        name, boots, legs, chest, helm, body, toughness, kb = m.groups()
        materials[name] = ((int(boots), int(legs), int(chest), int(helm), int(body)),
                           float(toughness), float(kb))
    if not materials:
        raise SystemExit("no ArmorMaterial rows parsed — ArmorMaterials.java changed shape")
    armor = {}
    for m in ARMOR_FORM.finditer(items_src):
        slug_lit, slug_id, form, mat, atype = m.groups()
        slug = slug_lit or slug_id.lower()
        if form != "humanoidArmor":
            atype = "BODY"
        if mat not in materials or atype not in ARMOR_TYPES:
            print("  %s: unknown material %s / type %s" % (slug, mat, atype))
            continue
        defense, toughness, kb = materials[mat]
        idx, group = ARMOR_TYPES[atype]
        armor[slug] = (group, mat, float(defense[idx]), toughness, kb)
    if not armor:
        raise SystemExit("no armour rows parsed — Items.java changed shape")
    for slug, row in ENGINE_ONLY_ARMOR.items():
        if slug in armor:
            raise SystemExit("engine-only armour '%s' collides with an MC item" % slug)
        armor[slug] = row

    # Resolve slugs against the generated item table so a weapon MC has and this
    # port does not is dropped loudly rather than emitting a dangling id.
    known = set(re.findall(r'//\s*"([a-z0-9_]+)"',
                           open("src/common/entity/GeneratedItemList.hpp", encoding="utf-8").read()))
    missing = sorted(s for s in rows if s not in known)

    emitted = [(s, rows[s]) for s in sorted(rows) if s in known]
    armor_missing = sorted(s for s in armor if s not in known)
    armor_emitted = [(s, armor[s]) for s in sorted(armor) if s in known]
    armor_body = ['    {{ "{}", ArmorSlotGroup::{}, {}, {}, {} }},   // {}'.format(
        slug, camel(group), cf(defense), cf(toughness), cf(kb), mat.lower())
        for slug, (group, mat, defense, toughness, kb) in armor_emitted]

    body = ['    {{ "{}", {}, {}, WeaponKind::{} }},   // {} {}'.format(
        slug, cf(dmg), cf(spd), camel(kind) if kind in KINDS else "Other",
        mat.lower(), kind)
        for slug, (kind, mat, dmg, spd) in emitted]

    hpp = """// GENERATED by tools/gen_item_attributes.py — do not edit by hand.
//
// MC's per-weapon ATTACK_DAMAGE and ATTACK_SPEED modifiers, from Items.java
// combined with ToolMaterial.java.
//
// Both are ADD_VALUE modifiers applied while the item is in the MAIN HAND, on
// top of the player's own base attributes (ATTACK_DAMAGE 1.0, ATTACK_SPEED
// 4.0). So an iron sword's row of (5.0, -2.4) means 1 + 5 = 6 damage and
// 4 - 2.4 = 1.6 attacks/second, which is 20 / 1.6 = 12.5 ticks per full swing.
//
// A weapon's damage is deliberately NOT one number in MC: it is a per-kind
// baseline plus a per-material bonus, so every sword shares 3.0 and differs
// only by its material. The generator does that addition.
#pragma once

#include <cstdint>
#include <string_view>

namespace Game {

    // MC's player base attributes, from Player.createAttributes(). A weapon's
    // row is ADDED to these — which is why an iron sword (row +5) reads as 6
    // damage in game and swings at 4.0 - 2.4 = 1.6 per second.
    inline constexpr float kPlayerBaseAttackDamage = 1.0f;
    inline constexpr float kPlayerBaseAttackSpeed  = 4.0f;

    // What the item is, for the handful of rules that ask. MC expresses this
    // as item TAGS (ItemTags.SWORDS and friends); this port has no runtime tag
    // system, and the tag's contents are exactly what the registration form in
    // Items.java already tells us, so the kind rides along on this row.
    enum class WeaponKind : uint8_t {
        Other = 0, Sword, Axe, Pickaxe, Shovel, Hoe, Trident, Mace, Spear
    };

    struct ItemAttributeRow {
        std::string_view slug;
        float attackDamage;   // ADD_VALUE onto ATTACK_DAMAGE, main hand
        float attackSpeed;    // ADD_VALUE onto ATTACK_SPEED, main hand
        WeaponKind kind;
    };

    inline constexpr int kItemAttributeCount = %d;
    extern const ItemAttributeRow kItemAttributes[kItemAttributeCount];

    // MC's per-piece armour modifiers (ArmorMaterial.createAttributes): the
    // ARMOR points of that piece, the material's ARMOR_TOUGHNESS and its
    // KNOCKBACK_RESISTANCE, all ADD_VALUE on the piece's own slot group —
    // what "When on Head: +2 Armor" in the tooltip reads. Wolf and horse
    // armour are ArmorType.BODY ("When equipped:").
    enum class ArmorSlotGroup : uint8_t { Head, Chest, Legs, Feet, Body };

    struct ItemArmorRow {
        std::string_view slug;
        ArmorSlotGroup   slot;
        float            armor;                 // ADD_VALUE onto ARMOR
        float            armorToughness;        // ADD_VALUE onto ARMOR_TOUGHNESS
        float            knockbackResistance;   // ADD_VALUE onto KNOCKBACK_RESISTANCE (0 = no modifier)
    };

    inline constexpr int kItemArmorCount = %d;
    extern const ItemArmorRow kItemArmorAttributes[kItemArmorCount];

    // Resolves slugs to ItemIDs once at startup, like the recipe and mob-food
    // tables. Safe to call more than once.
    void InitItemAttributes();

    // The main-hand modifiers for an item, or (0, 0) for anything that is not a
    // weapon — which is MC's answer too: a snowball adds nothing, so a player
    // holding one punches for the bare-hand 1.0.
    void GetItemAttackAttributes(uint32_t itemId, float& outDamage, float& outSpeed);

    WeaponKind GetWeaponKind(uint32_t itemId);

    // Whether the item carries main-hand attack modifiers AT ALL — the
    // tooltip's question. A diamond hoe's modifiers are (0, 0) and still
    // print " 1 Attack Damage" / " 4 Attack Speed" in vanilla, so "both
    // zero" cannot mean "no weapon".
    bool HasItemAttackAttributes(uint32_t itemId);

    // The armour row for an item, or null for anything that is not armour.
    const ItemArmorRow* GetItemArmorAttributes(uint32_t itemId);

    // MC ItemTags.SWORDS — the sweep attack's gate (Player.isSweepAttack).
    inline bool IsSwordItem(uint32_t itemId) {
        return GetWeaponKind(itemId) == WeaponKind::Sword;
    }

} // namespace Game
""" % (len(emitted), len(armor_emitted))

    cpp = "\n".join([
        "// GENERATED by tools/gen_item_attributes.py — do not edit by hand.",
        '#include "common/entity/GeneratedItemAttributes.hpp"',
        '#include "common/world/crafting/RecipeManager.hpp"',
        '#include "common/core/Log.hpp"',
        "",
        "#include <string>",
        "#include <unordered_map>",
        "",
        "namespace Game {",
        "",
        "    const ItemAttributeRow kItemAttributes[kItemAttributeCount] = {",
        *body,
        "    };",
        "",
        "    const ItemArmorRow kItemArmorAttributes[kItemArmorCount] = {",
        *armor_body,
        "    };",
        "",
        "    namespace {",
        "        std::unordered_map<uint32_t, const ItemAttributeRow*> g_byId;",
        "        std::unordered_map<uint32_t, const ItemArmorRow*> g_armorById;",
        "        bool g_initialised = false;",
        "    }",
        "",
        "    void InitItemAttributes() {",
        "        if (g_initialised) return;",
        "        g_initialised = true;",
        "        for (const ItemAttributeRow& row : kItemAttributes) {",
        "            const ItemID id = RecipeManager::ItemFromSlug(std::string(row.slug));",
        "            if (id != Items::Air) g_byId[static_cast<uint32_t>(id)] = &row;",
        "        }",
        "        for (const ItemArmorRow& row : kItemArmorAttributes) {",
        "            const ItemID id = RecipeManager::ItemFromSlug(std::string(row.slug));",
        "            if (id != Items::Air) g_armorById[static_cast<uint32_t>(id)] = &row;",
        "        }",
        "",
        "        // Boot line, because the failure is otherwise invisible: an",
        "        // unresolved table leaves every weapon at (0, 0), which reads in",
        "        // game as a netherite axe hitting for the bare-hand 1.0 rather",
        "        // than as anything obviously broken. Must run AFTER",
        "        // RecipeManager::Initialize — that owns the slug map.",
        "        Log::Info(\"[ItemAttributes] %zu/%d weapons, %zu/%d armour pieces resolved\",",
        "                  g_byId.size(), kItemAttributeCount, g_armorById.size(), kItemArmorCount);",
        "    }",
        "",
        "    bool HasItemAttackAttributes(uint32_t itemId) {",
        "        return g_byId.find(itemId) != g_byId.end();",
        "    }",
        "",
        "    const ItemArmorRow* GetItemArmorAttributes(uint32_t itemId) {",
        "        const auto it = g_armorById.find(itemId);",
        "        return it == g_armorById.end() ? nullptr : it->second;",
        "    }",
        "",
        "    void GetItemAttackAttributes(uint32_t itemId, float& outDamage, float& outSpeed) {",
        "        outDamage = 0.0f;",
        "        outSpeed  = 0.0f;",
        "        const auto it = g_byId.find(itemId);",
        "        if (it == g_byId.end()) return;",
        "        outDamage = it->second->attackDamage;",
        "        outSpeed  = it->second->attackSpeed;",
        "    }",
        "",
        "    WeaponKind GetWeaponKind(uint32_t itemId) {",
        "        const auto it = g_byId.find(itemId);",
        "        return it == g_byId.end() ? WeaponKind::Other : it->second->kind;",
        "    }",
        "",
        "} // namespace Game",
        "",
    ])

    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp)

    print("%s: %d weapons, %d armour pieces" % (OUT_HPP, len(emitted), len(armor_emitted)))
    if missing:
        print("  %d MC weapons this port has no item for: %s"
              % (len(missing), ", ".join(missing)))
    if armor_missing:
        print("  %d MC armour pieces this port has no item for: %s"
              % (len(armor_missing), ", ".join(armor_missing)))
    if "-v" in sys.argv:
        for slug, (kind, mat, dmg, spd) in emitted:
            print("    %-18s %-8s dmg +%-5g speed %-6g -> %.4g/s, %.4g ticks"
                  % (slug, kind, dmg, spd, 4.0 + spd, 20.0 / (4.0 + spd)))


if __name__ == "__main__":
    main()
