// File: src/common/entity/ai/TargetingConditions.cpp
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/Sensing.hpp"

#include <algorithm>

namespace Game {

    bool TargetingConditions::Test(LivingEntity* attacker, const LivingEntity& target) const {
        if (attacker == &target) return false;
        // MC canBeSeenByAnyone: alive and not a spectator (no spectator mode
        // reaches mob targeting here).
        if (!target.IsAlive()) return false;

        if (!attacker) {
            // MC's null-targeter branch: combat still refuses a target that
            // cannot be an enemy, and Peaceful outright.
            if (isCombat &&
                (!target.IsAttackable() ||
                 (target.Level() &&
                  target.Level()->GetDifficulty() == Difficulty::Peaceful))) {
                return false;
            }
            return true;
        }

        if (isCombat) {
            // MC targeter.canAttack(target): the port surrogate is
            // IsAttackable() (the player view's override excludes creative),
            // plus MC's players-are-safe-on-Peaceful rule. isAlliedTo is
            // scoreboard teams — absent.
            if (!target.IsAttackable()) return false;
            if (target.IsPlayer() && attacker->Level() &&
                attacker->Level()->GetDifficulty() == Difficulty::Peaceful) {
                return false;
            }
        }

        if (range > 0.0) {
            // MC scales the range by the target's visibility percent and
            // floors the product at 2.0 — an invisible player can still be
            // noticed point-blank. getVisibilityPercent: invisibility scales
            // by 0.7 * armor coverage floored at 0.1; mobs carry no armor
            // here, so the floor applies. (Sneaking affects players only and
            // player-vs-mob targeting reads mob state, not the reverse.)
            double modifier = 1.0;
            if (testInvisible && target.HasEffect(MobEffectId::Invisibility)) {
                modifier = 0.7 * 0.1;
            }
            const double visibilityDistance = std::max(range * modifier, 2.0);
            if (attacker->DistanceToSqr(target) >
                visibilityDistance * visibilityDistance) {
                return false;
            }
        }

        if (requiresLineOfSight) {
            if (Mob* mob = dynamic_cast<Mob*>(attacker)) {
                if (!mob->GetSensing().HasLineOfSight(target)) return false;
            }
        }

        return true;
    }

} // namespace Game
