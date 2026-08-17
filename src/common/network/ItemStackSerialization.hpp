// File: src/common/network/ItemStackSerialization.hpp
//
// Wire codec for a full ItemStack (id + count + component patch). Mirrors MC
// ItemStack.OPTIONAL_STREAM_CODEC (ItemStack.java:139-162):
//
//   encode (ItemStack.java:153-160):
//     empty        → VarInt 0
//     non-empty    → VarInt count, VarInt itemId, component patch
//   decode (ItemStack.java:141-150):
//     count <= 0   → EMPTY
//     else         → count, itemId, patch
//
// The component patch format lives in DataComponentMap::Serialize (mirrors
// DataComponentPatch.STREAM_CODEC). Used by all four inventory packets.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include "common/entity/Item.hpp"
#include <algorithm>

namespace Network::Serialization {

    inline void WriteItemStack(PacketBuffer& buffer, const Game::ItemStack& stack) {
        if (stack.IsEmpty()) {
            buffer.WriteVarInt(0);
            return;
        }
        buffer.WriteVarInt(static_cast<uint32_t>(stack.count)); // ItemStack.java:156
        buffer.WriteVarInt(stack.itemId);                        // ItemStack.java:157
        stack.components.Serialize(buffer);                      // ItemStack.java:158
    }

    inline Game::ItemStack ReadItemStack(PacketReader& reader) {
        const int count = static_cast<int>(reader.ReadVarInt()); // ItemStack.java:142
        if (count <= 0) {
            return Game::ItemStack{};                            // ItemStack.java:143-144
        }
        Game::ItemStack stack;
        stack.itemId     = reader.ReadVarInt();                  // ItemStack.java:146
        stack.components = Game::DataComponentMap::Deserialize(reader); // :147
        // Clamp to the item's own limit. The wire count is an unbounded VarInt,
        // and MC validates the same bound on decode (ItemStack.java:132-135
        // rejects count > getMaxStackSize outright). Clamping rather than
        // rejecting keeps a desynced peer from dropping the whole packet.
        stack.count      = std::min(count, Game::ItemRegistry::Get(stack.itemId).maxStackSize);
        return stack;
    }

} // namespace Network::Serialization
