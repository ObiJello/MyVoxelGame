// File: src/server/world/storage/anvil/components/StackNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/StackComponents.hpp — custom_data,
// max_stack_size — and the durability trio max_damage / damage / unbreakable
// (DataComponents.hpp), registered with ComponentNbt. creative_slot_lock has
// no codec: MC registers it network-only (not persistent).
//
// Registration order matters within this TU only for reading: MAX_DAMAGE is
// read before DAMAGE (static objects initialise in declaration order).
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "common/data/components/StackComponents.hpp"
#include "server/commands/SnbtParser.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"

#include <cmath>
#include <string>

namespace Game::Anvil::ComponentNbt {

    namespace {

        // Codec.INT over NbtOps: any numeric tag, narrowed as NbtOps'
        // Number.intValue does; anything else is "Not a number".
        std::optional<int32_t> ReadInt(const ::World::NBTTag& tag, const ReadContext& ctx) {
            const auto n = NumberOf(*Unwrap(&tag));
            if (!n) {
                ctx.Fail("Not a number: " + NbtTagToSnbt(tag));
                return std::nullopt;
            }
            const double v = *n;
            if (std::isnan(v)) return 0;
            if (v >= 2147483647.0) return INT32_MAX;
            if (v <= -2147483648.0) return INT32_MIN;
            return static_cast<int32_t>(v);
        }

        // ── custom_data ─────────────────────────────────────────────────
        // CustomData.CODEC = withAlternative(CompoundTag.CODEC,
        // TagParser.FLATTENED_CODEC): a compound, or an SNBT string that
        // parses to one.
        void WriteCustomData(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::CUSTOM_DATA);
            if (!v) return;
            WriteNbtTag(w, key, v->Tag());
        }

        bool ReadCustomData(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            // (Never unwrapped: `{"": …}` is a legitimate custom compound.)
            if (const auto* compound = AsCompound(&tag)) {
                auto copy = std::dynamic_pointer_cast<::World::NBTTagCompound>(CloneNbtTag(*compound));
                stack.components.set(DataComponents::CUSTOM_DATA, NbtCompoundValue(std::move(copy)));
                return true;
            }
            if (auto text = StringOf(tag)) {
                std::string parseError;
                ::World::NBTTagPtr parsed = Server::Snbt::ParseValue(*text, parseError);
                if (!parsed) {
                    ctx.Fail(parseError);
                    return false;
                }
                auto compound = std::dynamic_pointer_cast<::World::NBTTagCompound>(parsed);
                if (!compound) {
                    ctx.Fail("Expected compound tag, got " + NbtTagToSnbt(*parsed));
                    return false;
                }
                stack.components.set(DataComponents::CUSTOM_DATA, NbtCompoundValue(std::move(compound)));
                return true;
            }
            ctx.Fail("Not a compound tag: " + NbtTagToSnbt(tag));
            return false;
        }

