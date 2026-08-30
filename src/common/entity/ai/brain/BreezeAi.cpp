// File: src/common/entity/ai/brain/BreezeAi.cpp
#include "common/entity/ai/brain/BreezeAi.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/ai/brain/CommonBehaviors.hpp"
#include "common/entity/ai/brain/CoreBehaviors.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // ── BreezeUtil ─────────────────────────────────────────────────────

        // java.util.Random.nextGaussian (Marsaglia polar), minus Java's cached
        // second sample. The RNG stream already diverges from MC here (this
        // whole brain consumes different draws), so the DISTRIBUTION is the
        // contract, not the sequence.
        double NextGaussian(JavaRandom& rnd) {
            double v1, v2, s;
            do {
                v1 = 2.0 * rnd.NextDouble() - 1.0;
                v2 = 2.0 * rnd.NextDouble() - 1.0;
                s = v1 * v1 + v2 * v2;
            } while (s >= 1.0 || s == 0.0);
            return v1 * std::sqrt(-2.0 * std::log(s) / s);
        }

        // MC BreezeUtil.randomPointBehindTarget: a point 4–8 blocks from the
        // enemy, centred on the direction OPPOSITE its head, ±45° gaussian.
        glm::dvec3 RandomPointBehindTarget(const LivingEntity& enemy, JavaRandom& rnd) {
            const float viewAngle = enemy.yHeadRot + 180.0f
                                    + static_cast<float>(NextGaussian(rnd)) * 90.0f / 2.0f;
            const float r = Mth::Lerp(rnd.NextFloat(), 4.0f, 8.0f);
            const glm::vec3 dir = Mth::HorizontalViewVector(viewAngle) * r;
            return enemy.position + glm::dvec3(dir);
        }

        // MC BreezeUtil.hasLineOfSight — a COLLIDER clip from the breeze's
        // FEET (not eyes) to the point, same DDA scheme as Sensing.
        bool HasLineOfSightToPoint(const Breeze& breeze, const glm::dvec3& target) {
            EntityLevel* level = breeze.Level();
            if (!level) return false;
            const IBlockAccess* blocks = level->Blocks();
            if (!blocks) return false;

            const glm::dvec3 from = breeze.position;
            const glm::dvec3 delta = target - from;
            const double dist = std::sqrt(delta.x * delta.x + delta.y * delta.y
                                          + delta.z * delta.z);
            const double maxRange = std::max(
                50.0, breeze.GetAttributeValue(Attribute::FollowRange));
            if (dist > maxRange) return false;
            if (dist < 1.0e-4) return true;

            const int steps = static_cast<int>(std::ceil(dist * 4.0));
            const glm::dvec3 step = delta / static_cast<double>(steps);
            glm::dvec3 p = from;
            for (int i = 1; i < steps; ++i) {
                p += step;
                if (BlockRegistry::HasCollision(blocks->GetBlock(
                        static_cast<int>(std::floor(p.x)),
                        static_cast<int>(std::floor(p.y)),
                        static_cast<int>(std::floor(p.z))))) {
                    return false;
                }
            }
            return true;
        }

        // MC Entity.lookAt(EntityAnchor.EYES, target) — an instant snap, not a
        // LookControl request; the shoot and jump wind-ups aim in one tick.
        void LookAtInstantly(Mob& mob, const glm::dvec3& target) {
            const glm::dvec3 d = target - mob.GetEyePosition();
            const double horiz = std::sqrt(d.x * d.x + d.z * d.z);
            mob.xRot = Mth::WrapDegrees(static_cast<float>(
                -(std::atan2(d.y, horiz) * Mth::kRadToDeg)));
            mob.yRot = Mth::WrapDegrees(static_cast<float>(
                std::atan2(d.z, d.x) * Mth::kRadToDeg) - 90.0f);
            mob.SetYHeadRot(mob.yRot);
        }

        // MC LongJump.snapToSurface — clip 10 down for a floor, else 10 up for
        // the underside of a ceiling; the jump target is the block ABOVE the
        // hit. Block-column scan instead of a precise clip: the target only
        // needs to be a standable cell, not an exact surface point.
        std::optional<glm::ivec3> SnapToSurface(const EntityLevel& level,
                                                const glm::dvec3& target) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return std::nullopt;
            const int bx = static_cast<int>(std::floor(target.x));
            const int by = static_cast<int>(std::floor(target.y));
            const int bz = static_cast<int>(std::floor(target.z));
            for (int dy = 0; dy <= 10; ++dy) {
                if (BlockRegistry::HasCollision(blocks->GetBlock(bx, by - dy, bz))) {
                    return glm::ivec3(bx, by - dy + 1, bz);
                }
            }
            for (int dy = 1; dy <= 10; ++dy) {
                if (BlockRegistry::HasCollision(blocks->GetBlock(bx, by + dy, bz))) {
                    return glm::ivec3(bx, by + dy + 1, bz);
                }
            }
            return std::nullopt;
        }

        // MC EntityType.isBlockDangerous for a landing cell — the blocks a
        // breeze refuses to jump onto. The breeze has no relevant immunities.
        bool IsDangerousLanding(BlockID block) {
            switch (block) {
                case BlockID::Fire:
                case BlockID::Lava:
                case BlockID::Cactus:
                case BlockID::SweetBerryBush:
                case BlockID::WitherRose:
                case BlockID::PowderSnow:
                case BlockID::MagmaBlock:
                    return true;
                default:
                    return false;
            }
        }

        LivingEntity* GetAttackTarget(const LivingEntity& body) {
            const Brain* brain = body.GetBrain();
            return brain ? dynamic_cast<LivingEntity*>(
                               brain->GetEntity(MemoryModule::AttackTarget))
                         : nullptr;
        }

        // ── SlideToTargetSink (MC BreezeAi.SlideToTargetSink) ──────────────
        //
        // The breeze's ONLY mover. It lives in IDLE, and the FIGHT activity
        // requires WALK_TARGET absent — so Slide setting a walk target drops
        // the brain into IDLE, this sink slides there (pose SLIDING), and its
        // stop() arms BREEZE_SHOOT so the fight resumes with a shot.
        class SlideToTargetSink : public MoveToTargetSink {
        public:
            SlideToTargetSink(int minTimeout, int maxTimeout)
                : MoveToTargetSink(minTimeout, maxTimeout) {}
            const char* DebugString() const override { return "SlideToTargetSink"; }

        protected:
            void Start(EntityLevel& level, LivingEntity& body, int64_t t) override {
                MoveToTargetSink::Start(level, body, t);
                body.SetPose(Pose::Sliding);
            }

            void Stop(EntityLevel& level, LivingEntity& body, int64_t t) override {
                MoveToTargetSink::Stop(level, body, t);
                body.SetPose(Pose::Standing);
                Brain* brain = body.GetBrain();
                if (brain && brain->HasMemoryValue(MemoryModule::AttackTarget)) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShoot,
                                               std::monostate{}, 60);
                }
            }
        };

        // ── Shoot (MC monster/breeze/Shoot) ────────────────────────────────

        class Shoot : public Behavior {
        public:
            // MC's SHOOT_INITIAL_DELAY(15) + 1 + SHOOT_RECOVER_DELAY(4).
            static constexpr int kInitialDelay = 15;
            static constexpr int kRecoverDelay = 4;
            static constexpr int kCooldown     = 10;
            static constexpr double kAttackRangeSq = 256.0;

            Shoot()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::BreezeShootCooldown, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeShootCharging, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeShootRecovering, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeShoot, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeJumpTarget, MemoryStatus::ValueAbsent } },
                           kInitialDelay + 1 + kRecoverDelay) {}
            const char* DebugString() const override { return "BreezeShoot"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                if (body.GetPose() != Pose::Standing) return false;
                Brain* brain = body.GetBrain();
                LivingEntity* target = GetAttackTarget(body);
                if (!brain || !target) return false;
                // MC erases BREEZE_SHOOT when the target has drifted out of the
                // 16-block shooting range, so Slide/LongJump get to reposition.
                if (body.DistanceToSqr(*target) >= kAttackRangeSq) {
                    brain->EraseMemory(MemoryModule::BreezeShoot);
                    return false;
                }
                return true;
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                return brain && brain->HasMemoryValue(MemoryModule::AttackTarget)
                    && brain->HasMemoryValue(MemoryModule::BreezeShoot);
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (brain && brain->HasMemoryValue(MemoryModule::AttackTarget)) {
                    body.SetPose(Pose::Shooting);
                }
                if (brain) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShootCharging,
                                               std::monostate{}, kInitialDelay);
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (body.GetPose() == Pose::Shooting) body.SetPose(Pose::Standing);
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShootCooldown,
                                               std::monostate{}, kCooldown);
                    brain->EraseMemory(MemoryModule::BreezeShoot);
                }
            }

            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* breeze = dynamic_cast<Breeze*>(&body);
                Brain* brain = body.GetBrain();
                LivingEntity* target = GetAttackTarget(body);
                if (!breeze || !brain || !target) return;

                LookAtInstantly(*breeze, target->position);
                if (!brain->HasMemoryValue(MemoryModule::BreezeShootCharging)
                    && !brain->HasMemoryValue(MemoryModule::BreezeShootRecovering)) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShootRecovering,
                                               std::monostate{}, kRecoverDelay);
                    // MC Shoot.tick's aim: target.getY(0.3) (0.8 when riding —
                    // no riding here), from the firing height, inaccuracy
                    // 5 − 4·difficultyId.
                    const double xd = target->position.x - breeze->position.x;
                    const double yd = (target->position.y
                                       + target->GetBbHeight() * 0.3)
                                      - breeze->GetFiringYPosition();
                    const double zd = target->position.z - breeze->position.z;
                    const float inaccuracy = static_cast<float>(
                        5 - static_cast<int>(level.GetDifficulty()) * 4);
                    breeze->ShootWindCharge(xd, yd, zd, inaccuracy);
                }
            }
        };

        // ── LongJump (MC monster/breeze/LongJump) ──────────────────────────

        class LongJump : public Behavior {
        public:
            static constexpr int kInhalingDuration = 10;
            // MC MAX_JUMP_VELOCITY_MULTIPLIER — ×24 follow range = 1.4.
            static constexpr float kMaxJumpVelocityMultiplier = 0.058333334f;

            LongJump()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::BreezeJumpCooldown, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeJumpInhaling, MemoryStatus::Registered },
                             { MemoryModule::BreezeJumpTarget, MemoryStatus::Registered },
                             { MemoryModule::BreezeShoot, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeLeavingWater, MemoryStatus::Registered } },
                           200) {}
            const char* DebugString() const override { return "BreezeLongJump"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* breeze = dynamic_cast<Breeze*>(&body);
                return breeze && CanRun(level, *breeze);
            }

            bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override {
                const Brain* brain = body.GetBrain();
                return body.GetPose() != Pose::Standing && brain
                    && !brain->HasMemoryValue(MemoryModule::BreezeJumpCooldown);
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                Brain* brain = body.GetBrain();
                if (!brain) return;
                if (brain->CheckMemory(MemoryModule::BreezeJumpInhaling,
                                       MemoryStatus::ValueAbsent)) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeJumpInhaling,
                                               std::monostate{}, kInhalingDuration);
                }
                body.SetPose(Pose::Inhaling);
                if (const std::optional<glm::ivec3> target =
                        brain->GetBlockPos(MemoryModule::BreezeJumpTarget)) {
                    if (auto* mob = dynamic_cast<Mob*>(&body)) {
                        LookAtInstantly(*mob, glm::dvec3(*target) + glm::dvec3(0.5, 0.5, 0.5));
                    }
                }
            }

            void Tick(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* breeze = dynamic_cast<Breeze*>(&body);
                Brain* brain = body.GetBrain();
                if (!breeze || !brain) return;

                const bool inWater = breeze->IsInWater();
                if (!inWater && brain->CheckMemory(MemoryModule::BreezeLeavingWater,
                                                   MemoryStatus::ValuePresent)) {
                    brain->EraseMemory(MemoryModule::BreezeLeavingWater);
                }

                if (IsFinishedInhaling(*breeze)) {
                    std::optional<glm::dvec3> velocity;
                    if (const std::optional<glm::ivec3> target =
                            brain->GetBlockPos(MemoryModule::BreezeJumpTarget)) {
                        velocity = CalculateOptimalJumpVector(
                            *breeze, level,
                            glm::dvec3(target->x + 0.5, target->y, target->z + 0.5));
                    }
                    if (!velocity) {
                        breeze->SetPose(Pose::Standing);
                        return;
                    }
                    if (inWater) {
                        brain->SetMemory(MemoryModule::BreezeLeavingWater,
                                         std::monostate{});
                    }
                    breeze->SetPose(Pose::LongJumping);
                    breeze->yRot = breeze->yBodyRot;
                    breeze->SetDiscardFriction(true);
                    breeze->velocity = *velocity;
                } else if (IsFinishedJumping(*breeze)) {
                    breeze->SetPose(Pose::Standing);
                    breeze->SetDiscardFriction(false);
                    const bool wasHurt = brain->HasMemoryValue(MemoryModule::HurtBy);
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeJumpCooldown,
                                               std::monostate{}, wasHurt ? 2 : 10);
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShoot,
                                               std::monostate{}, 100);
                }
            }

            void Stop(EntityLevel&, LivingEntity& body, int64_t) override {
                if (body.GetPose() == Pose::LongJumping
                    || body.GetPose() == Pose::Inhaling) {
                    body.SetPose(Pose::Standing);
                }
                if (Brain* brain = body.GetBrain()) {
                    brain->EraseMemory(MemoryModule::BreezeJumpTarget);
                    brain->EraseMemory(MemoryModule::BreezeJumpInhaling);
                    brain->EraseMemory(MemoryModule::BreezeLeavingWater);
                }
            }

        private:
            static bool IsFinishedInhaling(const Breeze& breeze) {
                const Brain* brain = breeze.GetBrain();
                return brain
                    && !brain->HasMemoryValue(MemoryModule::BreezeJumpInhaling)
                    && breeze.GetPose() == Pose::Inhaling;
            }

            static bool IsFinishedJumping(const Breeze& breeze) {
                const Brain* brain = breeze.GetBrain();
                const bool isJumping = breeze.GetPose() == Pose::LongJumping;
                const bool landedOnGround = breeze.onGround;
                const bool landedInWater = breeze.IsInWater() && brain
                    && brain->CheckMemory(MemoryModule::BreezeLeavingWater,
                                          MemoryStatus::ValueAbsent);
                return isJumping && (landedOnGround || landedInWater);
            }

            // MC LongJump.canRun — the long gauntlet of jump preconditions.
            static bool CanRun(EntityLevel& level, Breeze& breeze) {
                // MC: (!onGround && !isInWater) → false, then shouldSwim →
                // false. shouldSwim tests the fluid HEIGHT against the jump
                // threshold; without fluid heights "in water at all" stands in,
                // which folds both clauses into onGround-only — the in-water
                // breeze escapes via ShootWhenStuck instead, as in MC.
                if (!breeze.onGround) return false;
                if (breeze.IsInWater()) return false;

                Brain* brain = breeze.GetBrain();
                if (!brain) return false;
                if (brain->CheckMemory(MemoryModule::BreezeJumpTarget,
                                       MemoryStatus::ValuePresent)) {
                    return true;
                }

                LivingEntity* target = GetAttackTarget(breeze);
                if (!target) return false;
                if (OutOfAggroRange(breeze, *target)) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                    return false;
                }
                if (TooCloseForJump(breeze, *target)) return false;
                if (!CanJumpFromCurrentPosition(level, breeze)) return false;

                const std::optional<glm::ivec3> targetPos = SnapToSurface(
                    level, RandomPointBehindTarget(*target, level.Random()));
                if (!targetPos) return false;
                const IBlockAccess* blocks = level.Blocks();
                if (blocks && IsDangerousLanding(blocks->GetBlock(
                        targetPos->x, targetPos->y - 1, targetPos->z))) {
                    return false;
                }
                const glm::dvec3 centre(targetPos->x + 0.5, targetPos->y + 0.5,
                                        targetPos->z + 0.5);
                if (!HasLineOfSightToPoint(breeze, centre)
                    && !HasLineOfSightToPoint(breeze,
                                              centre + glm::dvec3(0.0, 4.0, 0.0))) {
                    return false;
                }
                brain->SetMemory(MemoryModule::BreezeJumpTarget, *targetPos);
                return true;
            }

            static bool OutOfAggroRange(const Breeze& breeze, const LivingEntity& target) {
                const double range = breeze.GetAttributeValue(Attribute::FollowRange);
                return breeze.DistanceToSqr(target) > range * range;
            }

            static bool TooCloseForJump(const Breeze& breeze, const LivingEntity& target) {
                return breeze.DistanceTo(target) - 4.0 <= 0.0;
            }

            static bool CanJumpFromCurrentPosition(EntityLevel& level, const Breeze& breeze) {
                const IBlockAccess* blocks = level.Blocks();
                if (!blocks) return false;
                const glm::ivec3 pos = breeze.BlockPosition();
                // MC refuses to jump off honey.
                if (blocks->GetBlock(pos.x, pos.y, pos.z) == BlockID::HoneyBlock) {
                    return false;
                }
                // Four clear blocks above — air or water.
                for (int i = 1; i <= 4; ++i) {
                    const BlockID b = blocks->GetBlock(pos.x, pos.y + i, pos.z);
                    if (b != BlockID::Air && b != BlockID::Water) return false;
                }
                return true;
            }

            static std::optional<glm::dvec3> CalculateOptimalJumpVector(
                Breeze& breeze, EntityLevel& level, const glm::dvec3& targetPos) {
                // MC's ALLOWED_ANGLES, Util.shuffledCopy'd per attempt.
                int angles[] = { 40, 55, 60, 75, 80 };
                JavaRandom& rnd = level.Random();
                for (int i = 4; i > 0; --i) {
                    std::swap(angles[i], angles[rnd.NextInt(i + 1)]);
                }
                const float maxJumpVelocity =
                    kMaxJumpVelocityMultiplier
                    * static_cast<float>(breeze.GetAttributeValue(Attribute::FollowRange));
                for (int angle : angles) {
                    std::optional<glm::dvec3> v = CalculateJumpVectorForAngle(
                        breeze, targetPos, maxJumpVelocity, angle, false);
                    // MC adds jump-boost lift here; no status effects exist.
                    if (v) return v;
                }
                return std::nullopt;
            }
        };

        // ── ShootWhenStuck (MC monster/breeze/ShootWhenStuck) ──────────────

        class ShootWhenStuck : public Behavior {
        public:
            ShootWhenStuck()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::BreezeJumpInhaling, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeJumpTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeShoot, MemoryStatus::ValueAbsent } }) {}
            const char* DebugString() const override { return "BreezeShootWhenStuck"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                // MC: isPassenger() || isInWater() || has LEVITATION. Riding
                // and status effects do not exist, so water is the reachable
                // clause.
                return body.IsInWater();
            }

            void Start(EntityLevel&, LivingEntity& body, int64_t) override {
                if (Brain* brain = body.GetBrain()) {
                    brain->SetMemoryWithExpiry(MemoryModule::BreezeShoot,
                                               std::monostate{}, 60);
                }
            }
        };

        // ── Slide (MC monster/breeze/Slide) ────────────────────────────────

        class Slide : public Behavior {
        public:
            Slide()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
                             { MemoryModule::WalkTarget, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeJumpCooldown, MemoryStatus::ValueAbsent },
                             { MemoryModule::BreezeShoot, MemoryStatus::ValueAbsent } }) {}
            const char* DebugString() const override { return "BreezeSlide"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
                return body.onGround && !body.IsInWater()
                    && body.GetPose() == Pose::Standing;
            }

            void Start(EntityLevel& level, LivingEntity& body, int64_t) override {
                auto* breeze = dynamic_cast<Breeze*>(&body);
                Brain* brain = body.GetBrain();
                LivingEntity* enemy = GetAttackTarget(body);
                if (!breeze || !brain || !enemy) return;

                std::optional<glm::dvec3> position;
                // Inside the inner ring: flee 5 blocks, but only to a spot the
                // breeze can see that is FURTHER from the enemy than it already
                // is — otherwise fall through to the ring picks.
                if (breeze->WithinInnerCircleRange(enemy->position)) {
                    std::optional<glm::dvec3> away =
                        RandomPos::GetPosAway(*breeze, 5, 5, enemy->position);
                    if (away && HasLineOfSightToPoint(*breeze, *away)
                        && enemy->DistanceToSqr(away->x, away->y, away->z)
                               > enemy->DistanceToSqr(*breeze)) {
                        position = away;
                    }
                }
                if (!position) {
                    position = level.Random().NextBool()
                                   ? RandomPointBehindTarget(*enemy, level.Random())
                                   : RandomPointInMiddleCircle(*breeze, *enemy,
                                                               level.Random());
                }
                const glm::ivec3 blockPos(
                    static_cast<int>(std::floor(position->x)),
                    static_cast<int>(std::floor(position->y)),
                    static_cast<int>(std::floor(position->z)));
                brain->SetMemory(MemoryModule::WalkTarget,
                                 WalkTarget(blockPos, 0.6f, 1));
            }

        private:
            // MC Slide.randomPointInMiddleCircle — pull the enemy direction in
            // to 4–8 blocks short of it.
            static glm::dvec3 RandomPointInMiddleCircle(const Breeze& breeze,
                                                        const LivingEntity& enemy,
                                                        JavaRandom& rnd) {
                const glm::dvec3 direction = enemy.position - breeze.position;
                const double len = std::sqrt(direction.x * direction.x
                                             + direction.y * direction.y
                                             + direction.z * direction.z);
                const double distance = len - Mth::Lerp(rnd.NextDouble(), 8.0, 4.0);
                if (len < 1.0e-6) return breeze.position;
                return breeze.position + direction / len * distance;
            }
        };

        // ── StopAttacking (MC Sensor.wasEntityAttackableLastNTicks(100)) ───
        //
        // The breeze drops its target only after 100 straight ticks of it
        // being un-attackable (dead, out of follow range, or out of sight).
        // MC builds this from a remembered-timestamp predicate; the timestamp
        // lives here.
        class StopAttackingIfNotSeen : public Behavior {
        public:
            static constexpr int kTicksToRememberSeenTarget = 100;

            StopAttackingIfNotSeen()
                : Behavior({ { MemoryModule::AttackTarget, MemoryStatus::ValuePresent } },
                           1) {}
            const char* DebugString() const override { return "BreezeStopAttacking"; }

        protected:
            bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return false;

                auto* target = dynamic_cast<LivingEntity*>(
                    brain->GetEntity(MemoryModule::AttackTarget));
                const int64_t now = level.GetGameTime();

                bool attackableNow = false;
                if (target && target->IsAlive() && mob->CanAttack(*target)) {
                    const double range = mob->GetAttributeValue(Attribute::FollowRange);
                    attackableNow = mob->DistanceToSqr(*target) <= range * range
                                    && mob->GetSensing().HasLineOfSight(*target);
                }
                if (attackableNow) m_lastAttackableTime = now;

                if (!target || !target->IsAlive()
                    || now - m_lastAttackableTime > kTicksToRememberSeenTarget) {
                    brain->EraseMemory(MemoryModule::AttackTarget);
                }
                return true;
            }

        private:
            int64_t m_lastAttackableTime = 0;
        };

        // ── BreezeAttackEntitySensor (MC ai/sensing) ───────────────────────
        //
        // The first NEAREST_LIVING_ENTITIES entry that is attackable — alive,
        // visible, a valid breeze target and not creative/spectator.
        class BreezeAttackEntitySensor : public Sensor {
        public:
            std::vector<MemoryModule> Requires() const override {
                return { MemoryModule::NearestAttackable };
            }

        protected:
            void DoTick(EntityLevel&, LivingEntity& body) override {
                auto* mob = dynamic_cast<Mob*>(&body);
                Brain* brain = body.GetBrain();
                if (!mob || !brain) return;

                LivingEntity* found = nullptr;
                if (const std::vector<Entity*>* nearest =
                        brain->GetEntityList(MemoryModule::NearestLivingEntities)) {
                    for (Entity* e : *nearest) {
                        auto* living = dynamic_cast<LivingEntity*>(e);
                        if (!living || !living->IsAlive()) continue;
                        if (living->IsCreative() || living->IsSpectator()) continue;
                        if (!mob->CanAttack(*living)) continue;
                        if (!mob->GetSensing().HasLineOfSight(*living)) continue;
                        found = living;
                        break;
                    }
                }
                if (found) brain->SetMemory(MemoryModule::NearestAttackable, found);
                else       brain->EraseMemory(MemoryModule::NearestAttackable);
            }
        };

    } // namespace

    void BreezeAi::InitBrain(Breeze& breeze, Brain& brain) {
        (void)breeze;

        // MC BreezeAi.MEMORY_TYPES.
        for (MemoryModule m : { MemoryModule::LookTarget,
                                MemoryModule::NearestVisibleLivingEntities,
                                MemoryModule::NearestAttackable,
                                MemoryModule::CantReachWalkTargetSince,
                                MemoryModule::AttackTarget,
                                MemoryModule::WalkTarget,
                                MemoryModule::BreezeJumpCooldown,
                                MemoryModule::BreezeJumpInhaling,
                                MemoryModule::BreezeShoot,
                                MemoryModule::BreezeShootCharging,
                                MemoryModule::BreezeShootRecovering,
                                MemoryModule::BreezeShootCooldown,
                                MemoryModule::BreezeJumpTarget,
                                MemoryModule::BreezeLeavingWater,
                                MemoryModule::HurtBy,
                                MemoryModule::HurtByEntity,
                                MemoryModule::Path }) {
            brain.RegisterMemory(m);
        }

        brain.AddSensor(std::make_unique<NearestLivingEntitySensor>());
        brain.AddSensor(std::make_unique<HurtBySensor>());
        brain.AddSensor(std::make_unique<PlayerSensor>());
        brain.AddSensor(std::make_unique<BreezeAttackEntitySensor>());

        // ── CORE ───────────────────────────────────────────────────────────
        // No MoveToTargetSink here — SlideToTargetSink in IDLE is the mover,
        // and putting it there is what turns walking into the pose SLIDING.
        std::vector<BehaviorPtr> core;
        core.push_back(std::make_unique<Swim>(0.8f));
        core.push_back(std::make_unique<LookAtTargetSink>(45, 90));
        brain.AddActivity(Activity::Core, 0, std::move(core));

        // ── IDLE ───────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> idle;
        idle.push_back(std::make_unique<StartAttacking>(
            nullptr,
            [](Mob& mob) -> LivingEntity* {
                Brain* b = mob.GetBrain();
                return b ? dynamic_cast<LivingEntity*>(
                               b->GetEntity(MemoryModule::NearestAttackable))
                         : nullptr;
            }));
        idle.push_back(std::make_unique<StartAttacking>(
            nullptr,
            [](Mob& mob) -> LivingEntity* {
                auto* breeze = dynamic_cast<Breeze*>(&mob);
                return breeze ? breeze->GetHurtBy() : nullptr;
            }));
        idle.push_back(std::make_unique<SlideToTargetSink>(20, 40));
        std::vector<GateBehavior::Entry> idleGate;
        idleGate.push_back({ std::make_unique<DoNothing>(20, 100), 1 });
        idleGate.push_back({ RandomStroll::Stroll(0.6f), 2 });
        idle.push_back(MakeRunOne(std::move(idleGate)));
        brain.AddActivity(Activity::Idle, 0, std::move(idle));

        // ── FIGHT ──────────────────────────────────────────────────────────
        std::vector<BehaviorPtr> fight;
        fight.push_back(std::make_unique<StopAttackingIfNotSeen>());
        fight.push_back(std::make_unique<Shoot>());
        fight.push_back(std::make_unique<LongJump>());
        fight.push_back(std::make_unique<ShootWhenStuck>());
        fight.push_back(std::make_unique<Slide>());
        brain.AddActivityWithConditions(
            Activity::Fight, 0, std::move(fight),
            { MemoryCondition{ MemoryModule::AttackTarget, MemoryStatus::ValuePresent },
              MemoryCondition{ MemoryModule::WalkTarget, MemoryStatus::ValueAbsent } });

        brain.SetCoreActivities({ Activity::Core });
        brain.SetDefaultActivity(Activity::Fight);
        brain.UseDefaultActivity();
    }

    void BreezeAi::UpdateActivity(Breeze& breeze) {
        if (Brain* brain = breeze.GetBrain()) {
            brain->SetActiveActivityToFirstValid({ Activity::Fight, Activity::Idle });
        }
    }

} // namespace Game
