// File: src/common/data/DataComponentMap.cpp
#include "DataComponentMap.hpp"

#include <stdexcept>
#include <string>

namespace Game {

    bool DataComponentMap::has(const DataComponentTypeBase& key) const {
        for (const auto& e : entries) {
            if (e.type == &key) return e.value != nullptr;
        }
        return false;
    }

    void DataComponentMap::setRemoved(const DataComponentTypeBase& key) {
        for (auto& e : entries) {
            if (e.type == &key) { e.value.reset(); return; }
        }
        entries.push_back({&key, nullptr});
    }

    bool DataComponentMap::isRemoved(const DataComponentTypeBase& key) const {
        for (const auto& e : entries) {
            if (e.type == &key) return e.value == nullptr;
        }
        return false;
    }

    std::vector<const DataComponentTypeBase*> DataComponentMap::removedTypes() const {
        std::vector<const DataComponentTypeBase*> out;
        for (const auto& e : entries) {
            if (!e.value) out.push_back(e.type);
        }
        return out;
    }

    bool DataComponentMap::CopyNamed(const DataComponentMap& src, std::string_view name) {
        for (const auto& e : src.entries) {
            if (!e.type || e.type->name != name) continue;
            for (auto& mine : entries) {
                if (mine.type == e.type) {
                    mine.value = e.value;
                    return true;
                }
            }
            entries.push_back(e);
            return true;
        }
        return false;
    }

    void DataComponentMap::remove(const DataComponentTypeBase& key) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->type == &key) { entries.erase(it); return; }
        }
    }

    bool DataComponentMap::IsExactSubsetOf(const DataComponentMap& target,
                                           const DataComponentMap* targetDefaults) const {
        if (entries.empty()) return true;
        Network::PacketBuffer mine, theirs;
        for (const auto& e : entries) {
            const Entry* match = nullptr;
            for (const auto& o : target.entries) {
                if (o.type == e.type) { match = &o; break; }
            }
            if (!e.value) {
                // A removal in the predicate: the target must not have it.
                if (match) { if (match->value) return false; continue; }
                if (targetDefaults && targetDefaults->has(*e.type)) return false;
                continue;
            }
            if (match && !match->value) return false;   // the target removed it
            if (!match && targetDefaults) {
                for (const auto& o : targetDefaults->entries) {
                    if (o.type == e.type) { match = &o; break; }
                }
            }
            if (!match) return false;
            if (e.value.get() == match->value.get()) continue;
            // Same rule as Equals: without a codec there is no way to compare
            // values, and refusing is the safe answer.
            if (!e.type->HasNetworkCodec()) return false;
            mine.Clear();
            theirs.Clear();
            e.type->SerializeErased(mine, e.value.get());
            match->type->SerializeErased(theirs, match->value.get());
            if (mine.GetData() != theirs.GetData()) return false;
        }
        return true;
    }

    bool DataComponentMap::Equals(const DataComponentMap& other) const {
        // Fast path: the overwhelming majority of stacks are vanilla and carry
        // no per-stack overrides at all. Keeping this allocation-free matters —
        // the callers are inner loops (stack merging, and the 46-slot diff that
        // runs once per player per tick in BroadcastContainerChanges).
        if (entries.empty() && other.entries.empty()) return true;
        if (entries.size() != other.entries.size())   return false;

        // n is 0-3 in practice, so the nested scan is cheaper than building an
        // index. Buffers are hoisted out of the loop and reused.
        Network::PacketBuffer mine, theirs;
        for (const auto& e : entries) {
            const Entry* match = nullptr;
            for (const auto& o : other.entries) {
                if (o.type == e.type) { match = &o; break; }
            }
            if (!match) return false;

            // The same allocation is the same value: set() always installs a
            // fresh value and nothing mutates one in place, so a stack and
            // its copy (the per-tick diff's remote model) compare without
            // serializing — which matters once a component is a 100-page book.
            // (Two removals are both null.)
            if (e.value.get() == match->value.get()) continue;
            if (!e.value || !match->value) return false;   // removed vs set

            if (!e.type->HasNetworkCodec()) {
                // No codec means no way to inspect the value. Fall back to
                // identity: distinct allocations compare unequal. That is the
                // SAFE direction to be wrong in — it refuses to merge two
                // stacks that may in fact differ, rather than merging them and
                // destroying one side's data.
                if (e.value.get() != match->value.get()) return false;
                continue;
            }

            mine.Clear();
            theirs.Clear();
            e.type->SerializeErased(mine, e.value.get());
            match->type->SerializeErased(theirs, match->value.get());
            if (mine.GetData() != theirs.GetData()) return false;
        }
        return true;
    }

    // Mirrors DataComponentPatch.STREAM_CODEC.encode (DataComponentPatch.java:57-100).
    void DataComponentMap::Serialize(Network::PacketBuffer& buffer) const {
        uint32_t added = 0, removed = 0;
        for (const auto& e : entries) {
            if (!e.type->HasNetworkCodec()) continue;
            if (e.value) ++added; else ++removed;
        }
        buffer.WriteVarInt(added);
        buffer.WriteVarInt(removed);
        for (const auto& e : entries) {
            if (!e.type->HasNetworkCodec() || !e.value) continue;
            buffer.WriteVarInt(e.type->networkId);
            e.type->SerializeErased(buffer, e.value.get());
        }
        for (const auto& e : entries) {
            if (!e.type->HasNetworkCodec() || e.value) continue;
            buffer.WriteVarInt(e.type->networkId);
        }
    }

    // Mirrors DataComponentPatch.STREAM_CODEC.decode (DataComponentPatch.java:33-54).
    DataComponentMap DataComponentMap::Deserialize(Network::PacketReader& reader) {
        DataComponentMap map;
        const uint32_t added   = reader.ReadVarInt();
        const uint32_t removed = reader.ReadVarInt();
        if (added > 4096 || removed > 4096) {
            throw std::runtime_error("DataComponentMap: absurd component counts");
        }
        map.entries.reserve(added + removed);
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
        for (uint32_t i = 0; i < removed; ++i) {
            const uint32_t netId = reader.ReadVarInt();
            const DataComponentTypeBase* type = DataComponents::ById(netId);
            if (!type) {
                throw std::runtime_error(
                    "DataComponentMap: unknown removed component networkId " + std::to_string(netId));
            }
            map.setRemoved(*type);
        }
        return map;
    }

} // namespace Game
