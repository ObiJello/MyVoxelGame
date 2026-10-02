// File: src/common/data/components/AttributeComponents.cpp
#include "AttributeComponents.hpp"

#include "common/core/Log.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <stdexcept>
#include <string>

namespace Game {

    // ── EquipmentSlotGroup ─────────────────────────────────────────────────

    namespace {
        constexpr std::string_view kGroupNames[kEquipmentSlotGroupCount] = {
            "any", "mainhand", "offhand", "hand", "feet", "legs", "chest", "head", "armor", "body", "saddle" };

        bool IsArmorSlot(EquipmentSlot slot) {
            // EquipmentSlot.isArmor: HUMANOID_ARMOR or ANIMAL_ARMOR.
            switch (slot) {
                case EquipmentSlot::FEET: case EquipmentSlot::LEGS: case EquipmentSlot::CHEST:
                case EquipmentSlot::HEAD: case EquipmentSlot::BODY:
                    return true;
                default:
                    return false;
            }
        }
    }

    bool EquipmentSlotGroupTest(EquipmentSlotGroup group, EquipmentSlot slot) {
        switch (group) {
            case EquipmentSlotGroup::ANY:      return true;
            case EquipmentSlotGroup::MAINHAND: return slot == EquipmentSlot::MAINHAND;
            case EquipmentSlotGroup::OFFHAND:  return slot == EquipmentSlot::OFFHAND;
            case EquipmentSlotGroup::HAND:     return slot == EquipmentSlot::MAINHAND || slot == EquipmentSlot::OFFHAND;
            case EquipmentSlotGroup::FEET:     return slot == EquipmentSlot::FEET;
            case EquipmentSlotGroup::LEGS:     return slot == EquipmentSlot::LEGS;
            case EquipmentSlotGroup::CHEST:    return slot == EquipmentSlot::CHEST;
            case EquipmentSlotGroup::HEAD:     return slot == EquipmentSlot::HEAD;
            case EquipmentSlotGroup::ARMOR:    return IsArmorSlot(slot);
            case EquipmentSlotGroup::BODY:     return slot == EquipmentSlot::BODY;
            case EquipmentSlotGroup::SADDLE:   return slot == EquipmentSlot::SADDLE;
        }
        return false;
    }

    std::string_view EquipmentSlotGroupName(EquipmentSlotGroup group) {
        const auto i = static_cast<size_t>(group);
        return i < static_cast<size_t>(kEquipmentSlotGroupCount) ? kGroupNames[i] : kGroupNames[0];
    }

    bool EquipmentSlotGroupFromName(std::string_view name, EquipmentSlotGroup& out) {
        for (int i = 0; i < kEquipmentSlotGroupCount; ++i) {
            if (kGroupNames[i] == name) { out = static_cast<EquipmentSlotGroup>(i); return true; }
        }
        return false;
    }

    EquipmentSlotGroup EquipmentSlotGroupBySlot(EquipmentSlot slot) {
        switch (slot) {
            case EquipmentSlot::MAINHAND: return EquipmentSlotGroup::MAINHAND;
            case EquipmentSlot::OFFHAND:  return EquipmentSlotGroup::OFFHAND;
            case EquipmentSlot::FEET:     return EquipmentSlotGroup::FEET;
            case EquipmentSlot::LEGS:     return EquipmentSlotGroup::LEGS;
            case EquipmentSlot::CHEST:    return EquipmentSlotGroup::CHEST;
            case EquipmentSlot::HEAD:     return EquipmentSlotGroup::HEAD;
            case EquipmentSlot::BODY:     return EquipmentSlotGroup::BODY;
            case EquipmentSlot::SADDLE:   return EquipmentSlotGroup::SADDLE;
        }
        return EquipmentSlotGroup::ANY;
    }

