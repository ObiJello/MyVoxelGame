// File: src/common/data/components/ConsumableComponents.hpp
//
// MC 26.3 item components (consumables): use_cooldown, use_effects,
// death_protection — and the shared pieces of consumable / food /
// use_remainder (whose types live in DataComponents.hpp): the ConsumeEffect
// and ItemUseAnimation names and the ConsumeEffect wire codec.
// Wire ids 230-249 (DataComponents.hpp's id table). The NBT codecs (these
// three plus consumable, food and use_remainder) live in
// server/world/storage/anvil/components/ConsumableNbt.cpp.
#pragma once

#include "../DataComponents.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    // MC UseCooldown (world/item/component/UseCooldown.java): the rest a
    // successful use puts on the item's cooldown group — ItemStack.
    // applyAfterUseComponentSideEffects after a held use, ItemStack.use
    // after an instant one. `seconds` is POSITIVE_FLOAT.
    struct UseCooldown {
        float       seconds = 1.0f;
        // Optional<Identifier> cooldown_group; "" = the item's own id
        // (ItemCooldowns.getCooldownGroup). Kept with its namespace.
        std::string cooldownGroup;

        // UseCooldown.ticks().
        int Ticks() const { return static_cast<int>(seconds * 20.0f); }
    };

    // MC UseEffects (world/item/component/UseEffects.java, 26.x): what
    // holding the item in use does to its user. DEFAULT is on every item
    // (DataComponents.COMMON_ITEM_COMPONENTS) — an absent component reads as
    // it. The spears carry (true, false, 1.0).
    struct UseEffects {
        bool  canSprint          = false;   // LocalPlayer.isSlowDueToUsingItem
        bool  interactVibrations = true;    // ItemStack.causeUseVibration
        float speedMultiplier    = 0.2f;    // LocalPlayer.itemUseSpeedMultiplier — floatRange(0, 1)
    };

    // MC DeathProtection (world/item/component/DeathProtection.java): a hand
    // item carrying it saves its holder from a death that does not bypass
    // invulnerability (LivingEntity.checkTotemDeathProtection), is spent,
    // and applies these effects.
    struct DeathProtection {
        std::vector<ConsumeEffect> deathEffects;

        // DeathProtection.TOTEM_OF_UNDYING: clear every effect, then
        // REGENERATION II 45 s, ABSORPTION II 5 s, FIRE_RESISTANCE 40 s.
        static DeathProtection TotemOfUndying();
    };

    // USE_EFFECTS of a stack, UseEffects.DEFAULT when absent.
    UseEffects GetUseEffects(const ItemStack& stack);

    // ConsumeEffect.Type registry names ("apply_effects" …, no namespace) and
    // the reverse (accepts "minecraft:" too).
    std::string_view ConsumeEffectTypeName(ConsumeEffect::Type type);
    bool ConsumeEffectTypeFromName(std::string_view name, ConsumeEffect::Type& out);

    // ItemUseAnimation's serialized names ("eat", "toot_horn" …).
    std::string_view ItemUseAnimationName(ItemUseAnimation animation);
    bool ItemUseAnimationFromName(std::string_view name, ItemUseAnimation& out);

    // ConsumeEffect.STREAM_CODEC: the type, then that type's record.
    void WriteConsumeEffect(Network::PacketBuffer& b, const ConsumeEffect& e);
    ConsumeEffect ReadConsumeEffect(Network::PacketReader& r);
    // MobEffectInstance.STREAM_CODEC as this engine carries it (id,
    // amplifier, duration, ambient, particles, icon).
    void WriteEffectInstance(Network::PacketBuffer& b, const MobEffectInstance& e);
    MobEffectInstance ReadEffectInstance(Network::PacketReader& r);

    // The item defaults of this group (Items.java): useCooldown on the ender
    // pearl, wind charge and chorus fruit; the spears' UseEffects(true,
    // false, 1.0); the totem's DeathProtection.TOTEM_OF_UNDYING. Called by
    // ItemRegistry::Initialize after the behaviours.
    void ItemRegistry_RegisterConsumableComponents(std::unordered_map<ItemID, Item>& pureItems);

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.USE_COOLDOWN (id 230).
    extern const DataComponentType<UseCooldown> USE_COOLDOWN;
    // MC DataComponents.USE_EFFECTS (id 231).
    extern const DataComponentType<UseEffects> USE_EFFECTS;
    // MC DataComponents.DEATH_PROTECTION (id 232).
    extern const DataComponentType<DeathProtection> DEATH_PROTECTION;

} // namespace Game::DataComponents
