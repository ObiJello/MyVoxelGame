// File: src/common/entity/ai/goals/LlamaGoals.cpp
#include "common/entity/ai/goals/LlamaGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/physics/Physics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Game {

    LlamaFollowCaravanGoal::LlamaFollowCaravanGoal(Llama* llama, double speedModifier)
        : m_llama(llama), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool LlamaFollowCaravanGoal::CanUse() {
        if (m_llama->IsLeashed() || m_llama->InCaravan()) return false;
        EntityLevel* level = m_llama->Level();
        if (!level) return false;

        // getEntities(this, getBoundingBox().inflate(9, 4, 9), LLAMA or
        // TRADER_LLAMA).
        AABB box = m_llama->GetAABB();
        box.min -= glm::vec3(9.0f, 4.0f, 9.0f);
        box.max += glm::vec3(9.0f, 4.0f, 9.0f);
        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_llama, nearby);
        std::vector<Llama*> llamas;
        llamas.reserve(nearby.size());
        for (Entity* e : nearby) {
            if (!e || (e->GetType() != EntityTypeId::Llama && e->GetType() != EntityTypeId::TraderLlama)) continue;
            if (auto* llama = dynamic_cast<Llama*>(e)) llamas.push_back(llama);
        }

        // First choice: the end of an existing caravan; MC keeps the LAST of
        // equally near candidates (`!(dist > closest)`).
        Llama* closest = nullptr;
        double closestDistSquare = std::numeric_limits<double>::max();
        for (Llama* candidate : llamas) {
            if (candidate->InCaravan() && !candidate->HasCaravanTail()) {
                const double distSquare = m_llama->DistanceToSqr(*candidate);
                if (!(distSquare > closestDistSquare)) {
                    closestDistSquare = distSquare;
                    closest = candidate;
                }
            }
        }
        // Else a llama on a lead with nobody behind it.
        if (!closest) {
            for (Llama* candidate : llamas) {
                if (candidate->IsLeashed() && !candidate->HasCaravanTail()) {
                    const double distSquare = m_llama->DistanceToSqr(*candidate);
                    if (!(distSquare > closestDistSquare)) {
                        closestDistSquare = distSquare;
                        closest = candidate;
                    }
                }
            }
        }

        if (!closest) return false;
        if (closestDistSquare < 4.0) return false;
        if (!closest->IsLeashed() && !FirstIsLeashed(*closest, 1)) return false;
        m_llama->JoinCaravan(*closest);
        return true;
    }

    bool LlamaFollowCaravanGoal::CanContinueToUse() {
        Llama* head = m_llama->GetCaravanHead();
        if (!m_llama->InCaravan() || !head || !head->IsAlive() || !FirstIsLeashed(*m_llama, 0)) return false;
        const double distSqr = m_llama->DistanceToSqr(*head);
        if (distSqr > 676.0) {
            if (m_speedModifier <= 3.0) {
                m_speedModifier *= 1.2;
                m_distCheckCounter = ReducedTickDelay(40);
                return true;
            }
            if (m_distCheckCounter == 0) return false;
        }
        if (m_distCheckCounter > 0) --m_distCheckCounter;
        return true;
    }

    void LlamaFollowCaravanGoal::Stop() {
        m_llama->LeaveCaravan();
        m_speedModifier = 2.1;
    }

    void LlamaFollowCaravanGoal::Tick() {
        if (!m_llama->InCaravan()) return;
        // A chain tied to a fence post waits where it is.
        const Entity* holder = m_llama->GetLeashHolder();
        if (holder && holder->GetType() == EntityTypeId::LeashKnot) return;
        const Llama* follows = m_llama->GetCaravanHead();
        if (!follows) return;
        const double distanceTo = m_llama->DistanceTo(*follows);
        // Vec3.normalize (zero below 1e-5) scaled to the gap past 2 blocks.
        glm::dvec3 delta = follows->position - m_llama->position;
        const double length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        delta = length < 1.0e-5 ? glm::dvec3(0.0) : delta / length;
        delta *= std::max(distanceTo - 2.0, 0.0);
        m_llama->GetNavigation().MoveTo(m_llama->position.x + delta.x, m_llama->position.y + delta.y,
                                        m_llama->position.z + delta.z, m_speedModifier);
    }

    bool LlamaFollowCaravanGoal::FirstIsLeashed(const Llama& current, int counter) const {
        // MC's recursion, unrolled: at most kCaravanLimit links ahead.
        const Llama* mob = &current;
        while (counter <= kCaravanLimit) {
            if (!mob->InCaravan()) return false;
            const Llama* head = mob->GetCaravanHead();
            if (!head) return false;
            if (head->IsLeashed()) return true;
            mob = head;
            ++counter;
        }
        return false;
    }

} // namespace Game
