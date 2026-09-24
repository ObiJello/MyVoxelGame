// File: src/common/entity/projectile/ResonanceArrow.hpp
//
// The resonance bow's arrow (docs/the-hush.md, "Tools of the deep"): an
// ordinary Arrow that
//
//   * PIERCES one mob — the first living thing it strikes takes the arrow's
//     damage and the arrow flies on (MC AbstractArrow's piercing, at level 1:
//     piercingIgnoreEntityIds keeps the pierced mob out of the next clips);
//   * BURSTS on every impact — mob or block: knockback and kBurstDamage to
//     every living thing within kBurstRadius but the shooter, and a ring of
//     motes for the players nearby (HushItems::BroadcastSonicBurst).
//
// It is spawned as, and saved as, the vanilla `arrow` entity type: the client
// draws it as an arrow, and a reloaded one is a plain arrow — only a flying
// resonance arrow is ever special, and none survives a save in flight long
// enough to matter. Server-only behaviour (bursts and piercing both ride the
// server's clip; FindsHitEntities is false on the client).
#pragma once

#include "common/entity/projectile/Arrow.hpp"

#include <vector>

namespace Game {

    class ResonanceArrow : public Arrow {
    public:
        explicit ResonanceArrow(EntityLevel* level) : Arrow(level) {}

    protected:
        bool CanHitEntity(const Entity& entity) const override;
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHitBlockArrow(const glm::dvec3& hitPos, const glm::ivec3& blockPos) override;

    private:
        void Burst(const glm::dvec3& at);

        int m_pierceLeft = 1;
        // Entity ids already pierced (MC piercingIgnoreEntityIds): per-level
        // handles, compared only — never resolved.
        std::vector<int32_t> m_pierced;
    };

} // namespace Game
