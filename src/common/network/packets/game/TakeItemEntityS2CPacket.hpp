// File: src/common/network/packets/game/TakeItemEntityS2CPacket.hpp
//
// Server → client: a player collected (part of) a dropped item.
// Mirrors MC ClientboundTakeItemEntityPacket — {itemId, playerId, amount}.
//
// This is what drives the pickup animation: the client already knows where the
// item is, so the packet only has to say WHO took HOW MANY and the client can
// fly its own copy into that player.
//
// It is also what REMOVES a collected item on the client. A picked-up entity is
// deliberately left out of RemoveEntitiesS2CPacket — if a removal raced ahead of
// this packet the client would delete the entity first and the animation would
// have nothing to fly. MC has the same split for the same reason.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct TakeItemEntityS2CPacket {
        int32_t  itemEntityId = 0;
        uint32_t playerId     = 0;   // collector; entity id of the player
        int32_t  amount       = 0;   // how many were taken this pickup
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const TakeItemEntityS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.itemEntityId));
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.amount));
            return buffer.GetData();
        }

        inline TakeItemEntityS2CPacket
        DeserializeTakeItemEntityS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            TakeItemEntityS2CPacket packet;
            packet.itemEntityId = static_cast<int32_t>(reader.ReadVarInt());
            packet.playerId     = reader.ReadVarInt();
            packet.amount       = static_cast<int32_t>(reader.ReadVarInt());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
