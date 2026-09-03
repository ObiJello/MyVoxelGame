// File: src/common/network/packets/game/ChangeDimensionS2CPacket.hpp
//
// Server → client: you are somewhere else now. Throw everything away.
//
// Rough analogue of MC's ClientboundRespawnPacket, which is what vanilla
// sends on a dimension change and which drives ClientPacketListener
// .handleRespawn.
//
// WHY THIS IS THE ONLY NEW PACKET DIMENSIONS NEED
// -----------------------------------------------
// Not one existing packet grew a dimension field, and none had to. The client
// is a network client even in single-player, it never reads the server's
// World, and it is only ever in ONE dimension at a time — so "which dimension
// is this chunk for" is a question it never has to ask. What it does need is
// (a) a hard barrier saying "the world you have cached is not the world you
// are in", and (b) a server that never sends it another dimension's data.
// This packet is (a); watcher-scoped broadcasts are (b).
//
// The client MUST, on receipt: clear every cached chunk, every remote player,
// every item entity, XP orb and mob, the block-break overlay and the block
// prediction queue, and then re-enter the level-load wait so the loading
// screen stays up until the new dimension's chunks arrive.
//
// Wire layout:
//   int8   dimensionId   — Game::DimensionId raw value (-1 nether, 0 overworld, 1 end)
//   uint8  flags         — bit 0 hasSkyLight, bit 1 hasCeiling
//   float  ambientLight  — MC DimensionType.ambientLight (nether 0.1, else 0)
//   int32  minY          — lowest generated Y in the new dimension
//   int32  height        — its logical height
//
// The last four are derivable from dimensionId today, and are on the wire
// anyway because the client's fog, sky and (eventually) lighting read them,
// and a client that had to hardcode the table would have to be shipped in
// lockstep with every change to it.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <cstdint>
#include <vector>

namespace Network {

    struct ChangeDimensionS2CPacket {
        int8_t   dimensionId  = 0;
        uint8_t  flags        = 0;
        float    ambientLight = 0.0f;
        int32_t  minY         = -64;
        int32_t  height       = 384;

        static constexpr uint8_t kFlagHasSkyLight = 1u << 0;
        static constexpr uint8_t kFlagHasCeiling  = 1u << 1;
        // The level being left stays resident on the client (a seamless
        // portal crossing, where it is still visible through the portal
        // behind the player). Without it the client frees that level.
        static constexpr uint8_t kFlagKeepPrevious = 1u << 2;

        bool HasSkyLight()  const { return (flags & kFlagHasSkyLight)  != 0; }
        bool HasCeiling()   const { return (flags & kFlagHasCeiling)   != 0; }
        bool KeepPrevious() const { return (flags & kFlagKeepPrevious) != 0; }
    };

    namespace Serialization {

        inline std::vector<uint8_t> Serialize(const ChangeDimensionS2CPacket& p) {
            PacketBuffer b;
            b.WriteByte(static_cast<uint8_t>(p.dimensionId));
            b.WriteByte(p.flags);
            b.WriteFloat(p.ambientLight);
            b.WriteInt(static_cast<uint32_t>(p.minY));
            b.WriteInt(static_cast<uint32_t>(p.height));
            return b.GetData();
        }

        inline ChangeDimensionS2CPacket DeserializeChangeDimensionS2C(
                const std::vector<uint8_t>& data) {
            PacketReader r(data);
            ChangeDimensionS2CPacket p;
            p.dimensionId  = static_cast<int8_t>(r.ReadByte());
            p.flags        = r.ReadByte();
            p.ambientLight = r.ReadFloat();
            p.minY         = static_cast<int32_t>(r.ReadInt());
            p.height       = static_cast<int32_t>(r.ReadInt());
            return p;
        }

    } // namespace Serialization

} // namespace Network
