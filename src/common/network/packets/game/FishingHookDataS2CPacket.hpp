// File: src/common/network/packets/game/FishingHookDataS2CPacket.hpp
//
// A fishing bobber's synched state (Game::FishingHook): MC's DATA_HOOKED_ENTITY
// and DATA_BITING entity data, the owner id MC sends as the hook's
// ClientboundAddEntityPacket `data`, and the owner's holding arm, which MC's
// renderer reads off the owner's hands and a client here only knows for its
// own player. The shared mob packets have no room for them.
//
// Sent to a watcher right after the hook's AddEntityS2C and to every watcher
// whenever any field changes (hooked / unhooked, a bite starting or ending,
// the rod moving to the other hand). Ids are the entity ids the client keys
// on: a player's connection id, a mob's, or a dropped item's; -1 = none.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct FishingHookDataS2CPacket {
        static constexpr uint8_t kFlagBiting  = 0x01;
        static constexpr uint8_t kFlagOffhand = 0x02;

        int32_t entityId = 0;
        int32_t ownerId = -1;    // the fishing player
        int32_t hookedId = -1;   // what the hook is stuck in, -1 = nothing
        uint8_t flags = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const FishingHookDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteInt(static_cast<uint32_t>(p.ownerId));
            b.WriteInt(static_cast<uint32_t>(p.hookedId));
            b.WriteByte(p.flags);
            return b.GetData();
        }

        inline FishingHookDataS2CPacket DeserializeFishingHookDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            FishingHookDataS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.ownerId  = static_cast<int32_t>(r.ReadInt());
            p.hookedId = static_cast<int32_t>(r.ReadInt());
            p.flags    = r.ReadByte();
            return p;
        }

    } // namespace Serialization

} // namespace Network
