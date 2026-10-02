// File: src/common/entity/ai/goals/FoxGoals.cpp
#include "common/entity/ai/goals/FoxGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include <cmath>
#include <vector>

namespace Game {

    namespace {

        // ── The fox's two controls (MC Fox.FoxMoveControl / FoxLookControl) ─

        class FoxMoveControl : public MoveControl {
        public:
            explicit FoxMoveControl(Fox* fox) : MoveControl(fox), m_fox(fox) {}

            void Tick() override {
                // MC: a sitting/sleeping/faceplanted fox ignores steering.
                if (m_fox->CanMove()) MoveControl::Tick();
            }

        private:
            Fox* m_fox;
        };

        class FoxLookControl : public LookControl {
        public:
            explicit FoxLookControl(Fox* fox) : LookControl(fox), m_fox(fox) {}

            void Tick() override {
                // MC: a sleeping fox's head stays put.
                if (!m_fox->IsSleeping()) LookControl::Tick();
            }

        protected:
            bool ResetXRotOnTick() const override {
                // MC: keep the pitch through the pounce arc and the poses.
                return !m_fox->IsPouncing() && !m_fox->IsFoxCrouching()
                    && !m_fox->IsInterested() && !m_fox->IsFaceplanted();
            }

        private:
            Fox* m_fox;
        };

        // MC Fox.STALKABLE_PREY: chicken or rabbit.
        bool IsStalkablePrey(const Entity& e) {
            return e.GetType() == EntityTypeId::Chicken
                || e.GetType() == EntityTypeId::Rabbit;
        }

        // MC Fox.FoxAlertableEntitiesSelector — what interrupts a nap or a
        // perch, term for term. isSleeping is read off the SLEEPING pose (a
        // villager in bed); a player's bed rest does not reach the mob side
        // (the player view carries no pose), so a sleeping player still
        // counts as awake here.
        bool IsAlertableEntity(const Fox& fox, const LivingEntity& target) {
            if (target.GetType() == EntityTypeId::Fox) return false;
            if (IsStalkablePrey(target)) return true;
            if (IsMonsterCategory(target.TypeInfo().category)) return true;
            // TamableAnimal → !isTame: a wild wolf/cat/parrot alerts, a pet
            // does not.
            if (const auto* tamable = dynamic_cast<const TamableAnimal*>(&target)) {
                return !tamable->IsTame();
            }
            if (target.IsPlayer() && (target.IsSpectator() || target.IsCreative())) return false;
            if (fox.Trusts(target)) return false;
            return target.GetPose() != Pose::Sleeping && !target.IsDiscrete();
        }

        // MC FoxBehaviorGoal.alertable — anything alertable within the
        // 12x6x12 box around the fox (combat conditions, no line of sight).
        bool FoxAlertable(Fox& fox) {
            EntityLevel* level = fox.Level();
            if (!level) return false;

            TargetingConditions conditions =
                TargetingConditions::ForCombat().Range(12.0).IgnoreLineOfSight();

            AABB box = fox.GetAABB();
            box.min -= glm::vec3(12.0f, 6.0f, 12.0f);
            box.max += glm::vec3(12.0f, 6.0f, 12.0f);

            std::vector<Entity*> nearby;
            level->GetEntitiesInBox(box, &fox, nearby);
            for (Entity* e : nearby) {
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living || !living->IsAlive()) continue;
                if (!IsAlertableEntity(fox, *living)) continue;
                if (!conditions.Test(&fox, *living)) continue;
                return true;
            }
            return false;
        }

        // MC FoxBehaviorGoal.hasShelter — the block ABOVE the fox's box must
        // not see the sky, and must be somewhere the fox wants to stand.
        bool FoxHasShelter(Fox& fox) {
            EntityLevel* level = fox.Level();
            if (!level) return false;
            const AABB box = fox.GetAABB();
            const glm::ivec3 pos(static_cast<int>(std::floor(fox.position.x)),
                                 static_cast<int>(std::floor(box.max.y)),
                                 static_cast<int>(std::floor(fox.position.z)));
            return !level->CanSeeSky(pos.x, pos.y, pos.z)
                && fox.GetWalkTargetValue(pos) >= 0.0f;
        }

