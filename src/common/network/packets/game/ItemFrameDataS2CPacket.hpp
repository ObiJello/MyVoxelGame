// File: src/common/network/packets/game/ItemFrameDataS2CPacket.hpp
//
// An item frame's framed item — MC's DATA_ITEM entity-data entry, which the
// shared mob packets have no room for. Sent to a watcher on first sight,
// right after AddEntityS2C, and to every watcher whenever the item changes
// (placed, knocked out, broken). The rotation and invisible flag ride the
// mob packets' variant byte (ItemFrame.hpp).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct ItemFrameDataS2CPacket {
        int32_t         entityId = 0;
        Game::ItemStack item{};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ItemFrameDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            WriteItemStack(b, p.item);
            return b.GetData();
        }

        inline ItemFrameDataS2CPacket DeserializeItemFrameDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ItemFrameDataS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.item = ReadItemStack(r);
            return p;
        }

    } // namespace Serialization

} // namespace Network
