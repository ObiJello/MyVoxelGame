// File: src/client/entity/ClientFireworks.cpp
//
// See ClientFireworks.hpp. The rocket's own tick (FireworkRocket::Tick) does
// the boost arithmetic; this answers who it is attached to.
#include "client/entity/ClientFireworks.hpp"

#include "client/entity/ClientMobManager.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/sound/SoundInstance.hpp"
#include "client/sound/SoundManager.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/projectile/FireworkRocket.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdint>

namespace Client::Fireworks {

    namespace {

        uint32_t LocalPlayerId() {
            if (!g_networkClient) return UINT32_MAX;
            auto connection = g_networkClient->GetConnection();
            return connection ? connection->GetPlayerId() : UINT32_MAX;
        }

        // MC Entity.calculateViewVector(xRot, yRot).
        glm::dvec3 ViewVector(float xRotDeg, float yRotDeg) {
            const double f = static_cast<double>(xRotDeg) * Game::Mth::kDegToRad;
            const double g = -static_cast<double>(yRotDeg) * Game::Mth::kDegToRad;
            return { std::sin(g) * std::cos(f), -std::sin(f), std::cos(g) * std::cos(f) };
        }

        // MC Entity.getHandHoldingItemAngle(FIREWORK_ROCKET): half a block
        // out to the side of the hand holding one (the left only when it is
        // in the off hand alone; every player is right-handed here).
        glm::dvec3 HandAngle(bool inMain, bool inOff, float yRotDeg) {
            if (!inMain && !inOff) return glm::dvec3(0.0);
            const bool offOnly = inOff && !inMain;
            return ViewVector(0.0f, yRotDeg + (offOnly ? -80.0f : 80.0f)) * 0.5;
        }

        class Host final : public Game::FireworkAttachHost {
        public:
            Game::ClientPlayer* player = nullptr;

            bool Query(int32_t entityId, Game::FireworkAttachState& out) override {
                if (entityId < 0) return false;
                // The local player.
                if (player && static_cast<uint32_t>(entityId) == LocalPlayerId()) {
                    const glm::vec3 look = player->lookDir;
                    const float yRot = Game::Mth::YRotFromVector(look);
                    const float xRot = Game::Mth::XRotFromVector(look);
                    out.position = player->physics.position;
                    out.deltaMovement = glm::dvec3(player->physics.velocity) / 20.0;
                    out.lookAngle = ViewVector(xRot, yRot);
                    out.fallFlying = player->physics.isFallFlying;
                    const Game::Inventory& inv = player->inventory;
                    out.handAngle = HandAngle(inv.GetSelectedItem() == Game::Items::FireworkRocket,
                                              inv.GetSlot(Game::Inventory::OFFHAND_BEGIN).itemId ==
                                                  Game::Items::FireworkRocket,
                                              yRot);
                    return true;
                }
                // Another player: as their broadcasts place them. Their own
                // client applies their boost.
                if (!Game::IsMobEntityId(entityId) && g_remotePlayerManager) {
                    const auto& players = g_remotePlayerManager->GetPlayers();
                    const auto it = players.find(static_cast<uint32_t>(entityId));
                    if (it == players.end() || !it->second.positionInitialized) return false;
                    const RemotePlayer& rp = it->second;
                    out.position = rp.position;
                    out.deltaMovement = rp.position - rp.prevPosition;
                    out.lookAngle = ViewVector(rp.rotation.y, rp.rotation.x);
                    out.fallFlying = rp.fallFlying;
                    // The held items are not broadcast; a glider boosting
                    // holds the rocket in the main hand.
                    out.handAngle = rp.fallFlying ? HandAngle(true, false, rp.rotation.x) : glm::dvec3(0.0);
                    return true;
                }
                // A mob (MC allows any LivingEntity; only players glide).
                if (g_clientMobManager) {
                    if (const ClientMob* entry = g_clientMobManager->GetMob(entityId); entry && entry->mob) {
                        const Game::Mob& mob = *entry->mob;
                        out.position = mob.position;
                        out.deltaMovement = mob.velocity;
                        out.lookAngle = ViewVector(mob.xRot, mob.yRot);
                        out.fallFlying = false;
                        out.handAngle = glm::dvec3(0.0);
                        return true;
                    }
                }
                return false;
            }

