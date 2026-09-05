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
# MC 26.3 (minecraft_code2): the registrations moved to EntityTypes.java and
# key on EntityTypeIds.X instead of the string. Gameplay follows 26.3 here
# (the baby remodel's boxes already do), so every type present in BOTH trees
# takes its 26.3 box, eye height and peaceful flag from this file; the 26.1
# row only supplies what 26.3 does not restate. The differences that matter
# (2026-09-05): bee 0.7x0.6 -> 0.55x0.5, rabbit 0.4x0.5 -> 0.49x0.6 with an
# explicit 0.59 eye line, hoglin notInPeaceful.
SRC2 = "minecraft_code2/decompiled_net/minecraft/world/entity/EntityTypes.java"
# 26.3-only mobs this engine implements (2026-09-05: the sulfur cube —
# AbstractCubeMob sibling of the slime, ported as a standalone mob without
# its sulfur-caves biome).
MC2_ONLY = {"sulfur_cube"}
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
    # The End crystal (2026-09, dragon fight). A plain Entity in MC; rides the
    # Mob pipeline here like the other Misc entries. No MobDef, no goals, no
    # spawn entry — gen_mob_defs.py skips it.
    "end_crystal",
    # The thrown ender pearl (2026-09) — same projectile pipeline as the
    # snowball and the eye of ender.
    "ender_pearl",
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
# 26.3's form: `register(EntityTypeIds.ZOMBIE_HORSE, EntityType.Builder.of(
# ZombieHorse::new, MobCategory.MONSTER)...` — the slug is the lower-cased key.
ROW_HEAD2 = re.compile(
    r'register\(\s*EntityTypeIds\.([A-Z_0-9]+)\s*,\s*EntityType\.Builder\.'
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
    # 26.3 overlay: a type registered in both trees is read from the 26.3
    # builder chain instead (same fields, newer numbers). A type only 26.3
    # has (the sulfur cube) is NOT added here — a new mob needs its class,
    # model and data before a wire id is worth spending on it.
    if os.path.exists(SRC2):
        text2 = open(SRC2, encoding="utf-8").read()
        rows2 = {}
        for m in ROW_HEAD2.finditer(text2):
            rows2[m.group(1).lower()] = (m.group(2), builder_tail(text2, m.end()))
        rows = [(slug, rows2[slug][0], rows2[slug][1]) if slug in rows2
                else (slug, cat, tail) for slug, cat, tail in rows]
        # Types 26.3 registers that 26.1 never had. Opt-in, one by one: a
        # row here is a wire id, and a new mob only earns one once its class,
        # meshes and data are in place (each gets appended at the END of the
        # enum by existing_order, so nothing already on the wire moves).
        have = {slug for slug, _, _ in rows}
        for slug in sorted(MC2_ONLY):
            if slug in rows2 and slug not in have:
                rows.append((slug, rows2[slug][0], rows2[slug][1]))
    for slug, cat, tail in rows:
        if cat == "MISC" and slug not in MISC_KEEP:
            continue

        def num(name, default):
            # end_crystal passes Integer.MAX_VALUE for updateInterval; 26.3
            # spells the same thing `noUpdateInterval()`.
            if re.search(name + r"\(Integer\.MAX_VALUE\)", tail):
                return 2147483647
            if name == "updateInterval" and "noUpdateInterval()" in tail:
                return 2147483647
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
    hpp.append("        // MC 26.1 per-mob BABY_DIMENSIONS (Cow/Pig/Chicken/Rabbit/Sheep/Wolf/")
    hpp.append("        // Cat/Ocelot/Fox.java's `getDefaultDimensions`): the baby's own box when")
    hpp.append("        // it is NOT the adult's scaled by 0.5. 0 = derive (width * kBabyScale).")
    hpp.append("        float       babyWidth;")
    hpp.append("        float       babyHeight;")
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
    hpp.append("    // Baby bounding box for a type — the explicit 26.1 dimensions where MC")
    hpp.append("    // declares them, otherwise the adult's scaled by DEFAULT_BABY_SCALE.")
    hpp.append("    inline float GetBabyWidth(EntityTypeId t) {")
    hpp.append("        const EntityTypeInfo& info = GetEntityTypeInfo(t);")
    hpp.append("        return info.babyWidth > 0.0f ? info.babyWidth : info.width * kBabyScale;")
    hpp.append("    }")
    hpp.append("    inline float GetBabyHeight(EntityTypeId t) {")
    hpp.append("        const EntityTypeInfo& info = GetEntityTypeInfo(t);")
    hpp.append("        return info.babyHeight > 0.0f ? info.babyHeight : info.height * kBabyScale;")
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

    # Baby eye heights and boxes — MC 26.3's BABY_DIMENSIONS (minecraft_code2,
    # `<Mob>.java` static init: `EntityDimensions.scalable(w, h).withEyeHeight
    # (e)` or `<TYPE>.getDimensions().scale(s)...`) and the getAgeScale
    # overrides. The 26.x baby remodel gave nearly every baby its own box and
    # eye line, gameplay-side, so these apply whichever look is drawn.
    #
    # Eye heights: explicit where MC declares one, else eye * scale for the
    # mobs whose box is `getDimensions().scale(s)` with no eye of its own
    # (camel 0.6, turtle 0.3). Everything absent derives as eye * 0.5.
    BABY_EYE = {
        "zombie":           0.775,    # Zombie.java:491 (was the old 0.93)
        "husk":             0.825,    # Husk.java:128
        "drowned":          0.775,    # Drowned.java:312
        "zombie_villager":  0.67,     # ZombieVillager.java:363
        "zombified_piglin": 0.78,     # ZombifiedPiglin.java:223
        "piglin":           0.78,     # Piglin.java:404
        "villager":         0.63,     # Villager.java:904
        "cow":              0.69,     # Cow.java:138
        "mooshroom":        0.69,     # MushroomCow.java:252
        "chicken":          0.28125,  # Chicken.java:278
        "pig":              0.40625,  # Pig.java:314
        "rabbit":           0.39,     # Rabbit.java:86
        "sheep":            0.65625,  # Sheep.java:285
        "goat":             0.59375,  # Goat.java:336
        "wolf":             0.34375,  # Wolf.java:665
        "cat":              0.34375,  # Cat.java:503
        "ocelot":           0.34375,  # Ocelot.java:266
        "fox":              0.34375,  # Fox.java:717
        "panda":            0.28125,  # Panda.java:699
        "polar_bear":       0.34375,  # PolarBear.java:240
        "hoglin":           0.625,    # Hoglin.java:328
        "zoglin":           0.625,    # Zoglin.java:283
        "strider":          0.4375,   # Strider.java:466
        "axolotl":          0.09375,  # Axolotl.java:546
        "armadillo":        0.21875,  # Armadillo.java:377
        "happy_ghast":      0.46875,  # HappyGhast.java:557
        "camel":            1.365,    # Camel: 2.275 * BABY_SCALE 0.6
        "turtle":           0.102,    # Turtle: 0.34 * BABY_SCALE 0.3
        "dolphin":          0.09375,  # Dolphin.java:343
        "squid":            0.37,     # Squid.java:51
        "glow_squid":       0.37,     # (GlowSquid extends Squid)
    }

    # Boxes that are NOT the adult's halved. Cow, pig, sheep, goat, wolf,
    # cat, ocelot, polar bear, panda, strider, axolotl, nautilus and the
    # equines declare boxes equal to the halved adult (attachments only), so
    # they derive.
    BABY_DIMS = {
        "chicken":          (0.3, 0.4),      # was 0.2 x 0.35
        "rabbit":           (0.24, 0.4),     # was 0.2 x 0.25
        "fox":              (0.36, 0.42),    # FOX.scale(0.6)
        "hoglin":           (0.75, 0.85),    # was 0.698 x 0.7
        "zoglin":           (0.75, 0.85),
        "zombie":           (0.49, 0.98),    # the zombie family: was 0.3 x 0.975
        "husk":             (0.49, 0.98),
        "drowned":          (0.49, 0.98),
        "zombie_villager":  (0.49, 0.98),
        "zombified_piglin": (0.49, 0.98),
        "piglin":           (0.49, 0.98),
        "villager":         (0.49, 0.98),
        "camel":            (1.02, 1.425),   # CAMEL.scale(0.6) (was 0.45)
        "armadillo":        (0.42, 0.39),    # ARMADILLO.scale(0.6)
        "happy_ghast":      (0.95, 0.95),    # HAPPY_GHAST.scale(0.2375)
        "turtle":           (0.36, 0.12),    # TURTLE.scale(0.3)
        "dolphin":          (0.585, 0.39),   # DOLPHIN.scale(0.65)
        "squid":            (0.5, 0.5),      # was 0.4 x 0.4
        "glow_squid":       (0.5, 0.5),
    }

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
    cpp.append("        // slug  w  h  eye  category  track  upd  babyEye  babyW  babyH  xp  notInPeaceful")
    for slug in order:
        t = types[slug]
        cpp.append(
            '        {{ "{slug}", {w}f, {h}f, {e}f, {cat}, {tr}, {up}, {be}f, {bw}f, {bh}f, {xp}, {pf} }},'.format(
                slug=slug,
                w=round(t["width"], 5),
                h=round(t["height"], 5),
                e=round(t["eye"], 5),
                cat=CATEGORY_CPP[t["category"]],
                tr=t["track"],
                up=t["update"],
                be=BABY_EYE.get(slug, 0.0),
                bw=BABY_DIMS.get(slug, (0.0, 0.0))[0],
                bh=BABY_DIMS.get(slug, (0.0, 0.0))[1],
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
