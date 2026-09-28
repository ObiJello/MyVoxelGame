// File: src/common/network/packets/game/FireworkRocketDataS2CPacket.hpp
//
// A firework rocket's synched data — MC FireworkRocketEntity's
// DATA_ID_FIREWORKS_ITEM (the whole stack: its FIREWORKS explosions are what
// entity event 17 throws on the client), DATA_ATTACHED_TO_TARGET (the entity
// riding it, -1 for none) and DATA_SHOT_AT_ANGLE. The shared mob packets
// carry single bytes and have no room for a stack, so it is its own packet:
// sent to a watcher right after the rocket's add packet, and to every watcher
// whenever it changes (FireworkRocket::ConsumeDataDirty).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/entity/Item.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Network {

    struct FireworkRocketDataS2CPacket {
        int32_t         entityId = 0;
        Game::ItemStack item{};
        int32_t         attachedToId = -1;
        bool            shotAtAngle = false;
        // The rocket's LifeTime (server-side state MC never syncs; carried
        // for the client's elytra-boost diagnostics). Trailing.
        int32_t         lifetime = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const FireworkRocketDataS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            WriteItemStack(b, p.item);
            // OPTIONAL_UNSIGNED_INT: 0 = empty, else id + 1.
            b.WriteVarInt(p.attachedToId < 0 ? 0u : static_cast<uint32_t>(p.attachedToId) + 1u);
            b.WriteByte(p.shotAtAngle ? 1 : 0);
            b.WriteVarInt(static_cast<uint32_t>(std::max(0, p.lifetime)));
            return b.GetData();
        }

        inline FireworkRocketDataS2CPacket DeserializeFireworkRocketDataS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            FireworkRocketDataS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.item = ReadItemStack(r);
            const uint32_t attached = r.ReadVarInt();
            p.attachedToId = attached == 0 ? -1 : static_cast<int32_t>(attached - 1u);
            p.shotAtAngle = r.ReadByte() != 0;
            p.lifetime = r.HasMore() ? static_cast<int32_t>(r.ReadVarInt()) : 0;
            return p;
        }

    } // namespace Serialization

} // namespace Network
