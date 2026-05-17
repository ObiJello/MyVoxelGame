// File: src/common/network/packets/game/PortalRemoveS2CPacket.hpp
//
// Server → client: a portal (or a whole pair) is gone. Sent on:
//   • shift-clear gesture        → color = 2 (BOTH)
//   • per-color removal events   → color = 0 (BLUE) / 1 (ORANGE)  (currently
//     unused — Phase 2 only ever clears whole pairs, but the per-color form
//     is reserved here so a future "gun destroyed mid-pair" path can use it
//     without growing the wire format).
//
// Wire layout:
//   uint64  gunId
//   uint8   color   — 0 = BLUE, 1 = ORANGE, 2 = BOTH (whole pair)

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PortalRemoveS2CPacket {
        uint64_t gunId = 0;
        uint8_t  color = 2;       // default 2 = BOTH (the common "shift-clear" case)
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PortalRemoveS2CPacket& p) {
            PacketBuffer b;
            b.WriteLong(p.gunId);
            b.WriteByte(p.color);
            return b.GetData();
        }

        inline PortalRemoveS2CPacket DeserializePortalRemoveS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PortalRemoveS2CPacket p;
            p.gunId = r.ReadLong();
            p.color = r.ReadByte();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_PORTAL_GUN
