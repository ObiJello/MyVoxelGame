// File: src/common/entity/mobs/TwilightHostiles.cpp
#include "common/entity/mobs/TwilightHostiles.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    // ══ Factory ════════════════════════════════════════════════════════════

    std::unique_ptr<Mob> MakeTwilightHostile(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::HedgeSpider:         return std::make_unique<HedgeSpider>(level);
            case EntityTypeId::SwarmSpider:         return std::make_unique<SwarmSpider>(level);
            case EntityTypeId::Wraith:              return std::make_unique<Wraith>(level);
            case EntityTypeId::FireBeetle:          return std::make_unique<FireBeetle>(level);
            case EntityTypeId::SlimeBeetle:         return std::make_unique<SlimeBeetle>(level);
            case EntityTypeId::PinchBeetle:         return std::make_unique<PinchBeetle>(level);
            case EntityTypeId::HelmetCrab:          return std::make_unique<HelmetCrab>(level);
            case EntityTypeId::Troll:               return std::make_unique<Troll>(level);
            case EntityTypeId::TowerwoodBorer:      return std::make_unique<TowerwoodBorer>(level);
            case EntityTypeId::MazeSlime:           return std::make_unique<MazeSlime>(level);
            case EntityTypeId::Minotaur:            return std::make_unique<Minotaur>(level);
            case EntityTypeId::RedcapSapper:        return std::make_unique<RedcapSapper>(level);
            case EntityTypeId::BlockAndChainGoblin: return std::make_unique<BlockChainGoblin>(level);
            case EntityTypeId::UpperGoblinKnight:   return std::make_unique<UpperGoblinKnight>(level);
            case EntityTypeId::LowerGoblinKnight:   return std::make_unique<LowerGoblinKnight>(level);
            default:                                return nullptr;
        }
    }

    namespace {

        // Attribute modifier ids private to this file (outside the engine's
        // ModifierId table, which names only vanilla modifiers).
        constexpr uint32_t kRockFollowBoostId  = 0x54460001;   // Troll ROCK_MODIFIER
        constexpr uint32_t kUpperArmorBoostId  = 0x54460002;   // UpperGoblinKnight ARMOR_MODIFIER
        constexpr uint32_t kSpearAttackBoostId = 0x54460003;   // UpperGoblinKnight DAMAGE_MODIFIER
        constexpr uint32_t kLowerArmorBoostId  = 0x54460004;   // LowerGoblinKnight ARMOR_MODIFIER
        constexpr uint32_t kMazeSlimeHealthId  = 0x54460005;   // MazeSlime DOUBLE_HEALTH

        void SetModifier(AttributeMap& attrs, Attribute attr, uint32_t id, double amount,
                         AttributeOperation op, bool on) {
            const ModifierId mid = static_cast<ModifierId>(id);
            if (attrs.HasModifier(attr, mid)) attrs.RemoveModifier(attr, mid);
            if (on) attrs.AddModifier(attr, AttributeModifier{ id, amount, op });
        }

        // A block's data-pack tag membership, by the block's registry slug.
        bool BlockHasTag(BlockID id, const char* tag) {
            if (id == BlockID::Air) return false;
            const Block& def = BlockRegistry::Get(id);
            if (def.registrySlug.empty()) return false;
            const std::vector<std::string>& tags =
                DataTags::TagsFor(DataTags::Registry::Block, def.registrySlug);
            return std::binary_search(tags.begin(), tags.end(), std::string(tag));
        }

        // A TF block resolved by slug — BlockID::Air until the TF block pass
        // adds it, which turns every use into a no-op.
        BlockID TfBlock(const char* slug) {
            return BlockStates::FromSlug(slug).Block();
        }

        // MC EntitySelector.NO_CREATIVE_OR_SPECTATOR.and(LIVING_ENTITY_STILL_ALIVE).
        bool IsFairTarget(const LivingEntity* e) {
            return e && e->IsAlive() && !e->IsCreative() && !e->IsSpectator();
        }

        // MC AnimationUtils.bobArms is applied by the renderer; nothing here.

        // Entity.java's attachment rotation (Vec3.yRot(-yRot)) — the same
        // formula Entity.cpp applies to the passenger attachment table.
        glm::dvec3 RotateY(const glm::dvec3& p, float yRotDeg) {
            const double a = -static_cast<double>(yRotDeg) * Mth::kDegToRad;
            const double c = std::cos(a), s = std::sin(a);
            return glm::dvec3(p.x * c + p.z * s, p.y, p.z * c - p.x * s);
        }

        // Slab test of the segment [from, to] against a box: the entry
        // fraction, or false when it misses (MC AABB.clip).
        bool ClipBox(const glm::dvec3& lo, const glm::dvec3& hi,
                     const glm::dvec3& from, const glm::dvec3& to, double& tOut) {
            double tMin = 0.0, tMax = 1.0;
            const glm::dvec3 d = to - from;
            for (int a = 0; a < 3; ++a) {
                if (std::abs(d[a]) < 1e-12) {
                    if (from[a] < lo[a] || from[a] > hi[a]) return false;
                    continue;
                }
                double t0 = (lo[a] - from[a]) / d[a];
                double t1 = (hi[a] - from[a]) / d[a];
                if (t0 > t1) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMin > tMax) return false;
            }
            tOut = tMin;
            return true;
        }

        // TF Redcap.registerGoals minus the TNT goals — the redcap sapper's
        // inherited set (Redcap is read here, not shared: TwilightMobs owns
        // the class).
        void AddRedcapGoals(PathfinderMob* mob, GoalSelector& goals, GoalSelector& targets) {
            goals.AddGoal(0, std::make_unique<FloatGoal>(mob));
            goals.AddGoal(5, std::make_unique<MeleeAttackGoal>(mob, 1.0, false));
            goals.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(mob, 1.0));
            goals.AddGoal(7, std::make_unique<LookAtPlayerGoal>(mob, 8.0f));
            goals.AddGoal(7, std::make_unique<RandomLookAroundGoal>(mob));
            targets.AddGoal(1, std::make_unique<HurtByTargetGoal>(mob));
            targets.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(mob, true));
        }

    } // namespace

    // ══ TF goals with no engine counterpart ════════════════════════════════

    namespace {

        // TF ai.control.NoClipMoveControl — steers by velocity nudges with a
        // 2-6 tick course-change cooldown, through blocks.
        class NoClipMoveControl : public MoveControl {
        public:
            explicit NoClipMoveControl(Mob* mob) : MoveControl(mob) {}

            void Tick() override {
                if (m_operation != Operation::MoveTo) return;
                const double dx = m_wantedX - m_mob->position.x;
                const double dy = m_wantedY - m_mob->position.y;
                const double dz = m_wantedZ - m_mob->position.z;
                double dist = dx * dx + dy * dy + dz * dz;
                if (m_courseChangeCooldown-- <= 0) {
                    m_courseChangeCooldown += m_mob->Level()->Random().NextInt(5) + 2;
                    dist = std::sqrt(static_cast<float>(dist));
                    if (dist < 1e-7) return;
                    m_mob->velocity += glm::dvec3(dx / dist * 0.1, dy / dist * 0.1, dz / dist * 0.1) *
                                       m_speedModifier;
                    m_mob->needsSync = true;
                }
            }

        private:
            int m_courseChangeCooldown = 0;
        };

        // TF SimplifiedAttackGoal — melee for a mob with no navigation: swing
        // whenever the target is in reach and in sight, every 20 ticks.
        class SimplifiedAttackGoal : public Goal {
        public:
            explicit SimplifiedAttackGoal(Mob* mob) : m_mob(mob) {}

            bool CanUse() override {
                LivingEntity* target = m_mob->GetTarget();
                return target && m_mob->IsWithinMeleeAttackRange(*target);
            }
            bool RequiresUpdateEveryTick() const override { return true; }
            void Start() override { m_attackTick = 0; }
            void Stop() override { m_attackTick = 0; }
            void Tick() override {
                if (m_attackTick > 0) {
                    --m_attackTick;
                    return;
                }
                LivingEntity* target = m_mob->GetTarget();
                if (!target) return;
                if (m_mob->IsWithinMeleeAttackRange(*target) &&
                    m_mob->GetSensing().HasLineOfSight(*target)) {
                    m_attackTick = AdjustedTickDelay(20);
                    m_mob->Swing();
                    m_mob->DoHurtTarget(*target);
                }
            }
            const char* Name() const override { return "SimplifiedAttackGoal"; }

        private:
            Mob* m_mob;
            int  m_attackTick = 0;
        };

        // TF Wraith.FlyTowardsTargetGoal.
        class WraithFlyTowardsTargetGoal : public Goal {
        public:
            explicit WraithFlyTowardsTargetGoal(Mob* mob) : m_mob(mob) {
                SetFlags(static_cast<uint8_t>(GoalFlag::Move));
            }
            bool CanUse() override { return m_mob->GetTarget() != nullptr; }
            bool CanContinueToUse() override { return false; }
            void Start() override {
                if (LivingEntity* t = m_mob->GetTarget()) {
                    m_mob->GetMoveControl().SetWantedPosition(t->position.x, t->position.y,
                                                              t->position.z, 0.5);
                }
            }
            const char* Name() const override { return "WraithFlyTowardsTargetGoal"; }

        private:
            Mob* m_mob;
        };

        // TF Wraith.RandomFloatAroundGoal — [VanillaCopy] Ghast's, idle only
        // inside the home area.
        class WraithRandomFloatAroundGoal : public Goal {
        public:
            explicit WraithRandomFloatAroundGoal(Mob* mob) : m_mob(mob) {
                SetFlags(static_cast<uint8_t>(GoalFlag::Move));
            }
            bool CanUse() override {
                if (m_mob->GetTarget() || !m_mob->IsWithinHome()) return false;
                MoveControl& c = m_mob->GetMoveControl();
                const double dx = c.GetWantedX() - m_mob->position.x;
                const double dy = c.GetWantedY() - m_mob->position.y;
                const double dz = c.GetWantedZ() - m_mob->position.z;
                const double d = dx * dx + dy * dy + dz * dz;
                return d < 1.0 || d > 3600.0;
            }
            bool CanContinueToUse() override { return false; }
            void Start() override {
                JavaRandom& r = m_mob->Level()->Random();
                const double x = m_mob->position.x + (r.NextFloat() * 2.0f - 1.0f) * 16.0f;
                const double y = m_mob->position.y + (r.NextFloat() * 2.0f - 1.0f) * 16.0f;
                const double z = m_mob->position.z + (r.NextFloat() * 2.0f - 1.0f) * 16.0f;
                m_mob->GetMoveControl().SetWantedPosition(x, y, z, 0.5);
            }
            const char* Name() const override { return "WraithRandomFloatAroundGoal"; }

        private:
            Mob* m_mob;
        };

        // TF Wraith.MoveTowardsHomeGoal — a random cell beside home when out
        // of the home area and idle.
        class WraithMoveTowardsHomeGoal : public Goal {
        public:
            WraithMoveTowardsHomeGoal(Mob* mob, double speed) : m_mob(mob), m_speed(speed) {
                SetFlags(static_cast<uint8_t>(GoalFlag::Move));
            }
            bool CanUse() override {
                if (m_mob->IsWithinHome() || m_mob->GetTarget()) return false;
                JavaRandom& r = m_mob->Level()->Random();
                // BlockPos.relative(Direction.getRandom) then offset(0-4 each).
                static const glm::ivec3 kDirs[6] = {
                    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                glm::ivec3 p = m_mob->GetHomePosition() + kDirs[r.NextInt(6)];
                p.x += r.NextInt(5);
                p.y += r.NextInt(5);
                p.z += r.NextInt(5);
                m_wanted = glm::dvec3(p);
                return true;
            }
            bool CanContinueToUse() override { return false; }
            void Start() override {
                m_mob->GetMoveControl().SetWantedPosition(m_wanted.x, m_wanted.y, m_wanted.z, m_speed);
            }
            const char* Name() const override { return "WraithMoveTowardsHomeGoal"; }

        private:
            Mob*       m_mob;
            double     m_speed;
            glm::dvec3 m_wanted{0.0};
        };

        // TF ai.goal.BreathAttackGoal<T extends Mob & IBreathAttacker>.
        class BreathAttackGoal : public Goal {
        public:
            BreathAttackGoal(Mob* host, BreathAttacker* breather, float range, int time,
                             float chance)
                : m_host(host), m_breather(breather), m_breathRange(range),
                  m_maxDuration(time), m_attackChance(chance) {
                SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
            }

            bool CanUse() override {
                m_attackTarget = dynamic_cast<LivingEntity*>(m_host->GetLastHurtByMob());
                if (!m_attackTarget ||
                    m_host->DistanceTo(*m_attackTarget) > m_breathRange ||
                    !m_host->GetSensing().HasLineOfSight(*m_attackTarget) ||
                    !IsFairTarget(m_attackTarget)) {
                    return false;
                }
                m_breathPos = m_attackTarget->GetEyePosition();
                return m_host->Level()->Random().NextFloat() < m_attackChance;
            }

            void Start() override {
                m_durationLeft = m_maxDuration;
                m_breather->SetBreathing(true);
            }

            bool CanContinueToUse() override {
                return m_durationLeft > 0 && m_host->IsAlive() && m_attackTarget &&
                       m_attackTarget->IsAlive() &&
                       m_host->DistanceTo(*m_attackTarget) <= m_breathRange &&
                       m_host->GetSensing().HasLineOfSight(*m_attackTarget) &&
                       IsFairTarget(m_attackTarget);
            }

            void Tick() override {
                --m_durationLeft;
                m_host->GetLookControl().SetLookAt(m_breathPos);
                FaceVec(m_breathPos, 100.0f, 100.0f);
                if (m_maxDuration - m_durationLeft > 5) {
                    if (Entity* target = GetHeadLookTarget()) {
                        m_breather->DoBreathAttack(*target);
                    }
                }
            }

            void Stop() override {
                m_durationLeft = 0;
                m_attackTarget = nullptr;
                m_breather->SetBreathing(false);
            }

            void ClearReferenceTo(const Entity* e) override {
                if (m_attackTarget == e) m_attackTarget = nullptr;
            }
            const char* Name() const override { return "BreathAttackGoal"; }

        private:
            // BreathAttackGoal.getHeadLookTarget — the nearest pickable living
            // thing along a 30-block ray from 0.25 above the feet, among
            // those in the box pushed 3 blocks along the look and inflated
            // 0.5.
            Entity* GetHeadLookTarget() {
                EntityLevel* level = m_host->Level();
                const glm::dvec3 src(m_host->position.x, m_host->position.y + 0.25,
                                     m_host->position.z);
                const glm::vec3 look = Mth::ViewVector(m_host->xRot, m_host->yRot);
                const glm::dvec3 lookD(look);
                const glm::dvec3 dest = src + lookD * 30.0;
                AABBd bb = m_host->GetAABBd();
                bb.min += lookD * 3.0 - glm::dvec3(0.5);
                bb.max += lookD * 3.0 + glm::dvec3(0.5);
                std::vector<Entity*> found;
                level->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(bb.min), glm::vec3(bb.max)),
                                        m_host, found);
                std::vector<LivingEntity*> players;
                level->GetPlayers(players);
                for (LivingEntity* p : players) {
                    const AABBd pb = p->GetAABBd();
                    if (pb.min.x < bb.max.x && pb.max.x > bb.min.x && pb.min.y < bb.max.y &&
                        pb.max.y > bb.min.y && pb.min.z < bb.max.z && pb.max.z > bb.min.z &&
                        std::find(found.begin(), found.end(), p) == found.end()) {
                        found.push_back(p);
                    }
                }
                Entity* pointed = nullptr;
                double hitDist = 0.0;
                for (Entity* e : found) {
                    auto* living = e ? e->AsLiving() : nullptr;
                    if (!living || living == m_host || !IsFairTarget(living)) continue;
                    const AABBd box = living->GetAABBd();
                    double t = 0.0;
                    const bool inside = src.x >= box.min.x && src.x <= box.max.x &&
                                        src.y >= box.min.y && src.y <= box.max.y &&
                                        src.z >= box.min.z && src.z <= box.max.z;
                    if (inside) {
                        pointed = living;
                        hitDist = 0.0;
                    } else if (ClipBox(box.min, box.max, src, dest, t)) {
                        const double d = t * 30.0;
                        if (d < hitDist || hitDist == 0.0) {
                            pointed = living;
                            hitDist = d;
                        }
                    }
                }
                return pointed;
            }

            // BreathAttackGoal.faceVec.
            void FaceVec(const glm::dvec3& pos, float yawConstraint, float pitchConstraint) {
                const double xOff = pos.x - m_host->position.x;
                const double zOff = pos.z - m_host->position.z;
                const double yOff = (m_host->position.y + 0.25) - pos.y;
                const double distance = std::sqrt(static_cast<float>(xOff * xOff + zOff * zOff));
                const float xyAngle = static_cast<float>(std::atan2(zOff, xOff) * 180.0 / Mth::kPi) - 90.0f;
                const float zdAngle = static_cast<float>(-(std::atan2(yOff, distance) * 180.0 / Mth::kPi));
                m_host->xRot = -UpdateRotation(m_host->xRot, zdAngle, pitchConstraint);
                m_host->yRot = UpdateRotation(m_host->yRot, xyAngle, yawConstraint);
            }
            static float UpdateRotation(float current, float target, float maxDelta) {
                const float delta = std::clamp(Mth::WrapDegrees(target - current), -maxDelta, maxDelta);
                return current + delta;
            }

            Mob*            m_host;
            BreathAttacker* m_breather;
            LivingEntity*   m_attackTarget = nullptr;
            glm::dvec3      m_breathPos{0.0};
            float           m_breathRange;
            int             m_maxDuration;
            float           m_attackChance;
            int             m_durationLeft = 0;
        };

        // TF ai.goal.ChargeAttackGoal — the minotaur's and pinch beetle's
        // sprint through the target: on the ground, 4-8 blocks away, in
        // sight, 1 in 10; a 15-44 tick wind-up (legs racing in place), then a
        // path to a point 2.1 blocks past the target, striking once when in
        // reach. `canBreak` (the minoshroom's block smashing) is never set
        // by the mobs here.
        class ChargeAttackGoal : public Goal {
        public:
            ChargeAttackGoal(PathfinderMob* mob, TFCharger* charger, float speed)
                : m_charger(mob), m_chargeFlag(charger), m_speed(speed) {
                SetFlags(GoalFlag::Move | GoalFlag::Look);
            }

            bool CanUse() override {
                m_chargeTarget = m_charger->GetTarget();
                if (!m_chargeTarget) return false;
                const double distance = m_charger->DistanceToSqr(*m_chargeTarget);
                if (distance < kMinRangeSq || distance > kMaxRangeSq) return false;
                if (!m_charger->onGround) return false;
                m_chargePos = FindChargePoint(*m_charger, *m_chargeTarget);
                if (!m_charger->GetSensing().HasLineOfSight(*m_chargeTarget)) return false;
                return m_charger->Level()->Random().NextInt(kFreq) == 0;
            }

            void Start() override {
                m_windup = 15 + m_charger->Level()->Random().NextInt(30);
                m_charger->SetSprinting(true);
            }

            bool CanContinueToUse() override {
                return m_chargeTarget && (m_windup > 0 || !m_charger->GetNavigation().IsDone());
            }

            void Tick() override {
                if (!m_chargeTarget) return;
                m_charger->GetLookControl().SetLookAt(m_chargePos.x, m_chargePos.y - 1.0,
                                                      m_chargePos.z, 10.0f,
                                                      static_cast<float>(m_charger->GetMaxHeadXRot()));
                if (m_windup > 0) {
                    if (--m_windup == 0) {
                        m_charger->GetNavigation().MoveTo(m_chargePos.x, m_chargePos.y,
                                                          m_chargePos.z, m_speed);
                    } else {
                        m_charger->walkAnimation.SetSpeed(m_charger->walkAnimation.speed + 0.8f);
                        if (m_chargeFlag) m_chargeFlag->SetCharging(true);
                    }
                }
                const float w = m_charger->GetBbWidth();
                const double rangeSq = w * 2.0f * w * 2.0f + m_chargeTarget->GetBbWidth();
                if (m_charger->DistanceToSqr(m_chargeTarget->position.x,
                                             m_chargeTarget->position.y,
                                             m_chargeTarget->position.z) <= rangeSq &&
                    !m_hasAttacked) {
                    m_hasAttacked = true;
                    m_charger->DoHurtTarget(*m_chargeTarget);
                }
            }

            void Stop() override {
                m_windup = 0;
                m_chargeTarget = nullptr;
                m_hasAttacked = false;
                m_charger->SetSprinting(false);
                if (m_chargeFlag) m_chargeFlag->SetCharging(false);
            }

            void ClearReferenceTo(const Entity* e) override {
                if (m_chargeTarget == e) m_chargeTarget = nullptr;
            }
            const char* Name() const override { return "ChargeAttackGoal"; }

        private:
            static constexpr double kMinRangeSq = 16.0;
            static constexpr double kMaxRangeSq = 64.0;
            static constexpr int    kFreq = 10;

            static glm::dvec3 FindChargePoint(const Entity& attacker, const Entity& target) {
                const double vx = target.position.x - attacker.position.x;
                const double vz = target.position.z - attacker.position.z;
                const float angle = static_cast<float>(std::atan2(vz, vx));
                const double distance = std::sqrt(static_cast<float>(vx * vx + vz * vz));
                constexpr double kOvershoot = 2.1;
                return glm::dvec3(attacker.position.x + std::cos(angle) * (distance + kOvershoot),
                                  target.position.y,
                                  attacker.position.z + std::sin(angle) * (distance + kOvershoot));
            }

            PathfinderMob* m_charger;
            TFCharger*     m_chargeFlag;
            float          m_speed;
            LivingEntity*  m_chargeTarget = nullptr;
            glm::dvec3     m_chargePos{0.0};
            int            m_windup = 0;
            bool           m_hasAttacked = false;
        };

        // TF ai.goal.AlwaysWatchTargetGoal.
        class AlwaysWatchTargetGoal : public Goal {
        public:
            explicit AlwaysWatchTargetGoal(Mob* mob) : m_mob(mob) {}
            bool CanUse() override { return m_mob->GetTarget() != nullptr; }
            bool RequiresUpdateEveryTick() const override { return true; }
            void Tick() override {
                if (LivingEntity* t = m_mob->GetTarget()) {
                    m_mob->GetLookControl().SetLookAt(t->position.x, t->GetEyeY(), t->position.z,
                                                      100.0f, 100.0f);
                }
            }
            const char* Name() const override { return "AlwaysWatchTargetGoal"; }

        private:
            Mob* m_mob;
        };

        // TF HurtByTargetGoal(this, Troll.class) — MC's toIgnoreDamage: a
        // troll hit by another troll does not retaliate.
        class TrollHurtByTargetGoal : public HurtByTargetGoal {
        public:
            explicit TrollHurtByTargetGoal(Mob* mob) : HurtByTargetGoal(mob) {}
            bool CanUse() override {
                Entity* attacker = m_mob->GetLastHurtByMob();
                if (attacker && attacker->GetType() == EntityTypeId::Troll) return false;
                return HurtByTargetGoal::CanUse();
            }
        };

        // TF ThrowSpikeBlockGoal: target within sqrt(42), in sight, 1 in 56,
        // then a 100-199 tick cooldown; runs while the chain is out.
        class ThrowSpikeBlockGoal : public Goal {
        public:
            explicit ThrowSpikeBlockGoal(BlockChainGoblin* goblin) : m_goblin(goblin) {
                SetFlags(GoalFlag::Move | GoalFlag::Look);
            }
            bool CanUse() override {
                LivingEntity* target = m_goblin->GetTarget();
                if (!target || m_goblin->DistanceToSqr(*target) > 42.0 || m_cooldown > 0) {
                    --m_cooldown;
                    return false;
                }
                return m_goblin->IsAlive() && m_goblin->GetSensing().HasLineOfSight(*target) &&
                       m_goblin->Level()->Random().NextInt(56) == 0;
            }
            bool CanContinueToUse() override { return m_goblin->GetChainMoveLength() > 0.0f; }
            void Start() override {
                m_goblin->SetThrowing(true);
                m_cooldown = 100 + m_goblin->Level()->Random().NextInt(100);
            }
            const char* Name() const override { return "ThrowSpikeBlockGoal"; }

        private:
            BlockChainGoblin* m_goblin;
            int m_cooldown = 0;
        };

        // UpperGoblinKnight's MeleeAttackGoal subclass: idle during a slam.
        class UpperKnightMeleeGoal : public MeleeAttackGoal {
        public:
            explicit UpperKnightMeleeGoal(UpperGoblinKnight* knight)
                : MeleeAttackGoal(knight, 1.0, false), m_knight(knight) {}
            bool CanUse() override {
                return m_knight->GetHeavySpearTimer() <= 0 && MeleeAttackGoal::CanUse();
            }

        private:
            UpperGoblinKnight* m_knight;
        };

        // LowerGoblinKnight's MeleeAttackGoal subclass: idle while its rider
        // slams.
        class LowerKnightMeleeGoal : public MeleeAttackGoal {
        public:
            explicit LowerKnightMeleeGoal(LowerGoblinKnight* knight)
                : MeleeAttackGoal(knight, 1.0, false), m_knight(knight) {}
            bool CanUse() override {
                const UpperGoblinKnight* upper = m_knight->GetUpper();
                if (upper && upper->GetHeavySpearTimer() > 0) return false;
                return MeleeAttackGoal::CanUse();
            }

        private:
            LowerGoblinKnight* m_knight;
        };

        // TF HeavySpearAttackGoal — holds MOVE/LOOK for the slam and lands it
        // at timer 25.
        class HeavySpearAttackGoal : public Goal {
        public:
            explicit HeavySpearAttackGoal(UpperGoblinKnight* knight) : m_knight(knight) {
                SetFlags(GoalFlag::Move | GoalFlag::Look);
            }
            bool RequiresUpdateEveryTick() const override { return true; }
            bool CanUse() override {
                const int timer = m_knight->GetHeavySpearTimer();
                return timer > 0 && timer < UpperGoblinKnight::kHeavySpearTimerStart &&
                       IsFairTarget(m_knight->GetTarget());
            }
            void Tick() override {
                if (m_knight->GetHeavySpearTimer() == 25) m_knight->LandHeavySpearAttack();
            }
            const char* Name() const override { return "HeavySpearAttackGoal"; }

        private:
            UpperGoblinKnight* m_knight;
        };

        // TF RiderSpearAttackGoal — the mount holds still (MOVE/LOOK) while
        // its rider's slam runs.
        class RiderSpearAttackGoal : public Goal {
        public:
            explicit RiderSpearAttackGoal(LowerGoblinKnight* knight) : m_knight(knight) {
                SetFlags(GoalFlag::Move | GoalFlag::Look);
            }
            bool CanUse() override {
                const UpperGoblinKnight* upper = m_knight->GetUpper();
                if (!upper || !IsFairTarget(m_knight->GetTarget())) return false;
                const int timer = upper->GetHeavySpearTimer();
                return timer > 0 && timer < UpperGoblinKnight::kHeavySpearTimerStart;
            }
            const char* Name() const override { return "RiderSpearAttackGoal"; }

        private:
            LowerGoblinKnight* m_knight;
        };

    } // namespace

    // TF TowerwoodBorer.SummonBorersGoal — [VanillaCopy] Silverfish
    // WakeUpFriendsGoal over infested towerwood.
    class SummonBorersGoal : public Goal {
    public:
        explicit SummonBorersGoal(TowerwoodBorer* borer) : m_borer(borer) {}

        void NotifyHurt() {
            if (m_lookForFriends == 0) m_lookForFriends = 20;
        }
        bool CanUse() override { return m_lookForFriends > 0; }
        void Tick() override {
            if (--m_lookForFriends > 0) return;
            EntityLevel* level = m_borer->Level();
            const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            const BlockID infested = TfBlock("infested_towerwood");
            if (!blocks || infested == BlockID::Air) return;
            JavaRandom& random = level->Random();
            const glm::ivec3 pos = m_borer->BlockPosition();
            for (int i = 0; i <= 5 && i >= -5; i = (i <= 0 ? 1 : 0) - i) {
                for (int j = 0; j <= 10 && j >= -10; j = (j <= 0 ? 1 : 0) - j) {
                    for (int k = 0; k <= 10 && k >= -10; k = (k <= 0 ? 1 : 0) - k) {
                        const glm::ivec3 p = pos + glm::ivec3(j, i, k);
                        if (blocks->GetBlock(p.x, p.y, p.z) != infested) continue;
                        if (level->MobGriefing()) {
                            level->DestroyBlock(p, true);
                        } else {
                            const BlockID towerwood = TfBlock("towerwood");
                            if (towerwood != BlockID::Air) level->SetBlock(p, towerwood);
                        }
                        if (random.NextBool()) return;
                    }
                }
            }
        }
        const char* Name() const override { return "SummonBorersGoal"; }

    private:
        TowerwoodBorer* m_borer;
        int m_lookForFriends = 0;
    };

    namespace {

        // TF TowerwoodBorer.HideInTowerwoodGoal — [VanillaCopy] Silverfish
        // MergeWithStoneGoal over towerwood, with TF's 1-in-5 dig roll.
        class HideInTowerwoodGoal : public RandomStrollGoal {
        public:
            explicit HideInTowerwoodGoal(TowerwoodBorer* borer)
                : RandomStrollGoal(borer, 1.0, 10) {
                SetFlags(static_cast<uint8_t>(GoalFlag::Move));
            }

            bool CanUse() override {
                if (m_mob->GetTarget() || !m_mob->GetNavigation().IsDone()) return false;
                EntityLevel* level = m_mob->Level();
                JavaRandom& random = level->Random();
                const BlockID towerwood = TfBlock("towerwood");
                if (random.NextInt(10) == 0 && level->MobGriefing() && towerwood != BlockID::Air &&
                    level->Blocks()) {
                    m_facing = random.NextInt(6);
                    const glm::ivec3 p = TargetCell();
                    if (level->Blocks()->GetBlock(p.x, p.y, p.z) == towerwood) {
                        m_doMerge = true;
                        return true;
                    }
                }
                m_doMerge = false;
                return RandomStrollGoal::CanUse();
            }
            bool CanContinueToUse() override {
                return !m_doMerge && RandomStrollGoal::CanContinueToUse();
            }
            void Start() override {
                if (!m_doMerge) {
                    RandomStrollGoal::Start();
                    return;
                }
                EntityLevel* level = m_mob->Level();
                const glm::ivec3 p = TargetCell();
                const BlockID towerwood = TfBlock("towerwood");
                const BlockID infested = TfBlock("infested_towerwood");
                if (level->Blocks() && towerwood != BlockID::Air && infested != BlockID::Air &&
                    level->Blocks()->GetBlock(p.x, p.y, p.z) == towerwood &&
                    level->Random().NextInt(5) == 0) {
                    level->SetBlock(p, infested);
                    m_mob->MakePoofParticles();
                    m_mob->Discard();
                }
            }
            const char* Name() const override { return "HideInTowerwoodGoal"; }

        private:
            glm::ivec3 TargetCell() const {
                static const glm::ivec3 kDirs[6] = {
                    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                const glm::ivec3 base(static_cast<int>(std::floor(m_mob->position.x)),
                                      static_cast<int>(std::floor(m_mob->position.y + 0.5)),
                                      static_cast<int>(std::floor(m_mob->position.z)));
                return base + kDirs[m_facing];
            }

            int  m_facing = 0;
            bool m_doMerge = false;
        };

    } // namespace

    // ══ Hedge spider ═══════════════════════════════════════════════════════

    HedgeSpider::HedgeSpider(EntityLevel* level) : HedgeSpider(EntityTypeId::HedgeSpider, level) {
        RegisterGoals();
    }

    HedgeSpider::HedgeSpider(EntityTypeId type, EntityLevel* level) : Spider(type, level) {}

    void HedgeSpider::RegisterGoals() {
        // MC Spider.registerGoals with TF's two swaps (see the header).
        static constexpr EntityTypeId kArmadilloAvoid[] = { EntityTypeId::Armadillo };
        static constexpr EntityTypeId kIronGolemTargets[] = { EntityTypeId::IronGolem };
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(this, kArmadilloAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(3, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        // Replaced: MeleeAttackGoal(this, 1, true) — no daylight truce.
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        // Replaced: NearestAttackableTargetGoal(Player, true), light-blind.
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
        m_targetSelector.AddGoal(3, std::make_unique<SpiderTargetGoal>(this, kIronGolemTargets, 1,
                                                                       true));
    }

    bool HedgeSpider::IsValidLightLevel(EntityLevel& level, const glm::ivec3& pos,
                                        JavaRandom& rng) {
        return Monster::IsDarkEnoughToSpawn(level, pos, rng);
    }

    // ══ Swarm spider ═══════════════════════════════════════════════════════

    void SwarmSpider::CreateAttributes(AttributeMap& out) {
        // TF SwarmSpider.registerAttributes: Spider.createAttributes() +.
        Spider::CreateAttributes(out);
        out.Register(Attribute::MaxHealth,    3.0);
        out.Register(Attribute::AttackDamage, 1.0);
    }

    SwarmSpider::SwarmSpider(EntityLevel* level) : HedgeSpider(EntityTypeId::SwarmSpider, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();   // HedgeSpider's — TF makes the identical swaps
    }

    bool SwarmSpider::DoHurtTarget(Entity& target) {
        // SwarmSpider.doHurtTarget: one bite in four.
        return m_level && m_level->Random().NextInt(4) == 0 && Spider::DoHurtTarget(target);
    }

    std::shared_ptr<SpawnGroupData>
    SwarmSpider::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Spider::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level || m_level->IsClientSide()) return groupData;
        // SwarmSpider.summonJockey: a baby skeleton druid, on the seat the
        // spider jockey's skeleton took or 1 in 200.
        if (GetFirstPassenger() != nullptr || m_level->Random().NextInt(200) == 0) {
            std::unique_ptr<Mob> druid = MakeGenericMob(EntityTypeId::SkeletonDruid, m_level);
            if (druid) {
                druid->position = position;
                druid->yRot = yRot;
                druid->xRot = 0.0f;
                druid->SetBaby(true);
                druid->FinalizeSpawn(SpawnReason::Jockey, nullptr);
                if (IsVehicle()) EjectPassengers();
                Mob* placed = druid.get();
                m_level->AddFreshEntity(std::move(druid));
                placed->StartRiding(*this, /*force=*/true);
            }
        }
        return groupData;
    }

    // ══ Wraith ═════════════════════════════════════════════════════════════

    void Wraith::CreateAttributes(AttributeMap& out) {
        // TF Wraith.registerAttributes (Mob.createMobAttributes).
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,    20.0);
        out.Register(Attribute::MovementSpeed, 0.5);
        out.Register(Attribute::AttackDamage,  5.0);
    }

    Wraith::Wraith(EntityLevel* level) : Mob(EntityTypeId::Wraith, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<NoClipMoveControl>(this));
        RegisterGoals();
    }

    void Wraith::RegisterGoals() {
        // TF Wraith.registerGoals. LookAroundGoal is a [VanillaCopy] of
        // Ghast.GhastLookGoal — the engine's own.
        m_goalSelector.AddGoal(2, std::make_unique<WraithMoveTowardsHomeGoal>(this, 0.85));
        m_goalSelector.AddGoal(4, std::make_unique<SimplifiedAttackGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<WraithFlyTowardsTargetGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<WraithRandomFloatAroundGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<GhastLookGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(this, false));
    }

    void Wraith::Travel(const glm::dvec3& input) {
        // LivingEntity.travelFlying(input, 0.2) with noPhysics: Entity.move
        // is a plain translate for a no-physics entity.
        const float speed = 0.2f;
        MoveRelative(IsInWater() || IsInLava() ? 0.02f : speed, input);
        position += velocity;
        if (IsInWater())      velocity *= 0.8;
        else if (IsInLava())  velocity *= 0.5;
        else                  velocity *= 0.91;
    }

    bool Wraith::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // Wraith.hurtServer: whoever hurt it becomes its target.
        if (!Mob::Hurt(source, amount, attacker)) return false;
        if (attacker && attacker != this && attacker != GetVehicle() && !HasPassenger(*attacker)) {
            auto* living = attacker->AsLiving();
            if (living && !living->IsCreative()) SetTarget(living);
        }
        return true;
    }

    bool Wraith::DoHurtTarget(Entity& target) {
        // Wraith.doHurtTarget: a HAUNT hit for ATTACK_DAMAGE, then the plain
        // melee on top (which the victim's invulnerability window usually
        // swallows).
        if (auto* living = target.AsLiving()) {
            living->Hurt(MobDamageSource::MobAttack,
                         static_cast<float>(GetAttributeValue(Attribute::AttackDamage)), this);
        }
        return Mob::DoHurtTarget(target);
    }

    std::shared_ptr<SpawnGroupData>
    Wraith::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // Wraith.finalizeSpawn: structure/spawner wraiths get their home.
        // (The engine has no STRUCTURE reason yet; chunk generation stands
        // in for it.)
        if (reason == SpawnReason::ChunkGeneration || reason == SpawnReason::Spawner) {
            SetHomeTo(BlockPosition(), kHomeRadius);
        }
        return Mob::FinalizeSpawn(reason, std::move(groupData));
    }

    bool Wraith::CheckSpawnRules(EntityLevel& level, const glm::ivec3& pos, JavaRandom& rng) {
        return level.GetDifficulty() != Difficulty::Peaceful &&
               Monster::IsDarkEnoughToSpawn(level, pos, rng);
    }

    // ══ Fire beetle ════════════════════════════════════════════════════════

    void FireBeetle::CreateAttributes(AttributeMap& out) {
        // TF FireBeetle.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     25.0);
        out.Register(Attribute::MovementSpeed, 0.23);
        out.Register(Attribute::AttackDamage,   4.0);
    }

    FireBeetle::FireBeetle(EntityLevel* level) : Monster(EntityTypeId::FireBeetle, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void FireBeetle::RegisterGoals() {
        // TF FireBeetle.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<BreathAttackGoal>(this, this, 5.0f, 30, 0.1f));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void FireBeetle::DoBreathAttack(Entity& target) {
        // FireBeetle.doBreathAttack: SCORCHED 2, then 10 s of fire.
        auto* living = target.AsLiving();
        if (!living || target.FireImmune()) return;
        if (living->Hurt(MobDamageSource::Fire, kBreathDamage, this)) {
            target.IgniteForSeconds(kBreathDuration);
        }
    }

    bool FireBeetle::DoHurtTarget(Entity& target) {
        // FireBeetle.doHurtTarget: while breathing, the bite is a SCORCHED 2.
        if (m_breathing) {
            auto* living = target.AsLiving();
            return living && living->Hurt(MobDamageSource::Fire, kBreathDamage, this);
        }
        return Monster::DoHurtTarget(target);
    }

    void FireBeetle::AiStep() {
        Monster::AiStep();
        // FireBeetle.aiStep: two flame puffs a tick out of the jaws while
        // breathing — client side (level.addParticle is a server no-op).
        // The engine has no FLAME particle kind; SMOKE carries the stream
        // until one exists.
        if (!m_breathing || !m_level || !m_level->IsClientSide()) return;
        JavaRandom& rnd = m_level->Random();
        const glm::vec3 look = Mth::ViewVector(xRot, yRot);
        constexpr double kDist = 0.9;
        const double px = position.x + look.x * kDist;
        const double py = position.y + 0.25 + look.y * kDist;
        const double pz = position.z + look.z * kDist;
        for (int i = 0; i < 2; ++i) {
            double dx = look.x, dy = look.y, dz = look.z;
            const double spread = 5.0 + rnd.NextDouble() * 2.5;
            const double velocityScale = 0.15 + rnd.NextDouble() * 0.15;
            dx += rnd.NextGaussian() * 0.0075 * spread;
            dy += rnd.NextGaussian() * 0.0075 * spread;
            dz += rnd.NextGaussian() * 0.0075 * spread;
            m_level->AddParticle(ParticleKind::Smoke, px, py, pz, dx * velocityScale,
                                 dy * velocityScale, dz * velocityScale);
        }
    }

    // ══ Slime beetle ═══════════════════════════════════════════════════════

    void SlimeBeetle::CreateAttributes(AttributeMap& out) {
        // TF SlimeBeetle.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     25.0);
        out.Register(Attribute::MovementSpeed, 0.23);
        out.Register(Attribute::AttackDamage,   4.0);
    }

    SlimeBeetle::SlimeBeetle(EntityLevel* level) : Monster(EntityTypeId::SlimeBeetle, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void SlimeBeetle::RegisterGoals() {
        // TF SlimeBeetle.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        // AvoidEntityGoal<>(this, Player.class, 3.0F, 1.25F, 2.0F).
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(this, 3.0f, 1.25, 2.0));
        m_goalSelector.AddGoal(3, std::make_unique<RangedAttackGoal>(this, this, 1.0, 30, 10.0f));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void SlimeBeetle::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_level) return;
        // SlimeBeetle.performRangedAttack: from the thrower's eye (TFThrowable
        // (level, thrower) — ThrowableProjectile's owner position), aimed
        // 1.1 under the target's eye with a 0.2-per-block lob, speed 0.6,
        // inaccuracy 6.
        auto projectile = std::make_unique<SlimeProjectile>(m_level);
        projectile->SetOwnerAndPosition(*this);
        const double tx = target.position.x - position.x;
        const double ty = target.position.y + target.GetEyeHeight() - 1.1 - projectile->position.y;
        const double tz = target.position.z - position.z;
        const float heightOffset = std::sqrt(static_cast<float>(tx * tx + tz * tz)) * 0.2f;
        projectile->Shoot(tx, ty + heightOffset, tz, 0.6f, 6.0f);
        m_level->AddFreshEntity(std::move(projectile));
    }

    void SlimeProjectile::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // SlimeProjectile.onHitEntity: thrown damage 4.
        if (!m_level || m_level->IsClientSide()) return;
        Entity* owner = GetOwner();
        DealHitDamage(target, hit, MobDamageSource::Projectile, 4.0f, owner ? owner : this);
    }

    void SlimeProjectile::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        // SlimeProjectile.die: gone, with the event-3 splat.
        if (m_level && !m_level->IsClientSide()) {
            m_level->BroadcastEntityEvent(*this, 3);
            Discard();
        }
    }

    // ══ Pinch beetle ═══════════════════════════════════════════════════════

    void PinchBeetle::CreateAttributes(AttributeMap& out) {
        // TF PinchBeetle.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     40.0);
        out.Register(Attribute::MovementSpeed, 0.23);
        out.Register(Attribute::AttackDamage,   4.0);
        out.Register(Attribute::Armor,          2.0);
    }

    PinchBeetle::PinchBeetle(EntityLevel* level) : Monster(EntityTypeId::PinchBeetle, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void PinchBeetle::RegisterGoals() {
        // TF PinchBeetle.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<ChargeAttackGoal>(this, nullptr, 1.5f));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    float PinchBeetle::BaseBbWidth() const {
        // PinchBeetle.getDefaultDimensions: 2.2 x 1.6 while holding.
        return IsVehicle() || m_holdingClient ? 2.2f : Monster::BaseBbWidth();
    }

    float PinchBeetle::BaseBbHeight() const {
        return IsVehicle() || m_holdingClient ? 1.6f : Monster::BaseBbHeight();
    }

    float PinchBeetle::BaseEyeHeight() const {
        // The grown box's default eye (1.6 * 0.85).
        return IsVehicle() || m_holdingClient ? 1.6f * 0.85f : Monster::BaseEyeHeight();
    }

    void PinchBeetle::AiStep() {
        Monster::AiStep();
        if (!m_level || m_level->IsClientSide()) return;
        // PinchBeetle.aiStep: the held victim stays the target and is watched.
        Entity* passenger = GetFirstPassenger();
        if (!passenger) return;
        if (passenger->GetVehicle() != this) return;
        GetLookControl().SetLookAt(passenger->position.x, passenger->GetEyeY(), passenger->position.z,
                                   100.0f, 100.0f);
        if (auto* living = passenger->AsLiving()) SetTarget(living);
    }

    bool PinchBeetle::DoHurtTarget(Entity& target) {
        // PinchBeetle.doHurtTarget: with empty jaws, pluck the victim out of
        // whatever it rides and clamp it (a mob — see the header on players).
        if (!IsVehicle()) {
            auto* living = target.AsLiving();
            if (living && !living->IsPlayer() && &target != this) {
                target.StopRiding();
                target.StartRiding(*this, /*force=*/true);
            }
        }
        // CLAMPED: the beetle's melee damage.
        return Monster::DoHurtTarget(target);
    }

    void PinchBeetle::Knockback(double power, double dx, double dz) {
        // PinchBeetle.knockback: no shove while holding something.
        if (!IsVehicle()) Monster::Knockback(power, dx, dz);
    }

    void PinchBeetle::Die(MobDamageSource source, Entity* attacker) {
        EjectPassengers();
        Monster::Die(source, attacker);
    }

    glm::dvec3 PinchBeetle::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // PinchBeetle.getPassengerAttachmentPoint: super + (0, -height + eye,
        // 0.75) — the victim hangs in the jaws at eye level, in front.
        (void)passenger;
        return RotateY(glm::dvec3(0.0, GetEyeHeight(), 0.75), yRot);
    }

    // ══ Helmet crab ════════════════════════════════════════════════════════

    void HelmetCrab::CreateAttributes(AttributeMap& out) {
        // TF HelmetCrab.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     13.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   3.0);
        out.Register(Attribute::Armor,          6.0);
    }

    HelmetCrab::HelmetCrab(EntityLevel* level) : Monster(EntityTypeId::HelmetCrab, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void HelmetCrab::RegisterGoals() {
        // TF HelmetCrab.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<AlwaysWatchTargetGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<LeapAtTargetGoal>(this, 0.28f));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<RandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void HelmetCrab::Tick() {
        Monster::Tick();
        // HelmetCrab.tick — both sides.
        m_helmetRotO = m_helmetRot;
        m_helmetRot = yHeadRotO;
        while (m_helmetRot - m_helmetRotO < -180.0f) m_helmetRotO -= 360.0f;
        while (m_helmetRot - m_helmetRotO >= 180.0f) m_helmetRotO += 360.0f;
    }

    float HelmetCrab::GetHelmetRotation(float partialTick) const {
        const float body = Mth::RotLerp(partialTick, yBodyRotO, yBodyRot);
        const float helmet = Mth::RotLerp(partialTick, m_helmetRotO, m_helmetRot);
        return Mth::WrapDegrees(helmet - body) - 25.0f;
    }

    std::shared_ptr<SpawnGroupData>
    HelmetCrab::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) m_blue = m_level->Random().NextInt(10000) == 0;
        return Monster::FinalizeSpawn(reason, std::move(groupData));
    }

    void HelmetCrab::SaveModNbt(ModNbtOut& out) const { out.Bool("blue", m_blue); }
    void HelmetCrab::LoadModNbt(const ModNbtIn& in) { m_blue = in.Bool("blue", false); }

    // ══ Troll ══════════════════════════════════════════════════════════════

    void Troll::CreateAttributes(AttributeMap& out) {
        // TF Troll.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     30.0);
        out.Register(Attribute::MovementSpeed, 0.25);
        out.Register(Attribute::AttackDamage,   7.0);
    }

    Troll::Troll(EntityLevel* level) : Monster(EntityTypeId::Troll, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        if (level) m_rockCooldown = 300 + level->Random().NextInt(100);
        RegisterGoals();
    }

    void Troll::RegisterGoals() {
        // TF Troll.registerGoals; the attack goal at 4 is setCombatTask's.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<RestrictSunGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<FleeSunGoal>(this, 1.0));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<TrollHurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
        SetCombatTask();
    }

    void Troll::SetCombatTask() {
        // Troll.setCombatTask. Called only from outside the goal tick (see
        // SetHasRock), so removing a goal never frees the running one.
        if (m_meleeGoal)  { m_goalSelector.RemoveGoal(m_meleeGoal);  m_meleeGoal = nullptr; }
        if (m_rangedGoal) { m_goalSelector.RemoveGoal(m_rangedGoal); m_rangedGoal = nullptr; }
        if (m_hasRock) {
            // RangedAttackGoal(this, 1.0D, 20, 60, 15.0F).
            auto goal = std::make_unique<RangedAttackGoal>(this, this, 1.0, 20, 60, 15.0f);
            m_rangedGoal = goal.get();
            m_goalSelector.AddGoal(4, std::move(goal));
        } else {
            // MeleeAttackGoal(this, 1.2D, false).
            auto goal = std::make_unique<MeleeAttackGoal>(this, 1.2, false);
            m_meleeGoal = goal.get();
            m_goalSelector.AddGoal(4, std::move(goal));
        }
    }

    void Troll::SetHasRock(bool rock) {
        // Troll.setHasRock: the flag, the +8 FOLLOW_RANGE while held, and the
        // goal swap. The swap waits for the end of this tick (Troll::Tick):
        // the throw calls this from inside RangedAttackGoal.tick, and
        // removing that goal there would free it mid-call.
        m_hasRock = rock;
        if (m_level && !m_level->IsClientSide()) {
            SetModifier(m_attributes, Attribute::FollowRange, kRockFollowBoostId, 8.0,
                        AttributeOperation::AddValue, rock);
        }
    }

    void Troll::Tick() {
        Monster::Tick();
        if (!m_level || m_level->IsClientSide()) return;

        // Apply a pending combat-task swap (see SetHasRock).
        if ((m_hasRock && !m_rangedGoal) || (!m_hasRock && !m_meleeGoal)) SetCombatTask();

        if (m_hasRock || !GetTarget()) return;
        if (m_rockCooldown > 0) {
            --m_rockCooldown;
            return;
        }
        // Troll.tick — the EndermanTakeBlockGoal copy. (Its ClipContext
        // line-of-sight test from the troll's cell is not modelled.)
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return;
        JavaRandom& random = m_level->Random();
        const int i = static_cast<int>(std::floor(position.x - 2.0 + random.NextDouble() * 4.0));
        const int j = static_cast<int>(std::floor(position.y + random.NextDouble() * 3.0));
        const int k = static_cast<int>(std::floor(position.z - 2.0 + random.NextDouble() * 4.0));
        const BlockState state = blocks->GetBlockState(i, j, k);
        if (BlockHasTag(state.Block(), "#minecraft:base_stone_overworld")) {
            m_rock = state;
            m_level->SetBlock(glm::ivec3(i, j, k), BlockID::Air);
        }
        if (m_rock.RawId() != 0) SetHasRock(true);
    }

    void Troll::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_hasRock || !m_level) return;
        // Troll.performRangedAttack: from the top of the troll, lobbed at a
        // third of the target's height, speed 1.6, inaccuracy 4 - difficulty.
        auto block = std::make_unique<ThrownBlock>(m_level, m_rock);
        block->SetOwner(this);
        block->position = glm::dvec3(position.x, position.y + GetBbHeight(), position.z);
        const double d0 = target.position.x - position.x;
        const double d1 = target.position.y + target.GetBbHeight() / 3.0f - block->position.y;
        const double d2 = target.position.z - position.z;
        const double d3 = std::sqrt(static_cast<float>(d0 * d0 + d2 * d2));
        const float inaccuracy = 4.0f - static_cast<float>(static_cast<int>(m_level->GetDifficulty()));
        block->Shoot(d0, d1 + d3 * 0.2, d2, 1.6f, inaccuracy);
        m_level->AddFreshEntity(std::move(block));
        SetHasRock(false);
        m_rockCooldown = 300 + m_level->Random().NextInt(100);
        m_rock = BlockState{};
    }

    void Troll::TickDeath() {
        Monster::TickDeath();
        // Troll.tickDeath: every 5 death ticks, ripen a fifth of the unripe
        // trollbers within 12 blocks.
        if (deathTime % 5 == 0) RipenTrollBerNearby(deathTime / 5);
    }

    void Troll::RipenTrollBerNearby(int offset) {
        if (!m_level || m_level->IsClientSide() || !m_level->Blocks()) return;
        const BlockID unripe = TfBlock("unripe_trollber");
        const BlockID ripe = TfBlock("trollber");
        if (unripe == BlockID::Air || ripe == BlockID::Air) return;
        constexpr int kRange = 12;
        const glm::ivec3 c = BlockPosition();
        for (int x = -kRange; x <= kRange; ++x) {
            for (int y = -kRange; y <= kRange; ++y) {
                for (int z = -kRange; z <= kRange; ++z) {
                    const glm::ivec3 p = c + glm::ivec3(x, y, z);
                    if (m_level->Blocks()->GetBlock(p.x, p.y, p.z) != unripe) continue;
                    if (m_level->Random().NextBool() && std::abs(p.x + p.y + p.z) % 5 == offset) {
                        m_level->SetBlock(p, ripe);
                    }
                }
            }
        }
    }

    void Troll::SaveModNbt(ModNbtOut& out) const {
        out.Bool("HasRock", m_hasRock);
        out.Int("RockCooldown", m_rockCooldown);
        // RockState: the block state as this engine's raw id (TF writes the
        // BlockState codec).
        out.Int("RockState", static_cast<int32_t>(m_rock.RawId()));
    }

    void Troll::LoadModNbt(const ModNbtIn& in) {
        m_rockCooldown = in.Int("RockCooldown", 0);
        m_rock = BlockState::FromRawId(static_cast<uint32_t>(in.Int("RockState", 0)));
        SetHasRock(in.Bool("HasRock", false));
    }

    void ThrownBlock::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // ThrownBlock.onHitEntity: 6 to anything living but a troll.
        if (!m_level || m_level->IsClientSide()) return;
        if (target.GetType() == EntityTypeId::Troll) return;
        Entity* owner = GetOwner();
        DealHitDamage(target, hit, MobDamageSource::Projectile, 6.0f, owner ? owner : this);
    }

    void ThrownBlock::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            m_level->BroadcastEntityEvent(*this, 3);
            Discard();
        }
    }

    // ══ Towerwood borer ════════════════════════════════════════════════════

    void TowerwoodBorer::CreateAttributes(AttributeMap& out) {
        // TF TowerwoodBorer.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     15.0);
        out.Register(Attribute::MovementSpeed, 0.27);
        out.Register(Attribute::AttackDamage,   5.0);
        out.Register(Attribute::FollowRange,    8.0);
    }

    TowerwoodBorer::TowerwoodBorer(EntityLevel* level)
        : Monster(EntityTypeId::TowerwoodBorer, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void TowerwoodBorer::RegisterGoals() {
        // TF TowerwoodBorer.registerGoals (ClimbOnTopOfPowderSnowGoal has no
        // engine counterpart).
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        auto summon = std::make_unique<SummonBorersGoal>(this);
        m_summonBorers = summon.get();
        m_goalSelector.AddGoal(3, std::move(summon));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(5, std::make_unique<HideInTowerwoodGoal>(this));
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    bool TowerwoodBorer::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // [VanillaCopy] Silverfish.hurt: an attacker (or magic) wakes the
        // borers in the walls.
        if (!IsInvulnerable() && (attacker != nullptr || source == MobDamageSource::Magic) &&
            m_summonBorers) {
            m_summonBorers->NotifyHurt();
        }
        return Monster::Hurt(source, amount, attacker);
    }

    void TowerwoodBorer::Tick() {
        // TowerwoodBorer.tick: the body follows the yaw.
        yBodyRot = yRot;
        Monster::Tick();
    }

    // ══ Maze slime ═════════════════════════════════════════════════════════

    MazeSlime::MazeSlime(EntityLevel* level) : Slime(EntityTypeId::MazeSlime, level) {
        // Slime's constructor ran Slime::SetSize (no virtual dispatch there).
        SetSize(1, true);
    }

    void MazeSlime::SetSize(int size, bool resetHealth) {
        Slime::SetSize(size, resetHealth);
        // MazeSlime.setSize: DOUBLE_HEALTH (ADD_MULTIPLIED_BASE 2), full
        // health, xpReward size + 3 (GetXpReward).
        SetModifier(m_attributes, Attribute::MaxHealth, kMazeSlimeHealthId, 2.0,
                    AttributeOperation::AddMultipliedBase, true);
        m_health = GetMaxHealth();
    }

    bool MazeSlime::CheckSpawnRules(EntityLevel& level, const glm::ivec3& pos, JavaRandom& rng) {
        return level.GetDifficulty() != Difficulty::Peaceful &&
               Monster::IsDarkEnoughToSpawn(level, pos, rng);
    }

    // ══ Minotaur ═══════════════════════════════════════════════════════════

    void Minotaur::CreateAttributes(AttributeMap& out) {
        // TF Minotaur.registerAttributes + the held golden axe (see header).
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     30.0);
        out.Register(Attribute::MovementSpeed, 0.25);
        out.Register(Attribute::AttackDamage,   8.0);
    }

    Minotaur::Minotaur(EntityLevel* level) : Monster(EntityTypeId::Minotaur, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Minotaur::RegisterGoals() {
        // TF Minotaur.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<ChargeAttackGoal>(this, this, 1.5f));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, false));
    }

    void Minotaur::AiStep() {
        Monster::AiStep();
        // Minotaur.aiStep: legs race while charging (both sides).
        if (m_charging) walkAnimation.SetSpeed(walkAnimation.speed + 0.6f);
    }

    // ══ Redcap sapper ══════════════════════════════════════════════════════

    void RedcapSapper::CreateAttributes(AttributeMap& out) {
        // TF RedcapSapper.registerAttributes = Redcap.registerAttributes
        // (MAX_HEALTH 20, MOVEMENT_SPEED 0.28, + the pickaxe's +3 as in
        // Game::Redcap) then MAX_HEALTH 30, ARMOR 2.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     30.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   5.0);
        out.Register(Attribute::Armor,          2.0);
    }

    RedcapSapper::RedcapSapper(EntityLevel* level) : Monster(EntityTypeId::RedcapSapper, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void RedcapSapper::RegisterGoals() {
        // Redcap.registerGoals + RedcapPlantTNTGoal at 4 — every TNT goal
        // left out (see the header).
        AddRedcapGoals(this, m_goalSelector, m_targetSelector);
    }

    // ══ Block-and-chain goblin ═════════════════════════════════════════════

    void BlockChainGoblin::CreateAttributes(AttributeMap& out) {
        // TF BlockChainGoblin.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     20.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   8.0);
        out.Register(Attribute::Armor,         11.0);
    }

    BlockChainGoblin::BlockChainGoblin(EntityLevel* level)
        : Monster(EntityTypeId::BlockAndChainGoblin, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void BlockChainGoblin::RegisterGoals() {
        // TF BlockChainGoblin.registerGoals (AvoidAnyEntityGoal<PrimedTnt>
        // at 1 has no engine counterpart — see the header).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<ThrowSpikeBlockGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    bool BlockChainGoblin::IsSwingingChain() const {
        return swinging || (GetTarget() != nullptr && m_recoilCounter == 0);
    }

    float BlockChainGoblin::GetChainAngle() const {
        if (m_level && m_level->IsClientSide()) {
            return static_cast<float>(m_syncedChainPos) / 255.0f * 360.0f;
        }
        return m_chainAngle;
    }

    float BlockChainGoblin::GetChainLength() const {
        if (m_level && m_level->IsClientSide()) {
            return static_cast<float>(m_syncedChainLength) / 127.0f;
        }
        return IsSwingingChain() ? 0.9f : 0.3f;
    }

    uint8_t BlockChainGoblin::GetAnimStateByte() const {
        // DATA_CHAINPOS.
        return static_cast<uint8_t>(std::floor(m_chainAngle / 360.0f * 255.0f));
    }
    void BlockChainGoblin::SetAnimStateByte(uint8_t v) { m_syncedChainPos = v; }

    uint8_t BlockChainGoblin::GetVariantByte() const {
        // DATA_CHAINLENGTH (x127) | IS_THROWING.
        const float length = IsSwingingChain() ? 0.9f : 0.3f;
        const auto packed = static_cast<uint8_t>(std::floor(length * 127.0f)) & 0x7F;
        return static_cast<uint8_t>(packed | (m_throwing ? 0x80 : 0));
    }
    void BlockChainGoblin::SetVariantByte(uint8_t v) {
        m_syncedChainLength = static_cast<uint8_t>(v & 0x7F);
        m_throwing = (v & 0x80) != 0;
    }

    void BlockChainGoblin::Tick() {
        Monster::Tick();
        if (!m_level) return;
        if (m_recoilCounter > 0) --m_recoilCounter;

        // BlockChainGoblin.tick — the chain spins on both sides; the client
        // snaps to the synced angle when it drifts.
        m_chainAngle = std::fmod(m_chainAngle + kChainSpeed, 360.0f);
        if (m_level->IsClientSide() &&
            std::abs(m_chainAngle - GetChainAngle()) > kChainSpeed * 2.0f) {
            m_chainAngle = GetChainAngle();
        }

        m_ballOffsetO = m_ballOffset;
        if (IsAlive()) {
            if (m_chainMoveLength > 0.0f) {
                // Thrown: the ball rides the view ray, chainMoveLength out.
                const glm::vec3 view = Mth::ViewVector(xRot, yRot);
                const glm::dvec3 throwPos(position.x + view.x * m_chainMoveLength,
                                          position.y + GetEyeHeight(),
                                          position.z + view.z * m_chainMoveLength);
                if (m_chainMoveLength >= 6.0f) m_throwing = false;
                // setPos(sx2 - ox2, sy2 - oy2, sz2 - oz2) = throwPos + (0, 0.25, 0).
                m_ballOffset = glm::dvec3(throwPos.x, throwPos.y + 0.25, throwPos.z) - position;
            } else {
                // getChainPosition: on the circle, 1.5 - length/4 up.
                const float angle = GetChainAngle();
                const float length = GetChainLength();
                m_ballOffset = glm::dvec3(std::cos(angle * Mth::kPi / 180.0) * length,
                                          1.5 - length / 4.0,
                                          std::sin(angle * Mth::kPi / 180.0) * length);
            }
        }

        if (!m_level->IsClientSide() && IsAlive() && (m_throwing || IsSwingingChain())) {
            ApplyBlockCollisions();
        }

        // chainMove.
        m_chainMoveLength = std::clamp(m_chainMoveLength + (m_throwing ? 0.5f : -1.5f), 0.0f, 6.0f);
    }

    void BlockChainGoblin::ApplyBlockCollisions() {
        // BlockChainGoblin.applyBlockCollisions over the SpikeBlock's box
        // (0.75 cube at the ball's feet) inflated 0.2 horizontally.
        const glm::dvec3 ball = position + m_ballOffset;
        constexpr double kHalf = kBallSize * 0.5;
        const glm::vec3 lo(static_cast<float>(ball.x - kHalf - 0.2), static_cast<float>(ball.y),
                           static_cast<float>(ball.z - kHalf - 0.2));
        const glm::vec3 hi(static_cast<float>(ball.x + kHalf + 0.2),
                           static_cast<float>(ball.y + kBallSize),
                           static_cast<float>(ball.z + kHalf + 0.2));
        std::vector<Entity*> hits;
        m_level->GetEntitiesInBox(AABB::FromMinMax(lo, hi), this, hits);
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        const AABB box = AABB::FromMinMax(lo, hi);
        for (LivingEntity* p : players) {
            if (p->GetAABB().Intersects(box) &&
                std::find(hits.begin(), hits.end(), p) == hits.end()) {
                hits.push_back(p);
            }
        }
        for (Entity* e : hits) {
            if (!e || e == this || !e->IsPushable()) continue;
            // applyBlockCollision: push away from the ball, then the melee
            // hit (Monster.doHurtTarget — the goblin's own override is the
            // SPIKED source of the same damage) and a 0.4 hop.
            glm::dvec3 away = e->position - ball;
            away.y = 0.0;
            const double len = glm::length(away);
            if (len > 1e-4) e->AddDeltaMovement(away / len * 0.05);
            auto* living = e->AsLiving();
            if (living && Monster::DoHurtTarget(*e)) {
                living->AddDeltaMovement(glm::dvec3(0.0, 0.4, 0.0));
                m_recoilCounter = 40;
                if (m_throwing) m_throwing = false;
            }
        }
        // isInWall: a thrown ball that buries itself in a block comes back.
        const IBlockAccess* blocks = m_level->Blocks();
        if (m_throwing && blocks) {
            const glm::dvec3 c = ball + glm::dvec3(0.0, kBallSize * 0.85, 0.0);
            if (blocks->IsBlockSolid(static_cast<int>(std::floor(c.x)),
                                     static_cast<int>(std::floor(c.y)),
                                     static_cast<int>(std::floor(c.z)))) {
                m_throwing = false;
            }
        }
    }

    // ══ Upper goblin knight ════════════════════════════════════════════════

    void UpperGoblinKnight::CreateAttributes(AttributeMap& out) {
        // TF UpperGoblinKnight.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     30.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   8.0);
    }

    UpperGoblinKnight::UpperGoblinKnight(EntityLevel* level)
        : Monster(EntityTypeId::UpperGoblinKnight, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetHasArmor(true);
        SetHasShield(true);
        RegisterGoals();
    }

    void UpperGoblinKnight::RegisterGoals() {
        // TF UpperGoblinKnight.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<HeavySpearAttackGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<UpperKnightMeleeGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, false));
    }

    void UpperGoblinKnight::SetHasArmor(bool flag) {
        m_equip = static_cast<uint8_t>(flag ? (m_equip | 1) : (m_equip & ~1));
        // ARMOR_MODIFIER +20 while armoured.
        SetModifier(m_attributes, Attribute::Armor, kUpperArmorBoostId, 20.0,
                    AttributeOperation::AddValue, flag);
    }

    void UpperGoblinKnight::SetHasShield(bool flag) {
        m_equip = static_cast<uint8_t>(flag ? (m_equip | 2) : (m_equip & ~2));
    }

    void UpperGoblinKnight::BreakArmor() {
        if (m_level) m_level->BroadcastEntityEvent(*this, 5);   // STOP_ATTACKING
        SetHasArmor(false);
    }

    void UpperGoblinKnight::BreakShield() {
        if (m_level) m_level->BroadcastEntityEvent(*this, 5);
        SetHasShield(false);
    }

    void UpperGoblinKnight::DamageShield() {
        ++m_shieldHits;
        if (m_level && !m_level->IsClientSide() && m_shieldHits >= 3) BreakShield();
    }

    void UpperGoblinKnight::AiStep() {
        Monster::AiStep();
        // Decremented on both sides: the client's copy drives the renderer.
        if (m_level && (m_level->IsClientSide() || !IsNoAi()) && m_heavySpearTimer > 0) {
            --m_heavySpearTimer;
            // landHeavySpearAttack's LARGE_SMOKE burst, thrown by the client
            // off its own timer (TF sends a particle packet at the same tick).
            if (m_level->IsClientSide() && m_heavySpearTimer == 25) {
                JavaRandom& r = m_level->Random();
                const glm::vec3 v = Mth::ViewVector(0.0f, yRot);
                const double px = position.x + v.x * 1.25;
                const double py = position.y - (IsPassenger() ? 0.75 : 0.0);
                const double pz = position.z + v.z * 1.25;
                for (int i = 0; i < 50; ++i) {
                    m_level->AddParticle(
                        ParticleKind::LargeSmoke,
                        px + (r.NextFloat() - r.NextFloat()) * 0.25f * r.NextGaussian(), py,
                        pz + (r.NextFloat() - r.NextFloat()) * 0.25f * r.NextGaussian(),
                        0.0, 0.0, 0.0);
                }
            }
        }
        // LowerGoblinKnight.positionRider: the rider faces the mount's way.
        if (Entity* vehicle = GetVehicle(); vehicle && vehicle->GetType() == EntityTypeId::LowerGoblinKnight) {
            yRot = vehicle->yRot;
            yBodyRot = vehicle->yRot;
            yHeadRot = vehicle->yRot;
        }
        // The shield-disabled splash (ParticleTypes.SPLASH) has no engine
        // particle kind.
    }

    void UpperGoblinKnight::CustomServerAiStep() {
        Monster::CustomServerAiStep();
        if (m_shieldDisabled && m_shieldDisabledTicks++ >= 100) {
            m_shieldDisabledTicks = 0;
            m_shieldDisabled = false;
        }
        if (!IsAlive()) return;
        // Share the mount's target; unhorsed, the shield breaks.
        if (auto* mount = dynamic_cast<Mob*>(GetVehicle()); mount && !GetTarget()) {
            SetTarget(mount->GetTarget());
        }
        if (!IsPassenger() && HasShield()) BreakShield();
    }

    void UpperGoblinKnight::HandleEntityEvent(uint8_t id) {
        if (id == 4) {          // START_ATTACKING — the client's slam timer
            m_heavySpearTimer = kHeavySpearTimerStart;
        } else if (id == 5) {   // STOP_ATTACKING — broken armour (no sound/crack particles)
        } else {
            Monster::HandleEntityEvent(id);
        }
    }

    bool UpperGoblinKnight::DoHurtTarget(Entity& target) {
        // UpperGoblinKnight.doHurtTarget: no swing mid-slam; half the swings
        // wind up a slam instead.
        if (m_heavySpearTimer > 0) return false;
        if (!m_level) return false;
        if (m_level->Random().NextInt(2) == 0) {
            m_heavySpearTimer = kHeavySpearTimerStart;
            m_level->BroadcastEntityEvent(*this, 4);
            return false;
        }
        Swing();
        return Monster::DoHurtTarget(target);
    }

    void UpperGoblinKnight::LandHeavySpearAttack() {
        if (!m_level || m_level->IsClientSide()) return;
        // UpperGoblinKnight.landHeavySpearAttack: +12 damage to everything
        // in a 1.5 box 1.25 ahead, but not the mount.
        const glm::vec3 v = Mth::ViewVector(0.0f, yRot);
        const double px = position.x + v.x * 1.25;
        const double py = position.y - (IsPassenger() ? 0.75 : 0.0);
        const double pz = position.z + v.z * 1.25;
        constexpr double kRadius = 1.5;
        const AABB box = AABB::FromMinMax(
            glm::vec3(static_cast<float>(px - kRadius), static_cast<float>(py - kRadius),
                      static_cast<float>(pz - kRadius)),
            glm::vec3(static_cast<float>(px + kRadius), static_cast<float>(py + kRadius),
                      static_cast<float>(pz + kRadius)));
        std::vector<Entity*> inBox;
        m_level->GetEntitiesInBox(box, this, inBox);
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p->GetAABB().Intersects(box) &&
                std::find(inBox.begin(), inBox.end(), p) == inBox.end()) {
                inBox.push_back(p);
            }
        }
        SetModifier(m_attributes, Attribute::AttackDamage, kSpearAttackBoostId, 12.0,
                    AttributeOperation::AddValue, true);
        for (Entity* e : inBox) {
            if (!e || e == this || e == GetVehicle()) continue;
            Monster::DoHurtTarget(*e);
        }
        SetModifier(m_attributes, Attribute::AttackDamage, kSpearAttackBoostId, 0.0,
                    AttributeOperation::AddValue, false);
    }

    bool UpperGoblinKnight::TakeHitOnShield(Entity* attacker, float amount) {
        // UpperGoblinKnight.takeHitOnShield.
        if (m_shieldDisabled) return false;
        auto* living = attacker ? attacker->AsLiving() : nullptr;
        if (living && living->IsPlayer() && m_level && !m_level->IsClientSide()) {
            const uint32_t held = m_level->GetHeldItemId(*living);
            static constexpr ItemID kAxes[] = {
                Items::WoodenAxe, Items::CopperAxe, Items::StoneAxe, Items::GoldenAxe,
                Items::IronAxe, Items::DiamondAxe, Items::NetheriteAxe, Items::IronwoodAxe,
                Items::SteeleafAxe, Items::KnightmetalAxe,
            };
            for (const ItemID axe : kAxes) {
                if (held == axe) {
                    m_shieldDisabled = true;
                    return true;
                }
            }
        }
        if (amount > kShieldDamageThreshold && m_level && !m_level->IsClientSide()) {
            DamageShield();
        }
        // Knock the rider-and-mount back slightly; the attacker is remembered.
        LivingEntity* toKnockback = GetVehicle() && GetVehicle()->AsLiving()
                                        ? GetVehicle()->AsLiving() : this;
        if (attacker) {
            double d0 = attacker->position.x - position.x;
            double d1 = attacker->position.z - position.z;
            JavaRandom& r = m_level->Random();
            while (d0 * d0 + d1 * d1 < 1.0e-4) {
                d0 = (r.NextDouble() - r.NextDouble()) * 0.01;
                d1 = (r.NextDouble() - r.NextDouble()) * 0.01;
            }
            toKnockback->Knockback(0.0, d0 / 4.0, d1 / 4.0);
            if (living) SetLastHurtByMob(living);
        }
        return true;
    }

    bool UpperGoblinKnight::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // UpperGoblinKnight.hurtServer: shield from the front, armour from
        // behind.
        if (attacker && m_level) {
            const double dx = position.x - attacker->position.x;
            const double dz = position.z - attacker->position.z;
            const float angle = static_cast<float>(std::atan2(dz, dx) * 180.0 / Mth::kPi) - 90.0f;
            const float difference = std::abs(std::fmod(yBodyRot - angle, 360.0f));
            if (HasShield() && difference > 150.0f && difference < 230.0f) {
                if (TakeHitOnShield(attacker, amount)) return false;
            } else if (HasShield() && m_level->Random().NextBool()) {
                DamageShield();
            }
            if (HasArmor() && (difference > 300.0f || difference < 60.0f)) BreakArmor();
        }
        return Monster::Hurt(source, amount, attacker);
    }

    void UpperGoblinKnight::SaveModNbt(ModNbtOut& out) const {
        out.Bool("hasArmor", HasArmor());
        out.Bool("hasShield", HasShield());
    }

    void UpperGoblinKnight::LoadModNbt(const ModNbtIn& in) {
        SetHasArmor(in.Bool("hasArmor", false));
        SetHasShield(in.Bool("hasShield", false));
    }

    // ══ Lower goblin knight ════════════════════════════════════════════════

    void LowerGoblinKnight::CreateAttributes(AttributeMap& out) {
        // TF LowerGoblinKnight.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     20.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   4.0);
    }

    LowerGoblinKnight::LowerGoblinKnight(EntityLevel* level)
        : Monster(EntityTypeId::LowerGoblinKnight, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetHasArmor(true);
        RegisterGoals();
    }

    void LowerGoblinKnight::RegisterGoals() {
        // TF LowerGoblinKnight.registerGoals, verbatim.
        m_goalSelector.AddGoal(0, std::make_unique<RiderSpearAttackGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<LowerKnightMeleeGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, false));
    }

    UpperGoblinKnight* LowerGoblinKnight::GetUpper() const {
        Entity* rider = GetFirstPassenger();
        if (!rider || rider->GetType() != EntityTypeId::UpperGoblinKnight) return nullptr;
        return static_cast<UpperGoblinKnight*>(rider);
    }

    void LowerGoblinKnight::SetHasArmor(bool flag) {
        m_hasArmor = flag;
        // ARMOR_MODIFIER +17 while armoured.
        SetModifier(m_attributes, Attribute::Armor, kLowerArmorBoostId, 17.0,
                    AttributeOperation::AddValue, flag);
    }

    std::shared_ptr<SpawnGroupData>
    LowerGoblinKnight::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Monster::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level || m_level->IsClientSide()) return groupData;
        // LowerGoblinKnight.finalizeSpawn: every lower knight carries an
        // upper one, finalized as a natural spawn with the same group data.
        auto upper = std::make_unique<UpperGoblinKnight>(m_level);
        upper->position = position + glm::dvec3(0.0, 1.0, 0.0);
        upper->yRot = yRot;
        upper->xRot = 0.0f;
        groupData = upper->FinalizeSpawn(SpawnReason::Natural, std::move(groupData));
        UpperGoblinKnight* placed = upper.get();
        m_level->AddFreshEntity(std::move(upper));
        placed->StartRiding(*this, /*force=*/true);
        return groupData;
    }

    glm::dvec3 LowerGoblinKnight::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // LowerGoblinKnight.getPassengerAttachmentPoint: super - 9% height.
        return Monster::GetPassengerAttachmentPoint(passenger) +
               glm::dvec3(0.0, -GetBbHeight() * 0.09, 0.0);
    }

    bool LowerGoblinKnight::DoHurtTarget(Entity& target) {
        // LowerGoblinKnight.doHurtTarget: the rider does the hitting.
        if (UpperGoblinKnight* upper = GetUpper()) return upper->DoHurtTarget(target);
        return Monster::DoHurtTarget(target);
    }

    bool LowerGoblinKnight::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // LowerGoblinKnight.hurtServer: the rider's shield covers the front,
        // a hit from behind breaks the armour.
        if (attacker) {
            const double dx = position.x - attacker->position.x;
            const double dz = position.z - attacker->position.z;
            const float angle = static_cast<float>(std::atan2(dz, dx) * 180.0 / Mth::kPi) - 90.0f;
            const float difference = std::abs(std::fmod(yBodyRot - angle, 360.0f));
            UpperGoblinKnight* upper = GetUpper();
            if (upper && upper->HasShield() && difference > 150.0f && difference < 230.0f) {
                if (upper->TakeHitOnShield(attacker, amount)) return false;
            }
            if (m_hasArmor && (difference > 300.0f || difference < 60.0f)) {
                if (m_level) m_level->BroadcastEntityEvent(*this, 5);
                SetHasArmor(false);
            }
        }
        return Monster::Hurt(source, amount, attacker);
    }

    void LowerGoblinKnight::HandleEntityEvent(uint8_t id) {
        if (id == 5) return;   // STOP_ATTACKING — broken armour (no crack particles)
        Monster::HandleEntityEvent(id);
    }

    void LowerGoblinKnight::SaveModNbt(ModNbtOut& out) const { out.Bool("hasArmor", m_hasArmor); }
    void LowerGoblinKnight::LoadModNbt(const ModNbtIn& in) { SetHasArmor(in.Bool("hasArmor", false)); }

} // namespace Game