        // ── max_stack_size: ExtraCodecs.intRange(1, 99) ─────────────────
        void WriteMaxStackSize(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::MAX_STACK_SIZE)) w.Int(key, *v);
        }

        bool ReadMaxStackSize(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto v = ReadInt(tag, ctx);
            if (!v) return false;
            if (*v < 1 || *v > DataComponents::kMaxStackSizeLimit) {
                ctx.Fail("Value must be within range [1;" + std::to_string(DataComponents::kMaxStackSizeLimit) +
                         "]: " + std::to_string(*v));
                return false;
            }
            // A value equal to the item's own is no patch (PatchedDataComponent
            // Map.set drops it), so the stack still matches a plain one.
            const Item& item = ItemRegistry::Get(stack.itemId);
            const auto prototype = item.defaultComponents.get(DataComponents::MAX_STACK_SIZE);
            if ((prototype ? *prototype : item.maxStackSize) == *v &&
                !stack.components.isRemoved(DataComponents::MAX_STACK_SIZE)) {
                return true;
            }
            stack.components.set(DataComponents::MAX_STACK_SIZE, *v);
            return true;
        }

        // ── max_damage: ExtraCodecs.POSITIVE_INT ────────────────────────
        void WriteMaxDamage(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::MAX_DAMAGE)) w.Int(key, *v);
        }

        bool ReadMaxDamage(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto v = ReadInt(tag, ctx);
            if (!v) return false;
            if (*v < 1) {
                ctx.Fail("Value must be positive: " + std::to_string(*v));
                return false;
            }
            const auto prototype = ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::MAX_DAMAGE);
            if (prototype && *prototype == *v && !stack.components.isRemoved(DataComponents::MAX_DAMAGE)) return true;
            stack.components.set(DataComponents::MAX_DAMAGE, *v);
            return true;
        }

        // ── damage: ExtraCodecs.NON_NEGATIVE_INT ────────────────────────
        void WriteDamage(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::DAMAGE)) w.Int(key, *v < 0 ? 0 : *v);
        }

        bool ReadDamage(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto v = ReadInt(tag, ctx);
            if (!v) return false;
            if (*v < 0) {
                ctx.Fail("Value must be non-negative: " + std::to_string(*v));
                return false;
            }
            // A value equal to the item's own default is no patch (MC never
            // writes one); keeping it would stop the stack matching a fresh one.
            const auto prototype = ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::DAMAGE);
            if (prototype && *prototype == *v && !stack.components.isRemoved(DataComponents::DAMAGE)) return true;
            stack.components.set(DataComponents::DAMAGE, *v);
            return true;
        }

        // ── unbreakable: Unit.CODEC (written as {}) ─────────────────────
        void WriteUnbreakable(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (!stack.components.has(DataComponents::UNBREAKABLE)) return;
            w.BeginCompound(key);
            w.EndCompound();
        }

        bool ReadUnbreakable(const ::World::NBTTag&, ItemStack& stack, const ReadContext&) {
            stack.components.set(DataComponents::UNBREAKABLE, true);
            return true;
        }

        // ── enchantment_glint_override: Codec.BOOL ──────────────────────
        void WriteGlintOverride(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto v = stack.components.get(DataComponents::ENCHANTMENT_GLINT_OVERRIDE)) w.Bool(key, *v);
        }

        bool ReadGlintOverride(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto v = BoolOf(tag);
            if (!v) { ctx.Fail("Not a boolean: " + NbtTagToSnbt(tag)); return false; }
            stack.components.set(DataComponents::ENCHANTMENT_GLINT_OVERRIDE, *v);
            return true;
        }

        // ── bundle_contents: ItemStack.CODEC.listOf() ────────────────────
        void WriteBundleContents(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto v = stack.components.get(DataComponents::BUNDLE_CONTENTS);
            if (!v) return;
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const ItemStack& item : v->items) {
                if (item.IsEmpty()) continue;
                w.ListCompoundBegin(list);
                WriteItemStackBody(w, item);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool ReadBundleContents(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* list = AsList(&tag);
            if (!list) { ctx.Fail("Not a list: " + NbtTagToSnbt(tag)); return false; }
            BundleContents v;
            for (const auto& element : list->value) {
                const auto* c = AsCompound(Unwrap(element.get()));
                if (!c) { ctx.Fail("Not a map: " + (element ? NbtTagToSnbt(*element) : std::string("null"))); return false; }
                std::string error;
                ItemStack item = ReadItemStack(*c, &error);
                if (!error.empty()) { ctx.Fail(error); return false; }
                if (!item.IsEmpty()) v.items.push_back(std::move(item));
            }
            stack.components.set(DataComponents::BUNDLE_CONTENTS, std::move(v));
            return true;
        }

        const Registrar kGlintOverride{DataComponents::ENCHANTMENT_GLINT_OVERRIDE, &WriteGlintOverride, &ReadGlintOverride};
        const Registrar kBundleContents{DataComponents::BUNDLE_CONTENTS, &WriteBundleContents, &ReadBundleContents};
        const Registrar kCustomData  {DataComponents::CUSTOM_DATA,    &WriteCustomData,   &ReadCustomData};
        const Registrar kMaxStackSize{DataComponents::MAX_STACK_SIZE, &WriteMaxStackSize, &ReadMaxStackSize};
        const Registrar kMaxDamage   {DataComponents::MAX_DAMAGE,     &WriteMaxDamage,    &ReadMaxDamage};
        const Registrar kDamage      {DataComponents::DAMAGE,         &WriteDamage,       &ReadDamage};
        const Registrar kUnbreakable {DataComponents::UNBREAKABLE,    &WriteUnbreakable,  &ReadUnbreakable};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
