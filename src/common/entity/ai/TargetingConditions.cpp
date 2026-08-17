// File: src/common/entity/ai/TargetingConditions.cpp
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/Sensing.hpp"

namespace Game {

    bool TargetingConditions::Test(LivingEntity* attacker, const LivingEntity& target) const {
        if (attacker == &target) return false;
        if (!target.IsAlive()) return false;
        if (!target.IsAttackable()) return false;

        if (attacker) {
            if (range >= 0.0) {
                // MC scales the range by the target's visibility multiplier
                // (sneaking, invisibility). Neither is modelled for mobs here,
                // so the comparison is against the raw range — and the hook
                // stays in one place for when sneaking starts mattering.
                const double effectiveRange = range;
                if (attacker->DistanceToSqr(target) > effectiveRange * effectiveRange) {
                    return false;
                }
            }

            if (requiresLineOfSight) {
                if (Mob* mob = dynamic_cast<Mob*>(attacker)) {
                    if (!mob->GetSensing().HasLineOfSight(target)) return false;
                }
            }
        }

        return true;
    }

} // namespace Game
