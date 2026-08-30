// File: src/common/network/packets/game/ExplodeS2CPacket.hpp
//
// MC ClientboundExplodePacket — "a blast happened here, draw it".
//
// The client does NOT simulate explosions (MC's ClientLevel.explode is an
// empty method); it is told about them. That is why this packet carries the
// destroyed-block COUNT rather than the blocks themselves: the count only
// drives how much debris the particle tracker throws, and the actual block
// changes arrive through the ordinary section-change path, coalesced.
//
// It also carries the receiving player's knockback. Player movement is
// client-authoritative here, so a server-side velocity write on the player's
// entity view would be overwritten by their next move packet — the push has to
// be handed to the client to apply itself, which is exactly what vanilla does.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Network {

    struct ExplodeS2CPacket {
        glm::dvec3 center{0.0};
        float      radius = 0.0f;
        // Number of blocks the blast destroyed. Feeds MC's ClientExplosionTracker
        // debris loop, which spawns min(blockCount, 512) particles.
        int32_t    blockCount = 0;
        // MC Explosion.isSmall() — radius < 2, or the blast touched no blocks.
        // Picks EXPLOSION over EXPLOSION_EMITTER.
        bool       small = false;
        // The push to apply to THIS receiving player. Zero for a player who was
        // out of range, spectating, or flying in creative.
        glm::vec3  playerKnockback{0.0f};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ExplodeS2CPacket& p) {
            PacketBuffer b;
            b.WriteDouble(p.center.x);
            b.WriteDouble(p.center.y);
            b.WriteDouble(p.center.z);
            b.WriteFloat(p.radius);
            b.WriteInt(static_cast<uint32_t>(p.blockCount));
            b.WriteByte(p.small ? 1 : 0);
            b.WriteFloat(p.playerKnockback.x);
            b.WriteFloat(p.playerKnockback.y);
            b.WriteFloat(p.playerKnockback.z);
            return b.GetData();
        }

        inline ExplodeS2CPacket DeserializeExplodeS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ExplodeS2CPacket p;
            p.center.x   = r.ReadDouble();
            p.center.y   = r.ReadDouble();
            p.center.z   = r.ReadDouble();
            p.radius     = r.ReadFloat();
            p.blockCount = static_cast<int32_t>(r.ReadInt());
            p.small      = r.ReadByte() != 0;
            p.playerKnockback.x = r.ReadFloat();
            p.playerKnockback.y = r.ReadFloat();
            p.playerKnockback.z = r.ReadFloat();
            return p;
        }

    } // namespace Serialization

} // namespace Network
