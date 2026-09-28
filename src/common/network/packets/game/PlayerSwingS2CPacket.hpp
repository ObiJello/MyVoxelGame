// File: src/common/network/packets/game/PlayerSwingS2CPacket.hpp
//
// Server → the other clients: a player swung an arm (MC ServerboundSwingPacket
// → Player.swing(hand) → ClientboundAnimatePacket SWING_MAIN_HAND /
// SWING_OFF_HAND to everyone tracking them but the swinger). The swinging
// client reports it with the move packet's swung flag (PlayerMoveC2SPacket);
// the receivers start LivingEntity.swing on their copy of the player
// (RemotePlayer::StartSwing), whose attackAnim poses a morphed player's
// humanoid arm (HumanoidModel.setupAttackAnimation).
//
// Wire: VarInt playerId, byte hand (0 = main, 1 = off).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PlayerSwingS2CPacket {
        uint32_t playerId = 0;
        uint8_t  hand     = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PlayerSwingS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteByte(packet.hand);
            return buffer.GetData();
        }

        inline PlayerSwingS2CPacket DeserializePlayerSwingS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            PlayerSwingS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.hand     = reader.HasMore() ? reader.ReadByte() : 0;
            return packet;
        }

    } // namespace Serialization

} // namespace Network