    std::string_view AttributeOperationName(AttributeOperation op) {
        switch (op) {
            case AttributeOperation::AddValue:           return "add_value";
            case AttributeOperation::AddMultipliedBase:  return "add_multiplied_base";
            case AttributeOperation::AddMultipliedTotal: return "add_multiplied_total";
        }
        return "add_value";
    }

    bool AttributeOperationFromName(std::string_view name, AttributeOperation& out) {
        if (name == "add_value")            { out = AttributeOperation::AddValue; return true; }
        if (name == "add_multiplied_base")  { out = AttributeOperation::AddMultipliedBase; return true; }
        if (name == "add_multiplied_total") { out = AttributeOperation::AddMultipliedTotal; return true; }
        // 1.20.5's names, still accepted by the datafixer.
        if (name == "add_number")         { out = AttributeOperation::AddValue; return true; }
        if (name == "add_scalar")         { out = AttributeOperation::AddMultipliedBase; return true; }
        if (name == "multiply_scalar_1")  { out = AttributeOperation::AddMultipliedTotal; return true; }
        return false;
    }

    // ── ItemAttributeModifiers ─────────────────────────────────────────────

    AttributeModifier ItemAttributeModifiers::Entry::ToModifier() const {
        AttributeModifier m;
        m.id = static_cast<uint32_t>(ItemModifierId(id));
        m.amount = amount;
        m.operation = operation;
        return m;
    }

    bool ItemAttributeModifiers::Entry::Matches(Attribute a, std::string_view modifierId) const {
        if (a != attribute) return false;
        return ItemModifierId(id) == ItemModifierId(modifierId);
    }

    ItemAttributeModifiers ItemAttributeModifiers::WithModifierAdded(Attribute attribute, std::string id, double amount,
                                                                     AttributeOperation op,
                                                                     EquipmentSlotGroup slot) const {
        ItemAttributeModifiers out;
        out.modifiers.reserve(modifiers.size() + 1);
        for (const Entry& e : modifiers) {
            if (!e.Matches(attribute, id)) out.modifiers.push_back(e);
        }
        Entry added;
        added.attribute = attribute;
        added.id = std::move(id);
        added.amount = amount;
        added.operation = op;
        added.slot = slot;
        out.modifiers.push_back(std::move(added));
        return out;
    }

    void ItemAttributeModifiers::ForEach(EquipmentSlot slot, const std::function<void(const Entry&)>& consumer) const {
        for (const Entry& e : modifiers) {
            if (EquipmentSlotGroupTest(e.slot, slot)) consumer(e);
        }
    }

    void ItemAttributeModifiers::ForEach(EquipmentSlotGroup group, const std::function<void(const Entry&)>& consumer) const {
        for (const Entry& e : modifiers) {
            if (e.slot == group) consumer(e);
        }
    }

    double ItemAttributeModifiers::Compute(Attribute attribute, double baseValue, EquipmentSlot slot) const {
        double value = baseValue;
        for (const Entry& e : modifiers) {
            if (e.attribute != attribute || !EquipmentSlotGroupTest(e.slot, slot)) continue;
            switch (e.operation) {
                case AttributeOperation::AddValue:           value += e.amount; break;
                case AttributeOperation::AddMultipliedBase:  value += e.amount * baseValue; break;
                case AttributeOperation::AddMultipliedTotal: value += e.amount * value; break;
            }
        }
        return value;
    }

    void ForEachItemModifier(const ItemStack& stack, EquipmentSlot slot,
                             const std::function<void(Attribute, const AttributeModifier&)>& consumer,
                             bool includeBroken) {
        if (stack.IsEmpty() || (!includeBroken && IsBrokenItem(stack))) return;
        if (auto modifiers = stack.get(DataComponents::ATTRIBUTE_MODIFIERS)) {
            modifiers->ForEach(slot, [&](const ItemAttributeModifiers::Entry& e) {
                consumer(e.attribute, e.ToModifier());
            });
        }
        EnchantmentHelper::ForEachModifier(stack, slot, consumer);
    }

