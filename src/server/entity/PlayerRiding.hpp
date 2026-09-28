// File: src/server/entity/PlayerRiding.hpp
//
// The server's riding system for PLAYERS — MC Entity.startRiding /
// stopRiding / rideTick and ServerGamePacketListenerImpl's vehicle half, for
// every rideable entity the engine has: boats and minecarts
// (common/entity/vehicle), the equines (PlayerRideable / HorseTaming), the
// 26.3 cushion.
//
// ── The link ──────────────────────────────────────────────────────────────
// Players are not Entities here, but each has a PlayerEntityView (a real
// LivingEntity) in the level it stands in, and that view is what rides: it
// sits in the vehicle's passenger list (Entity::m_passengers / m_vehicle)
// exactly where MC keeps the Player — first when it drives, beside a mob in a
// boat. The ServerPlayer mirrors the link (getVehicleId, what the save, the
// joining clients' PlayerMountS2C and the move-packet gate read). Every
// change to it goes through here:
//
//   StartRiding    MC Player.startRiding: off any other vehicle (in place),
//                  onto this one — the view joins the passenger list (a
//                  player to the front), the rider is seated, PlayerMountS2C
//                  to every client and SetPassengersS2C from the tracker.
//   StopRiding     MC LivingEntity.stopRiding → dismountVehicle: the view
//                  leaves (PlayerEntityView::StopRiding lands in
//                  OnViewDismounted), the rider is put down at the vehicle's
//                  GetDismountLocationForPassenger and teleported there.
//   Tick           MC Player.rideTick, after the player's own tick: a sneak
//                  gets up (wantsToStopRiding), death, a bed, spectator mode
//                  end the ride; a vanished vehicle drops the rider; a
//                  teleport or a dimension change ends it where they stand.
//   PositionRider  MC ServerLevel.tickPassenger for a player riding a mob
//                  (MobManager's passenger chain): the rider's body is put in
//                  the seat after the vehicle moved.
//
// ── Driving ───────────────────────────────────────────────────────────────
// A boat's driver simulates it on their own client (MC isLocalInstance-
// Authoritative) and reports where it went (MoveVehicleC2S): HandleMoveVehicle
// validates the move as MC's handleMoveVehicle does and corrects the client
// (MoveVehicleS2C) when it is wrong. The paddles (PaddleBoatC2S) and the
// movement keys (PlayerInputC2S — a minecart's move intent, the shift that
// gets up) are kept per player.
#pragma once

#include "common/entity/vehicle/VehicleEntity.hpp"
#include "common/core/Uuid.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cstdint>

namespace Game { class Cushion; class Entity; class Mob; class PlayerRideable; }
namespace Network { struct MoveVehicleC2SPacket; struct RidingCommandC2SPacket; }

namespace Server {

    class PlayerSession;
    class PlayerEntityView;
    class ServerConnection;
    class ServerPlayer;

    namespace PlayerRiding {

        // MC Player.startRiding(vehicle, force). False when the player cannot
        // ride now (dead, asleep, a spectator, sneaking — MC canRide's
        // !isShiftKeyDown — unless `force`), the vehicle has no room
        // (canAddPassenger) or is gone.
        bool StartRiding(PlayerSession& session, Game::Entity& vehicle, bool force = false);
        // The older call sites: a cushion (Cushion.interact) and an equine's
        // doPlayerRide.
        bool StartRiding(PlayerSession& session, Game::Cushion& cushion);
        bool StartRiding(PlayerSession& session, Game::Mob& vehicle, Game::PlayerRideable& seat);

        // MC stopRiding: off the vehicle, to its dismount location. Nothing
        // when the player rides nothing.
        void StopRiding(PlayerSession& session);

        // Once per server tick, after ServerPlayer::tick. See the header.
        void Tick(PlayerSession& session);

