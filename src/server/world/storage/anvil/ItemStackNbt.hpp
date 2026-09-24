// File: src/server/world/storage/anvil/ItemStackNbt.hpp
//
// ItemStack <-> vanilla NBT, in the 1.20.5+ component form:
//
//   { id: "minecraft:diamond_sword", count: 1, components?: { ... } }
//
// Used by container block entities now and by playerdata later, so it lives on
// its own rather than inside BlockEntityNbt.
//
// WHICH COMPONENTS. Only the ones the engine ever writes onto a stack, found
// by grepping `components.set(` outside `defaultComponents`:
//   custom_name, stored_enchantments, potion_contents, potion_duration_scale,
//   suspicious_stew_effects, written_book_content, writable_book_content
//   (plus this engine's own obeycraft: ones).
// (bundle_contents is set on stacks too and is still not saved.)
// Everything else exists purely as Item::defaultComponents, which ItemStack::get
// falls back to and which is never stored on the stack — and several of our
// component structs are shape-simplified against vanilla's, so emitting them
// would produce invalid NBT rather than a faithful copy.
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/text/TextComponent.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <optional>
#include <string>
#include <string_view>

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

    // PotionContents.CODEC in its FULL (compound) form, as vanilla encodes
    // it: { potion?: "minecraft:<id>", custom_color?: int,
    //       custom_effects?: [MobEffectInstance...], custom_name?: string }.
    // Written as the body of an already-opened compound. The item component
    // (`minecraft:potion_contents`) and the area-effect cloud's
    // `potion_contents` both use it.
    void WritePotionContentsBody(Nbt::Writer& w, const PotionContents& contents);
    // Accepts both codec alternatives: the compound, or a bare potion-id
    // string (Codec.withAlternative(FULL_CODEC, Potion.CODEC)). Unknown
    // potion / effect ids are dropped, as vanilla drops unknown registry
    // entries.
    PotionContents ReadPotionContents(const ::World::NBTTag& tag);

    // A text component in MC 26.3's NBT form (ComponentSerialization.CODEC
    // over NbtOps): a string tag for plain text, a compound for the full form
    // ({text|translate|…, color, bold:1b, click_event:{…}, extra:[…]}), a
    // list for "first + siblings". A heterogeneous list's {"": value}
    // wrappers are unwrapped. nullopt for a tag that is not a component.
    std::optional<Text::Component> ReadTextComponent(const ::World::NBTTag& tag);
    // Writes `component` under `name` in the open compound: a string when it
    // collapses (Component.tryCollapseToString), else the compound form.
    void WriteTextComponent(Nbt::Writer& w, std::string_view name, const Text::Component& component);

} // namespace Game::Anvil