    void SwapEquipmentModifiers(AttributeMap& attributes, EquipmentSlot slot,
                                const ItemStack& previous, const ItemStack& current) {
        if (!previous.IsEmpty()) {
            ForEachItemModifier(previous, slot, [&](Attribute a, const AttributeModifier& m) {
                attributes.RemoveModifier(a, static_cast<ModifierId>(m.id));
            }, /*includeBroken=*/true);
        }
        if (!current.IsEmpty() && !IsBrokenItem(current)) {
            // addTransientAttributeModifiers: replace an equal id.
            ForEachItemModifier(current, slot, [&](Attribute a, const AttributeModifier& m) {
                attributes.AddModifier(a, m);
            });
        }
    }

    void ForEachTooltipModifier(const ItemStack& stack, EquipmentSlotGroup group,
                                const std::function<void(const TooltipModifier&)>& consumer) {
        if (stack.IsEmpty()) return;
        if (auto modifiers = stack.get(DataComponents::ATTRIBUTE_MODIFIERS)) {
            modifiers->ForEach(group, [&](const ItemAttributeModifiers::Entry& e) {
                consumer(TooltipModifier{e.attribute, e.ToModifier(), e.id, e.display});
            });
        }
        // EnchantmentHelper.forEachModifier(stack, EquipmentSlotGroup, …):
        // an enchantment whose definition lists exactly this group gives its
        // attribute effects, with the default display.
        const auto enchantments = stack.get(DataComponents::ENCHANTMENTS);
        if (!enchantments) return;
        const std::string_view groupName = EquipmentSlotGroupName(group);
        EquipmentSlot sample = EquipmentSlot::MAINHAND;
        for (int s = 0; s <= static_cast<int>(EquipmentSlot::SADDLE); ++s) {
            if (EquipmentSlotGroupTest(group, static_cast<EquipmentSlot>(s))) { sample = static_cast<EquipmentSlot>(s); break; }
        }
        for (const EnchantmentInstance& inst : enchantments->entries) {
            const EnchantmentDefinitions::Definition& d = EnchantmentDefinitions::Get(inst.id);
            if (!d.loaded || d.effects.attributes.empty()) continue;
            bool listed = false;
            for (const std::string& g : d.slots) if (g == groupName) { listed = true; break; }
            if (!listed) continue;
            for (const EnchantmentAttributeEffect& a : d.effects.attributes) {
                if (!a.Valid()) continue;
                TooltipModifier m{a.attribute, a.GetModifier(inst.level, sample), a.id, {}};
                consumer(m);
            }
        }
    }

    double ComputeItemAttribute(const ItemStack& stack, Attribute attribute, double baseValue, EquipmentSlot slot) {
        if (stack.IsEmpty()) return baseValue;
        const auto modifiers = stack.get(DataComponents::ATTRIBUTE_MODIFIERS);
        return modifiers ? modifiers->Compute(attribute, baseValue, slot) : baseValue;
    }

    TooltipDisplay GetTooltipDisplay(const ItemStack& stack) {
        if (auto display = stack.get(DataComponents::TOOLTIP_DISPLAY)) return *display;
        return TooltipDisplay{};
    }

    // ── Item defaults ──────────────────────────────────────────────────────

