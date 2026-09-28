// File: src/common/entity/raid/Raider.cpp
//
// See Raider.hpp. References (minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/): world/entity/monster/PatrollingMonster.java, world/entity/
// raid/Raider.java, world/entity/monster/illager/AbstractIllager.java.
#include "common/entity/raid/Raider.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/ai/goals/RaiderGoals.hpp"
#include "common/entity/raid/OminousBanner.hpp"

namespace Game {

    // ── PatrollingMonster ──────────────────────────────────────────────────

    PatrollingMonster::PatrollingMonster(EntityTypeId type, EntityLevel* level)
        : Monster(type, level) {}

    void PatrollingMonster::RegisterGoals() {
        m_goalSelector.AddGoal(4, std::make_unique<LongDistancePatrolGoal>(this, 0.7, 0.595));
    }

    void PatrollingMonster::FindPatrolTarget() {
        if (!m_level) return;
        JavaRandom& random = m_level->Random();
        const glm::ivec3 pos = BlockPosition();
        // blockPosition().offset(-500 + nextInt(1000), 0, -500 + nextInt(1000)).
        const int dx = -500 + random.NextInt(1000);
        const int dz = -500 + random.NextInt(1000);
        m_patrolTarget = glm::ivec3(pos.x + dx, pos.y, pos.z + dz);
        m_patrolling = true;
    }

    std::shared_ptr<SpawnGroupData>
    PatrollingMonster::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level && reason != SpawnReason::Patrol && reason != SpawnReason::Event &&
            reason != SpawnReason::Structure && m_level->Random().NextFloat() < kLeaderChance &&
            CanBeLeader()) {
            m_patrolLeader = true;
        }
        if (IsPatrolLeader()) {
            SetEquipment(EquipmentSlot::HEAD, Raid::GetOminousBannerInstance());
            SetEquipmentDropChance(EquipmentSlot::HEAD, kGuaranteedDropChance);
        }
        if (reason == SpawnReason::Patrol) m_patrolling = true;
        return Monster::FinalizeSpawn(reason, std::move(groupData));
    }

    bool PatrollingMonster::RemoveWhenFarAway(double distanceToClosestPlayerSq) const {
        return !m_patrolling || distanceToClosestPlayerSq > kPatrolDespawnDistanceSqr;
    }

    // ── Raider ─────────────────────────────────────────────────────────────

    Raider::Raider(EntityTypeId type, EntityLevel* level) : PatrollingMonster(type, level) {}

    void Raider::RegisterGoals() {
        PatrollingMonster::RegisterGoals();
        // MC then adds ObtainRaidLeaderBannerGoal (1), PathfindToRaidGoal (3),
        // RaiderMoveThroughVillageGoal (4) and RaiderCelebration (5) — every
        // one gated on an active raid; raids are not ported (Raider.hpp).
    }

    void Raider::UpdateNoActionTime() {
        SetNoActionTime(GetNoActionTime() + 2);
    }

    bool Raider::IsCaptain() const {
        return IsPatrolLeader() && Raid::IsOminousBanner(GetEquipment(EquipmentSlot::HEAD));
    }

    std::shared_ptr<SpawnGroupData>
    Raider::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        SetCanJoinRaid(GetType() != EntityTypeId::Witch || reason != SpawnReason::Natural);
        return PatrollingMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── AbstractIllager ────────────────────────────────────────────────────

    AbstractIllager::AbstractIllager(EntityTypeId type, EntityLevel* level) : Raider(type, level) {}

    bool AbstractIllager::CanAttack(const LivingEntity& target) const {
        const EntityTypeId type = target.GetType();
        if ((type == EntityTypeId::Villager || type == EntityTypeId::WanderingTrader) && target.IsBaby()) {
            return false;
        }
        return Raider::CanAttack(target);
    }

    bool AbstractIllager::ConsidersEntityAsAlly(const Entity& other) const {
        // super.considersEntityAsAlly is the scoreboard team test (no teams:
        // false); then #illager_friends with both sides teamless.
        return Raiders::IsIllagerFriend(other.GetType());
    }

    // ── Raiders ────────────────────────────────────────────────────────────

    bool Raiders::IsIllagerFriend(EntityTypeId type) {
        switch (type) {
            case EntityTypeId::Evoker:
            case EntityTypeId::Illusioner:
            case EntityTypeId::Pillager:
            case EntityTypeId::Vindicator:
                return true;
            default:
                return false;
        }
    }

    bool Raiders::IsRaider(const LivingEntity& entity) {
        return dynamic_cast<const Raider*>(&entity) != nullptr;
    }

    bool Raiders::IsAlliedTo(const Entity& a, const Entity& b) {
        if (&a == &b) return true;
        // Only illagers (and an evoker's vexes, through their owner) have an
        // ally rule; everything else is teamless and allied to nothing. The
        // type test keeps the per-candidate cost off every other mob.
        const auto considers = [](const Entity& self, const Entity& other) {
            if (!IsIllagerFriend(self.GetType())) return false;
            const auto* illager = dynamic_cast<const AbstractIllager*>(&self);
            return illager && illager->ConsidersEntityAsAlly(other);
        };
        return considers(a, b) || considers(b, a);
    }

} // namespace Game
