// File: src/server/world/storage/anvil/SpawnerNbt.hpp
//
// The monster spawner's NBT (MC BaseSpawner.load / save and the SpawnData /
// WeightedList codecs), plus the server hooks SpawnerBlockEntity's tick runs
// through (SpawnerServerHooks: build a mob from a SpawnData, rewrite its id,
// check the type's spawn predicate).
//
//   { Delay: short, MinSpawnDelay: short, MaxSpawnDelay: short,
//     SpawnCount: short, MaxNearbyEntities: short, RequiredPlayerRange: short,
//     SpawnRange: short,
//     SpawnData: { entity: {id: "...", ...}, custom_spawn_rules?: {...} },
//     SpawnPotentials: [ { data: <SpawnData>, weight: int }, ... ] }
//
// Also the generic NBT tree writer the entity compound needs: SpawnData keeps
// it as opaque binary (SpawnerBlockEntity.hpp), so writing it into the chunk
// means re-emitting a parsed tree.
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <string_view>

#include <glm/glm.hpp>

namespace Game {
    class SpawnerBlockEntity;
    class World;
    class JavaRandom;
    struct SpawnData;
    enum class EntityTypeId : uint16_t;
    enum class SpawnReason : uint8_t;
}

namespace Game::Anvil {

    // Emit `tag` as a named field of the open compound — any tag kind, lists
    // and compounds recursively.
    void WriteNbtTree(Nbt::Writer& w, std::string_view name, const ::World::NBTTag& tag);

    // MC SpawnData from its entity compound (the "entity" field): the binary
    // copy plus the parsed id / Pos / bare-id / baby fields.
    void SpawnDataFromEntityTag(const ::World::NBTTagCompound& entity, SpawnData& out);

    // MC SpawnData.CODEC over a compound ({entity, custom_spawn_rules?,
    // equipment?}). False when the codec fails (no entity compound, a bad
    // rules compound or light limit, a bad equipment table) — MC's read then
    // yields nothing.
    bool ReadSpawnDataTag(const ::World::NBTTagCompound& tag, SpawnData& out);

    // MC SpawnData.CODEC's encoding — the fields only, into a compound the
    // caller has open.
    void WriteSpawnDataBody(Nbt::Writer& w, const SpawnData& data);

    // MC SpawnPlacements.checkSpawnRules(type, level, reason, pos, random):
    // the type's registered predicate with the server's level context.
    bool CheckSpawnRulesAt(EntityTypeId type, ::Game::World& world, const glm::ivec3& pos,
                           JavaRandom& random, SpawnReason reason);

    // MC BaseSpawner.load(level, pos, input).
    void ReadSpawner(const ::World::NBTTagCompound& tag, SpawnerBlockEntity& spawner);

    // MC BaseSpawner.save(output) — the fields only; the caller has the
    // block-entity compound open.
    void WriteSpawner(Nbt::Writer& w, const SpawnerBlockEntity& spawner);

    // Install SpawnerServerHooks (IntegratedServer startup).
    void InstallSpawnerServerHooks();

} // namespace Game::Anvil