    void ItemRegistry_RegisterAttributeModifiers(std::unordered_map<ItemID, Item>& pureItems) {
        std::unordered_map<std::string_view, ItemID> bySlug;
        bySlug.reserve(kPureItemTableSize);
        for (size_t i = 0; i < kPureItemTableSize; ++i) {
            if (kPureItemTable[i].slug) bySlug.emplace(kPureItemTable[i].slug, PURE_ITEM_BASE + static_cast<ItemID>(i));
        }
        const auto find = [&](std::string_view slug) -> Item* {
            const auto id = bySlug.find(slug);
            if (id == bySlug.end()) return nullptr;
            auto it = pureItems.find(id->second);
            return it == pureItems.end() ? nullptr : &it->second;
        };

        size_t weapons = 0, armour = 0;
        // Weapons and tools: ToolMaterial.createToolAttributes /
        // createSwordAttributes, Item.Properties.spear, MaceItem /
        // TridentItem.createAttributes — BASE_ATTACK_DAMAGE_ID and
        // BASE_ATTACK_SPEED_ID, ADD_VALUE, MAINHAND.
        for (const ItemAttributeRow& row : kItemAttributes) {
            Item* item = find(row.slug);
            if (!item) continue;
            ItemAttributeModifiers mods;
            ItemAttributeModifiers::Entry damage;
            damage.attribute = Attribute::AttackDamage;
            damage.id = std::string(kBaseAttackDamageId);
            damage.amount = static_cast<double>(row.attackDamage);
            damage.slot = EquipmentSlotGroup::MAINHAND;
            mods.modifiers.push_back(damage);
            ItemAttributeModifiers::Entry speed;
            speed.attribute = Attribute::AttackSpeed;
            speed.id = std::string(kBaseAttackSpeedId);
            speed.amount = static_cast<double>(row.attackSpeed);
            speed.slot = EquipmentSlotGroup::MAINHAND;
            mods.modifiers.push_back(speed);
            item->defaultComponents.set(DataComponents::ATTRIBUTE_MODIFIERS, std::move(mods));
            ++weapons;
        }
        // Armour: ArmorMaterial.createAttributes(type) — ARMOR and
        // ARMOR_TOUGHNESS always (a zero toughness is still an entry; the
        // tooltip skips zero amounts), KNOCKBACK_RESISTANCE only when > 0;
        // one "minecraft:armor.<type>" id on the type's slot group.
        for (const ItemArmorRow& row : kItemArmorAttributes) {
            Item* item = find(row.slug);
            if (!item) continue;
            EquipmentSlotGroup group = EquipmentSlotGroup::BODY;
            const char* typeName = "body";
            switch (row.slot) {
                case ArmorSlotGroup::Head:  group = EquipmentSlotGroup::HEAD;  typeName = "helmet";     break;
                case ArmorSlotGroup::Chest: group = EquipmentSlotGroup::CHEST; typeName = "chestplate"; break;
                case ArmorSlotGroup::Legs:  group = EquipmentSlotGroup::LEGS;  typeName = "leggings";   break;
                case ArmorSlotGroup::Feet:  group = EquipmentSlotGroup::FEET;  typeName = "boots";      break;
                case ArmorSlotGroup::Body:  group = EquipmentSlotGroup::BODY;  typeName = "body";       break;
            }
            const std::string id = std::string("minecraft:armor.") + typeName;
            ItemAttributeModifiers mods;
            const auto add = [&](Attribute a, double amount) {
                ItemAttributeModifiers::Entry e;
                e.attribute = a;
                e.id = id;
                e.amount = amount;
                e.slot = group;
                mods.modifiers.push_back(std::move(e));
            };
            add(Attribute::Armor, static_cast<double>(row.armor));
            add(Attribute::ArmorToughness, static_cast<double>(row.armorToughness));
            if (row.knockbackResistance > 0.0f) add(Attribute::KnockbackResistance, static_cast<double>(row.knockbackResistance));
            item->defaultComponents.set(DataComponents::ATTRIBUTE_MODIFIERS, std::move(mods));
            ++armour;
        }
        Log::Info("[ItemAttributes] default attribute_modifiers on %zu weapons/tools, %zu armour pieces",
                  weapons, armour);
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {

        // ItemAttributeModifiers.STREAM_CODEC: a list of Entry (Attribute
        // holder, AttributeModifier {id, amount, operation}, slot group,
        // Display dispatched on its type id).
        void SerAttributeModifiers(Network::PacketBuffer& b, const ItemAttributeModifiers& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.modifiers.size()));
            for (const ItemAttributeModifiers::Entry& e : v.modifiers) {
                b.WriteVarInt(static_cast<uint32_t>(e.attribute));
                b.WriteString(e.id);
                b.WriteDouble(e.amount);
                b.WriteVarInt(static_cast<uint32_t>(e.operation));
                b.WriteVarInt(static_cast<uint32_t>(e.slot));
                b.WriteVarInt(static_cast<uint32_t>(e.display.type));
                if (e.display.type == ItemAttributeModifiers::Display::Type::Override) {
                    Text::Write(b, e.display.value);
                }
            }
        }

