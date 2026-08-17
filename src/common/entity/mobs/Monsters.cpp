// File: src/common/entity/mobs/Monsters.cpp
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── Zombie ─────────────────────────────────────────────────────────────

    void Zombie::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::FollowRange,   35.0);
        out.Register(Attribute::MovementSpeed,  0.23);
        out.Register(Attribute::AttackDamage,   3.0);
        out.Register(Attribute::Armor,          2.0);
        out.Register(Attribute::SpawnReinforcements, 0.0);
    }

    Zombie::Zombie(EntityLevel* level) : Monster(EntityTypeId::Zombie, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Zombie::SetBaby(bool baby) {
        if (m_baby == baby) return;
        m_baby = baby;

        // MC SPEED_MODIFIER_BABY: +50% movement speed, as a multiplier on the
        // base rather than a flat add, so it scales with any other modifier.
        if (baby) {
            m_attributes.AddModifier(Attribute::MovementSpeed,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::BabySpeedBoost), 0.5,
                                   AttributeOperation::AddMultipliedBase });
        } else {
            m_attributes.RemoveModifier(Attribute::MovementSpeed, ModifierId::BabySpeedBoost);
        }
    }

    void Zombie::RegisterGoals() {
        // MC Zombie.registerGoals. The turtle-egg and village goals are omitted
        // (no turtles, no villages); everything else is priority-for-priority.
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        AddBehaviourGoals();
    }

    void Zombie::AddBehaviourGoals() {
        m_goalSelector.AddGoal(3, std::make_unique<ZombieAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));

        // Alerting others is what turns hitting one zombie into a group fight.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, true));
    }

    // ── Skeleton ───────────────────────────────────────────────────────────

    void Skeleton::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Skeleton::Skeleton(EntityLevel* level) : Monster(EntityTypeId::Skeleton, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();

        // avoidSun is NOT pinned on here. MC toggles it from RestrictSunGoal
        // with the daylight; setting it once at construction made a skeleton
        // refuse sunlit paths at midnight too.
    }

    void Skeleton::RegisterGoals() {
        // MC AbstractSkeleton.registerGoals. The bow goal is replaced by the
        // melee goal MC itself uses when a skeleton has no bow — see the
        // header note.
        //
        // The sun goals used to be described as "covered by the navigation's
        // avoidSun flag", but that flag was only ever set once at construction:
        // it made the skeleton route around sunlight day AND night, and it
        // never made a burning one actively run for shade. These are MC's.
        m_goalSelector.AddGoal(2, std::make_unique<RestrictSunGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<FleeSunGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.2, false));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, true));
    }

    // ── Creeper ────────────────────────────────────────────────────────────

    void Creeper::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Creeper::Creeper(EntityLevel* level) : Monster(EntityTypeId::Creeper, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Creeper::RegisterGoals() {
        // MC Creeper.registerGoals, minus the ocelot/cat avoidance (neither mob
        // exists here). Note SwellGoal at 2 outranks the melee goal at 4: once
        // the fuse is lit, nothing else moves the creeper.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<SwellGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackablePlayerGoal>(this, true));
        m_targetSelector.AddGoal(2, std::make_unique<HurtByTargetGoal>(this));
    }

    void Creeper::SetSwellDir(int dir) { m_swellDir = dir; }

    float Creeper::GetSwelling(float partialTick) const {
        const float lerped = static_cast<float>(m_oldSwell) +
                             partialTick * static_cast<float>(m_swell - m_oldSwell);
        return lerped / static_cast<float>(kMaxSwell - 2);
    }

    void Creeper::Tick() {
        // MC runs the fuse BEFORE super.tick(), so a creeper that reaches full
        // swell explodes on the same tick rather than getting one more of AI.
        m_oldSwell = m_swell;

        if (m_ignited) SetSwellDir(1);

        m_swell += m_swellDir;
        if (m_swell < 0) m_swell = 0;

        if (m_swell >= kMaxSwell) {
            m_swell = kMaxSwell;
            Explode();
        }

        Monster::Tick();
    }

    void Creeper::Explode() {
        if (!IsEffectiveAi() || IsRemoved()) return;

        // No explosion system exists in this engine yet. What IS modelled is
        // the part that matters for gameplay parity: everything in radius takes
        // damage falling off with distance, exactly as MC's Explosion does for
        // entities. Terrain destruction is deliberately left out rather than
        // approximated — a half-implemented block-removal pass would be worse
        // than none, and this is the single place to add it.
        const double radius = static_cast<double>(kExplosionRadius);

        AABB box = GetAABB();
        box.min -= glm::vec3(radius * 2.0);
        box.max += glm::vec3(radius * 2.0);

        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);

        for (Entity* e : nearby) {
            LivingEntity* living = dynamic_cast<LivingEntity*>(e);
            if (!living) continue;

            const double dist = std::sqrt(DistanceToSqr(*living));
            if (dist > radius * 2.0) continue;

            // MC's falloff: full damage at the centre, zero at 2x radius.
            const double factor = 1.0 - dist / (radius * 2.0);
            const float damage = static_cast<float>((factor * factor + factor) * 3.5 * radius * 2.0 + 1.0);
            living->Hurt(MobDamageSource::Explosion, damage, this);
        }

        m_level->BroadcastEntityEvent(*this, 60);   // poof
        Discard();
    }

    // ── Spider ─────────────────────────────────────────────────────────────

    void Spider::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    16.0);
        out.Register(Attribute::MovementSpeed, 0.3);
    }

    Spider::Spider(EntityLevel* level) : Monster(EntityTypeId::Spider, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Spider::IsBrightEnoughToBePassive(Mob& mob) {
        EntityLevel* level = mob.Level();
        if (!level) return false;
        const glm::ivec3 p = mob.BlockPosition();
        // MC: spiders stop acquiring targets at half brightness or above, which
        // is why they are harmless in daylight but hostile in caves and at night.
        return level->GetMaxLocalRawBrightness(p.x, p.y, p.z) < 8;
    }

    void Spider::RegisterGoals() {
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));

        auto targetPlayer = std::make_unique<NearestAttackablePlayerGoal>(this, true);
        targetPlayer->SetExtraCondition(&Spider::IsBrightEnoughToBePassive);
        m_targetSelector.AddGoal(2, std::move(targetPlayer));
    }

    void Spider::Tick() {
        Monster::Tick();
        // MC sets the climbing flag from horizontalCollision on the server and
        // syncs it; the client reads it to decide whether to draw the spider
        // flat against a wall.
        if (IsEffectiveAi()) m_climbing = horizontalCollision;
    }

    void Spider::Travel(const glm::dvec3& input) {
        Monster::Travel(input);

        // MC handles this through onClimbable() inside
        // handleRelativeFrictionAndCalculateMovement, which clamps vertical
        // motion to 0.2 upward while climbing. Applying it after the base
        // travel gives the same result for a spider (it has no other source of
        // upward motion) without threading a climbable flag through
        // LivingEntity for the one mob that uses it.
        if (m_climbing && (horizontalCollision || jumping)) {
            velocity.y = 0.2;
        }
    }

} // namespace Game
