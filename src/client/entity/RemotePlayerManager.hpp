// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace Client {

    struct RemotePlayer {
        uint32_t playerId = 0;

        // Current rendered position (interpolated each tick)
        glm::vec3 position{0.0f};
        glm::vec2 rotation{0.0f}; // yaw, pitch
        bool isCrouching = false;

        // Interpolation target (set when server packet arrives)
        glm::vec3 targetPosition{0.0f};
        glm::vec2 targetRotation{0.0f};
        int lerpSteps = 0; // Remaining interpolation steps (0 = at target)
    };

    // Tracks positions of other players received from the server.
    // Uses Minecraft's interpolation system: when a position update arrives,
    // it sets a target and interpolates over 3 ticks (150ms) using
    // alpha = 1/stepsRemaining (accelerating lerp that reaches target exactly).
    class RemotePlayerManager {
    public:
        void UpdatePlayer(uint32_t id, const glm::vec3& pos, const glm::vec2& rot, bool crouching) {
            auto& rp = m_players[id];
            if (rp.playerId == 0) {
                // First update — snap directly, no previous position to lerp from
                rp.playerId = id;
                rp.position = pos;
                rp.rotation = rot;
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.lerpSteps = 0;
            } else {
                // Subsequent updates — set target and start interpolation
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.lerpSteps = 3; // Minecraft's DEFAULT_INTERPOLATION_STEPS
            }
            rp.isCrouching = crouching;
        }

        // Apply one interpolation step. Call at 20Hz (client tick rate).
        // Minecraft's InterpolationHandler.interpolate(): alpha = 1.0/steps,
        // position = lerp(alpha, current, target), steps--.
        void Tick() {
            for (auto& [id, rp] : m_players) {
                if (rp.lerpSteps > 0) {
                    float alpha = 1.0f / static_cast<float>(rp.lerpSteps);

                    // Position: linear lerp toward target
                    rp.position = glm::mix(rp.position, rp.targetPosition, alpha);

                    // Yaw: angle-wrapped lerp (Minecraft's Mth.rotLerp)
                    float yawDiff = rp.targetRotation.x - rp.rotation.x;
                    while (yawDiff > 180.0f) yawDiff -= 360.0f;
                    while (yawDiff < -180.0f) yawDiff += 360.0f;
                    rp.rotation.x += yawDiff * alpha;

                    // Pitch: regular lerp
                    rp.rotation.y = glm::mix(rp.rotation.y, rp.targetRotation.y, alpha);

                    rp.lerpSteps--;
                }
            }
        }

        void RemovePlayer(uint32_t id) {
            m_players.erase(id);
        }

        void Clear() {
            m_players.clear();
        }

        const std::unordered_map<uint32_t, RemotePlayer>& GetPlayers() const {
            return m_players;
        }

    private:
        std::unordered_map<uint32_t, RemotePlayer> m_players;
    };

    // Global instance (created/destroyed in PlatformMain)
    extern std::unique_ptr<RemotePlayerManager> g_remotePlayerManager;

} // namespace Client
