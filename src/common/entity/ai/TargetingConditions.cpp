// File: src/common/entity/ai/TargetingConditions.cpp
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/raid/Raider.hpp"

#include <algorithm>

namespace Game {

    bool TargetingConditions::Test(LivingEntity* attacker, const LivingEntity& target) const {
        if (attacker == &target) return false;
        // MC LivingEntity.canBeSeenByAnyone: alive and not a spectator — for
        // combat and non-combat conditions alike (a spectator is never looked
        // at, tempted by, followed or targeted).
        if (!target.IsAlive() || target.IsSpectator()) return false;

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
            // plus MC's players-are-safe-on-Peaceful rule.
            if (!target.IsAttackable()) return false;
            // MC targeter.canAttack(target) proper: a mob's own override
            // (an illager sparing baby villagers, the iron golem creepers,
            // the ghast anything far above or below it).
            if (const auto* mob = dynamic_cast<const Mob*>(attacker); mob && !mob->CanAttack(target)) {
                return false;
            }
            // MC targeter.isAlliedTo(target): no scoreboard teams, so only the
            // illagers' #illager_friends rule (and the evoker's vexes).
            if (Raiders::IsAlliedTo(*attacker, target)) return false;
            if (target.IsPlayer() && attacker->Level() &&
                attacker->Level()->GetDifficulty() == Difficulty::Peaceful) {
                return false;
            }
        }

        if (range > 0.0) {
            // MC scales the range by the target's visibility percent and
            // floors the product at 2.0 — an invisible player can still be
            // noticed point-blank. LivingEntity::GetVisibilityPercent is MC's
            // getVisibilityPercent: sneaking x0.8, invisibility x0.7 * the
            // worn-armor cover (floored at 0.1 — every armour piece a player
            // wears gives an invisible player away), and a mob head worn
            // against its own kind x0.5.
            const double modifier = testInvisible ? target.GetVisibilityPercent(attacker) : 1.0;
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
