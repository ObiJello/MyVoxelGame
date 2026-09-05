// File: src/common/entity/mobs/SulfurCube.cpp
//
// MC 26.3 SulfurCube, line by line where the engine has the seam and by the
// same observable rule where it does not (touch callbacks → per-tick
// proximity, as the slime's contact damage already does).
#include "common/entity/mobs/SulfurCube.hpp"
#include "common/entity/ai/goals/SlimeGoals.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/Animal.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneSignal.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Game {

    namespace {

        // ── The swallowable table, resolved once ────────────────────────
        //
        // GeneratedSulfurCubeArchetypes carries item SLUGS; a block this
        // engine does not have resolves to Air and drops out, so the cube
        // simply cannot swallow it.
        struct SwallowEntry {
            ItemID item;
            const SulfurCubeArchetypeDef* archetype;
        };

        const std::vector<SwallowEntry>& SwallowTable() {
            static const std::vector<SwallowEntry> table = [] {
                std::vector<SwallowEntry> out;
                out.reserve(kSulfurCubeSwallowableCount);
                for (size_t i = 0; i < kSulfurCubeSwallowableCount; ++i) {
                    const auto& row = kSulfurCubeSwallowable[i];
                    const ItemID id = RecipeManager::ItemFromSlug(std::string(row.itemSlug));
                    if (id == Items::Air) continue;
                    out.push_back({ id, &kSulfurCubeArchetypes[static_cast<size_t>(row.archetype)] });
                }
                return out;
            }();
            return table;
        }

        const std::vector<ItemID>& FoodTable() {
            static const std::vector<ItemID> table = [] {
                std::vector<ItemID> out;
                for (size_t i = 0; i < kSulfurCubeFoodCount; ++i) {
                    const ItemID id = RecipeManager::ItemFromSlug(std::string(kSulfurCubeFood[i]));
                    if (id != Items::Air) out.push_back(id);
                }
                return out;
            }();
            return table;
        }

        // MC Vec2.rotate(angleRadians).
        glm::vec2 Rotate2(const glm::vec2& v, float angleRadians) {
            const float c = std::cos(angleRadians), s = std::sin(angleRadians);
            return { v.x * c - v.y * s, v.y * c + v.x * s };
        }

        // MC Mth.wrapDegrees90: the offset to the nearest multiple of 180.
        float WrapDegrees90(float deg) {
            float d = std::fmod(deg, 180.0f);
            if (d >= 90.0f)  d -= 180.0f;
            if (d < -90.0f)  d += 180.0f;
            return d;
        }

        // MC PrimedTnt.getRandomShortFuse(fuse, random).
        int RandomShortFuse(int fuse, JavaRandom& rng) {
            return rng.NextInt(std::max(1, fuse / 4)) + fuse / 8;
        }

        // The DamageTypes in `#sulfur_cube_with_block_immune_to` this engine
        // has: everything a block would shrug off — melee, projectiles,
        // falls, falling blocks, explosions, the hot cube's own touch.
        bool ImmuneWithBlock(MobDamageSource source) {
            switch (source) {
                case MobDamageSource::PlayerAttack:
                case MobDamageSource::MobAttack:
                case MobDamageSource::Projectile:
                case MobDamageSource::Fall:
                case MobDamageSource::Explosion:
                case MobDamageSource::FallingBlock:
                case MobDamageSource::FallingAnvil:
                case MobDamageSource::FallingStalactite:
                case MobDamageSource::Stalagmite:
                    return true;
                default:
                    return false;
            }
        }

        // MC `#no_knockback` — of the immune set above, the ones that push
        // nothing (explosions push through their own sweep; falls don't).
        bool NoKnockback(MobDamageSource source) {
            switch (source) {
                case MobDamageSource::Explosion:
                case MobDamageSource::Fall:
                case MobDamageSource::Stalagmite:
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    // ── Controls ───────────────────────────────────────────────────────────

    SulfurCubeMoveControl::SulfurCubeMoveControl(SulfurCube* cube)
        : SlimeMoveControl(cube), m_cube(cube) {}

    void SulfurCubeMoveControl::Tick() {
        // MC SulfurCubeMobMoveControl.tick: no hopping with a block inside.
        if (!m_cube->HasBodyItem()) SlimeMoveControl::Tick();
    }

    SulfurCubeLookControl::SulfurCubeLookControl(SulfurCube* cube)
        : LookControl(cube), m_cube(cube) {}

    void SulfurCubeLookControl::Tick() {
        if (!m_cube->HasBodyItem()) {
            LookControl::Tick();
            return;
        }
        // MC SulfurCubeLookControl.tick: yRot -= wrapDegrees90(yRot) — snap
        // the body to 0 or 180 so the swallowed block sits square.
        const float closeAngle = WrapDegrees90(m_cube->yRot);
        m_cube->yRot -= closeAngle;
        m_cube->yHeadRot = m_cube->yRot;
        m_cube->yBodyRot = m_cube->yRot;
    }

    // ── Goals ──────────────────────────────────────────────────────────────

    // MC SulfurCube.SulfurCubeTemptGoal (TemptGoal.ForNonPathfinders, LOOK
    // flag only, canScare false, stop distance 1): turn toward the nearest
    // player within TEMPT_RANGE holding what this cube wants — a slime ball
    // as a baby, any swallowable block grown — and hop at them.
    class SulfurCubeTemptGoal : public Goal {
    public:
        explicit SulfurCubeTemptGoal(SulfurCube* cube) : m_cube(cube) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Look));
        }

        bool CanUse() override {
            if (m_calmDown > 0) { --m_calmDown; return false; }
            EntityLevel* level = m_cube->Level();
            if (!level) return false;
            const double range = m_cube->GetAttributeValue(Attribute::TemptRange);
            std::vector<LivingEntity*> players;
            level->GetPlayers(players);
            LivingEntity* best = nullptr;
            double bestDistSq = 0.0;
            for (LivingEntity* p : players) {
                if (!p->IsAlive() || p->IsSpectator()) continue;
                const uint32_t held = level->GetHeldItemId(*p);
                const bool wants = m_cube->IsBaby() ? SulfurCube::IsFood(held)
                                                    : SulfurCube::IsSwallowable(held);
                if (!wants) continue;
                const double d = m_cube->DistanceToSqr(*p);
                if (d > range * range) continue;
                if (!best || d < bestDistSq) { best = p; bestDistSq = d; }
            }
            m_player = best;
            return m_player != nullptr;
        }

        bool CanContinueToUse() override { return CanUse(); }

        void Stop() override {
            m_player = nullptr;
            // stopNavigation → CubeMobMoveControl.setWantedMovement(0).
            static_cast<SulfurCubeMoveControl&>(m_cube->GetMoveControl()).SetWantedMovement(0.0);
            m_calmDown = 100;
        }

        void Tick() override {
            if (!m_player) return;
            m_cube->GetLookControl().SetLookAt(
                m_player->position.x, m_player->GetEyePosition().y, m_player->position.z,
                static_cast<float>(m_cube->GetMaxHeadYRot() + 20),
                static_cast<float>(m_cube->GetMaxHeadXRot()));
            if (m_cube->DistanceToSqr(*m_player) < kStopDistance * kStopDistance) {
                static_cast<SulfurCubeMoveControl&>(m_cube->GetMoveControl()).SetWantedMovement(0.0);
            } else {
                // navigateTowards: lookAt(player, 10, 10) then hop that way.
                const double dx = m_player->position.x - m_cube->position.x;
                const double dz = m_player->position.z - m_cube->position.z;
                const float wantYaw = static_cast<float>(std::atan2(dz, dx) * Mth::kRadToDeg) - 90.0f;
                m_cube->yRot = Mth::ApproachDegrees(m_cube->yRot, wantYaw, 10.0f);
                static_cast<SulfurCubeMoveControl&>(m_cube->GetMoveControl())
                    .SetDirection(m_cube->yRot, true);
            }
        }

        void ClearReferenceTo(const Entity* entity) override {
            if (m_player == entity) m_player = nullptr;
        }

        const char* Name() const override { return "SulfurCubeTemptGoal"; }

    private:
        static constexpr double kStopDistance = 1.0;
        SulfurCube*   m_cube;
        LivingEntity* m_player = nullptr;
        int           m_calmDown = 0;
    };

    // MC SulfurCube.SulfurCubeSearchForItemsGoal: a grown, empty cube turns
    // toward the nearest swallowable block lying within 8 and hops at it.
    class SulfurCubeSearchForItemsGoal : public Goal {
    public:
        explicit SulfurCubeSearchForItemsGoal(SulfurCube* cube) : m_cube(cube) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Look));
        }

        bool CanUse() override {
            if (m_cube->IsBaby() || m_cube->m_pickupTimer > 0 || m_cube->HasBodyItem()) return false;
            EntityLevel* level = m_cube->Level();
            if (!level) return false;
            AABBd box = m_cube->GetAABBd();
            box.min -= glm::dvec3(8.0);
            box.max += glm::dvec3(8.0);
            std::vector<EntityLevel::NearbyItemEntity> items;
            level->GetItemEntitiesInBox(box, items);
            bool found = false;
            double bestDistSq = 0.0;
            for (const auto& it : items) {
                if (!it.canPickUp || !SulfurCube::IsSwallowable(it.itemId)) continue;
                const double d = m_cube->DistanceToSqr(it.pos.x, it.pos.y, it.pos.z);
                if (!found || d < bestDistSq) { found = true; bestDistSq = d; m_target = it.pos; }
            }
            return found;
        }

        void Tick() override {
            m_cube->GetLookControl().SetLookAt(m_target.x, m_target.y, m_target.z, 10.0f, 10.0f);
            const double dx = m_target.x - m_cube->position.x;
            const double dz = m_target.z - m_cube->position.z;
            const float wantYaw = static_cast<float>(std::atan2(dz, dx) * Mth::kRadToDeg) - 90.0f;
            m_cube->yRot = Mth::ApproachDegrees(m_cube->yRot, wantYaw, 10.0f);
            static_cast<SulfurCubeMoveControl&>(m_cube->GetMoveControl())
                .SetDirection(m_cube->yRot, true);
        }

        const char* Name() const override { return "SulfurCubeSearchForItemsGoal"; }

    private:
        SulfurCube* m_cube;
        glm::dvec3  m_target{0.0};
    };

    // ── SulfurCube ─────────────────────────────────────────────────────────

    SulfurCube::SulfurCube(EntityLevel* level) : Slime(EntityTypeId::SulfurCube, level) {
        SetMoveControl(std::make_unique<SulfurCubeMoveControl>(this));
        SetLookControl(std::make_unique<SulfurCubeLookControl>(this));
        // createSulfurCubeAttributes: Mob attributes + TEMPT_RANGE 8.
        m_attributes.SetBaseValue(Attribute::TemptRange, kTemptRange);
        // A fresh cube is the adult (setSpawnSize: 2 unless spawned a baby,
        // which SetBaby(true) → size 1 handles on the way in).
        SetSize(kMaxSize, true);
        // The slime's constructor already registered ITS goals (attack,
        // targets); this cube's set replaces them.
        Goals().Clear();
        Targets().Clear();
        RegisterGoals();
    }

    void SulfurCube::RegisterGoals() {
        // AbstractCubeMob.registerGoals: float 1, random direction 4, keep
        // jumping 5; addBehaviourGoals: tempt 2, search for items 3;
        // addTargetingGoals: nothing — a sulfur cube has no enemies.
        m_goalSelector.AddGoal(1, std::make_unique<SlimeFloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<SulfurCubeTemptGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<SulfurCubeSearchForItemsGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<SlimeRandomDirectionGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<SlimeKeepOnJumpingGoal>(this));
    }

    std::unique_ptr<Slime> SulfurCube::MakeSplitChild() {
        // setUpSplitCube: the halves are babies (size 1 → SetBaby below).
        return std::make_unique<SulfurCube>(m_level);
    }

    std::shared_ptr<SpawnGroupData>
    SulfurCube::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // AbstractCubeMob.finalizeSpawn → SulfurCube.setSpawnSize: 1 for a
        // baby, 2 otherwise (the constructor already made it 2; a SetBaby
        // before this call made it 1). Mob's finalizeSpawn, not the slime's.
        auto data = Mob::FinalizeSpawn(reason, std::move(groupData));
        SetSize(IsBaby() ? kMinSize : kMaxSize, true);
        return data;
    }

    // ── Bucket ─────────────────────────────────────────────────────────────

    void SulfurCube::SaveToBucket(ItemStack& bucket) const {
        SulfurCubeBucketData data;
        // A swallowed block is always a block item, so its registry slug is
        // the block's (ItemName's block branch).
        if (HasBodyItem()) data.bodyItem = std::string(BlockRegistry::Get(GetBodyBlock()).registrySlug);
        data.age       = m_age;
        data.ageLocked = m_ageLocked;
        data.noAi      = IsNoAi();
        bucket.components.set(DataComponents::SULFUR_CUBE_BUCKET, data);
    }

    void SulfurCube::LoadFromBucket(const SulfurCubeBucketData& data) {
        if (data.noAi) SetNoAi(true);
        SetAge(data.age);
        SetAgeLocked(data.ageLocked);
        // The age decides the size (setSpawnSize ran on create); a grown
        // cube in the bucket comes out size 2, a baby size 1.
        SetSize(IsBaby() ? kMinSize : kMaxSize, true);
        if (!data.bodyItem.empty()) {
            const ItemID item = RecipeManager::ItemFromSlug(data.bodyItem);
            if (item != Items::Air && !IsBaby()) SetBodyItem(item);
        }
        SetFromBucket(true);
    }

    // ── Ageing ─────────────────────────────────────────────────────────────

    void SulfurCube::SetBaby(bool baby) {
        SetAge(baby ? kBabyStartAge : 0);
        // A baby IS the size-1 cube and a grown one the size-2 cube (MC
        // setSpawnSize / ageBoundaryReached / setSize(1) → setBaby(true)).
        const int want = baby ? kMinSize : kMaxSize;
        // The client learns health from the wire (SetHealth precedes the
        // flags in ClientMobManager::Spawn); only the server resets it.
        if (GetSize() != want) SetSize(want, /*updateHealth=*/!m_level || !m_level->IsClientSide());
    }

    void SulfurCube::AgeUp(int seconds, bool forced) {
        const int oldAge = m_age;
        int age = std::min(0, oldAge + seconds * 20);
        const int delta = age - oldAge;
        m_age = age;
        if (forced) {
            m_forcedAge += delta;
            if (m_forcedAgeTimer == 0) m_forcedAgeTimer = 40;
        }
        if (m_age == 0) m_age = m_forcedAge;
    }

    void SulfurCube::AiStep() {
        Slime::AiStep();
        if (!IsEffectiveAi()) return;
        if (m_forcedAgeTimer > 0) --m_forcedAgeTimer;
        const bool wasBaby = IsBaby();
        if (m_age < 0)      ++m_age;
        else if (m_age > 0) --m_age;
        // ageBoundaryReached: grown → size 2.
        if (wasBaby && !IsBaby()) SetSize(kMaxSize, true);
    }

    // ── Size / rewards ─────────────────────────────────────────────────────

    void SulfurCube::SetSize(int size, bool updateHealth) {
        const int actual = std::clamp(size, 1, 127);
        // AbstractCubeMob.setSize minus the slime's size² health.
        m_size = actual;
        m_attributes.SetBaseValue(Attribute::MaxHealth, 4.0 * actual);   // setCubeMobHealth
        m_attributes.SetBaseValue(Attribute::MovementSpeed, 0.2 + 0.1 * actual);
        m_attributes.SetBaseValue(Attribute::AttackDamage, static_cast<double>(actual));
        if (updateHealth) m_health = GetMaxHealth();
        // SulfurCube.setSize: size 1 with a health reset makes it a baby.
        if (updateHealth && actual == 1 && !IsBaby()) SetAge(kBabyStartAge);
    }

    int SulfurCube::GetXpReward() const {
        if (IsBaby()) return 0;
        return 1 + (m_level ? m_level->Random().NextInt(2) : 0);
    }

    // ── The swallowed block ────────────────────────────────────────────────

    BlockID SulfurCube::GetBodyBlock() const {
        return ItemRegistry::IsBlockItem(m_bodyItem) ? ItemRegistry::ToBlock(m_bodyItem)
                                                     : BlockID::Air;
    }

    const SulfurCubeArchetypeDef* SulfurCube::ArchetypeFor(ItemID item) {
        if (item == Items::Air) return nullptr;
        for (const SwallowEntry& e : SwallowTable()) {
            if (e.item == item) return e.archetype;
        }
        return nullptr;
    }

    bool SulfurCube::IsFood(ItemID item) {
        for (ItemID f : FoodTable()) {
            if (f == item) return true;
        }
        return false;
    }

    bool SulfurCube::EquipItem(ItemID item) {
        if (IsBaby()) return false;
        if (HasBodyItem()) {
            if (m_bodyItem == item) return false;
            // The block it held pops out at the passenger attachment
            // (the cube's top).
            if (m_level && !m_level->IsClientSide()) {
                m_level->SpawnItemDrop(position + glm::dvec3(0.0, GetBbHeight(), 0.0),
                                       m_bodyItem, 1);
            }
        }
        SetBodyItem(item);
        return true;
    }

    void SulfurCube::SetBodyItem(ItemID item) {
        if (item == m_bodyItem) return;
        m_bodyItem = item;
        ApplyArchetype(item);
        needsSync = true;
    }

    void SulfurCube::ApplyArchetype(ItemID item) {
        // collectEquipmentChanges: strip the previous archetype's modifiers
        // and settings, then apply the new block's.
        m_attributes.RemoveModifier(Attribute::KnockbackResistance,
                                    ModifierId::SulfurCubeKnockbackResistance);
        m_attributes.RemoveModifier(Attribute::ExplosionKnockbackResistance,
                                    ModifierId::SulfurCubeExplosionKnockbackResistance);
        m_attributes.RemoveModifier(Attribute::Bounciness, ModifierId::SulfurCubeBounciness);
        m_attributes.RemoveModifier(Attribute::FrictionModifier, ModifierId::SulfurCubeFriction);
        m_attributes.RemoveModifier(Attribute::AirDragModifier, ModifierId::SulfurCubeAirDrag);
        m_floatsInLiquids = false;
        m_knockbackH = kDefaultKnockbackH;
        m_knockbackV = kDefaultKnockbackV;
        m_pushSoundThreshold = kDefaultPushThreshold;
        m_pushSoundCooldownSeconds = kDefaultPushCooldown;
        m_archetype = ArchetypeFor(item);

        if (m_archetype) {
            const SulfurCubeArchetypeDef& a = *m_archetype;
            const auto add = [&](Attribute attr, ModifierId id, double amount, AttributeOperation op) {
                m_attributes.AddModifier(attr, AttributeModifier{ static_cast<uint32_t>(id), amount, op });
            };
            add(Attribute::KnockbackResistance, ModifierId::SulfurCubeKnockbackResistance,
                a.knockbackResistance, AttributeOperation::AddValue);
            add(Attribute::ExplosionKnockbackResistance,
                ModifierId::SulfurCubeExplosionKnockbackResistance,
                a.explosionKnockbackResistance, AttributeOperation::AddValue);
            add(Attribute::Bounciness, ModifierId::SulfurCubeBounciness,
                a.bounciness, AttributeOperation::AddValue);
            add(Attribute::FrictionModifier, ModifierId::SulfurCubeFriction,
                a.frictionModifier - 1.0, AttributeOperation::AddMultipliedTotal);
            add(Attribute::AirDragModifier, ModifierId::SulfurCubeAirDrag,
                a.airDragModifier - 1.0, AttributeOperation::AddMultipliedTotal);
            m_floatsInLiquids = a.buoyant;
            m_knockbackH = a.knockbackHorizontal;
            m_knockbackV = a.knockbackVertical;
            m_pushSoundThreshold = a.pushSoundThreshold;
            m_pushSoundCooldownSeconds = a.pushSoundCooldown;
        }

        // A laden cube drops every goal and stops (removeAllGoals +
        // setSpeed(0)); an emptied one gets its goals back.
        SetGoalsForBodyItem(item != Items::Air);
    }

    void SulfurCube::SetGoalsForBodyItem(bool hasBody) {
        if (hasBody == m_goalsForBody) return;
        m_goalsForBody = hasBody;
        if (hasBody) {
            Goals().Clear();
            Targets().Clear();
            SetSpeed(0.0f);
        } else {
            RegisterGoals();
        }
    }

    void SulfurCube::Shear() {
        // MC shear: the block leaves through the top, and the cube ignores
        // dropped blocks for 100 ticks so it does not eat it straight back.
        const ItemID held = m_bodyItem;
        if (held == Items::Air) return;
        SetBodyItem(Items::Air);
        if (m_level && !m_level->IsClientSide()) {
            m_level->SpawnItemDrop(position + glm::dvec3(0.0, GetBbHeight(), 0.0), held, 1);
        }
        m_pickupTimer = kPickupTimerDuration;
    }

    // ── Wire ───────────────────────────────────────────────────────────────

    void SulfurCube::SetVariantByte(uint8_t v) {
        if (v >= 1 && static_cast<int>(v) != GetSize()) SetSize(v, /*updateHealth=*/false);
    }

    uint8_t SulfurCube::GetAnimStateByte() const {
        return m_maxFuse < 0 ? 0 : static_cast<uint8_t>(std::min(254, m_maxFuse) + 1);
    }

    void SulfurCube::SetAnimStateByte(uint8_t v) {
        if (v == 0) {
            m_maxFuse = -1;
            m_fuse = -1;
            return;
        }
        const int maxFuse = static_cast<int>(v) - 1;
        if (maxFuse != m_maxFuse) {
            // MC onSyncedDataUpdated(MAX_FUSE): fuse = MAX_FUSE, then the
            // client counts down on its own (tickFuse runs both sides).
            m_maxFuse = maxFuse;
            m_fuse = maxFuse;
        }
    }

    uint32_t SulfurCube::GetCarriedBlockRaw() const {
        const BlockID block = GetBodyBlock();
        return block == BlockID::Air ? 0u : BlockStates::Default(block).RawId();
    }

    void SulfurCube::SetCarriedBlockRaw(uint32_t raw) {
        if (raw == 0) { SetBodyItem(Items::Air); return; }
        const BlockID block = BlockState::FromRawId(raw).Block();
        SetBodyItem(block == BlockID::Air ? Items::Air : ItemRegistry::FromBlock(block));
    }

    // ── Fuse ───────────────────────────────────────────────────────────────

    bool SulfurCube::CanExplode() const {
        return m_archetype && m_archetype->explodes && IsAlive() && !IsPrimed();
    }

    bool SulfurCube::PrimeTime(bool imminent) {
        if (!m_archetype || !m_archetype->explodes || !IsAlive()) return false;
        if (!m_level || m_level->IsClientSide()) return false;
        if (!m_level->TntExplodes() || IsPrimed()) return false;
        const int fuse = m_archetype->explosionFuse;
        const int fuseTime = imminent ? RandomShortFuse(fuse, m_level->Random()) : fuse;
        SetInvulnerable(true);                  // setPermanentlyInvulnerable
        m_fuse = fuseTime;
        m_maxFuse = fuseTime;                   // MAX_FUSE, synched
        needsSync = true;
        return true;
    }

    void SulfurCube::TickFuse() {
        if (m_fuse > 0) --m_fuse;
        if (!m_archetype || !m_archetype->explodes) return;
        if (m_fuse == 0) {
            if (m_level && !m_level->IsClientSide()) {
                if (m_level->TntExplodes()) Explode();
                Discard();
            }
        }
    }

    void SulfurCube::Explode() {
        ExplosionParams params;
        // getY(0.0625), as the TNT.
        params.center = glm::dvec3(position.x, position.y + 0.0625 * GetBbHeight(), position.z);
        params.radius = static_cast<float>(m_archetype->explosionPower);
        params.source = this;
        params.attributedTo = this;
        params.interaction = m_level->MobGriefing() ? ExplosionInteraction::Tnt
                                                    : ExplosionInteraction::None;
        params.fire = m_archetype->explosionFire;
        m_level->QueueExplosion(params);
    }

    void SulfurCube::PrimeWhenOnPoweredPosition() {
        if (!m_level || m_level->IsClientSide() || !CanExplode()) return;
        const IBlockAccess* blocks = m_level->Blocks();
        if (blocks && HasNeighborSignal(*blocks, BlockPosition())) PrimeTime(false);
    }

    // ── Ticking ────────────────────────────────────────────────────────────

    void SulfurCube::Tick() {
        TickFuse();
        PrimeWhenOnPoweredPosition();
        if (IsRemoved()) return;
        Slime::Tick();
        if (HasBodyItem()) {
            FloatInLiquid();
            if (m_level && !m_level->IsClientSide()) PlayerPush();
        }
    }

    void SulfurCube::CustomServerAiStep() {
        Slime::CustomServerAiStep();
        if (m_pickupTimer > 0) --m_pickupTimer;
        if (m_pushSoundCooldown > 0) --m_pushSoundCooldown;
        PickUpNearbyItems();
    }

    void SulfurCube::FloatInLiquid() {
        // MC travelInFluid's extra: a buoyant laden cube bobs at the surface
        // (vibe = 0.2·sin(tick·0.4)). Without a fluid-height model the
        // immersion reads as "eyes under: fully immersed", else half a body.
        if (!m_floatsInLiquids || !IsInLiquid()) return;
        const float vibe = 0.2f * std::sin(static_cast<float>(tickCount) * 0.4f);
        const double fluidHeight = IsEyeInWater() ? GetBbHeight() : GetBbHeight() * 0.5;
        const double immersion = fluidHeight - GetBbHeight() * 0.2 + vibe;   // getFluidJumpThreshold
        if (immersion > 0.0) {
            velocity.y += std::min(1.0, immersion) * 0.03999999910593033;
            needsSync = true;
        }
    }

    void SulfurCube::PickUpNearbyItems() {
        // Mob.aiStep → pickUpItem over the items touching the cube's box
        // grown by (1, 0, 1), for a grown, empty cube past its timer.
        if (!m_level || IsBaby() || HasBodyItem() || m_pickupTimer > 0 || !IsAlive()) return;
        AABBd box = GetAABBd();
        box.min.x -= 1.0; box.max.x += 1.0;
        box.min.z -= 1.0; box.max.z += 1.0;
        std::vector<EntityLevel::NearbyItemEntity> items;
        m_level->GetItemEntitiesInBox(box, items);
        for (const auto& it : items) {
            if (!it.canPickUp || !IsSwallowable(it.itemId)) continue;
            if (m_level->TakeFromItemEntity(it.id, 1) == 1) {
                SetBodyItem(it.itemId);
                break;
            }
        }
    }

    void SulfurCube::ApplyContactDamage(LivingEntity& target) {
        if (!m_archetype || !m_archetype->contactDamage || !m_level || m_level->IsClientSide()) return;
        // The hot cube's damage type is `sulfur_cube_hot` (burning); the
        // source entity is credited only for players unless the archetype
        // says otherwise.
        Entity* source = (!m_archetype->contactDamageToSource && !target.IsPlayer()) ? nullptr : this;
        target.Hurt(MobDamageSource::Fire, m_archetype->contactDamageAmount, source);
    }

    void SulfurCube::PlayerPush() {
        // MC playerTouch(player) → playerPush: a player whose body overlaps the
        // cube's height and stands within 1.3 blocks shoves it in the
        // direction cube - player, at 2 × their known speed × 0.3 (0.16 when
        // riding), capped at 0.5, scaled by 1 - knockback resistance, with a
        // 0.3 hop while grounded.
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        const double knockback = std::max(0.0, 1.0 - GetAttributeValue(Attribute::KnockbackResistance));
        for (LivingEntity* player : players) {
            if (!player->IsAlive() || player->IsSpectator()) continue;
            const Entity* pusher = player;
            while (pusher->IsPassenger() && pusher->GetVehicle()) pusher = pusher->GetVehicle();
            const glm::dvec3 cubeToPusher = position - pusher->position;
            const double pusherFeet = pusher->position.y;
            const double cubeBottom = position.y;
            const double cubeTop = cubeBottom + GetBbHeight();
            const double pusherTop = pusherFeet + pusher->GetBbHeight();
            const double horiz = std::sqrt(cubeToPusher.x * cubeToPusher.x + cubeToPusher.z * cubeToPusher.z);
            if (!(horiz < kPushDistanceThreshold && pusherFeet <= cubeTop && pusherTop > cubeBottom)) continue;

            glm::dvec3 pushDirection(0.0);
            if (horiz > 1.0e-7) pushDirection = glm::dvec3(cubeToPusher.x / horiz, 0.0, cubeToPusher.z / horiz) * knockback;
            const float pushSpeedScale = player->IsPassenger() ? kVehiclePushSpeedScale : kPlayerPushSpeedScale;
            // getKnownSpeed: the movement the server saw this tick.
            const glm::dvec3 known = player->position - player->oldPosition;
            double playerSpeed = glm::length(known) * 2.0 * static_cast<double>(pushSpeedScale);
            playerSpeed = std::clamp(playerSpeed, 0.0, kMaxPlayerPushSpeed);
            const glm::dvec3 pushVelocity =
                glm::dvec3(pushDirection.x, onGround ? knockback * 0.30000001192092896 : 0.0, pushDirection.z)
                * playerSpeed;
            needsSync = true;
            const double thr = static_cast<double>(m_pushSoundThreshold);
            if (glm::dot(pushVelocity, pushVelocity) > thr * thr && m_pushSoundCooldown <= 0) {
                m_pushSoundCooldown = static_cast<int>(m_pushSoundCooldownSeconds * 20.0f);
                // (push sound — no mob sounds here)
            }
            velocity += pushVelocity;
            ApplyContactDamage(*player);
        }
    }

    // ── Damage ─────────────────────────────────────────────────────────────

    bool SulfurCube::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (HasBodyItem()) {
            // Fire, a burning arrow (the DIRECT entity of the blow — MC
            // source.getDirectEntity() instanceof AbstractArrow && isOnFire)
            // or, for a short fuse, an explosion lights an explosive cube.
            if (CanExplode() && !IsPrimed()) {
                const Entity* direct = HurtDirectEntity();
                const bool burningArrow = direct && direct != attacker &&
                                          dynamic_cast<const Projectile*>(direct) && direct->IsOnFire();
                if (source == MobDamageSource::Fire || burningArrow) {
                    PrimeTime(false);
                } else if (source == MobDamageSource::Explosion) {
                    PrimeTime(true);
                }
            }
            // `#sulfur_cube_with_block_immune_to`: the block takes the hit —
            // no damage, but the knockback still lands (dealDefaultKnockback
            // with the archetype's powers), unless the source pushes nothing.
            if (ImmuneWithBlock(source)) {
                if (!NoKnockback(source) && attacker && m_level && !m_level->IsClientSide()) {
                    KnockbackWithBody(*attacker, amount,
                                      attacker->position.x - position.x,
                                      attacker->position.z - position.z);
                }
                return true;
            }
        }
        m_hurtAttacker = attacker;
        m_hurtDamage = amount;
        const bool hit = Slime::Hurt(source, amount, attacker);
        m_hurtAttacker = nullptr;
        m_hurtDamage = 0.0f;
        return hit;
    }

    void SulfurCube::Knockback(double power, double dx, double dz) {
        // MC knockback(power, xd, zd, source, damage, comesFromEffect): the
        // body-item version when the source has an entity, else the
        // LivingEntity default.
        if (m_hurtAttacker && HasBodyItem()) {
            KnockbackWithBody(*m_hurtAttacker, m_hurtDamage, dx, dz);
            return;
        }
        Slime::Knockback(power, dx, dz);
    }

    void SulfurCube::KnockbackWithBody(Entity& attacker, float damage, double xd, double zd) {
        float horizontalPower = m_knockbackH;
        float verticalPower   = m_knockbackV;
        const float originalHorizontalPower = horizontalPower;
        const float originalVerticalPower   = verticalPower;

        const glm::dvec3 attackerPos = attacker.GetEyePosition();
        const glm::vec3  aimF = Mth::ViewVector(attacker.xRot, attacker.yRot);
        const glm::dvec3 aim  = glm::normalize(glm::dvec3(aimF));
        const AABBd box = GetAABBd();
        const glm::dvec3 targetCenter = (box.min + box.max) * 0.5;

        // applyHorizontalHitAngleScale: rotate the push by 1.6× the angle
        // between where the attacker aims and where the cube is — aim past
        // the cube's edge and it flies off sideways.
        glm::vec2 newAngle(static_cast<float>(xd), static_cast<float>(zd));
        {
            const glm::dvec3 toTarget = glm::normalize(targetCenter - attackerPos);
            const float angleDiff = static_cast<float>(std::atan2(
                aim.x * toTarget.z - aim.z * toTarget.x,
                aim.x * toTarget.x + aim.z * toTarget.z));
            newAngle = Rotate2(newAngle, angleDiff * kHorizontalHitAngleScale);
        }
        // applyVerticalHitAnglePowerTransfer: aiming at the cube's top or
        // bottom trades horizontal power for vertical, up to 0.5 of it.
        {
            const float halfHeight = 0.5f * GetBbHeight();
            const glm::dvec3 topPos = targetCenter + glm::dvec3(0.0, halfHeight, 0.0);
            const glm::dvec3 bottomPos = targetCenter - glm::dvec3(0.0, halfHeight, 0.0);
            const glm::dvec3 toTop = glm::normalize(topPos - attackerPos);
            const glm::dvec3 toBottom = glm::normalize(bottomPos - attackerPos);
            // Mth.clampedMap(aim.y, toTop.y, toBottom.y, -1, 1).
            float factor;
            {
                const double lo = toTop.y, hi = toBottom.y;
                double t = (hi - lo) != 0.0 ? (aim.y - lo) / (hi - lo) : 0.0;
                t = std::clamp(t, 0.0, 1.0);
                factor = static_cast<float>(-1.0 + t * 2.0);
            }
            float transferred = std::abs(factor * kVerticalHitAngleScale);
            if (factor < 0.0f) transferred = -transferred;
            horizontalPower = horizontalPower * (1.0f - transferred);
            verticalPower   = verticalPower   * (1.0f + transferred);
        }
        // applyVerticalPositionAnglePowerRotation: an attacker above or below
        // the cube tilts the push toward its own feet-to-feet line, capped so
        // neither component exceeds what it started with.
        {
            const glm::dvec3 feetToFeet = position - attacker.position;
            const double horizDist = std::sqrt(feetToFeet.x * feetToFeet.x + feetToFeet.z * feetToFeet.z);
            const float verticalPositionAngle = static_cast<float>(std::atan2(-feetToFeet.y, horizDist));
            glm::vec2 rotated = Rotate2(glm::vec2(horizontalPower, verticalPower),
                                        -verticalPositionAngle * kVerticalPositionAngleScale);
            const float hr = originalHorizontalPower > 0.0f ? std::abs(rotated.x) / originalHorizontalPower : 0.0f;
            const float vr = originalVerticalPower > 0.0f ? std::abs(rotated.y) / originalVerticalPower : 0.0f;
            const float maxRatio = std::max(hr, vr);
            if (maxRatio > 1.0f) rotated /= maxRatio;
            horizontalPower = rotated.x;
            verticalPower   = rotated.y;
        }
        xd = newAngle.x;
        zd = newAngle.y;

        // powerMultiplier = sqrt(damage) (comesFromEffect is false for a hit).
        const float powerMultiplier = std::sqrt(std::max(0.0f, damage));
        horizontalPower *= powerMultiplier;
        verticalPower   *= powerMultiplier;
        const double resistance = GetAttributeValue(Attribute::KnockbackResistance);
        horizontalPower *= static_cast<float>(1.0 - resistance);
        verticalPower   *= static_cast<float>(1.0 - resistance);
        needsSync = true;
        horizontalPower *= 0.4f;   // EXTRA_KNOCKBACK_DAMPENING → the 0.4 fold
        horizontalPower = std::clamp(horizontalPower, -128.0f, 128.0f);
        verticalPower   = std::clamp(verticalPower,   -128.0f, 128.0f);
        glm::dvec3 horizontal(xd, 0.0, zd);
        const double len = glm::length(horizontal);
        horizontal = len > 1.0e-9 ? horizontal / len * static_cast<double>(horizontalPower) : glm::dvec3(0.0);
        velocity = glm::dvec3(velocity.x - horizontal.x,
                              velocity.y + static_cast<double>(verticalPower) * 1.2,
                              velocity.z - horizontal.z);
    }

    // ── Interaction ────────────────────────────────────────────────────────

    UseResult SulfurCube::MobInteract(LivingEntity& player, ItemStack& held) {
        const bool client = m_level && m_level->IsClientSide();
        if (IsBaby()) {
            if (IsFood(held.itemId) && CanAgeUp()) {
                if (client) return UseResult::Success;
                const int age = GetAge();
                Animal::UsePlayerItem(held);
                AgeUp(AgeableMob::GetSpeedUpSecondsWhenFeeding(-age), true);
                return UseResult::Success;
            }
            return Slime::MobInteract(player, held);
        }
        if (IsPrimed()) return UseResult::Pass;

        if (CanExplode() && (held.itemId == Items::FlintAndSteel || held.itemId == Items::FireCharge)) {
            if (client) return UseResult::SuccessServer;
            if (!m_level->TntExplodes()) return UseResult::Pass;
            PrimeTime(false);
            // Flint and steel would hurtAndBreak — no durability yet (see the
            // sheep's shears note); a fire charge is consumed.
            if (held.itemId == Items::FireCharge) Animal::UsePlayerItem(held);
            return UseResult::SuccessServer;
        }
        if (held.itemId == Items::Shears && ReadyForShearing()) {
            if (client) return UseResult::Success;
            Shear();
            return UseResult::Success;
        }
        if (IsSwallowable(held.itemId)) {
            if (client) return UseResult::SuccessServer;
            const bool worked = EquipItem(held.itemId);
            if (worked) Animal::UsePlayerItem(held);
            return worked ? UseResult::SuccessServer : UseResult::Pass;
        }
        // Bucketable.bucketMobPickup: an empty bucket scoops the cube — the
        // bucket of sulfur cube carries its block, age and age lock, and the
        // cube is gone. ItemUtils.createFilledResult decides where the filled
        // bucket goes (hand, inventory or floor) and leaves the rest of a
        // stack of buckets alone.
        if (held.itemId == Items::Bucket && IsAlive()) {
            if (client) return UseResult::Success;
            ItemStack bucket(Items::SulfurCubeBucket, 1);
            SaveToBucket(bucket);
            m_level->CreateFilledResult(player, held, bucket);
            Discard();
            return UseResult::SuccessServer;
        }
        return Slime::MobInteract(player, held);
    }

} // namespace Game
