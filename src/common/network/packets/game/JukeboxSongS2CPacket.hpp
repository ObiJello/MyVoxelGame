// File: src/common/network/packets/game/JukeboxSongS2CPacket.hpp
//
// Engine packet (no MC counterpart) behind the "Jukebox Range: Global"
// sound option: the server tells EVERY connected player — any dimension, any
// distance — about each jukebox song. Sent unscoped (the packet names its own
// dimension):
//   • when a song starts (fresh = true, ticks = 0),
//   • when a song the registry did not know yet is found playing — a jukebox
//     whose chunk came back with its song, or loaded from disk mid-song
//     (fresh = false, ticks = how far in),
//   • to a player joining, once per song playing (fresh = false),
//   • when a song stops — ejected, broken, finished, or its jukebox no longer
//     simulated (songId = -1).
// A client on Normal range only keeps the record (so switching to Global
// mid-song picks the song up at the right place); vanilla's positional
// playback stays on level events 1010 / 1011 (LevelEventS2CPacket).
//
// Wire: Byte dimension, Int x, Int y, Int z, Int songId (-1 = stop),
// VarLong ticks, Byte fresh. New fields go on the end.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct JukeboxSongS2CPacket {
        int8_t  dimension = 0;   // Game::DimensionId's raw value (the Nether is -1)
        int32_t x = 0, y = 0, z = 0;
        int32_t songId = -1;     // JukeboxSongs id; -1 = the song at this jukebox stopped
        int64_t ticks = 0;       // ticks since the song started
        bool    fresh = false;   // a new start rather than a resync
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const JukeboxSongS2CPacket& packet) {
            PacketBuffer buffer(32);
            buffer.WriteByte(static_cast<uint8_t>(packet.dimension));
            buffer.WriteInt(static_cast<uint32_t>(packet.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.z));
            buffer.WriteInt(static_cast<uint32_t>(packet.songId));
            buffer.WriteVarLong(static_cast<uint64_t>(packet.ticks < 0 ? 0 : packet.ticks));
            buffer.WriteByte(packet.fresh ? 1 : 0);
            return buffer.GetData();
        }

        inline JukeboxSongS2CPacket DeserializeJukeboxSongS2C(const std::vector<uint8_t>& payload) {
            JukeboxSongS2CPacket packet;
            PacketReader reader(payload);
            packet.dimension = static_cast<int8_t>(reader.ReadByte());
            packet.x      = static_cast<int32_t>(reader.ReadInt());
            packet.y      = static_cast<int32_t>(reader.ReadInt());
            packet.z      = static_cast<int32_t>(reader.ReadInt());
            packet.songId = static_cast<int32_t>(reader.ReadInt());
            packet.ticks  = static_cast<int64_t>(reader.ReadVarLong());
            if (reader.HasMore()) packet.fresh = reader.ReadByte() != 0;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
