// File: src/common/entity/Mob.cpp
#include "common/entity/Mob.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    Mob::Mob(EntityTypeId type, EntityLevel* level)
        : LivingEntity(type, level) {
        CreateMobAttributes(m_attributes);
        m_health = GetMaxHealth();

        m_moveControl         = std::make_unique<MoveControl>(this);
        m_lookControl         = std::make_unique<LookControl>(this);
        m_jumpControl         = std::make_unique<JumpControl>(this);
        m_bodyRotationControl = std::make_unique<BodyRotationControl>(this);
        m_sensing             = std::make_unique<Sensing>(this);
        m_navigation          = std::make_unique<GroundPathNavigation>(this, level);

        // RegisterGoals is NOT called here. A virtual call during base
        // construction dispatches to Mob's own (empty) version, so every
        // concrete mob calls it at the end of its own constructor instead.
    }

    Mob::~Mob() = default;

    PathNavigation&       Mob::GetNavigation()       { return *m_navigation; }
    const PathNavigation& Mob::GetNavigation() const { return *m_navigation; }
    Sensing&              Mob::GetSensing()          { return *m_sensing; }

    float Mob::GetPathfindingMalus(PathType type) const {
        const auto it = m_pathfindingMalus.find(static_cast<uint8_t>(type));
        if (it != m_pathfindingMalus.end()) return it->second;
        return GetDefaultPathMalus(type);
    }

    void Mob::SetPathfindingMalus(PathType type, float malus) {
        m_pathfindingMalus[static_cast<uint8_t>(type)] = malus;
    }

    bool Mob::CanAttack(const LivingEntity& target) const {
        return target.IsAttackable() && target.IsAlive();
    }

    void Mob::StopInPlace() {
        m_navigation->Stop();
        xxa = 0.0f;
        yya = 0.0f;
        m_speed = 0.0f;
        zza = 0.0f;
        velocity = glm::dvec3(0.0);
    }

    bool Mob::IsWithinMeleeAttackRange(const LivingEntity& target) const {
        // MC inflates the ATTACKER's box by the reach and tests overlap. A
        // centre-to-centre distance check instead makes wide mobs unable to hit
        // something their own body is already touching, and gives tall mobs a
        // reach that varies with the target's height.
        AABB reach = GetAABB();
        reach.min -= glm::vec3(kDefaultAttackReach, 0.0f, kDefaultAttackReach);
        reach.max += glm::vec3(kDefaultAttackReach, 0.0f, kDefaultAttackReach);
        return reach.Intersects(target.GetAABB());
    }

    bool Mob::DoHurtTarget(Entity& target) {
        const float damage = static_cast<float>(GetAttributeValue(Attribute::AttackDamage));

        LivingEntity* living = dynamic_cast<LivingEntity*>(&target);
        if (!living) return false;

        const bool hit = living->Hurt(MobDamageSource::MobAttack, damage, this);
        if (hit) {
            // MC's extra knockback from ATTACK_KNOCKBACK, on top of the base
            // 0.4 that Hurt already applied. Zero for all eight of our mobs, so
            // this is a no-op today and correct the moment one gains the
            // attribute.
            const double extra = GetAttributeValue(Attribute::AttackKnockback) / 2.0;
            if (extra > 0.0) {
                const float angle = yRot * Mth::kDegToRad;
                // MC LivingEntity.doHurtTarget:2572 — (sin, -cos), the facing
                // NEGATED, because Knockback subtracts the impulse it is given.
                // The un-negated form pushed the victim toward the attacker.
                living->Knockback(extra, std::sin(angle), -std::cos(angle));
                velocity.x *= 0.6;
                velocity.z *= 0.6;
            }
            SetLastHurtMob(&target);
        }
        return hit;
    }

    void Mob::UpdateControlFlags() {
        // MC toggles MOVE/JUMP/LOOK off while something else is steering this
        // mob (a rider). Nothing rides anything here yet, so all three stay
        // enabled — the cadence is kept so the hook exists where MC has it.
        const bool enabled = true;
        m_goalSelector.SetControlFlag(GoalFlag::Move, enabled);
        m_goalSelector.SetControlFlag(GoalFlag::Jump, enabled);
        m_goalSelector.SetControlFlag(GoalFlag::Look, enabled);
    }

    void Mob::ServerAiStep() {
        PROFILE_ZONE_N("Mob.ServerAiStep");

        ++m_noActionTime;

        m_sensing->Tick();

        // ── The 2-tick evaluation cadence ──────────────────────────────────
        //
        // Adding the entity id staggers mobs against each other: a herd spawned
        // on the same tick would otherwise all run their full goal evaluation
        // on the same tick forever, turning a smooth cost into a sawtooth.
        const int idBasedTickCount = tickCount + GetId();
        if (idBasedTickCount % 2 != 0 && tickCount > 1) {
            m_targetSelector.TickRunningGoals(false);
            m_goalSelector.TickRunningGoals(false);
        } else {
            m_targetSelector.Tick();
            m_goalSelector.Tick();
        }

        m_navigation->Tick();

        // MC's brain mobs run their whole AI from customServerAiStep, in the
        // same slot the goal mobs use for their extras.
        if (GetBrain()) {
            TickBrain();
            UpdateBrainActivity();
        }

        CustomServerAiStep();

        // Controls last: they translate everything the goals and navigation
        // decided into the yaw/zza/jumping inputs that LivingEntity::AiStep
        // consumes later in this same tick.
        m_moveControl->Tick();
        m_lookControl->Tick();
        m_jumpControl->Tick();
    }

    void Mob::TickHeadTurn(float /*yBodyRotTarget*/) {
        // Mob replaces LivingEntity's direct body snap with the smoothed
        // control. The target is ignored on purpose — BodyRotationControl reads
        // yRot and the movement delta itself.
        m_bodyRotationControl->ClientTick();
    }

    void Mob::BaseTick() {
        LivingEntity::BaseTick();

        // Ambient sound cadence. No audio is emitted yet; the counter is kept
        // so wiring a sound in later is one call and not a behaviour change.
        if (IsAlive() && m_level) {
            if (m_level->Random().NextInt(1000) < m_ambientSoundTime++) {
                m_ambientSoundTime = -GetAmbientSoundInterval();
            }
        }
    }

    void Mob::Tick() {
        LivingEntity::Tick();

        // MC's `if (this.level().isClientSide()) this.setupAnimationStates();`,
        // in the same place: after the base tick, so walkAnimation and the pose
        // are already this tick's values when the animation state reads them.
        if (m_level && m_level->IsClientSide()) {
            SetupAnimationStates();
        }

        if (m_level && !m_level->IsClientSide() && tickCount % 5 == 0) {
            UpdateControlFlags();
        }

        // A dead or removed target is dropped here rather than in every goal.
        if (m_target && (!m_target->IsAlive() || m_target->IsRemoved())) {
            SetTarget(nullptr);
        }
    }

    void Mob::TickBrain() {
        if (Brain* brain = GetBrain()) {
            if (m_level) brain->Tick(*m_level, *this);
        }
    }

    void Mob::ClearReferenceTo(const Entity* entity) {
        LivingEntity::ClearReferenceTo(entity);
        if (Brain* brain = GetBrain()) brain->ClearReferenceTo(entity);
        if (m_target == entity) m_target = nullptr;
        m_goalSelector.ClearReferenceTo(entity);
        m_targetSelector.ClearReferenceTo(entity);
    }

    void Mob::AiStep() {
        LivingEntity::AiStep();
        if (BurnsInDaylight()) BurnUndead();
    }

    bool Mob::IsSunBurnTick() {
        if (!m_level || m_level->IsClientSide()) return false;

        // MC gates this on the MONSTERS_BURN timeline track, whose window
        // [23460, 12542) is narrower than "is day" at both ends.
        if (!m_level->MonstersBurn()) return false;

        // MC checks the block at the entity's EYE, not its feet — standing in a
        // one-block hole shades a zombie, which is behaviour players use.
        const glm::ivec3 p = BlockPosition();
        const int eyeY = static_cast<int>(std::floor(GetEyeY()));

        // MC LevelReader.getLightLevelDependentMagicValue: the raw brightness
        // pushed through v/(4-3v) and then lerped toward 1 by the dimension's
        // ambientLight, which is 0 in the overworld so the lerp drops out.
        //
        // That curve is why burning stops well before the sky goes dark:
        // br > 0.5 needs v > 0.8, i.e. a sky brightness above 12, i.e. skyDarken
        // below 3 — which the dusk ramp crosses several hundred ticks before
        // MONSTERS_BURN itself turns off.
        const float v =
            static_cast<float>(m_level->GetMaxLocalRawBrightness(p.x, eyeY, p.z)) / 15.0f;
        const float br = v / (4.0f - 3.0f * v);
        if (br <= 0.5f) return false;

        // ~4% per tick at full daylight, so a zombie caught out takes a moment
        // to catch fire rather than igniting the instant the sun clears the
        // horizon. The roll happens BEFORE the water and sky tests in MC, so it
        // is drawn even when those would reject.
        if (m_level->Random().NextFloat() * 30.0f >= (br - 0.4f) * 2.0f) return false;

        // MC also excludes rain and powder snow here; neither is modelled.
        if (IsInWater()) return false;

        return m_level->CanSeeSky(p.x, eyeY, p.z);
    }

    void Mob::BurnUndead() {
        // MC checks a helmet in the sun-protection slot first. Mobs here carry
        // no equipment, so the ignite is unconditional.
        if (IsAlive() && IsSunBurnTick()) IgniteForSeconds(8);
    }

    void Mob::CheckDespawn() {
        if (!m_level || m_level->IsClientSide()) return;

        if (m_level->GetDifficulty() == Difficulty::Peaceful && TypeInfo().notInPeaceful) {
            Discard();
            return;
        }

        if (IsPersistenceRequired()) {
            m_noActionTime = 0;
            return;
        }

        LivingEntity* nearest = m_level->GetNearestPlayer(position.x, position.y, position.z, -1.0);
        if (!nearest) return;

        const double d2 = nearest->DistanceToSqr(*this);
        const int despawnDistance = GetMobCategoryInfo(TypeInfo().category).despawnDistance;
        const int noDespawn = kNoDespawnDistance;

        // Hard cutoff: too far to matter, remove immediately.
        if (d2 > static_cast<double>(despawnDistance) * despawnDistance &&
            RemoveWhenFarAway(d2)) {
            Discard();
            return;
        }

        // Soft cutoff: a mob that has been idle for 30 seconds and is outside
        // the keep-alive radius has a 1-in-800 chance per tick of vanishing.
        // This is what stops a world slowly filling with mobs nobody visits.
        if (m_noActionTime > 600 && m_level->Random().NextInt(800) == 0 &&
            d2 > static_cast<double>(noDespawn) * noDespawn && RemoveWhenFarAway(d2)) {
            Discard();
        } else if (d2 < static_cast<double>(noDespawn) * noDespawn) {
            m_noActionTime = 0;
        }
    }

    // ── PathfinderMob ──────────────────────────────────────────────────────

    bool PathfinderMob::IsPathFinding() const {
        return !m_navigation->IsDone();
    }

} // namespace Game
