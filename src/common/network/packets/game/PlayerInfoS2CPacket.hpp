// File: src/common/network/packets/game/PlayerInfoS2CPacket.hpp
//
// Mirrors MC ClientboundPlayerInfoUpdatePacket — broadcast when players
// join/leave so every client can update its tab list and stick-figure colour
// table. Wire format keeps the colour byte tail-appended to the ADD record
// for forward/backward compatibility:
//   - old client reading new ADD packet stops after the name; trailing byte
//     is silently ignored.
//   - new client reading old ADD packet sees Remaining()==0 after the name
//     and falls back to colorId=0 (Default green).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct PlayerInfoS2CPacket {
        enum class Action : uint8_t {
            ADD    = 0, // Player joined
            REMOVE = 1, // Player left
        };

        Action      action  = Action::ADD;
        uint32_t    playerId = 0;
        std::string playerName;       // Only meaningful for ADD
        uint8_t     colorId = 0;      // Game::PlayerColorId — only for ADD
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerInfoS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.action));
            buffer.WriteInt(static_cast<int32_t>(packet.playerId));
            if (packet.action == PlayerInfoS2CPacket::Action::ADD) {
                buffer.WriteString(packet.playerName);
                buffer.WriteByte(packet.colorId);
            }
            return buffer.GetData();
        }

        inline PlayerInfoS2CPacket DeserializePlayerInfoS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerInfoS2CPacket packet;
            packet.action = static_cast<PlayerInfoS2CPacket::Action>(reader.ReadByte());
            packet.playerId = static_cast<uint32_t>(reader.ReadInt());
            if (packet.action == PlayerInfoS2CPacket::Action::ADD && reader.Remaining() > 0) {
                packet.playerName = reader.ReadString();
                if (reader.Remaining() >= 1) {
                    packet.colorId = reader.ReadByte();
                }
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
