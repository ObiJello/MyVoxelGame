// File: src/server/world/storage/anvil/components/PresentationNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/PresentationComponents.hpp, registered with
// ComponentNbt: custom_name and item_name (text components — the styled form
// when the stack has one, CUSTOM_NAME_STYLE / ITEM_NAME_STYLE never get a
// key of their own), lore, item_model, custom_model_data.
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "common/data/components/PresentationComponents.hpp"
#include "common/entity/raid/OminousBanner.hpp"

namespace Game::Anvil::ComponentNbt {

    namespace {

        // ── custom_name (ComponentSerialization.CODEC) ─────────────────────
        void WriteCustomName(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto name = GetCustomNameComponent(stack)) WriteTextComponent(w, key, *name);
        }

        bool ReadCustomName(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const std::optional<Text::Component> name = ReadTextComponent(tag);
            if (!name) { ctx.Fail("Not a text component"); return false; }
            SetCustomName(stack.components, *name);
            return true;
        }

        // ── item_name ──────────────────────────────────────────────────────
        void WriteItemName(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (!stack.components.get(DataComponents::ITEM_NAME)) return;
            // The ominous banner's is Raid's translatable name, written as
            // such so a vanilla reader sees the exact banner (ItemStack.
            // matches decides raid captaincy).
            if (IsSameItemSameComponents(stack, Raid::GetOminousBannerInstance())) {
                WriteTextComponent(w, key, Text::Component::Translatable(Raid::kOminousBannerNameKey));
                return;
            }
            WriteTextComponent(w, key, GetItemNameComponent(stack));
        }

        bool ReadItemName(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const std::optional<Text::Component> name = ReadTextComponent(tag);
            if (!name) { ctx.Fail("Not a text component"); return false; }
            SetItemName(stack.components, *name);
            return true;
        }

        // ── lore (ItemLore.CODEC: a list of components, at most 256) ───────
        void WriteLore(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto lore = stack.components.get(DataComponents::LORE);
            if (!lore) return;
            bool allStrings = true;
            std::string scratch;
            for (const Text::Component& line : lore->lines) {
                if (!line.TryCollapseToString(scratch)) { allStrings = false; break; }
            }
            if (allStrings) {
                auto list = w.BeginList(key, Nbt::TagType::String);
                for (const Text::Component& line : lore->lines) {
                    std::string text;
                    line.TryCollapseToString(text);
                    w.ListString(list, text);
                }
                w.EndList(list);
                return;
            }
            // A mixed list: every line in the wrapped form ({"": line}), as
            // 26.x's ListTag stores a heterogeneous list.
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const Text::Component& line : lore->lines) {
                w.ListCompoundBegin(list);
                WriteTextComponent(w, "", line);
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool ReadLore(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            ItemLore lore;
            if (const auto* list = AsList(&tag)) {
                if (list->value.size() > ItemLore::kMaxLines) {
                    ctx.Fail("List too long: " + std::to_string(list->value.size()) + ", expected range [0-256]");
                    return false;
                }
                for (const auto& element : list->value) {
                    if (!element) continue;
                    std::optional<Text::Component> line = ReadTextComponent(*element);
                    if (!line) { ctx.Fail("Not a text component"); return false; }
                    lore.lines.push_back(std::move(*line));
                }
            } else if (std::optional<Text::Component> single = ReadTextComponent(tag)) {
                // A bare component: one line (lenient; the /give shorthand).
                lore.lines.push_back(std::move(*single));
            } else {
                ctx.Fail("Not a list");
                return false;
            }
            stack.components.set(DataComponents::LORE, std::move(lore));
            return true;
        }

        // ── item_model (Identifier.CODEC) ───────────────────────────────────
        void WriteItemModel(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (auto model = stack.components.get(DataComponents::ITEM_MODEL)) w.String(key, WithNamespace(*model));
        }

        bool ReadItemModel(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const std::optional<std::string> id = StringOf(tag);
            if (!id || id->empty()) { ctx.Fail("Not a string"); return false; }
            for (char c : *id) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                                c == '.' || c == '/' || c == ':';
                if (!ok) { ctx.Fail("Non [a-z0-9/._-] character in path of location: " + *id); return false; }
            }
            stack.components.set(DataComponents::ITEM_MODEL, WithNamespace(*id));
            return true;
        }

        // ── custom_model_data (CustomModelData.CODEC) ──────────────────────
        // {floats: [F], flags: [B], strings: [S], colors: [RGB]} — each list
        // optional. The pre-1.21.4 bare int is read as floats: [value].
        void WriteCustomModelData(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto cmd = stack.components.get(DataComponents::CUSTOM_MODEL_DATA);
            if (!cmd) return;
            w.BeginCompound(key);
            if (!cmd->floats.empty()) {
                auto list = w.BeginList("floats", Nbt::TagType::Float);
                for (float f : cmd->floats) w.ListFloat(list, f);
                w.EndList(list);
            }
            if (!cmd->flags.empty()) {
                auto list = w.BeginList("flags", Nbt::TagType::Byte);
                for (bool f : cmd->flags) w.ListByte(list, f ? 1 : 0);
                w.EndList(list);
            }
            if (!cmd->strings.empty()) {
                auto list = w.BeginList("strings", Nbt::TagType::String);
                for (const std::string& s : cmd->strings) w.ListString(list, s);
                w.EndList(list);
            }
            if (!cmd->colors.empty()) {
                auto list = w.BeginList("colors", Nbt::TagType::Int);
                for (int32_t c : cmd->colors) w.ListInt(list, c);
                w.EndList(list);
            }
            w.EndCompound();
        }

        bool ReadCustomModelData(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            CustomModelData cmd;
            if (auto n = NumberOf(tag)) {
                cmd.floats.push_back(static_cast<float>(*n));
            } else if (const auto* c = AsCompound(&tag)) {
                cmd.floats = ReadFloatList(c->GetTag("floats").get());
                if (const auto* flags = AsList(c->GetTag("flags").get())) {
                    for (const auto& e : flags->value) {
                        if (!e) continue;
                        if (auto b = BoolOf(*Unwrap(e.get()))) cmd.flags.push_back(*b);
                    }
                }
                cmd.strings = ReadStringList(c->GetTag("strings").get());
                if (const auto* colors = AsList(c->GetTag("colors").get())) {
                    for (const auto& e : colors->value) {
                        if (!e) continue;
                        if (auto rgb = ReadRgb(*Unwrap(e.get()))) cmd.colors.push_back(*rgb);
                    }
                } else if (auto colorsTag = c->GetTag("colors")) {
                    cmd.colors = ReadIntList(colorsTag.get());
                }
            } else {
                ctx.Fail("Not a map");
                return false;
            }
            stack.components.set(DataComponents::CUSTOM_MODEL_DATA, std::move(cmd));
            return true;
        }

        const Registrar kCustomName{DataComponents::CUSTOM_NAME, &WriteCustomName, &ReadCustomName};
        const Registrar kItemName{DataComponents::ITEM_NAME, &WriteItemName, &ReadItemName};
        const Registrar kLore{DataComponents::LORE, &WriteLore, &ReadLore};
        const Registrar kItemModel{DataComponents::ITEM_MODEL, &WriteItemModel, &ReadItemModel};
        const Registrar kCustomModelData{DataComponents::CUSTOM_MODEL_DATA, &WriteCustomModelData, &ReadCustomModelData};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
