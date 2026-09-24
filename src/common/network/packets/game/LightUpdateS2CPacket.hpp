#pragma once
// LightUpdateS2C (0x6A) — MC ClientboundLightUpdatePacket.
//
// The light sections of one chunk that the server's light engine changed
// (or whose meshes read a changed cell), sent to the chunk's watchers once
// per tick, BEFORE that tick's block changes (MC ChunkHolder.broadcastChanges
// sends light first), so a client applies both and remeshes once.
//
//   Int chunkX, Int chunkZ, then the light block (Lighting::NetCodec: masks +
//   homogeneous-or-2048-byte layers). Only the masked sections are carried;
//   the client replaces exactly those layers and re-meshes exactly those
//   sections.
//
// Decoded on the I/O thread: the layers arrive ready to adopt.
#include "common/network/PacketRegistry.hpp"
#include "common/world/lighting/LightNetCodec.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Network {

    struct LightUpdateS2CPacket {
        int32_t chunkX = 0;
        int32_t chunkZ = 0;
        std::shared_ptr<Game::Lighting::NetCodec::Decoded> light;   // null if the block failed to decode
    };

    namespace Serialization {

        inline std::vector<uint8_t> SerializeLightUpdate(int32_t chunkX, int32_t chunkZ,
                                                         const Game::Lighting::ChunkLight& light,
                                                         uint32_t sectionMask, bool hasSkyLight) {
            Network::PacketBuffer b(16 + Game::Lighting::NetCodec::EncodedSize(light, sectionMask, hasSkyLight));
            b.WriteInt(static_cast<uint32_t>(chunkX));
            b.WriteInt(static_cast<uint32_t>(chunkZ));
            Game::Lighting::NetCodec::Write(b, light, sectionMask, hasSkyLight);
            return b.GetData();
        }

        inline LightUpdateS2CPacket DeserializeLightUpdateS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader r(data);
            LightUpdateS2CPacket p;
            p.chunkX = static_cast<int32_t>(r.ReadInt());
            p.chunkZ = static_cast<int32_t>(r.ReadInt());
            auto decoded = std::make_shared<Game::Lighting::NetCodec::Decoded>();
            if (Game::Lighting::NetCodec::Read(r, *decoded)) p.light = std::move(decoded);
            return p;
        }

    } // namespace Serialization

} // namespace Network
