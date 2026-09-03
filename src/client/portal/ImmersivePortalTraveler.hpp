// File: src/client/portal/ImmersivePortalTraveler.hpp
//
// Client-side crossing of immersive portals — the mod's
// ClientTeleportationManager, reduced to what this engine's player needs.
//
// Every frame, after physics, the frame loop asks whether the segment from
// last frame's eye to this frame's eye passed front-to-back through any
// portal of the level the player stands in. If it did, the player is moved
// at once: position, velocity and look pushed through the portal's
// transform, the active level switched if the portal leads elsewhere, and
// the server told (PortalTeleportC2S). No frame is ever rendered with the
// camera on the near side of a surface it has passed.
//
// The nudge: after a crossing the "last eye" is set a hair past the far
// surface along the new motion, so the next frame's segment cannot
// re-cross the reverse portal that now stands where the player arrived.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <optional>

namespace Client {

    class ImmersivePortalTraveler {
    public:
        struct Crossing {
            Game::Immersive::PortalId portalId = Game::Immersive::kInvalidPortalId;
            Game::DimensionId dimensionBefore = Game::DimensionId::Overworld;
            Game::DimensionId dimensionAfter  = Game::DimensionId::Overworld;
            glm::dvec3 eyeBefore{0.0};
            glm::dvec3 newEye{0.0};
            glm::dvec3 newFeet{0.0};
            glm::vec3  newVelocity{0.0f};
            float      newYaw = 0.0f;
            float      newPitch = 0.0f;
            // The player's size after the crossing: the old one times the
            // portal's scale (a mirror keeps it).
            float      newScale = 1.0f;
            // Where next frame's segment should start from (see the nudge).
            glm::dvec3 nextLastEye{0.0};
            // Unit vector away from the far surface into its side: the way
            // the player arrives facing through. An arrival correction must
            // not move against it.
            glm::dvec3 arrivalDirection{0.0, 0.0, 1.0};
        };

        // Test the eye's path against the BOUND level's teleportable portals.
        // `feet` is the physics origin, `velocity` the physics velocity,
        // yaw/pitch the camera's. Empty when nothing was crossed.
        std::optional<Crossing> Check(const glm::dvec3& lastEye, const glm::dvec3& eye,
                                      const glm::dvec3& feet, const glm::vec3& velocity,
                                      float yaw, float pitch, float scale);

        // Switch the active level (if the crossing changes dimension) and
        // tell the server. The caller applies the movement fields itself,
        // since they live on its player and camera.
        void Commit(const Crossing& crossing);

        // Crossings are ignored for a short while after one (a stale segment
        // on the arrival frame must not bounce the player straight back).
        bool OnCooldown() const;

    private:
        std::chrono::steady_clock::time_point m_cooldownUntil{};
    };

    extern ImmersivePortalTraveler g_immersivePortalTraveler;

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
