// File: src/common/entity/raid/Raider.hpp
//
// MC's raider class chain, the three layers between Monster and the
// illagers / witch / ravager:
//
//   PatrollingMonster (world/entity/monster/PatrollingMonster) — the patrol:
//     a patrol target, the leader flag and the patrolling flag (all saved:
//     "patrol_target", "PatrolLeader", "Patrolling"), the 6% leader roll on
//     spawn (never for PATROL / EVENT / STRUCTURE spawns, never for a mob
//     that cannot lead), the leader's ominous banner on its head (drop
//     chance 2.0), and the patroller's despawn rule (kept until 128 blocks
//     out while patrolling). Registers LongDistancePatrolGoal at 4.
//   Raider (world/entity/raid/Raider) — the raid member: its wave and the
//     CanJoinRaid flag (saved), the captain test (a patrol leader wearing the
//     ominous banner — the pillager loot table's ominous-bottle predicate),
//     and the doubled idle clock (noActionTime += 2 a tick, whatever the
//     light).
//   AbstractIllager (world/entity/monster/illager/AbstractIllager) — spares
//     baby villagers (canAttack) and counts every #illager_friends mob as an
//     ally (considersEntityAsAlly; no scoreboard teams exist here, so the
//     both-teamless half always holds).
//
// RAIDS ARE NOT PORTED (Raid / Raids: the Bad-Omen-in-a-village trigger,
// waves, the boss bar, hero of the village). What rides on them is absent
// rather than stubbed: Raider's ObtainRaidLeaderBannerGoal,
// PathfindToRaidGoal, RaiderMoveThroughVillageGoal and RaiderCelebration
// (each gated on an active raid in MC), the raid join in aiStep, the RaidId
// save key, applyRaidBuffs, the celebrating flag and AbstractIllager's
// RaiderOpenDoorGoal. Without a raid, MC's canJoinPatrol is always true,
// hasRaid() always false and getCurrentRaid() always null — the forms used
// below.
#pragma once

#include "common/entity/Monster.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace Game {

    class PatrollingMonster : public Monster {
    public:
        // PatrollingMonster.finalizeSpawn's leader chance.
        static constexpr float kLeaderChance = 0.06f;
        // removeWhenFarAway: a patroller is kept inside 128 blocks.
        static constexpr double kPatrolDespawnDistanceSqr = 16384.0;

        PatrollingMonster(EntityTypeId type, EntityLevel* level);

        // MC canBeLeader — true; the witch and the ravager never lead.
        virtual bool CanBeLeader() const { return true; }
        // MC canJoinPatrol — true (Raider: !hasActiveRaid(); no raids).
        virtual bool CanJoinPatrol() const { return true; }

        const std::optional<glm::ivec3>& GetPatrolTarget() const { return m_patrolTarget; }
        bool HasPatrolTarget() const { return m_patrolTarget.has_value(); }
        // MC setPatrolTarget — also marks the mob patrolling.
        void SetPatrolTarget(const glm::ivec3& target) { m_patrolTarget = target; m_patrolling = true; }
        bool IsPatrolLeader() const { return m_patrolLeader; }
        // MC setPatrolLeader — also marks the mob patrolling.
        void SetPatrolLeader(bool leader) { m_patrolLeader = leader; m_patrolling = true; }
        bool IsPatrolling() const { return m_patrolling; }
        void SetPatrolling(bool patrolling) { m_patrolling = patrolling; }
        // MC findPatrolTarget: a column within 500 blocks of the mob's CURRENT
        // block position (the patrol spawner calls it before placing the mob,
        // so a patrol leader heads for a point within 500 blocks of the world
        // origin — MC's behaviour, kept).
        void FindPatrolTarget();

        // The saved fields, restored verbatim (readAdditionalSaveData) — no
        // setter side effects.
        void RestorePatrolState(std::optional<glm::ivec3> target, bool leader, bool patrolling) {
            m_patrolTarget = target;
            m_patrolLeader = leader;
            m_patrolling = patrolling;
        }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC removeWhenFarAway: !patrolling || distSqr > 16384.
        bool RemoveWhenFarAway(double distanceToClosestPlayerSq) const override;

    protected:
        // MC PatrollingMonster.registerGoals: LongDistancePatrolGoal(0.7,
        // 0.595) at 4. Called first thing by every subclass's RegisterGoals
        // (MC's super.registerGoals()); never from a constructor here.
        void RegisterGoals() override;

    private:
        std::optional<glm::ivec3> m_patrolTarget;
        bool m_patrolLeader = false;
        bool m_patrolling = false;
    };

    class Raider : public PatrollingMonster {
    public:
        Raider(EntityTypeId type, EntityLevel* level);

        int  GetWave() const { return m_wave; }
        void SetWave(int wave) { m_wave = wave; }
        bool CanJoinRaid() const { return m_canJoinRaid; }
        void SetCanJoinRaid(bool v) { m_canJoinRaid = v; }

        // MC Raider.isCaptain: the head slot holds exactly the ominous banner
        // and the mob is a patrol leader.
        bool IsCaptain() const;

        // MC Raider.finalizeSpawn: every raider may join a raid except a
        // naturally spawned witch; then PatrollingMonster's.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        // MC Raider.registerGoals: super, then the four raid goals (absent —
        // see the header note).
        void RegisterGoals() override;
        // MC Raider.updateNoActionTime: += 2, no light test.
        void UpdateNoActionTime() override;

    private:
        int  m_wave = 0;
        bool m_canJoinRaid = false;
    };

    class AbstractIllager : public Raider {
    public:
        // MC AbstractIllager.IllagerArmPose, in ordinal order (the renderer's
        // pose ids).
        enum class IllagerArmPose : uint8_t {
            Crossed = 0, Attacking = 1, Spellcasting = 2, BowAndArrow = 3,
            CrossbowHold = 4, CrossbowCharge = 5, Celebrating = 6, Neutral = 7,
        };

        AbstractIllager(EntityTypeId type, EntityLevel* level);

        // MC AbstractIllager.canAttack: a baby villager (or wandering trader)
        // is never a target.
        bool CanAttack(const LivingEntity& target) const override;

        // MC AbstractIllager.considersEntityAsAlly: an #illager_friends mob
        // (both without a team — always, here). Virtual: the evoker adds its
        // own vexes' owners.
        virtual bool ConsidersEntityAsAlly(const Entity& other) const;

    protected:
        void RegisterGoals() override { Raider::RegisterGoals(); }
    };

    namespace Raiders {
        // MC EntityTypeTags.ILLAGER_FRIENDS (= #illager: evoker, illusioner,
        // pillager, vindicator).
        bool IsIllagerFriend(EntityTypeId type);
        // MC EntityTypeTags.RAIDERS — the HurtByTargetGoal(this, Raider.class)
        // friendly-fire exemption.
        bool IsRaider(const LivingEntity& entity);
        // MC Entity.isAlliedTo(other): the same entity, or either side
        // considering the other an ally (the illager rule; no teams).
        bool IsAlliedTo(const Entity& a, const Entity& b);
    }

} // namespace Game
