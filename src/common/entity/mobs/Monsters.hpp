// File: src/common/entity/mobs/Monsters.hpp
//
// Zombie, Skeleton, Creeper and Spider.
//
// Each is a transcription of its MC class's createAttributes() and
// registerGoals(). The PRIORITY NUMBERS are the behaviour — they decide what a
// mob does when two goals both want to move it — so they are reproduced exactly
// and should not be renumbered for tidiness.
#pragma once

#include "common/entity/Monster.hpp"

namespace Game {

    // MC Zombie. Attributes: FOLLOW_RANGE 35, MOVEMENT_SPEED 0.23,
    // ATTACK_DAMAGE 3, ARMOR 2.
    class Zombie : public Monster {
    public:
        explicit Zombie(EntityLevel* level);

        bool IsBaby() const override { return m_baby; }
        void SetBaby(bool baby);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
        virtual void AddBehaviourGoals();

        // MC EntityTypeTags.BURN_IN_DAYLIGHT. The burn itself lives on Mob.
        bool BurnsInDaylight() const override { return true; }

    private:
        bool m_baby = false;
    };

    // MC Skeleton / AbstractSkeleton. MOVEMENT_SPEED 0.25; health and attack
    // damage stay at the Monster defaults (20 / 2).
    //
    // Ranged attack is NOT implemented: arrows need a projectile entity, which
    // is out of scope for this pass. The skeleton therefore registers a melee
    // goal, exactly as MC does when a skeleton has no bow — so the mob is
    // complete and correct, just permanently in its melee configuration.
    //
    // It still CARRIES the bow, because MC's AbstractSkeleton always equips one
    // and the renderer poses the arms from that (ArmPose::BowAndArrow while
    // aggressive). The bow is client-side only: nothing on the server reads an
    // equipment slot yet, so there is no state to sync.
    class Skeleton : public Monster {
    public:
        explicit Skeleton(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
        bool BurnsInDaylight() const override { return true; }
    };

    // MC Creeper. MOVEMENT_SPEED 0.25; the fuse lives here rather than in
    // SwellGoal so that a creeper lit by other means still detonates.
    class Creeper : public Monster {
    public:
        explicit Creeper(EntityLevel* level);

        static constexpr int kMaxSwell = 30;
        static constexpr int kExplosionRadius = 3;

        int  GetSwellDir() const { return m_swellDir; }
        void SetSwellDir(int dir);

        // 0..1, for the renderer's flash-and-inflate. MC divides by
        // (maxSwell - 2) so the creeper reaches full white slightly BEFORE it
        // explodes, which is the visual tell players react to.
        float GetSwelling(float partialTick) const;

        bool IsIgnited() const { return m_ignited; }
        void Ignite() { m_ignited = true; }

        // MC Creeper.doHurtTarget returns true WITHOUT dealing damage —
        // creepers never melee, they only explode.
        bool DoHurtTarget(Entity& target) override { return true; }

        void Tick() override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;

    private:
        void Explode();

        int  m_swellDir = -1;
        int  m_swell = 0;
        int  m_oldSwell = 0;
        bool m_ignited = false;
    };

    // MC Spider. MAX_HEALTH 16, MOVEMENT_SPEED 0.3.
    class Spider : public Monster {
    public:
        explicit Spider(EntityLevel* level);

        // MC Spider.onClimbable — spiders climb walls by treating any
        // horizontal collision as a ladder.
        bool IsClimbing() const { return m_climbing; }

        void Tick() override;
        void Travel(const glm::dvec3& input) override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;

    private:
        // MC SpiderTargetGoal / SpiderAttackGoal: a spider is passive in
        // bright light. Both the acquire and the keep tests consult this.
        static bool IsBrightEnoughToBePassive(Mob& mob);

        bool m_climbing = false;
    };

} // namespace Game
