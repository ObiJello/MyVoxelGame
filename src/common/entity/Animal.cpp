// File: src/common/entity/Animal.cpp
#include "common/entity/Animal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>

namespace Game {

    // ── AgeableMob ─────────────────────────────────────────────────────────

    AgeableMob::AgeableMob(EntityTypeId type, EntityLevel* level)
        : PathfinderMob(type, level) {}

    void AgeableMob::AgeUp(int amount) {
        const int oldAge = m_age;
        m_age = std::min(0, m_age + amount * 20);
        if (oldAge < 0 && m_age >= 0) {
            // Grew up this tick: the hitbox changes, so anything caching
            // dimensions has to be told. Nothing does yet, but the transition
            // is the natural place for that hook.
            m_age = 0;
        }
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

        return static_cast<float>(m_level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z));
    }

    void Animal::SetInLove(Entity* /*cause*/) {
        m_inLove = 600;
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

        // Both parents go on cooldown and stop courting; without the reset they
        // would immediately breed again on the next tick.
        SetAge(kParentAgeAfterBreeding);
        partner.SetAge(kParentAgeAfterBreeding);
        ResetLove();
        partner.ResetLove();

        if (m_level) m_level->AddFreshEntity(std::move(baby));
    }

    void Animal::AiStep() {
        AgeableMob::AiStep();

        if (!IsEffectiveAi()) return;

        if (m_inLove > 0) {
            --m_inLove;
            // Hearts every half second while courting.
            if (m_inLove % 10 == 0 && m_level) {
                m_level->BroadcastEntityEvent(*this, 18);
            }
        }
    }

    int Animal::GetXpReward() const {
        if (!m_level) return 1;
        return 1 + m_level->Random().NextInt(3);
    }

    bool Animal::CheckAnimalSpawnRules(EntityLevel& level, const glm::ivec3& pos) {
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;

        // MC BlockTags.ANIMALS_SPAWNABLE_ON — which, checked against
        // data/minecraft/tags/block/animals_spawnable_on.json in this repo, is
        // exactly one block: grass_block. (Other surfaces are reached through
        // per-mob overrides such as MUSHROOM_GROWS_ON, not through this tag.)
        const BlockID below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
        if (below != BlockID::Grass) return false;

        // MC isBrightEnoughToSpawn is `getRawBrightness(pos, 0) > 8` — amount
        // ZERO, not skyDarken. That distinction is the whole rule: raw sky
        // light is 15 outdoors around the clock, so animals are gated on being
        // under open sky rather than on it being daytime. Passing skyDarken
        // here instead would silently stop every passive spawn at night.
        return level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 0) > 8;
    }

} // namespace Game
