// File: src/client/entity/RemotePlayerManager.hpp
#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace Client {

    struct RemotePlayer {
        uint32_t playerId = 0;
        glm::vec3 position{0.0f};
        glm::vec2 rotation{0.0f}; // yaw, pitch
    };

    // Tracks positions of other players received from the server.
    // Updated on the client thread when PlayerUpdateS2C packets arrive.
    class RemotePlayerManager {
    public:
        void UpdatePlayer(uint32_t id, const glm::vec3& pos, const glm::vec2& rot) {
            auto& rp = m_players[id];
            rp.playerId = id;
            rp.position = pos;
            rp.rotation = rot;
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
