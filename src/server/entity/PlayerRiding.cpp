// File: src/server/entity/PlayerRiding.cpp
#include "server/entity/PlayerRiding.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/PlayerRideable.hpp"
#include "common/entity/PlayerRideableJumping.hpp"
#include "common/entity/Inventory.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/entity/decoration/Cushion.hpp"
#include "common/entity/decoration/BlockAttachedEntity.hpp"
#include "server/level/LevelEntityStore.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "server/world/storage/NBTParser.hpp"
#include "common/entity/vehicle/Boat.hpp"
#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/PlayerMountS2CPacket.hpp"
#include "common/network/packets/game/VehiclePackets.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace Server::PlayerRiding {

    namespace {

        // Per player (connection id): the seat the riding system last put
        // the body in — a body found anywhere else was moved by something
        // other than the vehicle (a /tp, a portal) — the keys the client last
        // reported, and handleMoveVehicle's good-position bookkeeping.
        struct RiderState {
            bool       hasSeat = false;
            glm::dvec3 lastSeat{0.0};
            bool       hasInput = false;
            Game::PlayerInput input;
            // MC ServerGamePacketListenerImpl.lastVehicle / vehicleFirstGood /
            // vehicleLastGood / vehiclePositionLastResetAt.
            int32_t    lastVehicleId = 0;
            glm::dvec3 vehicleFirstGood{0.0};
            glm::dvec3 vehicleLastGood{0.0};
            int64_t    tickCount = 0;
            int64_t    vehiclePositionLastResetAt = 0;
            // CarryAcross: the vehicle to sit on again once the player
            // stands in `remountDimension` (0 = none), and until when.
            int32_t    remountVehicleId = 0;
            int        remountDimension = 0;
            int64_t    remountDeadline = 0;
        };

        // Recursive: the state is read from inside entity callbacks (a mount's
        // steering rule reads the rider's keys, LastInput) that can run while
        // a riding step holds it.
        std::recursive_mutex s_stateMutex;
        std::unordered_map<uint32_t, RiderState> s_states;

        RiderState& StateOf(uint32_t connectionId) {
            return s_states[connectionId];   // caller holds s_stateMutex
        }

        IntegratedServer* Server() { return g_integratedServer.get(); }

        PlayerEntityView* ViewOf(PlayerSession& session) {
            IntegratedServer* server = Server();
            if (!server) return nullptr;
            ServerLevelBridge* bridge = server->LevelOf(session).MobLevel();
            return bridge ? bridge->GetPlayerView(session.GetConnectionId()) : nullptr;
        }

        void Broadcast(uint32_t playerId, int32_t vehicleId) {
            Network::PlayerMountS2CPacket packet;
            packet.playerId  = playerId;
            packet.vehicleId = vehicleId;
            const auto data = Network::Serialization::Serialize(packet);
            IntegratedServer* server = Server();
            PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            if (!sessions) return;
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                if (ServerConnection* conn = session->GetConnection()) {
                    conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerMountS2C), data);
                }
            }
        }

        bool IsPortalBlock(Game::BlockID id) {
            // MC BlockTags.PORTALS.
            return id == Game::BlockID::NetherPortal || id == Game::BlockID::EndPortal ||
                   id == Game::BlockID::EndGateway;
        }

        // The first clear spot rising from `at` for a player's box — MC
        // Level.findFreePosition, as dismountVehicle's removed branch uses it.
        glm::dvec3 FreePositionFrom(const Game::EntityLevel& level, const glm::dvec3& at, double scale) {
            const Game::IBlockAccess* blocks = level.Blocks();
            if (!blocks) return at;
            Game::PhysicsContext context;
            context.blockAccess = blocks;
            const double hw = 0.3 * scale, h = 1.8 * scale;
            for (int step = 0; step <= 16; ++step) {
                const glm::dvec3 p = at + glm::dvec3(0.0, 0.0625 * step, 0.0);
                const Game::AABBd box = Game::AABBd::FromMinMax(p - glm::dvec3(hw, 0.0, hw), p + glm::dvec3(hw, h, hw));
                if (!Game::CollidesAt(box, context)) return p;
            }
            return at;
        }

        // MC LivingEntity.dismountVehicle's target for the player behind
        // `view`, off `vehicle`.
        glm::dvec3 DismountTarget(PlayerEntityView& view, Game::Entity& vehicle, const ServerPlayer& player) {
            if (view.IsRemoved()) return player.getPosition();
            const Game::EntityLevel* level = vehicle.Level();
            const Game::IBlockAccess* blocks = level ? level->Blocks() : nullptr;
            bool inPortal = false;
            if (blocks) {
                const glm::ivec3 cell = vehicle.BlockPosition();
                inPortal = IsPortalBlock(blocks->GetBlock(cell.x, cell.y, cell.z));
            }
            if (!vehicle.IsRemoved() && !inPortal) {
                return vehicle.GetDismountLocationForPassenger(view);
            }
            // The vehicle is gone (or stands in a portal): the higher of the
            // two feet, then the first free spot above.
            const glm::dvec3 here = player.getPosition();
            const glm::dvec3 start(here.x, std::max(here.y, vehicle.position.y), here.z);
            return level ? FreePositionFrom(*level, start, std::max(0.05f, player.getScale())) : start;
        }

        // The ride is over: the player's half of the link, the broadcast,
        // the teleport to `target`.
        void FinishDismount(PlayerSession& session, ServerPlayer& player, const glm::dvec3& target,
                            Game::Entity* oldVehicle) {
            player.clearVehicle();
            {
                std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                RiderState& state = StateOf(session.GetConnectionId());
                state.hasSeat = false;
                state.lastVehicleId = 0;
            }
            Broadcast(session.GetPlayerId(), 0);
            // LivingEntity.dismountTo → ServerPlayer.dismountTo: the teleport.
            player.teleport(target);
            if (ServerConnection* conn = session.GetConnection()) {
                conn->Teleport(target.x, target.y, target.z, player.getYaw(), player.getPitch());
            }
            if (PlayerEntityView* view = ViewOf(session)) {
                view->position = target;
                view->oldPosition = target;
                // Entity.removeVehicle: ENTITY_DISMOUNT at the vehicle.
                if (oldVehicle) {
                    if (Game::ILevelWrite* world = oldVehicle->Level() ? oldVehicle->Level()->MutableBlocks() : nullptr) {
                        world->GameEvent(view, Game::GameEventId::EntityDismount, oldVehicle->position);
                    }
                }
            }
            if (auto* seat = dynamic_cast<Game::PlayerRideable*>(oldVehicle)) seat->OnPlayerRiderDismounted();
        }

        // The ride ends where the player now is (a teleport, a dimension
        // change): the link is cut without moving anyone.
        void EndInPlace(PlayerSession& session) {
            ServerPlayer* player = session.GetPlayer();
            if (!player) return;
            // The view that rides is the one in the VEHICLE's level (after a
            // dimension change the player already has a new one elsewhere).
            PlayerEntityView* view = nullptr;
            if (player->isPassenger()) {
                IntegratedServer* server = Server();
                ServerLevel* level = server ? server->GetLevel(Game::DimensionFromRaw(
                                                  static_cast<int8_t>(player->getVehicleDimensionId())))
                                            : nullptr;
                ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
                view = bridge ? bridge->GetPlayerView(session.GetConnectionId()) : nullptr;
            }
            if (!view || !view->GetVehicle()) view = ViewOf(session);
            Game::Entity* old = view ? view->GetVehicle() : nullptr;
            if (view && old) view->RemoveVehicle();   // no dismount hook: nobody moves
            if (!player->isPassenger()) return;
            player->clearVehicle();
            {
                std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                RiderState& state = StateOf(session.GetConnectionId());
                state.hasSeat = false;
                state.lastVehicleId = 0;
            }
            Broadcast(session.GetPlayerId(), 0);
            if (auto* seat = dynamic_cast<Game::PlayerRideable*>(old)) seat->OnPlayerRiderDismounted();
        }

        bool InputResolver(const Game::Entity& player, Game::PlayerInput& out) {
            return LastInput(static_cast<uint32_t>(player.GetId()), out);
        }

        // MC Input.moveVector: (left - right, forward - backward), normalised
        // (Vec2.normalized: zero below a length of 1e-4).
        void MoveVector(const Game::PlayerInput& in, float& xxa, float& zza) {
            float x = (in.left ? 1.0f : 0.0f) - (in.right ? 1.0f : 0.0f);
            float z = (in.forward ? 1.0f : 0.0f) - (in.backward ? 1.0f : 0.0f);
            const float len = std::sqrt(x * x + z * z);
            if (len < 1.0e-4f) { x = 0.0f; z = 0.0f; } else { x /= len; z /= len; }
            xxa = x;
            zza = z;
        }

        // The server's view of the player in a mount's first seat: the keys
        // from their PlayerInputC2S, their view, what they hold.
        bool RiderControlResolver(const Game::LivingEntity& mount, Game::LivingEntity::RiderControl& out) {
            const auto& riders = mount.GetPassengers();
            if (riders.empty() || !riders.front() || !riders.front()->IsPlayer()) return false;
            const auto* view = dynamic_cast<const PlayerEntityView*>(riders.front());
            const ServerPlayer* player = view ? view->GetPlayer() : nullptr;
            if (!player) return false;
            Game::PlayerInput in;
            if (LastInput(static_cast<uint32_t>(view->GetId()), in)) {
                MoveVector(in, out.xxa, out.zza);
                out.jumping = in.jump;
                out.sprinting = in.sprint;
                out.shift = in.shift;
            }
            out.yRot = player->getYaw();
            out.xRot = player->getPitch();
            const Game::Inventory& inventory = player->getInventory();
            out.mainHandItem = inventory.GetSlot(Game::Inventory::HotbarToIndex(inventory.GetSelectedSlot())).itemId;
            out.offHandItem  = inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN).itemId;
            out.playerId = static_cast<int32_t>(player->getPlayerId());
            out.local = false;
            return true;
        }

        // MC containsInvalidValues.
        bool ContainsInvalidValues(const glm::dvec3& p, float yRot, float xRot) {
            return std::isnan(p.x) || std::isnan(p.y) || std::isnan(p.z) || !std::isfinite(yRot) ||
                   !std::isfinite(xRot);
        }

        void SendMoveVehicle(PlayerSession& session, const Game::Entity& vehicle) {
            ServerConnection* conn = session.GetConnection();
            if (!conn) return;
            Network::MoveVehicleS2CPacket packet;
            packet.position = vehicle.position;
            packet.yRot = vehicle.yRot;
            packet.xRot = vehicle.xRot;
            conn->SendPacket(static_cast<uint8_t>(Network::PacketId::MoveVehicleS2C),
                             Network::Serialization::Serialize(packet));
        }

        // MC isEntityCollidingWithAnythingNew: the moved box meets a block
        // the old box did not.
        bool CollidingWithAnythingNew(const Game::Entity& vehicle, const Game::AABBd& oldBox,
                                      const glm::dvec3& target) {
            const Game::EntityLevel* level = vehicle.Level();
            if (!level || !level->Blocks()) return false;
            const glm::dvec3 shift = target - vehicle.position;
            Game::AABBd newBox = vehicle.GetAABBd();
            newBox.min += shift + glm::dvec3(1.0e-5);
            newBox.max += shift - glm::dvec3(1.0e-5);
            Game::AABBd oldDeflated = oldBox;
            oldDeflated.min += glm::dvec3(1.0e-5);
            oldDeflated.max -= glm::dvec3(1.0e-5);
            Game::PhysicsContext context = level->Physics();
            std::vector<Game::AABBd> colliders;
            Game::CollectBlockColliders(newBox, context, colliders, nullptr);
            for (const Game::AABBd& c : colliders) {
                if (!c.Intersects(newBox)) continue;
                if (!c.Intersects(oldDeflated)) return true;
            }
            return false;
        }

        // The root of `vehicle`'s tree when it leaves with its player (see
        // RootVehicleLeavingWith): exactly one player anywhere on it, and a
        // root that is saved as an entity — not a cushion, which belongs to
        // its block and stays.
        Game::Mob* LeavingRoot(Game::Entity& vehicle) {
            Game::Entity* root = &vehicle;
            while (root->GetVehicle()) root = root->GetVehicle();
            int players = 0;
            std::vector<const Game::Entity*> stack(root->GetPassengers().begin(), root->GetPassengers().end());
            while (!stack.empty()) {
                const Game::Entity* e = stack.back();
                stack.pop_back();
                if (!e) continue;
                if (e->IsPlayer()) ++players;
                for (const Game::Entity* p : e->GetPassengers()) stack.push_back(p);
            }
            if (players != 1) return nullptr;
            auto* mob = dynamic_cast<Game::Mob*>(root);
            if (!mob || mob->IsPlayer() || mob->IsRemoved() || !mob->CanSerialize()) return nullptr;
            if (dynamic_cast<Game::BlockAttachedEntity*>(mob)) return nullptr;
            return mob;
        }

        // MC EntityType.loadEntityRecursive + ServerLevel.addWithUUID for a
        // RootVehicle: the entity, then each passenger mounted on it.
        Game::Mob* RestoreTree(const ::World::NBTTagCompound& tag, MobManager& mobs, ServerLevelBridge& bridge,
                               std::vector<Game::Mob*>& made) {
            Game::EntityTypeId type{};
            if (Game::Anvil::ClassifyEntity(tag, type) != Game::Anvil::EntityKind::Mob) return nullptr;
            std::unique_ptr<Game::Mob> mob = MakeMobForLoad(type, static_cast<Game::EntityLevel*>(&bridge));
            if (!mob) return nullptr;
            Game::Anvil::ApplyMobNbt(tag, *mob);
            if (!Game::UuidIsNil(mob->GetUuid()) && mobs.HasUuid(mob->GetUuid())) return nullptr;
            const int32_t id = mobs.Add(std::move(mob));
            Game::Mob* vehicle = id != 0 ? mobs.Find(id) : nullptr;
            if (!vehicle) return nullptr;
            made.push_back(vehicle);
            if (auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Passengers"))) {
                for (const auto& element : list->value) {
                    auto child = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                    if (!child) continue;
                    if (Game::Mob* rider = RestoreTree(*child, mobs, bridge, made)) {
                        rider->StartRiding(*vehicle, /*force=*/true);
                    }
                }
            }
            return vehicle;
        }

        // MC PlayerList.placeNewPlayer's RootVehicle half.
        void RestoreRootVehicle(PlayerSession& session, const std::shared_ptr<::World::NBTTagCompound>& rootTag) {
            IntegratedServer* server = Server();
            if (!server || !rootTag) return;
            auto entity = std::dynamic_pointer_cast<::World::NBTTagCompound>(rootTag->GetTag("Entity"));
            if (!entity) return;
            ServerLevel& level = server->LevelOf(session);
            MobManager* mobs = level.Mobs();
            ServerLevelBridge* bridge = level.MobLevel();
            if (!mobs || !bridge) return;
            Game::Uuid attach{};
            bool hasAttach = false;
            if (auto words = std::dynamic_pointer_cast<::World::NBTTagIntArray>(rootTag->GetTag("Attach"));
                words && words->value.size() == 4) {
                const int32_t w[4] = {words->value[0], words->value[1], words->value[2], words->value[3]};
                attach = Game::UuidFromIntArray(w);
                hasAttach = true;
            }
            std::vector<Game::Mob*> made;
            RestoreTree(*entity, *mobs, *bridge, made);
            if (made.empty()) return;
            Game::Mob* seat = nullptr;
            if (hasAttach) {
                for (Game::Mob* m : made) {
                    if (m->GetUuid() == attach) { seat = m; break; }
                }
            }
            if (seat && StartRiding(session, *seat, /*force=*/true)) return;
            // "Couldn't reattach entity to player": the vehicle and all on it
            // are discarded.
            Log::Warning("[Riding] couldn't reattach entity to player %u", session.GetPlayerId());
            for (auto it = made.rbegin(); it != made.rend(); ++it) {
                if ((*it)->IsPassenger()) (*it)->RemoveVehicle();
                (*it)->Remove(Game::RemovalReason::Discarded);
            }
        }

    } // namespace

    // ── Seating ────────────────────────────────────────────────────────────

    void PositionRider(Game::Entity& vehicle, PlayerEntityView& view) {
        ServerPlayer* player = view.GetPlayer();
        if (!player) return;
        const glm::dvec3 seat = Game::PlayerSeatFeetOn(vehicle, &view,
                                                       static_cast<int32_t>(player->getPlayerId()),
                                                       std::max(0.05f, player->getScale()));
        // snapTo: no anti-cheat distance gate, fall distance reset (MC
        // LivingEntity.rideTick's resetFallDistance).
        player->snapTo(seat);
        view.position = seat;
        view.ResetFallDistance();
        std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
        RiderState& state = StateOf(static_cast<uint32_t>(view.GetId()));
        state.hasSeat = true;
        state.lastSeat = seat;
    }

    bool StartRiding(PlayerSession& session, Game::Entity& vehicle, bool force) {
        ServerPlayer* player = session.GetPlayer();
        IntegratedServer* server = Server();
        const auto refuse = [&](const char* why) {
            Log::Info("[Riding] player %u cannot ride %s #%d: %s", session.GetPlayerId(),
                      std::string(vehicle.TypeInfo().slug).c_str(), vehicle.GetId(), why);
            return false;
        };
        if (!player || !server) return false;
        if (vehicle.IsRemoved()) return refuse("vehicle removed");
        // Not while dead or in bed, nor as a spectator.
        if (player->isDead() || player->isSleeping() || player->getGameMode() == GameMode::SPECTATOR) {
            return refuse("dead, asleep or spectating");
        }
        // canRide: !isShiftKeyDown (a player's boarding cooldown is always 0 —
        // Player.removeVehicle clears it).
        if (!force && player->IsSneaking()) return refuse("sneaking");

        ServerLevel& level = server->LevelOf(session);
        ServerLevelBridge* bridge = level.MobLevel();
        PlayerEntityView* view = bridge ? bridge->GetPlayerView(session.GetConnectionId()) : nullptr;
        if (!view) return refuse("no view in the player's level");
        if (vehicle.Level() != bridge) return refuse("vehicle in another level");
        // Entity.startRiding: already on this one; no cycles.
        if (view->GetVehicle() == &vehicle) return refuse("already riding it");
        if (!vehicle.CouldAcceptPassenger()) return refuse("vehicle accepts no passenger now");
        if (!force && !vehicle.CanAddPassenger(*view)) return refuse("no free seat");

        // `if (this.isPassenger()) this.stopRiding()` — off the old vehicle,
        // in place: the new seat positions the player next.
        if (player->isPassenger() || view->IsPassenger()) EndInPlace(session);

        if (!view->StartRiding(vehicle, /*force=*/true)) return refuse("startRiding refused");
        player->setVehicle(vehicle.GetId(), Game::DimensionToRaw(level.Dimension()));
        {
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            RiderState& state = StateOf(session.GetConnectionId());
            state.lastVehicleId = vehicle.GetId();
            state.vehicleFirstGood = vehicle.position;
            state.vehicleLastGood = vehicle.position;
        }
        PositionRider(vehicle, *view);
        // ServerPlayer.startRiding's ClientboundSetPassengersPacket — here the
        // player half (the tracker sends the vehicle's list), and
        // GameEvent.ENTITY_MOUNT.
        Broadcast(session.GetPlayerId(), vehicle.GetId());
        if (Game::ILevelWrite* world = level.World()) {
            world->GameEvent(view, Game::GameEventId::EntityMount, vehicle.position);
        }
        return true;
    }

    bool StartRiding(PlayerSession& session, Game::Cushion& cushion) {
        return StartRiding(session, static_cast<Game::Entity&>(cushion), false);
    }

    bool StartRiding(PlayerSession& session, Game::Mob& vehicle, Game::PlayerRideable& seat) {
        (void)seat;
        ServerPlayer* player = session.GetPlayer();
        if (!player || vehicle.IsDeadOrDying()) return false;
        // MC AbstractHorse.doPlayerRide: the player turns to the mount, then
        // startRiding. An equine seats one — no mob passenger, no player.
        if (vehicle.IsVehicle()) {
            Log::Info("[Riding] player %u cannot ride %s #%d: already carrying a rider", session.GetPlayerId(),
                      std::string(vehicle.TypeInfo().slug).c_str(), vehicle.GetId());
            return false;
        }
        const float yaw = player->getYaw(), pitch = player->getPitch();
        player->setRotation(vehicle.yRot, vehicle.xRot);
        if (!StartRiding(session, static_cast<Game::Entity&>(vehicle), false)) {
            player->setRotation(yaw, pitch);
            return false;
        }
        return true;
    }

    bool ValidateSeat(Game::Cushion& cushion) {
        return cushion.IsVehicle();
    }

    // ── Getting off ────────────────────────────────────────────────────────

    void StopRiding(PlayerSession& session) {
        ServerPlayer* player = session.GetPlayer();
        if (!player || !player->isPassenger()) return;
        PlayerEntityView* view = ViewOf(session);
        if (view && view->GetVehicle()) {
            // The view's StopRiding lands in OnViewDismounted.
            view->StopRiding();
            if (player->isPassenger()) {
                // The hook did not finish it (no session found for the view):
                // finish here, in place.
                FinishDismount(session, *player, player->getPosition(), nullptr);
            }
            return;
        }
        // The link is already broken (the vehicle went without a Remove): put
        // the player down where they are, risen to a free spot.
        const glm::dvec3 target = view && view->Level()
            ? FreePositionFrom(*view->Level(), player->getPosition(), std::max(0.05f, player->getScale()))
            : player->getPosition();
        FinishDismount(session, *player, target, nullptr);
    }

    void OnViewDismounted(PlayerEntityView& view, Game::Entity& oldVehicle) {
        ServerPlayer* player = view.GetPlayer();
        IntegratedServer* server = Server();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!player || !sessions) return;
        std::shared_ptr<PlayerSession> session = sessions->GetSessionByConnection(static_cast<uint32_t>(view.GetId()));
        if (!session || session->GetPlayer() != player) return;
        if (!player->isPassenger()) return;
        const glm::dvec3 target = DismountTarget(view, oldVehicle, *player);
        FinishDismount(*session, *player, target, &oldVehicle);
    }

    // ── The riding tick ────────────────────────────────────────────────────

    void Tick(PlayerSession& session) {
        ServerPlayer* player = session.GetPlayer();
        IntegratedServer* server = Server();
        if (!player || !server) return;
        {
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            ++StateOf(session.GetConnectionId()).tickCount;
        }
        // The vehicle the player logged out on, once they stand in their
        // level (their view exists from the level's first tick with them).
        if (player->hasPendingRootVehicle() && ViewOf(session)) {
            RestoreRootVehicle(session, player->takePendingRootVehicle());
        }
        // The vehicle this player crossed a portal with: seated again once
        // the player's view and the vehicle share the new level.
        {
            int32_t vehicleId = 0;
            int dimension = 0;
            int64_t deadline = 0, now = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                RiderState& state = StateOf(session.GetConnectionId());
                vehicleId = state.remountVehicleId;
                dimension = state.remountDimension;
                deadline = state.remountDeadline;
                now = state.tickCount;
            }
            if (vehicleId != 0) {
                ServerLevel& here = server->LevelOf(session);
                bool done = now > deadline || Game::DimensionToRaw(here.Dimension()) != dimension ||
                            player->isPassenger();
                if (!done && ViewOf(session)) {
                    Game::Mob* vehicle = here.Mobs() ? here.Mobs()->Find(vehicleId) : nullptr;
                    if (vehicle && !vehicle->IsRemoved()) {
                        StartRiding(session, static_cast<Game::Entity&>(*vehicle), /*force=*/true);
                        done = true;
                    }
                }
                if (done) {
                    std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                    StateOf(session.GetConnectionId()).remountVehicleId = 0;
                }
            }
        }
        if (!player->isPassenger()) return;

        // A dimension change took the player out of the vehicle's level.
        ServerLevel& level = server->LevelOf(session);
        if (Game::DimensionToRaw(level.Dimension()) != player->getVehicleDimensionId()) {
            EndInPlace(session);
            return;
        }

        PlayerEntityView* view = level.MobLevel() ? level.MobLevel()->GetPlayerView(session.GetConnectionId())
                                                  : nullptr;
        Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
        if (!vehicle || vehicle->GetId() != player->getVehicleId()) {
            // The view was rebuilt (a return to this level) or the vehicle
            // went without a Remove: re-attach to it if it is still here,
            // otherwise the rider comes off where they sit.
            Game::Mob* byId = level.Mobs() ? level.Mobs()->Find(player->getVehicleId()) : nullptr;
            if (view && byId && !byId->IsRemoved() && view->GetVehicle() != byId &&
                view->StartRiding(*byId, /*force=*/true)) {
                vehicle = byId;
            } else {
                StopRiding(session);
                return;
            }
        }
        // ServerLevel.tickPassenger: a vehicle that is gone drops its rider.
        if (vehicle->IsRemoved()) {
            view->StopRiding();
            return;
        }
        // A teleport moved the rider off the seat: the ride is over where
        // they now stand.
        {
            bool movedAway = false;
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            const RiderState& state = StateOf(session.GetConnectionId());
            if (state.hasSeat) {
                // A real teleport (/tp, a portal the player took alone) moves
                // the body well away; anything within half a block is the seat
                // itself (a knockback's residue, float round-off).
                const glm::dvec3 off = player->getPosition() - state.lastSeat;
                movedAway = glm::dot(off, off) > 0.25;
            }
            if (movedAway) {
                Log::Info("[Riding] player %u was moved off the seat (teleport) — the ride ends",
                          session.GetPlayerId());
                // EndInPlace takes the lock itself.
                s_stateMutex.unlock();
                EndInPlace(session);
                s_stateMutex.lock();
                return;
            }
        }
        // MC LivingEntity.baseTick: a rider whose eyes are under water (not
        // in a bubble column) gets off a vehicle in #dismounts_underwater.
        if (view->IsEyeInWater() && vehicle->Level() && vehicle->Level()->Blocks()) {
            const glm::dvec3 eye = view->GetEyePosition();
            const Game::BlockID atEye = vehicle->Level()->Blocks()->GetBlock(
                static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)),
                static_cast<int>(std::floor(eye.z)));
            if (atEye != Game::BlockID::BubbleColumn &&
                Game::DataTags::HasTag(Game::DataTags::Registry::EntityType, vehicle->TypeInfo().slug,
                                       "minecraft:dismounts_underwater")) {
                view->StopRiding();
                return;
            }
        }
        // Death, a bed, spectator mode; Player.rideTick: a sneak
        // (wantsToStopRiding = isShiftKeyDown) gets up.
        if (player->isDead() || player->isSleeping() || player->getGameMode() == GameMode::SPECTATOR ||
            player->IsSneaking()) {
            view->StopRiding();
            return;
        }
        // MC tickPlayer's vehicleFirstGood / vehicleLastGood for a vehicle
        // this player drives.
        {
            Game::Entity* root = vehicle;
            while (root->GetVehicle()) root = root->GetVehicle();
            // Asked BEFORE taking the state lock: a mount's controlling
            // passenger reads this player's keys through LastInput, which
            // takes the same lock (the pig-saddle hang: a self-deadlock).
            const bool controls = root->GetControllingPassenger() == view;
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            RiderState& state = StateOf(session.GetConnectionId());
            if (controls) {
                state.lastVehicleId = root->GetId();
                state.vehicleFirstGood = root->position;
                state.vehicleLastGood = root->position;
            } else {
                state.lastVehicleId = 0;
            }
        }
        PositionRider(*vehicle, *view);
    }

    // ── Leaving the server ─────────────────────────────────────────────────

    void OnDisconnect(PlayerSession& session) {
        ServerPlayer* player = session.GetPlayer();
        const uint32_t connectionId = session.GetConnectionId();
        if (!player || !player->isPassenger()) {
            Forget(connectionId);
            return;
        }
        PlayerEntityView* view = ViewOf(session);
        Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
        if (!vehicle) {
            player->clearVehicle();
            Forget(connectionId);
            return;
        }
        Game::Mob* root = LeavingRoot(*vehicle);
        const bool leavesWithPlayer = root != nullptr;
        if (leavesWithPlayer) {
            // MC PlayerList.remove: "Removing player mount" — the player gets
            // off, and the vehicle and everything on it are removed
            // (UNLOADED_WITH_PLAYER). Its NBT is in the player's file already
            // (RootVehicle, written by the save that runs before this).
            view->RemoveVehicle();
            player->clearVehicle();
            std::vector<Game::Entity*> chain;
            std::vector<Game::Entity*> stack{root};
            while (!stack.empty()) {
                Game::Entity* e = stack.back();
                stack.pop_back();
                if (!e || e->IsPlayer()) continue;
                chain.push_back(e);
                for (Game::Entity* p : e->GetPassengers()) stack.push_back(p);
            }
            // Riders first, so no dismount runs off a vehicle mid-removal.
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                Game::Entity* e = *it;
                if (e->IsPassenger()) e->RemoveVehicle();
                e->Remove(Game::RemovalReason::UnloadedToChunk);
            }
        } else {
            // Another player is aboard: off, at the vehicle's dismount spot
            // (no broadcast — the session manager's lock is held).
            const glm::dvec3 target = DismountTarget(*view, *vehicle, *player);
            view->RemoveVehicle();
            player->clearVehicle();
            player->teleport(target);
        }
        Forget(connectionId);
    }

    const Game::Mob* RootVehicleLeavingWith(const ServerPlayer& player, Game::Uuid& attach) {
        if (!player.isPassenger()) return nullptr;
        IntegratedServer* server = Server();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) return nullptr;
        const std::shared_ptr<PlayerSession> session = sessions->GetSession(player.getPlayerId());
        if (!session) return nullptr;
        ServerLevel* level = server->GetLevel(Game::DimensionFromRaw(
            static_cast<int8_t>(player.getVehicleDimensionId())));
        ServerLevelBridge* bridge = level ? level->MobLevel() : nullptr;
        PlayerEntityView* view = bridge ? bridge->GetPlayerView(session->GetConnectionId()) : nullptr;
        Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
        if (!vehicle) return nullptr;
        const Game::Mob* root = LeavingRoot(*vehicle);
        if (root) attach = vehicle->GetUuid();
        return root;
    }

    void SendMountsTo(ServerConnection& connection) {
        IntegratedServer* server = Server();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) return;
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session) continue;
            const ServerPlayer* player = session->GetPlayer();
            if (!player || !player->isPassenger()) continue;
            Network::PlayerMountS2CPacket packet;
            packet.playerId  = session->GetPlayerId();
            packet.vehicleId = player->getVehicleId();
            connection.SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerMountS2C),
                                  Network::Serialization::Serialize(packet));
        }
    }

    Game::Entity* VehicleOf(PlayerSession& session) {
        PlayerEntityView* view = ViewOf(session);
        return view ? view->GetVehicle() : nullptr;
    }

    int32_t PlayerIdOfView(const Game::Entity& view) {
        const auto* v = dynamic_cast<const PlayerEntityView*>(&view);
        const ServerPlayer* player = v ? v->GetPlayer() : nullptr;
        return player ? static_cast<int32_t>(player->getPlayerId()) : -1;
    }

    // ── Packets ────────────────────────────────────────────────────────────

    void HandlePlayerInput(PlayerSession& session, uint8_t keys) {
        const Game::PlayerInput input = Game::PlayerInput::Unpack(keys);
        {
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            RiderState& state = StateOf(session.GetConnectionId());
            state.input = input;
            state.hasInput = true;
        }
        // MC handlePlayerInput: setShiftKeyDown once the client has loaded.
        if (session.HasClientLoaded()) {
            if (ServerPlayer* player = session.GetPlayer()) player->setSneaking(input.shift);
        }
    }

    bool LastInput(uint32_t connectionId, Game::PlayerInput& out) {
        std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
        const auto it = s_states.find(connectionId);
        if (it == s_states.end() || !it->second.hasInput) return false;
        out = it->second.input;
        return true;
    }

    void HandlePaddleBoat(PlayerSession& session, bool left, bool right, bool reverse) {
        // MC handlePaddleBoat: the controlled vehicle, when it is a boat.
        PlayerEntityView* view = ViewOf(session);
        Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
        if (!vehicle || vehicle->GetControllingPassenger() != view) return;
        if (auto* boat = dynamic_cast<Game::Boat*>(vehicle)) boat->SetPaddleState(left, right, reverse);
    }

    void HandleMoveVehicle(PlayerSession& session, const Network::MoveVehicleC2SPacket& packet) {
        // MC ServerGamePacketListenerImpl.handleMoveVehicle.
        ServerPlayer* player = session.GetPlayer();
        ServerConnection* conn = session.GetConnection();
        if (!player || !conn) return;
        if (ContainsInvalidValues(packet.position, packet.yRot, packet.xRot)) {
            Log::Warning("[Riding] player %u sent an invalid vehicle move", session.GetPlayerId());
            return;
        }
        if (conn->UpdateAwaitingTeleport() || !session.HasClientLoaded()) return;
        PlayerEntityView* view = ViewOf(session);
        if (!view || !view->GetVehicle()) return;
        Game::Entity* vehicle = view->GetVehicle();
        while (vehicle->GetVehicle()) vehicle = vehicle->GetVehicle();   // getRootVehicle

        RiderState snapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            snapshot = StateOf(session.GetConnectionId());
        }

        auto* driven = dynamic_cast<Game::VehicleEntity*>(vehicle);
        auto* drivenMount = driven ? nullptr : dynamic_cast<Game::LivingEntity*>(vehicle);
        if ((driven || drivenMount) && vehicle->GetControllingPassenger() == view &&
            vehicle->GetId() == snapshot.lastVehicleId) {
            const glm::dvec3 oldPos = vehicle->position;
            const double targetX = std::clamp(packet.position.x, -3.0E7, 3.0E7);
            const double targetY = std::clamp(packet.position.y, -2.0E7, 2.0E7);
            const double targetZ = std::clamp(packet.position.z, -3.0E7, 3.0E7);
            const float targetYRot = Game::Mth::WrapDegrees(packet.yRot);
            const float targetXRot = Game::Mth::WrapDegrees(packet.xRot);

            glm::dvec3 d = glm::dvec3(targetX, targetY, targetZ) - snapshot.vehicleFirstGood;
            const double expectedDist = glm::dot(vehicle->velocity, vehicle->velocity);
            double movedDist = glm::dot(d, d);
            if (movedDist - expectedDist > 100.0 && !conn->IsSingleplayerOwner()) {
                Log::Warning("[Riding] %s (vehicle of player %u) moved too quickly! %.2f,%.2f,%.2f",
                             std::string(vehicle->TypeInfo().slug).c_str(), session.GetPlayerId(), d.x, d.y, d.z);
                SendMoveVehicle(session, *vehicle);
                return;
            }

            const Game::AABBd oldBox = vehicle->GetAABBd();
            d = glm::dvec3(targetX, targetY, targetZ) - snapshot.vehicleLastGood;
            const bool vehicleRestsOnSomething = vehicle->onGround;   // verticalCollisionBelow
            if (driven) {
                driven->ServerMoveFromDriver(d);
            } else {
                // vehicle.move(MoverType.PLAYER, …) for a steered mount.
                drivenMount->Move(d);
            }
            const double oyDist = d.y;
            glm::dvec3 miss = glm::dvec3(targetX, targetY, targetZ) - vehicle->position;
            // MC: `if (yDist > -0.5 || yDist < 0.5) yDist = 0.0` — always.
            miss.y = 0.0;
            movedDist = glm::dot(miss, miss);
            bool fail = false;
            if (movedDist > 0.0625) {
                fail = true;
                Log::Warning("[Riding] %s (vehicle of player %u) moved wrongly! %.3f",
                             std::string(vehicle->TypeInfo().slug).c_str(), session.GetPlayerId(), std::sqrt(movedDist));
            }
            bool oldBoxFree = true;
            if (fail) {
                Game::PhysicsContext context = vehicle->Level()->Physics();
                oldBoxFree = !Game::CollidesAt(oldBox, context);
            }
            if ((fail && oldBoxFree) ||
                CollidingWithAnythingNew(*vehicle, oldBox, glm::dvec3(targetX, targetY, targetZ))) {
                vehicle->position = oldPos;
                vehicle->yRot = targetYRot;
                vehicle->xRot = targetXRot;
                SendMoveVehicle(session, *vehicle);
                return;
            }
            // absSnapTo(target, rotation).
            vehicle->position = glm::dvec3(targetX, targetY, targetZ);
            vehicle->yRot = targetYRot;
            vehicle->xRot = targetXRot;
            const glm::dvec3 clientDelta = vehicle->position - oldPos;
            // setOnGroundWithMovement + doCheckFallDamage.
            if (driven) {
                driven->ServerLandFromDriver(clientDelta.y, packet.onGround);
            } else {
                // Entity::Move above already ran checkFallDamage for the
                // move (the engine's move always does); the client's ground
                // flag is the one that stands.
                drivenMount->onGround = packet.onGround;
                // The steered mount's yaw is the body's too (tickRidden
                // turned it on the client).
                drivenMount->yBodyRot = drivenMount->yRot;
                drivenMount->yHeadRot = drivenMount->yRot;
            }
            // (clientVehicleIsFloating / the flying kick: vehicles here
            // float on water by design and flight is never kicked.)
            (void)oyDist;
            (void)vehicleRestsOnSomething;
            vehicle->needsSync = true;
            {
                std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                StateOf(session.GetConnectionId()).vehicleLastGood = vehicle->position;
            }
            // The riders follow the vehicle at once (their bodies are the
            // chunk-loading and tracking centre).
            for (Game::Entity* rider : std::vector<Game::Entity*>(vehicle->GetPassengers())) {
                if (auto* riderView = dynamic_cast<PlayerEntityView*>(rider)) PositionRider(*vehicle, *riderView);
            }
            return;
        }
        if (vehicle->GetControllingPassenger() != view) {
            const glm::dvec3 off = packet.position - vehicle->position;
            if (snapshot.tickCount - snapshot.vehiclePositionLastResetAt > 20 || glm::dot(off, off) > 1.0) {
                // The client moves a vehicle the server does not think it
                // controls — a snap-back every second on its screen. Named
                // so the disagreement (saddle, steering item) can be found.
                Log::Info("[Riding] player %u sent moves for %s #%d it does not control here; resynced (off %.2f)",
                          session.GetPlayerId(), std::string(vehicle->TypeInfo().slug).c_str(), vehicle->GetId(),
                          std::sqrt(glm::dot(off, off)));
                SendMoveVehicle(session, *vehicle);
                std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
                StateOf(session.GetConnectionId()).vehiclePositionLastResetAt = snapshot.tickCount;
            }
        }
    }

    void CarryAcross(PlayerSession& session, int32_t vehicleId, Game::DimensionId toDimension) {
        // Off the vehicle where they sit — the view stays in the level being
        // left, the body goes with the portal (MovePlayer).
        EndInPlace(session);
        std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
        RiderState& state = StateOf(session.GetConnectionId());
        state.remountVehicleId = vehicleId;
        state.remountDimension = Game::DimensionToRaw(toDimension);
        // Long enough for the destination's chunks and the client's
        // dimension switch; a ride that could not be re-made by then is
        // simply over.
        state.remountDeadline = state.tickCount + 200;
    }

    void OnVehicleTeleported(Game::Entity& vehicle) {
        IntegratedServer* server = Server();
        PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) return;
        std::vector<Game::Entity*> stack(vehicle.GetPassengers().begin(), vehicle.GetPassengers().end());
        while (!stack.empty()) {
            Game::Entity* e = stack.back();
            stack.pop_back();
            if (!e) continue;
            for (Game::Entity* p : e->GetPassengers()) stack.push_back(p);
            auto* view = dynamic_cast<PlayerEntityView*>(e);
            ServerPlayer* player = view ? view->GetPlayer() : nullptr;
            Game::Entity* seat = view ? view->GetVehicle() : nullptr;
            if (!player || !seat) continue;
            // Their own seat's position first (a rider of a rider sits on a
            // vehicle that moved with the root).
            if (seat != &vehicle) {
                std::vector<Game::Entity*> chain;
                for (Game::Entity* link = seat; link && link != &vehicle; link = link->GetVehicle()) {
                    chain.push_back(link);
                }
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    if (Game::Entity* carrier = (*it)->GetVehicle()) carrier->PositionRider(**it);
                }
            }
            PositionRider(*seat, *view);
            std::shared_ptr<PlayerSession> session =
                sessions->GetSessionByConnection(static_cast<uint32_t>(view->GetId()));
            ServerConnection* conn = session ? session->GetConnection() : nullptr;
            if (!conn) continue;
            // The driver's own simulated copy is corrected first, then the
            // view (ServerPlayer.teleport's position packet) — which holds
            // the driver's vehicle moves until the client acks it.
            Game::Entity* root = seat;
            while (root->GetVehicle()) root = root->GetVehicle();
            if (root->GetControllingPassenger() == view) SendMoveVehicle(*session, *root);
            const glm::dvec3 feet = player->getPosition();
            conn->Teleport(feet.x, feet.y, feet.z, player->getYaw(), player->getPitch());
            std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
            RiderState& state = StateOf(session->GetConnectionId());
            state.vehicleFirstGood = root->position;
            state.vehicleLastGood = root->position;
        }
    }

    void Forget(uint32_t connectionId) {
        std::lock_guard<std::recursive_mutex> lock(s_stateMutex);
        s_states.erase(connectionId);
    }

    void HandleRidingCommand(PlayerSession& session, const Network::RidingCommandC2SPacket& packet) {
        // MC handlePlayerCommand: the vehicle the player controls
        // (getControlledVehicle), when it is PlayerRideableJumping.
        PlayerEntityView* view = ViewOf(session);
        Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
        if (!vehicle || vehicle->GetControllingPassenger() != view) return;
        auto* jumping = dynamic_cast<Game::PlayerRideableJumping*>(vehicle);
        if (!jumping) return;
        if (packet.action == Network::RidingCommand::StartRidingJump) {
            if (jumping->CanJump() && packet.data > 0) jumping->HandleStartJump(packet.data);
        } else {
            jumping->HandleStopJump();
        }
    }

    void Install() {
        Game::VehicleEntity::SetPlayerInputResolver(&InputResolver);
        Game::LivingEntity::SetRiderControlResolver(false, &RiderControlResolver);
    }

} // namespace Server::PlayerRiding
