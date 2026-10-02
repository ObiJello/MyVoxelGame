// File: src/common/data/components/PresentationComponents.hpp
//
// MC 26.3 item components (presentation): item_model, custom_model_data, and
// the styled (text-component) forms of custom_name / item_name / lore.
// Wire ids 430-449 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/PresentationNbt.cpp, the tooltip
// providers register with ComponentTooltips.
//
// ── Styled names ────────────────────────────────────────────────────────────
// MC's CUSTOM_NAME / ITEM_NAME are text Components. The engine keeps them as
// their plain text (DataComponents::CUSTOM_NAME / ITEM_NAME, std::string) —
// the form a hundred readers (name tags, death messages, container titles,
// the anvil box …) want. A styled value (`{text:"Excalibur",color:"gold"}`,
// a translatable) rides beside it in CUSTOM_NAME_STYLE / ITEM_NAME_STYLE,
// which also records the plain text it was set with: the styled form is
// honoured only while the plain component still holds that text, so any
// writer that knows nothing of styles (the anvil, loot, an entity handing its
// name to its item) simply supersedes it. Set both through SetCustomName /
// SetItemName; clear with ClearCustomName.
#pragma once

#include "../DataComponents.hpp"
#include "common/text/TextComponent.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Game {

    // The styled half of CUSTOM_NAME / ITEM_NAME (see above).
    struct StyledName {
        Text::Component component;
        std::string     plain;   // CUSTOM_NAME / ITEM_NAME when this was set
    };

    // MC CustomModelData (world/item/component/CustomModelData.java): the
    // four value lists item model definitions read by index (range_dispatch /
    // select / condition "minecraft:custom_model_data", the
    // "minecraft:custom_model_data" tint source).
    struct CustomModelData {
        std::vector<float>       floats;
        std::vector<bool>        flags;
        std::vector<std::string> strings;
        std::vector<int32_t>     colors;   // RGB

        // getFloat / getBoolean / getString / getColor: nullopt out of range.
        std::optional<float>       GetFloat(int index) const;
        std::optional<bool>        GetFlag(int index) const;
        std::optional<std::string> GetString(int index) const;
        std::optional<int32_t>     GetColor(int index) const;
        bool IsEmpty() const { return floats.empty() && flags.empty() && strings.empty() && colors.empty(); }
    };

    // ── Names ───────────────────────────────────────────────────────────────

    // CUSTOM_NAME as a component: the styled form while it is current, else
    // the plain text as a literal; nullopt without a custom name.
    std::optional<Text::Component> GetCustomNameComponent(const ItemStack& stack);
    // MC ItemStack.getItemName (Item.getName): ITEM_NAME's styled form while
    // current, else the engine's item name (potion contents, ITEM_NAME, the
    // registry name) as a literal.
    Text::Component GetItemNameComponent(const ItemStack& stack);
    // MC ItemStack.getHoverName: the custom name, else a written book's
    // title, else the item name.
    Text::Component GetHoverNameComponent(const ItemStack& stack);

    // MC ItemStack.getStyledHoverName: the hover name (CUSTOM_NAME, else the
    // item name) as a text component, styled with the stack's rarity colour
    // (getRarity().color()) and ITALIC when CUSTOM_NAME is set. The tooltip's
    // first line.
    Text::Component GetStyledHoverName(const ItemStack& stack);

    // Sets CUSTOM_NAME (its plain text) and, when the component is more than
    // a bare literal, CUSTOM_NAME_STYLE; a bare literal clears the style.
    void SetCustomName(DataComponentMap& components, const Text::Component& name);
    // The plain-text setter the anvil uses: CUSTOM_NAME = text, style gone.
    void SetCustomNameText(DataComponentMap& components, const std::string& text);
    // Removes CUSTOM_NAME and its style.
    void ClearCustomName(DataComponentMap& components);
    // The same pair for ITEM_NAME.
    void SetItemName(DataComponentMap& components, const Text::Component& name);
    void ClearItemName(DataComponentMap& components);

    // ItemLore.LORE_STYLE: DARK_PURPLE + ITALIC, the base every lore line is
    // drawn over (its own style wins where it sets one).
    Text::Style LoreStyle();

    // ── Item model ──────────────────────────────────────────────────────────

    // The stack the renderers should DRAW for `stack` — MC ItemModelResolver.
    // updateForTopItem with the stack's ITEM_MODEL (default: the item's own
    // definition) evaluated against the stack:
    //   • no ITEM_MODEL and an own definition that ignores the stack's
    //     custom_model_data: `stack` itself (the common case — no copy);
    //   • ITEM_MODEL naming another registered item's definition that does
    //     not read custom_model_data: a copy drawn as that item (its sprite,
    //     block model, special renderer, animations);
    //   • otherwise the definition is evaluated for the stack (custom_model_
    //     data, has_component, damaged, broken, damage, count branches) and
    //     the chosen model becomes a client-side render variant item
    //     (ItemRegistry::RegisterRenderVariant).
    // The copy keeps every component of `stack`, and its enchantment glint
    // (HasFoil) is pinned to the original's. Render-only: never put the
    // result in an inventory. `scratch` backs the copy.
    const ItemStack& GetRenderStack(const ItemStack& stack, ItemStack& scratch);

} // namespace Game

namespace Game::DataComponents {

    // The styled halves of CUSTOM_NAME / ITEM_NAME (ids 430, 431). Engine
    // bookkeeping: saved as the vanilla minecraft:custom_name /
    // minecraft:item_name component, never under their own key.
    extern const DataComponentType<StyledName> CUSTOM_NAME_STYLE;
    extern const DataComponentType<StyledName> ITEM_NAME_STYLE;
    // MC DataComponents.ITEM_MODEL (id 432) — the Identifier of the item
    // model definition (assets/<ns>/items/<path>.json) the stack draws with,
    // kept with its namespace. Absent = the item's own.
    extern const DataComponentType<std::string> ITEM_MODEL;
    // MC DataComponents.CUSTOM_MODEL_DATA (id 433).
    extern const DataComponentType<CustomModelData> CUSTOM_MODEL_DATA;

} // namespace Game::DataComponents
