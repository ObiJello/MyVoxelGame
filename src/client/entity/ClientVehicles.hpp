// File: src/client/entity/ClientVehicles.hpp
//
// The client half of riding boats and minecarts (and every other vehicle the
// server seats a player on):
//
//   • the synched passenger order (SetPassengersS2C) — which seat each rider
//     takes, and whether THIS client drives (a boat's front seat), which
//     makes the boat the client's to simulate (MC isLocalInstanceAuthoritative
//     → LocalPlayer.sendPosition sends ServerboundMoveVehiclePacket);
//   • the vehicles' synched data (VehicleDataS2C) and the server's
//     correction of a driven vehicle (MoveVehicleS2C);
//   • the driving itself: the movement keys into the boat (MC
//     LocalPlayer.aiStep → boat.setInput), the moved boat and its paddles to
//     the server each tick, the keys on change (ServerboundPlayerInputPacket);
//   • the rider's body and view: the local player's feet at its seat,
//     interpolated with the vehicle each frame; a driven boat's turn carried
//     into the view (AbstractBoat.positionRider's deltaRotation) and the view
//     held within 105° of the boat's heading (clampRotation); remote riders
//     put in their seats;
//   • MC's minecart sound loops (MinecartSoundInstance for every cart,
//     RidingMinecartSoundInstance ×2 for the local rider).
#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Game { class ClientPlayer; class Mob; }
namespace Network {
    struct SetPassengersS2CPacket;
    struct MoveVehicleS2CPacket;
    struct VehicleDataS2CPacket;
}

namespace Client::Vehicles {

    // ── Packets ──────────────────────────────────────────────────────────────
    void OnSetPassengers(const Network::SetPassengersS2CPacket& packet, uint32_t localPlayerId);
    void OnMoveVehicle(const Network::MoveVehicleS2CPacket& packet);
    void OnVehicleData(const Network::VehicleDataS2CPacket& packet);

    // ── Entity lifecycle ────────────────────────────────────────────────────
    // A vehicle arrived on this client (ClientMobManager::Spawn): a minecart
    // starts its rolling loop (ClientPacketListener.postAddEntitySoundInstance).
    void OnVehicleSpawned(Game::Mob& mob);
    // The local player was seated on `vehicleId` (PlayerMountS2C): a
    // minecart's two riding loops (LocalPlayer.startRiding).
    void OnLocalMounted(int32_t vehicleId);

    // Is the vehicle with this client entity id driven by this client (its
    // position is simulated here; server moves are not applied to it)?
    bool IsLocallyDriven(const Game::Mob& mob);

    // ── The client tick ─────────────────────────────────────────────────────
    // Before the entities tick: the keys go into the boat the local player
    // drives.
    // `viewYaw` / `viewPitch`: the camera's view — the rider's yRot / xRot
    // a steered mount turns to. Also runs MC LocalPlayer.aiStep's riding
    // jump (the bar charges while the jump key is held).
    void BeforeEntityTick(Game::ClientPlayer& player, float viewYaw, float viewPitch);
    // After the entities tick: the driven vehicle and its paddles go to the
    // server, the keys on change, remote riders to their seats.
    void AfterEntityTick(Game::ClientPlayer& player);

    // ── The frame ───────────────────────────────────────────────────────────
    // Once a frame, before the player's physics step, with the frame's
    // partial tick (the same one the entity renderers lerp with): the driven
    // boat's turn spread over the frames and the view clamped to the boat
    // (`cameraYaw` is the view yaw the camera and the move packets carry).
    void FrameUpdate(Game::ClientPlayer& player, float& cameraYaw, float dtSeconds, float partialTick);

    // The local rider's feet this frame: the seat on the vehicle's
    // interpolated position. False when the vehicle is not here (the last
    // seat holds).
    bool LocalSeatPosition(const Game::ClientPlayer& player, glm::dvec3& out);

    // MC MultiPlayerGameMode.isServerControlledInventory: the local player
    // rides a vehicle with its own inventory screen (a chest boat), so the
    // inventory key opens that instead of the player's own.
    bool RidesServerControlledInventory(const Game::ClientPlayer& player);

    // The HUD's mount half: the ridden mob's hearts (Hud.extractVehicleHealth)
    // and the jump bar (JumpableVehicleBar). False when there is neither.
    bool GetHudState(const Game::ClientPlayer& player, int& vehicleHearts, int& vehicleHealth, bool& jumpable,
                     float& jumpScale, bool& jumpCooldown);

    // Install the client's rider-control resolver (once, at startup): a
    // mount this client's player steers reads its keys and view here.
    void Install();

    // Forget everything (disconnect / world change).
    void Reset();

} // namespace Client::Vehicles
