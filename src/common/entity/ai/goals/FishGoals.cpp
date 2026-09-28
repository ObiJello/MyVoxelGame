// File: src/common/entity/ai/goals/FishGoals.cpp
#include "common/entity/ai/goals/FishGoals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <vector>

namespace Game {

    // ── FishSwimGoal ───────────────────────────────────────────────────────

    FishSwimGoal::FishSwimGoal(Fish* fish)
        : RandomSwimmingGoal(fish, 1.0, 40), m_fish(fish) {}

    bool FishSwimGoal::CanUse() {
        return m_fish->CanRandomSwim() && RandomSwimmingGoal::CanUse();
    }

    // ── PufferfishPuffGoal ─────────────────────────────────────────────────

    bool PufferfishPuffGoal::CanUse() {
        // MC: any scary LivingEntity within the fish's box inflated by 2.
        EntityLevel* level = m_fish->Level();
        if (!level) return false;

        AABB box = m_fish->GetAABB();
        box.min -= glm::vec3(2.0f);
        box.max += glm::vec3(2.0f);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_fish, nearby);
        for (Entity* e : nearby) {
            const auto* living = dynamic_cast<const LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (Pufferfish::IsScaryTarget(*living)) return true;
        }
        return false;
    }

    void PufferfishPuffGoal::Start() {
        // MC: inflateCounter = 1; deflateTimer = 0.
        m_fish->StartPuffing();
    }

    void PufferfishPuffGoal::Stop() {
        m_fish->StopPuffing();
    }

    // ── FollowFlockLeaderGoal ──────────────────────────────────────────────

    FollowFlockLeaderGoal::FollowFlockLeaderGoal(SchoolingFish* fish) : m_fish(fish) {
        m_nextStartTick = NextStartTick();
    }

    int FollowFlockLeaderGoal::NextStartTick() const {
        // MC: reducedTickDelay(200 + nextInt(200) % 20).
        return ReducedTickDelay(kIntervalTicks +
                                m_fish->Level()->Random().NextInt(200) % 20);
    }

    bool FollowFlockLeaderGoal::CanUse() {
        if (m_fish->HasFollowers()) return false;
        if (m_fish->IsFollower()) return true;
        if (m_nextStartTick > 0) {
            --m_nextStartTick;
            return false;
        }
        m_nextStartTick = NextStartTick();

        // MC: every same-class fish within the box inflated by 8 that
        // either leads a school with room or follows nobody — THIS fish
        // included (getEntitiesOfClass does not skip the caller). Any leader
        // with room among them takes the lot (DataFixUtils.orElse(…
        // canBeFollowed().findAny(), this)): every non-follower in the list,
        // up to its free places, joins it — so a lone fish meeting a school
        // brings its own leaderless neighbours along, and with no such leader
        // this fish recruits them itself.
        EntityLevel* level = m_fish->Level();
        if (!level) return false;

        AABB box = m_fish->GetAABB();
        box.min -= glm::vec3(8.0f);
        box.max += glm::vec3(8.0f);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, nullptr, nearby);

        std::vector<SchoolingFish*> candidates;
        candidates.reserve(nearby.size());
        for (Entity* e : nearby) {
            if (e->GetType() != m_fish->GetType()) continue;
            auto* other = dynamic_cast<SchoolingFish*>(e);
            if (!other || !other->IsAlive()) continue;
            if (other->CanBeFollowed() || !other->IsFollower()) candidates.push_back(other);
        }
        // The caller is always its own candidate (it follows nobody here).
        if (std::find(candidates.begin(), candidates.end(), m_fish) == candidates.end()) {
            candidates.push_back(m_fish);
        }

        SchoolingFish* leader = m_fish;
        for (SchoolingFish* other : candidates) {
            if (other->CanBeFollowed()) { leader = other; break; }
        }
        std::vector<SchoolingFish*> loners;
        loners.reserve(candidates.size());
        for (SchoolingFish* other : candidates) {
            if (!other->IsFollower()) loners.push_back(other);
        }
        leader->AddFollowers(loners);
        return m_fish->IsFollower();
    }

    bool FollowFlockLeaderGoal::CanContinueToUse() {
        return m_fish->IsFollower() && m_fish->InRangeOfLeader();
    }

    void FollowFlockLeaderGoal::Start() {
        m_timeToRecalcPath = 0;
    }

    void FollowFlockLeaderGoal::Stop() {
        m_fish->StopFollowing();
    }

    void FollowFlockLeaderGoal::Tick() {
        if (--m_timeToRecalcPath <= 0) {
            m_timeToRecalcPath = AdjustedTickDelay(10);
            m_fish->PathToLeader();
        }
    }

    // ── SquidRandomMovementGoal ────────────────────────────────────────────

    void SquidRandomMovementGoal::Tick() {
        // MC: an idle squid (100+ ticks without action) parks; otherwise a
        // fresh direction on a ~50-tick roll, or immediately when the current
        // vector is spent or the squid left the water.
        if (m_squid->GetNoActionTime() > 100) {
            m_squid->SetMovementVector(glm::dvec3(0.0));
            return;
        }
        JavaRandom& rng = m_squid->Level()->Random();
        if (rng.NextInt(ReducedTickDelay(50)) == 0 || !m_squid->IsInWater() ||
            !m_squid->HasMovementVector()) {
            const float angle = rng.NextFloat() * 2.0f * 3.14159265358979323846f;
            m_squid->SetMovementVector(glm::dvec3(
                std::cos(angle) * 0.2f,
                -0.1f + rng.NextFloat() * 0.2f,
                std::sin(angle) * 0.2f));
        }
    }

    // ── SquidFleeGoal ──────────────────────────────────────────────────────

    bool SquidFleeGoal::CanUse() {
        Entity* enemy = m_squid->GetLastHurtByMob();
        if (!m_squid->IsInWater() || !enemy) return false;
        return m_squid->DistanceToSqr(*enemy) < 100.0;
    }

    void SquidFleeGoal::Tick() {
        ++m_fleeTicks;
        Entity* enemy = m_squid->GetLastHurtByMob();
        if (!enemy) return;

        glm::dvec3 away = m_squid->position - enemy->position;
        const double len = glm::length(away);
        if (len < 1.0e-8) return;
        away /= len;

        // MC caps the escape at 10 blocks of separation and climbs while
        // shallow; the essential is the tripled jet directly away.
        if (len < 10.0) {
            m_squid->SetMovementVector(away * (0.2 * 3.0));
        } else if (m_fleeTicks > 100) {
            m_squid->SetMovementVector(glm::dvec3(0.0));
        }
    }

} // namespace Game