        ItemAttributeModifiers DeAttributeModifiers(Network::PacketReader& r) {
            ItemAttributeModifiers v;
            const uint32_t count = r.ReadVarInt();
            if (count > 4096) throw std::runtime_error("attribute_modifiers: too many entries");
            v.modifiers.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                ItemAttributeModifiers::Entry e;
                const uint32_t attribute = r.ReadVarInt();
                if (attribute >= static_cast<uint32_t>(Attribute::Count)) {
                    throw std::runtime_error("attribute_modifiers: attribute id out of range");
                }
                e.attribute = static_cast<Attribute>(attribute);
                e.id = r.ReadString();
                e.amount = r.ReadDouble();
                const uint32_t op = r.ReadVarInt();
                e.operation = op <= 2 ? static_cast<AttributeOperation>(op) : AttributeOperation::AddValue;
                const uint32_t slot = r.ReadVarInt();
                e.slot = slot < static_cast<uint32_t>(kEquipmentSlotGroupCount)
                    ? static_cast<EquipmentSlotGroup>(slot) : EquipmentSlotGroup::ANY;
                const uint32_t display = r.ReadVarInt();
                e.display.type = display <= 2 ? static_cast<ItemAttributeModifiers::Display::Type>(display)
                                              : ItemAttributeModifiers::Display::Type::Default;
                if (e.display.type == ItemAttributeModifiers::Display::Type::Override) {
                    e.display.value = Text::Read(r);
                }
                v.modifiers.push_back(std::move(e));
            }
            return v;
        }

        // TooltipDisplay.STREAM_CODEC: hide_tooltip, then the hidden
        // component types (by registry name here — our ids are bespoke).
        void SerTooltipDisplay(Network::PacketBuffer& b, const TooltipDisplay& v) {
            b.WriteByte(v.hideTooltip ? 1 : 0);
            b.WriteVarInt(static_cast<uint32_t>(v.hiddenComponents.size()));
            for (const DataComponentTypeBase* t : v.hiddenComponents) b.WriteString(t ? t->name : std::string());
        }

        TooltipDisplay DeTooltipDisplay(Network::PacketReader& r) {
            TooltipDisplay v;
            v.hideTooltip = r.ReadByte() != 0;
            const uint32_t count = r.ReadVarInt();
            if (count > 1024) throw std::runtime_error("tooltip_display: too many hidden components");
            for (uint32_t i = 0; i < count; ++i) {
                const std::string name = r.ReadString();
                if (const DataComponentTypeBase* t = ByName(name)) v.hiddenComponents.push_back(t);
            }
            return v;
        }

        void SerIdentifier(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeIdentifier(Network::PacketReader& r) { return r.ReadString(); }

    } // namespace

    const DataComponentType<ItemAttributeModifiers> ATTRIBUTE_MODIFIERS{"attribute_modifiers", 200,
                                                                        &SerAttributeModifiers, &DeAttributeModifiers};
    const DataComponentType<TooltipDisplay> TOOLTIP_DISPLAY{"tooltip_display", 201,
                                                            &SerTooltipDisplay, &DeTooltipDisplay};
    const DataComponentType<std::string> TOOLTIP_STYLE{"tooltip_style", 202, &SerIdentifier, &DeIdentifier};

} // namespace Game::DataComponents
