// File: src/server/player/SpectatorMode.cpp
#include "SpectatorMode.hpp"

#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../level/PortalTravel.hpp"
#include "../entity/MobManager.hpp"
#include "../entity/ServerLevelBridge.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/MenuProviders.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/portal/PortalState.hpp"
#include "common/world/block/entity/DoubleChest.hpp"

#include <cmath>

namespace Server {

    namespace Spectator {

        namespace {

            // The entity `id` names in the level the player stands in — a mob,
            // or another player's view. Only the player's own level: a camera
            // that has left it is gone as far as the spectator is concerned.
            Game::Entity* ResolveEntity(PlayerSession& session, int32_t id) {
                IntegratedServer* server = g_integratedServer.get();
                if (!server || id == ServerPlayer::kSelfCamera) return nullptr;
                ServerLevel& level = server->LevelOf(session);
                if (MobManager* mobs = level.Mobs()) {
                    if (Game::Mob* mob = mobs->Find(id)) return mob;
                }
                if (id < 0) return nullptr;
                if (ServerLevelBridge* bridge = level.MobLevel()) {
                    if (PlayerEntityView* view = bridge->GetPlayerView(static_cast<uint32_t>(id))) {
                        if (static_cast<uint32_t>(id) == session.GetPlayerId()) return nullptr;
                        return view;
                    }
                }
                return nullptr;
            }

            // MC ServerPlayerGameMode.isInRangeOfGround: the body overlaps no
            // collider and there is ground less than a block below the feet
            // (getAvailableSpaceBelow(1.0) < 1.0).
            bool IsInRangeOfGround(PlayerSession& session) {
                const ServerPlayer* player = session.GetPlayer();
                Game::World* world = session.SessionWorld();
                if (!player || !world) return false;
                const glm::dvec3 feet = player->getPosition();
                const double halfW = 0.3 * player->getScale();
                const double h     = 1.8 * player->getScale();
                Game::PhysicsContext ctx;
                ctx.blockAccess = world;
                Game::AABB body;
                body.min = glm::vec3(feet.x - halfW, feet.y, feet.z - halfW);
                body.max = glm::vec3(feet.x + halfW, feet.y + h, feet.z + halfW);
                if (Game::CollidesAt(body, ctx)) return false;
                Game::AABB below;
                below.min = glm::vec3(feet.x - halfW, feet.y - 1.0, feet.z - halfW);
                below.max = glm::vec3(feet.x + halfW, feet.y, feet.z + halfW);
                return Game::CollidesAt(below, ctx);
            }

            void SendCameraPacket(PlayerSession& session, int32_t entityId) {
                ServerConnection* conn = session.GetConnection();
                if (!conn) return;
                Network::SetCameraS2CPacket packet;
                packet.entityId = entityId;
                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::SetCameraS2C),
                                 Network::Serialization::Serialize(packet));
            }

