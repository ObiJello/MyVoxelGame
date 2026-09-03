// File: src/common/network/packets/game/AoRegionsS2CPacket.hpp
//
// AoRegionsS2C — the complete list of "no ambient occlusion" boxes of one
// dimension (the occlusion wand's). Sent whole on every change and when a
// client first holds the dimension; the client swaps its list and remeshes
// the sections the boxes touch.
//
//   int8    dimension
//   VarInt  count
//   count × (Int minX, Int minY, Int minZ, Int maxX, Int maxY, Int maxZ)   inclusive block bounds
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Network {

    struct AoRegionsS2CPacket {
        struct Box { glm::ivec3 min{0}; glm::ivec3 max{0}; };
        int8_t           dimensionId = 0;
        std::vector<Box> boxes;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const AoRegionsS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.dimensionId));
            b.WriteVarInt(static_cast<uint32_t>(p.boxes.size()));
            for (const auto& box : p.boxes) {
                b.WriteInt(static_cast<uint32_t>(box.min.x)); b.WriteInt(static_cast<uint32_t>(box.min.y)); b.WriteInt(static_cast<uint32_t>(box.min.z));
                b.WriteInt(static_cast<uint32_t>(box.max.x)); b.WriteInt(static_cast<uint32_t>(box.max.y)); b.WriteInt(static_cast<uint32_t>(box.max.z));
            }
            return b.GetData();
        }

        inline AoRegionsS2CPacket DeserializeAoRegionsS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            AoRegionsS2CPacket p;
            p.dimensionId = static_cast<int8_t>(r.ReadByte());
            const uint32_t n = r.ReadVarInt();
            p.boxes.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                AoRegionsS2CPacket::Box box;
                box.min.x = static_cast<int32_t>(r.ReadInt()); box.min.y = static_cast<int32_t>(r.ReadInt()); box.min.z = static_cast<int32_t>(r.ReadInt());
                box.max.x = static_cast<int32_t>(r.ReadInt()); box.max.y = static_cast<int32_t>(r.ReadInt()); box.max.z = static_cast<int32_t>(r.ReadInt());
                p.boxes.push_back(box);
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network
