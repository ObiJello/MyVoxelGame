// File: src/common/network/packets/game/XpOrbPackets.hpp
//
// Server → client packets for experience orbs, shaped exactly like the item
// entity pair (ItemEntitySpawnS2CPacket / ItemEntityMoveS2CPacket) they sit
// beside:
//
//   XpOrbSpawnS2CPacket — spawn or legal full refresh for a known id. MC's
//   ClientboundAddExperienceOrbPacket carries {id, pos, value}; velocity rides
//   along here because our client seeds its local simulation from it instead
//   of waiting for a motion packet.
//
//   XpOrbMoveS2CPacket — batched compact position/velocity refresh.
//
// Pickup reuses TakeItemEntityS2CPacket (MC uses ClientboundTakeItemEntityPacket
// for both entity kinds); removal reuses RemoveEntitiesS2CPacket. The client
// tells everything apart by id range (Game::kXpOrbEntityIdBase).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct XpOrbSpawnS2CPacket {
        int32_t    entityId = 0;
        glm::dvec3 position{0.0};
        glm::vec3  velocity{0.0f};   // blocks per TICK
        int32_t    value    = 0;     // XP points — picks the sprite client-side
    };

    struct XpOrbMoveS2CPacket {
        struct Entry {
            int32_t    entityId = 0;
            glm::dvec3 position{0.0};
            glm::vec3  velocity{0.0f};
        };
        std::vector<Entry> entries;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const XpOrbSpawnS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entityId));
            // Doubles for position, floats for velocity — same reasoning as
            // the item spawn packet (the bob-scale precision note there).
            buffer.WriteDouble(packet.position.x);
            buffer.WriteDouble(packet.position.y);
            buffer.WriteDouble(packet.position.z);
            buffer.WriteFloat(packet.velocity.x);
            buffer.WriteFloat(packet.velocity.y);
            buffer.WriteFloat(packet.velocity.z);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.value));
            return buffer.GetData();
        }

        inline XpOrbSpawnS2CPacket
        DeserializeXpOrbSpawnS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            XpOrbSpawnS2CPacket packet;
            packet.entityId   = static_cast<int32_t>(reader.ReadVarInt());
            packet.position.x = reader.ReadDouble();
            packet.position.y = reader.ReadDouble();
            packet.position.z = reader.ReadDouble();
            packet.velocity.x = reader.ReadFloat();
            packet.velocity.y = reader.ReadFloat();
            packet.velocity.z = reader.ReadFloat();
            packet.value      = static_cast<int32_t>(reader.ReadVarInt());
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const XpOrbMoveS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteVarInt(static_cast<uint32_t>(packet.entries.size()));
            for (const auto& e : packet.entries) {
                buffer.WriteVarInt(static_cast<uint32_t>(e.entityId));
                buffer.WriteDouble(e.position.x);
                buffer.WriteDouble(e.position.y);
                buffer.WriteDouble(e.position.z);
                buffer.WriteFloat(e.velocity.x);
                buffer.WriteFloat(e.velocity.y);
                buffer.WriteFloat(e.velocity.z);
            }
            return buffer.GetData();
        }

        inline XpOrbMoveS2CPacket
        DeserializeXpOrbMoveS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            XpOrbMoveS2CPacket packet;
            const uint32_t count = reader.ReadVarInt();
            packet.entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                XpOrbMoveS2CPacket::Entry e;
                e.entityId   = static_cast<int32_t>(reader.ReadVarInt());
                e.position.x = reader.ReadDouble();
                e.position.y = reader.ReadDouble();
                e.position.z = reader.ReadDouble();
                e.velocity.x = reader.ReadFloat();
                e.velocity.y = reader.ReadFloat();
                e.velocity.z = reader.ReadFloat();
                packet.entries.push_back(e);
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
