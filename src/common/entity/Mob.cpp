// File: src/common/entity/Mob.cpp
#include "common/entity/Mob.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"

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

    // See Mob::NoAiTag. Six make_uniques and the attribute registration, all
    // skipped; everything else about the entity is unchanged.
    Mob::Mob(EntityTypeId type, EntityLevel* level, NoAiTag)
        : LivingEntity(type, level, LivingEntity::NoAttributesTag{}) {
    }

    Mob::~Mob() = default;

    PathNavigation&       Mob::GetNavigation()       { return *m_navigation; }
    const PathNavigation& Mob::GetNavigation() const { return *m_navigation; }

    void Mob::SetLevel(EntityLevel* level) {
        LivingEntity::SetLevel(level);
        if (m_navigation) m_navigation->SetLevel(level);
        // The target lived in the old level. The portal travel code sets a
        // new one (the same player's view in the new level) once it exists.
        m_target = nullptr;
    }
    Sensing&              Mob::GetSensing()          { return *m_sensing; }

    void Mob::SetNavigation(std::unique_ptr<PathNavigation> navigation) {
        m_navigation = std::move(navigation);
    }

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
        if (m_navigation) m_navigation->Stop();
        xxa = 0.0f;
        yya = 0.0f;
        m_speed = 0.0f;
        zza = 0.0f;
        velocity = glm::dvec3(0.0);
    }

    // ── Conversion (MC Mob.convertTo) ──────────────────────────────────────

    void Mob::CopyConversionState(Mob& to) {
        // MC ConversionType.SINGLE.convert + convertCommon, reduced to what
        // this port tracks (the header's comment is the inventory).

        // copyPosition + deltaMovement + the SINGLE extras.
        to.position    = position;
        to.oldPosition = position;
        to.yRot = yRot;   to.yRotO = yRot;
        to.xRot = xRot;   to.xRotO = xRot;
        to.yBodyRot = yBodyRot;   to.yBodyRotO = yBodyRot;
        to.yHeadRot = yHeadRot;   to.yHeadRotO = yHeadRot;
        to.velocity = velocity;
        to.fallDistance = fallDistance;
        to.hurtTime = hurtTime;
        to.onGround = onGround;
        to.needsSync = true;

        // Passenger hand-off: the root passenger dismounts and re-mounts the
        // replacement; a vehicle keeps its (new) rider.
        if (Entity* rootPassenger = GetFirstPassenger()) {
            rootPassenger->StopRiding();
            rootPassenger->StartRiding(to, /*force=*/true);
        }
        if (Entity* vehicle = GetVehicle()) {
            StopRiding();
            to.StartRiding(*vehicle, /*force=*/true);
        }

        // convertCommon: active effects carry over whole.
        for (const MobEffectInstance& effect : ActiveEffects()) {
            to.AddEffect(MobEffectInstance(effect));
        }

        // convertCommon flags this port tracks. (Baby is per-family — Zombie
        // and AgeableMob keep separate machinery — so the family copy owns
        // it; see Zombie::ConvertToZombieType.)
        to.SetCanPickUpLoot(CanPickUpLoot());   // preserveCanPickUpLoot
        to.SetLeftHanded(IsLeftHanded());
        to.SetNoAi(IsNoAi());
        if (IsPersistenceRequired()) to.SetPersistenceRequired(true);
        to.SetRemainingFireTicks(GetRemainingFireTicks());   // setSharedFlagOnFire
    }

    Mob* Mob::FinishConversion(std::unique_ptr<Mob> replacement) {
        Mob* placed = replacement.get();
        if (m_level) {
            m_level->AddFreshEntity(std::move(replacement));
        }
        // MC ConversionType.SINGLE.shouldDiscardAfterConversion() — the old
        // body vanishes; the tracker's remove+add is all the clients see.
        Discard();
        return placed;
    }

    Mob* Mob::ConvertTo(std::unique_ptr<Mob> replacement) {
        // MC Mob.convertTo: a removed mob converts to nothing.
        if (IsRemoved() || !replacement) return nullptr;
        CopyConversionState(*replacement);
        return FinishConversion(std::move(replacement));
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

    bool Mob::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool result = LivingEntity::Hurt(source, amount, attacker);
        // MC LivingEntity.hurtServer → resolvePlayerResponsibleForDamage
        // (LivingEntity.java:1330-1339): an accepted player hit remembers the
        // player for 100 ticks. Projectiles arrive here with the shooter as
        // `attacker` (Arrow.cpp passes its owner), so bow kills credit too.
        // SKIPPED: the tamed-wolf branch (LivingEntity.java:1334-1338), which
        // credits the wolf's OWNER — a wolf kill passes the wolf itself as
        // attacker and no owner reference reaches this seam.
        //
        // Set on the way OUT rather than before the base call: the base Hurt
        // runs Die() on a killing blow, but nothing reads the credit until
        // MobManager's death-drop pass, so the late write is safe and keeps
        // this override purely additive.
        if (result && attacker && attacker->IsPlayer()) {
            m_lastHurtByPlayerId   = attacker->GetId();
            m_lastHurtByPlayerTime = 100;
        }
        return result;
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

        // CATCH-ALL for Entity::HoldsEntityRefs. Individual goals cache their
        // own raw victim pointers (GuardianGoals, EndermanGoals, WitherGoals
        // and friends all carry an m_target), and instrumenting each of those
        // setters would be a use-after-free waiting on the one that gets
        // missed. A goal cannot acquire a reference without TICKING, and the
        // only place goals tick is below — so marking here covers every one of
        // them by construction, including any added later.
        //
        // Deliberately coarse: any mob that has ever run AI keeps paying for
        // the pre-sweep. The entities this exists to spare — primed TNT,
        // falling blocks — never reach this function at all.
        MarkHoldsEntityRefs();

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
        if (m_bodyRotationControl) m_bodyRotationControl->ClientTick();
    }

    void Mob::BaseTick() {
        LivingEntity::BaseTick();

        // MC LivingEntity.baseTick (LivingEntity.java:437-440): the player
        // kill-credit window counts down; expired means the player is
        // forgotten and a later death is not credited.
        if (m_lastHurtByPlayerTime > 0) {
            --m_lastHurtByPlayerTime;
        } else {
            m_lastHurtByPlayerId = -1;
        }

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

        // Riding backup unlink. The protocol paths (Entity::Remove,
        // StopRiding) clear vehicle/passenger pointers before anything dies;
        // this catches direct-erase paths so the invariant "nothing points at
        // a dead mob once the sweep returns" stays total. See the riding
        // lifetime note in Entity.hpp.
        UnlinkRidingReferenceTo(entity);
    }

    void Mob::AiStep() {
        LivingEntity::AiStep();
        if (BurnsInDaylight()) BurnUndead();

        // MC LivingEntity.baseTick: a water-sensitive mob (blaze, snow golem)
        // takes 1 drowning damage per tick while wet. Lives here rather than
        // BaseTick so it stays server-side with the rest of the damage the AI
        // step deals; MC's isInWaterRainOrBubble collapses to IsInWater with
        // no weather system.
        if (IsSensitiveToWater() && IsAlive() && IsInWater() &&
            m_level && !m_level->IsClientSide()) {
            Hurt(MobDamageSource::Drown, 1.0f, nullptr);
        }
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

    std::shared_ptr<SpawnGroupData>
    Mob::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        (void)reason;
        if (m_level) {
            JavaRandom& rng = m_level->Random();

            // MC Mob.finalizeSpawn: RANDOM_SPAWN_BONUS —
            // triangle(0.0, 0.11485) on FOLLOW_RANGE, ADD_MULTIPLIED_BASE.
            m_attributes.RemoveModifier(Attribute::FollowRange, ModifierId::RandomSpawnBonus);
            m_attributes.AddModifier(Attribute::FollowRange,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::RandomSpawnBonus),
                                   rng.Triangle(0.0, 0.11485),
                                   AttributeOperation::AddMultipliedBase });

            SetLeftHanded(rng.NextFloat() < 0.05f);
        }
        return groupData;
    }

    int Mob::GetMaxFallDistance() const {
        if (!GetTarget()) return GetComfortableFallDistance(0.0f);

        // MC Mob.getMaxFallDistance: sacrifice everything above a third of max
        // health, minus the difficulty allowance (Peaceful=0 .. Hard=3, so
        // easier settings make mobs more cautious).
        int sacrifice = static_cast<int>(GetHealth() - GetMaxHealth() * 0.33f);
        const int difficultyId =
            m_level ? static_cast<int>(m_level->GetDifficulty()) : 2;
        sacrifice -= (3 - difficultyId) * 4;
        if (sacrifice < 0) sacrifice = 0;
        return GetComfortableFallDistance(static_cast<float>(sacrifice));
    }

    bool Mob::CheckMobSpawnRules(EntityLevel& level, SpawnReason reason,
                                 const glm::ivec3& pos) {
        if (IsSpawner(reason)) return true;
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;
        return IsValidSpawnBlock(*blocks, pos.x, pos.y - 1, pos.z);
    }

    bool Mob::CheckSpawnObstruction(EntityLevel& level) const {
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;

        // MC LevelReader.containsAnyLiquid(getBoundingBox()) — any fluid block
        // the box overlaps rejects the spawn. This is what keeps ON_GROUND
        // mobs out of water even when the feet/head columns were dry.
        const AABB box = GetAABB();
        const int minX = static_cast<int>(std::floor(box.min.x));
        const int maxX = static_cast<int>(std::ceil(box.max.x));
        const int minY = static_cast<int>(std::floor(box.min.y));
        const int maxY = static_cast<int>(std::ceil(box.max.y));
        const int minZ = static_cast<int>(std::floor(box.min.z));
        const int maxZ = static_cast<int>(std::ceil(box.max.z));
        for (int x = minX; x < maxX; ++x) {
            for (int y = minY; y < maxY; ++y) {
                for (int z = minZ; z < maxZ; ++z) {
                    if (blocks->IsBlockFluid(x, y, z)) return false;
                }
            }
        }

        // MC EntityGetter.isUnobstructed(this) — no other entity already
        // occupying the box. (MC filters on blocksBuilding, which is true for
        // every living entity — the ones this query returns.)
        std::vector<Entity*> occupants;
        level.GetEntitiesInBox(box, this, occupants);
        for (const Entity* other : occupants) {
            if (!other->IsRemoved() && other->GetAABB().Intersects(box)) return false;
        }
        return true;
    }

    void Mob::CheckDespawn() {
        if (!m_level || m_level->IsClientSide()) return;

        if (m_level->GetDifficulty() == Difficulty::Peaceful && TypeInfo().notInPeaceful) {
            Discard();
            return;
        }

        if (IsPersistenceRequired() || RequiresCustomPersistence()) {
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
