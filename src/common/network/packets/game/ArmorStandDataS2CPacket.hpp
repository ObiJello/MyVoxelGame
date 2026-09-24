// File: src/common/network/packets/game/ArmorStandDataS2CPacket.hpp
//
// An armor stand's synched state that the shared mob packets have no room
// for: the six part poses (MC's six ROTATIONS entity-data entries) and its
// six equipment stacks (MC ClientboundSetEquipmentPacket). Sent to a watcher
// on first sight, right after AddEntityS2C, and to every watcher whenever
// either changes (a swap, a break, /data). The client flags byte (small,
// arms, base plate, marker, invisible) rides the mob packets' variant byte.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/entity/Item.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Network {

    struct ArmorStandDataS2CPacket {
        int32_t entityId = 0;
        // Degrees, MC's Rotations: head, body, left arm, right arm, left leg,
        // right leg — ArmorStand::Pose's order.
        std::array<glm::vec3, 6> poses{};
        // MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD — ArmorStand's slot order.
        std::array<Game::ItemStack, 6> equipment{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ArmorStandDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            for (const glm::vec3& r : p.poses) {
                b.WriteFloat(r.x);
                b.WriteFloat(r.y);
                b.WriteFloat(r.z);
            }
            for (const Game::ItemStack& s : p.equipment) WriteItemStack(b, s);
            return b.GetData();
        }

        inline ArmorStandDataS2CPacket
        DeserializeArmorStandDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ArmorStandDataS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            for (glm::vec3& rot : p.poses) {
                rot.x = r.ReadFloat();
                rot.y = r.ReadFloat();
                rot.z = r.ReadFloat();
            }
            for (Game::ItemStack& s : p.equipment) s = ReadItemStack(r);
            return p;
        }

    } // namespace Serialization

} // namespace Network
