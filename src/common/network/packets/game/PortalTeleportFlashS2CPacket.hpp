// File: src/common/network/packets/game/PortalTeleportFlashS2CPacket.hpp
//
// Server → client: a player just teleported through a portal pair. The
// client should briefly light up BOTH portals of the pair (matches Portal-
// game where firing a portal or stepping through both flashes the rims).
//
// Wire layout:
//   uint64 gunId — owning gun's instance id (PortalRegistry key)
// (No color byte: both portals of the pair flash; the client looks up
// the pair by gunId and applies the flash to both.)

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PortalTeleportFlashS2CPacket {
        uint64_t gunId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PortalTeleportFlashS2CPacket& p) {
            PacketBuffer b;
            b.WriteLong(p.gunId);
            return b.GetData();
        }

        inline PortalTeleportFlashS2CPacket DeserializePortalTeleportFlashS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PortalTeleportFlashS2CPacket p;
            p.gunId = r.ReadLong();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_PORTAL_GUN
