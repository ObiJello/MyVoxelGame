#!/usr/bin/env python3
"""Bake MC's per-biome mob spawn weights into C++.

Reads data/minecraft/worldgen/biome/*.json (the real vanilla files this repo
already ships and already resolves at runtime via MC_DATA_ROOT) and emits
src/common/world/spawn/GeneratedMobSpawns.{hpp,cpp}.

Why bake rather than parse at runtime: the spawner consults this table for
every spawn attempt in every eligible chunk, several times a second. Parsing is
not the cost — the cost is that a runtime parse makes the table a startup
failure mode, and this project already established the "generate to C++" pattern
for recipes (tools/gen_recipes.py) and block shapes (tools/gen_block_shapes.py).

Entries naming a mob this port does not implement are DROPPED with a note, the
same way gen_recipes.py drops recipes for unknown items. That keeps the biome
files usable verbatim as MC ships them: today eight of roughly forty mob types
resolve, and adding a ninth needs no change here.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "data")
BIOME_DIR = os.path.join(DATA_DIR, "minecraft", "worldgen", "biome")


def biome_files():
    """Every data/<namespace>/worldgen/biome/*.json; slug is the bare name for
    minecraft and "<namespace>:<name>" otherwise (same rule as gen_biomes.py,
    and the string BiomeRegistry names the biome by)."""
    for ns in sorted(os.listdir(DATA_DIR)):
        d = os.path.join(DATA_DIR, ns, "worldgen", "biome")
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".json"):
                continue
            slug = name[:-5] if ns == "minecraft" else f"{ns}:{name[:-5]}"
            yield slug, os.path.join(d, name)
OUT_DIR = os.path.join(ROOT, "src", "common", "world", "spawn")

# Mob slug -> Game::EntityTypeId enumerator, read from the GENERATED entity
# table so this generator never falls behind it. It used to hard-code the eight
# mobs that existed at the time, which quietly dropped every spawn row for
# everything added since.
def load_known_mobs():
    hpp = os.path.join(ROOT, "src", "common", "entity", "GeneratedEntityTypes.hpp")
    known = {}
    for slug in re.findall(r'//\s*"([a-z_]+)"', open(hpp, encoding="utf-8").read()):
        known[slug] = "".join(p.capitalize() for p in slug.split("_"))
    return known


KNOWN_MOBS = load_known_mobs()

# MC MobCategory keys as they appear in the biome JSON.
CATEGORIES = ["monster", "creature", "ambient", "water_creature", "water_ambient",
              "underground_water_creature", "axolotls", "misc",
              # The Aether's appended MobCategory values (its
              # enumextensions.json; MobCategory.hpp), keyed by their
              # serialized names in the Aether's biome JSON.
              "aether:aether_surface_monster", "aether:aether_darkness_monster",
              "aether:aether_sky_monster", "aether:aether_aerwhale"]
CATEGORY_ENUM = {
    "monster": "Monster",
    "creature": "Creature",
    "ambient": "Ambient",
    "axolotls": "Axolotls",
    "underground_water_creature": "UndergroundWaterCreature",
    "water_creature": "WaterCreature",
    "water_ambient": "WaterAmbient",
    "misc": "Misc",
    "aether:aether_surface_monster": "AetherSurfaceMonster",
    "aether:aether_darkness_monster": "AetherDarknessMonster",
    "aether:aether_sky_monster": "AetherSkyMonster",
    "aether:aether_aerwhale": "AetherAerwhale",
}


def main():
    if not os.path.isdir(BIOME_DIR):
        print(f"error: {BIOME_DIR} not found", file=sys.stderr)
        return 1

    biomes = []
    skipped = set()

    for slug, path in biome_files():
        with open(path, encoding="utf-8") as f:
            data = json.load(f)

        # 26.1 files carry "spawners" / "spawn_costs" at the top level; 26.3
        # moved both into the minecraft:gameplay/natural_mob_spawns attribute
        # (spawns_by_category, with "count" an int or a uniform provider).
        spawn_attr = data.get("attributes", {}).get("minecraft:gameplay/natural_mob_spawns")
        if spawn_attr is not None:
            spawn_arg = spawn_attr.get("argument", spawn_attr)
            spawners = spawn_arg.get("spawns_by_category", {})
            spawn_costs = spawn_arg.get("spawn_costs", {})
        else:
            spawners = data.get("spawners", {})
            spawn_costs = data.get("spawn_costs", {})
        entries = []

        for category in CATEGORIES:
            for entry in spawners.get(category, []):
                mob = entry.get("type", "").split(":")[-1]   # any namespace: mod mobs are engine slugs
                if mob not in KNOWN_MOBS:
                    skipped.add(mob)
                    continue
                if "count" in entry:
                    count = entry["count"]
                    if isinstance(count, dict):
                        min_count = int(count["min_inclusive"])
                        max_count = int(count["max_inclusive"])
                    else:
                        min_count = max_count = int(count)
                else:
                    min_count = int(entry.get("minCount", 1))
                    max_count = int(entry.get("maxCount", 1))
                entries.append((
                    CATEGORY_ENUM[category],
                    KNOWN_MOBS[mob],
                    int(entry.get("weight", 0)),
                    min_count,
                    max_count,
                ))

        # MC MobSpawnSettings.mobSpawnCosts — the PotentialCalculator budget
        # (soul sand valley and friends). Kept even when every weighted entry
        # was dropped, so the costs arrive the day the mob does.
        costs = []
        for mob_key, cost in sorted(spawn_costs.items()):
            mob = mob_key.split(":")[-1]
            if mob not in KNOWN_MOBS:
                skipped.add(mob)
                continue
            costs.append((KNOWN_MOBS[mob],
                          float(cost["energy_budget"]), float(cost["charge"])))

        if entries or costs:
            biomes.append((slug, entries, costs))

    os.makedirs(OUT_DIR, exist_ok=True)

    hpp = os.path.join(OUT_DIR, "GeneratedMobSpawns.hpp")
    cpp = os.path.join(OUT_DIR, "GeneratedMobSpawns.cpp")

    with open(hpp, "w", encoding="utf-8") as f:
        f.write("""// File: src/common/world/spawn/GeneratedMobSpawns.hpp
// AUTO-GENERATED by tools/gen_mob_spawns.py — DO NOT EDIT BY HAND.
//
// Per-biome spawn weights, from data/minecraft/worldgen/biome/*.json.
// Regenerate after any Minecraft version bump; see CLAUDE.md.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/MobCategory.hpp"

#include <cstdint>
#include <string_view>

namespace Game {

    struct MobSpawnEntry {
        MobCategory  category;
        EntityTypeId type;
        int          weight;
        int          minCount;
        int          maxCount;
    };

    // MC MobSpawnSettings.MobSpawnCost — the PotentialCalculator budget for a
    // charged type in this biome (soul sand valley skeletons, etc.).
    struct MobSpawnCost {
        EntityTypeId type;
        double       energyBudget;
        double       charge;
    };

    struct BiomeSpawnList {
        std::string_view     biome;
        const MobSpawnEntry* entries;
        int                  count;
        const MobSpawnCost*  costs;      // null when the biome has none
        int                  costCount;
    };

    // Sorted by biome slug so lookup can binary-search.
    extern const BiomeSpawnList kBiomeSpawnLists[];
    extern const int            kBiomeSpawnListCount;

    // Null when the biome has no spawns for any implemented mob.
    const BiomeSpawnList* FindBiomeSpawnList(std::string_view biomeSlug);

    // Null when the biome assigns this type no spawn cost.
    const MobSpawnCost* FindMobSpawnCost(const BiomeSpawnList* list, EntityTypeId type);

} // namespace Game
""")

    with open(cpp, "w", encoding="utf-8") as f:
        f.write("""// File: src/common/world/spawn/GeneratedMobSpawns.cpp
// AUTO-GENERATED by tools/gen_mob_spawns.py — DO NOT EDIT BY HAND.
#include "common/world/spawn/GeneratedMobSpawns.hpp"

#include <algorithm>

namespace Game {

""")
        # FindBiomeSpawnList binary-searches on the slug string: sort by the
        # full slug (namespaced mod biomes interleave with vanilla ones).
        biomes.sort(key=lambda b: b[0])
        for slug, entries, costs in biomes:
            ident = slug.replace("-", "_").replace(":", "__")
            if entries:
                f.write(f"    static const MobSpawnEntry k_{ident}[] = {{\n")
                for cat, mob, weight, mn, mx in entries:
                    f.write(f"        {{ MobCategory::{cat}, EntityTypeId::{mob}, "
                            f"{weight}, {mn}, {mx} }},\n")
                f.write("    };\n")
            if costs:
                f.write(f"    static const MobSpawnCost k_{ident}_costs[] = {{\n")
                for mob, budget, charge in costs:
                    f.write(f"        {{ EntityTypeId::{mob}, {budget}, {charge} }},\n")
                f.write("    };\n")
            f.write("\n")

        f.write("    const BiomeSpawnList kBiomeSpawnLists[] = {\n")
        for slug, entries, costs in biomes:
            ident = slug.replace("-", "_").replace(":", "__")
            entries_ref = f"k_{ident}" if entries else "nullptr"
            costs_ref = f"k_{ident}_costs" if costs else "nullptr"
            f.write(f'        {{ "{slug}", {entries_ref}, {len(entries)}, '
                    f"{costs_ref}, {len(costs)} }},\n")
        f.write("    };\n\n")
        f.write(f"    const int kBiomeSpawnListCount = {len(biomes)};\n\n")

        f.write("""    const BiomeSpawnList* FindBiomeSpawnList(std::string_view biomeSlug) {
        const auto* begin = kBiomeSpawnLists;
        const auto* end = kBiomeSpawnLists + kBiomeSpawnListCount;
        const auto* it = std::lower_bound(begin, end, biomeSlug,
            [](const BiomeSpawnList& row, std::string_view key) { return row.biome < key; });
        if (it == end || it->biome != biomeSlug) return nullptr;
        return it;
    }

    const MobSpawnCost* FindMobSpawnCost(const BiomeSpawnList* list, EntityTypeId type) {
        if (!list || !list->costs) return nullptr;
        for (int i = 0; i < list->costCount; ++i) {
            if (list->costs[i].type == type) return &list->costs[i];
        }
        return nullptr;
    }

} // namespace Game
""")

    total = sum(len(e) for _, e, _ in biomes)
    print(f"Wrote {len(biomes)} biomes, {total} spawn entries")
    if skipped:
        print(f"Skipped {len(skipped)} unimplemented mob types: "
              f"{', '.join(sorted(skipped))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
