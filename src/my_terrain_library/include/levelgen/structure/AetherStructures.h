#pragma once

#include "levelgen/structure/Structures.h"
#include <functional>

// The Aether 1.5.10 structures — world/structure/*.java and
// world/structurepiece/** — data from data/aether/worldgen/structure/*.json
// (StructureInfo::modJson). Templates in data/aether/structure/** are our own
// designs from tools/gen_aether_structures.py (the mod's NBTs are All Rights
// Reserved), built to the sizes, anchors and DATA markers this code reads.
//
//   aether:large_aercloud   LargeAercloudStructure + LargeAercloudChunk
//   aether:bronze_dungeon   BronzeDungeonStructure + bronzedungeon/* (builder,
//                           rooms, boss room, tunnels, surface ruins)
//   aether:silver_dungeon   SilverDungeonStructure + silverdungeon/* (cloud
//                           bed, rear/skeleton temple, 3x3x3 room grid, boss room)
//   aether:gold_dungeon     GoldDungeonStructure + golddungeon/* (island,
//                           stubs, gumdrop caves, tunnel, boss room, golden oaks)
//
// Bosses: each boss room holds its boss as a template entity in the mod
// (Slider, Valkyrie Queen, Sun Spirit). Template entities are not placed yet —
// the boss room logs a PENDING BOSS marker line ("[AetherStructures] PENDING
// BOSS aether:slider at x y z") where the boss would stand, once per room.
// Treasure chests get their reward loot table; they are not locked.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace AetherStructures {

/** True for the Aether structure types whose layout and pieces are ported. */
bool isImplemented(const StructureInfo& info);

/**
 * Structure.generate for an Aether type: findGenerationPoint (its own stub
 * and biome check through `validBiomeAt`), then the pieces. Never throws —
 * a template that cannot load fails the start (logged once).
 */
bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
              const std::function<bool(int x, int y, int z)>& validBiomeAt);

} // namespace AetherStructures
} // namespace structure
} // namespace levelgen
} // namespace minecraft
