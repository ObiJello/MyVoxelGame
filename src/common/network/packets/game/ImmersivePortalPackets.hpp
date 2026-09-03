// File: src/common/network/packets/game/ImmersivePortalPackets.hpp
//
// Server → client sync of immersive portals (Game::Immersive::Portal).
//
// Two packets, mirroring how the Immersive Portals mod syncs its portal
// entities — a FULL record every time rather than deltas, because a portal
// changes rarely and wholesale, and a client that missed one update must
// still end up with the right record:
//
//   ImmersivePortalSyncS2C    upsert one portal (create or replace by id)
//   ImmersivePortalRemoveS2C  drop one portal by id
//
// Who receives them: the watchers of the portal's ORIGIN chunk. A portal is
// sent alongside its chunk (PlayerSession → IntegratedServer::OnChunkSentToClient),
// re-sent to those watchers when it changes, and dropped by the client when
// that chunk unloads — so its lifetime on the client is its chunk's, and no
// separate "forget everything" pass is needed on dimension change.
//
// Wire layout of a portal (big-endian, fixed-width per PacketBuffer):
//   VarInt  id                 uint8  kind            VarInt flags
//   int8    dimension          3×f64  origin
//   3×f32   axisW              3×f32  axisH           f64 width   f64 height
//   int8    destDimension      3×f64  destination
//   4×f32   rotation (w,x,y,z) f64    scale
//   VarInt  specificPlayerId   VarInt reverse   VarInt flipped   VarInt parallel
//   string  tag
//   uint8   shapeType          [Mesh: VarInt n, n×(2×f32) vertices, VarInt m, m×VarInt indices]
//
// Axes are floats: they are unit vectors, and 24 bits of mantissa is far
// past anything the frame's orthonormality check can tell apart. Positions
// are doubles because a portal a million blocks out must not drift.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/network/PacketRegistry.hpp"
#include "common/portal/ImmersivePortal.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct ImmersivePortalSyncS2CPacket {
        Game::Immersive::Portal portal;
    };

    struct ImmersivePortalRemoveS2CPacket {
        Game::Immersive::PortalId portalId = Game::Immersive::kInvalidPortalId;
    };

    namespace Serialization {

        // Shared by the sync packet and by anything else that wants the
        // wire form of a portal (the save format deliberately does not —
        // saves are JSON so they stay readable and editable).
        inline void WritePortal(PacketBuffer& b, const Game::Immersive::Portal& p) {
            using namespace Game::Immersive;
            b.WriteVarInt(p.id);
            b.WriteByte(static_cast<uint8_t>(p.kind));
            b.WriteVarInt(p.flags);
            b.WriteByte(static_cast<uint8_t>(static_cast<int8_t>(Game::DimensionToRaw(p.dimension))));
            b.WriteDouble(p.origin.x); b.WriteDouble(p.origin.y); b.WriteDouble(p.origin.z);
            b.WriteFloat(static_cast<float>(p.axisW.x));
            b.WriteFloat(static_cast<float>(p.axisW.y));
            b.WriteFloat(static_cast<float>(p.axisW.z));
            b.WriteFloat(static_cast<float>(p.axisH.x));
            b.WriteFloat(static_cast<float>(p.axisH.y));
            b.WriteFloat(static_cast<float>(p.axisH.z));
            b.WriteDouble(p.width);
            b.WriteDouble(p.height);
            b.WriteByte(static_cast<uint8_t>(static_cast<int8_t>(Game::DimensionToRaw(p.destDimension))));
            b.WriteDouble(p.destination.x); b.WriteDouble(p.destination.y); b.WriteDouble(p.destination.z);
            b.WriteFloat(static_cast<float>(p.rotation.w));
            b.WriteFloat(static_cast<float>(p.rotation.x));
            b.WriteFloat(static_cast<float>(p.rotation.y));
            b.WriteFloat(static_cast<float>(p.rotation.z));
            b.WriteDouble(p.scale);
            b.WriteVarInt(p.specificPlayerId);
            b.WriteVarInt(p.reversePortalId);
            b.WriteVarInt(p.flippedPortalId);
            b.WriteVarInt(p.parallelPortalId);
            b.WriteString(p.tag);
            b.WriteByte(static_cast<uint8_t>(p.shape.type));
            if (p.shape.type == PortalShape::Type::Mesh) {
                b.WriteVarInt(static_cast<uint32_t>(p.shape.vertices.size()));
                for (const auto& v : p.shape.vertices) { b.WriteFloat(v.x); b.WriteFloat(v.y); }
                b.WriteVarInt(static_cast<uint32_t>(p.shape.indices.size()));
                for (uint32_t i : p.shape.indices) b.WriteVarInt(i);
            }
        }

        inline Game::Immersive::Portal ReadPortal(PacketReader& r) {
            using namespace Game::Immersive;
            // Caps that no real portal approaches, so a corrupt length field
            // cannot make the reader allocate gigabytes before it fails.
            constexpr uint32_t kMaxShapeVertices = 65536;
            constexpr uint32_t kMaxShapeIndices  = 3 * 65536;
            constexpr size_t   kMaxTagLength     = 256;

            Portal p;
            p.id    = r.ReadVarInt();
            p.kind  = static_cast<PortalKind>(r.ReadByte());
            p.flags = r.ReadVarInt();
            p.dimension = Game::DimensionFromRaw(static_cast<int8_t>(r.ReadByte()));
            p.origin.x = r.ReadDouble(); p.origin.y = r.ReadDouble(); p.origin.z = r.ReadDouble();
            p.axisW.x = r.ReadFloat(); p.axisW.y = r.ReadFloat(); p.axisW.z = r.ReadFloat();
            p.axisH.x = r.ReadFloat(); p.axisH.y = r.ReadFloat(); p.axisH.z = r.ReadFloat();
            p.width  = r.ReadDouble();
            p.height = r.ReadDouble();
            p.destDimension = Game::DimensionFromRaw(static_cast<int8_t>(r.ReadByte()));
            p.destination.x = r.ReadDouble(); p.destination.y = r.ReadDouble(); p.destination.z = r.ReadDouble();
            p.rotation.w = r.ReadFloat();
            p.rotation.x = r.ReadFloat();
            p.rotation.y = r.ReadFloat();
            p.rotation.z = r.ReadFloat();
            p.scale = r.ReadDouble();
            p.specificPlayerId = r.ReadVarInt();
            p.reversePortalId  = r.ReadVarInt();
            p.flippedPortalId  = r.ReadVarInt();
            p.parallelPortalId = r.ReadVarInt();
            p.tag = r.ReadString(kMaxTagLength);
            p.shape.type = static_cast<PortalShape::Type>(r.ReadByte());
            if (p.shape.type == PortalShape::Type::Mesh) {
                const uint32_t nv = r.ReadVarInt();
                if (nv > kMaxShapeVertices) throw std::runtime_error("ImmersivePortal: shape vertex count");
                p.shape.vertices.resize(nv);
                for (auto& v : p.shape.vertices) { v.x = r.ReadFloat(); v.y = r.ReadFloat(); }
                const uint32_t ni = r.ReadVarInt();
                if (ni > kMaxShapeIndices) throw std::runtime_error("ImmersivePortal: shape index count");
                p.shape.indices.resize(ni);
                for (auto& i : p.shape.indices) i = r.ReadVarInt();
            } else {
                p.shape.type = PortalShape::Type::Rectangle;
            }
            // Floats on the wire; snap the frame back to exactly orthonormal
            // so every derived quantity (normal, local frame, reverse) agrees
            // bit-for-bit with what the server computes from its doubles
            // to within the tolerance IsValidGeometry allows.
            p.Orthonormalize();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const ImmersivePortalSyncS2CPacket& p) {
            PacketBuffer b;
            WritePortal(b, p.portal);
            return b.GetData();
        }

        inline ImmersivePortalSyncS2CPacket DeserializeImmersivePortalSyncS2C(
                const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ImmersivePortalSyncS2CPacket p;
            p.portal = ReadPortal(r);
            return p;
        }

        inline std::vector<uint8_t> Serialize(const ImmersivePortalRemoveS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(p.portalId);
            return b.GetData();
        }

        inline ImmersivePortalRemoveS2CPacket DeserializeImmersivePortalRemoveS2C(
                const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ImmersivePortalRemoveS2CPacket p;
            p.portalId = r.ReadVarInt();
            return p;
        }

    } // namespace Serialization

} // namespace Network

#endif // ENABLE_IMMERSIVE_PORTALS
