// File: src/common/network/packets/game/WorldgenIdsS2CPacket.hpp
//
// Server → client: the worldgen ids of the dimension the player is in — the
// biomes its biome source can produce, the structures its structure sets can
// place (and that this build generates), and the biome / structure tags with
// at least one member there. Sent on join and on every dimension change.
//
// It is what /locate's tab-completion offers: in the Hush, `/locate biome `
// lists the seven Hush biomes and nothing from the Overworld, the Twilight
// Forest or the Aether. MC's client completes from its full synced registries
// instead; the per-dimension list is this engine's choice (the user asked for
// it), and the client cannot work it out itself — it needs the dimension's
// generator, which only the server holds.
//
// Full namespaced ids on the wire ("minecraft:hush_meadows",
// "#twilightforest:in_twilight_forest"); the chat screen shows the bare path.
#pragma once

#include "common/network/PacketRegistry.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Network {

    struct WorldgenIdsS2CPacket {
        int8_t                   dimension = 0;   // raw DimensionId the lists describe
        std::vector<std::string> biomes;
        std::vector<std::string> structures;
        std::vector<std::string> biomeTags;       // "#ns:path"
        std::vector<std::string> structureTags;   // "#ns:path"
    };

    namespace Serialization {

        namespace WorldgenIdsDetail {
            inline void WriteList(Network::PacketBuffer& buffer, const std::vector<std::string>& list) {
                buffer.WriteVarInt(static_cast<uint32_t>(list.size()));
                for (const auto& id : list) buffer.WriteString(id);
            }
            inline std::vector<std::string> ReadList(Network::PacketReader& reader, size_t payloadSize) {
                // Every id costs at least one byte: a corrupt count cannot make
                // us reserve more than the payload could hold.
                const uint32_t count = std::min<uint32_t>(reader.ReadVarInt(),
                                                          static_cast<uint32_t>(payloadSize));
                std::vector<std::string> list;
                list.reserve(count);
                for (uint32_t i = 0; i < count; ++i) list.push_back(reader.ReadString());
                return list;
            }
        }

        inline std::vector<uint8_t> Serialize(const WorldgenIdsS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.dimension));
            WorldgenIdsDetail::WriteList(buffer, packet.biomes);
            WorldgenIdsDetail::WriteList(buffer, packet.structures);
            WorldgenIdsDetail::WriteList(buffer, packet.biomeTags);
            WorldgenIdsDetail::WriteList(buffer, packet.structureTags);
            return buffer.GetData();
        }

        inline WorldgenIdsS2CPacket DeserializeWorldgenIdsS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            WorldgenIdsS2CPacket packet;
            packet.dimension     = static_cast<int8_t>(reader.ReadByte());
            packet.biomes        = WorldgenIdsDetail::ReadList(reader, data.size());
            packet.structures    = WorldgenIdsDetail::ReadList(reader, data.size());
            packet.biomeTags     = WorldgenIdsDetail::ReadList(reader, data.size());
            packet.structureTags = WorldgenIdsDetail::ReadList(reader, data.size());
            return packet;
        }

    } // namespace Serialization

} // namespace Network
