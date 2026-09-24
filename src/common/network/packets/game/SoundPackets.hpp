// File: src/common/network/packets/game/SoundPackets.hpp
//
// MC ClientboundSoundPacket and ClientboundSoundEntityPacket — the server
// telling a client to play a sound (ServerLevel.playSeededSound →
// PlayerList.broadcast). See common/sound/LevelSound.hpp for who is sent what.
//
// The sound is MC's Holder<SoundEvent> encoding (ByteBufCodecs.holder over
// SoundEvent.DIRECT_STREAM_CODEC):
//   VarInt n; n > 0  → registry entry n - 1 (Game::SoundEvents registry id)
//             n == 0 → inline: String id, byte hasFixedRange, [float range]
// Engine and mod events ("aether:block.aether_portal.ambient") are not in the
// vanilla registry and travel inline, which is exactly what MC does for a
// datapack's unregistered event.
//
// ClientboundSoundPacket (MC field order):
//   holder sound, VarInt source (SoundSource ordinal), int x*8, int y*8,
//   int z*8, float volume, float pitch, long seed
// ClientboundSoundEntityPacket:
//   holder sound, VarInt source, VarInt entityId, float volume, float pitch,
//   long seed
// New fields go on the end (trailing-field extension; readers check HasMore).
//
// Both are world-scoped (sent with SendPacketIn), and the client plays them
// only when their scope is the level it stands in.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundSource.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Network {

    struct SoundS2CPacket {
        std::string       event;   // the event id ("block.stone.break")
        Game::SoundSource source = Game::SoundSource::Master;
        int32_t           x = 0, y = 0, z = 0;   // position * 8, as MC
        float             volume = 1.0f;
        float             pitch  = 1.0f;
        int64_t           seed   = 0;

        // MC's (int)(x * 8.0F) — fixed-point eighths of a block.
        void SetPosition(const glm::dvec3& pos) {
            x = static_cast<int32_t>(pos.x * 8.0);
            y = static_cast<int32_t>(pos.y * 8.0);
            z = static_cast<int32_t>(pos.z * 8.0);
        }
        glm::dvec3 Position() const {
            return glm::dvec3(static_cast<double>(x) / 8.0, static_cast<double>(y) / 8.0,
                              static_cast<double>(z) / 8.0);
        }
    };

    struct SoundEntityS2CPacket {
        std::string       event;
        Game::SoundSource source = Game::SoundSource::Master;
        int32_t           entityId = 0;
        float             volume = 1.0f;
        float             pitch  = 1.0f;
        int64_t           seed   = 0;
    };

    namespace Serialization {

        // Holder<SoundEvent> (see the header note). Vanilla events never carry
        // a fixed range in 26.3, so an inline event writes hasFixedRange = 0.
        inline void WriteSoundHolder(PacketBuffer& b, std::string_view event) {
            const int registryId = Game::SoundEvents::RegistryId(event);
            if (registryId >= 0) {
                b.WriteVarInt(static_cast<uint32_t>(registryId + 1));
            } else {
                b.WriteVarInt(0);
                b.WriteString(std::string(event));
                b.WriteByte(0);
            }
        }

        inline std::string ReadSoundHolder(PacketReader& r) {
            const uint32_t n = r.ReadVarInt();
            if (n > 0) {
                const char* id = Game::SoundEvents::ByRegistryId(static_cast<int>(n) - 1);
                return id ? std::string(id) : std::string();
            }
            std::string id = r.ReadString(32767);
            if (r.ReadByte() != 0) (void)r.ReadFloat();   // fixed range: unused by the client
            return id;
        }

        inline std::vector<uint8_t> Serialize(const SoundS2CPacket& p) {
            PacketBuffer b(48);
            WriteSoundHolder(b, p.event);
            b.WriteVarInt(static_cast<uint32_t>(p.source));
            b.WriteInt(static_cast<uint32_t>(p.x));
            b.WriteInt(static_cast<uint32_t>(p.y));
            b.WriteInt(static_cast<uint32_t>(p.z));
            b.WriteFloat(p.volume);
            b.WriteFloat(p.pitch);
            b.WriteLong(static_cast<uint64_t>(p.seed));
            return b.GetData();
        }

        inline SoundS2CPacket DeserializeSoundS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            SoundS2CPacket p;
            p.event  = ReadSoundHolder(r);
            p.source = Game::SoundSourceFromRaw(r.ReadVarInt());
            p.x      = static_cast<int32_t>(r.ReadInt());
            p.y      = static_cast<int32_t>(r.ReadInt());
            p.z      = static_cast<int32_t>(r.ReadInt());
            p.volume = r.ReadFloat();
            p.pitch  = r.ReadFloat();
            p.seed   = static_cast<int64_t>(r.ReadLong());
            return p;
        }

        inline std::vector<uint8_t> Serialize(const SoundEntityS2CPacket& p) {
            PacketBuffer b(40);
            WriteSoundHolder(b, p.event);
            b.WriteVarInt(static_cast<uint32_t>(p.source));
            b.WriteVarInt(static_cast<uint32_t>(p.entityId));
            b.WriteFloat(p.volume);
            b.WriteFloat(p.pitch);
            b.WriteLong(static_cast<uint64_t>(p.seed));
            return b.GetData();
        }

        inline SoundEntityS2CPacket DeserializeSoundEntityS2C(const std::vector<uint8_t>& data) {
            PacketReader r(data);
            SoundEntityS2CPacket p;
            p.event    = ReadSoundHolder(r);
            p.source   = Game::SoundSourceFromRaw(r.ReadVarInt());
            p.entityId = static_cast<int32_t>(r.ReadVarInt());
            p.volume   = r.ReadFloat();
            p.pitch    = r.ReadFloat();
            p.seed     = static_cast<int64_t>(r.ReadLong());
            return p;
        }

    } // namespace Serialization

} // namespace Network
