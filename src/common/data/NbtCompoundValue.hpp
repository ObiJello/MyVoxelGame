// File: src/common/data/NbtCompoundValue.hpp
//
// An NBT compound carried whole by an item component — MC's CustomData
// (custom_data, bucket_entity_data) and the tag half of TypedEntityData
// (entity_data, block_entity_data). The component keeps the tree exactly as
// it was read (an SNBT literal from /give, a saved stack's compound), so a
// vanilla save round-trips every key the engine does not interpret itself.
//
// Values are immutable once built (components are never mutated in place —
// DataComponentMap shares them between copies), hence the const tree.
//
// The canonical binary form (Encode) writes compound keys in sorted order:
// NBTTagCompound is an unordered_map, and DataComponentMap::Equals compares
// components by their wire bytes, so two equal trees must encode alike.
#pragma once

#include "../network/PacketRegistry.hpp"
#include "../nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    struct NbtCompoundValue {
        std::shared_ptr<const ::World::NBTTagCompound> tag;

        NbtCompoundValue();
        explicit NbtCompoundValue(std::shared_ptr<const ::World::NBTTagCompound> t);

        // MC CustomData.EMPTY / isEmpty.
        bool IsEmpty() const { return !tag || tag->value.empty(); }
        const ::World::NBTTagCompound& Tag() const;

        // A deep, mutable copy (CustomData.copyTag) for code that edits and
        // rebuilds the value.
        std::shared_ptr<::World::NBTTagCompound> Copy() const;

        // Canonical big-endian NBT of the compound (unnamed root, sorted
        // keys) and back. Decode of malformed bytes yields an empty value.
        std::vector<uint8_t> Encode() const;
        static NbtCompoundValue Decode(const std::vector<uint8_t>& bytes);

        // MC's SNBT form of the tree (NbtUtils.toPrettyString's compact
        // cousin, TagParser-readable): {a:1b,b:"x",c:[I;1,2]} — what an
        // advanced tooltip or a command echo prints.
        std::string ToSnbt() const;

        bool operator==(const NbtCompoundValue& o) const { return Encode() == o.Encode(); }
    };

    // Writes any NBT tag (compound keys sorted) under `name` in the open
    // compound — the generic tree emitter every raw-NBT component uses.
    void WriteNbtTag(Nbt::Writer& w, std::string_view name, const ::World::NBTTag& tag);
    // Writes a compound's entries (keys sorted) into an already open compound.
    void WriteNbtCompoundEntries(Nbt::Writer& w, const ::World::NBTTagCompound& compound);
    // Deep copy of any tag.
    ::World::NBTTagPtr CloneNbtTag(const ::World::NBTTag& tag);
    // SNBT of any tag (see NbtCompoundValue::ToSnbt).
    std::string NbtTagToSnbt(const ::World::NBTTag& tag);

    // Wire codec (MC CustomData.STREAM_CODEC = ByteBufCodecs.COMPOUND_TAG):
    // VarInt length + the canonical bytes.
    void SerNbtCompoundValue(Network::PacketBuffer& b, const NbtCompoundValue& v);
    NbtCompoundValue DeNbtCompoundValue(Network::PacketReader& r);

} // namespace Game
