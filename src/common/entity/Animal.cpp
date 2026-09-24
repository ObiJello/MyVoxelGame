// File: src/common/entity/Animal.cpp
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/spawn/GeneratedSpawnTags.hpp"

#include <algorithm>

namespace Game {

    // ── AgeableMob ─────────────────────────────────────────────────────────

    AgeableMob::AgeableMob(EntityTypeId type, EntityLevel* level)
        : PathfinderMob(type, level) {}

    void AgeableMob::AgeUp(int seconds, bool forced) {
        // MC AgeableMob.ageUp(seconds, forced), verbatim — including the
        // closing quirk: forced growth accumulates in forcedAge, and the
        // moment the animal reaches adulthood it is set ONTO that
        // accumulator, i.e. a breeding cooldown equal to the growth it was
        // fed. That is vanilla's behaviour, not an accident.
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

    // MC LivingEntity.getDefaultDimensions scaled by getAgeScale, or the
    // per-mob BABY_DIMENSIONS override MC 26.1 gave the remodeled babies —
    // the generated type table carries both (Game::GetBabyWidth/Height).
    float AgeableMob::BaseBbWidth() const {
        return IsBaby() ? Game::GetBabyWidth(GetType()) : TypeInfo().width;
    }

    float AgeableMob::BaseBbHeight() const {
        return IsBaby() ? Game::GetBabyHeight(GetType()) : TypeInfo().height;
    }

    float AgeableMob::BaseEyeHeight() const {
        return Game::GetEyeHeight(GetType(), IsBaby());
    }

    void AgeableMob::AiStep() {
        PathfinderMob::AiStep();

        // Server only: the client learns the age from synched data rather than
        // counting it itself, so both sides agree on when a baby grows up.
        if (IsEffectiveAi()) {
            // MC ticks forcedAgeTimer client-side, purely for the happy-villager
            // particles every 4th tick; with no particle system the countdown
            // runs where the timer lives, and only the timer's zero matters
            // (AgeUp re-arms it at 40 per feeding).
            if (m_forcedAgeTimer > 0) --m_forcedAgeTimer;

            // MC AgeableMob.aiStep: a baby counts up only while canAgeUp —
            // an age-locked baby stays exactly where the dandelion put it.
            if (IsAlive()) {
                if (CanAgeUp())     ++m_age;
                else if (m_age > 0) --m_age;
            }
        }

        // Both sides, like MC: the timer is the toggle cooldown on the
        // server and the particle clock on the client.
        TickAgeLockParticles();
    }

    // ── Age lock ───────────────────────────────────────────────────────────

    bool AgeableMob::CanBeAgeLocked(EntityTypeId type) {
        switch (type) {
            case EntityTypeId::ZombieHorse:
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::Villager:
                return false;
            default:
                return true;
        }
    }

    bool AgeableMob::CanUseGoldenDandelion(const ItemStack& held, bool isBaby, int cooldown,
                                           const Mob& mob) {
        static constexpr ItemID kGoldenDandelion = ItemRegistry::FromBlock(BlockID::GoldenDandelion);
        return held.itemId == kGoldenDandelion && isBaby && cooldown == 0 &&
               CanBeAgeLocked(mob.GetType());
    }

    void AgeableMob::SetAgeLockedData() {
        SetAgeLocked(!IsAgeLocked());
        SetAge(kBabyStartAge);
        m_ageLockParticleTimer = kAgeLockCooldownTicks;
    }

    void AgeableMob::TickAgeLockParticles() {
        // MC AgeableMob.makeAgeLockedParticle, verbatim positions: x/z
        // anywhere across the body (getRandomX(1.0)), y in the bottom fifth
        // of the body plus the body's height — i.e. just above the head —
        // and a further 0.2 up for the locked burst, whose particles drift
        // DOWN (PAUSE_MOB_GROWTH) where the unlocked ones drift up.
        if (m_ageLockParticleTimer <= 0) return;
        if ((m_ageLockParticleTimer % 2) == 0 && m_level) {
            JavaRandom& rng = m_level->Random();
            const double w = static_cast<double>(GetBbWidth());
            const double h = static_cast<double>(GetBbHeight());
            const double yOffset = m_ageLocked ? static_cast<double>(kAgeLockDownwardsParticleYOffset) : 0.0;
            const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
            const double py = position.y + h * 0.2 * rng.NextDouble() + h + yOffset;
            const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
            m_level->AddParticle(m_ageLocked ? ParticleKind::PauseMobGrowth : ParticleKind::ResetMobGrowth,
                                 px, py, pz, 0.0, 0.0, 0.0);
        }
        --m_ageLockParticleTimer;
    }

    UseResult AgeableMob::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC AgeableMob.mobInteract + the static setAgeLocked, in one place.
        if (CanUseGoldenDandelion(held, IsBaby(), m_ageLockParticleTimer, *this)) {
            const bool clientSide = m_level && m_level->IsClientSide();
            // The client spends the flower and swallows the click, as it does
            // for the feed path; the toggle itself is the server's, and the
            // client sees it through the synched lock flag (which is also
            // what starts its particle burst — see ArmAgeLockParticles).
            Animal::UsePlayerItem(held);
            if (!clientSide) {
                SetAgeLockedData();
                // A locked animal is a kept pet: it never despawns. (Animals
                // never do anyway; a locked dolphin or hoglin now doesn't
                // either.)
                if (IsAgeLocked()) SetPersistenceRequired(true);
                // MC AgeableMob.setAgeLocked: level.playSound(null,
                // blockPosition(), GOLDEN_DANDELION_[UN]USE, PLAYERS, 1, 1).
                if (m_level) {
                    m_level->PlaySound(nullptr, BlockPosition(),
                                       IsAgeLocked() ? SoundEvents::GOLDEN_DANDELION_USE
                                                     : SoundEvents::GOLDEN_DANDELION_UNUSE,
                                       SoundSource::Players, 1.0f, 1.0f);
                }
            }
            return UseResult::Success;
        }
        return PathfinderMob::MobInteract(player, held);
    }

