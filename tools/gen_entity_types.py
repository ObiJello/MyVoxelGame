#!/usr/bin/env python3
"""Generate src/common/entity/GeneratedEntityTypes.{hpp,cpp} from MC's EntityType.java.

Every mob type MC registers with a MobCategory other than MISC, plus the few
MISC ones this engine needs (arrow), with the exact numbers from the decompile:
size, eye height, client tracking range, update interval and the peaceful flag.

APPEND-AWARE, like tools/gen_items.py. The enum index is the wire id — it is
what AddEntityS2C sends — so an existing entry may never move. The generator
reads the order out of the previously generated header and appends anything new
at the end. Delete the header to re-derive the order from scratch, and expect
every client and save to disagree with every other one if you do.

    python3 tools/gen_entity_types.py
"""

import os
import re
import sys

MC = "minecraft_code/decompiled_net/minecraft"
SRC = os.path.join(MC, "world/entity/EntityType.java")
OUT_HPP = "src/common/entity/GeneratedEntityTypes.hpp"
OUT_CPP = "src/common/entity/GeneratedEntityTypes.cpp"

# The order the hand-written table used before this generator existed. Seeds the
# id space so nothing already on the wire moves.
LEGACY_ORDER = [
    "zombie", "skeleton", "creeper", "spider",
    "cow", "pig", "sheep", "chicken", "arrow",
]

# MISC types this engine models anyway. Everything else with MobCategory.MISC is
# a projectile, a vehicle or a display block — none of which the mob system owns.
#
# The four golem/villager rows are MISC in MC only because MISC means "never
# naturally spawned by NaturalSpawner" (its cap is -1) — they are real Mob
# subclasses with models, attributes and spawn eggs, so the mob system does own
# them. Leaving them out is what left their four spawn eggs inert.
MISC_KEEP = {
    "arrow",
    "villager", "iron_golem", "snow_golem", "copper_golem",
    # Non-arrow projectiles (2026-08). Entities, not mobs: they ride the mob
    # pipeline the way the arrow does (see projectile/Arrow.hpp) but have no
    # MobDef, no goals and no spawn entries — gen_mob_defs.py skips them.
    "snowball", "egg", "splash_potion",
    "small_fireball", "fireball", "dragon_fireball", "wither_skull",
    "shulker_bullet", "llama_spit", "trident",
    "wind_charge", "breeze_wind_charge",
    # The evoker's summoned fang trap (2026-08, evoker promotion). A plain
    # Entity in MC, not a projectile, but it rides the same Misc pipeline.
    "evoker_fangs",
    # The lingering-potion cloud (2026-08, potion work). Was in the generated
    # header without a MISC_KEEP row, which made a regeneration silently DROP
    # it (and EntityTypeId::AreaEffectCloud with it) — the row here is what
    # keeps the generator's output a superset of the code's references.
    "area_effect_cloud",
    # The thrown Eye of Ender (portal work). MISC in MC and not a projectile
    # subclass there either — it is a plain Entity with its own flight code —
    # but it rides this engine's mob pipeline the way the arrow does, so it
    # needs a type id, a size and a tracking range like any other.
    "eye_of_ender",
    # Block-shaped entities (2026-08, falling blocks + TNT). Neither is a Mob
    # in MC — both are plain Entity — but both ride this engine's Mob pipeline
    # for the same reason the projectiles do: tracking, the wire, NBT and the
    # client factory are all Mob-shaped. See the architecture note at the top
    # of common/entity/FallingBlockEntity.hpp. Neither has a MobDef, goals or
    # a spawn entry, so gen_mob_defs.py skips them.
    "falling_block", "tnt",
}

CATEGORY_CPP = {
    "MONSTER": "MobCategory::Monster",
    "CREATURE": "MobCategory::Creature",
    "AMBIENT": "MobCategory::Ambient",
    "AXOLOTLS": "MobCategory::Axolotls",
    "UNDERGROUND_WATER_CREATURE": "MobCategory::UndergroundWaterCreature",
    "WATER_CREATURE": "MobCategory::WaterCreature",
    "WATER_AMBIENT": "MobCategory::WaterAmbient",
    "MISC": "MobCategory::Misc",
}

# The head of each registration. The TAIL (the builder chain) cannot be matched
# with a regex — it nests parentheses — so it is extracted by balancing them.
# Getting this wrong is quiet: a truncated tail simply misses `.clientTrackingRange`
# and `.notInPeaceful()`, and every value silently falls back to its default.
ROW_HEAD = re.compile(
    r'register\(\s*"([a-z_]+)"\s*,\s*EntityType\.Builder\.'
    r'(?:of|createNothing|<[^>]*>of)\([^,]*,\s*MobCategory\.([A-Z_]+)\)'
)


def builder_tail(text, start):
    """The builder chain from `start` to the paren that closes register(...)."""
    depth = 1  # we are inside register(
    i = start
    while i < len(text) and depth > 0:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    return text[start:i]


