// File: src/common/network/packets/game/MorphPickupS2CPacket.hpp
//
// /morph item: a player morphed into an item was just picked up. Every
// client in the dimension plays the pickup the way a real one looks (MC
// ItemPickupParticle: the item flies into the collector over three ticks)
// from the picked-up player's position; the player itself goes invisible
// on the next position update.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include <cstdint>

namespace Network {

    struct MorphPickupS2CPacket {
        uint32_t        heldPlayerId = 0;
        uint32_t        holderId = 0;
        Game::ItemStack stack;
    };

    namespace Serialization {
        inline std::vector<uint8_t> Serialize(const MorphPickupS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(p.heldPlayerId);
            b.WriteVarInt(p.holderId);
            WriteItemStack(b, p.stack);
            return b.GetData();
        }
        inline MorphPickupS2CPacket DeserializeMorphPickupS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MorphPickupS2CPacket p;
            p.heldPlayerId = r.ReadVarInt();
            p.holderId     = r.ReadVarInt();
            p.stack        = ReadItemStack(r);
            return p;
        }
    }
}
