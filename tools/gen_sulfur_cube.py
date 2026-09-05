#!/usr/bin/env python3
"""Bake MC 26.3's sulfur cube archetype registry into C++.

MC keeps the sulfur cube's behaviour as DATA: `data/minecraft/sulfur_cube_
archetype/<name>.json` (a registry, `SulfurCubeArchetype` records) says what
swallowing a block from its item tag does — attribute modifiers on five
attributes (SulfurCubeArchetypes.archetype(speed, bounce, friction, drag)),
buoyancy, an optional explosion, optional contact damage, the knockback
powers a hit transfers and the push-sound settings — and
`data/minecraft/tags/item/sulfur_cube_archetype/<name>.json` says which
blocks belong to it. `sulfur_cube_swallowable` is the union of those tags,
`sulfur_cube_food` what a baby eats.

Reads (copied out of the 26.3-pre-2 jar, see CLAUDE.md):
    data/minecraft/sulfur_cube_archetype/*.json
    data/minecraft/tags/item/sulfur_cube_archetype/*.json
    data/minecraft/tags/item/sulfur_cube_food.json
    data/minecraft/tags/item/*.json           (nested tags: #planks, #wool …)
Emits:
    src/common/entity/mobs/GeneratedSulfurCubeArchetypes.{hpp,cpp}

Items are emitted as SLUGS: the engine resolves them once at runtime through
RecipeManager::ItemFromSlug (a block item's id IS its block id), so this table
needs no item constants and survives item-list regeneration. A slug that
names a block this engine does not have resolves to Air and is skipped — the
cube simply cannot swallow it.

    python3 tools/gen_sulfur_cube.py
"""
import glob
import json
import os
import sys

ARCH_DIR = "data/minecraft/sulfur_cube_archetype"
TAG_DIR = "data/minecraft/tags/item"
FOOD_TAG = os.path.join(TAG_DIR, "sulfur_cube_food.json")
OUT_HPP = "src/common/entity/mobs/GeneratedSulfurCubeArchetypes.hpp"
OUT_CPP = "src/common/entity/mobs/GeneratedSulfurCubeArchetypes.cpp"

# SulfurCubeArchetypes.java's bootstrap order — the registry order MC's
# `matchingArchetypes` walks. The JSON files carry no order of their own.
ORDER = ["regular", "bouncy", "slow_bouncy", "slow_flat", "fast_flat", "light",
         "fast_sliding", "slow_sliding", "high_resistance", "sticky",
         "explosive", "hot"]


def bare(ident):
    return ident.split(":", 1)[1] if ":" in ident else ident


def camel(slug):
    return "".join(p.capitalize() for p in slug.split("_"))


def cf(v):
    s = "%.6g" % float(v)
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def resolve_tag(name, seen=None):
    """`#minecraft:planks` -> its item slugs, nested tags expanded."""
    seen = seen or set()
    name = bare(name.lstrip("#"))
    if name in seen:
        return []
    seen.add(name)
    path = os.path.join(TAG_DIR, name + ".json")
    if not os.path.exists(path):
        print(f"  WARNING: no item tag {name} — its blocks are not swallowable")
        return []
    out = []
    for v in json.load(open(path, encoding="utf-8"))["values"]:
        ident = v["id"] if isinstance(v, dict) else v
        if ident.startswith("#"):
            out += resolve_tag(ident, seen)
        else:
            out.append(bare(ident))
    return out


def amount_of(v):
    """MC FloatProvider: a bare number or {"type": "constant", "value": x}."""
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, dict):
        if "value" in v:
            return float(v["value"])
        if "min_inclusive" in v:   # uniform: emit its mean, note it
            return (float(v["min_inclusive"]) + float(v["max_exclusive"])) / 2.0
    return 0.0