    // ── Animal ─────────────────────────────────────────────────────────────

    Animal::Animal(EntityTypeId type, EntityLevel* level)
        : AgeableMob(type, level) {
        CreateAnimalAttributes(m_attributes);
        m_health = GetMaxHealth();

        // MC Animal's constructor: animals path AROUND fire rather than
        // treating it as merely expensive, and refuse to walk into it at all.
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);
    }

    float Animal::GetWalkTargetValue(const glm::ivec3& pos) const {
        if (!m_level) return 0.0f;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return 0.0f;

        // Grass scores a flat 10 — far above any light value — which is why
        // animals visibly congregate on grass rather than on stone or sand.
        const BlockID below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
        if (below == BlockID::Grass) return 10.0f;

        // MC: getPathfindingCostFromLightLevels — negative in the dark. Feeds
        // PathfinderMob's post-spawn CheckSpawnRules (walk value >= 0), so an
        // animal placed on non-grass needs brightness >= 12 to survive it.
        return PathfindingCostFromLightLevels(*m_level, pos.x, pos.y, pos.z);
    }

    UseResult Animal::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Animal.mobInteract, verbatim shape. The packet carries no hand
        // (mainhand-only — see InteractC2SPacket), so `held` IS the hand MC
        // reads. Each feeding plays the animal's playEatingSound.
        if (IsFood(held.itemId)) {
            const int age = GetAge();
            const bool clientSide = m_level && m_level->IsClientSide();

            // MC gates this branch on ServerPlayer — i.e. server side.
            if (!clientSide && age == 0 && CanFallInLove()) {
                UsePlayerItem(held);
                SetInLove(&player);
                PlayEatingSound();
                return UseResult::SuccessServer;
            }

            // MC: canAgeUp, not isBaby — food does nothing for an age-locked
            // baby, and the click falls through to the flower test below.
            if (CanAgeUp()) {
                UsePlayerItem(held);
                // 10% of the remaining growth, forced (the forcedAge path).
                AgeUp(GetSpeedUpSecondsWhenFeeding(-age), /*forced=*/true);
                PlayEatingSound();
                return UseResult::Success;
            }

            // MC: an adult that could not fall in love still swallows the
            // click client-side so nothing else claims it.
            if (clientSide) return UseResult::Consume;
        }

        return AgeableMob::MobInteract(player, held);
    }
    int32_t Animal::GetLoveCauseId() const {
        EntityLevel* level = Level();
        if (!level) return -1;
        const Entity* cause = m_loveCauseRef.Get(*level);
        return cause ? cause->GetId() : -1;
    }


    void Animal::SetInLove(Entity* cause) {
        m_inLove = 600;
        // MC Animal.setInLove remembers the feeding player (loveCause) — the
        // breeding-XP award reads it when the child appears.
        // Only a player is remembered as the cause, matching MC — a mob-bred
        // pair credits nobody.
        if (cause && cause->IsPlayer()) m_loveCauseRef.Set(cause);
        else                            m_loveCauseRef.Clear();
        if (m_level) m_level->BroadcastEntityEvent(*this, 18);  // heart particles
    }

    bool Animal::CanMate(const Animal& other) const {
        if (&other == this) return false;
        if (other.GetType() != GetType()) return false;
        return IsInLove() && other.IsInLove();
    }

    void Animal::SpawnChildFromBreeding(Animal& partner) {
        std::unique_ptr<Animal> baby = CreateBaby();
        if (!baby) return;

        baby->SetAge(kBabyStartAge);
        baby->position = position;
        baby->yRot = yRot;
        baby->yHeadRot = yRot;
        baby->yBodyRot = yRot;

        // The feeder, captured before ResetLove — MC's getLoveCause; this
        // parent's cause wins, the partner's fills in (MC breaks the tie the
        // same way when only one side was player-fed).
        const int32_t feeder = GetLoveCauseId() != -1 ? GetLoveCauseId()
                                                      : partner.GetLoveCauseId();

        // Both parents go on cooldown and stop courting; without the reset they
        // would immediately breed again on the next tick.
        SetAge(kParentAgeAfterBreeding);
        partner.SetAge(kParentAgeAfterBreeding);
        ResetLove();
        partner.ResetLove();

        // MC finalizeSpawnChildFromBreeding: broadcastEntityEvent(18) — the
        // 7-heart burst everyone sees when the child appears.
        if (m_level) m_level->BroadcastEntityEvent(*this, 18);

        if (m_level) m_level->AddFreshEntity(std::move(baby));

        // MC Animal.finalizeSpawnChildFromBreeding (Animal.java:224-226):
        // new ExperienceOrb(level, x, y, z, getRandom().nextInt(7) + 1) —
        // 1..7 XP per breeding. No orb entity here — the level grants it to
        // the feeder (or whoever stands within orb-follow range); see
        // ServerLevelBridge::AwardExperience for the documented deviation.
        if (m_level && !m_level->IsClientSide()) {
            m_level->AwardExperience(position, m_level->Random().NextInt(7) + 1,
                                     feeder);
        }
    }

    void Animal::HandleEntityEvent(uint8_t id) {
        if (id == 18) {
            // MC Animal.handleEntityEvent(18): 7 hearts, gaussian * 0.02
            // velocities, random points on the body — verbatim, including
            // Java's left-to-right argument evaluation (xa, ya, za, then the
            // three position draws). HeartParticle discards the velocity
            // args; the draws still happen.
            if (!m_level) return;
            JavaRandom& rng = m_level->Random();
            const double w = static_cast<double>(GetBbWidth());
            const double h = static_cast<double>(GetBbHeight());
            for (int i = 0; i < 7; ++i) {
                const double xa = rng.NextGaussian() * 0.02;
                const double ya = rng.NextGaussian() * 0.02;
                const double za = rng.NextGaussian() * 0.02;
                const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
                const double py = position.y + h * rng.NextDouble() + 0.5;
                const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
                m_level->AddParticle(ParticleKind::Heart, px, py, pz, xa, ya, za);
            }
        } else if (id == kEntityEventLoveHeart) {
            // MC Animal.aiStep's periodic courtship heart — ONE heart, same
            // per-particle shape as event 18's. In MC this is spawned
            // client-locally from inLove (not synced here); the server sends
            // the cadence as this custom byte instead (see EntityLevel.hpp).
            if (!m_level) return;
            JavaRandom& rng = m_level->Random();
            const double w = static_cast<double>(GetBbWidth());
            const double h = static_cast<double>(GetBbHeight());
            const double xa = rng.NextGaussian() * 0.02;
            const double ya = rng.NextGaussian() * 0.02;
            const double za = rng.NextGaussian() * 0.02;
            const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
            const double py = position.y + h * rng.NextDouble() + 0.5;
            const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
            m_level->AddParticle(ParticleKind::Heart, px, py, pz, xa, ya, za);
        } else {
            AgeableMob::HandleEntityEvent(id);
        }
    }

    void Animal::AiStep() {
        AgeableMob::AiStep();

        if (!IsEffectiveAi()) return;

        if (m_inLove > 0) {
            --m_inLove;
            // MC Animal.aiStep: one heart every 10 ticks while courting
            // (inLove % 10 == 0). MC spawns it client-locally; inLove is not
            // synced in this port, so the server broadcasts the custom
            // single-heart byte at MC's cadence — NOT event 18, which is the
            // 7-heart burst (see EntityLevel.hpp).
            if (m_inLove % 10 == 0 && m_level) {
                m_level->BroadcastEntityEvent(*this, kEntityEventLoveHeart);
            }
        }
    }

    int Animal::GetXpReward() const {
        if (!m_level) return 1;
        return 1 + m_level->Random().NextInt(3);
    }

    bool Animal::CheckAnimalSpawnRules(EntityLevel& level, SpawnReason reason,
                                       const glm::ivec3& pos) {
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;

        // MC: brightEnough first (short-circuited by reasons that ignore
        // light — none exist in this engine yet), then the surface tag.
        const bool brightEnough =
            IgnoresLightRequirements(reason) || IsBrightEnoughToSpawn(level, pos);

        // MC BlockTags.ANIMALS_SPAWNABLE_ON, baked by gen_spawn_tags.py —
        // exactly one block in the current data: grass_block.
        return SpawnTags::AnimalsSpawnableOn(blocks->GetBlock(pos.x, pos.y - 1, pos.z)) &&
               brightEnough;
    }

    bool Animal::IsBrightEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos) {
        // MC isBrightEnoughToSpawn is `getRawBrightness(pos, 0) > 8` — amount
        // ZERO, not skyDarken. That distinction is the whole rule: raw sky
        // light is 15 outdoors around the clock, so animals are gated on being
        // under open sky rather than on it being daytime. Passing skyDarken
        // here instead would silently stop every passive spawn at night.
        return level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 0) > 8;
    }

} // namespace Game
