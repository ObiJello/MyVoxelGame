// File: src/common/network/packets/game/PortalSetS2CPacket.hpp
//
// Server → client: announce that a portal of color C now exists at the given
// world-space pose, replacing any prior portal of the same color on the same
// gun. Sent on:
//   • initial fire (PlacePortal succeeds)
//   • re-fire of an existing color (the portal moved)
//   • on-join sync (catches a fresh client up to all currently active portals)
//
// Wire layout (fixed-width, big-endian per the project's PacketBuffer):
//   uint64  gunId         — owning gun's instance id (PortalRegistry key)
//   uint8   color         — 0 = blue, 1 = orange  (matches PortalColor enum)
//   3×f64   origin        — world-space center of the 1×2 rectangle
//   3×f32   normal        — outward face normal (unit)
//   3×f32   upDir         — portal's local +Y / long axis (unit)
//
// `right` is intentionally NOT on the wire — the client recomputes it as
// cross(upDir, normal). Both vectors are unit by construction so the cross
// is also unit, saving 12 bytes per packet.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PortalSetS2CPacket {
        uint64_t gunId   = 0;
        uint8_t  color   = 0;     // 0 = blue, 1 = orange (mirrors Game::Portal::PortalColor)
        double   originX = 0.0, originY = 0.0, originZ = 0.0;
        float    normalX = 0.0f, normalY = 0.0f, normalZ = 1.0f;
        float    upX     = 0.0f, upY     = 1.0f, upZ     = 0.0f;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PortalSetS2CPacket& p) {
            PacketBuffer b;
            b.WriteLong(p.gunId);
            b.WriteByte(p.color);
            b.WriteDouble(p.originX);
            b.WriteDouble(p.originY);
            b.WriteDouble(p.originZ);
            b.WriteFloat(p.normalX);
            b.WriteFloat(p.normalY);
            b.WriteFloat(p.normalZ);
            b.WriteFloat(p.upX);
            b.WriteFloat(p.upY);
            b.WriteFloat(p.upZ);
            return b.GetData();
        }

        inline PortalSetS2CPacket DeserializePortalSetS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PortalSetS2CPacket p;
            p.gunId   = r.ReadLong();
            p.color   = r.ReadByte();
            p.originX = r.ReadDouble();
            p.originY = r.ReadDouble();
            p.originZ = r.ReadDouble();
            p.normalX = r.ReadFloat();
            p.normalY = r.ReadFloat();
            p.normalZ = r.ReadFloat();
            p.upX     = r.ReadFloat();
            p.upY     = r.ReadFloat();
            p.upZ     = r.ReadFloat();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_PORTAL_GUN
