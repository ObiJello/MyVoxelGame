// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include "common/entity/PlayerColors.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstdint>
#include <cmath>
#include <cctype>

namespace Client {

    struct RemotePlayer {
        uint32_t playerId = 0;
        std::string name;  // Player name (populated from PlayerInfoS2C ADD action)
        // Stick-figure colour (server-broadcast in PlayerInfoS2C ADD). Default
        // = the historical neon green so unknown / pre-update servers behave
        // exactly as they did before colours existed.
        Game::PlayerColorId color = Game::PlayerColorId::Default;
        bool positionInitialized = false;  // True after first UpdatePlayer call

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
            if (!rp.positionInitialized) {
                // First position packet for this player — snap everything (PlayerInfo may have
                // already created the entry to set the name, so we can't use playerId == 0).
                rp.playerId = id;
                rp.position = pos;
                rp.rotation = rot;
                rp.targetPosition = pos;
                rp.targetRotation = rot;
                rp.bodyYaw = rot.x; // start body facing same as head
                rp.prevPosition = pos;
                rp.lerpSteps = 0;
                rp.positionInitialized = true;
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

        // Set the player's name. Creates an entry if the player isn't tracked yet so that
        // PlayerInfoS2C ADD can arrive before the first position update (matching MC, where
        // ClientboundPlayerInfoUpdatePacket arrives before the player entity is spawned).
        void SetPlayerName(uint32_t id, const std::string& name) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.name = name;
        }

        // Set the player's stick-figure colour. Same lazy-create semantics as
        // SetPlayerName so the colour from PlayerInfoS2C ADD lands cleanly even
        // before the first position packet arrives.
        void SetPlayerColor(uint32_t id, Game::PlayerColorId color) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.color = color;
        }

        // Case-insensitive name lookup (matching MC's PlayerList.getPlayerByName)
        const RemotePlayer* FindPlayerByName(const std::string& name) const {
            for (const auto& [id, rp] : m_players) {
                if (rp.name.size() == name.size()) {
                    bool equal = true;
                    for (size_t i = 0; i < name.size(); i++) {
                        if (std::tolower(static_cast<unsigned char>(rp.name[i])) !=
                            std::tolower(static_cast<unsigned char>(name[i]))) {
                            equal = false; break;
                        }
                    }
                    if (equal) return &rp;
                }
            }
            return nullptr;
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
