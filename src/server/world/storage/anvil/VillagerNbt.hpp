// File: src/server/world/storage/anvil/VillagerNbt.hpp
//
// The villager's and the zombie villager's save data, in MC 26.3's format
// (Villager / AbstractVillager / ZombieVillager add/readAdditionalSaveData):
//
//   VillagerData          {type:"minecraft:plains", profession:"minecraft:farmer", level:2}
//   VillagerDataFinalized byte
//   FoodLevel             byte
//   Gossips               [{Target:[I;..], Type:"trading", Value:12}, ...]
//   Xp                    int
//   LastRestock           long
//   LastGossipDecay       long
//   RestocksToday         int
//   Offers                {Recipes:[{buy:{id,count[,components]}, buyB:{..}, sell:{item},
//                                    uses, maxUses, rewardExp, specialPrice, demand,
//                                    priceMultiplier, xp}, ...]}   — only once rolled
//   Inventory             [{id, count[, components]}, ...]
//   Brain                 {memories:{"minecraft:home":{value:{pos:[I;x,y,z],
//                          dimension:"minecraft:overworld"}}, "minecraft:last_slept":
//                          {value:123L}, "minecraft:golem_detected_recently":
//                          {value:1b, ttl:599L}, ...}}
//
// The same readers take a structure template's villager (a village's
// nitwit, a cartographer in a house) and a villager from a real MC save.
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

namespace Game {
    class Villager;
    class ZombieVillager;
}

namespace Game::Anvil {

    // The per-type fields (everything above but Brain).
    void WriteVillagerNbt(Nbt::Writer& w, const Villager& villager);
    // The Brain compound with the villager's saveable memories (replaces
    // LivingEntity's empty one).
    void WriteVillagerBrain(Nbt::Writer& w, const Villager& villager);
    // Both, in MC's read order; rebuilds the brain for the loaded profession
    // (MC refreshBrain) and queues the POI tickets the memories name.
    void ReadVillagerNbt(const ::World::NBTTagCompound& tag, Villager& villager);

    // MC ZombieVillager: VillagerData, VillagerDataFinalized, Xp (the
    // offers and gossips it carries for a cure are not kept — no curing).
    void WriteZombieVillagerNbt(Nbt::Writer& w, const ZombieVillager& zombie);
    void ReadZombieVillagerNbt(const ::World::NBTTagCompound& tag, ZombieVillager& zombie);

} // namespace Game::Anvil
