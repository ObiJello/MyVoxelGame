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
#include <vector>

namespace Game {
    class AttributeMap;
    class Mob;
    struct MobEffectInstance;
    struct EntityLevel;
    struct ItemEntity;
    struct ExperienceOrb;
    class EntityTags;
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
    // The same mob as a NAMED compound — MC's RootVehicle "Entity" (the
    // vehicle a player logged out on, with its Passengers).
    bool WriteMobCompound(Nbt::Writer& w, std::string_view key, const Mob& mob);
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

    // MC LivingEntity's "active_effects" list (MobEffectInstance.CODEC), for
    // the player file: ServerPlayer holds its own list and writes it itself.
    // Write omits the key for an empty list, as MC does; Read drops unknown
    // effect ids.
    void WriteActiveEffects(Nbt::Writer& w, const std::vector<MobEffectInstance>& effects);
    std::vector<MobEffectInstance> ReadActiveEffects(const ::World::NBTTagCompound& tag);

    // MC Entity.saveWithoutId / load's "Tags": the scoreboard tags `/tag`
    // sets, a list of strings written only when there are any. Shared by
    // every entity writer here and the player file.
    void WriteEntityTags(Nbt::Writer& w, const EntityTags& tags);
    void ReadEntityTags(const ::World::NBTTagCompound& tag, EntityTags& out);

    // MC AttributeMap.save / load — the "attributes" list ({id, base,
    // modifiers?: [{id, amount, operation}]}): every row's base, plus the
    // modifiers that persist — the engine's fixed permanent ones (spawn
    // rolls, zombie leader bonuses) and every `permanent` one by its
    // Identifier (/attribute … modifier add). Shared by the mob writer and
    // the player file. Read applies only to rows `map` has when
    // `onlyRegistered` (a mob: MC's apply() skips what the type lacks) and
    // sets `*customized` when a client-syncable row's saved value differs
    // from what `map` held, or a named permanent modifier came back.
    void WriteAttributeList(Nbt::Writer& w, const AttributeMap& map);
    void ReadAttributeList(const ::World::NBTTagCompound& tag, AttributeMap& map, bool onlyRegistered,
                           bool* customized = nullptr);

} // namespace Game::Anvil
