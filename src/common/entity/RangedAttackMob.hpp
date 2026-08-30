// File: src/common/entity/RangedAttackMob.hpp
//
// MC net.minecraft.world.entity.monster.RangedAttackMob — the interface a mob
// implements to be drivable by RangedBowAttackGoal (and, later,
// RangedAttackGoal for witches/drowned). `power` is the bow charge in 0..1;
// a mob-drawn bow always releases at full charge, so mobs receive 1.0.
#pragma once

namespace Game {

    class LivingEntity;

    class RangedAttackMob {
    public:
        virtual ~RangedAttackMob() = default;
        virtual void PerformRangedAttack(LivingEntity& target, float power) = 0;
    };

} // namespace Game
