// File: src/common/entity/Animal.cpp
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
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

    float AgeableMob::GetBbWidth() const {
        return IsBaby() ? TypeInfo().width * kBabyScale : TypeInfo().width;
    }

    float AgeableMob::GetBbHeight() const {
        return IsBaby() ? TypeInfo().height * kBabyScale : TypeInfo().height;
    }

    float AgeableMob::GetEyeHeight() const {
        return Game::GetEyeHeight(GetType(), IsBaby());
    }

    void AgeableMob::AiStep() {
        PathfinderMob::AiStep();

        // Server only: the client learns the age from synched data rather than
        // counting it itself, so both sides agree on when a baby grows up.
        if (!IsEffectiveAi()) return;

        // MC ticks forcedAgeTimer client-side, purely for the happy-villager
        // particles every 4th tick; with no particle system the countdown
        // runs where the timer lives, and only the timer's zero matters
        // (AgeUp re-arms it at 40 per feeding).
        if (m_forcedAgeTimer > 0) --m_forcedAgeTimer;

        if (m_age < 0)      ++m_age;
        else if (m_age > 0) --m_age;
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
        // reads. playEatingSound waits on the sound system at each site.
        if (IsFood(held.itemId)) {
            const int age = GetAge();
            const bool clientSide = m_level && m_level->IsClientSide();

            // MC gates this branch on ServerPlayer — i.e. server side.
            if (!clientSide && age == 0 && CanFallInLove()) {
                UsePlayerItem(held);
                SetInLove(&player);
                return UseResult::SuccessServer;
            }

            if (IsBaby()) {
                UsePlayerItem(held);
                // 10% of the remaining growth, forced (the forcedAge path).
                AgeUp(GetSpeedUpSecondsWhenFeeding(-age), /*forced=*/true);
                return UseResult::Success;
            }

            // MC: an adult that could not fall in love still swallows the
            // click client-side so nothing else claims it.
            if (clientSide) return UseResult::Consume;
        }

        return Mob::MobInteract(player, held);
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
