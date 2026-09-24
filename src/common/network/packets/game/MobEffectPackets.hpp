// File: src/common/network/packets/game/MobEffectPackets.hpp
//
// MC ClientboundUpdateMobEffectPacket / ClientboundRemoveMobEffectPacket.
//
// Sent ONLY to a player about their own effects (MC ServerPlayer.onEffect*
// and PlayerList.sendActivePlayerEffects): the full instance, so the client
// can draw the HUD icons and the inventory list and run everything the local
// player's own simulation needs (speed, jump boost, levitation, slow falling,
// dolphin's grace, blindness, darkness, night vision, nausea…). Other
// clients see an entity's effects only as particles and flags — the synched
// EffectVisuals on AddEntity / SetEntityData / PlayerUpdate.
//
// `entityId` is the player's connection id — the id the client's own player
// answers to (as PlayerSleepS2C uses it). MC's field order, MC's flags:
//   VarInt entityId, VarInt effect (registry id = Game::MobEffectId ordinal),
//   VarInt amplifier, VarInt duration (-1 = infinite), byte flags.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct UpdateMobEffectS2CPacket {
        static constexpr uint8_t kFlagAmbient  = 0x01;   // MC FLAG_AMBIENT
        static constexpr uint8_t kFlagVisible  = 0x02;   // MC FLAG_VISIBLE
        static constexpr uint8_t kFlagShowIcon = 0x04;   // MC FLAG_SHOW_ICON
        static constexpr uint8_t kFlagBlend    = 0x08;   // MC FLAG_BLEND

        int32_t  entityId  = 0;
        uint8_t  effectId  = 0;
        int32_t  amplifier = 0;
        int32_t  duration  = 0;
        uint8_t  flags     = 0;

        bool IsAmbient()   const { return (flags & kFlagAmbient)  != 0; }
        bool IsVisible()   const { return (flags & kFlagVisible)  != 0; }
        bool ShowsIcon()   const { return (flags & kFlagShowIcon) != 0; }
        bool ShouldBlend() const { return (flags & kFlagBlend)    != 0; }
    };

    struct RemoveMobEffectS2CPacket {
        int32_t entityId = 0;
        uint8_t effectId = 0;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const UpdateMobEffectS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(p.effectId);
            b.WriteVarInt(static_cast<uint32_t>(p.amplifier));
            b.WriteVarInt(static_cast<uint32_t>(p.duration));   // two's complement, as Java's writeVarInt
            b.WriteByte(p.flags);
            return b.GetData();
        }

        inline UpdateMobEffectS2CPacket DeserializeUpdateMobEffectS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            UpdateMobEffectS2CPacket p;
            p.entityId  = static_cast<int32_t>(r.ReadVarInt());
            p.effectId  = static_cast<uint8_t>(r.ReadVarInt());
            p.amplifier = static_cast<int32_t>(r.ReadVarInt());
            p.duration  = static_cast<int32_t>(r.ReadVarInt());
            p.flags     = r.ReadByte();
            return p;
        }

        inline std::vector<uint8_t> Serialize(const RemoveMobEffectS2CPacket& p) {
            PacketBuffer b;
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteVarInt(p.effectId);
            return b.GetData();
        }

        inline RemoveMobEffectS2CPacket DeserializeRemoveMobEffectS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            RemoveMobEffectS2CPacket p;
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.effectId = static_cast<uint8_t>(r.ReadVarInt());
            return p;
        }

        // The synched effect visuals (Game::EffectVisuals) as a trailing
        // block on AddEntity / SetEntityData / PlayerUpdate: flags byte,
        // count byte, one byte per visible effect. Readers treat absence as
        // "no effects".
        inline void WriteEffectVisuals(PacketBuffer& b, uint8_t flags,
                                       const std::vector<uint8_t>& particles) {
            b.WriteByte(flags);
            const size_t n = particles.size() > 255 ? 255 : particles.size();
            b.WriteByte(static_cast<uint8_t>(n));
            for (size_t i = 0; i < n; ++i) b.WriteByte(particles[i]);
        }

        inline void ReadEffectVisuals(PacketReader& r, uint8_t& flags,
                                      std::vector<uint8_t>& particles) {
            flags = 0;
            particles.clear();
            if (r.Remaining() < 2) return;
            flags = r.ReadByte();
            const uint8_t n = r.ReadByte();
            if (r.Remaining() < n) return;
            particles.reserve(n);
            for (uint8_t i = 0; i < n; ++i) particles.push_back(r.ReadByte());
        }

    } // namespace Serialization

} // namespace Network
