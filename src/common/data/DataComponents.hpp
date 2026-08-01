// File: src/common/data/DataComponents.hpp
//
// Mirrors net/minecraft/core/component/DataComponents.java — the registry of
// every DataComponentType. Components are added here as features need them;
// the eventual MC-parity list is ~50 entries.
//
// Source line numbers cited per declaration so the C++ side can be diffed
// against MC's source.
// ── Network id table ────────────────────────────────────────────────────────
// Every component that crosses the wire gets a stable id here, passed as the
// second constructor argument in DataComponents.cpp. These are OUR bespoke
// protocol ids — deliberately NOT MC's registry ordinals (both endpoints ship
// in one binary, so the only requirement is stability within a build).
// 0 is reserved for "never serialized".
//
//    1  ENCHANTMENT_GLINT_OVERRIDE     8  CONSUMABLE
//    2  STORED_ENCHANTMENTS            9  FOOD
//    3  TOOL                          10  USE_REMAINDER
//    4  CUSTOM_NAME                   11  EQUIPPABLE
//    5  ITEM_NAME                     12  BLOCKS_ATTACKS
//    6  LORE                          13  BUNDLE_CONTENTS
//    7  RARITY
//  100  PORTAL_GUN_NEXT_COLOR        101  PORTAL_GUN_INSTANCE_ID
#pragma once

#include "DataComponentType.hpp"
#include "../world/enchantment/ItemEnchantments.hpp"
#include "../entity/MiningTier.hpp"
#include "../entity/Item.hpp"              // ItemStack (UseRemainder), ItemUseAnimation
#include "../entity/EquipmentSlot.hpp"
#include "../core/Features.hpp"
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    // Mirrors net.minecraft.world.item.component.Tool (a record carrying the
    // mining-speed table + correct-tool tier). We collapse MC's "rules list"
    // (per-block-tag overrides) into a single (toolType, tier, miningSpeed)
    // tuple — sufficient for vanilla tools where the speed is uniform across
    // the entire mineable/<tag> set.
    struct Tool {
        ToolType   type        = ToolType::None;
        MiningTier tier        = MiningTier::Wood;
        float      miningSpeed = 1.0f;
    };

    // Mirrors world/item/consume_effects/ConsumeEffect.java — a sealed
    // interface with 5 record implementations. Data-only here: the appliers
    // are log-stubbed until the status-effect / teleport systems exist, but
    // the effect LIST is carried faithfully so food definitions are complete.
    struct ConsumeEffect {
        enum class Type : uint8_t {
            ApplyStatusEffects   = 0,  // ApplyStatusEffectsConsumeEffect.java
            RemoveStatusEffects  = 1,  // RemoveStatusEffectsConsumeEffect.java
            ClearAllStatusEffects= 2,  // ClearAllStatusEffectsConsumeEffect.java
            TeleportRandomly     = 3,  // TeleportRandomlyConsumeEffect.java
            PlaySound            = 4,  // PlaySoundConsumeEffect.java
        };
        Type        type = Type::PlaySound;
        // Freeform payload until the target systems exist: effect slugs +
        // durations for the status types, sound event name for PlaySound,
        // diameter for TeleportRandomly. Logged on consume.
        std::string payload;
    };

    // Mirrors the Consumable record — Consumable.java:32:
    //   (consumeSeconds, animation, sound, hasConsumeParticles, onConsumeEffects)
    struct Consumable {
        float                      consumeSeconds      = 1.6f;  // DEFAULT_CONSUME_SECONDS (:33)
        ItemUseAnimation           animation           = ItemUseAnimation::EAT;
        std::string                sound               = "entity.generic.eat"; // Holder<SoundEvent> → name (log-stub)
        bool                       hasConsumeParticles = true;
        std::vector<ConsumeEffect> onConsumeEffects;

        // Mirrors Consumable.consumeTicks (:81-83).
        int consumeTicks() const { return static_cast<int>(consumeSeconds * 20.0f); }
    };

    // Mirrors the FoodProperties record — FoodProperties.java:22:
    //   (nutrition, saturation, canAlwaysEat)
    // NOTE: `saturation` is the FINAL saturation value (MC's Builder converts
    // saturationModifier via FoodConstants.saturationByModifier at build time
    // — FoodProperties.java:60-62); our FoodDefs table does the same.
    struct FoodProperties {
        int   nutrition    = 0;
        float saturation   = 0.0f;
        bool  canAlwaysEat = false;
    };

    // Mirrors the UseRemainder record — UseRemainder.java:8. What the stack
    // converts into when fully used up (stew → bowl, honey bottle → glass
    // bottle, milk bucket → bucket).
    struct UseRemainder {
        ItemStack convertInto{};
    };

    // Mirrors the Rarity enum — Rarity.java:13-16 (id, name, ChatFormatting
    // color). Ids are wire-stable (Rarity.STREAM_CODEC id-mapper).
    enum class Rarity : uint8_t {
        COMMON   = 0,   // WHITE
        UNCOMMON = 1,   // YELLOW
        RARE     = 2,   // AQUA
        EPIC     = 3,   // LIGHT_PURPLE
    };

    // The tooltip name-line color per rarity — MC ChatFormatting ARGB values.
    inline uint32_t RarityColorARGB(Rarity rarity) {
        switch (rarity) {
            case Rarity::UNCOMMON: return 0xFFFFFF55;  // YELLOW
            case Rarity::RARE:     return 0xFF55FFFF;  // AQUA
            case Rarity::EPIC:     return 0xFFFF55FF;  // LIGHT_PURPLE
            case Rarity::COMMON:
            default:               return 0xFFFFFFFF;  // WHITE
        }
    }

    // Mirrors the ItemLore record — ItemLore.java (list of Components; plain
    // strings here — no rich-text). MAX_LINES = 256 (ItemLore.java:22),
    // enforced at the wire decode.
    struct ItemLore {
        std::vector<std::string> lines;
    };

    // Mirrors the BundleContents record — BundleContents.java:21-29. `items`
    // is newest-first (Mutable.tryInsert adds at index 0). `selectedItem` is
    // the client-side scroll selection and is NOT serialized (matches MC's
    // STREAM_CODEC, which carries only the item list). Weight math lives in
    // BundleBehavior.cpp (needs ItemRegistry + recursion for nested bundles).
    struct BundleContents {
        std::vector<ItemStack> items;
        int                    selectedItem = -1;
    };

    // Mirrors the Equippable record — Equippable.java:32. Omitted fields
    // (assetId, cameraOverlay, allowedEntities, dispensable, damageOnHurt,
    // equipOnInteract, canBeSheared, shearingSound) have no consumers here —
    // no entity rendering / dispensers / mob equip; add when those exist.
    struct Equippable {
        EquipmentSlot slot       = EquipmentSlot::HEAD;
        std::string   equipSound = "item.armor.equip_generic"; // Holder<SoundEvent> → name (log-stub)
        bool          swappable  = true;   // right-click auto-equip allowed
    };

    // Mirrors the BlocksAttacks record — BlocksAttacks.java:30. The full data
    // shape is carried (so shield definitions match Items.java verbatim) but
    // the damage math is unused — no combat system. What IS consumed:
    // blockDelayTicks() gates ServerPlayer::isBlocking(), and the sound names
    // are log-stub fodder.
    struct BlocksAttacks {
        float blockDelaySeconds    = 0.0f;
        float disableCooldownScale = 1.0f;
        // DamageReduction record — BlocksAttacks.java:89 (type filter omitted
        // — no damage-type registry).
        struct DamageReduction {
            float horizontalBlockingAngle = 90.0f;
            float base   = 0.0f;
            float factor = 1.0f;
        };
        std::vector<DamageReduction> damageReductions{DamageReduction{}};
        // ItemDamageFunction record — BlocksAttacks.java:106; DEFAULT {1,0,1} (:117).
        struct ItemDamageFunction {
            float threshold = 1.0f;
            float base      = 0.0f;
            float factor    = 1.0f;
        };
        ItemDamageFunction itemDamage{};
        std::string blockSound;     // Optional<Holder<SoundEvent>> → name ("" = none)
        std::string disableSound;

        // BlocksAttacks.blockDelayTicks — :71-73.
        int blockDelayTicks() const {
            return static_cast<int>(std::round(blockDelaySeconds * 20.0f));
        }
    };

}

