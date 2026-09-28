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
            // MC ClientboundPlayerInfoUpdatePacket.Action.UPDATE_GAME_MODE —
            // broadcast to everyone when a player's game mode changes
            // (ServerPlayerGameMode.changeGameModeForPlayer). The tab list's
            // spectator styling, the spectator menu's player list and the
            // spectator-only rendering of other players all read it.
            UPDATE_GAME_MODE = 2,
            // MC Action.UPDATE_LATENCY — PlayerList.tick re-sends every
            // player's latency to everyone each 600 ticks; the tab list's
            // ping bars read it.
            UPDATE_LATENCY = 3,
        };

        Action      action  = Action::ADD;
        uint32_t    playerId = 0;
        std::string playerName;       // Only meaningful for ADD
        uint8_t     colorId = 0;      // Game::PlayerColorId — only for ADD
        // Server::GameMode raw value (0 survival … 3 spectator): ADD (trailing,
        // after the colour) and UPDATE_GAME_MODE.
        uint8_t     gameMode = 0;
        // Round-trip latency in ms (MC ServerCommonPacketListenerImpl
        // .latency()): ADD (trailing, after the game mode) and UPDATE_LATENCY.
        int32_t     latency = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerInfoS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.action));
            buffer.WriteInt(static_cast<int32_t>(packet.playerId));
            if (packet.action == PlayerInfoS2CPacket::Action::ADD) {
                buffer.WriteString(packet.playerName);
                buffer.WriteByte(packet.colorId);
                buffer.WriteByte(packet.gameMode);
                buffer.WriteVarInt(static_cast<uint32_t>(packet.latency));
            } else if (packet.action == PlayerInfoS2CPacket::Action::UPDATE_GAME_MODE) {
                buffer.WriteByte(packet.gameMode);
            } else if (packet.action == PlayerInfoS2CPacket::Action::UPDATE_LATENCY) {
                buffer.WriteVarInt(static_cast<uint32_t>(packet.latency));
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
                if (reader.Remaining() >= 1) {
                    packet.gameMode = reader.ReadByte();
                }
                if (reader.Remaining() >= 1) {
                    packet.latency = static_cast<int32_t>(reader.ReadVarInt());
                }
            } else if (packet.action == PlayerInfoS2CPacket::Action::UPDATE_GAME_MODE &&
                       reader.Remaining() >= 1) {
                packet.gameMode = reader.ReadByte();
            } else if (packet.action == PlayerInfoS2CPacket::Action::UPDATE_LATENCY &&
                       reader.Remaining() >= 1) {
                packet.latency = static_cast<int32_t>(reader.ReadVarInt());
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
