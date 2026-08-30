// File: src/common/entity/TamableAnimal.cpp
#include "common/entity/TamableAnimal.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"

#include <cmath>
#include <vector>

namespace Game {

    void TamableAnimal::SetOwner(const LivingEntity* owner) {
        m_ownerRef.Set(owner);
    }

    LivingEntity* TamableAnimal::GetOwner() const {
        EntityLevel* level = m_tamableSelf->Level();
        if (!level) return nullptr;
        // EntityRef checks this level and its siblings, then the player list —
        // an owner can be in another dimension. The old code only ever scanned
        // the players of THIS level.
        return const_cast<EntityRef&>(m_ownerRef).GetLiving(*level);
    }

    bool TamableAnimal::IsOwnedBy(const LivingEntity& entity) const {
        // A RESOLVED-pointer compare, matching TamableAnimal.isOwnedBy.
        // Deliberately NOT a UUID compare: an owner who is offline must read
        // as "not owned by anyone present" so the attack-target rules stay
        // correct, and comparing identities would make an absent owner match.
        return &entity == GetOwner();
    }

    void TamableAnimal::Tame(const LivingEntity& player) {
        SetTame(true, /*includeSideEffects=*/true);
        SetOwner(&player);
        // MC also fires CriteriaTriggers.TAME_ANIMAL — no advancements here.
    }

    bool TamableAnimal::UnableToMoveToOwner() const {
        // MC: isOrderedToSit() || isPassenger() || mayBeLeashed() ||
        // (owner && owner.isSpectator()). No leash system, noted in the header.
        if (IsOrderedToSit()) return true;
        if (m_tamableSelf->IsPassenger()) return true;
        const LivingEntity* owner = GetOwner();
        return owner && owner->IsSpectator();
    }

    bool TamableAnimal::ShouldTryTeleportToOwner() const {
        const LivingEntity* owner = GetOwner();
        return owner &&
               m_tamableSelf->DistanceToSqr(*owner) >= kTeleportWhenDistanceIsSq;
    }

    void TamableAnimal::TryToTeleportToOwner() {
        const LivingEntity* owner = GetOwner();
        if (!owner) return;
        EntityLevel* level = m_tamableSelf->Level();
        if (!level) return;

        // MC teleportToAroundBlockPos: up to 10 rolls in the ±3 ring, at
        // least 2 blocks out horizontally on one axis, ±1 vertically.
        const glm::ivec3 target = owner->BlockPosition();
        JavaRandom& rng = level->Random();
        for (int attempt = 0; attempt < 10; ++attempt) {
            const int xd = rng.NextInt(-3, 3);
            const int zd = rng.NextInt(-3, 3);
            if (std::abs(xd) < 2 && std::abs(zd) < 2) continue;
            const int yd = rng.NextInt(-1, 1);
            if (MaybeTeleportTo(target.x + xd, target.y + yd, target.z + zd)) {
                return;
            }
        }
    }

    bool TamableAnimal::MaybeTeleportTo(int x, int y, int z) {
        if (!CanTeleportTo(x, y, z)) return false;
        // MC snapTo(x + 0.5, y, z + 0.5, yRot, xRot) + navigation.stop(). The
        // server's position is authoritative; the tracker broadcasts the jump.
        m_tamableSelf->position = glm::dvec3(x + 0.5, y, z + 0.5);
        m_tamableSelf->GetNavigation().Stop();
        return true;
    }

    bool TamableAnimal::CanTeleportTo(int x, int y, int z) const {
        EntityLevel* level = m_tamableSelf->Level();
        if (!level) return false;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return false;

        // MC: WalkNodeEvaluator.getPathTypeStatic must say WALKABLE.
        PathfindingContext ctx;
        ctx.blocks = blocks;
        ctx.mob = m_tamableSelf;
        if (WalkNodeEvaluator::GetPathTypeStatic(ctx, x, y, z)
            != PathType::Walkable) {
            return false;
        }

        // MC also refuses a leaves block below unless canFlyToOwner() (the
        // parrot may perch). No leaves classification exists on BlockID here,
        // so a ground tamable can in principle land on a treetop MC would
        // reject — WALKABLE already filters everything else.

        // MC level.noCollision(this, boundingBox.move(delta)).
        const double half = m_tamableSelf->GetBbWidth() * 0.5;
        AABBd box;
        box.min = glm::dvec3(x + 0.5 - half, static_cast<double>(y),
                             z + 0.5 - half);
        box.max = glm::dvec3(x + 0.5 + half,
                             y + static_cast<double>(m_tamableSelf->GetBbHeight()),
                             z + 0.5 + half);
        std::vector<AABBd> colliders;
        CollectBlockColliders(box, level->Physics(), colliders);
        return colliders.empty();
    }

    void TamableAnimal::Feed(ItemStack& held, float healingFactor,
                             float defaultHeal) {
        // MC TamableAnimal.feed: usePlayerItem (creative restore is
        // HandleInteract's), then heal by nutrition * factor. playEatingSound
        // waits on the sound system.
        const auto food = held.get(DataComponents::FOOD);
        held.count -= 1;
        if (held.count <= 0) held.Clear();
        m_tamableSelf->Heal(food ? healingFactor * static_cast<float>(food->nutrition)
                                 : defaultHeal);
    }

    void TamableAnimal::BroadcastTamingResult(bool success) {
        EntityLevel* level = m_tamableSelf->Level();
        if (!level) return;
        // MC entity events 7 (success hearts) / 6 (failure smoke); the
        // implementers' HandleEntityEvent overrides answer them client-side
        // via HandleTamableEntityEvent → SpawnTamingParticles.
        level->BroadcastEntityEvent(*m_tamableSelf, success ? 7 : 6);
    }

    void TamableAnimal::SpawnTamingParticles(bool success) {
        // MC TamableAnimal.spawnTamingParticles, verbatim: 7 particles,
        // gaussian * 0.02 velocities, positions at getRandomX(1.0) /
        // getRandomY() + 0.5 / getRandomZ(1.0). Java evaluates the argument
        // list left to right, so the RNG draw order is xa, ya, za, then the
        // three position draws. (For HEART the velocity args are discarded
        // by the particle — HeartParticle's provider ignores them — but the
        // draws still happen, exactly as in MC.)
        EntityLevel* level = m_tamableSelf->Level();
        if (!level) return;
        const ParticleKind kind = success ? ParticleKind::Heart
                                          : ParticleKind::Smoke;
        JavaRandom& rng = level->Random();
        const double w = static_cast<double>(m_tamableSelf->GetBbWidth());
        const double h = static_cast<double>(m_tamableSelf->GetBbHeight());
        for (int i = 0; i < 7; ++i) {
            const double xa = rng.NextGaussian() * 0.02;
            const double ya = rng.NextGaussian() * 0.02;
            const double za = rng.NextGaussian() * 0.02;
            // MC Entity.getRandomX(spread) = x + width * (2*nextDouble()-1) * spread.
            const double px = m_tamableSelf->position.x +
                              w * (2.0 * rng.NextDouble() - 1.0);
            // MC Entity.getRandomY() = y + height * nextDouble().
            const double py = m_tamableSelf->position.y + h * rng.NextDouble() + 0.5;
            const double pz = m_tamableSelf->position.z +
                              w * (2.0 * rng.NextDouble() - 1.0);
            level->AddParticle(kind, px, py, pz, xa, ya, za);
        }
    }

} // namespace Game
