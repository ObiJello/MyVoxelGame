// File: src/common/entity/ai/brain/PiglinAi.hpp
//
// MC net.minecraft.world.entity.monster.piglin.PiglinAi.
//
// The portable surface: idle wander and piglin-to-piglin socialising, the
// jealous stare at players carrying gold, hunting hoglins (with the shared
// 30-120s hunt cooldown and the pack broadcast), celebrating (and sometimes
// dancing) over a kill, retreating from zombified piglins and soul-fire
// repellent blocks, fleeing when hoglins outnumber the pack, the baby's
// nemesis-flight and hoglin-riding games, and retaliation with pack anger.
//
// SKIPPED, each commented at its MC call site in the .cpp: everything riding
// on the item/equipment systems — admiring, bartering, item pickup, crossbow
// and spear combat, gold-armor truce — plus door interaction and sounds.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {

    class Mob;
    class Piglin;
    struct EntityLevel;

    namespace PiglinAi {

        void InitBrain(Piglin& piglin, Brain& brain);

        // MC PiglinAi.initMemories — a fresh piglin starts with the hunt
        // cooldown armed (30-120 s), so a spawn does not immediately gore the
        // nearest hoglin.
        void InitMemories(Piglin& piglin);

        void UpdateActivity(Piglin& piglin);

        // MC PiglinAi.wasHurtBy — the retaliate/flee/pack-anger switchboard.
        void WasHurtBy(EntityLevel& level, Piglin& piglin, LivingEntity& attacker);

        // Shared with PiglinBruteAi.
        void MaybeRetaliate(EntityLevel& level, Mob& piglin, LivingEntity& attacker);
        void SetAngerTarget(Mob& piglin, LivingEntity& target);

        // MC PiglinAi.isZombified — zombified piglin or zoglin.
        bool IsZombified(const Entity& entity);

        // MC PiglinAi.isPlayerHoldingLovedItem — a player holding anything in
        // the piglin_loved tag (the flattened gold-item list here).
        bool IsPlayerHoldingLovedItem(EntityLevel& level, LivingEntity& entity);

    } // namespace PiglinAi

} // namespace Game