        // The vehicle `view` rides just moved (MobManager's passenger chain,
        // a driver's accepted move): the rider's body goes to its seat.
        void PositionRider(Game::Entity& vehicle, PlayerEntityView& view);

        // PlayerEntityView::StopRiding: the view just left `oldVehicle` (the
        // vehicle ejected its riders, broke, went under, or the player got
        // off) — MC LivingEntity.dismountVehicle for the player behind it.
        void OnViewDismounted(PlayerEntityView& view, Game::Entity& oldVehicle);

        // The session is going away (disconnect). MC PlayerList.remove: a
        // root vehicle this player is the only player on leaves with them
        // (its NBT went into the player's file as RootVehicle — the save
        // runs first); anything else, the player gets off where the vehicle
        // would put them. No broadcast: the session manager's lock is held,
        // and the other clients drop the player with its PlayerInfo REMOVE.
        void OnDisconnect(PlayerSession& session);

        // MC PlayerList.remove / ServerPlayer.addAdditionalSaveData: the root
        // vehicle that leaves (and is saved) with a riding player — the root
        // of the tree when this player is the only player anywhere on it and
        // it is a saveable entity (a cushion stays: it belongs to its
        // block). `attach` gets the UUID of the entity the player sits on.
        // Null when the player rides nothing that leaves with them.
        const Game::Mob* RootVehicleLeavingWith(const ServerPlayer& player, Game::Uuid& attach);

        // A joining client learns who already sits where (MC's
        // ClientboundSetPassengersPacket rides the vehicle's pairing; the
        // tracker sends each vehicle's SetPassengersS2C with its add).
        void SendMountsTo(ServerConnection& connection);

        // Is the cushion's seat taken by a live rider?
        bool ValidateSeat(Game::Cushion& cushion);

        // The vehicle the session's player rides, in its level, or null.
        Game::Entity* VehicleOf(PlayerSession& session);
        // The player id a riding view goes by on the wire (SetPassengersS2C),
        // -1 when its player is gone.
        int32_t PlayerIdOfView(const Game::Entity& view);

        // ── Packets ──────────────────────────────────────────────────────
        // MC handlePlayerInput: the keys, and setShiftKeyDown from them.
        void HandlePlayerInput(PlayerSession& session, uint8_t keys);
        // MC handleMoveVehicle.
        void HandleMoveVehicle(PlayerSession& session, const Network::MoveVehicleC2SPacket& packet);
        // MC handlePaddleBoat.
        void HandlePaddleBoat(PlayerSession& session, bool left, bool right, bool reverse = false);
        // MC handlePlayerCommand START_RIDING_JUMP / STOP_RIDING_JUMP: the
        // mount the player steers (getControlledVehicle), when it jumps.
        void HandleRidingCommand(PlayerSession& session, const Network::RidingCommandC2SPacket& packet);
        // The last keys a player sent (false when none yet) — the resolver
        // VehicleEntity::GetPlayerInput reads.
        bool LastInput(uint32_t connectionId, Game::PlayerInput& out);

        // ── Through portals with the vehicle (MC teleportCrossDimension) ──
        // The player rides `vehicleId`, which is about to cross into
        // `toDimension`: the ride ends in place now (the view stays behind)
        // and is re-made — forced, as MC's startRiding(newEntity, true) —
        // as soon as the player stands in the new level with the vehicle.
        void CarryAcross(PlayerSession& session, int32_t vehicleId, Game::DimensionId toDimension);
        // A vehicle carrying players moved within its level (an end gateway,
        // a gun portal): each rider's body goes to its new seat and its
        // client is told (a teleport for the view, MoveVehicleS2C for the
        // driver's own copy).
        void OnVehicleTeleported(Game::Entity& vehicle);

        // Forget a leaving player's riding state (input, seat bookkeeping).
        void Forget(uint32_t connectionId);

        // Install the input resolver (once, at server start).
        void Install();

    } // namespace PlayerRiding

} // namespace Server