namespace Game::DataComponents {

    // Explicit override for whether an item shows the enchantment "foil" (purple
    // glint) regardless of whether it has stored enchantments. Mirrors MC
    // DataComponents.java:125. Used by `enchanted_book` (Items.java:2854) so the
    // book glints even when its STORED_ENCHANTMENTS is still EMPTY.
    extern const DataComponentType<bool> ENCHANTMENT_GLINT_OVERRIDE;

    // Enchantments stored on an enchanted_book (NOT the same as ENCHANTMENTS,
    // which lives on the actual tool/armor — DataComponents.java:117). MC
    // DataComponents.java:146. Default for enchanted_book is ItemEnchantments::EMPTY
    // per Items.java:2854.
    extern const DataComponentType<ItemEnchantments> STORED_ENCHANTMENTS;

    // The TOOL component carries the mining-speed table + correct-tool tier.
    // Mirrors MC DataComponents.java:108 + world/item/component/Tool.java.
    // Present on every tool item (pickaxe/axe/shovel/hoe/sword/shears); absent
    // on non-tool items (Item.getDestroySpeed returns 1.0 in that case).
    extern const DataComponentType<Tool> TOOL;

    // Everything edible/drinkable — the eat-timer + animation + sound +
    // on-consume effects. Mirrors DataComponents.CONSUMABLE (the modern
    // eating system; drives the base Item.use dispatch step 1).
    extern const DataComponentType<Consumable> CONSUMABLE;

    // Nutrition/saturation restored when a CONSUMABLE with this component
    // finishes. Mirrors DataComponents.FOOD. Applied by the FoodProperties
    // ConsumableListener (FoodProperties.java:26-34) → ServerPlayer's FoodData.
    extern const DataComponentType<FoodProperties> FOOD;

