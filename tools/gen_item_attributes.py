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

MC = "minecraft_code/decompiled_net/minecraft"
ITEMS = os.path.join(MC, "world/item/Items.java")
MATERIAL = os.path.join(MC, "world/item/ToolMaterial.java")
OUT_HPP = "src/common/entity/GeneratedItemAttributes.hpp"
OUT_CPP = "src/common/entity/GeneratedItemAttributes.cpp"

# `.sword(ToolMaterial.IRON, 3.0F, -2.4F)` and `.pickaxe(...)`.
PROP_FORM = re.compile(
    r'registerItem\(\s*"([a-z_0-9]+)"\s*,\s*\(new Item\.Properties\(\)\)'
    r'\.(sword|pickaxe)\(\s*ToolMaterial\.([A-Z]+)\s*,\s*([-\d.]+)F?\s*,\s*([-\d.]+)F?\s*\)')

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


# The kinds worth naming in C++. `sword` is the one with behaviour attached —
# MC's sweep attack is gated on ItemTags.SWORDS, whose contents are exactly the
# items registered through `.sword(...)`, so the parse IS the tag.
KINDS = ("sword", "axe", "pickaxe", "shovel", "hoe", "trident", "mace")


def camel(slug):
    return "".join(p.capitalize() for p in slug.split("_"))


def cf(v):
    s = repr(round(float(v), 4))
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def main():
    mat_src = open(MATERIAL).read()
    bonus = {m.group(1): float(m.group(2)) for m in MATERIAL_BONUS.finditer(mat_src)}
    if not bonus:
        raise SystemExit("no ToolMaterial rows parsed — ToolMaterial.java changed shape")

    items_src = open(ITEMS).read()

    rows = {}
    for m in PROP_FORM.finditer(items_src):
        slug, kind, mat, dmg, spd = m.groups()
        rows[slug] = (kind, mat, float(dmg) + bonus.get(mat, 0.0), float(spd))
    for m in CTOR_FORM.finditer(items_src):
        slug, kind, mat, dmg, spd = m.groups()
        rows[slug] = (kind.lower(), mat, float(dmg) + bonus.get(mat, 0.0), float(spd))

    # Trident and mace: modifiers built in their own class, already final (no
    # material bonus to add).
    for m in ATTR_CLASS_FORM.finditer(items_src):
        slug, cls = m.groups()
        path = os.path.join(MC, "world/item", cls + ".java")
        if not os.path.exists(path):
            print("  %s: no source for %s.createAttributes()" % (slug, cls))
            continue
        found = dict(ATTR_BUILDER_FORM.findall(open(path).read()))
        if "ATTACK_DAMAGE" not in found:
            continue   # a non-weapon that still ships attributes
        rows[slug] = (slug, "-",
                      float(found["ATTACK_DAMAGE"]),
                      float(found.get("ATTACK_SPEED", 0.0)))

    if not rows:
        raise SystemExit("no weapon rows parsed — Items.java changed shape")

    # Resolve slugs against the generated item table so a weapon MC has and this
    # port does not is dropped loudly rather than emitting a dangling id.
    known = set(re.findall(r'//\s*"([a-z0-9_]+)"',
                           open("src/common/entity/GeneratedItemList.hpp").read()))
    missing = sorted(s for s in rows if s not in known)

    emitted = [(s, rows[s]) for s in sorted(rows) if s in known]

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
        Other = 0, Sword, Axe, Pickaxe, Shovel, Hoe, Trident, Mace
    };

    struct ItemAttributeRow {
        std::string_view slug;
        float attackDamage;   // ADD_VALUE onto ATTACK_DAMAGE, main hand
        float attackSpeed;    // ADD_VALUE onto ATTACK_SPEED, main hand
        WeaponKind kind;
    };

    inline constexpr int kItemAttributeCount = %d;
    extern const ItemAttributeRow kItemAttributes[kItemAttributeCount];

    // Resolves slugs to ItemIDs once at startup, like the recipe and mob-food
    // tables. Safe to call more than once.
    void InitItemAttributes();

    // The main-hand modifiers for an item, or (0, 0) for anything that is not a
    // weapon — which is MC's answer too: a snowball adds nothing, so a player
    // holding one punches for the bare-hand 1.0.
    void GetItemAttackAttributes(uint32_t itemId, float& outDamage, float& outSpeed);

    WeaponKind GetWeaponKind(uint32_t itemId);

    // MC ItemTags.SWORDS — the sweep attack's gate (Player.isSweepAttack).
    inline bool IsSwordItem(uint32_t itemId) {
        return GetWeaponKind(itemId) == WeaponKind::Sword;
    }

} // namespace Game
""" % len(emitted)

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
        "    namespace {",
        "        std::unordered_map<uint32_t, const ItemAttributeRow*> g_byId;",
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
        "",
        "        // Boot line, because the failure is otherwise invisible: an",
        "        // unresolved table leaves every weapon at (0, 0), which reads in",
        "        // game as a netherite axe hitting for the bare-hand 1.0 rather",
        "        // than as anything obviously broken. Must run AFTER",
        "        // RecipeManager::Initialize — that owns the slug map.",
        "        Log::Info(\"[ItemAttributes] %zu/%d weapons resolved\",",
        "                  g_byId.size(), kItemAttributeCount);",
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

    open(OUT_HPP, "w").write(hpp)
    open(OUT_CPP, "w").write(cpp)

    print("%s: %d weapons" % (OUT_HPP, len(emitted)))
    if missing:
        print("  %d MC weapons this port has no item for: %s"
              % (len(missing), ", ".join(missing)))
    if "-v" in sys.argv:
        for slug, (kind, mat, dmg, spd) in emitted:
            print("    %-18s %-8s dmg +%-5g speed %-6g -> %.4g/s, %.4g ticks"
                  % (slug, kind, dmg, spd, 4.0 + spd, 20.0 / (4.0 + spd)))


if __name__ == "__main__":
    main()
