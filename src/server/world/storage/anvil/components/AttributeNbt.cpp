// File: src/server/world/storage/anvil/components/AttributeNbt.cpp
//
// NBT codecs (MC DataComponentType.codec() over NbtOps) for the components
// of common/data/components/AttributeComponents.hpp, registered with
// ComponentNbt: attribute_modifiers, tooltip_display, tooltip_style.
#include "server/world/storage/anvil/ComponentNbt.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "common/data/components/AttributeComponents.hpp"

#include <cmath>

namespace Game::Anvil::ComponentNbt {

    namespace {

        // ── attribute_modifiers ───────────────────────────────────────────
        // ItemAttributeModifiers.CODEC = Entry.CODEC.listOf(); Entry =
        // {type: Attribute id, id: Identifier, amount: double, operation:
        // Operation name (AttributeModifier.MAP_CODEC), slot: group name
        // (optional, default "any"), display: Display (optional, default
        // {type:"default"})}.

        void WriteAttributeModifiers(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto mods = stack.components.get(DataComponents::ATTRIBUTE_MODIFIERS);
            if (!mods) return;
            auto list = w.BeginList(key, Nbt::TagType::Compound);
            for (const ItemAttributeModifiers::Entry& e : mods->modifiers) {
                w.ListCompoundBegin(list);
                w.String("type", "minecraft:" + std::string(AttributeName(e.attribute)));
                w.String("id", WithNamespace(e.id));
                w.Double("amount", e.amount);
                w.String("operation", std::string(AttributeOperationName(e.operation)));
                if (e.slot != EquipmentSlotGroup::ANY) w.String("slot", std::string(EquipmentSlotGroupName(e.slot)));
                switch (e.display.type) {
                    case ItemAttributeModifiers::Display::Type::Default:
                        break;
                    case ItemAttributeModifiers::Display::Type::Hidden:
                        w.BeginCompound("display");
                        w.String("type", "hidden");
                        w.EndCompound();
                        break;
                    case ItemAttributeModifiers::Display::Type::Override:
                        w.BeginCompound("display");
                        w.String("type", "override");
                        WriteTextComponent(w, "value", e.display.value);
                        w.EndCompound();
                        break;
                }
                w.ListCompoundEnd(list);
            }
            w.EndList(list);
        }

        bool ReadEntry(const ::World::NBTTagCompound& c, ItemAttributeModifiers::Entry& out, const ReadContext& ctx) {
            // type (required, a registered attribute).
            const auto typeTag = c.GetTag("type");
            const std::optional<std::string> type = typeTag ? StringOf(*typeTag) : std::nullopt;
            if (!type) { ctx.Fail("No key type in MapLike"); return false; }
            if (!AttributeFromName(*type, out.attribute)) {
                ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:attribute]: " + WithNamespace(*type));
                return false;
            }
            // id (required, an Identifier). Pre-1.21 saves carried a UUID and
            // a name; MC's datafixer turned those into an id — accept "name"
            // as a fallback so such a stack keeps its modifier.
            std::optional<std::string> id;
            if (auto t = c.GetTag("id")) id = StringOf(*t);
            if (!id) if (auto t = c.GetTag("name")) id = StringOf(*t);
            if (!id || id->empty()) { ctx.Fail("No key id in MapLike"); return false; }
            for (char ch : *id) {
                const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                                ch == '.' || ch == '/' || ch == ':';
                if (!ok) { ctx.Fail("Non [a-z0-9/._-] character in path of location: " + *id); return false; }
            }
            out.id = WithNamespace(*id);
            // amount (required double).
            const auto amountTag = c.GetTag("amount");
            const std::optional<double> amount = amountTag ? NumberOf(*amountTag) : std::nullopt;
            if (!amount || !std::isfinite(*amount)) { ctx.Fail("No key amount in MapLike"); return false; }
            out.amount = *amount;
            // operation (required).
            const auto opTag = c.GetTag("operation");
            const std::optional<std::string> op = opTag ? StringOf(*opTag) : std::nullopt;
            if (!op) {
                // A numeric operation (the pre-1.20.5 int form).
                if (opTag) {
                    if (auto n = NumberOf(*opTag); n && *n >= 0 && *n <= 2) {
                        out.operation = static_cast<AttributeOperation>(static_cast<int>(*n));
                    } else { ctx.Fail("No key operation in MapLike"); return false; }
                } else { ctx.Fail("No key operation in MapLike"); return false; }
            } else if (!AttributeOperationFromName(*op, out.operation)) {
                ctx.Fail("Unknown element name:" + *op);
                return false;
            }
            // slot (optional, default any).
            out.slot = EquipmentSlotGroup::ANY;
            if (auto slotTag = c.GetTag("slot")) {
                const std::optional<std::string> slot = StringOf(*slotTag);
                if (!slot || !EquipmentSlotGroupFromName(*slot, out.slot)) {
                    ctx.Fail("Unknown element name:" + slot.value_or(std::string("?")));
                    return false;
                }
            }
            // display (optional).
            out.display = {};
            if (const auto* display = AsCompound(c.GetTag("display").get())) {
                const std::string kind(StripMinecraft(display->GetValue<std::string>("type", "default")));
                if (kind == "default") {
                    out.display.type = ItemAttributeModifiers::Display::Type::Default;
                } else if (kind == "hidden") {
                    out.display.type = ItemAttributeModifiers::Display::Type::Hidden;
                } else if (kind == "override") {
                    auto valueTag = display->GetTag("value");
                    std::optional<Text::Component> value = valueTag ? ReadTextComponent(*valueTag) : std::nullopt;
                    if (!value) { ctx.Fail("No key value in MapLike"); return false; }
                    out.display.type = ItemAttributeModifiers::Display::Type::Override;
                    out.display.value = std::move(*value);
                } else {
                    ctx.Fail("Unknown element name:" + kind);
                    return false;
                }
            }
            return true;
        }

