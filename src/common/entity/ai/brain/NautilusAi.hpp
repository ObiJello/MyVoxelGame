// File: src/common/entity/ai/brain/NautilusAi.hpp
//
// MC net.minecraft.world.entity.animal.nautilus.NautilusAi — swim-wander,
// temptation, breeding, panic, and the charge attack: an angered (or, on a
// long random cooldown, an unprovoked) nautilus rams its target in a straight
// line, dealing attack damage plus a speed-scaled knockback.
//
// The taming/riding half of AbstractNautilus (TamableAnimal, saddle dashes,
// the rider's water-breathing aura, the shell inventory) is SKIPPED with the
// item and riding-input systems; ZombieNautilusAi reuses the target finder
// and the charge from here.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {

    class Mob;
    class Nautilus;
    class LivingEntity;

    namespace NautilusAi {

        void InitBrain(Nautilus& nautilus, Brain& brain);

        // MC NautilusAi.initMemories — the unprovoked-attack cooldown starts
        // rolled (2400-3600 ticks), shared by both nautilus variants.
        void InitMemories(Mob& nautilus);

        void UpdateActivity(Nautilus& nautilus);

        // MC NautilusAi.setAngerTarget — ANGRY_AT for 400 ticks.
        void SetAngerTarget(Mob& nautilus, LivingEntity& target);

        // MC NautilusAi.findNearestValidAttackTarget — the anger target while
        // it is in water, else (off cooldown, 50%) a swimming pufferfish.
        LivingEntity* FindNearestValidAttackTarget(Mob& mob);

        // MC ai/behavior/ChargeAttack, parameterised on the two variants'
        // speeds (0.6 nautilus, 0.5 zombie nautilus).
        BehaviorPtr MakeChargeAttack(float speed);

    } // namespace NautilusAi

} // namespace Game
