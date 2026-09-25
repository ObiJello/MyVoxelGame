// File: src/common/world/enchantment/EnchantmentHelper.hpp
//
// Mirrors net/minecraft/world/item/enchantment/EnchantmentHelper.java — utility
// functions for setting/reading enchantments on ItemStacks, plus the random
// roll the enchanting table and loot tables share (selectEnchantment /
// enchantItem). The per-enchantment data those rolls read (weight, cost
// curves, supported items, exclusive sets) comes from EnchantmentDefinitions.
//
// Reference points in MC source:
//   - setEnchantments               — line 75
//   - getComponentType              — line 83
//   - processDurabilityChange       — line 91 (ProcessDurabilityChange, Item.hpp)
//   - createBook                    — line 118
//   - enchantItem                   — line 556 / 564
//   - selectEnchantment             — line 580
//   - getAvailableEnchantmentResults — line 632
//   - filterCompatibleEnchantments  — line 611
#pragma once

#include "EnchantmentEffects.hpp"
#include "EnchantmentInstance.hpp"
#include "ItemEnchantments.hpp"
#include "common/entity/Item.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace Game {

    struct ItemStack;
    class JavaRandom;
    class Entity;
    class LivingEntity;
    class Inventory;
    struct EntityLevel;
    struct DamageSourceInfo;
    struct MobEffectInstance;

    // MC LivingEntity.activeLocationDependentEnchantments: per worn slot, the
    // location_changed effects of each enchantment that are currently on
    // (EnchantmentLocationBasedEffect identity = its index in the
    // enchantment's list). Owned by whatever runs the location effects — the
    // player's server view, and the client's own player for the movement
    // attributes it applies locally.
    struct ActiveLocationEnchantments {
        struct Entry {
            EquipmentSlot slot;
            EnchantmentId id;
            int           level;
            uint32_t      activeMask;   // bit i = locationChanged[i] is active
        };
        std::vector<Entry> entries;
    };

    // What the equipment runners need to know about the entity whose slots
    // they walk (MC's LivingEntity, reduced to the parts the runners touch).
    struct EnchantmentEquipment {
        // MC getItemBySlot; null = no such slot.
        std::function<ItemStack*(EquipmentSlot)> itemBySlot;
        // The owner effects run on and are credited to; null on the client.
        LivingEntity* owner = nullptr;
        // Where attribute modifiers from location effects land.
        AttributeMap* attributes = nullptr;
        // THIS_ENTITY's facts when there is no owner entity (the client).
        const EnchantmentEntityFacts* facts = nullptr;

        // The runner view of a LivingEntity (its EquipmentInSlot).
        static EnchantmentEquipment Of(LivingEntity& living);
        // A player's inventory, MAINHAND being the selected hotbar slot.
        static EnchantmentEquipment OfInventory(Inventory& inventory);
    };

    namespace EnchantmentHelper {

        // Mirrors EnchantmentHelper.setEnchantments — overwrites the stack's
        // enchantment component: STORED_ENCHANTMENTS on an enchanted_book,
        // ENCHANTMENTS on everything else (getComponentType). An EMPTY value
        // on an item whose default is EMPTY drops the stack's override, as
        // MC's patch map does.
        void SetEnchantments(ItemStack& stack, ItemEnchantments enchantments);

        // MC EnchantmentHelper.canStoreEnchantments: the stack carries the
        // component getComponentType names. Every MC item has ENCHANTMENTS
        // (COMMON_ITEM_COMPONENTS), so that is any non-empty stack but an
        // enchanted book, which needs its STORED_ENCHANTMENTS default.
        bool CanStoreEnchantments(const ItemStack& stack);

        // MC EnchantmentHelper.getEnchantmentsForCrafting: the value of that
        // same component, EMPTY when absent — what an anvil or grindstone
        // reads, book or not.
        ItemEnchantments GetEnchantmentsForCrafting(const ItemStack& stack);

        // MC EnchantmentHelper.getItemEnchantmentLevel: the level of `id` in
        // the stack's ENCHANTMENTS (never STORED), 0 when absent.
        int GetItemEnchantmentLevel(EnchantmentId id, const ItemStack& stack);

        // MC EnchantmentHelper.hasAnyEnchantments: ENCHANTMENTS or
        // STORED_ENCHANTMENTS non-empty.
        bool HasAnyEnchantments(const ItemStack& stack);

        // MC EnchantmentHelper.updateEnchantments: edit the stack's
        // enchantment component (getComponentType) in place; a stack without
        // one is left alone.
        void UpdateEnchantments(ItemStack& stack, const std::function<void(ItemEnchantments&)>& edit);

        // Mirrors EnchantmentHelper.java:118 — produce a fresh enchanted_book
        // ItemStack carrying exactly one enchantment at the given level.
        // Used by the inventory's "all enchantment book variants" search.
        ItemStack CreateBook(EnchantmentInstance inst);

        // MC ItemStack.enchant(enchantment, level): adds (or raises —
        // ItemEnchantments.Mutable.upgrade) one enchantment on the stack, in
        // STORED_ENCHANTMENTS on an enchanted book and ENCHANTMENTS on
        // anything else.
        void Enchant(ItemStack& stack, EnchantmentId id, int level);

        // The stack's ENCHANTABLE value (Items.java `enchantable(n)`, the
        // tool / armour materials' enchantment value — DataComponents
        // defaults from GeneratedItemDurability). 0 = not enchantable, which
        // makes selectEnchantment return nothing.
        int Enchantability(const ItemStack& stack);

        // MC EnchantmentHelper.selectEnchantment: the weighted roll at
        // `enchantmentCost` levels over `candidates` (a HolderSet — the
        // whole registry when a caller has no options).
        std::vector<EnchantmentInstance> SelectEnchantment(JavaRandom& random, const ItemStack& stack,
                                                           int enchantmentCost,
                                                           const std::vector<EnchantmentId>& candidates);

        // MC EnchantmentHelper.enchantItem: selectEnchantment, a plain book
        // becomes an enchanted book, then every result is applied.
        ItemStack EnchantItem(JavaRandom& random, ItemStack stack, int enchantmentCost,
                              const std::vector<EnchantmentId>& candidates);

        // ── Effect runners (EnchantmentHelper.java:84-440) ────────────────
        //
        // Each walks the ENCHANTMENTS component of an item (never STORED) or
        // of every item an entity wears, and runs the matching component of
        // each enchantment's definition. The *level* argument is the SERVER
        // level: every runner but the attribute fold is server-only in MC
        // (ServerLevel parameters), and passing a client level makes the
        // effect-applying ones no-ops.

        // MC EnchantmentHelper.has(item, component) for the two unit
        // components: prevent_equipment_drop (Curse of Vanishing) and
        // prevent_armor_change (Curse of Binding).
        bool HasPreventEquipmentDrop(const ItemStack& item);
        bool HasPreventArmorChange(const ItemStack& item);

        // MC getEnchantmentLevel(enchantment, entity): the best level of
        // `id` among the items in the enchantment's own slots.
        int GetEnchantmentLevel(EnchantmentId id, const EnchantmentEquipment& equipment);

        // MC processBlockExperience — block_experience over the tool (Silk
        // Touch's set 0). `random` is the server level's.
        int ProcessBlockExperience(const ItemStack& tool, int amount, JavaRandom& random);

        // MC processMobExperience — mob_experience over the killer's
        // equipment (a living killer only; unchanged otherwise).
        int ProcessMobExperience(EntityLevel& level, Entity* killer, Entity& killed, int amount);

        // MC processAmmoUse — ammo_use over the weapon, tested against the
        // AMMO (Infinity's match_tool arrow). 0 = the shot costs nothing.
        int ProcessAmmoUse(EntityLevel& level, const ItemStack& weapon, const ItemStack& ammo, int amount);

        // MC processProjectileCount / processProjectileSpread /
        // getPiercingCount — the multishot and piercing numbers a launcher
        // hands its projectiles.
        int   ProcessProjectileCount(EntityLevel& level, const ItemStack& weapon, Entity& shooter, int count);
        float ProcessProjectileSpread(EntityLevel& level, const ItemStack& weapon, Entity& shooter, float angle);
        int   GetPiercingCount(EntityLevel& level, const ItemStack& weapon, const ItemStack& ammo);

        // MC isImmuneToDamage — any worn enchantment's damage_immunity
        // matching the source (Frost Walker vs #burn_from_stepping).
        bool IsImmuneToDamage(EntityLevel& level, LivingEntity& victim, const DamageSourceInfo& source,
                              const EnchantmentEquipment& equipment);

        // MC getDamageProtection — the summed damage_protection of every
        // worn enchantment (the Protection family, Feather Falling), which
        // CombatRules.getDamageAfterMagicAbsorb then caps at 20.
        float GetDamageProtection(EntityLevel& level, LivingEntity& victim, const DamageSourceInfo& source,
                                  const EnchantmentEquipment& equipment);

        // MC modifyDamage / modifyFallBasedDamage / modifyArmorEffectiveness
        // / modifyKnockback — the weapon's damage-context value components
        // (Sharpness/Smite/Bane/Impaling/Power, Density, Breach,
        // Knockback/Punch).
        float ModifyDamage(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                           const DamageSourceInfo& source, float damage);
        float ModifyFallBasedDamage(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                                    const DamageSourceInfo& source, float damage);
        float ModifyArmorEffectiveness(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                                       const DamageSourceInfo& source, float armorFraction);
        float ModifyKnockback(EntityLevel& level, const ItemStack& weapon, Entity& victim,
                              const DamageSourceInfo& source, float knockback);

        // MC doPostAttackEffects / doPostAttackEffectsWithItemSource: the
        // victim's worn post_attack effects enchanted=VICTIM (Thorns), then
        // `weapon`'s enchanted=ATTACKER ones (Fire Aspect, Bane's slowness,
        // Channeling, Wind Burst). The first form uses the causing entity's
        // weapon (getWeaponItem).
        void DoPostAttackEffects(EntityLevel& level, Entity& victim, const DamageSourceInfo& source);
        void DoPostAttackEffectsWithItemSource(EntityLevel& level, Entity& victim,
                                               const DamageSourceInfo& source, ItemStack* weapon);

        // MC onProjectileSpawned — projectile_spawned over the launcher
        // (Flame). `onBreak` receives the launcher if an effect wears it out.
        void OnProjectileSpawned(EntityLevel& level, ItemStack& weapon, Entity& projectile,
                                 LivingEntity* owner,
                                 const std::function<void(const ItemStack&)>& onBreak);

        // MC onHitBlock — hit_block over a projectile's launcher (Channeling
        // on a lightning rod).
        void OnHitBlock(EntityLevel& level, ItemStack& weapon, LivingEntity* owner, Entity& projectile,
                        const glm::dvec3& hitLocation, BlockState hitBlock,
                        const std::function<void(const ItemStack&)>& onBreak);

        // MC tickEffects — every worn enchantment's `tick` effects.
        void TickEffects(EntityLevel& level, const EnchantmentEquipment& equipment);

        // MC runLocationChangedEffects(level, entity) — the location_changed
        // effects of everything worn, switching each on or off as its
        // conditions now read (Frost Walker's disk, Soul Speed's boost and
        // wear). `level` null runs only the attribute half (the client).
        void RunLocationChangedEffects(EntityLevel* level, const IBlockAccess* blocks, JavaRandom& random,
                                       const EnchantmentEquipment& equipment,
                                       ActiveLocationEnchantments& state);
        // MC stopLocationBasedEffects(entity): everything active goes off.
        void StopLocationBasedEffects(const EnchantmentEquipment& equipment,
                                      ActiveLocationEnchantments& state);
        // The one-slot pair collectEquipmentChanges runs when a slot's item
        // changes: stopLocationBasedEffects(previous, slot) then
        // runLocationChangedEffects(level, current, entity, slot).
        void StopLocationBasedEffectsInSlot(const EnchantmentEquipment& equipment,
                                            ActiveLocationEnchantments& state, EquipmentSlot slot);
        void RunLocationChangedEffectsInSlot(EntityLevel* level, const IBlockAccess* blocks, JavaRandom& random,
                                             const EnchantmentEquipment& equipment,
                                             ActiveLocationEnchantments& state, EquipmentSlot slot);

        // MC modifyDurabilityToRepairFromXp — repair_with_xp over the item
        // (Mending's ×2 points of durability per XP point).
        int ModifyDurabilityToRepairFromXp(const ItemStack& item, int durability, JavaRandom& random);

        // MC getRandomItemWith(REPAIR_WITH_XP, entity, ItemStack::isDamaged):
        // one uniformly chosen worn/held damaged item carrying the
        // component in a slot its enchantment works in; null when none.
        ItemStack* GetRandomDamagedItemWithRepairWithXp(const EnchantmentEquipment& equipment,
                                                        JavaRandom& random);

        // MC forEachModifier(itemStack, slot, consumer): the attribute
        // modifiers the item's enchantments grant while worn in `slot`.
        void ForEachModifier(const ItemStack& item, EquipmentSlot slot,
                             const std::function<void(Attribute, const AttributeModifier&)>& consumer);

        // A player attribute as its client and ServerPlayer (which keep no
        // AttributeMap) read it: `base`, the worn items' enchantment
        // modifiers (collectEquipmentChanges' forEachModifier), the
        // location-effect modifiers already folded into `locationModifiers`
        // (may be null), and the effect templates — MC's one fold.
        double PlayerAttributeValue(Attribute attribute, double base, Inventory& inventory,
                                    const std::vector<MobEffectInstance>& effects,
                                    const AttributeMap* locationModifiers = nullptr);

    } // namespace EnchantmentHelper
} // namespace Game