def camel(slug):
    return "".join(p.capitalize() for p in slug.split("_"))


def parse():
    text = open(SRC, encoding="utf-8").read()
    out = {}
    rows = []
    for m in ROW_HEAD.finditer(text):
        rows.append((m.group(1), m.group(2), builder_tail(text, m.end())))
    for slug, cat, tail in rows:
        if cat == "MISC" and slug not in MISC_KEEP:
            continue

        def num(name, default):
            m = re.search(name + r"\(([-0-9.]+)F?\)", tail)
            return float(m.group(1)) if m else default

        m = re.search(r"sized\(([-0-9.]+)F,\s*([-0-9.]+)F\)", tail)
        width, height = (float(m.group(1)), float(m.group(2))) if m else (1.0, 1.0)

        # MC's default when .eyeHeight() is absent (EntityDimensions.scalable).
        eye = num("eyeHeight", height * 0.85)

        out[slug] = dict(
            slug=slug,
            category=cat,
            width=width,
            height=height,
            eye=eye,
            track=int(num("clientTrackingRange", 5)),
            update=int(num("updateInterval", 3)),
            peaceful="notInPeaceful()" in tail,
        )
    return out


def existing_order():
    """Ids already published, in order, so they keep their wire value."""
    if not os.path.exists(OUT_HPP):
        return list(LEGACY_ORDER)
    order = []
    for line in open(OUT_HPP, encoding="utf-8"):
        m = re.search(r'^\s*([A-Za-z0-9]+)\s*(?:=\s*\d+\s*)?,\s*//\s*"([a-z_]+)"', line)
        if m:
            order.append(m.group(2))
    return order or list(LEGACY_ORDER)


