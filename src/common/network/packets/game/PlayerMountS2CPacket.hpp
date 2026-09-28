// File: src/common/network/packets/game/PlayerMountS2CPacket.hpp
//
// Server → clients: a player sat down on a vehicle entity, or got off it.
//
// MC ClientboundSetPassengersPacket (the vehicle's passenger list) for the
// one case this engine has: a PLAYER riding an entity (the 26.3 cushion).
// Players are not tracked entities here, so the pair travels player-first:
// `vehicleId` is the entity the player now rides, 0 when they got off.
// Each client puts its own player in the seat (ClientPlayer::vehicleId) or
// draws a remote one sitting (RemotePlayer::vehicleId).
//
// Wire: VarInt playerId, int32 vehicleId (0 = none).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerMountS2CPacket {
        uint32_t playerId  = 0;
        int32_t  vehicleId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerMountS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteInt(static_cast<uint32_t>(packet.vehicleId));
            return buffer.GetData();
        }

        inline PlayerMountS2CPacket DeserializePlayerMountS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerMountS2CPacket packet;
            packet.playerId  = reader.ReadVarInt();
            packet.vehicleId = static_cast<int32_t>(reader.ReadInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
