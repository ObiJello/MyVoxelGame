// File: src/server/commands/ItemArgument.hpp
//
// MC net.minecraft.commands.arguments.item.{ItemArgument, ItemParser,
// ItemInput}: `id[component=value,...]` — an item id (namespace optional)
// and, in brackets, data components as SNBT (`diamond_sword[enchantments=
// {sharpness:5},custom_name="Excalibur"]`). The components are applied
// through the stack reader the world save uses (ItemStackNbt ReadItemStack),
// so a command can set exactly what a saved stack can hold; the accepted
// names are common/entity/ItemComponentHints.hpp. `!component` removes the
// item's default for any registered component (`stone_sword[!max_damage]`)
// — a DataComponentPatch removal on the stack. A component may be named once
// (set OR removed). The pre-1.20.5 `{...}` tag is refused: MC 26.3 no longer
// reads it either.
#pragma once

#include "common/entity/Item.hpp"

#include <string>

namespace Server {

    // A stack of one on success (ItemInput.createItemStack(1)).
    bool ParseItemArgument(const std::string& text, Game::ItemStack& out, std::string& error);

} // namespace Server
