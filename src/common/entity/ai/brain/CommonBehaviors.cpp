// File: src/common/entity/ai/brain/CommonBehaviors.cpp
#include "common/entity/ai/brain/CommonBehaviors.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // MC BehaviorUtils.lookAtEntity / setWalkAndLookTargetMemories, which
        // every behaviour that wants a mob to approach something goes through.
        void SetWalkAndLookTarget(Brain& brain, Entity& target, float speed, int closeEnough) {
            brain.SetMemory(MemoryModule::LookTarget,
                            PositionTracker::OfEntity(&target, true));
            brain.SetMemory(MemoryModule::WalkTarget,
                            WalkTarget(PositionTracker::OfEntity(&target, false),
                                       speed, closeEnough));
        }
    }

    // ── CountDownCooldownTicks ─────────────────────────────────────────────

    CountDownCooldownTicks::CountDownCooldownTicks(MemoryModule cooldown)
        : Behavior({ MemoryCondition{ cooldown, MemoryStatus::ValuePresent } }),
          m_cooldown(cooldown) {}

    bool CountDownCooldownTicks::CanStillUse(EntityLevel&, LivingEntity& body, int64_t) {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        const std::optional<int> ticks = brain->GetInt(m_cooldown);
        return ticks.has_value() && *ticks > 0;
    }

    void CountDownCooldownTicks::Tick(EntityLevel&, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        if (const std::optional<int> ticks = brain->GetInt(m_cooldown)) {
            brain->SetMemory(m_cooldown, *ticks - 1);
        }
    }

    void CountDownCooldownTicks::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) brain->EraseMemory(m_cooldown);
    }

    // ── AnimalPanic ────────────────────────────────────────────────────────

    AnimalPanic::AnimalPanic(float speedMultiplier)
        : Behavior({ MemoryCondition{ MemoryModule::IsPanicking, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::HurtBy, MemoryStatus::Registered } },
                   100, 120),
          m_speedMultiplier(speedMultiplier) {}

    bool AnimalPanic::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        // MC tests the damage type against PANIC_CAUSES. Every damage source
        // this engine produces is in that tag, so "was hurt at all" is the same
        // predicate — the same reasoning PanicGoal already documents.
        return brain->HasMemoryValue(MemoryModule::HurtBy)
            || brain->HasMemoryValue(MemoryModule::IsPanicking);
    }

    void AnimalPanic::Start(EntityLevel&, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        auto* mob = dynamic_cast<Mob*>(&body);
        if (!brain || !mob) return;
        brain->SetMemory(MemoryModule::IsPanicking, true);
        brain->EraseMemory(MemoryModule::WalkTarget);
        mob->GetNavigation().Stop();
    }

    void AnimalPanic::Tick(EntityLevel&, LivingEntity& body, int64_t) {
        auto* mob = dynamic_cast<PathfinderMob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return;
        // MC only re-picks when the navigation has run out, so a panicking mob
        // commits to a direction instead of jittering.
        if (!mob->GetNavigation().IsDone()) return;
        if (auto pos = RandomPos::GetLandPos(*mob, 5, 4)) {
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfBlock(glm::ivec3(
                                            static_cast<int>(std::floor(pos->x)),
                                            static_cast<int>(std::floor(pos->y)),
                                            static_cast<int>(std::floor(pos->z)))),
                                        m_speedMultiplier, 0));
        }
    }

    void AnimalPanic::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) brain->EraseMemory(MemoryModule::IsPanicking);
    }

    // ── RandomStroll ───────────────────────────────────────────────────────

    RandomStroll::RandomStroll(float speedModifier, Kind kind, bool mayStrollFromWater)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
                   1),
          m_speedModifier(speedModifier), m_kind(kind),
          m_mayStrollFromWater(mayStrollFromWater) {}

    BehaviorPtr RandomStroll::Stroll(float speedModifier, bool mayStrollFromWater) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Land, mayStrollFromWater);
    }
    BehaviorPtr RandomStroll::Swim(float speedModifier) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Swim, true);
    }

    bool RandomStroll::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<PathfinderMob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        // MC's canRun predicate: swim only in water, and `stroll(s, false)`
        // refuses to pick a land target while swimming.
        if (m_kind == Kind::Swim && !mob->IsInWater()) return false;
        if (m_kind == Kind::Land && !m_mayStrollFromWater && mob->IsInWater()) return false;

        // MC's swim variant walks a tier list of distances looking for a
        // swimmable position; without fluid-aware random positions this port
        // uses the same land search, which finds a reachable spot either way.
        const auto pos = RandomPos::GetLandPos(*mob, 10, 7);
        if (!pos) return true;   // MC's setOrErase with an empty optional

        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(PositionTracker::OfBlock(glm::ivec3(
                                        static_cast<int>(std::floor(pos->x)),
                                        static_cast<int>(std::floor(pos->y)),
                                        static_cast<int>(std::floor(pos->z)))),
                                    m_speedModifier, 0));
        return true;
    }

    // ── SetWalkTargetFromLookTarget ────────────────────────────────────────

    SetWalkTargetFromLookTarget::SetWalkTargetFromLookTarget(float speedModifier,
                                                             int closeEnoughDistance)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::ValuePresent } },
                   1),
          m_speedModifier(speedModifier), m_closeEnoughDistance(closeEnoughDistance) {}

    bool SetWalkTargetFromLookTarget::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        const PositionTracker* look = brain->GetPositionTracker(MemoryModule::LookTarget);
        if (!look) return false;
        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(*look, m_speedModifier, m_closeEnoughDistance));
        return true;
    }

    // ── SetEntityLookTargetSometimes ───────────────────────────────────────

    SetEntityLookTargetSometimes::SetEntityLookTargetSometimes(float maxDist,
                                                               int intervalMin, int intervalMax)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::ValuePresent } },
                   1),
          m_maxDistSqr(maxDist * maxDist),
          m_intervalMin(intervalMin), m_intervalMax(intervalMax) {}

    bool SetEntityLookTargetSometimes::CheckExtraStartConditions(EntityLevel& level,
                                                                 LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;

        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) return false;

        LivingEntity* target = visible->FindClosest([&](LivingEntity* e) {
            return e->IsPlayer()
                && body.DistanceToSqr(*e) <= static_cast<double>(m_maxDistSqr);
        });
        if (!target) return false;

        // MC's Ticker: fire at zero, then re-sample. The countdown runs only
        // while a target is actually in range, which is why a mob alone in a
        // field does not "use up" its glances.
        if (m_ticksUntilNextStart == 0) {
            m_ticksUntilNextStart =
                level.Random().NextInt(m_intervalMin, m_intervalMax) - 1;
            return false;
        }
        if (--m_ticksUntilNextStart != 0) return false;

        brain->SetMemory(MemoryModule::LookTarget,
                         PositionTracker::OfEntity(target, true));
        return true;
    }

    // ── StartAttacking ─────────────────────────────────────────────────────

    StartAttacking::StartAttacking(CanAttack canAttack, TargetFinder finder)
        : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                      MemoryStatus::Registered } },
                   1),
          m_canAttack(std::move(canAttack)), m_finder(std::move(finder)) {}

    bool StartAttacking::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;
        if (m_canAttack && !m_canAttack(*mob)) return false;

        LivingEntity* target = m_finder ? m_finder(*mob) : nullptr;
        if (!target || !mob->CanAttack(*target)) return false;

        brain->SetMemory(MemoryModule::AttackTarget, static_cast<Entity*>(target));
        brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
        return true;
    }

    // ── StopAttackingIfTargetInvalid ───────────────────────────────────────

    StopAttackingIfTargetInvalid::StopAttackingIfTargetInvalid()
        : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                      MemoryStatus::Registered } },
                   1) {}

    bool StopAttackingIfTargetInvalid::CheckExtraStartConditions(EntityLevel& level,
                                                                 LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        Entity* target = brain->GetEntity(MemoryModule::AttackTarget);
        auto* living = dynamic_cast<LivingEntity*>(target);

        // MC TIMEOUT_TO_GET_WITHIN_ATTACK_RANGE: 200 ticks of failing to reach
        // the target and the mob gives up. Without it a frog stares at a slime
        // across a ravine forever.
        bool tired = false;
        if (const std::optional<int64_t> since =
                brain->GetLong(MemoryModule::CantReachWalkTargetSince)) {
            tired = (level.GetGameTime() - *since) > 200;
        }

        if (!living || !living->IsAlive() || !mob->CanAttack(*living) || tired) {
            brain->EraseMemory(MemoryModule::AttackTarget);
        }
        return true;
    }

    // ── FollowTemptation ───────────────────────────────────────────────────

    FollowTemptation::FollowTemptation(float speedModifier, double closeEnoughDistance)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::TemptationCooldownTicks,
                                      MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::IsTempted, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::TemptingPlayer,
                                      MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::BreedTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::IsPanicking, MemoryStatus::ValueAbsent } }),
          m_speedModifier(speedModifier), m_closeEnoughDistance(closeEnoughDistance) {}

    bool FollowTemptation::CanStillUse(EntityLevel&, LivingEntity& body, int64_t) {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        return brain->HasMemoryValue(MemoryModule::TemptingPlayer)
            && !brain->HasMemoryValue(MemoryModule::BreedTarget)
            && !brain->HasMemoryValue(MemoryModule::IsPanicking);
    }

    void FollowTemptation::Start(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) {
            brain->SetMemory(MemoryModule::IsTempted, true);
        }
    }

    void FollowTemptation::Tick(EntityLevel&, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        Entity* player = brain->GetEntity(MemoryModule::TemptingPlayer);
        if (!player) return;

        brain->SetMemory(MemoryModule::LookTarget,
                         PositionTracker::OfEntity(player, true));
        if (body.DistanceToSqr(*player) < m_closeEnoughDistance * m_closeEnoughDistance) {
            // Close enough — stop walking but keep looking, which is what makes
            // a tempted animal cluster at your feet rather than shove past you.
            brain->EraseMemory(MemoryModule::WalkTarget);
        } else {
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfEntity(player, false),
                                        m_speedModifier, 2));
        }
    }

    void FollowTemptation::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        brain->SetMemory(MemoryModule::TemptationCooldownTicks, kTemptationCooldown);
        brain->EraseMemory(MemoryModule::IsTempted);
        brain->EraseMemory(MemoryModule::WalkTarget);
        brain->EraseMemory(MemoryModule::LookTarget);
    }

    // ── AnimalMakeLove ─────────────────────────────────────────────────────

    AnimalMakeLove::AnimalMakeLove(EntityTypeId partnerType, float speedModifier,
                                   int closeEnoughDistance)
        : Behavior({ MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::BreedTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::IsPanicking, MemoryStatus::ValueAbsent } },
                   110),
          m_partnerType(partnerType), m_speedModifier(speedModifier),
          m_closeEnoughDistance(closeEnoughDistance) {}

    Animal* AnimalMakeLove::FindValidBreedPartner(Animal& body) const {
        const Brain* brain = body.GetBrain();
        if (!brain) return nullptr;
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) return nullptr;

        LivingEntity* found = visible->FindClosest([&](LivingEntity* e) {
            if (e->GetType() != m_partnerType) return false;
            auto* animal = dynamic_cast<Animal*>(e);
            return animal && body.CanMate(*animal);
        });
        return dynamic_cast<Animal*>(found);
    }

    bool AnimalMakeLove::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* animal = dynamic_cast<Animal*>(&body);
        if (!animal || !animal->IsInLove()) return false;
        return FindValidBreedPartner(*animal) != nullptr;
    }

    void AnimalMakeLove::Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        auto* animal = dynamic_cast<Animal*>(&body);
        if (!animal) return;
        Animal* partner = FindValidBreedPartner(*animal);
        if (!partner) return;

        Brain* mine = animal->GetBrain();
        Brain* theirs = partner->GetBrain();
        if (mine) mine->SetMemory(MemoryModule::BreedTarget, static_cast<Entity*>(partner));
        if (theirs) theirs->SetMemory(MemoryModule::BreedTarget, static_cast<Entity*>(animal));

        // MC lockGazeAndWalkToEachOther — BOTH sides get the memories, which is
        // why a breeding pair converges instead of one chasing the other.
        if (mine)   SetWalkAndLookTarget(*mine, *partner, m_speedModifier, m_closeEnoughDistance);
        if (theirs) SetWalkAndLookTarget(*theirs, *animal, m_speedModifier, m_closeEnoughDistance);

        m_spawnChildAtTime = timestamp + 60 + level.Random().NextInt(50);
    }

    bool AnimalMakeLove::CanStillUse(EntityLevel&, LivingEntity& body, int64_t timestamp) {
        auto* animal = dynamic_cast<Animal*>(&body);
        Brain* brain = body.GetBrain();
        if (!animal || !brain) return false;
        auto* partner = dynamic_cast<Animal*>(brain->GetEntity(MemoryModule::BreedTarget));
        if (!partner || partner->GetType() != m_partnerType) return false;
        return partner->IsAlive() && animal->CanMate(*partner)
            && timestamp <= m_spawnChildAtTime;
    }

    void AnimalMakeLove::Tick(EntityLevel&, LivingEntity& body, int64_t timestamp) {
        auto* animal = dynamic_cast<Animal*>(&body);
        Brain* brain = body.GetBrain();
        if (!animal || !brain) return;
        auto* partner = dynamic_cast<Animal*>(brain->GetEntity(MemoryModule::BreedTarget));
        if (!partner) return;

        SetWalkAndLookTarget(*brain, *partner, m_speedModifier, m_closeEnoughDistance);
        if (Brain* theirs = partner->GetBrain()) {
            SetWalkAndLookTarget(*theirs, *animal, m_speedModifier, m_closeEnoughDistance);
        }

        if (animal->DistanceToSqr(*partner) < 3.0 * 3.0 && timestamp >= m_spawnChildAtTime) {
            animal->SpawnChildFromBreeding(*partner);
            brain->EraseMemory(MemoryModule::BreedTarget);
            if (Brain* theirs = partner->GetBrain()) {
                theirs->EraseMemory(MemoryModule::BreedTarget);
            }
        }
    }

    void AnimalMakeLove::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) {
            brain->EraseMemory(MemoryModule::BreedTarget);
            brain->EraseMemory(MemoryModule::WalkTarget);
            brain->EraseMemory(MemoryModule::LookTarget);
        }
        m_spawnChildAtTime = 0;
    }

    void AnimalMakeLove::ClearReferenceTo(const Entity*) {
        // The partner is held in a MEMORY, and Brain::ClearReferenceTo scrubs
        // those. Nothing is cached on the behaviour itself.
    }

    // ── TryFindLand ────────────────────────────────────────────────────────

    TryFindLand::TryFindLand(int range, float speedModifier)
        : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered } },
                   1),
          m_range(range), m_speedModifier(speedModifier) {}

    bool TryFindLand::CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        const IBlockAccess* blocks = level.Blocks();
        if (!brain || !blocks) return false;

        const glm::ivec3 origin = body.BlockPosition();
        if (!blocks->IsBlockFluid(origin.x, origin.y, origin.z)) return false;

        const int64_t now = level.GetGameTime();
        if (now < m_nextOkStartTime) {
            m_nextOkStartTime = now + 60;
            return true;
        }

        // MC scans a Manhattan ball and takes the FIRST dry, non-colliding block
        // with a sturdy face beneath it — not the nearest by euclidean distance.
        for (int dx = -m_range; dx <= m_range; ++dx) {
            for (int dy = -m_range; dy <= m_range; ++dy) {
                for (int dz = -m_range; dz <= m_range; ++dz) {
                    if (std::abs(dx) + std::abs(dy) + std::abs(dz) > m_range) continue;
                    if (dx == 0 && dz == 0) continue;
                    const glm::ivec3 p = origin + glm::ivec3(dx, dy, dz);
                    if (blocks->IsBlockFluid(p.x, p.y, p.z)) continue;
                    if (blocks->IsBlockSolid(p.x, p.y, p.z)) continue;
                    if (!blocks->IsBlockSolid(p.x, p.y - 1, p.z)) continue;

                    brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(p));
                    brain->SetMemory(MemoryModule::WalkTarget,
                                     WalkTarget(PositionTracker::OfBlock(p),
                                                m_speedModifier, 1));
                    m_nextOkStartTime = now + 60;
                    return true;
                }
            }
        }
        m_nextOkStartTime = now + 60;
        return true;
    }

    // ── Swim / DoNothing / RandomLookAround ────────────────────────────────

    bool Swim::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        // MC also tests the fluid HEIGHT against the mob's jump threshold; this
        // engine has no fluid height, so "in water" is the whole condition —
        // the same simplification FloatGoal already documents.
        return body.IsInWater() || body.IsInLava();
    }

    void Swim::Tick(EntityLevel& level, LivingEntity& body, int64_t) {
        auto* mob = dynamic_cast<Mob*>(&body);
        if (!mob) return;
        if (level.Random().NextFloat() < m_chance) mob->GetJumpControl().Jump();
    }

    RandomLookAround::RandomLookAround(int intervalMin, int intervalMax,
                                       float maxYaw, float minPitch, float maxPitch)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::GazeCooldownTicks,
                                      MemoryStatus::ValueAbsent } }),
          m_intervalMin(intervalMin), m_intervalMax(intervalMax),
          m_maxYaw(maxYaw), m_minPitch(minPitch), m_pitchRange(maxPitch - minPitch) {}

    void RandomLookAround::Start(EntityLevel& level, LivingEntity& body, int64_t) {
        Brain* brain = body.GetBrain();
        if (!brain) return;

        const float pitch = std::clamp(level.Random().NextFloat() * m_pitchRange + m_minPitch,
                                       -90.0f, 90.0f);
        const float yaw = Mth::WrapDegrees(
            body.yRot + 2.0f * level.Random().NextFloat() * m_maxYaw - m_maxYaw);

        // MC Vec3.directionFromRotation, then offset from the EYE — the look
        // control aims at a point, not a direction.
        const float p = pitch * Mth::kDegToRad;
        const float y = -yaw * Mth::kDegToRad;
        const glm::dvec3 dir(std::cos(p) * std::sin(y) * -1.0,
                             -std::sin(p),
                             std::cos(p) * std::cos(y));
        const glm::dvec3 look = body.GetEyePosition() + dir;

        brain->SetMemory(MemoryModule::LookTarget,
                         PositionTracker::OfBlock(glm::ivec3(
                             static_cast<int>(std::floor(look.x)),
                             static_cast<int>(std::floor(look.y)),
                             static_cast<int>(std::floor(look.z)))));
        brain->SetMemory(MemoryModule::GazeCooldownTicks,
                         level.Random().NextInt(m_intervalMin, m_intervalMax));
    }

    // ── MeleeAttack ────────────────────────────────────────────────────────

    MeleeAttack::MeleeAttack(int cooldownBetweenAttacks)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::AttackCoolingDown,
                                      MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::ValuePresent } },
                   1),
          m_cooldown(cooldownBetweenAttacks) {}

    bool MeleeAttack::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget));
        if (!target) return false;
        if (!mob->IsWithinMeleeAttackRange(*target)) return false;

        // MC requires the target to be in the VISIBLE set, not merely named by
        // ATTACK_TARGET — a mob does not swing at something behind a wall.
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible || !visible->Contains(target)) return false;

        brain->SetMemory(MemoryModule::LookTarget,
                         PositionTracker::OfEntity(target, true));
        mob->Swing();
        mob->DoHurtTarget(*target);
        brain->SetMemoryWithExpiry(MemoryModule::AttackCoolingDown, true, m_cooldown);
        return true;
    }

    // ── SetWalkTargetFromAttackTarget ──────────────────────────────────────

    SetWalkTargetFromAttackTarget::SetWalkTargetFromAttackTarget(float speedModifier)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::Registered } },
                   1),
          m_speedModifier(speedModifier) {}

    bool SetWalkTargetFromAttackTarget::CheckExtraStartConditions(EntityLevel&,
                                                                  LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;
        auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget));
        if (!target) return false;

        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (visible && visible->Contains(target) && mob->IsWithinMeleeAttackRange(*target)) {
            // Already in reach — stop walking so the mob stands and swings
            // instead of shoving its target around.
            brain->EraseMemory(MemoryModule::WalkTarget);
        } else {
            brain->SetMemory(MemoryModule::LookTarget,
                             PositionTracker::OfEntity(target, true));
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfEntity(target, false),
                                        m_speedModifier, 0));
        }
        return true;
    }

    // ── EraseMemoryIf ──────────────────────────────────────────────────────

    EraseMemoryIf::EraseMemoryIf(Pred pred, MemoryModule memory)
        : Behavior({ MemoryCondition{ memory, MemoryStatus::ValuePresent } }, 1),
          m_pred(std::move(pred)), m_memory(memory) {}

    bool EraseMemoryIf::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        if (!m_pred || !m_pred(body)) return false;
        if (Brain* brain = body.GetBrain()) brain->EraseMemory(m_memory);
        return true;
    }

    // ── SetEntityLookTarget ────────────────────────────────────────────────

    SetEntityLookTarget::SetEntityLookTarget(Pred pred, float maxDist)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::ValuePresent } },
                   1),
          m_pred(std::move(pred)), m_maxDistSqr(maxDist * maxDist) {}

    BehaviorPtr SetEntityLookTarget::OfType(EntityTypeId type, float maxDist) {
        return std::make_unique<SetEntityLookTarget>(
            [type](LivingEntity& e) { return e.GetType() == type; }, maxDist);
    }
    BehaviorPtr SetEntityLookTarget::Any(float maxDist) {
        return std::make_unique<SetEntityLookTarget>(
            [](LivingEntity&) { return true; }, maxDist);
    }

    bool SetEntityLookTarget::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) return false;

        LivingEntity* target = visible->FindClosest([&](LivingEntity* e) {
            return m_pred(*e)
                && body.DistanceToSqr(*e) <= static_cast<double>(m_maxDistSqr);
        });
        if (!target) return false;
        brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(target, true));
        return true;
    }

    // ── BabyFollowAdult ────────────────────────────────────────────────────

    BabyFollowAdult::BabyFollowAdult(int followRangeMin, int followRangeMax,
                                     float speedModifier)
        : Behavior({ MemoryCondition{ MemoryModule::NearestVisibleAdult,
                                      MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
                   1),
          m_min(followRangeMin), m_max(followRangeMax), m_speedModifier(speedModifier) {}

    bool BabyFollowAdult::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        if (!body.IsBaby()) return false;
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        Entity* adult = brain->GetEntity(MemoryModule::NearestVisibleAdult);
        if (!adult) return false;

        // Inside the min: already close enough, stop. Beyond the max + 1: too
        // far to bother. Only the band between makes a baby trot after its
        // parent, which is what stops it either shoving or teleport-chasing.
        const double d2 = body.DistanceToSqr(*adult);
        const double maxD = static_cast<double>(m_max + 1);
        const double minD = static_cast<double>(m_min);
        if (d2 >= maxD * maxD || d2 < minD * minD) return false;

        brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(adult, true));
        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(PositionTracker::OfEntity(adult, false),
                                    m_speedModifier, m_min - 1));
        return true;
    }

    // ── SetWalkTargetAwayFrom ──────────────────────────────────────────────

    SetWalkTargetAwayFrom::SetWalkTargetAwayFrom(MemoryModule avoidMemory,
                                                 float speedModifier, int desiredDistance,
                                                 bool interruptCurrentWalk)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::Registered },
                     MemoryCondition{ avoidMemory, MemoryStatus::ValuePresent } },
                   1),
          m_avoid(avoidMemory), m_speedModifier(speedModifier),
          m_desiredDistance(desiredDistance), m_interruptCurrentWalk(interruptCurrentWalk) {}

    bool SetWalkTargetAwayFrom::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<PathfinderMob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        const WalkTarget* current = brain->GetWalkTarget(MemoryModule::WalkTarget);
        if (current && !m_interruptCurrentWalk) return false;

        Entity* avoid = brain->GetEntity(m_avoid);
        if (!avoid) return false;
        const glm::dvec3 avoidPos = avoid->position;
        if (body.DistanceToSqr(*avoid)
            >= static_cast<double>(m_desiredDistance) * m_desiredDistance) {
            return false;
        }

        // MC keeps an existing flee target when it already points away — this is
        // what stops a fleeing mob dithering between two escape routes.
        if (current && current->speedModifier == m_speedModifier) {
            const glm::dvec3 currentDir = current->target.CurrentPosition() - body.position;
            const glm::dvec3 avoidDir = avoidPos - body.position;
            if (glm::dot(currentDir, avoidDir) < 0.0) return false;
        }

        for (int i = 0; i < 10; ++i) {
            if (auto flee = RandomPos::GetPosAway(*mob, 16, 7, avoidPos)) {
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(PositionTracker::OfBlock(glm::ivec3(
                                                static_cast<int>(std::floor(flee->x)),
                                                static_cast<int>(std::floor(flee->y)),
                                                static_cast<int>(std::floor(flee->z)))),
                                            m_speedModifier, 0));
                break;
            }
        }
        return true;
    }

    // ── LongJumpMidJump ────────────────────────────────────────────────────

    LongJumpMidJump::LongJumpMidJump(int cooldownMin, int cooldownMax)
        : Behavior({ MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::LongJumpMidJump,
                                      MemoryStatus::ValuePresent } },
                   100),
          m_cooldownMin(cooldownMin), m_cooldownMax(cooldownMax) {}

    bool LongJumpMidJump::CanStillUse(EntityLevel&, LivingEntity& body, int64_t) {
        return !body.onGround;
    }

    void LongJumpMidJump::Start(EntityLevel&, LivingEntity& body, int64_t) {
        body.SetDiscardFriction(true);
        body.SetPose(Pose::LongJumping);
    }

    void LongJumpMidJump::Stop(EntityLevel& level, LivingEntity& body, int64_t) {
        if (body.onGround) {
            body.velocity.x *= 0.1;
            body.velocity.z *= 0.1;
        }
        body.SetDiscardFriction(false);
        body.SetPose(Pose::Standing);
        if (Brain* brain = body.GetBrain()) {
            brain->EraseMemory(MemoryModule::LongJumpMidJump);
            brain->SetMemory(MemoryModule::LongJumpCooldownTicks,
                             level.Random().NextInt(m_cooldownMin, m_cooldownMax));
        }
    }

    // ── Sensors ────────────────────────────────────────────────────────────

    void AdultSensor::DoTick(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) { brain->EraseMemory(MemoryModule::NearestVisibleAdult); return; }

        LivingEntity* adult = visible->FindClosest([&](LivingEntity* e) {
            return e->GetType() == body.GetType() && !e->IsBaby();
        });
        if (adult) brain->SetMemory(MemoryModule::NearestVisibleAdult,
                                    static_cast<Entity*>(adult));
        else       brain->EraseMemory(MemoryModule::NearestVisibleAdult);
    }



    void IsInWaterSensor::DoTick(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        // A Unit memory: its PRESENCE is the value, which is what lets an
        // activity require IS_IN_WATER absent.
        if (body.IsInWater()) brain->SetMemory(MemoryModule::IsInWater, std::monostate{});
        else                  brain->EraseMemory(MemoryModule::IsInWater);
    }

    void HurtBySensor::DoTick(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        if (body.HasLastDamageSource() && body.hurtTime > 0) {
            // MC stores the DamageSource; this port does not model one, so the
            // memory is a Unit and the attacker rides in HURT_BY_ENTITY.
            brain->SetMemoryWithExpiry(MemoryModule::HurtBy, std::monostate{}, 100);
            if (Entity* by = body.GetLastHurtByMob()) {
                brain->SetMemoryWithExpiry(MemoryModule::HurtByEntity, by, 100);
            }
        }
    }

    void TemptingSensor::DoTick(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;

        std::vector<LivingEntity*> players;
        level.GetPlayers(players);

        LivingEntity* best = nullptr;
        double bestDistSq = kTemptationRange * kTemptationRange;
        for (LivingEntity* p : players) {
            if (!p->IsAlive()) continue;
            if (!m_pred || !m_pred(level.GetHeldItemId(*p))) continue;
            const double d = body.DistanceToSqr(*p);
            if (d < bestDistSq) { bestDistSq = d; best = p; }
        }

        if (best) brain->SetMemory(MemoryModule::TemptingPlayer, static_cast<Entity*>(best));
        else      brain->EraseMemory(MemoryModule::TemptingPlayer);
    }

    void NearestAttackableSensor::DoTick(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) { brain->EraseMemory(MemoryModule::NearestAttackable); return; }

        LivingEntity* found = visible->FindClosest([&](LivingEntity* e) {
            return m_pred && m_pred(*e);
        });
        if (found) brain->SetMemory(MemoryModule::NearestAttackable,
                                    static_cast<Entity*>(found));
        else       brain->EraseMemory(MemoryModule::NearestAttackable);
    }

} // namespace Game
