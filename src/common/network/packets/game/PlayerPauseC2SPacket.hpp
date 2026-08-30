// File: src/common/network/packets/game/PlayerPauseC2SPacket.hpp
//
// Client → server: "my pause screen just opened / closed".
//
// There is no vanilla equivalent, and the reason is worth stating. MC's
// integrated server reads the pause state DIRECTLY off the client it is
// embedded in (IntegratedServer.tickServer:103 —
// `Minecraft.getInstance().isPaused() || playerList.isEmpty()`), and it refuses
// to pause at all once the world is published to LAN
// (Minecraft.java:1284 `&& !this.singleplayerServer.isPublished()`), because a
// second player has no way to tell it what their screen is doing.
//
// This engine keeps the same split — the world simulation stops, networking
// does not — but answers the LAN case properly instead of giving up on it: each
// client reports its own pause state, and the server freezes only when EVERY
// connected player is paused. One person in the options menu while a friend is
// mining does not stop that friend's world.
//
// Sent on CHANGE only, as a dirty check beside the fly-state one in
// ClientPlayerController — a per-tick "still paused" heartbeat would be pure
// waste for a flag that changes when a menu opens.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerPauseC2SPacket {
        bool paused = false;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerPauseC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.paused ? 1 : 0);
            return buffer.GetData();
        }

        inline PlayerPauseC2SPacket DeserializePlayerPauseC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerPauseC2SPacket packet;
            packet.paused = reader.ReadByte() != 0;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
