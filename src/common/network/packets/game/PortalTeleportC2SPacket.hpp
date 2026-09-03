// File: src/common/network/packets/game/PortalTeleportC2SPacket.hpp
//
// Client → server: "my eye just crossed immersive portal P".
//
// Crossing is decided on the CLIENT, per frame, on the eye's path between
// two frames (the Immersive Portals mod's ClientTeleportationManager): that
// is the only place it can be decided without the camera spending a frame
// on the wrong side. The client has already moved itself; the server
// validates loosely (the reported position must be near where it believes
// the player is, and near the portal) and moves its own copy of the player
// — into another level if the portal leads there — without a respawn.
// A rejected crossing is answered with a position resync, which puts the
// client back.
//
// Wire layout:
//   int8    dimensionBefore    — where the client was standing
//   3×f64   eye                 — eye position just before the crossing
//   VarInt  portalId
//   2×f32   yaw, pitch          — after the crossing (the portal may rotate)
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct PortalTeleportC2SPacket {
        int8_t   dimensionBefore = 0;
        double   eyeX = 0.0, eyeY = 0.0, eyeZ = 0.0;
        uint32_t portalId = 0;
        float    yaw = 0.0f;
        float    pitch = 0.0f;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const PortalTeleportC2SPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.dimensionBefore));
            b.WriteDouble(p.eyeX); b.WriteDouble(p.eyeY); b.WriteDouble(p.eyeZ);
            b.WriteVarInt(p.portalId);
            b.WriteFloat(p.yaw);
            b.WriteFloat(p.pitch);
            return b.GetData();
        }

        inline PortalTeleportC2SPacket DeserializePortalTeleportC2S(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            PortalTeleportC2SPacket p;
            p.dimensionBefore = static_cast<int8_t>(r.ReadByte());
            p.eyeX = r.ReadDouble(); p.eyeY = r.ReadDouble(); p.eyeZ = r.ReadDouble();
            p.portalId = r.ReadVarInt();
            p.yaw   = r.ReadFloat();
            p.pitch = r.ReadFloat();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_IMMERSIVE_PORTALS
