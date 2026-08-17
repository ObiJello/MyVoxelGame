// File: src/common/entity/ai/goals/AnimalGoals.cpp
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Game {

    // ── TemptGoal ──────────────────────────────────────────────────────────

    TemptGoal::TemptGoal(PathfinderMob* mob, double speedModifier, bool canScare)
        : m_mob(mob), m_speedModifier(speedModifier), m_canScare(canScare) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
        // Non-combat: an animal does not need line of sight to notice food.
        m_conditions = TargetingConditions::ForNonCombat().IgnoreLineOfSight();
    }

    bool TemptGoal::CanUse() {
        // Spooked cooldown. Ticks down here rather than in Tick() because the
        // goal is not running while it is calming down.
        if (m_calmDown > 0) { --m_calmDown; return false; }

        EntityLevel* level = m_mob->Level();
        if (!level) return false;

        const double range = m_mob->GetAttributeValue(Attribute::TemptRange);
        LivingEntity* player = level->GetNearestPlayer(
            m_mob->position.x, m_mob->position.y, m_mob->position.z, range);
        if (!player) return false;

        // Whether the player is HOLDING food is checked by the caller-supplied
        // food test on the animal — the item layer lives outside the entity
        // system, so the level bridge answers this via the adapter's held item.
        Animal* animal = dynamic_cast<Animal*>(m_mob);
        if (!animal) return false;
        if (!animal->IsFood(level->GetHeldItemId(*player))) return false;

        m_player = player;
        return true;
    }

    bool TemptGoal::CanContinueToUse() {
        if (!m_player || !m_player->IsAlive()) return false;

        if (m_canScare && m_mob->DistanceToSqr(*m_player) < 36.0) {
            // Inside 6 blocks the player must hold still. The thresholds are
            // MC's: a hair of movement (0.01 blocks) or 5 degrees of turn is
            // enough to spook.
            const double dx = m_player->position.x - m_px;
            const double dy = m_player->position.y - m_py;
            const double dz = m_player->position.z - m_pz;
            if (dx * dx + dy * dy + dz * dz > 0.010000000000000002) return false;
            if (std::abs(m_player->xRot - m_pRotX) > 5.0f) return false;
            if (std::abs(m_player->yRot - m_pRotY) > 5.0f) return false;
        } else {
            m_px = m_player->position.x;
            m_py = m_player->position.y;
            m_pz = m_player->position.z;
        }

        m_pRotX = m_player->xRot;
        m_pRotY = m_player->yRot;
        return CanUse();
    }

    void TemptGoal::Start() {
        if (!m_player) return;
        m_px = m_player->position.x;
        m_py = m_player->position.y;
        m_pz = m_player->position.z;
        m_isRunning = true;
    }

    void TemptGoal::Stop() {
        m_player = nullptr;
        m_mob->GetNavigation().Stop();
        // 100 ticks, halved by the 2-tick evaluation cadence.
        m_calmDown = ReducedTickDelay(100);
        m_isRunning = false;
    }

    void TemptGoal::Tick() {
        if (!m_player) return;

        m_mob->GetLookControl().SetLookAt(
            m_player->position.x, m_player->GetEyeY(), m_player->position.z,
            static_cast<float>(m_mob->GetMaxHeadYRot() + 20),
            static_cast<float>(m_mob->GetMaxHeadXRot()));

        // Stop short rather than walking into the player.
        if (m_mob->DistanceToSqr(*m_player) < kDefaultStopDistance * kDefaultStopDistance) {
            m_mob->GetNavigation().Stop();
        } else {
            m_mob->GetNavigation().MoveTo(*m_player, m_speedModifier);
        }
    }

    // ── BreedGoal ──────────────────────────────────────────────────────────

    BreedGoal::BreedGoal(Animal* animal, double speedModifier)
        : m_animal(animal), m_speedModifier(speedModifier) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    Animal* BreedGoal::GetFreePartner() {
        EntityLevel* level = m_animal->Level();
        if (!level) return nullptr;

        AABB box = m_animal->GetAABB();
        box.min -= glm::vec3(8.0f);
        box.max += glm::vec3(8.0f);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_animal, nearby);

        Animal* best = nullptr;
        double bestDist = std::numeric_limits<double>::max();

        for (Entity* e : nearby) {
            Animal* other = dynamic_cast<Animal*>(e);
            if (!other) continue;
            if (!m_animal->CanMate(*other)) continue;
            // A panicking animal will not stop to breed.
            if (other->IsPanicking()) continue;

            const double d = m_animal->DistanceToSqr(*other);
            if (d < bestDist) { bestDist = d; best = other; }
        }
        return best;
    }

    bool BreedGoal::CanUse() {
        if (!m_animal->IsInLove()) return false;
        m_partner = GetFreePartner();
        return m_partner != nullptr;
    }

    bool BreedGoal::CanContinueToUse() {
        if (!m_partner || !m_partner->IsAlive()) return false;
        if (!m_partner->IsInLove()) return false;
        if (m_partner->IsPanicking()) return false;
        // 60 ticks is the courtship timeout — if they cannot reach each other
        // in three seconds they give up and try again.
        return m_loveTime < 60;
    }

    void BreedGoal::Stop() {
        m_partner = nullptr;
        m_loveTime = 0;
    }

    void BreedGoal::Tick() {
        if (!m_partner) return;

        m_animal->GetLookControl().SetLookAt(
            m_partner->position.x, m_partner->GetEyeY(), m_partner->position.z,
            10.0f, static_cast<float>(m_animal->GetMaxHeadXRot()));
        m_animal->GetNavigation().MoveTo(*m_partner, m_speedModifier);

        ++m_loveTime;
        if (m_loveTime >= AdjustedTickDelay(60) && m_animal->DistanceToSqr(*m_partner) < 9.0) {
            m_animal->SpawnChildFromBreeding(*m_partner);
        }
    }

    // ── FollowParentGoal ───────────────────────────────────────────────────

    FollowParentGoal::FollowParentGoal(Animal* animal, double speedModifier)
        : m_animal(animal), m_speedModifier(speedModifier) {
        // No flags — see the header. A following baby can still panic and look
        // around, which is what MC relies on.
    }

    bool FollowParentGoal::CanUse() {
        if (m_animal->GetAge() >= 0) return false;   // adults do not follow

        EntityLevel* level = m_animal->Level();
        if (!level) return false;

        AABB box = m_animal->GetAABB();
        box.min -= glm::vec3(kHorizontalScanRange, kVerticalScanRange, kHorizontalScanRange);
        box.max += glm::vec3(kHorizontalScanRange, kVerticalScanRange, kHorizontalScanRange);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_animal, nearby);

        Animal* best = nullptr;
        double bestDist = std::numeric_limits<double>::max();
        for (Entity* e : nearby) {
            Animal* other = dynamic_cast<Animal*>(e);
            if (!other || other->GetType() != m_animal->GetType()) continue;
            if (other->GetAge() < 0) continue;       // another baby is not a parent

            const double d = m_animal->DistanceToSqr(*other);
            if (d < bestDist) { bestDist = d; best = other; }
        }

        if (!best) return false;
        // Already close enough — do not crowd.
        if (bestDist < static_cast<double>(kDontFollowIfCloserThan) * kDontFollowIfCloserThan) {
            return false;
        }

        m_parent = best;
        return true;
    }

    bool FollowParentGoal::CanContinueToUse() {
        if (m_animal->GetAge() >= 0) return false;
        if (!m_parent || !m_parent->IsAlive()) return false;

        const double d = m_animal->DistanceToSqr(*m_parent);
        // Stop when close, and give up entirely past 16 blocks.
        return d >= 9.0 && d <= 256.0;
    }

    void FollowParentGoal::Start() { m_timeToRecalcPath = 0; }
    void FollowParentGoal::Stop()  { m_parent = nullptr; }

    void FollowParentGoal::Tick() {
        if (!m_parent) return;
        if (--m_timeToRecalcPath > 0) return;
        m_timeToRecalcPath = AdjustedTickDelay(10);
        m_animal->GetNavigation().MoveTo(*m_parent, m_speedModifier);
    }

    // ── EatBlockGoal ───────────────────────────────────────────────────────

    EatBlockGoal::EatBlockGoal(Animal* animal) : m_animal(animal) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    namespace {
        // MC BlockTags.EDIBLE_FOR_SHEEP, from
        // data/minecraft/tags/block/edible_for_sheep.json.
        bool IsEdibleForSheep(BlockID b) {
            return b == BlockID::ShortGrass || b == BlockID::Fern ||
                   b == BlockID::ShortDryGrass || b == BlockID::TallDryGrass;
        }
    }

    bool EatBlockGoal::CanUse() {
        EntityLevel* level = m_animal->Level();
        if (!level) return false;

        // Babies graze 20x more often than adults — MC's numbers, and the
        // reason a field of lambs is constantly bobbing.
        //
        // AdjustedTickDelay, not the raw number: goals are evaluated every
        // OTHER tick, so MC halves the interval to keep the real-time rate
        // right. Rolling against the raw 1000 made sheep graze half as often
        // as vanilla.
        // /sheepeat sets this; it skips ONLY the dice, so everything below
        // still has to hold.
        const bool forced = m_forceNextUse;
        m_forceNextUse = false;

        const int chance = AdjustedTickDelay(m_animal->IsBaby() ? 50 : 1000);
        if (!forced && level->Random().NextInt(chance) != 0) return false;

        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return false;

        const glm::ivec3 p = m_animal->BlockPosition();
        // MC checks the plant AT the mob first, then the grass block below.
        // Both count, and both regrow wool — `mob.ate()` is called in either
        // branch.
        if (IsEdibleForSheep(blocks->GetBlock(p.x, p.y, p.z))) return true;
        return blocks->GetBlock(p.x, p.y - 1, p.z) == BlockID::Grass;
    }

    bool EatBlockGoal::CanContinueToUse() { return m_eatAnimationTick > 0; }

    void EatBlockGoal::Start() {
        m_eatAnimationTick = AdjustedTickDelay(kEatAnimationTicks);
        // The client plays the head-down animation off this event; the timing
        // has to come from the server or host and joiner disagree.
        if (EntityLevel* level = m_animal->Level()) {
            level->BroadcastEntityEvent(*m_animal, 10);
        }
        m_animal->GetNavigation().Stop();
    }

    void EatBlockGoal::Stop() { m_eatAnimationTick = 0; }

    void EatBlockGoal::Tick() {
        m_eatAnimationTick = std::max(0, m_eatAnimationTick - 1);
        // MC fires the effect exactly 4 ticks before the animation ends, which
        // is when the head is down.
        if (m_eatAnimationTick != AdjustedTickDelay(4)) return;

        EntityLevel* level = m_animal->Level();
        if (!level) return;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return;

        const glm::ivec3 p = m_animal->BlockPosition();
        const glm::ivec3 below(p.x, p.y - 1, p.z);

        // MC EatBlockGoal.tick — the grazing actually CONSUMES the block, and
        // which one it takes decides what is left behind:
        //   • an edible plant at the mob      -> destroyed outright
        //   • otherwise a grass block below   -> turned to DIRT
        // Both are gated on the mobGriefing gamerule, and `ate()` fires either
        // way, so a sheep on protected land still regrows its wool.
        if (IsEdibleForSheep(blocks->GetBlock(p.x, p.y, p.z))) {
            if (level->MobGriefing()) level->DestroyBlock(p, false);
            m_animal->OnEatBlock();
        } else if (blocks->GetBlock(below.x, below.y, below.z) == BlockID::Grass) {
            if (level->MobGriefing()) level->SetBlock(below, BlockID::Dirt);
            m_animal->OnEatBlock();
        }
    }


    void TemptGoal::ClearReferenceTo(const Entity* entity) {
        if (m_player == entity) m_player = nullptr;
    }

    void BreedGoal::ClearReferenceTo(const Entity* entity) {
        if (m_partner == entity) m_partner = nullptr;
    }

    void FollowParentGoal::ClearReferenceTo(const Entity* entity) {
        if (m_parent == entity) m_parent = nullptr;
    }

} // namespace Game
