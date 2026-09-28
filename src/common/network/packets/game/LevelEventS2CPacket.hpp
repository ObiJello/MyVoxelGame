// File: src/common/network/packets/game/LevelEventS2CPacket.hpp
//
// MC ClientboundLevelEventPacket — Level.levelEvent's trip to the client:
// an event type (LevelEvent.java), a block position, one int of data, and
// whether it is a global event (heard from anywhere).
//
// This engine plays most level events as ordinary sounds on the server
// (common/sound/LevelEventSounds.hpp), so the packet carries only the ones
// whose client half is more than a one-shot sound:
//   1010  SOUND_PLAY_JUKEBOX_SONG  data = the song's JukeboxSongs id. The
//         client starts the streamed song at the jukebox, shows "Now
//         Playing" and tells the entities within 3 blocks
//         (LevelEventHandler.playJukeboxSong).
//   1011  SOUND_STOP_JUKEBOX_SONG  stops the song playing at that position
//         and tells the same entities it stopped.
// A client ignores any other type.
//
// Sent to every player in the dimension within 64 blocks of the event, minus
// the excepted player (ServerSoundBroadcaster::LevelEvent), scoped to the
// dimension like every positional packet.
//
// Wire: Int type, Int x, Int y, Int z, Int data, Byte global, VarLong
// startTicks (trailing). New fields go
// on the end (trailing-field extension; readers check HasMore).
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct LevelEventS2CPacket {
        int32_t type = 0;
        int32_t x = 0, y = 0, z = 0;
        int32_t data = 0;
        bool    globalEvent = false;
        // Engine trailing field, 1010 only: how many ticks into the song the
        // jukebox already is — a song picked up part-way (its chunk reached
        // this player mid-song, or came back from disk). 0 = from the top.
        int64_t startTicks = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const LevelEventS2CPacket& packet) {
            PacketBuffer buffer(24);
            buffer.WriteInt(static_cast<uint32_t>(packet.type));
            buffer.WriteInt(static_cast<uint32_t>(packet.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.z));
            buffer.WriteInt(static_cast<uint32_t>(packet.data));
            buffer.WriteByte(packet.globalEvent ? 1 : 0);
            buffer.WriteVarLong(static_cast<uint64_t>(packet.startTicks < 0 ? 0 : packet.startTicks));
            return buffer.GetData();
        }

        inline LevelEventS2CPacket DeserializeLevelEventS2C(const std::vector<uint8_t>& payload) {
            LevelEventS2CPacket packet;
            PacketReader reader(payload);
            packet.type = static_cast<int32_t>(reader.ReadInt());
            packet.x    = static_cast<int32_t>(reader.ReadInt());
            packet.y    = static_cast<int32_t>(reader.ReadInt());
            packet.z    = static_cast<int32_t>(reader.ReadInt());
            packet.data = static_cast<int32_t>(reader.ReadInt());
            if (reader.HasMore()) packet.globalEvent = reader.ReadByte() != 0;
            if (reader.HasMore()) packet.startTicks = static_cast<int64_t>(reader.ReadVarLong());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