            void SetDeltaMovement(int32_t entityId, const glm::dvec3& deltaMovement) override {
                // Only the local player's movement is this client's to change
                // (MC LocalPlayer is the one entity it simulates for real).
                if (!player || entityId < 0 || static_cast<uint32_t>(entityId) != LocalPlayerId()) return;
                player->physics.velocity = glm::vec3(deltaMovement * 20.0);
            }
        };

        Host g_host;

        // MC ElytraOnPlayerSoundInstance.
        class ElytraOnPlayerSoundInstance final : public AbstractTickableSoundInstance {
        public:
            static constexpr int kDelay = 20;

            explicit ElytraOnPlayerSoundInstance(const Game::ClientPlayer& player)
                : AbstractTickableSoundInstance(Game::SoundEvents::ELYTRA_FLYING, Game::SoundSource::Players,
                                                SoundInstance::UnseededSeed()),
                  m_player(player) {
                m_looping = true;
                m_delay = 0;
                m_volume = 0.1f;
                m_x = player.physics.position.x;
                m_y = player.physics.position.y;
                m_z = player.physics.position.z;
            }

            void Tick() override {
                ++m_time;
                if (m_time <= kDelay || m_player.physics.isFallFlying) {
                    m_x = m_player.physics.position.x;
                    m_y = m_player.physics.position.y;
                    m_z = m_player.physics.position.z;
                    // deltaMovement in blocks per tick.
                    const glm::vec3 v = m_player.physics.velocity / 20.0f;
                    const float speedSqr = glm::dot(v, v);
                    m_volume = speedSqr >= 1.0e-7f ? std::clamp(speedSqr / 4.0f, 0.0f, 1.0f) : 0.0f;
                    if (m_time < kDelay) {
                        m_volume = 0.0f;
                    } else if (m_time < kDelay * 2) {
                        m_volume *= static_cast<float>(m_time - kDelay) / static_cast<float>(kDelay);
                    }
                    constexpr float kPitchVolumeThreshold = 0.8f;
                    m_pitch = m_volume > kPitchVolumeThreshold ? 1.0f + (m_volume - kPitchVolumeThreshold) : 1.0f;
                } else {
                    Stop();
                }
            }

        private:
            const Game::ClientPlayer& m_player;
            int m_time = 0;
        };

        bool g_wasFallFlying = false;

        // Diagnostics state.
        int  g_boostRequestTicks = -1;   // ticks since a boost use awaiting an attach

    } // namespace

    void Install(Game::ClientPlayer* localPlayer) {
        g_host.player = localPlayer;
        Game::FireworkRocket::SetClientAttachHost(&g_host);
        g_wasFallFlying = false;
    }

    void TickLocalPlayer(const Game::ClientPlayer& localPlayer) {
        // LocalPlayer: isFallFlying && !wasFallFlying → the glide sound.
        const bool fallFlying = localPlayer.physics.isFallFlying;
        if (fallFlying && !g_wasFallFlying) {
            GetSoundManager().Play(std::make_shared<ElytraOnPlayerSoundInstance>(localPlayer));
        }
        g_wasFallFlying = fallFlying;

        // Diagnostics: a boost use that never produced an attached rocket.
        if (g_boostRequestTicks >= 0 && ++g_boostRequestTicks > 5) {
            Log::Warning("[Elytra] boost rocket used but none attached to the local player within 5 ticks "
                         "(server fall-flying flag, spawn, or FireworkRocketDataS2C missing?)");
            g_boostRequestTicks = -1;
        }
    }

    void OnRocketData(int32_t rocketId, int32_t attachedToId, int lifetime, bool found) {
        if (attachedToId < 0 || static_cast<uint32_t>(attachedToId) != LocalPlayerId()) return;
        if (!found) {
            Log::Warning("[Elytra] rocket %d attached to the local player, but the client has no copy of it",
                         rocketId);
            return;
        }
        (void)lifetime;
        g_boostRequestTicks = -1;
    }

    void NoteBoostUse() {
        if (g_boostRequestTicks < 0) g_boostRequestTicks = 0;
    }

} // namespace Client::Fireworks
