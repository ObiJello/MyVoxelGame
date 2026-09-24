// File: src/common/network/packets/game/AurelithS2CPacket.hpp
//
// Aurelith's cities as a client needs them (docs/the-hush.md, "Reawakening
// the Heart"): where each nearby city's Heart is, how the city is turned,
// and which state of the quest it is in since when. Everything a client
// draws or plays for the quest — the rings spinning up, the beams bending
// into one pillar, the city's music, the Heart's hum growing richer — is a
// pure function of these few numbers and the level's game time
// (Game::Aurelith's timeline in common/world/level/AurelithQuest.hpp), so
// the server never streams an animation, only its start.
//
// ONE packet with a kind byte (the HushSignalS2C shape), so later kinds can
// share the id:
//
//   CityState — upsert one city: its Heart, rotation (-1 unknown: a city
//               generated before the rotation was recorded), state, the game
//               tick its current stage began, and the tick it was awakened
//               (0 = never). Sent to every player in the city's dimension
//               within Server::AurelithCities::kSyncRange of its Heart: when
//               they come into range (or log in, or change dimension), and to
//               all of them on every state change.
//   CityForget — the player left the range: drop the city.
//   Burst      — a one-off flourish at a point (the server has no particles
//                of its own): a cabinet singing open, a key seated, the
//                discord's shards, motes bursting from the Heart. `style`
//                picks the shape (BurstStyle), `colour` tints it (0xRRGGBB).
//                Sent to the players near it.
//
// MC has no counterpart; the nearest is the End's ClientboundBossEvent pair
// (a server-owned event whose client half animates on its own clock).
//
// Wire: byte kind, byte dimension (DimensionToRaw, signed), then per kind:
//   CityState:  3×Int (x, y, z of the Heart), byte rotation (signed),
//               byte state, Long stageStartTick, Long awakenedTick
//   CityForget: 3×Int (the Heart)
//   Burst:      3×Int (the Heart it belongs to, or the block), 3×Double
//               origin, byte style, Int colour
// New fields go on the end (trailing-field extension; readers check HasMore).
//
// Handled on the network I/O thread (ClientConnection): the record lands in
// Client::AurelithState behind a mutex; the main and render threads read
// copies.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Network {

    struct AurelithS2CPacket {
        enum class Kind : uint8_t { CityState = 0, CityForget = 1, Burst = 2 };
        // Burst shapes (Client::AurelithState turns each into particles).
        enum class BurstStyle : uint8_t {
            KeySeat    = 0,   // a small ring of motes rising round a socket
            Discord    = 1,   // shards flung outward, dark and violet
            Cabinet    = 2,   // the cabinet's doors breathe out a plume
            HeartBloom = 3,   // the Heart's motes burst outward (the awakening)
            Resolve    = 4,   // the resolution: a vast slow bloom from the Heart
            Pedestal   = 5,   // an item lifted from / set on a pedestal
        };

        Kind       kind = Kind::CityState;
        int8_t     dimension = 0;
        glm::ivec3 heart{0};
        int8_t     rotation = -1;
        Game::Aurelith::CityState state = Game::Aurelith::CityState::Dormant;
        int64_t    stageStartTick = 0;
        int64_t    awakenedTick = 0;
        // Burst.
        glm::dvec3 origin{0.0};
        BurstStyle style = BurstStyle::KeySeat;
        uint32_t   colour = 0xFFFFFF;
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const AurelithS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.kind));
            buffer.WriteByte(static_cast<uint8_t>(packet.dimension));
            buffer.WriteInt(static_cast<uint32_t>(packet.heart.x));
            buffer.WriteInt(static_cast<uint32_t>(packet.heart.y));
            buffer.WriteInt(static_cast<uint32_t>(packet.heart.z));
            if (packet.kind == AurelithS2CPacket::Kind::CityState) {
                buffer.WriteByte(static_cast<uint8_t>(packet.rotation));
                buffer.WriteByte(static_cast<uint8_t>(packet.state));
                buffer.WriteLong(static_cast<uint64_t>(packet.stageStartTick));
                buffer.WriteLong(static_cast<uint64_t>(packet.awakenedTick));
            } else if (packet.kind == AurelithS2CPacket::Kind::Burst) {
                buffer.WriteDouble(packet.origin.x);
                buffer.WriteDouble(packet.origin.y);
                buffer.WriteDouble(packet.origin.z);
                buffer.WriteByte(static_cast<uint8_t>(packet.style));
                buffer.WriteInt(packet.colour);
            }
            return buffer.GetData();
        }

        inline AurelithS2CPacket DeserializeAurelithS2C(const std::vector<uint8_t>& data) {
            AurelithS2CPacket packet;
            if (data.size() < 14) return packet;
            PacketReader reader(data);
            packet.kind = static_cast<AurelithS2CPacket::Kind>(reader.ReadByte());
            packet.dimension = static_cast<int8_t>(reader.ReadByte());
            packet.heart.x = static_cast<int32_t>(reader.ReadInt());
            packet.heart.y = static_cast<int32_t>(reader.ReadInt());
            packet.heart.z = static_cast<int32_t>(reader.ReadInt());
            if (packet.kind == AurelithS2CPacket::Kind::CityState && reader.HasMore()) {
                packet.rotation = static_cast<int8_t>(reader.ReadByte());
                const uint8_t state = reader.ReadByte();
                packet.state = static_cast<Game::Aurelith::CityState>(state <= 3 ? state : 0);
                packet.stageStartTick = static_cast<int64_t>(reader.ReadLong());
                packet.awakenedTick = static_cast<int64_t>(reader.ReadLong());
            } else if (packet.kind == AurelithS2CPacket::Kind::Burst && reader.HasMore()) {
                packet.origin.x = reader.ReadDouble();
                packet.origin.y = reader.ReadDouble();
                packet.origin.z = reader.ReadDouble();
                const uint8_t style = reader.ReadByte();
                packet.style = static_cast<AurelithS2CPacket::BurstStyle>(style <= 5 ? style : 0);
                packet.colour = reader.ReadInt();
            }
            return packet;
        }

    } // namespace Serialization

} // namespace Network
