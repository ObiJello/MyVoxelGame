// File: src/common/network/packets/game/ItemEntityMoveS2CPacket.hpp
//
// Server → client: periodic position/velocity refresh for dropped items.
// Batched, because items arrive in piles and one packet per entity per sync
// would be mostly framing overhead.
//
// The stack COUNT rides along. It is the only field of an item entity's stack
// that can change while the entity lives (a merge grows it, a partial pickup
// shrinks it) — the item id and its components are fixed at spawn. Carrying one
// VarInt here means both of those stay in sync for free, instead of needing a
// separate stack-changed packet or a full spawn re-send.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct ItemEntityMoveS2CPacket {
        struct Entry {
            int32_t    entityId = 0;
            glm::dvec3 position{0.0};
            glm::vec3  velocity{0.0f};   // blocks per TICK
            int32_t    count    = 0;     // current stack size
        };
        std::vector<Entry> entries;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ItemEntityMoveS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entries.size()));
            for (const auto& e : packet.entries) {
                buffer.WriteVarInt(static_cast<uint32_t>(e.entityId));
                buffer.WriteDouble(e.position.x);
                buffer.WriteDouble(e.position.y);
                buffer.WriteDouble(e.position.z);
                buffer.WriteFloat(e.velocity.x);
                buffer.WriteFloat(e.velocity.y);
                buffer.WriteFloat(e.velocity.z);
                buffer.WriteVarInt(static_cast<uint32_t>(e.count));
            }
            return buffer.GetData();
        }

        inline ItemEntityMoveS2CPacket
        DeserializeItemEntityMoveS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ItemEntityMoveS2CPacket packet;
            const uint32_t count = reader.ReadVarInt();
            packet.entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                ItemEntityMoveS2CPacket::Entry e;
                e.entityId   = static_cast<int32_t>(reader.ReadVarInt());
                e.position.x = reader.ReadDouble();
                e.position.y = reader.ReadDouble();
                e.position.z = reader.ReadDouble();
                e.velocity.x = reader.ReadFloat();
                e.velocity.y = reader.ReadFloat();
                e.velocity.z = reader.ReadFloat();
                e.count      = static_cast<int32_t>(reader.ReadVarInt());
                packet.entries.push_back(e);
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
