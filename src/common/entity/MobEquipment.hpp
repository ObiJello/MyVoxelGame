// File: src/common/entity/MobEquipment.hpp
//
// MC Mob's equipment operations that take an EquipmentTable (Mob.equip /
// EquipmentUser.equip), the preserved-equipment drop the ominous trial
// spawner calls on the mobs it discards, and the death drop. The storage
// itself — six humanoid slots and their drop chances — lives on Game::Mob
// (GetEquipment / SetEquipment / GetEquipmentDropChance).
#pragma once

#include <string>

namespace Game {

    class JavaRandom;
    class Mob;
    struct SpawnData;

    namespace MobEquipment {

        // MC Mob.equip(EquipmentTable): the table's loot rolled with the
        // EQUIPMENT parameter set (the level random), each stack placed by
        // EquipmentUser.resolveSlot — its EQUIPPABLE slot, else the main hand
        // — first come first served, and each filled slot takes the table's
        // drop chance for it. Server-only; a no-op on a client mob.
        void Equip(Mob& mob, const std::string& lootTable, bool uniformDropChance,
                   const float dropChances[8]);

        // The same from a SpawnData's equipment (nothing when it has none).
        void EquipFromSpawnData(Mob& mob, const SpawnData& data);

        // MC Mob.dropPreservedEquipment(level): every slot whose drop chance
        // marks it preserved (> 1) is emptied and its stack dropped at the
        // mob (spawnAtLocation).
        void DropPreservedEquipment(Mob& mob);

        // MC Mob.dropCustomDeathLoot's equipment loop: each slot with a
        // non-zero chance (plus Looting's equipment_drops, 1% a level, for a
        // player killer) drops when the kill was a player's or the slot is
        // preserved — a preserved piece whole, any other damageable piece
        // worn down at random — unless it carries Curse of Vanishing.
        void DropEquipmentOnDeath(Mob& mob, bool killedByPlayer, int lootingLevel, bool killerIsPlayer);

        // Mob.getBaseExperienceReward's equipment half (for a mob whose base
        // reward is positive): 1 + nextInt(3) for every worn piece whose drop
        // chance is not a guaranteed drop (<= 1).
        int ExperienceBonus(const Mob& mob, JavaRandom& random);

    } // namespace MobEquipment

} // namespace Game
