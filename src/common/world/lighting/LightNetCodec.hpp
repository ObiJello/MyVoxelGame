// File: src/common/world/lighting/LightNetCodec.hpp
//
// The light block on the wire — MC ClientboundLightUpdatePacketData, shared by
// ChunkDataS2C (the full column, appended after the sections) and
// LightUpdateS2C (just the sections a light run touched):
//
//   u8   version (2)
//   u32  skyMask      bit i: a sky layer for light section i follows
//   u32  blockMask    bit i: a block layer for light section i follows
//   per set bit of skyMask, ascending, then of blockMask:
//        u8 tag       0..15  a homogeneous layer of that value (no bytes)
//                     16     2048 bytes of nibbles follow (DataLayer layout)
//   then per set bit of blockMask, ascending, the coloured channels (engine,
//   ChunkLight's header note) red, green, blue:
//        u8 tag       0..16  as above
//                     17     the same as this section's block layer — every
//                            section no coloured light reaches, one byte
// Version 1 (no channels) reads as white light everywhere.
//
// MC sends every non-empty layer as 2048 bytes and zlib squeezes the sky's
// solid 15s back out; the homogeneous tag keeps that off the wire and out of
// the compressor. Sections not in a mask are "unchanged" in an update and
// "absent" in a chunk packet (the client then reads sky 15 / block 0 there).
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/world/lighting/ChunkLight.hpp"

#include <array>
#include <cstdint>

namespace Game::Lighting::NetCodec {

    inline constexpr uint8_t kVersion   = 2;
    inline constexpr uint8_t kTagArray  = 16;
    inline constexpr uint8_t kTagSameAsBlock = 17;
    inline constexpr uint32_t kAllSections = (1u << kLightSectionCount) - 1u;

    struct Decoded {
        uint32_t skyMask = 0;
        uint32_t blockMask = 0;
        std::array<DataLayer, kLightSectionCount> sky{};
        std::array<DataLayer, kLightSectionCount> block{};
        // Per blockMask bit: red, green, blue; block light's own storage
        // where the channel is white.
        std::array<std::array<DataLayer, kLightSectionCount>, 3> channel{};
    };

    inline void WriteLayer(Network::PacketBuffer& out, const DataLayer& layer) {
        if (layer.IsDefinitelyHomogeneous()) {
            out.WriteByte(static_cast<uint8_t>(layer.DefaultValue()));
            return;
        }
        out.WriteByte(kTagArray);
        out.WriteBytes(layer.RawData(), DataLayer::kSize);
    }

    // `sectionMask`: the light sections to send. Sky layers only when the
    // dimension has sky light.
    inline void Write(Network::PacketBuffer& out, const ChunkLight& light,
                      uint32_t sectionMask, bool hasSkyLight) {
        const uint32_t skyMask = hasSkyLight ? sectionMask : 0u;
        out.WriteByte(kVersion);
        out.WriteInt(skyMask);
        out.WriteInt(sectionMask);
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (skyMask & (1u << i)) WriteLayer(out, light.sky[static_cast<size_t>(i)]);
        }
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (sectionMask & (1u << i)) WriteLayer(out, light.block[static_cast<size_t>(i)]);
        }
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (!(sectionMask & (1u << i))) continue;
            const DataLayer& block = light.block[static_cast<size_t>(i)];
            for (const auto& c : light.channel) {
                const DataLayer& layer = c[static_cast<size_t>(i)];
                if (layer.SharesWith(block)) out.WriteByte(kTagSameAsBlock);
                else WriteLayer(out, layer);
            }
        }
    }

    inline DataLayer ReadLayer(Network::PacketReader& in) {
        const uint8_t tag = in.ReadByte();
        if (tag == kTagArray) {
            return DataLayer::FromBytes(in.ReadSpan(DataLayer::kSize));
        }
        return DataLayer(tag & 15);
    }

    // Throws (PacketReader) on a truncated block; false on an unknown version.
    inline bool Read(Network::PacketReader& in, Decoded& out) {
        const uint8_t version = in.ReadByte();
        if (version != 1 && version != kVersion) return false;
        out.skyMask = in.ReadInt() & kAllSections;
        out.blockMask = in.ReadInt() & kAllSections;
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (out.skyMask & (1u << i)) out.sky[static_cast<size_t>(i)] = ReadLayer(in);
        }
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (out.blockMask & (1u << i)) out.block[static_cast<size_t>(i)] = ReadLayer(in);
        }
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (!(out.blockMask & (1u << i))) continue;
            const DataLayer& block = out.block[static_cast<size_t>(i)];
            for (auto& c : out.channel) {
                if (version == 1) { c[static_cast<size_t>(i)] = block; continue; }
                const uint8_t tag = in.ReadByte();
                if (tag == kTagSameAsBlock) c[static_cast<size_t>(i)] = block;
                else if (tag == kTagArray)  c[static_cast<size_t>(i)] = DataLayer::FromBytes(in.ReadSpan(DataLayer::kSize));
                else                        c[static_cast<size_t>(i)] = DataLayer(tag & 15);
            }
        }
        return true;
    }

    // Bytes Write() will produce (packet size estimates).
    inline size_t EncodedSize(const ChunkLight& light, uint32_t sectionMask, bool hasSkyLight) {
        size_t n = 9;
        for (int i = 0; i < kLightSectionCount; ++i) {
            if (!(sectionMask & (1u << i))) continue;
            if (hasSkyLight) n += 1 + (light.sky[static_cast<size_t>(i)].IsDefinitelyHomogeneous() ? 0 : DataLayer::kSize);
            n += 1 + (light.block[static_cast<size_t>(i)].IsDefinitelyHomogeneous() ? 0 : DataLayer::kSize);
            for (const auto& c : light.channel) {
                const DataLayer& layer = c[static_cast<size_t>(i)];
                n += 1 + ((layer.SharesWith(light.block[static_cast<size_t>(i)]) || layer.IsDefinitelyHomogeneous())
                              ? 0 : DataLayer::kSize);
            }
        }
        return n;
    }

} // namespace Game::Lighting::NetCodec