        bool ReadAttributeModifiers(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const ::World::NBTTagList* list = AsList(&tag);
            // The 1.20.5–1.21.4 wrapper {modifiers: [...], show_in_tooltip}.
            bool hideAll = false;
            if (!list) {
                const auto* wrapper = AsCompound(&tag);
                if (!wrapper) { ctx.Fail("Not a list"); return false; }
                list = AsList(wrapper->GetTag("modifiers").get());
                if (!list) { ctx.Fail("No key modifiers in MapLike"); return false; }
                if (auto show = wrapper->GetTag("show_in_tooltip")) hideAll = !BoolOf(*show).value_or(true);
            }
            ItemAttributeModifiers value;
            for (const auto& element : list->value) {
                if (!element) continue;
                const auto* entryTag = AsCompound(Unwrap(element.get()));
                if (!entryTag) { ctx.Fail("Not a map"); return false; }
                ItemAttributeModifiers::Entry entry;
                if (!ReadEntry(*entryTag, entry, ctx)) return false;
                value.modifiers.push_back(std::move(entry));
            }
            stack.components.set(DataComponents::ATTRIBUTE_MODIFIERS, std::move(value));
            if (hideAll) {
                TooltipDisplay display = GetTooltipDisplay(stack);
                display.hiddenComponents.push_back(&DataComponents::ATTRIBUTE_MODIFIERS);
                stack.components.set(DataComponents::TOOLTIP_DISPLAY, std::move(display));
            }
            return true;
        }

        // ── tooltip_display ───────────────────────────────────────────────
        // TooltipDisplay.CODEC: {hide_tooltip: bool (default false),
        // hidden_components: [component type ids] (default [])}.

        void WriteTooltipDisplay(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            const auto display = stack.components.get(DataComponents::TOOLTIP_DISPLAY);
            if (!display) return;
            w.BeginCompound(key);
            if (display->hideTooltip) w.Bool("hide_tooltip", true);
            if (!display->hiddenComponents.empty()) {
                auto list = w.BeginList("hidden_components", Nbt::TagType::String);
                for (const DataComponentTypeBase* t : display->hiddenComponents) {
                    if (t) w.ListString(list, "minecraft:" + t->name);
                }
                w.EndList(list);
            }
            w.EndCompound();
        }

        bool ReadTooltipDisplay(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const auto* c = AsCompound(&tag);
            if (!c) { ctx.Fail("Not a map"); return false; }
            TooltipDisplay display;
            if (auto hide = c->GetTag("hide_tooltip")) display.hideTooltip = BoolOf(*hide).value_or(false);
            for (const std::string& name : ReadStringList(c->GetTag("hidden_components").get())) {
                const DataComponentTypeBase* type = DataComponents::ByName(StripMinecraft(name));
                if (!type) {
                    ctx.Fail("Unknown registry key in ResourceKey[minecraft:root / minecraft:data_component_type]: " +
                             WithNamespace(name));
                    return false;
                }
                bool dup = false;
                for (const DataComponentTypeBase* t : display.hiddenComponents) dup = dup || t == type;
                if (!dup) display.hiddenComponents.push_back(type);
            }
            stack.components.set(DataComponents::TOOLTIP_DISPLAY, std::move(display));
            return true;
        }

        // ── tooltip_style ─────────────────────────────────────────────────
        // Identifier.CODEC.

        void WriteTooltipStyle(Nbt::Writer& w, std::string_view key, const ItemStack& stack) {
            if (const auto style = stack.components.get(DataComponents::TOOLTIP_STYLE)) w.String(key, WithNamespace(*style));
        }

        bool ReadTooltipStyle(const ::World::NBTTag& tag, ItemStack& stack, const ReadContext& ctx) {
            const std::optional<std::string> id = StringOf(tag);
            if (!id || id->empty()) { ctx.Fail("Not a string"); return false; }
            stack.components.set(DataComponents::TOOLTIP_STYLE, WithNamespace(*id));
            return true;
        }

        const Registrar kAttributeModifiers{DataComponents::ATTRIBUTE_MODIFIERS, &WriteAttributeModifiers, &ReadAttributeModifiers};
        const Registrar kTooltipDisplay{DataComponents::TOOLTIP_DISPLAY, &WriteTooltipDisplay, &ReadTooltipDisplay};
        const Registrar kTooltipStyle{DataComponents::TOOLTIP_STYLE, &WriteTooltipStyle, &ReadTooltipStyle};

    } // namespace

} // namespace Game::Anvil::ComponentNbt
