// File: src/common/network/packets/game/PortalFizzleS2CPacket.hpp
//
// Server → client: one-shot particle burst event. Sent on:
//   • placement attempt failed → reason = BadSurface (client bursts at hit point)
//   • portal removed (wall broken, replaced by re-fire, or shift-clear) →
//     reason = Close (client bursts inward at the old portal's center)
//
// This is a fire-and-forget visual event — the client uses it purely to drive
// PortalParticleSystem::EmitOneShot. No state is created or destroyed on the
// client by this packet alone.
//
// Wire layout:
//   3×f64   origin  — world-space center of the burst
//   3×f32   normal  — orientation (portal-out-of-wall direction)
//   uint8   color   — 0 = blue, 1 = orange (palette select)
//   uint8   reason  — see FizzleReason in PortalParticleSystem.hpp

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    struct PortalFizzleS2CPacket {
        double  originX = 0.0, originY = 0.0, originZ = 0.0;
        float   normalX = 0.0f, normalY = 0.0f, normalZ = 1.0f;
        uint8_t color   = 0;        // 0=blue, 1=orange
        uint8_t reason  = 0;        // 0=BadSurface, 1=Close
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PortalFizzleS2CPacket& p) {
            PacketBuffer b;
            b.WriteDouble(p.originX);
            b.WriteDouble(p.originY);
            b.WriteDouble(p.originZ);
            b.WriteFloat(p.normalX);
            b.WriteFloat(p.normalY);
            b.WriteFloat(p.normalZ);
            b.WriteByte(p.color);
            b.WriteByte(p.reason);
            return b.GetData();
        }

        inline PortalFizzleS2CPacket DeserializePortalFizzleS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PortalFizzleS2CPacket p;
            p.originX = r.ReadDouble();
            p.originY = r.ReadDouble();
            p.originZ = r.ReadDouble();
            p.normalX = r.ReadFloat();
            p.normalY = r.ReadFloat();
            p.normalZ = r.ReadFloat();
            p.color   = r.ReadByte();
            p.reason  = r.ReadByte();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_PORTAL_GUN
