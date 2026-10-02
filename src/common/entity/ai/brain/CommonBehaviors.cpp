// File: src/common/entity/ai/brain/CommonBehaviors.cpp
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/MobCrossbow.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/pathfinder/Path.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/block/BlockPlacement.hpp"

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
        // MC: HURT_BY's damage source is(DamageTypeTags.PANIC_CAUSES), or
        // IS_PANICKING. HURT_BY mirrors the mob's last damage source
        // (HurtBySensor), so that source's type is tested against the tag —
        // a fall, a drowning, cramming or the void does not start a panic.
        if (brain->HasMemoryValue(MemoryModule::IsPanicking)) return true;
        if (!brain->HasMemoryValue(MemoryModule::HurtBy)) return false;
        const std::string_view type =
            DamageSourceInfo::TypeIdFor(body.GetLastDamageSource(), nullptr, nullptr);
        return DataTags::HasTag(DataTags::Registry::DamageType, type, "minecraft:panic_causes");
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
        // MC AnimalPanic.getPanicPos: a burning mob runs for the closest
        // water first (lookForWater), else a LandRandomPos 5 x 4 hop.
        if (mob->IsOnFire()) {
            if (auto water = LookForWater(*mob)) {
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(PositionTracker::OfBlock(*water), m_speedMultiplier, 0));
                return;
            }
        }
        if (auto pos = RandomPos::GetLandPos(*mob, 5, 4)) {
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfBlock(glm::ivec3(
                                            static_cast<int>(std::floor(pos->x)),
                                            static_cast<int>(std::floor(pos->y)),
                                            static_cast<int>(std::floor(pos->z)))),
                                        m_speedMultiplier, 0));
        }
    }

    std::optional<glm::ivec3> AnimalPanic::LookForWater(PathfinderMob& mob) {
        // MC AnimalPanic.lookForWater: nothing from inside a collision shape;
        // else BlockPos.findClosestMatch(pos, 5, 1, water) — for a mob two
        // blocks wide (Mth.ceil(width) == 2) the 2x2 square out to the south
        // east must all be water.
        EntityLevel* level = mob.Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return std::nullopt;
        const glm::ivec3 origin = mob.BlockPosition();
        if (BlockRegistry::HasCollision(blocks->GetBlock(origin.x, origin.y, origin.z))) return std::nullopt;
        const bool wide = static_cast<int>(std::ceil(mob.GetBbWidth())) == 2;
        const auto isWater = [&](int x, int y, int z) {
            if (!wide) return blocks->ContainsWater(x, y, z);
            return blocks->ContainsWater(x, y, z) && blocks->ContainsWater(x + 1, y, z) &&
                   blocks->ContainsWater(x, y, z + 1) && blocks->ContainsWater(x + 1, y, z + 1);
        };
        // findClosestMatch walks shells of growing Manhattan distance;
        // minimising it over the box picks the same cell (ties in raster
        // order, as arbitrary as MC's in-shell order).
        std::optional<glm::ivec3> best;
        int bestManhattan = 0;
        for (int dx = -5; dx <= 5; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -5; dz <= 5; ++dz) {
                    if (!isWater(origin.x + dx, origin.y + dy, origin.z + dz)) continue;
                    const int manhattan = std::abs(dx) + std::abs(dy) + std::abs(dz);
                    if (!best || manhattan < bestManhattan) {
                        bestManhattan = manhattan;
                        best = origin + glm::ivec3(dx, dy, dz);
                    }
                }
            }
        }
        return best;
    }

    void AnimalPanic::Stop(EntityLevel&, LivingEntity& body, int64_t) {
        if (Brain* brain = body.GetBrain()) brain->EraseMemory(MemoryModule::IsPanicking);
    }

    // ── RandomStroll ───────────────────────────────────────────────────────

    RandomStroll::RandomStroll(float speedModifier, Kind kind, bool mayStrollFromWater,
                               int maxHorizontalDistance, int maxVerticalDistance)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
                   1),
          m_speedModifier(speedModifier), m_kind(kind),
          m_mayStrollFromWater(mayStrollFromWater),
          m_maxHorizontalDistance(maxHorizontalDistance),
          m_maxVerticalDistance(maxVerticalDistance) {}

    BehaviorPtr RandomStroll::Stroll(float speedModifier, bool mayStrollFromWater) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Land, mayStrollFromWater);
    }
    BehaviorPtr RandomStroll::Stroll(float speedModifier, int maxHorizontalDistance,
                                     int maxVerticalDistance) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Land, true,
                                              maxHorizontalDistance, maxVerticalDistance);
    }
    BehaviorPtr RandomStroll::Swim(float speedModifier) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Swim, true);
    }
    BehaviorPtr RandomStroll::Fly(float speedModifier) {
        return std::make_unique<RandomStroll>(speedModifier, Kind::Fly, true);
    }

    bool RandomStroll::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<PathfinderMob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        // MC's canRun predicate: swim only in water, and `stroll(s, false)`
        // refuses to pick a land target while swimming.
        if (m_kind == Kind::Swim && !mob->IsInWater()) return false;
        if (m_kind == Kind::Land && !m_mayStrollFromWater && mob->IsInWater()) return false;

        // MC fly(): AirAndWaterRandomPos ahead of the view vector, the
        // allay's exact wander shape.
        if (m_kind == Kind::Fly) {
            const glm::vec3 view = Mth::ViewVector(0.0f, mob->yRot);
            const auto flyPos = RandomPos::GetAirAndWaterPos(
                *mob, m_maxHorizontalDistance, m_maxVerticalDistance, -2,
                view.x, view.z, 3.14159265358979 / 2.0);
            if (!flyPos) return true;
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfBlock(glm::ivec3(
                                            static_cast<int>(std::floor(flyPos->x)),
                                            static_cast<int>(std::floor(flyPos->y)),
                                            static_cast<int>(std::floor(flyPos->z)))),
                                        m_speedModifier, 0));
            return true;
        }

        // MC's swim variant walks a tier list of distances looking for a
        // swimmable position; without fluid-aware random positions this port
        // uses the same land search, which finds a reachable spot either way.
        const auto pos = RandomPos::GetLandPos(*mob, m_maxHorizontalDistance,
                                               m_maxVerticalDistance);
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

    SetWalkTargetFromLookTarget::SetWalkTargetFromLookTarget(Pred canSet, SpeedFn speed,
                                                             int closeEnoughDistance)
        : Behavior({ MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::ValuePresent } },
                   1),
          m_speedModifier(1.0f), m_closeEnoughDistance(closeEnoughDistance),
          m_canSet(std::move(canSet)), m_speedFn(std::move(speed)) {}

    bool SetWalkTargetFromLookTarget::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        if (m_canSet && !m_canSet(body)) return false;
        const PositionTracker* look = brain->GetPositionTracker(MemoryModule::LookTarget);
        if (!look) return false;
        const float speed = m_speedFn ? m_speedFn(body) : m_speedModifier;
        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(*look, speed, m_closeEnoughDistance));
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

    StopAttackingIfTargetInvalid::StopAttackingIfTargetInvalid(
            StopAttackCondition stopAttackingWhen, TargetErasedCallback onTargetErased,
            bool canGrowTiredOfTryingToReachTarget)
        : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::CantReachWalkTargetSince,
                                      MemoryStatus::Registered } },
                   1),
          m_stopAttackingWhen(std::move(stopAttackingWhen)),
          m_onTargetErased(std::move(onTargetErased)),
          m_canGrowTired(canGrowTiredOfTryingToReachTarget) {}

    bool StopAttackingIfTargetInvalid::CheckExtraStartConditions(EntityLevel& level,
                                                                 LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget));
        if (!target) {
            // A reference that no longer resolves (MC's memory would hold the
            // removed entity, which fails isAlive below). Nothing to hand the
            // callback, so just drop it.
            brain->EraseMemory(MemoryModule::AttackTarget);
            return true;
        }

        // MC TIMEOUT_TO_GET_WITHIN_ATTACK_RANGE: 200 ticks of failing to reach
        // the target and the mob gives up. Without it a frog stares at a slime
        // across a ravine forever.
        bool tired = false;
        if (m_canGrowTired) {
            if (const std::optional<int64_t> since =
                    brain->GetLong(MemoryModule::CantReachWalkTargetSince)) {
                tired = (level.GetGameTime() - *since) > 200;
            }
        }

        // MC, condition for condition and in order.
        const bool stillValid = mob->CanAttack(*target)
            && !tired
            && target->IsAlive()
            && target->Level() == body.Level()
            && !(m_stopAttackingWhen && m_stopAttackingWhen(level, *mob, *target));
        if (!stillValid) {
            if (m_onTargetErased) m_onTargetErased(level, *mob, *target);
            brain->EraseMemory(MemoryModule::AttackTarget);
        }
        return true;
    }

    // ── Sensor targeting tests ─────────────────────────────────────────────

    namespace SensorTargeting {

        namespace {
            TargetingConditions RangedFor(LivingEntity& body, TargetingConditions conditions) {
                return conditions.Range(body.GetAttributeValue(Attribute::FollowRange));
            }

            bool IsAttackTarget(const LivingEntity& body, const LivingEntity& target) {
                const Brain* brain = body.GetBrain();
                return brain && brain->IsMemoryValue(MemoryModule::AttackTarget, &target);
            }
        } // namespace

        bool IsEntityTargetable(LivingEntity& body, const LivingEntity& target) {
            TargetingConditions conditions = RangedFor(body, TargetingConditions::ForNonCombat());
            if (IsAttackTarget(body, target)) conditions.IgnoreInvisibility();
            return conditions.Test(&body, target);
        }

        bool IsEntityAttackable(LivingEntity& body, const LivingEntity& target) {
            TargetingConditions conditions = RangedFor(body, TargetingConditions::ForCombat());
            if (IsAttackTarget(body, target)) conditions.IgnoreInvisibility();
            return conditions.Test(&body, target);
        }

        bool IsEntityAttackableIgnoringLineOfSight(LivingEntity& body, const LivingEntity& target) {
            TargetingConditions conditions =
                RangedFor(body, TargetingConditions::ForCombat()).IgnoreLineOfSight();
            if (IsAttackTarget(body, target)) conditions.IgnoreInvisibility();
            return conditions.Test(&body, target);
        }

    } // namespace SensorTargeting

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

    FollowTemptation::FollowTemptation(SpeedFn speed, double closeEnoughDistance)
        : FollowTemptation(1.0f, closeEnoughDistance) {
        m_speedFn = std::move(speed);
    }

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
            const float speed = m_speedFn ? m_speedFn(body) : m_speedModifier;
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfEntity(player, false),
                                        speed, 2));
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

    // ── TryFindWater ───────────────────────────────────────────────────────

    TryFindWater::TryFindWater(int range, float speedModifier)
        : Behavior({ MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered } },
                   1),
          m_range(range), m_speedModifier(speedModifier) {}

    bool TryFindWater::CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        const IBlockAccess* blocks = level.Blocks();
        if (!brain || !blocks) return false;

        const glm::ivec3 origin = body.BlockPosition();
        // MC: already standing in water — nothing to find.
        if (blocks->ContainsWater(origin.x, origin.y, origin.z)) return false;

        const int64_t now = level.GetGameTime();
        if (now < m_nextOkStartTime) {
            m_nextOkStartTime = now + 20 + 2;
            return true;
        }

        // MC scans the Manhattan ball for water: FIRST water with air above
        // wins outright; failing that, the first water at least 1.5 blocks
        // from the mob's centre.
        std::optional<glm::ivec3> best;
        std::optional<glm::ivec3> alternate;
        for (int dx = -m_range; dx <= m_range && !best; ++dx) {
            for (int dy = -m_range; dy <= m_range && !best; ++dy) {
                for (int dz = -m_range; dz <= m_range; ++dz) {
                    if (std::abs(dx) + std::abs(dy) + std::abs(dz) > m_range) continue;
                    if (dx == 0 && dz == 0) continue;
                    const glm::ivec3 p = origin + glm::ivec3(dx, dy, dz);
                    if (!blocks->ContainsWater(p.x, p.y, p.z)) continue;
                    if (!blocks->IsBlockSolid(p.x, p.y + 1, p.z)
                        && !blocks->ContainsWater(p.x, p.y + 1, p.z)) {
                        best = p;
                        break;
                    }
                    if (!alternate) {
                        const glm::dvec3 centre(p.x + 0.5, p.y + 0.5, p.z + 0.5);
                        const glm::dvec3 d = centre - body.position;
                        if (glm::dot(d, d) >= 1.5 * 1.5) alternate = p;
                    }
                }
            }
        }
        if (!best) best = alternate;
        if (best) {
            brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfBlock(*best));
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfBlock(*best),
                                        m_speedModifier, 0));
        }
        m_nextOkStartTime = now + 40;
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
        // MC !isHoldingUsableNonMeleeWeapon(body): a mob holding a weapon it
        // fires (a piglin's crossbow) leaves the melee to nobody.
        if (mob->CanUseNonMeleeWeapon(mob->GetMainHandEquipment()) ||
            mob->CanUseNonMeleeWeapon(mob->GetOffhandEquipment())) {
            return false;
        }
        if (!mob->IsWithinMeleeAttackRange(*target)) return false;

        // MC requires the target to be in the VISIBLE set, not merely named by
        // ATTACK_TARGET — a mob does not swing at something behind a wall.
        // Visibility is now query-time (MC contains applies lineOfSightTest),
        // so membership alone is not enough.
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible || !visible->Contains(target) || !visible->IsVisible(target)) return false;

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

    SetWalkTargetFromAttackTarget::SetWalkTargetFromAttackTarget(SpeedFn speed)
        : SetWalkTargetFromAttackTarget(1.0f) {
        m_speedFn = std::move(speed);
    }

    bool SetWalkTargetFromAttackTarget::CheckExtraStartConditions(EntityLevel&,
                                                                  LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;
        auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AttackTarget));
        if (!target) return false;

        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        // MC contains applies the query-time visibility predicate too.
        // BehaviorUtils.isWithinAttackRange(body, target, 1): a fired weapon's
        // range less one block, else the melee reach.
        if (visible && visible->Contains(target) && visible->IsVisible(target) &&
            MobCrossbow::IsWithinAttackRange(*mob, *target, 1)) {
            // Already in reach — stop walking so the mob stands and swings
            // instead of shoving its target around.
            brain->EraseMemory(MemoryModule::WalkTarget);
        } else {
            const float speed = m_speedFn ? m_speedFn(body) : m_speedModifier;
            brain->SetMemory(MemoryModule::LookTarget,
                             PositionTracker::OfEntity(target, true));
            brain->SetMemory(MemoryModule::WalkTarget,
                             WalkTarget(PositionTracker::OfEntity(target, false),
                                        speed, 0));
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

    BabyFollowAdult::BabyFollowAdult(int followRangeMin, int followRangeMax, SpeedFn speed)
        : BabyFollowAdult(followRangeMin, followRangeMax, 1.0f) {
        m_speedFn = std::move(speed);
    }

    BabyFollowAdult::BabyFollowAdult(int followRangeMin, int followRangeMax, SpeedFn speed,
                                     MemoryModule followMemory, bool targetEye)
        : Behavior({ MemoryCondition{ followMemory, MemoryStatus::ValuePresent },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
                   1),
          m_min(followRangeMin), m_max(followRangeMax), m_speedModifier(1.0f),
          m_speedFn(std::move(speed)), m_followMemory(followMemory),
          m_targetEye(targetEye) {}

    bool BabyFollowAdult::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        if (!body.IsBaby()) return false;
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        Entity* adult = brain->GetEntity(m_followMemory);
        if (!adult) return false;

        // Inside the min: already close enough, stop. Beyond the max + 1: too
        // far to bother. Only the band between makes a baby trot after its
        // parent, which is what stops it either shoving or teleport-chasing.
        const double d2 = body.DistanceToSqr(*adult);
        const double maxD = static_cast<double>(m_max + 1);
        const double minD = static_cast<double>(m_min);
        if (d2 >= maxD * maxD || d2 < minD * minD) return false;

        const float speed = m_speedFn ? m_speedFn(body) : m_speedModifier;
        brain->SetMemory(MemoryModule::LookTarget, PositionTracker::OfEntity(adult, true));
        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(PositionTracker::OfEntity(adult, m_targetEye),
                                    speed, m_min - 1));
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

    BehaviorPtr SetWalkTargetAwayFrom::Pos(MemoryModule avoidMemory, float speedModifier,
                                           int desiredDistance, bool interruptCurrentWalk) {
        auto b = std::make_unique<SetWalkTargetAwayFrom>(
            avoidMemory, speedModifier, desiredDistance, interruptCurrentWalk);
        b->m_isPosMemory = true;
        return b;
    }

    bool SetWalkTargetAwayFrom::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        auto* mob = dynamic_cast<PathfinderMob*>(&body);
        Brain* brain = body.GetBrain();
        if (!mob || !brain) return false;

        const WalkTarget* current = brain->GetWalkTarget(MemoryModule::WalkTarget);
        if (current && !m_interruptCurrentWalk) return false;

        // MC's entity and pos variants differ only in where the avoided
        // position comes from.
        glm::dvec3 avoidPos;
        if (m_isPosMemory) {
            const std::optional<glm::ivec3> pos = brain->GetBlockPos(m_avoid);
            if (!pos) return false;
            avoidPos = glm::dvec3(pos->x + 0.5, pos->y + 0.5, pos->z + 0.5);
        } else {
            Entity* avoid = brain->GetEntity(m_avoid);
            if (!avoid) return false;
            avoidPos = avoid->position;
        }
        const glm::dvec3 delta = avoidPos - body.position;
        if (glm::dot(delta, delta)
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

    // ── InteractWith ───────────────────────────────────────────────────────

    InteractWith::InteractWith(EntityTypeId type, int interactionRange,
                               float speedModifier, int stopDistance)
        : Behavior({ MemoryCondition{ MemoryModule::InteractionTarget,
                                      MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::LookTarget, MemoryStatus::Registered },
                     MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                     MemoryCondition{ MemoryModule::NearestVisibleLivingEntities,
                                      MemoryStatus::ValuePresent } },
                   1),
          m_type(type), m_rangeSqr(interactionRange * interactionRange),
          m_speedModifier(speedModifier), m_stopDistance(stopDistance) {}

    bool InteractWith::CheckExtraStartConditions(EntityLevel&, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        const NearestVisibleLivingEntities* visible =
            brain->GetVisibleEntities(MemoryModule::NearestVisibleLivingEntities);
        if (!visible) return false;
        LivingEntity* target = visible->FindClosest([&](LivingEntity* e) {
            return e->GetType() == m_type
                && body.DistanceToSqr(*e) <= static_cast<double>(m_rangeSqr);
        });
        if (!target) return false;
        brain->SetMemory(MemoryModule::InteractionTarget, static_cast<Entity*>(target));
        brain->SetMemory(MemoryModule::LookTarget,
                         PositionTracker::OfEntity(target, true));
        brain->SetMemory(MemoryModule::WalkTarget,
                         WalkTarget(PositionTracker::OfEntity(target, false),
                                    m_speedModifier, m_stopDistance));
        return true;
    }

    // ── StopBeingAngryIfTargetDead ─────────────────────────────────────────

    StopBeingAngryIfTargetDead::StopBeingAngryIfTargetDead()
        : Behavior({ MemoryCondition{ MemoryModule::AngryAt,
                                      MemoryStatus::ValuePresent } },
                   1) {}

    bool StopBeingAngryIfTargetDead::CheckExtraStartConditions(EntityLevel&,
                                                               LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return false;
        auto* target = dynamic_cast<LivingEntity*>(brain->GetEntity(MemoryModule::AngryAt));
        // A dead PLAYER is forgiven only under forgive_dead_players.
        if (target && target->IsDeadOrDying() &&
            (!target->IsPlayer() || Rules::GetBool(Rules::Id::ForgiveDeadPlayers))) {
            brain->EraseMemory(MemoryModule::AngryAt);
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
        // MC HurtBySensor.doTick, scan rate 1: the memory mirrors
        // getLastDamageSource() — SET (no TTL) while the source is live (MC's
        // 40-tick lastDamageStamp window, which HasLastDamageSource models),
        // ERASED the tick it lapses. No hurtTime gate: hurtTime is the 10-tick
        // red flash, and gating on it made the memory die 30 ticks early.
        if (body.HasLastDamageSource()) {
            // MC stores the DamageSource; this port does not model one, so the
            // memory is a Unit and the attacker rides in HURT_BY_ENTITY.
            brain->SetMemory(MemoryModule::HurtBy, std::monostate{});
            // MC takes damageSource.getEntity(); the port's damage source does
            // not carry its attacker, so GetLastHurtByMob (stamped by the same
            // hit) is the closest record.
            if (Entity* by = body.GetLastHurtByMob()) {
                if (dynamic_cast<LivingEntity*>(by) != nullptr) {
                    brain->SetMemory(MemoryModule::HurtByEntity, by);
                }
            }
        } else {
            // MC erases only HURT_BY here — HURT_BY_ENTITY lives on until the
            // attacker dies (below) or the entity-memory TTL machinery ends it.
            brain->EraseMemory(MemoryModule::HurtBy);
        }

        // MC: a dead (or level-changed) attacker is forgotten immediately.
        if (Entity* attacker = brain->GetEntity(MemoryModule::HurtByEntity)) {
            if (!attacker->IsAlive() || attacker->Level() != body.Level()) {
                brain->EraseMemory(MemoryModule::HurtByEntity);
            }
        }
    }

    void PlayerSensor::DoTick(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;

        // MC filters level.players() to within FOLLOW_RANGE, sorted nearest
        // first. Spectators are excluded from the raw list too.
        const double range = body.GetAttributeValue(Attribute::FollowRange);
        std::vector<LivingEntity*> all;
        level.GetPlayers(all);

        std::vector<Entity*> players;
        for (LivingEntity* p : all) {
            if (p->IsSpectator()) continue;
            if (body.DistanceToSqr(*p) > range * range) continue;
            players.push_back(p);
        }
        std::sort(players.begin(), players.end(), [&](Entity* a, Entity* b) {
            return body.DistanceToSqr(*a) < body.DistanceToSqr(*b);
        });
        brain->SetMemory(MemoryModule::NearestPlayers, players);

        // The visible subset (MC isEntityTargetable: non-combat conditions at
        // follow range — line of sight, invisibility-scaled range), then the
        // attackable subset of that (MC isEntityAttackable: the combat
        // conditions — canAttack, allies, Peaceful). Both keep the distance
        // order.
        std::vector<Entity*> attackable;
        Entity* nearestVisible = nullptr;
        for (Entity* e : players) {
            auto* p = static_cast<LivingEntity*>(e);
            if (!SensorTargeting::IsEntityTargetable(body, *p)) continue;
            if (!nearestVisible) nearestVisible = p;
            if (!SensorTargeting::IsEntityAttackable(body, *p)) continue;
            attackable.push_back(p);
        }
        if (nearestVisible) {
            brain->SetMemory(MemoryModule::NearestVisiblePlayer, nearestVisible);
        } else {
            brain->EraseMemory(MemoryModule::NearestVisiblePlayer);
        }
        if (!attackable.empty()) {
            brain->SetMemory(MemoryModule::NearestVisibleAttackablePlayer, attackable.front());
        } else {
            brain->EraseMemory(MemoryModule::NearestVisibleAttackablePlayer);
        }
        brain->SetMemory(MemoryModule::NearestVisibleAttackablePlayers, std::move(attackable));
    }

    void TemptingSensor::DoTick(EntityLevel& level, LivingEntity& body) {
        Brain* brain = body.GetBrain();
        if (!brain) return;

        std::vector<LivingEntity*> players;
        level.GetPlayers(players);

        LivingEntity* best = nullptr;
        double bestDistSq = kTemptationRange * kTemptationRange;
        for (LivingEntity* p : players) {
            // MC TemptingSensor: `filter(EntitySelector.NO_SPECTATORS)`.
            if (!p->IsAlive() || p->IsSpectator()) continue;
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

    // ── Doors (MC DoorBlock.setOpen / isOpen, #mob_interactable_doors) ─────

    namespace {
        // MC Vec3i.closerToCenterThan(Position, dist): the block's CENTRE
        // within `dist` (strictly) of the point.
        bool DoorCloserToCenterThan(const glm::ivec3& b, const glm::dvec3& p, double dist) {
            const double dx = b.x + 0.5 - p.x, dy = b.y + 0.5 - p.y, dz = b.z + 0.5 - p.z;
            return dx * dx + dy * dy + dz * dz < dist * dist;
        }

        BlockState DoorStateAt(EntityLevel& level, const glm::ivec3& p) {
            const IBlockAccess* blocks = level.Blocks();
            return blocks ? blocks->GetBlockState(p.x, p.y, p.z) : BlockState{};
        }

        // MC InteractWithDoor.isMobComingThroughDoor — the other mob's live
        // path (MC reads its PATH memory; the navigation holds the same path).
        bool IsMobComingThroughDoor(LivingEntity& other, const glm::ivec3& doorPos) {
            auto* mob = dynamic_cast<Mob*>(&other);
            if (!mob || !mob->HasAiControls()) return false;
            const Path* path = mob->GetNavigation().GetPath();
            if (!path || path->IsDone() || path->GetNextNodeIndex() <= 0) return false;
            const Node& from = path->GetNode(path->GetNextNodeIndex() - 1);
            const Node& to = path->GetNextNode();
            return doorPos == glm::ivec3(from.x, from.y, from.z) || doorPos == glm::ivec3(to.x, to.y, to.z);
        }

        void RememberDoorToClose(std::vector<glm::ivec3>& doors, const glm::ivec3& pos) {
            if (std::find(doors.begin(), doors.end(), pos) == doors.end()) doors.push_back(pos);
        }
    } // namespace

    namespace Doors {

        bool IsMobInteractableDoor(BlockState s) { return IsWoodenDoorBlock(s.Block()); }
        bool IsDoorOpen(BlockState s) { return s.GetValueByName("open") == "true"; }

        void SetDoorOpen(EntityLevel& level, const glm::ivec3& pos, bool open, Entity* source) {
            // MC DoorBlock.setOpen(entity, level, state, pos, open): flags 10
            // (UPDATE_CLIENTS | UPDATE_IMMEDIATE), the other half following
            // (MC through updateShape; explicit here — the engine has no
            // double-block linkage), and the door's open/close sound.
            ILevelWrite* world = level.MutableBlocks();
            if (!world) return;
            const BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
            if (!IsMobInteractableDoor(state) || IsDoorOpen(state) == open) return;
            const std::string_view to = open ? "true" : "false";
            constexpr uint32_t kFlags = World::UpdateFlags::UpdateClients | World::UpdateFlags::Immediate;
            world->SetBlock(pos.x, pos.y, pos.z, state.SetName(PropertyId::OPEN, to), kFlags);
            const bool lower = state.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower";
            const glm::ivec3 other = pos + glm::ivec3(0, lower ? 1 : -1, 0);
            const BlockState otherState = world->GetBlockState(other.x, other.y, other.z);
            if (otherState.Block() == state.Block() &&
                otherState.GetName(PropertyId::DOUBLE_BLOCK_HALF) == (lower ? "upper" : "lower")) {
                world->SetBlock(other.x, other.y, other.z, otherState.SetName(PropertyId::OPEN, to), kFlags);
            }
            if (const BlockSetType* set = BlockSetTypeOf(state.Block())) {
                // DoorBlock.playSound: pitch nextFloat() * 0.1 + 0.9.
                const float pitch = level.Random().NextFloat() * 0.1f + 0.9f;
                level.PlaySound(nullptr, pos, open ? set->doorOpen : set->doorClose,
                                SoundSource::Blocks, 1.0f, pitch);
            }
            // MC setOpen: level.gameEvent(sourceEntity, BLOCK_OPEN / BLOCK_CLOSE, pos).
            world->GameEvent(source, open ? GameEventId::BlockOpen : GameEventId::BlockClose, pos);
        }

        void CloseDoorsThatIHaveOpenedOrPassedThrough(EntityLevel& level, LivingEntity& body,
                                                      std::vector<glm::ivec3>& doors,
                                                      const glm::ivec3* movingFrom,
                                                      const glm::ivec3* movingTo) {
            const Brain* brain = body.GetBrain();
            const std::vector<Entity*>* nearest =
                brain ? brain->GetEntityList(MemoryModule::NearestLivingEntities) : nullptr;
            for (auto it = doors.begin(); it != doors.end();) {
                const glm::ivec3 doorPos = *it;
                if ((movingFrom && *movingFrom == doorPos) || (movingTo && *movingTo == doorPos)) {
                    ++it;
                    continue;
                }
                // isDoorTooFarAway: another dimension, or the door's centre 3+
                // blocks from the body.
                if (!DoorCloserToCenterThan(doorPos, body.position, 3.0)) { it = doors.erase(it); continue; }
                const BlockState state = DoorStateAt(level, doorPos);
                if (!IsMobInteractableDoor(state) || !IsDoorOpen(state)) { it = doors.erase(it); continue; }
                bool othersComing = false;
                if (nearest) {
                    for (Entity* e : *nearest) {
                        auto* other = e ? e->AsLiving() : nullptr;
                        if (!other || other->GetType() != body.GetType()) continue;
                        if (!DoorCloserToCenterThan(doorPos, other->position, 2.0)) continue;
                        if (IsMobComingThroughDoor(*other, doorPos)) { othersComing = true; break; }
                    }
                }
                if (!othersComing) SetDoorOpen(level, doorPos, false, &body);
                it = doors.erase(it);
            }
        }

    } // namespace Doors

    // ── InteractWithDoor ───────────────────────────────────────────────────

    InteractWithDoor::InteractWithDoor(Doors::DoorsToCloseFn doorsToClose)
        : Behavior({ { MemoryModule::DoorsToClose, MemoryStatus::Registered },
                     { MemoryModule::NearestLivingEntities, MemoryStatus::Registered } }, 1),
          m_doorsToClose(doorsToClose) {}

    bool InteractWithDoor::CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) {
        auto* mob = dynamic_cast<Mob*>(&body);
        if (!mob || !m_doorsToClose) return false;
        // i.present(PATH): the navigation's live path.
        const Path* path = mob->GetNavigation().GetPath();
        if (!path || path->GetNextNodeIndex() <= 0 || path->IsDone()) return false;
        const Node& nextNode = path->GetNextNode();
        const glm::ivec3 toPos(nextNode.x, nextNode.y, nextNode.z);
        if (m_lastCheckedNode && *m_lastCheckedNode == toPos) {
            m_remainingCooldown = 20;
        } else if (--m_remainingCooldown > 0) {
            return false;
        }
        m_lastCheckedNode = toPos;
        const Node& fromNode = path->GetNode(path->GetNextNodeIndex() - 1);
        const glm::ivec3 fromPos(fromNode.x, fromNode.y, fromNode.z);

        std::vector<glm::ivec3>& doors = m_doorsToClose(body);
        const BlockState fromState = DoorStateAt(level, fromPos);
        if (Doors::IsMobInteractableDoor(fromState)) {
            if (!Doors::IsDoorOpen(fromState)) Doors::SetDoorOpen(level, fromPos, true, &body);
            RememberDoorToClose(doors, fromPos);
        }
        const BlockState toState = DoorStateAt(level, toPos);
        if (Doors::IsMobInteractableDoor(toState) && !Doors::IsDoorOpen(toState)) {
            Doors::SetDoorOpen(level, toPos, true, &body);
            RememberDoorToClose(doors, toPos);
        }
        Doors::CloseDoorsThatIHaveOpenedOrPassedThrough(level, body, doors, &fromPos, &toPos);
        return true;
    }

} // namespace Game
