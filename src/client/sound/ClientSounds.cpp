// File: src/client/sound/ClientSounds.cpp
#include "client/sound/ClientSounds.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"

#include <cmath>

namespace Client::Sounds {

    void PlayAt(const glm::dvec3& pos, std::string_view event, Game::SoundSource source,
                float volume, float pitch, bool distanceDelay, int64_t seed) {
        if (event.empty()) return;
        SoundManager& manager = GetSoundManager();
        auto instance = std::make_shared<SimpleSoundInstance>(event, source, volume, pitch, seed,
                                                              pos.x, pos.y, pos.z);
        if (distanceDelay) {
            // MC: camera distance > 10 blocks → delay of distance / 40 seconds.
            const glm::dvec3 d = manager.ListenerPosition() - pos;
            const double distanceToSqr = d.x * d.x + d.y * d.y + d.z * d.z;
            if (distanceToSqr > 100.0) {
                const double delayInSeconds = std::sqrt(distanceToSqr) / 40.0;
                manager.PlayDelayed(instance, static_cast<int>(delayInSeconds * 20.0));
                return;
            }
        }
        manager.Play(instance);
    }

    void PlayLocal(const glm::dvec3& pos, std::string_view event, Game::SoundSource source,
                   float volume, float pitch, bool distanceDelay) {
        PlayAt(pos, event, source, volume, pitch, distanceDelay, SoundInstance::UnseededSeed());
    }

    void PlayEntityBound(int32_t entityId, std::string_view event, Game::SoundSource source,
                         float volume, float pitch, int64_t seed) {
        if (event.empty()) return;
        GetSoundManager().Play(std::make_shared<EntityBoundSoundInstance>(event, source, volume, pitch,
                                                                         entityId, seed));
    }

    void PlayUI(std::string_view event, float pitch, float volume) {
        if (event.empty()) return;
        GetSoundManager().Play(SimpleSoundInstance::ForUI(event, pitch, volume));
    }

    void InstallEntityResolver(const Game::ClientPlayer* localPlayer) {
        SetSoundEntityResolver([localPlayer](int32_t id, SoundEntityState& out) -> bool {
            // The local player answers to its connection id.
            if (localPlayer && g_networkClient) {
                if (auto connection = g_networkClient->GetConnection()) {
                    if (static_cast<int32_t>(connection->GetPlayerId()) == id) {
                        out.position = localPlayer->physics.position;
                        out.removed = false;
                        out.silent = false;
                        return true;
                    }
                }
            }
            if (g_clientMobManager) {
                if (const ClientMob* cm = g_clientMobManager->GetMob(id); cm && cm->mob) {
                    out.position = cm->mob->position;
                    out.removed = cm->mob->IsRemoved();
                    out.silent = cm->mob->IsSilent();
                    return true;
                }
            }
            if (g_remotePlayerManager) {
                const auto& players = g_remotePlayerManager->GetPlayers();
                const auto it = players.find(static_cast<uint32_t>(id));
                if (it != players.end()) {
                    out.position = it->second.position;
                    out.removed = false;
                    out.silent = false;
                    return true;
                }
            }
            return false;
        });
    }

} // namespace Client::Sounds
