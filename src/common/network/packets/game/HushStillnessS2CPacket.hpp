// File: src/common/network/packets/game/HushStillnessS2CPacket.hpp
//
// The Hush's "stillness" (docs/the-hush.md, Atmosphere): a level-wide event
// the SERVER owns (Server::HushStillness — the timer, the frozen mobs). The
// client only draws it — the fog closes in, the auroras dim, the sky
// flattens — so all it needs is "it began, and lasts this long" and "it
// lifted". MC has no equivalent packet; the nearest is ClientboundGameEvent
// (rain start/stop), which carries a one-byte event and a float and would
// have been the natural host if this engine had one.
//
// Sent to the players IN the Hush only: on each start/end, and to a player
// who arrives while one is running (see HushStillness::SyncPlayers). A
// player leaving the Hush mid-stillness is sent the end, so no client keeps
// a stale one.
//
// Wire: byte active, VarInt remainingTicks (0 when !active). New fields go
// on the end (trailing-field extension; readers check HasMore).
//
// Handled on the network I/O thread (ClientConnection), like
// ServerPausedS2C: the payload only lands in atomics
// (Client::HushStillnessState) which the render thread reads.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct HushStillnessS2CPacket {
        bool     active = false;
        // Server ticks until the stillness lifts. The client eases the look
        // out when it receives the end; this is only its safety net for an
        // end packet that never comes (a dropped session).
        uint32_t remainingTicks = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const HushStillnessS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(packet.active ? 1 : 0);
            buffer.WriteVarInt(packet.active ? packet.remainingTicks : 0u);
            return buffer.GetData();
        }

        inline HushStillnessS2CPacket DeserializeHushStillnessS2C(const std::vector<uint8_t>& data) {
            HushStillnessS2CPacket packet;
            if (data.empty()) return packet;
            PacketReader reader(data);
            packet.active = reader.ReadByte() != 0;
            if (reader.HasMore()) packet.remainingTicks = reader.ReadVarInt();
            return packet;
        }

    } // namespace Serialization

} // namespace Network