def main():
    if not os.path.isdir(ARCH_DIR):
        sys.exit(f"missing {ARCH_DIR} — copy the 26.3 data first")

    archetypes = []
    swallowable = []          # (slug, archetype index)
    owner = {}
    for name in ORDER:
        path = os.path.join(ARCH_DIR, name + ".json")
        if not os.path.exists(path):
            print(f"  WARNING: archetype {name} missing")
            continue
        d = json.load(open(path, encoding="utf-8"))
        mods = {"knockback_resistance": 0.0, "explosion_knockback_resistance": 0.0,
                "bounciness": 0.0, "friction_modifier": 1.0, "air_drag_modifier": 1.0}
        for m in d.get("attribute_modifiers", []):
            attr = bare(m["attribute"])
            if attr not in mods:
                print(f"  WARNING: {name}: unknown attribute {attr}")
                continue
            if m["operation"] == "add_value":
                mods[attr] += float(m["amount"])
            elif m["operation"] == "add_multiplied_total":
                mods[attr] *= 1.0 + float(m["amount"])
            else:
                print(f"  WARNING: {name}: unsupported operation {m['operation']}")
        ex = d.get("explosion")
        cd = d.get("contact_damage")
        kb = d["knockback_modifiers"]
        snd = d["sound_settings"]
        archetypes.append(dict(
            name=name,
            kbRes=mods["knockback_resistance"],
            exKbRes=mods["explosion_knockback_resistance"],
            bounce=mods["bounciness"],
            friction=mods["friction_modifier"],
            drag=mods["air_drag_modifier"],
            buoyant=bool(d.get("buoyant", False)),
            explodes=ex is not None,
            exPower=int(ex["power"]) if ex else 0,
            exFire=bool(ex["causes_fire"]) if ex else False,
            exFuse=int(ex["fuse"]) if ex else 0,
            contact=cd is not None,
            contactAmount=amount_of(cd["amount"]) if cd else 0.0,
            contactToSource=bool(cd["attribute_to_source"]) if cd else False,
            contactType=bare(cd["damage_type"]) if cd else "",
            kbH=float(kb["horizontal_power"]),
            kbV=float(kb["vertical_power"]),
            pushThreshold=float(snd["push_sound_impulse_threshold"]),
            pushCooldown=float(snd["push_sound_cooldown"]),
        ))
        idx = len(archetypes) - 1
        items = resolve_tag(bare(d["items"]).lstrip("#")) if isinstance(d["items"], str) \
            else [bare(i) for i in d["items"]]
        for slug in items:
            if slug in owner:
                if owner[slug] != idx:
                    print(f"  WARNING: {slug} in both {ORDER[owner[slug]]} and {name}; "
                          f"keeping the first (MC applies every match — none overlap today)")
                continue
            owner[slug] = idx
            swallowable.append((slug, idx))

    food = resolve_tag("sulfur_cube_food") if os.path.exists(FOOD_TAG) else []

    enum = ",\n".join(f"        {camel(a['name'])}" for a in archetypes)
    hpp = f"""// GENERATED by tools/gen_sulfur_cube.py — do not edit by hand.
//
// MC 26.3's sulfur_cube_archetype registry: what a sulfur cube becomes when
// it swallows a block from each archetype's item tag. Attribute values are
// the FOLDED result of the JSON's modifiers on a fresh attribute (the
// engine's AttributeMap applies them as one add/multiply each); items are
// slugs resolved at runtime (see the generator's docstring).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Game {{

    enum class SulfurCubeArchetypeId : uint8_t {{
{enum},
        Count
    }};

    struct SulfurCubeArchetypeDef {{
        std::string_view name;
        // SulfurCubeArchetypes.archetype(speed, bounce, friction, drag), folded:
        float knockbackResistance;           // add_value -speed (clamped 0..1 by the attribute)
        float explosionKnockbackResistance;  // same amount
        float bounciness;                    // add_value bounce
        float frictionModifier;              // add_multiplied_total → ×friction
        float airDragModifier;               // add_multiplied_total → ×drag
        bool  buoyant;
        // ExplosionData(power, causesFire, fuse), or explodes = false.
        bool  explodes;
        int   explosionPower;
        bool  explosionFire;
        int   explosionFuse;
        // ContactDamage(damageType, amount, attributeToSource), or contact = false.
        bool  contactDamage;
        float contactDamageAmount;
        bool  contactDamageToSource;
        std::string_view contactDamageType;
        // KnockbackModifiers(horizontalPower, verticalPower).
        float knockbackHorizontal;
        float knockbackVertical;
        // SoundSettings' push threshold (blocks/tick, squared compare) and
        // cooldown (seconds) — the sounds themselves have nowhere to play.
        float pushSoundThreshold;
        float pushSoundCooldown;
    }};

    extern const SulfurCubeArchetypeDef kSulfurCubeArchetypes[static_cast<size_t>(SulfurCubeArchetypeId::Count)];

    // `#sulfur_cube_swallowable`, each item with the archetype whose tag
    // holds it, in registry order.
    struct SulfurCubeSwallowable {{
        std::string_view      itemSlug;
        SulfurCubeArchetypeId archetype;
    }};
    extern const SulfurCubeSwallowable kSulfurCubeSwallowable[];
    extern const size_t kSulfurCubeSwallowableCount;

    // `#sulfur_cube_food` — what a baby sulfur cube grows on.
    extern const std::string_view kSulfurCubeFood[];
    extern const size_t kSulfurCubeFoodCount;

}} // namespace Game
"""

    rows = []
    for a in archetypes:
        rows.append(
            f'        {{ "{a["name"]}", {cf(a["kbRes"])}, {cf(a["exKbRes"])}, {cf(a["bounce"])}, '
            f'{cf(a["friction"])}, {cf(a["drag"])}, {"true" if a["buoyant"] else "false"}, '
            f'{"true" if a["explodes"] else "false"}, {a["exPower"]}, {"true" if a["exFire"] else "false"}, {a["exFuse"]}, '
            f'{"true" if a["contact"] else "false"}, {cf(a["contactAmount"])}, '
            f'{"true" if a["contactToSource"] else "false"}, "{a["contactType"]}", '
            f'{cf(a["kbH"])}, {cf(a["kbV"])}, {cf(a["pushThreshold"])}, {cf(a["pushCooldown"])} }},')
    items = "\n".join(f'        {{ "{slug}", SulfurCubeArchetypeId::{camel(ORDER[idx])} }},'
                      for slug, idx in swallowable)
    foods = "\n".join(f'        "{f}",' for f in food) or '        "slime_ball",'
    cpp = f"""// GENERATED by tools/gen_sulfur_cube.py — do not edit by hand.
#include "common/entity/mobs/GeneratedSulfurCubeArchetypes.hpp"

namespace Game {{

    const SulfurCubeArchetypeDef kSulfurCubeArchetypes[static_cast<size_t>(SulfurCubeArchetypeId::Count)] = {{
        // name, kbRes, exKbRes, bounce, friction, drag, buoyant, explodes, power, fire, fuse, contact, amount, toSource, type, kbH, kbV, pushThreshold, pushCooldown
{chr(10).join(rows)}
    }};

    const SulfurCubeSwallowable kSulfurCubeSwallowable[] = {{
{items}
    }};
    const size_t kSulfurCubeSwallowableCount =
        sizeof(kSulfurCubeSwallowable) / sizeof(kSulfurCubeSwallowable[0]);

    const std::string_view kSulfurCubeFood[] = {{
{foods}
    }};
    const size_t kSulfurCubeFoodCount = sizeof(kSulfurCubeFood) / sizeof(kSulfurCubeFood[0]);

}} // namespace Game
"""
    open(OUT_HPP, "w", encoding="utf-8").write(hpp)
    open(OUT_CPP, "w", encoding="utf-8").write(cpp)
    print(f"{OUT_HPP}: {len(archetypes)} archetypes, {len(swallowable)} swallowable items, "
          f"{len(food)} foods")


if __name__ == "__main__":
    main()
