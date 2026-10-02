// File: src/server/world/storage/anvil/ComponentNbt.hpp
//
// The item-component half of ItemStack <-> NBT as a registry: each component
// that is not written inline by ItemStackNbt.cpp registers its codec (MC's
// DataComponentType.codec(), over NbtOps) from its own .cpp with a static
// Registrar. WriteItemStackBody writes every registered component the stack's
// patch holds (`components: {"minecraft:<name>": …}`); ReadItemStack hands
// every registered key it finds to the codec's reader — the save/load path
// and the /give item argument (ItemArgument parses SNBT into the same tree)
// alike.
//
// Readers must be lenient in the vanilla way: a value the codec rejects
// drops that component (return false, with a reason in ctx.error for the
// command to print) rather than the whole stack.
#pragma once

#include "common/entity/Item.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game { class DataComponentTypeBase; }

namespace Game::Anvil::ComponentNbt {

    struct ReadContext {
        // Set by a reader that rejects its value: the reason, as MC's codec
        // error reads ("Malformed 'minecraft:<name>' component: …" is added
        // by the item argument).
        std::string* error = nullptr;
        void Fail(const std::string& why) const { if (error && error->empty()) *error = why; }
    };

    // Writes the value the stack's own patch holds (stack.components) under
    // `key` ("minecraft:<name>") into the open `components` compound. Only
    // called when stack.components.has(type).
    using WriteFn = void (*)(Nbt::Writer& w, std::string_view key, const ItemStack& stack);
    // Reads `tag` (the value under the component's key) onto `stack`. False
    // when the value is malformed (nothing set).
    using ReadFn  = bool (*)(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx);

    struct Codec {
        const DataComponentTypeBase* type = nullptr;
        WriteFn write = nullptr;
        ReadFn  read  = nullptr;
    };

    void Register(const Codec& codec);
    const std::vector<Codec>& All();
    const Codec* Find(const DataComponentTypeBase* type);

    struct Registrar {
        Registrar(const DataComponentTypeBase& type, WriteFn write, ReadFn read) {
            Register(Codec{&type, write, read});
        }
    };

    // ── Helpers shared by the codecs ─────────────────────────────────────

    // Any numeric tag, as NbtOps.getNumberValue reads it.
    std::optional<double> NumberOf(const ::World::NBTTag& tag);
    // Codec.BOOL over NbtOps: a numeric tag (0 = false); SNBT's true/false
    // arrive as bytes. A "true"/"false" string is accepted too.
    std::optional<bool> BoolOf(const ::World::NBTTag& tag);
    std::optional<std::string> StringOf(const ::World::NBTTag& tag);
    // A heterogeneous list's {"": value} wrapper, unwrapped (ListTag.tryUnwrap).
    const ::World::NBTTag* Unwrap(const ::World::NBTTag* tag);
    // Typed views (nullptr when the tag is of another type).
    const ::World::NBTTagCompound* AsCompound(const ::World::NBTTag* tag);
    const ::World::NBTTagList*     AsList(const ::World::NBTTag* tag);

    // "minecraft:" + id unless it already has a namespace; and the reverse
    // (only "minecraft:" is stripped).
    std::string      WithNamespace(std::string_view id);
    std::string_view StripMinecraft(std::string_view id);

    // A HolderSet<T> (RegistryCodecs.homogeneousList): a "#tag" or id
    // string, or a list of ids. Entries are returned as written.
    std::vector<std::string> ReadHolderSet(const ::World::NBTTag* tag);
    void WriteHolderSet(Nbt::Writer& w, std::string_view name, const std::vector<std::string>& entries);

    // An int list in any form: IntArray, a list of numbers.
    std::vector<int32_t> ReadIntList(const ::World::NBTTag* tag);
    // A float list (list of numeric tags).
    std::vector<float> ReadFloatList(const ::World::NBTTag* tag);
    // A string list (list of strings, or one bare string).
    std::vector<std::string> ReadStringList(const ::World::NBTTag* tag);

    // ExtraCodecs.RGB_COLOR_CODEC / ARGB: an int, or a list of 3 (4) floats
    // 0..1 — as an 0xRRGGBB / 0xAARRGGBB int.
    std::optional<int32_t> ReadRgb(const ::World::NBTTag& tag);
    std::optional<int32_t> ReadArgb(const ::World::NBTTag& tag);

    // DyeColor.CODEC: the colour's name ("light_blue") <-> its id 0..15.
    std::optional<int> DyeFromName(std::string_view name);
    const char*        DyeName(int id);

    // The NBT tree as JSON (numbers stay numbers, a byte stays a number the
    // way NbtOps hands one to Codec.BOOL).
    nlohmann::json NbtToJson(const ::World::NBTTag& tag);

    // MC ItemPredicate.test over its NBT form (items, count, components,
    // predicates — ItemPredicateNbt.cpp). An empty compound matches anything.
    bool ItemPredicateMatches(const ::World::NBTTagCompound& predicate, const ItemStack& stack);

} // namespace Game::Anvil::ComponentNbt
