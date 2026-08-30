// File: src/common/entity/ai/brain/WardenAi.cpp
#include "common/entity/ai/brain/WardenAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC WardenAi's per-behaviour durations.
        constexpr int kDiggingDuration  = 100;   // Mth.ceil(100.0F)
        constexpr int kSniffingDuration = 84;    // Mth.ceil(83.2F)

        // ── SetWardenLookTarget (ai/behavior/warden) ───────────────────────
        // Face the roar target, else the disturbance — a warden stares at what
        // it suspects long before it walks over.
        class SetWardenLookTarget : public Behavior {
        public:
            SetWardenLookTarget()
                : Behavior({ { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::DisturbanceLocation, MemoryStatus::Registered },
                             { MemoryModule::RoarTarget, MemoryStatus::Registered },
                             { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent } },
                           1) {}
            const char* DebugString() const override { return "SetWardenLookTarget"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                std::optional<glm::ivec3> target;
                if (const Entity* roarTarget = brain->GetEntity(MemoryModule::RoarTarget)) {
                    target = roarTarget->BlockPosition();
                } else {
                    target = brain->GetBlockPos(MemoryModule::DisturbanceLocation);
                }
                if (!target) return false;
                brain->SetMemory(MemoryModule::LookTarget,
                                 PositionTracker::OfBlock(*target));
                return true;
            }
        };

        // ── SetRoarTarget (ai/behavior/warden) ─────────────────────────────
        class SetRoarTarget : public Behavior {
        public:
            SetRoarTarget()
                : Behavior({ { MemoryModule::RoarTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::CantReachWalkTargetSince,
                               MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "SetRoarTarget"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return false;
                LivingEntity* target = warden->GetEntityAngryAt();
                if (!target || !warden->CanTargetEntity(target)) return false;
                brain->SetMemory(MemoryModule::RoarTarget, static_cast<Entity*>(target));
                brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
                return true;
            }
        };

        // ── TryToSniff (ai/behavior/warden) ────────────────────────────────
        class TryToSniff : public Behavior {
        public:
            TryToSniff()
                : Behavior({ { MemoryModule::IsSniffing, MemoryStatus::Registered },
                             { MemoryModule::WalkTarget, MemoryStatus::Registered },
                             { MemoryModule::SniffCooldown, MemoryStatus::ValueAbsent },
                             { MemoryModule::NearestAttackable, MemoryStatus::ValuePresent },
                             { MemoryModule::DisturbanceLocation, MemoryStatus::ValueAbsent } },
                           1) {}
            const char* DebugString() const override { return "TryToSniff"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                brain->SetMemory(MemoryModule::IsSniffing, std::monostate{});
                // MC SNIFF_COOLDOWN = UniformInt.of(100, 200).
                brain->SetMemoryWithExpiry(MemoryModule::SniffCooldown, std::monostate{},
                                           level.Random().NextInt(100, 200));
                brain->EraseMemory(MemoryModule::WalkTarget);
                body.SetPose(Pose::Sniffing);
                return true;
            }
        };

        // ── Sniffing (ai/behavior/warden) ──────────────────────────────────
        class Sniffing : public Behavior {
        public:
            explicit Sniffing(int ticks)
                : Behavior({ { MemoryModule::IsSniffing, MemoryStatus::ValuePresent },
                             { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered },
                             { MemoryModule::NearestAttackable, MemoryStatus::Registered },
                             { MemoryModule::DisturbanceLocation, MemoryStatus::Registered },
                             { MemoryModule::SniffCooldown, MemoryStatus::Registered } },
                           ticks) {}
            const char* DebugString() const override { return "WardenSniffing"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return;
                if (warden->GetPose() == Pose::Sniffing) warden->SetPose(Pose::Standing);
                brain->EraseMemory(MemoryModule::IsSniffing);
                auto* sniffed = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::NearestAttackable));
                if (sniffed && warden->CanTargetEntity(sniffed)) {
                    // MC: within 6 blocks XZ / 20 Y, the sniff FOUND you.
                    const double dx = sniffed->position.x - warden->position.x;
                    const double dy = sniffed->position.y - warden->position.y;
                    const double dz = sniffed->position.z - warden->position.z;
                    if (dx * dx + dz * dz < 6.0 * 6.0 && std::abs(dy) < 20.0) {
                        warden->IncreaseAngerAt(sniffed);
                    }
                    if (!brain->HasMemoryValue(MemoryModule::DisturbanceLocation)) {
                        WardenAi::SetDisturbanceLocation(*warden,
                                                         sniffed->BlockPosition());
                    }
                }
            }
        };

        // ── Emerging (ai/behavior/warden) ──────────────────────────────────
        class Emerging : public Behavior {
        public:
            explicit Emerging(int ticks)
                : Behavior({ { MemoryModule::IsEmerging, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } },
                           ticks) {}
            const char* DebugString() const override { return "WardenEmerging"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                body.SetPose(Pose::Emerging);
                // MC plays WARDEN_EMERGE here.
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (body.GetPose() == Pose::Emerging) body.SetPose(Pose::Standing);
            }
        };

        // ── Digging (ai/behavior/warden) ───────────────────────────────────
        // The warden's exit: 100 ticks nose-down in the dirt, then gone.
        class Digging : public Behavior {
        public:
            explicit Digging(int ticks)
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } },
                           ticks) {}
            const char* DebugString() const override { return "WardenDigging"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return body.onGround || body.IsInWater() || body.IsInLava();
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                return !body.IsRemoved();
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (body.onGround) {
                    body.SetPose(Pose::Digging);
                    // MC plays WARDEN_DIG.
                } else {
                    // MC plays WARDEN_AGITATED and stop()s itself, which
                    // discards the warden on the spot — a floating warden
                    // cannot dig, it just leaves.
                    body.Discard();
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (!body.IsRemoved()) body.Discard();
            }
        };

        // ── Roar (ai/behavior/warden) ──────────────────────────────────────
        class Roar : public Behavior {
        public:
            static constexpr int kTicksBeforeRoarSound = 25;

            Roar()
                : Behavior({ { MemoryModule::RoarTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::RoarSoundCooldown, MemoryStatus::Registered },
                             { MemoryModule::RoarSoundDelay, MemoryStatus::Registered } },
                           WardenAi::kRoarDuration) {}
            const char* DebugString() const override { return "WardenRoar"; }

        protected:
            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return;
                brain->SetMemoryWithExpiry(MemoryModule::RoarSoundDelay,
                                           std::monostate{}, kTicksBeforeRoarSound);
                brain->EraseMemory(MemoryModule::WalkTarget);
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::RoarTarget));
                if (target) {
                    // MC BehaviorUtils.lookAtEntity.
                    brain->SetMemory(MemoryModule::LookTarget,
                                     PositionTracker::OfEntity(target, true));
                    warden->IncreaseAngerAt(target, 20, false);
                }
                warden->SetPose(Pose::Roaring);
            }

            void Tick(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                // MC: the WARDEN_ROAR sound fires once, 25 ticks in. The
                // memory dance is kept so the timing is right when sounds
                // arrive.
                if (!brain->HasMemoryValue(MemoryModule::RoarSoundDelay)
                    && !brain->HasMemoryValue(MemoryModule::RoarSoundCooldown)) {
                    brain->SetMemoryWithExpiry(
                        MemoryModule::RoarSoundCooldown, std::monostate{},
                        WardenAi::kRoarDuration - kTicksBeforeRoarSound);
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return;
                if (warden->GetPose() == Pose::Roaring) warden->SetPose(Pose::Standing);
                if (auto* target = dynamic_cast<LivingEntity*>(
                        brain->GetEntity(MemoryModule::RoarTarget))) {
                    warden->SetAttackTarget(target);
                }
                brain->EraseMemory(MemoryModule::RoarTarget);
            }
        };

        // ── SonicBoom (ai/behavior/warden) ─────────────────────────────────
        class SonicBoom : public Behavior {
        public:
            static constexpr int    kDuration = 60;      // Mth.ceil(60.0F)
            static constexpr int    kTicksBeforeSound = 34;
            static constexpr int    kCooldown = 40;
            static constexpr double kDistanceXZ = 15.0;
            static constexpr double kDistanceY  = 20.0;

            SonicBoom()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::SonicBoomCooldown, MemoryStatus::ValueAbsent },
                             { MemoryModule::SonicBoomSoundCooldown, MemoryStatus::Registered },
                             { MemoryModule::SonicBoomSoundDelay, MemoryStatus::Registered } },
                           kDuration) {}
            const char* DebugString() const override { return "WardenSonicBoom"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                const Brain* brain = body.GetBrain();
                const Entity* target =
                    brain ? brain->GetEntity(MemoryModule::AttackTarget) : nullptr;
                return target && CloserThan(body, *target);
            }

            bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }

            void Start(EntityLevel& level, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                brain->SetMemoryWithExpiry(MemoryModule::AttackCoolingDown, true, kDuration);
                brain->SetMemoryWithExpiry(MemoryModule::SonicBoomSoundDelay,
                                           std::monostate{}, kTicksBeforeSound);
                // Entity event 62 — the client starts the sonic-boom wind-up.
                level.BroadcastEntityEvent(body, 62);
                // MC plays WARDEN_SONIC_CHARGE here.
            }

            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return;

                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                if (target) {
                    warden->GetLookControl().SetLookAt(target->position);
                }
                if (brain->HasMemoryValue(MemoryModule::SonicBoomSoundDelay)
                    || brain->HasMemoryValue(MemoryModule::SonicBoomSoundCooldown)) {
                    return;
                }
                brain->SetMemoryWithExpiry(MemoryModule::SonicBoomSoundCooldown,
                                           std::monostate{},
                                           kDuration - kTicksBeforeSound);
                if (!target || !warden->CanTargetEntity(target)
                    || !CloserThan(*warden, *target)) {
                    return;
                }

                // MC fires the beam from the WARDEN_CHEST attachment and draws
                // SONIC_BOOM particles along it — no particle system here, the
                // damage line is the whole of the effect.
                const glm::dvec3 source = warden->GetEyePosition();
                glm::dvec3 delta = target->GetEyePosition() - source;
                const double len = std::sqrt(delta.x * delta.x + delta.y * delta.y
                                             + delta.z * delta.z);
                if (len > 1.0e-8) delta /= len;

                // MC: 10.0F of armor-BYPASSING damage. This port's armor
                // absorb has no bypass channel; mobs carry no armor, so only
                // an armored player notices the difference.
                if (target->Hurt(MobDamageSource::Generic, 10.0f, warden)) {
                    // MC push: horizontal 2.5, vertical 0.5, both scaled by
                    // (1 − knockback resistance). Knockback() carries the
                    // resistance scaling and the player wire-sync; MC's exact
                    // vertical component becomes its 0.4-capped ground lift.
                    target->Knockback(2.5, -delta.x, -delta.z);
                }
                // MC plays WARDEN_SONIC_BOOM here.
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemoryWithExpiry(MemoryModule::SonicBoomCooldown,
                                               std::monostate{}, kCooldown);
                }
            }

        private:
            static bool CloserThan(const LivingEntity& body, const Entity& target) {
                const double dx = target.position.x - body.position.x;
                const double dy = target.position.y - body.position.y;
                const double dz = target.position.z - body.position.z;
                return dx * dx + dz * dz < kDistanceXZ * kDistanceXZ
                    && std::abs(dy) < kDistanceY;
            }
        };

        // ── GoToTargetLocation (ai/behavior, INVESTIGATE's mover) ──────────
        class GoToTargetLocation : public Behavior {
        public:
            GoToTargetLocation(MemoryModule locationMemory, int closeEnoughDist,
                               float speedModifier)
                : Behavior({ { locationMemory, MemoryStatus::ValuePresent },
                             { MemoryModule::AttackTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } },
                           1),
                  m_memory(locationMemory), m_closeEnoughDist(closeEnoughDist),
                  m_speedModifier(speedModifier) {}
            const char* DebugString() const override { return "GoToTargetLocation"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                if (!brain) return false;
                const std::optional<glm::ivec3> location = brain->GetBlockPos(m_memory);
                if (!location) return false;

                const glm::ivec3 self = body.BlockPosition();
                const glm::ivec3 d = *location - self;
                const bool close = static_cast<double>(d.x) * d.x
                                       + static_cast<double>(d.y) * d.y
                                       + static_cast<double>(d.z) * d.z
                                   < static_cast<double>(m_closeEnoughDist)
                                         * m_closeEnoughDist;
                if (!close) {
                    // MC getNearbyPos — a ±1 jitter so a crowd of investigators
                    // does not stack on the same block.
                    JavaRandom& rnd = level.Random();
                    const glm::ivec3 nearby = *location
                        + glm::ivec3(rnd.NextInt(3) - 1, 0, rnd.NextInt(3) - 1);
                    brain->SetMemory(MemoryModule::LookTarget,
                                     PositionTracker::OfBlock(nearby));
                    brain->SetMemory(MemoryModule::WalkTarget,
                                     WalkTarget(nearby, m_speedModifier,
                                                m_closeEnoughDist));
                }
                return true;
            }

        private:
            MemoryModule m_memory;
            int   m_closeEnoughDist;
            float m_speedModifier;
        };

        // ── DIG_COOLDOWN_SETTER (MC WardenAi's inline behaviour) ───────────
        // Runs every tick of FIGHT, holding the dig cooldown at 1200 so a
        // warden never digs away mid-battle.
        class DigCooldownSetter : public Behavior {
        public:
            DigCooldownSetter()
                : Behavior({ { MemoryModule::DigCooldown, MemoryStatus::Registered } }, 1) {}
            const char* DebugString() const override { return "WardenDigCooldownSetter"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (auto* warden = dynamic_cast<Warden*>(&body)) {
                    WardenAi::SetDigCooldown(*warden);
                }
                return true;
            }
        };

        // ── StopAttackingIfTargetInvalid, warden flavour ───────────────────
        // MC's create(pred, onStop, false): the target is invalid once the
        // warden is no longer ANGRY or the entity stopped being targetable;
        // dropping it clears its anger and re-arms the dig cooldown.
        class WardenStopAttacking : public Behavior {
        public:
            WardenStopAttacking()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent } },
                           1) {}
            const char* DebugString() const override { return "WardenStopAttacking"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return false;
                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                const bool invalid = !target || !target->IsAlive()
                    || !warden->IsAngry() || !warden->CanTargetEntity(target);
                if (invalid) {
                    if (target && !warden->CanTargetEntity(target)) {
                        warden->ClearAnger(target);
                    }
                    WardenAi::SetDigCooldown(*warden);
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
                return true;
            }
        };

        // ── SetEntityLookTarget, warden flavour ────────────────────────────
        // MC passes `isTarget(body, entity)` — during FIGHT the warden only
        // ever look-locks its own attack target.
        class LookAtAttackTarget : public Behavior {
        public:
            LookAtAttackTarget()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::LookTarget, MemoryStatus::Registered } },
                           1) {}
            const char* DebugString() const override { return "WardenLookAtAttackTarget"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                Brain* brain = body.GetBrain();
                auto* target = brain ? dynamic_cast<LivingEntity*>(
                                   brain->GetEntity(MemoryModule::AttackTarget))
                                     : nullptr;
                if (!target) return false;
                brain->SetMemory(MemoryModule::LookTarget,
                                 PositionTracker::OfEntity(target, true));
                return true;
            }
        };

        // ── WardenEntitySensor (ai/sensing) ────────────────────────────────
        // NEAREST_ATTACKABLE = the closest targetable entity, players first.
        class WardenEntitySensor : public NearestLivingEntitySensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                std::vector<MemoryModule> req = NearestLivingEntitySensor::Requires();
                req.push_back(MemoryModule::NearestAttackable);
                return req;
            }

        protected:
            void DoTick(EntityLevel& level, LivingEntity& body) override {
                NearestLivingEntitySensor::DoTick(level, body);
                auto* warden = dynamic_cast<Warden*>(&body);
                Brain* brain = body.GetBrain();
                if (!warden || !brain) return;

                Entity* found = nullptr;
                if (const std::vector<Entity*>* nearest =
                        brain->GetEntityList(MemoryModule::NearestLivingEntities)) {
                    // Players first, then anything else — MC's two getClosest
                    // passes.
                    for (int pass = 0; pass < 2 && !found; ++pass) {
                        for (Entity* e : *nearest) {
                            if ((pass == 0) != e->IsPlayer()) continue;
                            if (!warden->CanTargetEntity(e)) continue;
                            found = e;
                            break;
                        }
                    }
                }
                if (found) brain->SetMemory(MemoryModule::NearestAttackable, found);
                else       brain->EraseMemory(MemoryModule::NearestAttackable);
            }
        };

    } // namespace

    void WardenAi::SetDigCooldown(Warden& warden) {
        Brain* brain = warden.GetBrain();
        if (brain && brain->HasMemoryValue(MemoryModule::DigCooldown)) {
            brain->SetMemoryWithExpiry(MemoryModule::DigCooldown, std::monostate{},
                                       kDiggingCooldown);
        }
    }

    void WardenAi::SetDisturbanceLocation(Warden& warden, const glm::ivec3& pos) {
        // MC also rejects positions outside the world border, which this
        // engine does not have.
        if (warden.GetEntityAngryAt()) return;
        Brain* brain = warden.GetBrain();
        if (!brain || brain->HasMemoryValue(MemoryModule::AttackTarget)) return;
        SetDigCooldown(warden);
        brain->SetMemoryWithExpiry(MemoryModule::SniffCooldown, std::monostate{}, 100);
        brain->SetMemoryWithExpiry(MemoryModule::LookTarget,
                                   PositionTracker::OfBlock(pos), 100);
        brain->SetMemoryWithExpiry(MemoryModule::DisturbanceLocation, pos, 100);
        brain->EraseMemory(MemoryModule::WalkTarget);
    }

    void WardenAi::InitBrain(Warden& warden, Brain& brain) {
        (void)warden;

        // MC WardenAi.MEMORY_TYPES.
        for (MemoryModule m : { MemoryModule::NearestLivingEntities,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestVisiblePlayer,
                                MemoryModule::NearestVisibleAttackablePlayer,
                                MemoryModule::NearestVisibleNemesis,
                                MemoryModule::LookTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::Path,
                                MemoryModule::AttackTarget,
                                MemoryModule::AttackCoolingDown,
                                MemoryModule::NearestAttackable,
                                MemoryModule::RoarTarget,
                                MemoryModule::DisturbanceLocation,
                                MemoryModule::RecentProjectile,
                                MemoryModule::IsSniffing,
                                MemoryModule::IsEmerging,
                                MemoryModule::RoarSoundDelay,
                                MemoryModule::DigCooldown,
                                MemoryModule::RoarSoundCooldown,
                                MemoryModule::SniffCooldown,
                                MemoryModule::TouchCooldown,
                                MemoryModule::VibrationCooldown,
                                MemoryModule::SonicBoomCooldown,
                                MemoryModule::SonicBoomSoundCooldown,
                                MemoryModule::SonicBoomSoundDelay }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<WardenEntitySensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<SetWardenLookTarget>());
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        core.push_back(std::make_unique<MoveToTargetSink>());
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── EMERGE ─────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> emerge;
        emerge.push_back(std::make_unique<Emerging>(kEmergeDuration));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Emerge, 5,
                                                    std::move(emerge),
                                                    MemoryModule::IsEmerging);

        // ── DIG ────────────────────────────────────────────────────────────
        // MC puts a ForceUnmount ahead of Digging; nothing rides here.
        std::vector<BehaviorPtr> dig;
        dig.push_back(std::make_unique<Digging>(kDiggingDuration));
        brain.AddActivityWithConditions(
            Activity::Dig, 1, std::move(dig),
            { MemoryCondition{ MemoryModule::RoarTarget, MemoryStatus::ValueAbsent },
              MemoryCondition{ MemoryModule::DigCooldown, MemoryStatus::ValueAbsent } });

        // ── IDLE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<SetRoarTarget>());
        idle.push_back(std::make_unique<TryToSniff>());
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ RandomStroll::Stroll(0.5f), 2 });
        idleGate.push_back({ std::make_unique<DoNothing>(30, 60), 1 });
        idle.push_back(MakeRunOne(
            { MemoryCondition{ MemoryModule::IsSniffing, MemoryStatus::ValueAbsent } },
            std::move(idleGate)));
        brain.AddActivity(Activity::Idle, 10, std::move(idle));

        // ── ROAR ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> roar;
        roar.push_back(std::make_unique<Roar>());
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Roar, 10,
                                                    std::move(roar),
                                                    MemoryModule::RoarTarget);

        // ── FIGHT ──────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<DigCooldownSetter>());
        fight.push_back(std::make_unique<WardenStopAttacking>());
        fight.push_back(std::make_unique<LookAtAttackTarget>());
        fight.push_back(std::make_unique<SetWalkTargetFromAttackTarget>(1.2f));
        fight.push_back(std::make_unique<SonicBoom>());
        fight.push_back(std::make_unique<MeleeAttack>(18));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Fight, 10,
                                                    std::move(fight),
                                                    MemoryModule::AttackTarget);

        // ── INVESTIGATE ────────────────────────────────────────────────────
        std::vector<BehaviorPtr> investigate;
        investigate.push_back(std::make_unique<SetRoarTarget>());
        investigate.push_back(std::make_unique<GoToTargetLocation>(
            MemoryModule::DisturbanceLocation, 2, 0.7f));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Investigate, 5,
                                                    std::move(investigate),
                                                    MemoryModule::DisturbanceLocation);

        // ── SNIFF ──────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> sniff;
        sniff.push_back(std::make_unique<SetRoarTarget>());
        sniff.push_back(std::make_unique<Sniffing>(kSniffingDuration));
        brain.AddActivityAndRemoveMemoryWhenStopped(Activity::Sniff, 5,
                                                    std::move(sniff),
                                                    MemoryModule::IsSniffing);

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Idle);
        brain.UseDefaultActivity();
    }

    void WardenAi::UpdateActivity(Warden& warden) {
        if (Brain* brain = warden.GetBrain()) {
            brain->SetActiveActivityToFirstValid(
                { Activity::Emerge, Activity::Dig, Activity::Roar, Activity::Fight,
                  Activity::Investigate, Activity::Sniff, Activity::Idle });
        }
    }

} // namespace Game
