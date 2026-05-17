// File: src/common/data/DataComponents.hpp
//
// Mirrors net/minecraft/core/component/DataComponents.java — the registry of
// every DataComponentType. Components are added here as features need them;
// the eventual MC-parity list is ~50 entries.
//
// Source line numbers cited per declaration so the C++ side can be diffed
// against MC's source.
#pragma once

#include "DataComponentType.hpp"
#include "../world/enchantment/ItemEnchantments.hpp"
#include "../core/Features.hpp"

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
