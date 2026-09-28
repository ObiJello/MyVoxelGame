// File: src/common/entity/ai/brain/HoglinAi.hpp
//
// MC net.minecraft.world.entity.monster.hoglin.HoglinAi (+ the
// HoglinSpecificSensor it reads).
//
// Four activities: CORE (look / move sinks), IDLE (breed, back off the
// nearest repellent — warped fungus, a potted one, a nether portal, a
// respawn anchor — and turn passive near it for 200 ticks, attack the
// nearest visible attackable player, adults keep 8 blocks from the nearest
// adult piglin, babies trail an adult, wander), FIGHT (walk into reach,
// 40-tick swings for an adult / 15 for a baby, drop the target when
// breeding) and AVOID (flee 15 blocks from the AVOID_TARGET at 1.3 until
// the hoglins no longer lose the numbers game against the piglins in
// sight).
//
// Retaliation: a hit adult turns on its attacker (not a hoglin, not a piglin
// while it is retreating) and rallies every visible adult hoglin onto the
// nearer of their target and the attacker; a hit baby flees the nearest of
// its threats. A hoglin's own hit on a piglin that is outnumbered by piglins
// makes it — and every visible adult hoglin — retreat instead (onHitTarget).
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

#include <glm/glm.hpp>

namespace Game {
    class Hoglin;
    class LivingEntity;
    struct EntityLevel;
    namespace HoglinAi {
        void InitBrain(Hoglin& hoglin, Brain& brain);
        void UpdateActivity(Hoglin& hoglin);

        // MC HoglinAi.onHitTarget — from Hoglin.doHurtTarget.
        void OnHitTarget(Hoglin& attacker, LivingEntity& target);
        // MC HoglinAi.wasHurtBy — from Hoglin.hurtServer.
        void WasHurtBy(EntityLevel& level, Hoglin& hoglin, LivingEntity& attacker);

        // MC HoglinAi.isPacified — PACIFIED (a repellent was near recently).
        bool IsPacified(const Hoglin& hoglin);
        // MC HoglinAi.isPosNearNearestRepellent — within 8 of the remembered
        // repellent (Hoglin.getWalkTargetValue avoids those cells).
        bool IsPosNearNearestRepellent(const Hoglin& hoglin, const glm::ivec3& pos);
        // MC HoglinAi.getSoundForCurrentActivity ("" with no activity).
        const char* SoundForCurrentActivity(const Hoglin& hoglin);
    }
}
