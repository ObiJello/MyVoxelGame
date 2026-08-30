// File: src/server/world/storage/anvil/EntityNbt.hpp
//
// One entity <-> vanilla NBT.
//
// Layered exactly as vanilla's addAdditionalSaveData chain: base Entity, then
// LivingEntity, then Mob, then the ageable/animal tier, then the concrete
// type. Reading applies them in MC's own read order, which is NOT the reverse
// of the write order and is not "most-derived last" — see the ordering rules
// in the .cpp, where three classes deliberately clobber saved values.
//
// Per-type dispatch is a switch on EntityTypeId, not a virtual. A virtual
// WriteNbt on Mob would drag Game::Nbt::Writer into every header under
// common/entity/mobs/ — which the CLIENT links — for a call that happens once
// per entity per save. The switch lives here, in server code, where the writer
// already is, and keeps the whole save format readable in one file.
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "common/world/math/WorldMath.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <string>
#include <string_view>

namespace Game {
    class Mob;
    class EntityLevel;
    struct ItemEntity;
    struct ExperienceOrb;
    enum class EntityTypeId : uint16_t;
}

namespace Game::Anvil {

    // "pig" -> "minecraft:pig". The generated slugs carry no namespace.
    std::string EntityName(EntityTypeId type);
    // Accepts "minecraft:pig" and bare "pig" — vanilla's Identifier.parse
    // defaults the namespace, so both appear in the wild. False if unknown.
    bool EntityTypeFromName(std::string_view name, EntityTypeId& out);

    // Append one element to an open `Entities` list.
    //
    // Returns false having written NOTHING when the entity must not be stored:
    // CanSerialize() is false, it is already removed, or its type has no
    // vanilla name. A half-formed compound must never reach the file.
    bool WriteMob (Nbt::Writer& w, Nbt::Writer::ListScope& list, const Mob& mob);
    bool WriteItem(Nbt::Writer& w, Nbt::Writer::ListScope& list, const ItemEntity& item);
    bool WriteOrb (Nbt::Writer& w, Nbt::Writer::ListScope& list, const ExperienceOrb& orb);

    // Apply a saved compound to an already-constructed mob. Split from
    // construction because the factory that builds a Mob from an EntityTypeId
    // lives in the server, and this must not duplicate its 71 cases.
    void ApplyMobNbt(const ::World::NBTTagCompound& tag, Mob& mob);

    // The two plain structs are built outright.
    bool ReadItem(const ::World::NBTTagCompound& tag, ItemEntity& out);
    bool ReadOrb (const ::World::NBTTagCompound& tag, ExperienceOrb& out);

    // What kind of entity a compound describes, so the caller can route it.
    enum class EntityKind : uint8_t { Mob, Item, Orb, Unknown };
    EntityKind ClassifyEntity(const ::World::NBTTagCompound& tag, EntityTypeId& outType);

} // namespace Game::Anvil