    // What the stack converts into when used up. Mirrors
    // DataComponents.USE_REMAINDER (applied in
    // ItemStack.applyAfterUseComponentSideEffects, ItemStack.java:332-348).
    extern const DataComponentType<UseRemainder> USE_REMAINDER;

    // Anvil-renamed display name. Mirrors DataComponents.CUSTOM_NAME (a
    // Component in MC; plain string here — MC renders it italic, our font
    // has no italics). Wins over ITEM_NAME in the tooltip name line.
    extern const DataComponentType<std::string> CUSTOM_NAME;

    // Data-driven base name override (potion variants etc.). Mirrors
    // DataComponents.ITEM_NAME. Falls between CUSTOM_NAME and the registry
    // display name.
    extern const DataComponentType<std::string> ITEM_NAME;

    // Tooltip lore lines (dark purple). Mirrors DataComponents.LORE.
    extern const DataComponentType<ItemLore> LORE;

    // Name-line color tier. Mirrors DataComponents.RARITY.
    extern const DataComponentType<Rarity> RARITY;

    // Which slot the item is worn in + right-click auto-equip. Mirrors
    // DataComponents.EQUIPPABLE (base Item.use dispatch step 2, Equippable
    // swap logic Equippable.java:54-83). Also drives the click-handler's
    // per-armor-slot mayPlace.
    extern const DataComponentType<Equippable> EQUIPPABLE;

    // Shield blocking config. Mirrors DataComponents.BLOCKS_ATTACKS (base
    // Item.use dispatch step 3 → startUsingItem; BLOCK use animation; the
    // 72000-tick "infinite" use duration).
    extern const DataComponentType<BlocksAttacks> BLOCKS_ATTACKS;

    // Bundle contents (nested stacks + client-side selection). Mirrors
    // DataComponents.BUNDLE_CONTENTS; behaviour in BundleBehavior.cpp.
    extern const DataComponentType<BundleContents> BUNDLE_CONTENTS;

    // ── TODO: future component types to register, in MC parity order ────────
    // Each one unlocks a chunk of behaviour by populating Item.use() base
    // dispatch (see Item.hpp ItemUseFn doc comment) and other systems.
    //
    //   DAMAGE             (int)        DataComponents.java:81  — current durability used
    //   MAX_DAMAGE         (int)        DataComponents.java:82  — durability cap
    //   TOOL               (Tool)       DataComponents.java:108 — mining speed + correctForDrops
    //   CONSUMABLE         (Consumable) DataComponents.java:128 — food/drink eat-timer + sound + saturation
    //   EQUIPPABLE         (Equippable) DataComponents.java:130 — armor slot + auto-equip on right-click
    //   BLOCKS_ATTACKS     (BlocksAttacks) DataComponents.java:135 — shield blocking config
    //   KINETIC_WEAPON     (KineticWeapon) DataComponents.java:139 — mace wind-up swing
    //   FOOD               (FoodProperties) — older eating system; superseded by CONSUMABLE
    //   CUSTOM_NAME        (Component)  — anvil-renamed items
    //   LORE               (List<Component>) — book lore text
    //   DYED_COLOR         (DyedItemColor) — leather-armor dye
    //   BANNER_PATTERNS    (BannerPatternLayers) — banner / shield patterns
    //
    // None are registered yet because we have no consumers (no food eating,
    // no armor equipping, no anvil renaming). When the consumer lands, add
    // the type AND its struct definition (mirror MC's record verbatim) here
    // and wire defaults onto the right items in ItemRegistry::Initialize.

#if ENABLE_PORTAL_GUN
    // ── Portal-gun per-stack state (custom non-MC components) ───────────────
    // These don't mirror anything in MC's DataComponents.java — they're
    // bespoke to our portal-gun feature. Toggled out via the central
    // feature flag so a "no portal gun" build sees no portal-related
    // component types at all.

    // Which color the gun fires NEXT. 0 = blue, 1 = orange.
    // Right-clicking toggles this on the held stack — see
    // src/common/entity/PortalGunBehavior.cpp::OnGunUseOn. Default 0 (blue
    // first) so a freshly-spawned gun matches Portal-game's "always-blue
    // first" muscle memory.
    extern const DataComponentType<uint8_t>  PORTAL_GUN_NEXT_COLOR;

    // Stable per-stack instance id. Lazily assigned by the server on the
    // gun's first shot via PortalRegistry::AllocId(); zero = unassigned.
    // Used as the key into the server's PortalRegistry map of (gunId →
    // PortalPair) so the gun's blue+orange pair persists across inventory
    // moves, dropping/picking-up, trades. Stack size 1 means we never have
    // to handle splitting (if we did, a child stack would inherit the same
    // id, sharing the pair — probably not what you'd want; so we don't).
    extern const DataComponentType<uint64_t> PORTAL_GUN_INSTANCE_ID;
#endif

} // namespace Game::DataComponents
