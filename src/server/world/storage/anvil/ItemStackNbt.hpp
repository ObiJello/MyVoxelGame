// File: src/server/world/storage/anvil/ItemStackNbt.hpp
//
// ItemStack <-> vanilla NBT, in the 1.20.5+ component form:
//
//   { id: "minecraft:diamond_sword", count: 1, components?: { ... } }
//
// Used by container block entities now and by playerdata later, so it lives on
// its own rather than inside BlockEntityNbt.
//
// WHICH COMPONENTS. Only the three the engine ever writes onto a stack, found
// by grepping `components.set(` outside `defaultComponents`:
//   custom_name, stored_enchantments, bundle_contents.
// Everything else exists purely as Item::defaultComponents, which ItemStack::get
// falls back to and which is never stored on the stack — and several of our
// component structs are shape-simplified against vanilla's, so emitting them
// would produce invalid NBT rather than a faithful copy.
#pragma once

#include "common/entity/Item.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <string>

namespace Game::Anvil {

    // ItemID -> "minecraft:<slug>". Empty for an id with no vanilla name.
    std::string ItemName(ItemID id);
    // The inverse. Returns Items::Air for an unknown name, which is how
    // vanilla treats an item its registry does not have.
    ItemID ItemFromName(std::string_view name);

    // Writes the stack as a bare compound body — the caller has already opened
    // it (as a list element or a named compound). `slot` >= 0 adds the `Slot`
    // byte a container's Items list needs.
    void WriteItemStackBody(Nbt::Writer& w, const ItemStack& stack, int slot = -1);

    // Reads one. Returns an empty stack for anything unrecognised rather than
    // failing, matching vanilla's tolerance for items it does not know.
    ItemStack ReadItemStack(const ::World::NBTTagCompound& tag);

} // namespace Game::Anvil