            // MC RandomizableContainerBlockEntity.canOpen for a spectator: a
            // container whose loot table has not been rolled yet stays shut —
            // opening it would roll the loot, and a spectator changes nothing.
            // ChestBlock's combiner asks both halves of a double chest.
            bool LootNotGeneratedYet(Game::World& world, const glm::ivec3& pos) {
                auto hasLoot = [&world](const glm::ivec3& cell) {
                    auto* container = dynamic_cast<Game::BaseContainerBlockEntity*>(world.GetBlockEntity(cell));
                    return container && container->HasLootTable();
                };
                if (hasLoot(pos)) return true;
                if (auto pair = Game::FindChestPartner(world, pos)) {
                    if (hasLoot(pair->partnerPos)) return true;
                }
                return false;
            }

        } // namespace

        bool SkipsChunkLoading(const ServerPlayer& player) {
            return player.isSpectator() &&
                   !Game::Rules::GetBool(Game::Rules::Id::SpectatorsGenerateChunks);
        }

        void BroadcastGameMode(const ServerPlayer& player) {
            IntegratedServer* server = g_integratedServer.get();
            PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            if (!sessions) return;
            Network::PlayerInfoS2CPacket packet;
            packet.action   = Network::PlayerInfoS2CPacket::Action::UPDATE_GAME_MODE;
            packet.playerId = player.getPlayerId();
            packet.gameMode = static_cast<uint8_t>(player.getGameMode());
            const auto data = Network::Serialization::Serialize(packet);
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || !session->GetConnection()) continue;
                session->GetConnection()->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PlayerInfoS2C), data);
            }
        }

        bool ChangeGameMode(PlayerSession& session, GameMode mode) {
            ServerPlayer* player = session.GetPlayer();
            if (!player || player->getGameMode() == mode) return false;

            // changeGameModeForPlayer, before updatePlayerAbilities: a flier
            // who ends up close to the ground lands — except as a spectator,
            // who always flies. Leaving spectator for creative in open air
            // keeps you flying, next to the floor puts you on it.
            const bool landNearGround =
                player->isFlying() && mode != GameMode::SPECTATOR && IsInRangeOfGround(session);

            if (!player->setGameMode(mode)) return false;
            if (landNearGround && player->canFly()) player->setFlying(false);

            if (mode == GameMode::SPECTATOR) {
                // MC setGameMode(SPECTATOR): removeEntitiesOnShoulder,
                // stopRiding, stopUsingItem. Players carry nothing on their
                // shoulders and ride nothing here, so the item is the rest.
                player->stopUsingItem();
            } else {
                // Anyone else sees through their own eyes.
                SetCamera(session, ServerPlayer::kSelfCamera);
            }

            if (ServerConnection* conn = session.GetConnection()) conn->SendPlayerAbilities(*player);
            BroadcastGameMode(*player);
            return true;
        }

        void SetCamera(PlayerSession& session, int32_t entityId) {
            ServerPlayer* player = session.GetPlayer();
            if (!player) return;
            if (player->getCameraEntityId() == entityId) return;

            glm::dvec3 pos = player->getPosition();
            if (entityId != ServerPlayer::kSelfCamera) {
                Game::Entity* camera = ResolveEntity(session, entityId);
                if (!camera) return;
                pos = camera->position;
            }
            player->setCameraEntityId(entityId);

            // MC: teleportTo(camera's level, camera x/y/z, keep rotation,
            // resetCamera = false), then ClientboundSetCameraPacket. Coming
            // back to your own eyes teleports you to where you are — which is
            // where the camera last carried you — so the client's body is put
            // there before it takes control again.
            if (ServerConnection* conn = session.GetConnection()) {
                conn->Teleport(pos.x, pos.y, pos.z, player->getYaw(), player->getPitch());
            }
            SendCameraPacket(session, entityId);
            Log::Info("[Spectator] %s now sees through %s",
                      player->getName().c_str(),
                      entityId == ServerPlayer::kSelfCamera ? "their own eyes"
                                                            : ("entity " + std::to_string(entityId)).c_str());
        }

        void TickCamera(PlayerSession& session) {
            ServerPlayer* player = session.GetPlayer();
            if (!player || !player->isCameraDetached()) return;
            // Only a spectator looks through another entity.
            if (!player->isSpectator() || player->isDead()) {
                SetCamera(session, ServerPlayer::kSelfCamera);
                return;
            }
            Game::Entity* camera = ResolveEntity(session, player->getCameraEntityId());
            if (!camera || camera->IsRemoved() || !camera->IsAlive()) {
                SetCamera(session, ServerPlayer::kSelfCamera);
                return;
            }
            // absSnapTo(camera x/y/z, yRot, xRot) + chunkSource.move — the
            // session's watch set and ticket follow the position.
            player->snapTo(camera->position);
            player->setRotation(camera->yRot, camera->xRot);
            // wantsToStopRiding: the sneak key.
            if (player->IsSneaking()) {
                SetCamera(session, ServerPlayer::kSelfCamera);
            }
        }

        void HandleSpectatorAction(PlayerSession& session, const Network::SpectatorActionC2SPacket& packet) {
            ServerPlayer* player = session.GetPlayer();
            if (!player || !session.HasClientLoaded() || !player->isSpectator()) return;
            // resetLastActionTime — no idle kick here.
            if (!packet.hasEntity) return;
            Game::Entity* target = ResolveEntity(session, packet.entityId);
            if (!target || target->IsRemoved()) return;
            // Player.isWithinEntityInteractionRange(target box, 3.0): the
            // ENTITY_INTERACTION_RANGE attribute (3) plus the buffer, eye to
            // the nearest point of the box.
            const glm::dvec3 eye = player->getPosition() +
                                   glm::dvec3(0.0, player->getEyeHeight(), 0.0);
            const double range = 3.0 * player->getScale() + 3.0;
            if (target->GetAABBd().DistanceToSqr(eye) >= range * range) return;
            if (!target->IsPickable()) return;
            SetCamera(session, packet.entityId);
        }

        void HandleTeleportToEntity(PlayerSession& session, const Network::TeleportToEntityC2SPacket& packet) {
            ServerPlayer* player = session.GetPlayer();
            IntegratedServer* server = g_integratedServer.get();
            if (!player || !server || !player->isSpectator()) return;
            PlayerSessionManager* sessions = server->GetSessionManager();
            if (!sessions) return;
            auto targetSession = sessions->GetSession(packet.playerId);
            ServerPlayer* target = targetSession ? targetSession->GetPlayer() : nullptr;
            if (!target) return;

            // teleportTo(level, x, y, z, ∅, yRot, xRot, resetCamera = true).
            SetCamera(session, ServerPlayer::kSelfCamera);
            const glm::dvec3 pos = target->getPosition();
            const float yRot = target->getYaw();
            const float xRot = target->getPitch();

            if (target->getDimensionId() == player->getDimensionId()) {
                if (ServerConnection* conn = session.GetConnection()) {
                    conn->Teleport(pos.x, pos.y, pos.z, yRot, xRot);
                }
                return;
            }
            // Into the other player's level: the same crossing /tp makes.
            ServerLevel& from = server->LevelOf(session);
            ServerLevel* to = server->GetLevel(Game::DimensionFromRaw(target->getDimensionId()));
            PlayerEntityView* view = server->GetPlayerEntityView(session.GetPlayerId());
            if (!to || !view) return;
            player->setRotation(yRot, xRot);
            PortalTravel::ArriveAt(*server, from, *to, *view, pos);
        }

        bool UseItemOn(PlayerSession& session, Game::World& world, const glm::ivec3& pos) {
            ServerPlayer* player = session.GetPlayer();
            IntegratedServer* server = g_integratedServer.get();
            if (!player || !server) return false;
            const Game::BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);

            if (const auto menu = Game::BlockMenuProviderType(state)) {
                // ServerPlayer.openMenu: a provider that yields no menu tells a
                // spectator why (container.spectatorCantOpen, red).
                if (LootNotGeneratedYet(world, pos)) {
                    session.SendOverlayMessage("\xC2\xA7" "cUnable to open. Loot not generated yet.");
                    return true;
                }
                player->OpenMenu(*menu, pos);   // opened by FlushPendingMenuOpen
                return true;
            }

            if (Game::Portals::IsPortal(state.Block())) {
                // MC 26.3: the portal's destination, straight away — the
                // camera comes home first.
                if (player->isCameraDetached()) SetCamera(session, ServerPlayer::kSelfCamera);
                PlayerEntityView* view = server->GetPlayerEntityView(session.GetPlayerId());
                if (!view) return true;
                const int         dimBefore = player->getDimensionId();
                const glm::dvec3  posBefore = player->getPosition();
                PortalTravel::Traverse(*server, server->LevelOf(session), *view, state.Block(), pos);
                if (player->getDimensionId() == dimBefore && player->getPosition() == posBefore) {
                    // getPortalDestination came back null (spectator.cannot_teleport).
                    session.SendOverlayMessage("\xC2\xA7" "cUnable to teleport. The other side is not generated yet.");
                }
                return true;
            }
            return false;
        }

    } // namespace Spectator

} // namespace Server
