// File: src/common/entity/ItemDurability.cpp
//
// Item durability — MC ItemStack.java's isDamageableItem .. hurtAndBreak, the
// durability bar (Item.isBarVisible / getBarWidth / getBarColor), and the
// registration of every item's durability / enchanting defaults from
// GeneratedItemDurability (tools/gen_item_durability.py).
//
// All wear is SERVER work, as in MC (hurtAndBreak is gated on a ServerLevel):
// the item behaviours run on both sides here, and the client's prediction
// simply never wears anything. The server's per-tick container diff sends the
// changed DAMAGE, or the emptied slot, like any other inventory change.
#include "Item.hpp"
#include "IUsePlayer.hpp"
#include "EntityLevel.hpp"
#include "LivingEntity.hpp"
#include "GeneratedItemDurability.hpp"
#include "GeneratedItemList.hpp"
#include "../data/DataComponents.hpp"
#include "../world/block/BlockRegistry.hpp"
#include "../world/enchantment/EnchantmentDefinitions.hpp"
#include "../world/enchantment/EnchantmentValueEffect.hpp"
#include "../world/level/ILevelWrite.hpp"
#include "../sound/SoundEvents.hpp"
#include "../core/JavaRandom.hpp"
#include "../core/Mth.hpp"
#include "../core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    // ── Component reads ─────────────────────────────────────────────────────

    bool IsDamageableItem(const ItemStack& stack) {
        if (stack.IsEmpty()) return false;
        return stack.get(DataComponents::MAX_DAMAGE).has_value()
            && !stack.get(DataComponents::UNBREAKABLE).has_value()
            && stack.get(DataComponents::DAMAGE).has_value();
    }

    bool IsDamaged(const ItemStack& stack) {
        return IsDamageableItem(stack) && GetDamageValue(stack) > 0;
    }

    int GetMaxDamage(const ItemStack& stack) {
        return stack.get(DataComponents::MAX_DAMAGE).value_or(0);
    }

    int GetDamageValue(const ItemStack& stack) {
        return std::clamp(stack.get(DataComponents::DAMAGE).value_or(0), 0, GetMaxDamage(stack));
    }

    void SetDamageValue(ItemStack& stack, int value) {
        const int clamped = std::clamp(value, 0, GetMaxDamage(stack));
        // PatchedDataComponentMap.set: a value equal to the prototype's is no
        // patch at all. Without this a tool repaired back to 0 would carry
        // an explicit DAMAGE 0 and never stack or match a fresh one again.
        const std::optional<int32_t> prototype =
            ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::DAMAGE);
        if (prototype && *prototype == clamped) {
            stack.components.remove(DataComponents::DAMAGE);
        } else {
            stack.components.set(DataComponents::DAMAGE, static_cast<int32_t>(clamped));
        }
    }

    bool IsBrokenItem(const ItemStack& stack) {
        return IsDamageableItem(stack) && GetDamageValue(stack) >= GetMaxDamage(stack);
    }

    bool NextDamageWillBreak(const ItemStack& stack) {
        return IsDamageableItem(stack) && GetDamageValue(stack) >= GetMaxDamage(stack) - 1;
    }

    bool IsValidRepairItem(const ItemStack& item, const ItemStack& repairItem) {
        // ItemStack.isValidRepairItem → Repairable.isValidRepairItem →
        // repairItemStack.is(items).
        if (repairItem.IsEmpty()) return false;
        const std::optional<Repairable> repairable = item.get(DataComponents::REPAIRABLE);
        return repairable && ItemHolderSetContains(repairable->items, repairItem.itemId);
    }

    bool IsEnchantable(const ItemStack& stack) {
        if (stack.IsEmpty() || !stack.get(DataComponents::ENCHANTABLE)) return false;
        // `enchantments != null && enchantments.isEmpty()` — every MC item
        // carries ENCHANTMENTS (EMPTY by default); absent reads as EMPTY.
        const std::optional<ItemEnchantments> enchantments = stack.get(DataComponents::ENCHANTMENTS);
        return !enchantments || enchantments->IsEmpty();
    }

    bool IsEnchanted(const ItemStack& stack) {
        const std::optional<ItemEnchantments> enchantments = stack.get(DataComponents::ENCHANTMENTS);
        return enchantments && !enchantments->IsEmpty();
    }

    uint8_t GetStackRarity(const ItemStack& stack) {
        const Rarity base = stack.get(DataComponents::RARITY).value_or(Rarity::COMMON);
        if (!IsEnchanted(stack)) return static_cast<uint8_t>(base);
        switch (base) {
            case Rarity::COMMON:
            case Rarity::UNCOMMON: return static_cast<uint8_t>(Rarity::RARE);
            case Rarity::RARE:     return static_cast<uint8_t>(Rarity::EPIC);
            default:               return static_cast<uint8_t>(base);
        }
    }

    // ── The durability bar (Item.java isBarVisible / getBarWidth / getBarColor)

    bool IsBarVisible(const ItemStack& stack) {
        return IsDamaged(stack);
    }

    int GetBarWidth(const ItemStack& stack) {
        const int maxDamage = GetMaxDamage(stack);
        if (maxDamage <= 0) return 0;
        // Math.round(13 - damage * 13 / maxDamage), clamped 0..13.
        const float width = 13.0f - static_cast<float>(GetDamageValue(stack)) * 13.0f /
                                    static_cast<float>(maxDamage);
        return std::clamp(static_cast<int>(std::floor(width + 0.5f)), 0, kItemMaxBarWidth);
    }

    uint32_t GetBarColor(const ItemStack& stack) {
        const int maxDamage = GetMaxDamage(stack);
        if (maxDamage <= 0) return 0;
        const float health = std::max(0.0f, (static_cast<float>(maxDamage) -
                                             static_cast<float>(GetDamageValue(stack))) /
                                            static_cast<float>(maxDamage));
        return Mth::HsvToArgb(health / 3.0f, 1.0f, 1.0f) & 0x00FFFFFFu;
    }

    // ── Wear (ItemStack.java hurtAndBreak and friends) ──────────────────────

    int ProcessDurabilityChange(const ItemStack& stack, int amount, JavaRandom& random,
                                bool hasInfiniteMaterials) {
        if (!IsDamageableItem(stack)) return 0;
        if (hasInfiniteMaterials) return 0;
        if (amount <= 0) return amount;
        // EnchantmentHelper.processDurabilityChange: every enchantment on the
        // item (ENCHANTMENTS only — runIterationOnItem) rewrites the amount in
        // turn through its item_damage effects; MutableFloat.intValue
        // truncates.
        float modified = static_cast<float>(amount);
        if (const std::optional<ItemEnchantments> enchantments = stack.get(DataComponents::ENCHANTMENTS)) {
            for (const EnchantmentInstance& e : enchantments->entries) {
                EnchantmentDefinitions::ModifyDurabilityChange(e.id, e.level, stack, random, modified);
            }
        }
        return static_cast<int>(modified);
    }

    namespace {

        // ItemStack.applyDamage.
        void ApplyDamage(ItemStack& stack, int newDamage,
                         const std::function<void(const ItemStack&)>& onBreak) {
            // (CriteriaTriggers.ITEM_DURABILITY_CHANGED — no advancements.)
            SetDamageValue(stack, newDamage);
            if (IsBrokenItem(stack)) {
                const ItemStack broken = stack;
                stack.count -= 1;                    // shrink(1)
                if (stack.count <= 0) stack.Clear();
                if (onBreak) onBreak(broken);
            }
        }

        // A level without a random of its own (never a real server level)
        // still has to roll Unbreaking with something.
        JavaRandom& FallbackRandom() {
            thread_local JavaRandom random(static_cast<int64_t>(0x2F6A1C3B));
            return random;
        }

    } // namespace

    void HurtAndBreak(ItemStack& stack, int amount, JavaRandom& random, bool hasInfiniteMaterials,
                      const std::function<void(const ItemStack& broken)>& onBreak) {
        const int newAmount = ProcessDurabilityChange(stack, amount, random, hasInfiniteMaterials);
        if (newAmount != 0) ApplyDamage(stack, GetDamageValue(stack) + newAmount, onBreak);
    }

    void HurtWithoutBreaking(ItemStack& stack, int amount, JavaRandom& random,
                             bool hasInfiniteMaterials) {
        const int newAmount = ProcessDurabilityChange(stack, amount, random, hasInfiniteMaterials);
        if (newAmount == 0) return;
        const int newDamage = std::min(GetDamageValue(stack) + newAmount, GetMaxDamage(stack) - 1);
        ApplyDamage(stack, newDamage, nullptr);
    }

    void HurtAndBreak(ItemStack& stack, int amount, LivingEntity& owner, EquipmentSlot slot) {
        EntityLevel* level = owner.Level();
        if (!level || level->IsClientSide()) return;   // `owner.level() instanceof ServerLevel`
        // Only a ServerPlayer is passed on (player != null) — its
        // hasInfiniteMaterials is creative.
        const bool infinite = owner.IsPlayer() && owner.IsCreative();
        HurtAndBreak(stack, amount, level->Random(), infinite,
                     [&owner, slot](const ItemStack& broken) { owner.OnEquippedItemBroken(broken, slot); });
    }

    void HurtAndBreak(ItemStack& stack, int amount, ILevelWrite* level, IUsePlayer* player,
                      uint32_t hand) {
        if (!level || level->IsClientSide()) return;
        JavaRandom* random = level->Random();
        const bool infinite = player && player->isCreative();
        const EquipmentSlot slot = hand == 1 ? EquipmentSlot::OFFHAND : EquipmentSlot::MAINHAND;
        const bool wasEmpty = stack.IsEmpty();
        HurtAndBreak(stack, amount, random ? *random : FallbackRandom(), infinite,
                     [player, slot](const ItemStack& broken) {
                         if (player) player->OnEquippedItemBroken(broken, slot);
                     });
        if (player && !wasEmpty) player->markSlotDirty(player->handSlotIndex(hand));
    }

    ItemStack HurtAndConvertOnBreak(ItemStack& stack, int amount, ItemID newItem,
                                    LivingEntity& owner, EquipmentSlot slot) {
        EntityLevel* level = owner.Level();
        if (!level || level->IsClientSide()) return stack;
        std::optional<ItemStack> broken;
        const bool infinite = owner.IsPlayer() && owner.IsCreative();
        HurtAndBreak(stack, amount, level->Random(), infinite,
                     [&](const ItemStack& b) {
                         broken = b;
                         owner.OnEquippedItemBroken(b, slot);
                     });
        if (!stack.IsEmpty() || !broken) return stack;
        // transmuteCopyIgnoreEmpty(newItem, 1): the new item with the broken
        // stack's component patch; a damageable result starts undamaged.
        ItemStack replacement(newItem, 1);
        replacement.components = broken->components;
        if (IsDamageableItem(replacement)) SetDamageValue(replacement, 0);
        return replacement;
    }

    void MineBlock(ItemStack& stack, BlockID block, ILevelWrite* level, IUsePlayer* player) {
        if (stack.IsEmpty()) return;
        const std::optional<Tool> tool = stack.get(DataComponents::TOOL);
        if (!tool) return;
        if (!level || level->IsClientSide()) return;
        if (tool->damagePerBlock <= 0) return;
        if (stack.itemId == Items::Shears) {
            // ShearsItem.mineBlock: anything but #minecraft:fire — shearing
            // through leaves (destroy time 0.2) and even instant plants wears.
            if (block == BlockID::Fire || block == BlockID::SoulFire) return;
        } else if (BlockRegistry::Get(block).destroyTime == 0.0f) {
            // Item.mineBlock: `state.getDestroySpeed(level, pos) != 0.0F`.
            return;
        }
        HurtAndBreak(stack, tool->damagePerBlock, level, player, 0);
    }

    bool HurtEnemy(ItemStack& stack, LivingEntity& attacker) {
        if (stack.IsEmpty()) return false;
        const std::optional<Weapon> weapon = stack.get(DataComponents::WEAPON);
        if (!weapon) return false;
        // postHurtEnemy (the caller only reaches it for a landed hit).
        HurtAndBreak(stack, weapon->itemDamagePerAttack, attacker, EquipmentSlot::MAINHAND);
        return true;
    }

    uint8_t EntityEventForEquipmentBreak(EquipmentSlot slot) {
        switch (slot) {
            case EquipmentSlot::MAINHAND: return 47;
            case EquipmentSlot::OFFHAND:  return 48;
            case EquipmentSlot::HEAD:     return 49;
            case EquipmentSlot::CHEST:    return 50;
            case EquipmentSlot::LEGS:     return 51;
            case EquipmentSlot::FEET:     return 52;
            case EquipmentSlot::BODY:     return 65;
            case EquipmentSlot::SADDLE:   return 68;
        }
        return 47;
    }

    bool EquipmentSlotForBreakEvent(uint8_t event, EquipmentSlot& out) {
        switch (event) {
            case 47: out = EquipmentSlot::MAINHAND; return true;
            case 48: out = EquipmentSlot::OFFHAND;  return true;
            case 49: out = EquipmentSlot::HEAD;     return true;
            case 50: out = EquipmentSlot::CHEST;    return true;
            case 51: out = EquipmentSlot::LEGS;     return true;
            case 52: out = EquipmentSlot::FEET;     return true;
            case 65: out = EquipmentSlot::BODY;     return true;
            case 68: out = EquipmentSlot::SADDLE;   return true;
            default: return false;
        }
    }

    std::string GetBreakSound(const ItemStack& stack) {
        if (std::optional<std::string> sound = stack.get(DataComponents::BREAK_SOUND)) return *sound;
        return SoundEvents::ITEM_BREAK;
    }

    // ── Registration ────────────────────────────────────────────────────────

    // Called from ItemRegistry::Initialize after ItemRegistry_RegisterBehaviors
    // (it adjusts the TOOL components that pass writes). Mirrors the
    // Item.Properties calls Items.java makes: durability(n) → MAX_DAMAGE n,
    // DAMAGE 0, MAX_STACK_SIZE 1; enchantable(n); repairable(...); the WEAPON
    // and TOOL damage_per_block a tool material builds; BREAK_SOUND;
    // fireResistant() → DAMAGE_RESISTANT.
    void ItemRegistry_RegisterDurability(std::unordered_map<ItemID, Item>& pureItems) {
        std::unordered_map<std::string_view, ItemID> bySlug;
        bySlug.reserve(kPureItemTableSize);
        for (size_t i = 0; i < kPureItemTableSize; ++i) {
            if (kPureItemTable[i].slug) bySlug.emplace(kPureItemTable[i].slug, PURE_ITEM_BASE + static_cast<ItemID>(i));
        }

        size_t durable = 0, unresolved = 0;
        for (const ItemDurabilityRow& row : kItemDurability) {
            const auto idIt = bySlug.find(row.slug);
            auto it = idIt != bySlug.end() ? pureItems.find(idIt->second) : pureItems.end();
            if (it == pureItems.end()) { ++unresolved; continue; }
            Item& item = it->second;
            DataComponentMap& c = item.defaultComponents;

            if (row.maxDamage > 0) {
                c.set(DataComponents::MAX_DAMAGE, row.maxDamage);
                c.set(DataComponents::DAMAGE, int32_t{0});
                item.maxStackSize = 1;   // "Item cannot have both durability and be stackable"
                ++durable;
            }
            if (row.enchantable > 0) c.set(DataComponents::ENCHANTABLE, row.enchantable);
            if (!row.repairable.empty()) {
                c.set(DataComponents::REPAIRABLE, Repairable{{std::string(row.repairable)}});
            }
            if (row.weaponDamagePerAttack >= 0) {
                c.set(DataComponents::WEAPON, Weapon{row.weaponDamagePerAttack, row.disableBlockingSeconds});
            }
            if (row.toolDamagePerBlock >= 0) {
                // The sword's / mace's / trident's TOOL. The mace and trident
                // have no mining rules (their createToolProperties is an empty
                // rule list at speed 1.0), which a type-None tool at 1.0 is.
                Tool tool = c.get(DataComponents::TOOL).value_or(Tool{});
                tool.damagePerBlock = row.toolDamagePerBlock;
                c.set(DataComponents::TOOL, tool);
            }
            if (!row.breakSound.empty()) {
                c.set(DataComponents::BREAK_SOUND, std::string(row.breakSound));
            }
            if (!row.damageResistant.empty()) {
                c.set(DataComponents::DAMAGE_RESISTANT, std::string(row.damageResistant));
            }
        }
        Log::Info("[ItemRegistry] durability defaults: %zu durable items from %d rows (%zu unresolved)",
                  durable, kItemDurabilityCount, unresolved);
    }

} // namespace Game
