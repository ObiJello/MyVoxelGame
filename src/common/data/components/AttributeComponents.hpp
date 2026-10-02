// File: src/common/data/components/AttributeComponents.hpp
//
// MC 26.3 item components (core): attribute_modifiers, tooltip_display,
// tooltip_style. Wire ids 200-209 (DataComponents.hpp's id table). The NBT
// codecs live in server/world/storage/anvil/components/AttributeNbt.cpp.
//
// ATTRIBUTE_MODIFIERS is the one source of an item's attribute effect: the
// weapon's attack damage and speed, the armour's armour / toughness /
// knockback resistance, anything a /give or a datapack puts on a stack.
// Every item's defaults (MC Items.java through ToolMaterial / ArmorMaterial /
// Item.Properties.spear / MaceItem / TridentItem .createAttributes) are put
// on the item's prototype by RegisterDefaultAttributeModifiers, and a stack's
// patch overrides them — `diamond_sword[attribute_modifiers=[]]` deals bare-
// hand damage, exactly as in vanilla.
#pragma once

#include "../DataComponents.hpp"
#include "common/entity/Attributes.hpp"
#include "common/text/TextComponent.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    // MC EquipmentSlotGroup (world/entity/EquipmentSlotGroup.java): the
    // slots an attribute modifier applies in. Ordinals are MC's ids (wire)
    // and its declaration order is the tooltip's section order.
    enum class EquipmentSlotGroup : uint8_t {
        ANY      = 0,
        MAINHAND = 1,
        OFFHAND  = 2,
        HAND     = 3,
        FEET     = 4,
        LEGS     = 5,
        CHEST    = 6,
        HEAD     = 7,
        ARMOR    = 8,
        BODY     = 9,
        SADDLE   = 10,
    };
    inline constexpr int kEquipmentSlotGroupCount = 11;

    // EquipmentSlotGroup.test(slot).
    bool EquipmentSlotGroupTest(EquipmentSlotGroup group, EquipmentSlot slot);
    // getSerializedName ("mainhand") and the reverse.
    std::string_view EquipmentSlotGroupName(EquipmentSlotGroup group);
    bool EquipmentSlotGroupFromName(std::string_view name, EquipmentSlotGroup& out);
    // EquipmentSlotGroup.bySlot.
    EquipmentSlotGroup EquipmentSlotGroupBySlot(EquipmentSlot slot);

    // MC AttributeModifier.Operation's serialized names ("add_value",
    // "add_multiplied_base", "add_multiplied_total").
    std::string_view AttributeOperationName(AttributeOperation op);
    bool AttributeOperationFromName(std::string_view name, AttributeOperation& out);

    // MC ItemAttributeModifiers (world/item/component/ItemAttributeModifiers
    // .java) — the list of (attribute, modifier, slot group, display).
    struct ItemAttributeModifiers {
        // MC ItemAttributeModifiers.Display: how the tooltip shows an entry.
        struct Display {
            enum class Type : uint8_t { Default = 0, Hidden = 1, Override = 2 };
            Type type = Type::Default;
            Text::Component value;   // Override's text
        };

        struct Entry {
            Attribute          attribute = Attribute::AttackDamage;
            // AttributeModifier.id — an Identifier, kept with its namespace
            // ("minecraft:base_attack_damage").
            std::string        id;
            double             amount = 0.0;
            AttributeOperation operation = AttributeOperation::AddValue;
            EquipmentSlotGroup slot = EquipmentSlotGroup::ANY;
            Display            display;

            // The entity-side modifier this entry puts on (its id hashed).
            AttributeModifier ToModifier() const;
            // Entry.matches(attribute, id).
            bool Matches(Attribute a, std::string_view modifierId) const;
        };

        std::vector<Entry> modifiers;

        bool IsEmpty() const { return modifiers.empty(); }

        // withModifierAdded: an entry for the same (attribute, id) is replaced.
        ItemAttributeModifiers WithModifierAdded(Attribute attribute, std::string id, double amount,
                                                 AttributeOperation op, EquipmentSlotGroup slot) const;

        // forEach(EquipmentSlot, consumer): every entry whose group holds `slot`.
        void ForEach(EquipmentSlot slot, const std::function<void(const Entry&)>& consumer) const;
        // forEach(EquipmentSlotGroup, consumer): every entry of exactly `group`.
        void ForEach(EquipmentSlotGroup group, const std::function<void(const Entry&)>& consumer) const;

        // compute(attribute, baseValue, slot) — the value of `attribute`
        // from `baseValue` through the entries worn in `slot` (MC's single-
        // pass fold, used for "what would this item give" comparisons).
        double Compute(Attribute attribute, double baseValue, EquipmentSlot slot) const;
    };

    // Item.BASE_ATTACK_DAMAGE_ID / BASE_ATTACK_SPEED_ID — the entries the
    // tooltip prints as the player's total ("equals") instead of "+N".
    inline constexpr std::string_view kBaseAttackDamageId = "minecraft:base_attack_damage";
    inline constexpr std::string_view kBaseAttackSpeedId  = "minecraft:base_attack_speed";

    // MC ItemStack.forEachModifier(EquipmentSlot, consumer): the stack's
    // ATTRIBUTE_MODIFIERS entries worn in `slot`, then its enchantments'
    // attribute effects for that slot (EnchantmentHelper.forEachModifier).
    // Nothing for an empty stack, nor for a broken one unless
    // `includeBroken` (collectEquipmentChanges adds only an unbroken piece's
    // modifiers but removes the previous piece's whatever its state).
    void ForEachItemModifier(const ItemStack& stack, EquipmentSlot slot,
                             const std::function<void(Attribute, const AttributeModifier&)>& consumer,
                             bool includeBroken = false);

    // MC LivingEntity.collectEquipmentChanges for one slot of `attributes`:
    // the previous stack's modifiers come off, the new stack's go on (unless
    // it is broken). The one routine every AttributeMap-holding wearer uses.
    void SwapEquipmentModifiers(AttributeMap& attributes, EquipmentSlot slot,
                                const ItemStack& previous, const ItemStack& current);

    // The same, entries only, with their display — the tooltip's
    // forEachModifier(EquipmentSlotGroup, TriConsumer) (enchantment
    // modifiers join with Display.Default).
    struct TooltipModifier {
        Attribute                              attribute;
        AttributeModifier                      modifier;
        std::string                            id;      // the Identifier (for the BASE_* test)
        ItemAttributeModifiers::Display        display;
    };
    void ForEachTooltipModifier(const ItemStack& stack, EquipmentSlotGroup group,
                                const std::function<void(const TooltipModifier&)>& consumer);

    // The value `stack` gives `attribute` when worn in `slot`, from
    // `baseValue` (MC Mob.getApproximateAttributeWith's item half).
    double ComputeItemAttribute(const ItemStack& stack, Attribute attribute, double baseValue, EquipmentSlot slot);

    // Puts every item's default ATTRIBUTE_MODIFIERS on its prototype (the
    // generated weapon / armour rows — GeneratedItemAttributes: MC's
    // ToolMaterial.createToolAttributes / createSwordAttributes, Item.
    // Properties.spear, MaceItem / TridentItem.createAttributes and
    // ArmorMaterial.createAttributes). Called by ItemRegistry::Initialize.
    void ItemRegistry_RegisterAttributeModifiers(std::unordered_map<ItemID, Item>& pureItems);

    // MC TooltipDisplay (world/item/component/TooltipDisplay.java).
    struct TooltipDisplay {
        bool                                       hideTooltip = false;
        std::vector<const DataComponentTypeBase*>  hiddenComponents;

        // TooltipDisplay.shows(type).
        bool Shows(const DataComponentTypeBase& type) const {
            if (hideTooltip) return false;
            for (const DataComponentTypeBase* t : hiddenComponents) if (t == &type) return false;
            return true;
        }
    };

    // TOOLTIP_DISPLAY of a stack, DEFAULT when absent.
    TooltipDisplay GetTooltipDisplay(const ItemStack& stack);

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.ATTRIBUTE_MODIFIERS (id 200).
    extern const DataComponentType<ItemAttributeModifiers> ATTRIBUTE_MODIFIERS;
    // MC DataComponents.TOOLTIP_DISPLAY (id 201).
    extern const DataComponentType<TooltipDisplay> TOOLTIP_DISPLAY;
    // MC DataComponents.TOOLTIP_STYLE (id 202) — an Identifier naming the
    // tooltip's background / frame sprites (gui/sprites/tooltip/<path>_
    // background, _frame), kept with its namespace.
    extern const DataComponentType<std::string> TOOLTIP_STYLE;

} // namespace Game::DataComponents
