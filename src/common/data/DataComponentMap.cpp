// File: src/common/data/DataComponentMap.cpp
#include "DataComponentMap.hpp"

#include <stdexcept>
#include <string>

namespace Game {

    bool DataComponentMap::has(const DataComponentTypeBase& key) const {
        for (const auto& e : entries) {
            if (e.type == &key) return true;
        }
        return false;
    }

    void DataComponentMap::remove(const DataComponentTypeBase& key) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->type == &key) { entries.erase(it); return; }
        }
    }

    // Mirrors DataComponentPatch.STREAM_CODEC.encode (DataComponentPatch.java:57-100).
    void DataComponentMap::Serialize(Network::PacketBuffer& buffer) const {
        uint32_t added = 0;
        for (const auto& e : entries) {
            if (e.type->HasNetworkCodec()) ++added;
        }
        buffer.WriteVarInt(added);
        buffer.WriteVarInt(0);  // removedCount — always 0 (flat map, no removals)
        for (const auto& e : entries) {
            if (!e.type->HasNetworkCodec()) continue;
            buffer.WriteVarInt(e.type->networkId);
            e.type->SerializeErased(buffer, e.value.get());
        }
    }

    // Mirrors DataComponentPatch.STREAM_CODEC.decode (DataComponentPatch.java:33-54).
    DataComponentMap DataComponentMap::Deserialize(Network::PacketReader& reader) {
        DataComponentMap map;
        const uint32_t added   = reader.ReadVarInt();
        const uint32_t removed = reader.ReadVarInt();
        if (removed != 0) {
            throw std::runtime_error(
                "DataComponentMap: non-zero removedCount " + std::to_string(removed) +
                " — our writer never emits removals");
        }
        map.entries.reserve(added);
        for (uint32_t i = 0; i < added; ++i) {
            const uint32_t netId = reader.ReadVarInt();
            const DataComponentTypeBase* type = DataComponents::ById(netId);
            if (!type) {
                throw std::runtime_error(
                    "DataComponentMap: unknown component networkId " + std::to_string(netId));
            }
            auto value = type->DeserializeErased(reader);
            if (!value) {
                throw std::runtime_error(
                    "DataComponentMap: component '" + type->name + "' has no deserializer");
            }
            map.entries.push_back({type, std::move(value)});
        }
        return map;
    }

} // namespace Game
