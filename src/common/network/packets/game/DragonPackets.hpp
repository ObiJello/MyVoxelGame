// File: src/common/network/packets/game/DragonPackets.hpp
//
// Server → client packets for the End dragon fight.
//
//   BossEventS2CPacket — MC ClientboundBossEventPacket, reduced to the ops
//   this engine's one boss (the dragon) exercises: add (name + style),
//   remove, and progress. MC keys bars by UUID; a byte id is enough for a
//   fight-owned bar and leaves room for a wither later.
//
//   EndCrystalBeamS2CPacket — MC syncs a crystal's beam target as the
//   DATA_BEAM_TARGET entity-data entry; this engine's SetEntityData packet is
//   a flat struct of bytes with no room for an optional BlockPos, so the beam
//   rides its own packet. Sent by ServerEntityTracker when a crystal enters
//   tracking (only if a target is set) and whenever the target changes —
//   including clearing (hasTarget false).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct BossEventS2CPacket {
        enum class Op : uint8_t {
            Add = 0,            // show the bar: name, color, overlay, progress
            Remove = 1,         // hide the bar
            UpdateProgress = 2, // progress only
        };
        // MC BossEvent.BossBarColor ordinals — also the sprite name prefix
        // under assets/textures/gui/sprites/boss_bar/ (pink_progress.png...).
        enum class Color : uint8_t {
            Pink = 0, Blue, Red, Green, Yellow, Purple, White,
        };
        // MC BossEvent.BossBarOverlay: 0 = smooth, 1/2/3 = notched 6/10/12/20
        // — kept as the notch count so the client picks the sprite directly.
        Op          op = Op::Add;
        uint8_t     barId = 0;          // one live bar today (the dragon)
        float       progress = 1.0f;    // 0..1
        Color       color = Color::Pink;
        uint8_t     notches = 0;        // 0 smooth, else 6/10/12/20
        std::string name;               // Add only
    };

    struct EndCrystalBeamS2CPacket {
        int32_t    entityId = 0;
        bool       hasTarget = false;
        glm::ivec3 target{0};
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const BossEventS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.op));
            b.WriteByte(p.barId);
            b.WriteFloat(p.progress);
            b.WriteByte(static_cast<uint8_t>(p.color));
            b.WriteByte(p.notches);
            b.WriteString(p.name);
            return b.GetData();
        }

        inline BossEventS2CPacket
        DeserializeBossEventS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            BossEventS2CPacket p;
            p.op = static_cast<BossEventS2CPacket::Op>(r.ReadByte());
            p.barId = r.ReadByte();
            p.progress = r.ReadFloat();
            p.color = static_cast<BossEventS2CPacket::Color>(r.ReadByte());
            p.notches = r.ReadByte();
            p.name = r.ReadString();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const EndCrystalBeamS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteByte(p.hasTarget ? 1 : 0);
            b.WriteInt(static_cast<uint32_t>(p.target.x));
            b.WriteInt(static_cast<uint32_t>(p.target.y));
            b.WriteInt(static_cast<uint32_t>(p.target.z));
            return b.GetData();
        }

        inline EndCrystalBeamS2CPacket
        DeserializeEndCrystalBeamS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            EndCrystalBeamS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.hasTarget = r.ReadByte() != 0;
            p.target.x = static_cast<int32_t>(r.ReadInt());
            p.target.y = static_cast<int32_t>(r.ReadInt());
            p.target.z = static_cast<int32_t>(r.ReadInt());
            return p;
        }

    } // namespace Serialization

} // namespace Network
