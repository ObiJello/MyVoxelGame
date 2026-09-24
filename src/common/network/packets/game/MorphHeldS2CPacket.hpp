// File: src/common/network/packets/game/MorphHeldS2CPacket.hpp
//
// /morph item: a player morphed into an item can be picked up by another
// player, like the item would be. While held they ride along inside the
// holder (invisible, their client keeps its position on the holder's) and
// watch through the holder's eyes (the Spectator / Watched control roles).
// Left Alt throws them out of the holder's hands like a Q drop.
//
//   held = true  — picked up by `holderId`
//   held = false — released; `throwVel` (blocks per tick, MC's drop
//                  velocity; zero when simply let go) is applied on top of
//                  the teleport that put the player at the holder's hand.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>

namespace Network {

    struct MorphHeldS2CPacket {
        bool      held = false;
        uint32_t  holderId = 0;
        glm::vec3 throwVel{0.0f};
    };

    namespace Serialization {
        inline std::vector<uint8_t> Serialize(const MorphHeldS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(p.held ? 1 : 0);
            b.WriteVarInt(p.holderId);
            b.WriteFloat(p.throwVel.x);
            b.WriteFloat(p.throwVel.y);
            b.WriteFloat(p.throwVel.z);
            return b.GetData();
        }
        inline MorphHeldS2CPacket DeserializeMorphHeldS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            MorphHeldS2CPacket p;
            p.held     = r.ReadByte() != 0;
            p.holderId = r.ReadVarInt();
            p.throwVel.x = r.ReadFloat();
            p.throwVel.y = r.ReadFloat();
            p.throwVel.z = r.ReadFloat();
            return p;
        }
    }
}
