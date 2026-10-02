// File: src/common/network/packets/game/DragonPackets.hpp
//
// Server → client packets for the End dragon fight.
//
//   BossEventS2CPacket — MC ClientboundBossEventPacket, reduced to the ops
//   the engine's bosses exercise: add (name + style + properties), remove,
//   progress and name. MC keys bars by UUID; a byte id is enough here — 0 is
//   the dragon fight's (and the Hush bosses'), 1..255 the withers'
//   (WitherBossEvents).
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
            Add = 0,            // show the bar: name, color, overlay, progress, properties
            Remove = 1,         // hide the bar
            UpdateProgress = 2, // progress only
            UpdateName = 3,     // name only (a renamed wither)
        };
        // MC UpdatePropertiesOperation's three booleans, as bits.
        static constexpr uint8_t kDarkenScreen   = 0x01;
        static constexpr uint8_t kPlayBossMusic  = 0x02;
        static constexpr uint8_t kCreateWorldFog = 0x04;
        // MC BossEvent.BossBarColor ordinals — also the sprite name prefix
        // under assets/textures/gui/sprites/boss_bar/ (pink_progress.png...).
        enum class Color : uint8_t {
            Pink = 0, Blue, Red, Green, Yellow, Purple, White,
        };
        // MC BossEvent.BossBarOverlay: 0 = smooth, 1/2/3 = notched 6/10/12/20
        // — kept as the notch count so the client picks the sprite directly.
        Op          op = Op::Add;
        uint8_t     barId = 0;          // 0 dragon / Hush, 1..255 withers
        float       progress = 1.0f;    // 0..1
        Color       color = Color::Pink;
        uint8_t     notches = 0;        // 0 smooth, else 6/10/12/20
        std::string name;               // Add / UpdateName
        // APPENDED: the kDarkenScreen / kPlayBossMusic / kCreateWorldFog
        // bits (Add only); absent decodes as none.
        uint8_t     properties = 0;
    };

    struct EndCrystalBeamS2CPacket {
        int32_t    entityId = 0;
        bool       hasTarget = false;
        glm::ivec3 target{0};
    };

    // MC WitherBoss DATA_TARGET_B / DATA_TARGET_C — the side heads'
    // alternative targets (entity ids, 0 = none), which the client's aiStep
    // turns each side head toward. Sent by ServerEntityTracker when a wither
    // enters tracking (only if a head has a target) and whenever either
    // changes.
    struct WitherHeadTargetsS2CPacket {
        int32_t entityId = 0;
        int32_t targets[2] = { 0, 0 };
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
            b.WriteByte(p.properties);
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
            if (r.Remaining() >= 1) p.properties = r.ReadByte();
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

        inline std::vector<uint8_t> Serialize(const WitherHeadTargetsS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteInt(static_cast<uint32_t>(p.targets[0]));
            b.WriteInt(static_cast<uint32_t>(p.targets[1]));
            return b.GetData();
        }

        inline WitherHeadTargetsS2CPacket
        DeserializeWitherHeadTargetsS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            WitherHeadTargetsS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.targets[0] = static_cast<int32_t>(r.ReadInt());
            p.targets[1] = static_cast<int32_t>(r.ReadInt());
            return p;
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
