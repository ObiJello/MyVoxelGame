// File: src/client/entity/XpOrbManager.hpp
//
// Client-side view of the experience orbs the server owns — the orb sibling
// of Client::ItemEntityManager, and the same philosophy: MC's
// ExperienceOrb.tick runs its physics AND followNearbyPlayer on both sides,
// so the client simulates the full motion (including the pull toward the
// nearest player) and treats server snapshots as InterpolationHandler-style
// corrections. Simulating the pull locally is what makes the "suck" look
// continuous — the pull accelerates the orb by a few hundredths of a block
// per tick, far below any resend threshold, so an interpolate-only client
// would show orbs drifting in 20 Hz stair-steps.
#pragma once

#include "common/entity/ExperienceOrb.hpp"
#include "common/physics/Physics.hpp"
#include "common/core/JavaRandom.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

namespace Client {

    struct ClientXpOrb {
        Game::ExperienceOrb sim;

        // Server correction (MC InterpolationHandler) — identical mechanism
        // to ClientItemEntity's.
        int        interpSteps = 0;
        glm::dvec3 interpTarget{0.0};
        glm::dvec3 prevTickPos{0.0};

        // Previous-tick position for sub-tick render blending.
        glm::dvec3 renderPrevPosition{0.0};

        // Drives the render colour cycle (MC ExperienceOrbRenderer's
        // sin(age/2) red/blue pulse). Client-local, like the item bob age.
        float ageTicks = 0.0f;

        bool initialized = false;
    };

    // An orb flying into the player who collected it — the orb flavour of
    // MC's ItemPickupParticle, which serves both entity kinds.
    struct XpOrbPickupAnim {
        int        value    = 0;      // picks the sprite
        float      ageTicks = 0.0f;   // frozen colour phase

        glm::dvec3 startPos{0.0};

        uint32_t   targetPlayerId = 0;
        bool       targetSeeded = false;
        glm::dvec3 targetPos{0.0};
        glm::dvec3 targetPosOld{0.0};

        int life = 0;
    };

    class XpOrbManager {
    public:
        // Spawn or full-refresh — re-sending a known id updates in place.
        void Spawn(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                   int32_t value);

        void Move(int32_t id, const glm::dvec3& pos, const glm::vec3& vel);

        void Remove(int32_t id) { m_entities.erase(id); }
        void Clear() { m_entities.clear(); m_pickups.clear(); }

        // A player absorbed this orb (TakeItemEntityS2C with an orb-range
        // id). Starts the fly-to-player animation and removes the local copy
        // — MC removes the orb on its first take packet even when the server
        // keeps draining a merged count.
        void TakeOrb(int32_t orbId, uint32_t playerId);

        // 20 Hz: local physics (with the player pull), pending corrections,
        // pickup animations. Takes the local player's feet because both the
        // pull and the pickup flight need it, and the local player is not in
        // any entity map.
        void Tick(const glm::dvec3& localPlayerPos);

        const std::unordered_map<int32_t, ClientXpOrb>& GetEntities() const {
            return m_entities;
        }
        const std::vector<XpOrbPickupAnim>& GetPickups() const { return m_pickups; }
        size_t Count() const { return m_entities.size(); }

        // Same 3-tick snap as the item pickup animation.
        static constexpr int kPickupLifeTicks = 3;

    private:
        static constexpr int    kInterpSteps    = 3;
        static constexpr double kSnapDistanceSq = 4.0 * 4.0;

        // followingPlayerId value that means "the local player" — remote
        // players use their (uint32) connection ids, which are non-negative,
        // so any negative sentinel except -1 (nobody) is free.
        static constexpr int64_t kLocalPlayerFollowId = -2;

        void InterpolateTo(ClientXpOrb& e, const glm::dvec3& target);

        // Pickup-flight target — chest height of the collector, local player
        // fallback, exactly like ItemEntityManager::ResolveTarget.
        glm::dvec3 ResolveTarget(uint32_t playerId,
                                 const glm::dvec3& localPlayerPos) const;

        std::unordered_map<int32_t, ClientXpOrb> m_entities;
        std::vector<XpOrbPickupAnim> m_pickups;

        // Feeds the lava pop in the shared tick. Divergence from the server's
        // rolls is healed by the periodic sync, as in MC.
        Game::JavaRandom m_random{0x9E3779B9LL};
    };

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    extern XpOrbManager* g_xpOrbManager;

} // namespace Client
