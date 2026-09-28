// File: src/common/network/packets/game/BodyArmorS2CPacket.hpp
//
// A mob's BODY equipment slot — MC ClientboundSetEquipmentPacket, reduced to
// the one slot whose client copy matters here: the wolf's wolf armor, which
// WolfArmorLayer draws (its crack overlay reads the stack's DAMAGE, its dyed
// overlay the DYED_COLOR). The shared mob packets carry single bytes and have
// no room for a stack, so it is its own packet, sent to a watcher on first
// sight (when the slot holds anything) and to every watcher whenever the
// stack changes (equipped, damaged, repaired, sheared off, broken). An empty
// stack means the slot was emptied.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/EquipmentSlot.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct BodyArmorS2CPacket {
        int32_t         entityId = 0;
        Game::ItemStack item{};
        // APPENDED FIELD: which equipment slot the stack fills (MC
        // ClientboundSetEquipmentPacket's slot) — BODY for the wolf's armour,
        // MAINHAND for the allay's held item. Absent = BODY, so older
        // streams decode as they always did.
        Game::EquipmentSlot slot = Game::EquipmentSlot::BODY;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BodyArmorS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            WriteItemStack(b, p.item);
            b.WriteByte(static_cast<uint8_t>(p.slot));
            return b.GetData();
        }

        inline BodyArmorS2CPacket DeserializeBodyArmorS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            BodyArmorS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.item = ReadItemStack(r);
            if (r.Remaining() >= 1) {
                const uint8_t slot = r.ReadByte();
                p.slot = slot <= static_cast<uint8_t>(Game::EquipmentSlot::SADDLE)
                    ? static_cast<Game::EquipmentSlot>(slot) : Game::EquipmentSlot::BODY;
            }
            return p;
        }

    } // namespace Serialization

} // namespace Network