def main():
    if not os.path.exists(SRC):
        sys.exit(f"missing {SRC} — run from the repo root")

    types = parse()
    order = [s for s in existing_order() if s in types]
    order += sorted(s for s in types if s not in order)

    hpp = []
    hpp.append("// GENERATED by tools/gen_entity_types.py — do not edit by hand.")
    hpp.append("//")
    hpp.append("// Every number here is transcribed from MC's EntityType.java. They are not")
    hpp.append("// tunable: `sized(0.6F, 1.95F)` really is 1.95, and the difference is whether")
    hpp.append("// a zombie fits under a two-block ceiling.")
    hpp.append("//")
    hpp.append("// ORDERING IS WIRE-VISIBLE. AddEntityS2C sends the type as an index into this")
    hpp.append("// enum, so entries may be APPENDED but never reordered or removed. The")
    hpp.append("// generator preserves the existing order for exactly that reason.")
    hpp.append("#pragma once")
    hpp.append("")
    hpp.append('#include "common/entity/MobCategory.hpp"')
    hpp.append("")
    hpp.append("#include <cstdint>")
    hpp.append("#include <string_view>")
    hpp.append("")
    hpp.append("namespace Game {")
    hpp.append("")
    hpp.append("    enum class EntityTypeId : uint16_t {")
    for i, slug in enumerate(order):
        hpp.append(f'        {camel(slug)} = {i},  // "{slug}"')
    hpp.append("        Count")
    hpp.append("    };")
    hpp.append("")
    hpp.append("    struct EntityTypeInfo {")
    hpp.append("        std::string_view slug;")
    hpp.append("        float       width;")
    hpp.append("        float       height;")
    hpp.append("        // MC's default when `.eyeHeight(...)` is absent is height * 0.85,")
    hpp.append("        // already folded in here so nothing has to remember the rule.")
    hpp.append("        float       eyeHeight;")
    hpp.append("        MobCategory category;")
    hpp.append("        // clientTrackingRange, in CHUNKS. Multiply by 16 for blocks.")
    hpp.append("        int         clientTrackingRange;")
    hpp.append("        // ServerEntity.updateInterval — ticks between periodic position sends.")
    hpp.append("        int         updateInterval;")
    hpp.append("        // Eye height when this type is a baby. 0 means 'derive it'")
    hpp.append("        // (eyeHeight * 0.5); MC hardcodes an override for the mobs whose baby")
    hpp.append("        // model has a proportionally larger head.")
    hpp.append("        float       babyEyeHeight;")
    hpp.append("        // MC Mob.xpReward as seeded by the entity's constructor chain")
    hpp.append("        // (Monster.java:34 base 5, per-mob ctor overrides — see the")
    hpp.append("        // generator's XP_OVERRIDES). Mob::GetXpReward reads this; mobs whose")
    hpp.append("        // reward is dynamic (animals 1..3, slime size, baby zombie x2.5)")
    hpp.append("        // override GetXpReward in code instead.")
    hpp.append("        int         xpReward;")
    hpp.append("        bool        notInPeaceful;")
    hpp.append("    };")
    hpp.append("")
    hpp.append("    // LivingEntity.DEFAULT_BABY_SCALE — one value for every type.")
    hpp.append("    inline constexpr float kBabyScale = 0.5f;")
    hpp.append("")
    hpp.append(f"    inline constexpr int kEntityTypeCount = {len(order)};")
    hpp.append("    extern const EntityTypeInfo kEntityTypeTable[kEntityTypeCount];")
    hpp.append("")
    hpp.append("    inline const EntityTypeInfo& GetEntityTypeInfo(EntityTypeId t) {")
    hpp.append("        return kEntityTypeTable[static_cast<size_t>(t)];")
    hpp.append("    }")
    hpp.append("")
    hpp.append("    inline bool IsValidEntityType(uint16_t raw) {")
    hpp.append("        return raw < static_cast<uint16_t>(EntityTypeId::Count);")
    hpp.append("    }")
    hpp.append("")
    hpp.append("    // Eye height for an instance, honouring the baby override.")
    hpp.append("    inline float GetEyeHeight(EntityTypeId t, bool baby) {")
    hpp.append("        const EntityTypeInfo& info = GetEntityTypeInfo(t);")
    hpp.append("        if (!baby) return info.eyeHeight;")
    hpp.append("        if (info.babyEyeHeight > 0.0f) return info.babyEyeHeight;")
    hpp.append("        return info.eyeHeight * kBabyScale;")
    hpp.append("    }")
    hpp.append("")
    hpp.append("} // namespace Game")
    hpp.append("")

    # Baby eye heights MC hardcodes. Everything else derives.
    BABY_EYE = {"zombie": 0.93, "cow": 0.665, "chicken": 0.2975}

    # MC seeds Mob.xpReward in constructors: Monster.java:34 sets 5 and these
    # ctors override it (values transcribed from the decompile, cited by
    # site). Everything DYNAMIC stays a C++ override on the mob class:
    # Animal 1+rand(3) (Animal.java:123), WaterAnimal/AgeableWaterCreature
    # ditto, Slime = its size (Slime.java:100), baby zombie x2.5
    # (Zombie.java:165-170), baby hoglin 3 (Hoglin.java:161), chicken jockey
    # 10 (Chicken.java:166-167), tadpole none (Tadpole.java:208).
    XP_OVERRIDES = {
        "blaze":         10,    # Blaze.java:41
        "guardian":      10,    # Guardian.java:63
        "elder_guardian": 10,   # inherits Guardian's ctor (no own write)
        "evoker":        10,    # Evoker.java:51
        "breeze":        10,    # Breeze.java:70
        "ravager":       20,    # Ravager.java:67
        "piglin_brute":  20,    # PiglinBrute.java:44
        "endermite":     3,     # Endermite.java:36
        "vex":           3,     # Vex.java:59
        "wither":        50,    # WitherBoss.java:84
        "creaking":      0,     # Creaking.java:91 — the heart's construct
        # EnderDragon pays its 12000 (500 when respawned) in tranches over the
        # death animation (EnderDragon.java:513,534); the respawned-dragon
        # half and the tranche schedule are not modelled — one lump here.
        "ender_dragon":  12000,
    }

    def xp(slug, cat):
        return XP_OVERRIDES.get(slug, 5 if cat == "MONSTER" else 0)

    cpp = []
    cpp.append("// GENERATED by tools/gen_entity_types.py — do not edit by hand.")
    cpp.append('#include "common/entity/GeneratedEntityTypes.hpp"')
    cpp.append("")
    cpp.append("namespace Game {")
    cpp.append("")
    cpp.append("    const EntityTypeInfo kEntityTypeTable[kEntityTypeCount] = {")
    cpp.append("        // slug  w  h  eye  category  track  upd  babyEye  xp  notInPeaceful")
    for slug in order:
        t = types[slug]
        cpp.append(
            '        {{ "{slug}", {w}f, {h}f, {e}f, {cat}, {tr}, {up}, {be}f, {xp}, {pf} }},'.format(
                slug=slug,
                w=round(t["width"], 5),
                h=round(t["height"], 5),
                e=round(t["eye"], 5),
                cat=CATEGORY_CPP[t["category"]],
                tr=t["track"],
                up=t["update"],
                be=BABY_EYE.get(slug, 0.0),
                xp=xp(slug, t["category"]),
                pf="true" if t["peaceful"] else "false",
            )
        )
    cpp.append("    };")
    cpp.append("")
    cpp.append("} // namespace Game")
    cpp.append("")

    open(OUT_HPP, "w", encoding="utf-8").write("\n".join(hpp))
    open(OUT_CPP, "w", encoding="utf-8").write("\n".join(cpp))
    print(f"{OUT_HPP}: {len(order)} entity types")
    kept = [s for s in LEGACY_ORDER if s in order]
    print(f"  legacy ids preserved: {', '.join(kept)}")


if __name__ == "__main__":
    main()
