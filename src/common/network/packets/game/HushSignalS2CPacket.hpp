// File: src/common/network/packets/game/HushSignalS2CPacket.hpp
//
// The Hush's item signals (docs/the-hush.md, "Tools of the deep") — ONE
// small packet with a kind byte, so the three server→client effects the
// items need share an id instead of claiming three:
//
//   ResonancePing — the tuning fork struck a crystal: `origin` is the crystal
//                   (the ring of motes expands from it) and `targets` the
//                   things the ping found (ores, mobs, the nearest vault or
//                   tomb entrance), each an AABB and a colour class, outlined
//                   through walls for `durationTicks`. Sent to the striker only.
//   SonicBurst    — a resonance arrow's burst: a small ring of motes at
//                   `origin`, `radius` wide. Sent to the players near it.
//   CompassTarget — the echo compass's needle target (the nearest Echo
//                   Vault): `origin`.xz, or `hasTarget` false to make it
//                   spin. Sent to its holder when it changes.
//
// MC has no counterpart; the nearest is ClientboundLevelParticlesPacket (for
// the rings) and the lodestone tracker component (for the compass), neither of
// which this engine has.
//
// Wire: byte kind, byte dimension (DimensionToRaw, signed), then per kind:
//   ResonancePing: 3×double origin, VarInt durationTicks, VarInt n,
//                  n × (3×float min, 3×float max, byte class)
//   SonicBurst:    3×double origin, float radius
//   CompassTarget: byte hasTarget, 2×double x/z
// New fields go on the end (trailing-field extension; readers check HasMore).
//
// Handled on the network I/O thread (ClientConnection): the payload lands in
// Client::HushSignalState behind a mutex, and the main thread draws it.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Network {

    struct HushSignalS2CPacket {
        enum class Kind : uint8_t { ResonancePing = 0, SonicBurst = 1, CompassTarget = 2 };
        // What a ping outline is, for its colour.
        enum class TargetClass : uint8_t { Ore = 0, EchoOre = 1, Mob = 2, Structure = 3 };

        struct Target {
            // World-space box. Floats: a mob's box is not block-aligned, and
            // a float holds a block coordinate to 1/16 out to a million.
            glm::vec3   min{0.0f};
            glm::vec3   max{0.0f};
            TargetClass cls = TargetClass::Ore;
        };

        Kind       kind = Kind::ResonancePing;
        int8_t     dimension = 0;
        glm::dvec3 origin{0.0};
        uint32_t   durationTicks = 0;   // ResonancePing
        float      radius = 0.0f;       // SonicBurst
        bool       hasTarget = false;   // CompassTarget
        std::vector<Target> targets;    // ResonancePing
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const HushSignalS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.kind));
            buffer.WriteByte(static_cast<uint8_t>(packet.dimension));
            switch (packet.kind) {
                case HushSignalS2CPacket::Kind::ResonancePing:
                    buffer.WriteDouble(packet.origin.x);
                    buffer.WriteDouble(packet.origin.y);
                    buffer.WriteDouble(packet.origin.z);
                    buffer.WriteVarInt(packet.durationTicks);
                    buffer.WriteVarInt(static_cast<uint32_t>(packet.targets.size()));
                    for (const auto& t : packet.targets) {
                        buffer.WriteFloat(t.min.x);
                        buffer.WriteFloat(t.min.y);
                        buffer.WriteFloat(t.min.z);
                        buffer.WriteFloat(t.max.x);
                        buffer.WriteFloat(t.max.y);
                        buffer.WriteFloat(t.max.z);
                        buffer.WriteByte(static_cast<uint8_t>(t.cls));
                    }
                    break;
                case HushSignalS2CPacket::Kind::SonicBurst:
                    buffer.WriteDouble(packet.origin.x);
                    buffer.WriteDouble(packet.origin.y);
                    buffer.WriteDouble(packet.origin.z);
                    buffer.WriteFloat(packet.radius);
                    break;
                case HushSignalS2CPacket::Kind::CompassTarget:
                    buffer.WriteByte(packet.hasTarget ? 1 : 0);
                    buffer.WriteDouble(packet.origin.x);
                    buffer.WriteDouble(packet.origin.z);
                    break;
            }
            return buffer.GetData();
        }

        inline HushSignalS2CPacket DeserializeHushSignalS2C(const std::vector<uint8_t>& data) {
            HushSignalS2CPacket packet;
            if (data.size() < 2) return packet;
            PacketReader reader(data);
            packet.kind = static_cast<HushSignalS2CPacket::Kind>(reader.ReadByte());
            packet.dimension = static_cast<int8_t>(reader.ReadByte());
            switch (packet.kind) {
                case HushSignalS2CPacket::Kind::ResonancePing: {
                    packet.origin.x = reader.ReadDouble();
                    packet.origin.y = reader.ReadDouble();
                    packet.origin.z = reader.ReadDouble();
                    packet.durationTicks = reader.ReadVarInt();
                    // A hostile count cannot make the client allocate
                    // gigabytes: the server never sends more than a few
                    // hundred, and each entry is 25 bytes on the wire.
                    const uint32_t n = std::min<uint32_t>(reader.ReadVarInt(), 4096u);
                    packet.targets.reserve(n);
                    for (uint32_t i = 0; i < n && reader.HasMore(); ++i) {
                        HushSignalS2CPacket::Target t;
                        t.min.x = reader.ReadFloat();
                        t.min.y = reader.ReadFloat();
                        t.min.z = reader.ReadFloat();
                        t.max.x = reader.ReadFloat();
                        t.max.y = reader.ReadFloat();
                        t.max.z = reader.ReadFloat();
                        t.cls = static_cast<HushSignalS2CPacket::TargetClass>(reader.ReadByte());
                        packet.targets.push_back(t);
                    }
                    break;
                }
                case HushSignalS2CPacket::Kind::SonicBurst:
                    packet.origin.x = reader.ReadDouble();
                    packet.origin.y = reader.ReadDouble();
                    packet.origin.z = reader.ReadDouble();
                    packet.radius = reader.ReadFloat();
                    break;
                case HushSignalS2CPacket::Kind::CompassTarget:
                    packet.hasTarget = reader.ReadByte() != 0;
                    packet.origin.x = reader.ReadDouble();
                    packet.origin.z = reader.ReadDouble();
                    break;
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
