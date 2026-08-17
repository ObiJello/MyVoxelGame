// File: src/common/network/packets/game/ItemEntitySpawnS2CPacket.hpp
//
// Server → client: a dropped item appeared. Rough equivalent of MC's
// ClientboundAddEntityPacket + ClientboundSetEntityDataPacket(DATA_ITEM)
// collapsed into one message — MC needs two because its spawn packet is
// generic over every entity type and the ItemStack rides the synched-data
// channel. With exactly one entity type to carry, splitting buys nothing.
//
// Re-sending this for an id the client already knows is a legal full refresh;
// the client overwrites in place rather than duplicating.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/entity/Item.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct ItemEntitySpawnS2CPacket {
        int32_t         entityId = 0;
        glm::dvec3      position{0.0};
        glm::vec3       velocity{0.0f};   // blocks per TICK
        float           bobOffs  = 0.0f;  // render bob/spin phase
        Game::ItemStack stack{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ItemEntitySpawnS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entityId));
            // Position is double, unlike PlayerUpdateS2C's float. An item sits
            // still and bobs by a tenth of a block; at the coordinates a real
            // world reaches, float mantissa steps are large enough for that bob
            // to visibly stair-step. Velocity stays float — it is always small.
            buffer.WriteDouble(packet.position.x);
            buffer.WriteDouble(packet.position.y);
            buffer.WriteDouble(packet.position.z);
            buffer.WriteFloat(packet.velocity.x);
            buffer.WriteFloat(packet.velocity.y);
            buffer.WriteFloat(packet.velocity.z);
            buffer.WriteFloat(packet.bobOffs);
            WriteItemStack(buffer, packet.stack);
            return buffer.GetData();
        }

        inline ItemEntitySpawnS2CPacket
        DeserializeItemEntitySpawnS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ItemEntitySpawnS2CPacket packet;
            packet.entityId   = static_cast<int32_t>(reader.ReadVarInt());
            packet.position.x = reader.ReadDouble();
            packet.position.y = reader.ReadDouble();
            packet.position.z = reader.ReadDouble();
            packet.velocity.x = reader.ReadFloat();
            packet.velocity.y = reader.ReadFloat();
            packet.velocity.z = reader.ReadFloat();
            packet.bobOffs    = reader.ReadFloat();
            packet.stack      = ReadItemStack(reader);
            return packet;
        }

    } // namespace Serialization

} // namespace Network
