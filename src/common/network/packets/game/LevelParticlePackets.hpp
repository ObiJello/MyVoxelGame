// File: src/common/network/packets/game/LevelParticlePackets.hpp
//
// MC ClientboundLevelParticlesPacket.
//
//   LevelParticlesS2C — ServerLevel.sendParticles: a particle type with its
//       options, the overrideLimiter / alwaysShow flags, a centre, the
//       per-axis spread (xDist..) and speed, a count and the randomization
//       type. The client (ClientPacketListener.handleParticleEvent) expands
//       it: count 0 is one particle whose velocity is dist * speed; otherwise
//       `count` particles, gaussian-spread (or uniform, for the ALTERNATIVE
//       randomizations) around the centre.
//
// (ClientboundLevelEventPacket is LevelEventS2CPacket.hpp; the client's
// LevelEventHandler port, client/world/ClientLevelEvents.cpp, runs its
// particle half.)
//
// The options travel in this engine's own compact encoding
// (Game::WriteParticleOptions), not MC's registry stream codec; client and
// server ship together.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/particle/LevelParticles.hpp"
#include "common/particle/ParticleOptions.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Network {

    struct LevelParticlesS2CPacket {
        Game::ParticleOptions options;
        bool       overrideLimiter = false;
        bool       alwaysShow = false;
        glm::dvec3 pos{0.0};
        glm::vec3  dist{0.0f};
        glm::vec3  maxSpeed{0.0f};
        int32_t    count = 0;
        Game::Particles::Randomization randomization = Game::Particles::Randomization::Default;
    };

    // Engine packet: what the local client cannot know about its own player
    // but its particles depend on — the server-side /invisible flag (also set
    // by a morph carry / block anchor). Sent on change. Trailing fields may
    // be appended.
    struct SelfParticleStateS2CPacket {
        bool invisible = false;
    };


    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const LevelParticlesS2CPacket& p) {
            PacketBuffer b(64);
            Game::WriteParticleOptions(b, p.options);
            b.WriteByte(static_cast<uint8_t>((p.overrideLimiter ? 1 : 0) | (p.alwaysShow ? 2 : 0)));
            b.WriteDouble(p.pos.x);
            b.WriteDouble(p.pos.y);
            b.WriteDouble(p.pos.z);
            b.WriteFloat(p.dist.x);
            b.WriteFloat(p.dist.y);
            b.WriteFloat(p.dist.z);
            b.WriteFloat(p.maxSpeed.x);
            b.WriteFloat(p.maxSpeed.y);
            b.WriteFloat(p.maxSpeed.z);
            b.WriteVarInt(static_cast<uint32_t>(p.count));
            b.WriteByte(static_cast<uint8_t>(p.randomization));
            return b.GetData();
        }

        inline LevelParticlesS2CPacket DeserializeLevelParticlesS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            LevelParticlesS2CPacket p;
            p.options = Game::ReadParticleOptions(r);
            const uint8_t flags = r.ReadByte();
            p.overrideLimiter = (flags & 1) != 0;
            p.alwaysShow = (flags & 2) != 0;
            p.pos.x = r.ReadDouble();
            p.pos.y = r.ReadDouble();
            p.pos.z = r.ReadDouble();
            p.dist.x = r.ReadFloat();
            p.dist.y = r.ReadFloat();
            p.dist.z = r.ReadFloat();
            p.maxSpeed.x = r.ReadFloat();
            p.maxSpeed.y = r.ReadFloat();
            p.maxSpeed.z = r.ReadFloat();
            p.count = static_cast<int32_t>(r.ReadVarInt());
            const uint8_t randomization = r.ReadByte();
            p.randomization = randomization <= 3 ? static_cast<Game::Particles::Randomization>(randomization)
                                                 : Game::Particles::Randomization::Default;
            return p;
        }

        inline std::vector<uint8_t> Serialize(const SelfParticleStateS2CPacket& p) {
            PacketBuffer b(4);
            b.WriteByte(p.invisible ? 1 : 0);
            return b.GetData();
        }

        inline SelfParticleStateS2CPacket DeserializeSelfParticleStateS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            SelfParticleStateS2CPacket p;
            p.invisible = r.HasMore() ? (r.ReadByte() != 0) : false;
            return p;
        }

    } // namespace Serialization

} // namespace Network
