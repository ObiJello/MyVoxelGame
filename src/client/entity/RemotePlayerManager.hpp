// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstdint>
#include <cmath>

namespace Client {

    struct RemotePlayer {
        uint32_t playerId = 0;

        // Current rendered state (interpolated each tick)
        glm::vec3 position{0.0f};
        glm::vec2 rotation{0.0f}; // head yaw, pitch
        bool isCrouching = false;

        // Body yaw — follows movement direction or head with 50-degree max offset
        // (Minecraft's LivingEntity.yBodyRot)
        float bodyYaw = 0.0f;
        glm::vec3 prevPosition{0.0f}; // previous tick position for velocity estimation

        // Interpolation target (set when server packet arrives)
        glm::vec3 targetPosition{0.0f};
        glm::vec2 targetRotation{0.0f};
        int lerpSteps = 0;

        // Chat bubble
        std::string chatBubbleText;
        float chatBubbleTimer = 0.0f;
        static constexpr float CHAT_BUBBLE_DURATION = 5.0f;
    };

    class RemotePlayerManager {
    public:
        void UpdatePlayer(uint32_t id, const glm::vec3& pos, const glm::vec2& rot, bool crouching) {
            auto& rp = m_players[id];
            if (rp.playerId == 0) {
                // First update — snap everything
                rp.playerId = id;
                rp.position = pos;
                rp.rotation = rot;
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.bodyYaw = rot.x; // start body facing same as head
                rp.prevPosition = pos;
                rp.lerpSteps = 0;
            } else {
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.lerpSteps = 3;
            }
            rp.isCrouching = crouching;
        }

        // Apply one interpolation step + body rotation. Call at 20Hz.
        void Tick() {
            for (auto& [id, rp] : m_players) {
                // --- Position/rotation interpolation (Minecraft's InterpolationHandler) ---
                if (rp.lerpSteps > 0) {
                    float alpha = 1.0f / static_cast<float>(rp.lerpSteps);
                    rp.position = glm::mix(rp.position, rp.targetPosition, alpha);

                    float yawDiff = Wrap180(rp.targetRotation.x - rp.rotation.x);
                    rp.rotation.x += yawDiff * alpha;
                    rp.rotation.y = glm::mix(rp.rotation.y, rp.targetRotation.y, alpha);

                    rp.lerpSteps--;
                }

                // --- Body rotation (Minecraft's LivingEntity.tickHeadTurn) ---
                // Estimate horizontal velocity from position change
                glm::vec3 vel = rp.position - rp.prevPosition;
                float speedSq = vel.x * vel.x + vel.z * vel.z;
                rp.prevPosition = rp.position;

                float headYaw = rp.rotation.x;

                // Determine body target: movement direction when moving, head when still
                float bodyTarget;
                if (speedSq > 0.0001f) {
                    bodyTarget = glm::degrees(atan2f(vel.z, vel.x));
                } else {
                    bodyTarget = headYaw;
                }

                // Smooth body toward target at 30% per tick
                float bodyDiff = Wrap180(bodyTarget - rp.bodyYaw);
                rp.bodyYaw += bodyDiff * 0.3f;

                // Clamp: head can't rotate more than 50 degrees from body
                float headOffset = Wrap180(headYaw - rp.bodyYaw);
                if (fabsf(headOffset) > 50.0f) {
                    rp.bodyYaw += headOffset - copysignf(50.0f, headOffset);
                }
            }
        }

        void SetChatBubble(uint32_t playerId, const std::string& message) {
            auto it = m_players.find(playerId);
            if (it != m_players.end()) {
                // Strip "<Name> " prefix to show just the message in the bubble
                std::string text = message;
                if (text.size() > 2 && text[0] == '<') {
                    auto closeAngle = text.find("> ");
                    if (closeAngle != std::string::npos) {
                        text = text.substr(closeAngle + 2);
                    }
                }
                it->second.chatBubbleText = text;
                it->second.chatBubbleTimer = RemotePlayer::CHAT_BUBBLE_DURATION;
            }
        }

        void UpdateBubbles(float deltaTime) {
            for (auto& [id, rp] : m_players) {
                if (rp.chatBubbleTimer > 0.0f) {
                    rp.chatBubbleTimer -= deltaTime;
                    if (rp.chatBubbleTimer <= 0.0f) {
                        rp.chatBubbleText.clear();
                        rp.chatBubbleTimer = 0.0f;
                    }
                }
            }
        }

        void RemovePlayer(uint32_t id) { m_players.erase(id); }
        void Clear() { m_players.clear(); }
        const std::unordered_map<uint32_t, RemotePlayer>& GetPlayers() const { return m_players; }

    private:
        std::unordered_map<uint32_t, RemotePlayer> m_players;

        // Wrap angle to [-180, 180] range
        static float Wrap180(float deg) {
            deg = fmodf(deg + 180.0f, 360.0f);
            if (deg < 0.0f) deg += 360.0f;
            return deg - 180.0f;
        }
    };

    extern std::unique_ptr<RemotePlayerManager> g_remotePlayerManager;

} // namespace Client
