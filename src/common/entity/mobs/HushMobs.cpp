// File: src/common/entity/mobs/HushMobs.cpp
#include "common/entity/mobs/HushMobs.hpp"
#include "common/world/level/HushItems.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/EvokerGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Game {

    // ══ Hushling ═══════════════════════════════════════════════════════════

    void Hushling::CreateAttributes(AttributeMap& out) {
        // MC Endermite.createAttributes minus ATTACK_DAMAGE: MAX_HEALTH 8,
        // MOVEMENT_SPEED 0.25 on the mob base (it is a creature, not a
        // monster, so no attack attribute at all).
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,     8.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Hushling::Hushling(EntityLevel* level)
        : PathfinderMob(EntityTypeId::Hushling, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Hushling::RegisterGoals() {
        // MC Endermite.registerGoals' shape (float 1, stroll 3, look 7/8)
        // with the bite replaced by the flee and the freeze. The flee sits
        // above the freeze so a player who keeps coming breaks the stare.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        // MC AvoidEntityGoal(this, Player.class, 6.0F, 1.0D, 1.4D): walk
        // away at 1.0x, sprint at 1.4x once the player is inside 7 blocks.
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(
                                      this, HushlingFreezeGoal::kFleeDistance, 1.0, 1.4));
        m_goalSelector.AddGoal(3, std::make_unique<HushlingFreezeGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        // No target goals: a hushling never fights back.
    }

    bool Hushling::IsLookedAtBy(LivingEntity& player) {
        // MC LivingEntity.isLookingAtMe(player, 0.025, scaleByDistance=true,
        // visual=true, eyeY) — the same test Enderman::IsBeingStaredBy runs.
        const glm::vec3 viewF = Mth::ViewVector(player.xRot, player.yRot);
        glm::dvec3 view(viewF.x, viewF.y, viewF.z);
        view = glm::normalize(view);

        glm::dvec3 toMe(position.x - player.position.x,
                        GetEyeY() - player.GetEyeY(),
                        position.z - player.position.z);
        const double dist = glm::length(toMe);
        if (dist < 1.0e-8) return false;
        toMe /= dist;

        const double dot = glm::dot(view, toMe);
        if (dot <= 1.0 - 0.025 / dist) return false;
        return GetSensing().HasLineOfSight(player);
    }

    void Hushling::DropCustomDeathLoot(EntityLevel& level) {
        // 0-1 sculk (docs/the-hush.md: "drops sculk, rarely an echo shard").
        // Resolved by slug because sculk is a block item — see the header.
        static const ItemID kSculk = RecipeManager::ItemFromSlug("sculk");
        if (kSculk == Items::Air) return;
        if (level.Random().NextInt(2) == 0) {
            level.SpawnItemDrop(position, kSculk, 1);
        }
    }

    // ── HushlingFreezeGoal ─────────────────────────────────────────────────

    HushlingFreezeGoal::HushlingFreezeGoal(Hushling* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    LivingEntity* HushlingFreezeGoal::FindWatcher() const {
        EntityLevel* level = m_mob->Level();
        if (!level) return nullptr;
        LivingEntity* player = level->GetNearestPlayer(
            m_mob->position.x, m_mob->position.y, m_mob->position.z, kWatchDistance);
        if (!player || player->IsSpectator() || !player->IsAlive()) return nullptr;
        // Closer than the flee distance is the AvoidEntityGoal's business.
        const double distSq = m_mob->DistanceToSqr(*player);
        if (distSq < static_cast<double>(kFleeDistance) * kFleeDistance) return nullptr;
        return m_mob->IsLookedAtBy(*player) ? player : nullptr;
    }

    bool HushlingFreezeGoal::CanUse() {
        if (m_mob->tickCount < m_cooldownUntil) return false;
        m_watcher = FindWatcher();
        return m_watcher != nullptr;
    }

    bool HushlingFreezeGoal::CanContinueToUse() {
        if (m_holdTicks <= 0 || !m_watcher || !m_watcher->IsAlive()) return false;
        // Keep holding while the watcher stays in the freeze band, even if
        // they glance away — a hushling does not un-freeze the instant you
        // blink. The flee goal breaks it when they step in.
        const double distSq = m_mob->DistanceToSqr(*m_watcher);
        return distSq <= static_cast<double>(kWatchDistance) * kWatchDistance;
    }

    void HushlingFreezeGoal::Start() {
        // 40-80 ticks, the same window LookAtPlayerGoal holds a look.
        EntityLevel* level = m_mob->Level();
        m_holdTicks = 40 + (level ? level->Random().NextInt(41) : 20);
        m_mob->GetNavigation().Stop();
    }

    void HushlingFreezeGoal::Stop() {
        m_watcher = nullptr;
        m_holdTicks = 0;
        m_cooldownUntil = m_mob->tickCount + kCooldownTicks;
    }

    void HushlingFreezeGoal::Tick() {
        --m_holdTicks;
        m_mob->GetNavigation().Stop();
        if (m_watcher) {
            m_mob->GetLookControl().SetLookAt(m_watcher->position.x, m_watcher->GetEyeY(),
                                              m_watcher->position.z);
        }
    }

    void HushlingFreezeGoal::ClearReferenceTo(const Entity* entity) {
        if (m_watcher == entity) m_watcher = nullptr;
    }

    // ══ Echo Wraith ════════════════════════════════════════════════════════

    void EchoWraith::CreateAttributes(AttributeMap& out) {
        // The vex's own numbers (docs/the-hush.md: 14 HP, 4 damage).
        Vex::CreateAttributes(out);
    }

    EchoWraith::EchoWraith(EntityLevel* level)
        : Vex(level, EntityTypeId::EchoWraith) {
        // Vex's constructor registered ITS goal set through the virtual (a
        // base constructor cannot reach the derived override); replace it
        // with the wraith's — see the header.
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterGoals();
    }

    void EchoWraith::RegisterGoals() {
        // MC Vex.registerGoals without VexCopyOwnerTargetGoal.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<VexChargeAttackGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<VexRandomMoveGoal>(this));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f));

        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        // The wraith hunts by sound (docs/the-hush.md): a player in the cloak
        // of silence is not a candidate. Hurt it and HurtByTargetGoal above
        // still answers.
        auto hunt = std::make_unique<NearestAttackablePlayerGoal>(this, true);
        hunt->SetSelector([](Mob& self, const LivingEntity& candidate) {
            const EntityLevel* level = self.Level();
            return !level || !HushItems::IsSoundCloaked(*level, candidate);
        });
        m_targetSelector.AddGoal(3, std::move(hunt));
    }

    // ══ Silent Warden ══════════════════════════════════════════════════════

    void SilentWarden::CreateAttributes(AttributeMap& out) {
        // MC Warden.createAttributes with the boss numbers: MAX_HEALTH 300
        // (vanilla 500 — the fight is meant to be won with resonite gear),
        // FOLLOW_RANGE 32 (vanilla 24); MOVEMENT_SPEED 0.3,
        // KNOCKBACK_RESISTANCE 1, ATTACK_KNOCKBACK 1.5, ATTACK_DAMAGE 30 as
        // vanilla.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,           300.0);
        out.Register(Attribute::MovementSpeed,         0.3);
        out.Register(Attribute::KnockbackResistance,   1.0);
        out.Register(Attribute::AttackKnockback,       1.5);
        out.Register(Attribute::AttackDamage,         30.0);
        out.Register(Attribute::FollowRange,          32.0);
    }

    SilentWarden::SilentWarden(EntityLevel* level)
        : Warden(level, EntityTypeId::SilentWarden) {
        // GenericMonster's constructor applied the (absent) def — there is
        // no MobDef row for an engine-only type — so the attributes it left
        // are Monster's defaults. Re-register on top and re-fill health.
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // A boss: never despawns, never digs away (Warden::Tick keeps the
        // dig cooldown armed while persistence is required).
        SetPersistenceRequired(true);
    }

} // namespace Game
