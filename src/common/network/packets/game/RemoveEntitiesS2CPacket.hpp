// File: src/common/network/packets/game/RemoveEntitiesS2CPacket.hpp
//
// Mirrors MC ClientboundRemoveEntitiesPacket — sent to clients when entities
// (currently just remote players) leave the world.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>
#include <utility>

namespace Network {

    struct RemoveEntitiesS2CPacket {
        std::vector<int32_t> entityIds;

        RemoveEntitiesS2CPacket() = default;
        explicit RemoveEntitiesS2CPacket(std::vector<int32_t> ids) : entityIds(std::move(ids)) {}
        explicit RemoveEntitiesS2CPacket(int32_t singleId) { entityIds.push_back(singleId); }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const RemoveEntitiesS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entityIds.size()));
            for (int32_t id : packet.entityIds) {
                buffer.WriteVarInt(static_cast<uint32_t>(id));
            }
            return buffer.GetData();
        }

        inline RemoveEntitiesS2CPacket DeserializeRemoveEntitiesS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            RemoveEntitiesS2CPacket packet;
            uint32_t count = reader.ReadVarInt();
            packet.entityIds.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                packet.entityIds.push_back(static_cast<int32_t>(reader.ReadVarInt()));
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