        // The 4-way horizontal direction of a vector, 0..3 — enough to port
        // MC's getMotionDirection() != getDirection() pounce gate.
        int HorizontalDirOf(double x, double z) {
            return std::abs(x) > std::abs(z) ? (x > 0.0 ? 0 : 1)
                                             : (z > 0.0 ? 2 : 3);
        }

    } // namespace

    std::unique_ptr<MoveControl> MakeFoxMoveControl(Fox* fox) {
        return std::make_unique<FoxMoveControl>(fox);
    }

    std::unique_ptr<LookControl> MakeFoxLookControl(Fox* fox) {
        return std::make_unique<FoxLookControl>(fox);
    }

    // ── FoxFloatGoal ───────────────────────────────────────────────────────

    FoxFloatGoal::FoxFloatGoal(Fox* fox) : FloatGoal(fox), m_fox(fox) {}

    void FoxFloatGoal::Start() {
        // MC: hitting water snaps the fox out of every pose.
        m_fox->ClearStates();
    }

    // ── FaceplantGoal ──────────────────────────────────────────────────────

    FaceplantGoal::FaceplantGoal(Fox* fox) : m_fox(fox) {
        SetFlags(GoalFlag::Look | GoalFlag::Jump | GoalFlag::Move);
    }

    bool FaceplantGoal::CanUse() { return m_fox->IsFaceplanted(); }

    bool FaceplantGoal::CanContinueToUse() {
        return CanUse() && m_countdown > 0;
    }

    void FaceplantGoal::Start() { m_countdown = AdjustedTickDelay(40); }
    void FaceplantGoal::Stop() { m_fox->SetFaceplanted(false); }
    void FaceplantGoal::Tick() { --m_countdown; }

    // ── FoxPanicGoal ───────────────────────────────────────────────────────

    FoxPanicGoal::FoxPanicGoal(Fox* fox, double speedModifier)
        : PanicGoal(fox, speedModifier), m_fox(fox) {}

    bool FoxPanicGoal::ShouldPanic() const {
        return !m_fox->IsDefending() && PanicGoal::ShouldPanic();
    }

    // ── FoxBreedGoal ───────────────────────────────────────────────────────

    FoxBreedGoal::FoxBreedGoal(Fox* fox, double speedModifier)
        : BreedGoal(fox, speedModifier), m_fox(fox) {}

    void FoxBreedGoal::Start() {
        m_fox->ClearStates();
        BreedGoal::Start();
    }

    // ── StalkPreyGoal ──────────────────────────────────────────────────────

    StalkPreyGoal::StalkPreyGoal(Fox* fox) : m_fox(fox) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool StalkPreyGoal::CanUse() {
        if (m_fox->IsSleeping()) return false;
        LivingEntity* target = m_fox->GetTarget();
        return target && target->IsAlive() && IsStalkablePrey(*target)
            && m_fox->DistanceToSqr(*target) > 36.0
            && !m_fox->IsFoxCrouching() && !m_fox->IsInterested()
            && !m_fox->jumping;
    }

    void StalkPreyGoal::Start() {
        m_fox->SetSitting(false);
        m_fox->SetFaceplanted(false);
    }

    void StalkPreyGoal::Stop() {
        LivingEntity* target = m_fox->GetTarget();
        if (target && Fox::IsPathClear(*m_fox, *target)) {
            m_fox->SetIsInterested(true);
            m_fox->SetIsCrouching(true);
            m_fox->GetNavigation().Stop();
            m_fox->GetLookControl().SetLookAt(
                target->position.x, target->GetEyeY(), target->position.z,
                static_cast<float>(m_fox->GetMaxHeadYRot()),
                static_cast<float>(m_fox->GetMaxHeadXRot()));
        } else {
            m_fox->SetIsInterested(false);
            m_fox->SetIsCrouching(false);
        }
    }

    void StalkPreyGoal::Tick() {
        LivingEntity* target = m_fox->GetTarget();
        if (!target) return;
        m_fox->GetLookControl().SetLookAt(
            target->position.x, target->GetEyeY(), target->position.z,
            static_cast<float>(m_fox->GetMaxHeadYRot()),
            static_cast<float>(m_fox->GetMaxHeadXRot()));
        if (m_fox->DistanceToSqr(*target) <= 36.0) {
            m_fox->SetIsInterested(true);
            m_fox->SetIsCrouching(true);
            m_fox->GetNavigation().Stop();
        } else {
            m_fox->GetNavigation().MoveTo(*target, 1.5);
        }
    }

    // ── FoxPounceGoal ──────────────────────────────────────────────────────

    FoxPounceGoal::FoxPounceGoal(Fox* fox) : m_fox(fox) {
        // MC JumpGoal's flags.
        SetFlags(GoalFlag::Move | GoalFlag::Jump);
    }

    bool FoxPounceGoal::CanUse() {
        if (!m_fox->IsFullyCrouched()) return false;
        LivingEntity* target = m_fox->GetTarget();
        if (!target || !target->IsAlive()) return false;

        // MC: only pounce prey moving the way it faces (or standing still —
        // MC's getMotionDirection falls back to the facing at rest).
        const double vx = target->velocity.x, vz = target->velocity.z;
        if (vx * vx + vz * vz > 1.0e-8) {
            const float yaw = target->yRot * Mth::kDegToRad;
            if (HorizontalDirOf(vx, vz)
                != HorizontalDirOf(-std::sin(yaw), std::cos(yaw))) {
                return false;
            }
        }

        const bool clear = Fox::IsPathClear(*m_fox, *target);
        if (!clear) {
            // MC warms a path to the target here (createPath) — our
            // navigation computes paths on demand, so only the pose reset
            // carries over.
            m_fox->SetIsCrouching(false);
            m_fox->SetIsInterested(false);
        }
        return clear;
    }

    bool FoxPounceGoal::CanContinueToUse() {
        LivingEntity* target = m_fox->GetTarget();
        if (!target || !target->IsAlive()) return false;
        const double yd = m_fox->velocity.y;
        return (!(yd * yd < 0.05) || !(std::abs(m_fox->xRot) < 15.0f)
                || !m_fox->onGround)
            && !m_fox->IsFaceplanted();
    }

    void FoxPounceGoal::Start() {
        m_fox->jumping = true;
        m_fox->SetIsPouncing(true);
        m_fox->SetIsInterested(false);
        LivingEntity* target = m_fox->GetTarget();
        if (target) {
            m_fox->GetLookControl().SetLookAt(
                target->position.x, target->GetEyeY(), target->position.z,
                60.0f, 30.0f);
            glm::dvec3 uv = glm::normalize(glm::dvec3(
                target->position.x - m_fox->position.x,
                target->position.y - m_fox->position.y,
                target->position.z - m_fox->position.z));
            m_fox->velocity += glm::dvec3(uv.x * 0.8, 0.9, uv.z * 0.8);
            m_fox->needsSync = true;
        }
        m_fox->GetNavigation().Stop();
    }

    void FoxPounceGoal::Stop() {
        m_fox->SetIsCrouching(false);
        m_fox->ResetCrouchAmount();
        m_fox->SetIsInterested(false);
        m_fox->SetIsPouncing(false);
    }

    void FoxPounceGoal::Tick() {
        LivingEntity* target = m_fox->GetTarget();
        if (target) {
            m_fox->GetLookControl().SetLookAt(
                target->position.x, target->GetEyeY(), target->position.z,
                60.0f, 30.0f);
        }

        if (!m_fox->IsFaceplanted()) {
            const glm::dvec3 v = m_fox->velocity;
            if (v.y * v.y < 0.03 && m_fox->xRot != 0.0f) {
                // MC Mth.rotLerp(0.2, xRot, 0).
                m_fox->xRot = Mth::RotLerp(0.2f, m_fox->xRot, 0.0f);
            } else {
                const double horizontal = std::sqrt(v.x * v.x + v.z * v.z);
                const double length = glm::length(v);
                if (length > 1.0e-8) {
                    const double rotation =
                        (v.y > 0.0 ? -1.0 : (v.y < 0.0 ? 1.0 : 0.0))
                        * std::acos(std::clamp(horizontal / length, -1.0, 1.0))
                        * (180.0 / Mth::kPi);
                    m_fox->xRot = static_cast<float>(rotation);
                }
            }
        }

        if (target && m_fox->DistanceTo(*target) <= 2.0f) {
            m_fox->DoHurtTarget(*target);
        } else if (m_fox->xRot > 0.0f && m_fox->onGround
                   && static_cast<float>(m_fox->velocity.y) != 0.0f) {
            // MC: the faceplant only happens on a SNOW layer.
            const IBlockAccess* blocks =
                m_fox->Level() ? m_fox->Level()->Blocks() : nullptr;
            const glm::ivec3 p = m_fox->BlockPosition();
            if (blocks && blocks->GetBlock(p.x, p.y, p.z) == BlockID::SnowLayer) {
                m_fox->xRot = 60.0f;
                m_fox->SetTarget(nullptr);
                m_fox->SetFaceplanted(true);
            }
        }
    }

    // ── SeekShelterGoal ────────────────────────────────────────────────────

    SeekShelterGoal::SeekShelterGoal(Fox* fox, double speedModifier)
        : m_fox(fox), m_speedModifier(speedModifier),
          m_interval(ReducedTickDelay(100)) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool SeekShelterGoal::CanUse() {
        if (m_fox->IsSleeping() || m_fox->GetTarget() != nullptr) return false;
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        const glm::ivec3 pos = m_fox->BlockPosition();

        if (level->IsThundering() && level->CanSeeSky(pos.x, pos.y, pos.z)) {
            return SetWantedPos();
        }
        if (m_interval > 0) {
            --m_interval;
            return false;
        }
        m_interval = 100;
        // MC also rejects village positions; no villages exist here.
        // Level.isBrightOutside: skyDarken below 4 (weather-darkened) and no
        // fixed time.
        const bool brightOutside = !DimensionFixedTime(level->Dimension()).has_value() && level->GetSkyDarken() < 4;
        return brightOutside && level->CanSeeSky(pos.x, pos.y, pos.z)
            && SetWantedPos();
    }

    bool SeekShelterGoal::SetWantedPos() {
        // MC FleeSunGoal.lookForHidePos: ten tries in a 20x6x20 box for a
        // spot the sky cannot see with a NEGATIVE walk-target value.
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        const glm::ivec3 origin = m_fox->BlockPosition();
        for (int i = 0; i < 10; ++i) {
            const glm::ivec3 candidate(
                origin.x + level->Random().NextInt(20) - 10,
                origin.y + level->Random().NextInt(6) - 3,
                origin.z + level->Random().NextInt(20) - 10);
            if (!level->CanSeeSky(candidate.x, candidate.y, candidate.z)
                && m_fox->GetWalkTargetValue(candidate) < 0.0f) {
                m_wantedX = candidate.x + 0.5;
                m_wantedY = candidate.y;
                m_wantedZ = candidate.z + 0.5;
                return true;
            }
        }
        return false;
    }

    bool SeekShelterGoal::CanContinueToUse() {
        return !m_fox->GetNavigation().IsDone();
    }

    void SeekShelterGoal::Start() {
        m_fox->ClearStates();
        m_fox->GetNavigation().MoveTo(m_wantedX, m_wantedY, m_wantedZ,
                                      m_speedModifier);
    }

    // ── FoxMeleeAttackGoal ─────────────────────────────────────────────────

    FoxMeleeAttackGoal::FoxMeleeAttackGoal(Fox* fox, double speedModifier,
                                           bool trackTarget)
        : MeleeAttackGoal(fox, speedModifier, trackTarget), m_fox(fox) {}

    bool FoxMeleeAttackGoal::CanUse() {
        return !m_fox->IsSitting() && !m_fox->IsSleeping()
            && !m_fox->IsFoxCrouching() && !m_fox->IsFaceplanted()
            && MeleeAttackGoal::CanUse();
    }

    void FoxMeleeAttackGoal::CheckAndPerformAttack(LivingEntity& target) {
        // MC Fox.FoxMeleeAttackGoal.checkAndPerformAttack.
        if (!CanPerformAttack(target)) return;
        ResetAttackCooldown();
        m_mob->DoHurtTarget(target);
        m_fox->PlaySound(SoundEvents::FOX_BITE, 1.0f, 1.0f);
    }

    void FoxMeleeAttackGoal::Start() {
        m_fox->SetIsInterested(false);
        MeleeAttackGoal::Start();
    }

    // ── SleepGoal ──────────────────────────────────────────────────────────

    SleepGoal::SleepGoal(Fox* fox) : m_fox(fox), m_countdown(0) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
        if (fox->Level()) {
            m_countdown = fox->Level()->Random().NextInt(ReducedTickDelay(140));
        }
    }

    bool SleepGoal::CanUse() {
        if (m_fox->xxa != 0.0f || m_fox->yya != 0.0f || m_fox->zza != 0.0f) {
            return false;
        }
        return CanSleep() || m_fox->IsSleeping();
    }

    bool SleepGoal::CanContinueToUse() { return CanSleep(); }

    bool SleepGoal::CanSleep() {
        if (m_countdown > 0) {
            --m_countdown;
            return false;
        }
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        // MC isBrightOutside && shelter && nothing alertable && !powder snow
        // (no powder snow entity state exists here).
        const bool brightOutside = !DimensionFixedTime(level->Dimension()).has_value() && level->GetSkyDarken() < 4;
        return brightOutside && FoxHasShelter(*m_fox) && !FoxAlertable(*m_fox);
    }

    void SleepGoal::Stop() {
        if (m_fox->Level()) {
            m_countdown = m_fox->Level()->Random().NextInt(ReducedTickDelay(140));
        }
        m_fox->ClearStates();
    }

    void SleepGoal::Start() {
        m_fox->SetSitting(false);
        m_fox->SetIsCrouching(false);
        m_fox->SetIsInterested(false);
        m_fox->jumping = false;
        m_fox->SetSleeping(true);
        m_fox->GetNavigation().Stop();
        m_fox->GetMoveControl().SetWantedPosition(
            m_fox->position.x, m_fox->position.y, m_fox->position.z, 0.0);
    }

    // ── PerchAndSearchGoal ─────────────────────────────────────────────────

    PerchAndSearchGoal::PerchAndSearchGoal(Fox* fox) : m_fox(fox) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    // ── FoxSearchForItemsGoal ──────────────────────────────────────────────

    bool FoxSearchForItemsGoal::FindItem(glm::dvec3& out) const {
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        AABBd box = m_fox->GetAABBd();
        box.min -= glm::dvec3(8.0);
        box.max += glm::dvec3(8.0);
        std::vector<EntityLevel::NearbyItemEntity> items;
        level->GetItemEntitiesInBox(box, items);
        for (const auto& item : items) {
            if (!item.canPickUp || item.count <= 0) continue;   // ALLOWED_ITEMS
            out = item.pos;
            return true;
        }
        return false;
    }

    bool FoxSearchForItemsGoal::CanUse() {
        if (!m_fox->GetEquipment(EquipmentSlot::MAINHAND).IsEmpty()) return false;
        if (m_fox->GetTarget() != nullptr || m_fox->GetLastHurtByMob() != nullptr) return false;
        if (!m_fox->CanMove()) return false;
        EntityLevel* level = m_fox->Level();
        if (!level || level->Random().NextInt(ReducedTickDelay(10)) != 0) return false;
        glm::dvec3 at;
        return FindItem(at) && m_fox->GetEquipment(EquipmentSlot::MAINHAND).IsEmpty();
    }

    void FoxSearchForItemsGoal::Tick() {
        glm::dvec3 at;
        if (m_fox->GetEquipment(EquipmentSlot::MAINHAND).IsEmpty() && FindItem(at)) {
            m_fox->GetNavigation().MoveTo(at.x, at.y, at.z, 1.2000000476837158);
        }
    }

    void FoxSearchForItemsGoal::Start() {
        glm::dvec3 at;
        if (FindItem(at)) m_fox->GetNavigation().MoveTo(at.x, at.y, at.z, 1.2000000476837158);
    }

    // ── FoxEatBerriesGoal ─────────────────────────────────────────────────

    FoxEatBerriesGoal::FoxEatBerriesGoal(Fox* fox, double speedModifier, int searchRange,
                                         int verticalSearchRange)
        : MoveToBlockGoal(fox, speedModifier, searchRange, verticalSearchRange), m_fox(fox) {}

    bool FoxEatBerriesGoal::IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const {
        const BlockState state = blocks.GetBlockState(pos.x, pos.y, pos.z);
        if (state.Is(BlockID::SweetBerryBush)) return state.GetIndex(PropertyId::AGE_3) >= 2;
        // CaveVines.hasGlowBerries: a cave vine (head or body) with berries.
        if (state.Is(BlockID::CaveVines) || state.Is(BlockID::CaveVinesPlant)) {
            return state.GetName(PropertyId::BERRIES) == "true";
        }
        return false;
    }

    bool FoxEatBerriesGoal::CanUse() {
        return !m_fox->IsSleeping() && MoveToBlockGoal::CanUse();
    }

    void FoxEatBerriesGoal::Start() {
        m_ticksWaited = 0;
        m_fox->SetSitting(false);
        MoveToBlockGoal::Start();
    }

    void FoxEatBerriesGoal::Tick() {
        if (IsReachedTarget()) {
            if (m_ticksWaited >= 40) OnReachedTarget();
            else ++m_ticksWaited;
        } else if (m_fox->Level() && m_fox->Level()->Random().NextFloat() < 0.05f) {
            m_fox->PlaySound(SoundEvents::FOX_SNIFF, 1.0f, 1.0f);
        }
        MoveToBlockGoal::Tick();
    }

    void FoxEatBerriesGoal::OnReachedTarget() {
        EntityLevel* level = m_fox->Level();
        if (!level || level->IsClientSide() || !level->MobGriefing()) return;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return;
        const BlockState state = blocks->GetBlockState(m_blockPos.x, m_blockPos.y, m_blockPos.z);
        if (state.Is(BlockID::SweetBerryBush)) {
            PickSweetBerries(state);
        } else if ((state.Is(BlockID::CaveVines) || state.Is(BlockID::CaveVinesPlant)) &&
                   state.GetName(PropertyId::BERRIES) == "true") {
            PickGlowBerry(state);
        }
    }

    void FoxEatBerriesGoal::PickGlowBerry(BlockState state) {
        // MC CaveVines.use(entity, state, level, pos): pop one glow berry,
        // the pick sound (1.0, 0.8..1.2), berries off (flag 2), BLOCK_CHANGE.
        EntityLevel* level = m_fox->Level();
        DropItemStackNear(level->Dimension(), m_blockPos, ItemStack(Items::GlowBerries, 1));
        const float pitch = 0.8f + level->Random().NextFloat() * 0.4f;
        level->PlaySeededSound(nullptr, glm::dvec3(m_blockPos) + glm::dvec3(0.5),
                               SoundEvents::CAVE_VINES_PICK_BERRIES, SoundSource::Blocks, 1.0f, pitch,
                               level->Random().NextLong());
        const BlockState picked = state.SetName(PropertyId::BERRIES, "false");
        level->SetBlockState(m_blockPos, picked);
        if (ILevelWrite* write = level->MutableBlocks()) {
            write->GameEvent(GameEventId::BlockChange, m_blockPos, GameEventContext::Of(m_fox, picked));
        }
    }

    void FoxEatBerriesGoal::PickSweetBerries(BlockState state) {
        EntityLevel* level = m_fox->Level();
        const int age = state.GetIndex(PropertyId::AGE_3);
        int count = 1 + level->Random().NextInt(2) + (age == 3 ? 1 : 0);
        if (m_fox->GetEquipment(EquipmentSlot::MAINHAND).IsEmpty()) {
            m_fox->SetEquipment(EquipmentSlot::MAINHAND, ItemStack(Items::SweetBerries, 1));
            --count;
        }
        if (count > 0) {
            DropItemStackNear(level->Dimension(), m_blockPos, ItemStack(Items::SweetBerries, count));
        }
        m_fox->PlaySound(SoundEvents::SWEET_BERRY_BUSH_PICK_BERRIES, 1.0f, 1.0f);
        level->SetBlockState(m_blockPos, state.SetIndex(PropertyId::AGE_3, 1));
        if (ILevelWrite* write = level->MutableBlocks()) {
            write->GameEvent(GameEventId::BlockChange, m_blockPos, GameEventContext::Of(m_fox));
        }
    }

    bool PerchAndSearchGoal::CanUse() {
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        return m_fox->GetLastHurtByMob() == nullptr
            && level->Random().NextFloat() < 0.02f
            && !m_fox->IsSleeping()
            && m_fox->GetTarget() == nullptr
            && m_fox->GetNavigation().IsDone()
            && !FoxAlertable(*m_fox)
            && !m_fox->IsPouncing() && !m_fox->IsFoxCrouching();
    }

    bool PerchAndSearchGoal::CanContinueToUse() { return m_looksRemaining > 0; }

    void PerchAndSearchGoal::Start() {
        ResetLook();
        m_looksRemaining = 2 + m_fox->Level()->Random().NextInt(3);
        m_fox->SetSitting(true);
        m_fox->GetNavigation().Stop();
    }

    void PerchAndSearchGoal::Stop() { m_fox->SetSitting(false); }

    void PerchAndSearchGoal::Tick() {
        --m_lookTime;
        if (m_lookTime <= 0) {
            --m_looksRemaining;
            ResetLook();
        }
        m_fox->GetLookControl().SetLookAt(
            m_fox->position.x + m_relX, m_fox->GetEyeY(),
            m_fox->position.z + m_relZ,
            static_cast<float>(m_fox->GetMaxHeadYRot()),
            static_cast<float>(m_fox->GetMaxHeadXRot()));
    }

    void PerchAndSearchGoal::ResetLook() {
        const double angle =
            2.0 * Mth::kPi * m_fox->Level()->Random().NextDouble();
        m_relX = std::cos(angle);
        m_relZ = std::sin(angle);
        m_lookTime = AdjustedTickDelay(80 + m_fox->Level()->Random().NextInt(20));
    }

    // ── FoxFollowParentGoal ────────────────────────────────────────────────

    FoxFollowParentGoal::FoxFollowParentGoal(Fox* fox, double speedModifier)
        : FollowParentGoal(fox, speedModifier), m_fox(fox) {}

    bool FoxFollowParentGoal::CanUse() {
        return !m_fox->IsDefending() && FollowParentGoal::CanUse();
    }

    bool FoxFollowParentGoal::CanContinueToUse() {
        return !m_fox->IsDefending() && FollowParentGoal::CanContinueToUse();
    }

    void FoxFollowParentGoal::Start() {
        m_fox->ClearStates();
        FollowParentGoal::Start();
    }

    // ── FoxLookAtPlayerGoal ────────────────────────────────────────────────

    FoxLookAtPlayerGoal::FoxLookAtPlayerGoal(Fox* fox, float lookDistance)
        : LookAtPlayerGoal(fox, lookDistance), m_fox(fox) {}

    bool FoxLookAtPlayerGoal::CanUse() {
        return LookAtPlayerGoal::CanUse() && !m_fox->IsFaceplanted()
            && !m_fox->IsInterested();
    }

    bool FoxLookAtPlayerGoal::CanContinueToUse() {
        return LookAtPlayerGoal::CanContinueToUse() && !m_fox->IsFaceplanted()
            && !m_fox->IsInterested();
    }

    // ── FoxAvoidEntityGoal ─────────────────────────────────────────────────

    FoxAvoidEntityGoal::FoxAvoidEntityGoal(Fox* fox, float maxDistance,
                                           double walkSpeedModifier,
                                           double sprintSpeedModifier)
        : AvoidEntityGoal(fox, maxDistance, walkSpeedModifier,
                          sprintSpeedModifier),
          m_fox(fox) {
        SetFiltersThreats(true);
    }

    FoxAvoidEntityGoal::FoxAvoidEntityGoal(Fox* fox, const EntityTypeId* types,
                                           int typeCount, float maxDistance,
                                           double walkSpeedModifier,
                                           double sprintSpeedModifier)
        : AvoidEntityGoal(fox, types, typeCount, maxDistance,
                          walkSpeedModifier, sprintSpeedModifier),
          m_fox(fox) {
        SetFiltersThreats(true);
    }

    bool FoxAvoidEntityGoal::CanUse() {
        return !m_fox->IsDefending() && AvoidEntityGoal::CanUse();
    }

    bool FoxAvoidEntityGoal::CanContinueToUse() {
        return !m_fox->IsDefending() && AvoidEntityGoal::CanContinueToUse();
    }

    bool FoxAvoidEntityGoal::AcceptsThreat(const LivingEntity& candidate) const {
        if (candidate.IsPlayer()) {
            // AVOID_PLAYERS: !isDiscrete && NO_CREATIVE_OR_SPECTATOR; then !trusts.
            return !candidate.IsDiscrete() && !candidate.IsCreative() &&
                   !candidate.IsSpectator() && !m_fox->Trusts(candidate);
        }
        // The wolf goal's !((Wolf) entity).isTame().
        if (const auto* tamable = dynamic_cast<const TamableAnimal*>(&candidate)) {
            return !tamable->IsTame();
        }
        return true;
    }

    // ── DefendTrustedTargetGoal ────────────────────────────────────────────

    DefendTrustedTargetGoal::DefendTrustedTargetGoal(Fox* fox)
        : TargetGoal(fox, /*mustSee=*/false, /*mustReach=*/false), m_fox(fox) {}

    bool DefendTrustedTargetGoal::CanUse() {
        EntityLevel* level = m_fox->Level();
        if (!level || level->IsClientSide()) return false;
        // NearestAttackableTargetGoal's randomInterval: reducedTickDelay(10).
        if (level->Random().NextInt(ReducedTickDelay(10)) != 0) return false;

        // The first trusted identity that resolves to a living entity in this
        // level (MC EntityReference.getEntity(level, LivingEntity.class)).
        LivingEntity* trusted = nullptr;
        for (const Uuid& uuid : m_fox->GetTrustedUuids()) {
            EntityRef ref;
            ref.SetUnresolved(uuid);
            LivingEntity* e = ref.GetLiving(*level);
            if (e && e->Level() == level) { trusted = e; break; }
        }
        if (!trusted) return false;

        m_trustedLastHurt = trusted;
        Entity* hurtBy = trusted->GetLastHurtByMob();
        m_trustedLastHurtBy = dynamic_cast<LivingEntity*>(hurtBy);
        const int64_t timestamp = trusted->GetLastHurtByMobTimestamp();
        if (timestamp == m_timestamp || !m_trustedLastHurtBy) return false;

        // TargetGoal.canAttack with the goal's conditions: forCombat over the
        // follow range, the selector = TRUSTED_TARGET_SELECTOR && !trusts.
        LivingEntity& target = *m_trustedLastHurtBy;
        if (target.GetLastHurtMob() == nullptr) return false;
        if (!(target.GetLastHurtMobTimestamp() < level->GetGameTime() + 600)) return false;
        if (m_fox->Trusts(target)) return false;
        // NearestAttackableTargetGoal's targetConditions: forCombat over the
        // follow range — line of sight INCLUDED (mustSee only governs
        // canContinueToUse, never acquisition).
        const TargetingConditions conditions =
            TargetingConditions::ForCombat().Range(GetFollowDistance());
        if (!conditions.Test(m_fox, target)) return false;
        // The rest of TargetGoal.canAttack: the home restriction.
        return m_fox->IsWithinHome(target.BlockPosition());
    }

    void DefendTrustedTargetGoal::Start() {
        // MC: target = trustedLastHurtBy, then NearestAttackableTargetGoal
        // .start's mob.setTarget(target). targetMob is never set, so a target
        // something else clears (the fox sleeping, a trusted player's
        // grudge forgotten) stays cleared — CanContinueToUse has nothing to
        // fall back on.
        m_fox->SetTarget(m_trustedLastHurtBy);
        if (m_trustedLastHurt) m_timestamp = m_trustedLastHurt->GetLastHurtByMobTimestamp();
        m_fox->PlaySound(SoundEvents::FOX_AGGRO, 1.0f, 1.0f);
        m_fox->SetDefending(true);
        m_fox->WakeUp();
        TargetGoal::Start();
    }

    void DefendTrustedTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_trustedLastHurtBy == entity) m_trustedLastHurtBy = nullptr;
        if (m_trustedLastHurt == entity) m_trustedLastHurt = nullptr;
    }

    // ── FoxPreyTargetGoal ──────────────────────────────────────────────────

    FoxPreyTargetGoal::FoxPreyTargetGoal(Fox* fox, Kind kind, int randomInterval)
        : TargetGoal(fox, /*mustSee=*/false, /*mustReach=*/false),
          // MC NearestAttackableTargetGoal stores reducedTickDelay(interval).
          m_fox(fox), m_kind(kind), m_randomInterval(ReducedTickDelay(randomInterval)) {}

    bool FoxPreyTargetGoal::CanUse() {
        EntityLevel* level = m_fox->Level();
        if (!level) return false;
        if (m_randomInterval > 0
            && level->Random().NextInt(m_randomInterval) != 0) {
            return false;
        }

        const double follow = GetFollowDistance();
        // MC NearestAttackableTargetGoal.targetConditions: forCombat over the
        // follow range, line of sight included (mustSee=false only relaxes
        // canContinueToUse).
        const TargetingConditions conditions =
            TargetingConditions::ForCombat().Range(follow);

        // MC 26.3 getTargetSearchArea: the follow range on ALL axes (the old
        // 4-block vertical clamp is gone).
        AABB box = m_fox->GetAABB();
        box.min -= glm::vec3(follow, follow, follow);
        box.max += glm::vec3(follow, follow, follow);

        std::vector<Entity*> nearby;
        level->GetEntitiesInBox(box, m_fox, nearby);

        LivingEntity* best = nullptr;
        double bestDistSq = 0.0;
        for (Entity* e : nearby) {
            bool wanted = false;
            switch (m_kind) {
                case Kind::LandPrey:
                    wanted = IsStalkablePrey(*e);
                    break;
                case Kind::BabyTurtles:
                    wanted = e->GetType() == EntityTypeId::Turtle;
                    break;
                case Kind::Fish:
                    // MC: AbstractSchoolingFish only — not pufferfish.
                    wanted = e->GetType() == EntityTypeId::Cod
                          || e->GetType() == EntityTypeId::Salmon
                          || e->GetType() == EntityTypeId::TropicalFish;
                    break;
            }
            if (!wanted) continue;

            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living) continue;
            if (m_kind == Kind::BabyTurtles && !Turtle::IsBabyOnLand(*living)) {
                continue;
            }
            if (!conditions.Test(m_fox, *living)) continue;

            const double d = m_fox->DistanceToSqr(*living);
            if (!best || d < bestDistSq) { best = living; bestDistSq = d; }
        }

        m_found = best;
        return m_found != nullptr;
    }

    void FoxPreyTargetGoal::Start() {
        // MC NearestAttackableTargetGoal.start: setTarget + super only — no
        // targetMob cache to resurrect a cleared target from.
        m_fox->SetTarget(m_found);
        TargetGoal::Start();
    }

    void FoxPreyTargetGoal::ClearReferenceTo(const Entity* entity) {
        TargetGoal::ClearReferenceTo(entity);
        if (m_found == entity) m_found = nullptr;
    }

} // namespace Game
