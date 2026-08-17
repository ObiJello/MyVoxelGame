// File: src/common/network/packets/game/WorldStateS2CPackets.hpp
//
// The two world-state packets that had no struct of their own: the server
// wrote them with raw PacketBuffer calls and the client read them back with a
// raw PacketReader, which is why they were stuck on the legacy registry path.
//
// MC equivalents: ClientboundSetTimePacket and ClientboundSetDefaultSpawnPositionPacket.
// Both have real packet types there, and both of their client handlers call
// PacketUtils.ensureRunningOnSameThread — which is the whole reason these need
// typed representations here: a typed packet is what routes them through the
// queue onto the client main thread.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    // MC ClientboundSetTimePacket. Wire order must match
    // ServerConnection::SendTimeUpdate exactly: long, long, byte.
    struct TimeUpdateS2CPacket {
        uint64_t worldAge = 0;
        uint64_t timeOfDay = 0;
        bool     doDaylightCycle = true;

        TimeUpdateS2CPacket() = default;
        TimeUpdateS2CPacket(uint64_t age, uint64_t time, bool cycle)
            : worldAge(age), timeOfDay(time), doDaylightCycle(cycle) {}
    };

    // MC ClientboundSetDefaultSpawnPositionPacket. Wire order must match
    // ServerConnection::sendInitialGameData: three ints.
    struct WorldSpawnS2CPacket {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        WorldSpawnS2CPacket() = default;
        WorldSpawnS2CPacket(int32_t px, int32_t py, int32_t pz) : x(px), y(py), z(pz) {}
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const TimeUpdateS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteLong(packet.worldAge);
            buffer.WriteLong(packet.timeOfDay);
            buffer.WriteByte(packet.doDaylightCycle ? 1 : 0);
            return buffer.GetData();
        }

        inline TimeUpdateS2CPacket DeserializeTimeUpdateS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            TimeUpdateS2CPacket packet;
            packet.worldAge = reader.ReadLong();
            packet.timeOfDay = reader.ReadLong();
            packet.doDaylightCycle = reader.ReadByte() != 0;
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const WorldSpawnS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteInt(packet.x);
            buffer.WriteInt(packet.y);
            buffer.WriteInt(packet.z);
            return buffer.GetData();
        }

        inline WorldSpawnS2CPacket DeserializeWorldSpawnS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            WorldSpawnS2CPacket packet;
            packet.x = static_cast<int32_t>(reader.ReadInt());
            packet.y = static_cast<int32_t>(reader.ReadInt());
            packet.z = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
