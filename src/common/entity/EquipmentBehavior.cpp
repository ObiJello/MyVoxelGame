// File: src/common/entity/EquipmentBehavior.cpp
// See header. Also hosts the EQUIPPABLE / BLOCKS_ATTACKS registration table
// (armor tiers, elytra, shield) — values verbatim from Items.java +
// ArmorMaterials.java equip sounds.
#include "EquipmentBehavior.hpp"

#include "GeneratedItemList.hpp"
#include "Inventory.hpp"
#include "../core/Log.hpp"
#include "../world/level/WorldDrops.hpp"
#include "../world/enchantment/EnchantmentHelper.hpp"
#include "server/player/ServerPlayer.hpp"

#include <unordered_map>

namespace Game::EquipmentBehavior {

    // Mirrors Equippable.swapWithEquipmentSlot — Equippable.java:54-83.
    UseResult SwapWithEquipmentSlot(Server::ServerPlayer& player, uint32_t hand,
                                    const Equippable& equippable) {
        // :55 canUseSlot + canBeEquippedBy — players can use every wearable
        // slot we model; BODY/SADDLE (animal-only) → PASS (:80-81).
        const int slotIdx = InventoryIndexFor(equippable.slot);
        if (slotIdx < 0) return UseResult::Pass;

        auto& inv = player.getInventory();
        Game::ItemStack& inHand = player.getItemInHand(hand);
        const Game::ItemStack inEquipmentSlot = inv.GetSlot(slotIdx);  // value copy
        const bool creative = player.getGameMode() == Server::GameMode::CREATIVE;

        // :57 — a worn piece carrying prevent_armor_change (Curse of
        // Binding) stays on outside creative; isSameItemSameComponents
        // approximated by item id (no component equality op) → same item
        // already worn = FAIL.
        if (!creative && Game::EnchantmentHelper::HasPreventArmorChange(inEquipmentSlot)) {
            return UseResult::Fail;
        }
        if (!inEquipmentSlot.IsEmpty() && inEquipmentSlot.itemId == inHand.itemId) {
            return UseResult::Fail;
        }

        // The equip sound is not played here: MC plays it from
        // LivingEntity.onEquipItem whatever filled the slot, and so does
        // this engine — ServerPlayer::tick notices the new armor piece and
        // sounds its Equippable.equipSound for everyone.

        if (inHand.count <= 1) {
            // :62-66 — single item: straight swap.
            //   swappedToHand      = slot empty ? inHand(cleared) : old equipment
            //   swappedToEquipment = creative ? copy (hand keeps it) : the item
            Game::ItemStack swappedToEquipment = inHand;   // copy (count 1)
            if (creative) {
                // Creative keeps the hand copy UNLESS displaced equipment
                // replaces it (heldItemTransformedTo(old equipment)).
                if (!inEquipmentSlot.IsEmpty()) {
                    player.setItemInHand(hand, inEquipmentSlot);
                }
            } else {
                // Survival: hand receives the old equipment (or empties).
                player.setItemInHand(hand, inEquipmentSlot.IsEmpty()
                                               ? Game::ItemStack{}
                                               : inEquipmentSlot);
            }
            inv.SetSlotFull(slotIdx, swappedToEquipment);
            player.markSlotDirty(slotIdx);
            return UseResult::Success;
        }

        // :67-76 — stacked equippables (carved pumpkin-style): move ONE into
        // the slot, displaced equipment goes to the inventory (drop-stub on
        // overflow), hand shrinks by one (creative keeps the count —
        // consumeAndReturn respects hasInfiniteMaterials).
        Game::ItemStack swappedToEquipment = inHand;
        swappedToEquipment.count = 1;
        if (!creative) {
            inHand.count -= 1;
            if (inHand.count <= 0) inHand.Clear();
            player.markSlotDirty(player.handSlotIndex(hand));
        }
        inv.SetSlotFull(slotIdx, swappedToEquipment);
        player.markSlotDirty(slotIdx);

        if (!inEquipmentSlot.IsEmpty()) {
            // :71-73 — displaced equipment into the inventory. Placed into
            // the first empty main/hotbar slot via SetSlotFull so per-stack
            // components survive (Inventory::AddItems merges by bare id).
            bool placed = false;
            const int regions[2][2] = {
                { Inventory::MAIN_BEGIN,   Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE     },
                { Inventory::HOTBAR_BEGIN, Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE },
            };
            for (auto& range : regions) {
                for (int i = range[0]; i < range[1] && !placed; ++i) {
                    if (inv.GetSlot(i).IsEmpty()) {
                        inv.SetSlotFull(i, inEquipmentSlot);
                        player.markSlotDirty(i);
                        placed = true;
                    }
                }
                if (placed) break;
            }
            if (!placed) {
                // Inventory full — the armour that just came off goes on the
                // ground rather than being destroyed. Swapping helmets with a
                // full pack used to silently eat the old one.
                DropItemStackNear(DimensionFromRaw(player.getDimensionId()),
                                  glm::ivec3(glm::floor(player.getPosition())),
                                  inEquipmentSlot);
            }
        }
        return UseResult::Success;
    }

} // namespace Game::EquipmentBehavior

namespace Game {

    // EQUIPPABLE / BLOCKS_ATTACKS defaults — called from
    // ItemRegistry_RegisterBehaviors. Rows cite Items.java; equip sounds from
    // ArmorMaterials.java. All armor stacksTo(1) (Item.Properties.humanoidArmor
    // → durability → stacksTo(1)).
    void ItemRegistry_RegisterEquipment(std::unordered_map<ItemID, Item>& pureItems) {
        using DataComponents::EQUIPPABLE;
        using DataComponents::BLOCKS_ATTACKS;

        auto SetArmor = [&](ItemID id, EquipmentSlot slot, const char* sound) {
            auto it = pureItems.find(id);
            if (it == pureItems.end()) return;
            it->second.defaultComponents.set(EQUIPPABLE, Equippable{slot, sound, true});
            it->second.maxStackSize = 1;
        };
        auto Tier = [&](ItemID helmet, ItemID chest, ItemID legs, ItemID boots,
                        const char* sound) {
            SetArmor(helmet, EquipmentSlot::HEAD,  sound);
            SetArmor(chest,  EquipmentSlot::CHEST, sound);
            SetArmor(legs,   EquipmentSlot::LEGS,  sound);
            SetArmor(boots,  EquipmentSlot::FEET,  sound);
        };

        // ArmorMaterials.java equip sounds per tier.
        Tier(Items::LeatherHelmet,   Items::LeatherChestplate,   Items::LeatherLeggings,   Items::LeatherBoots,   "item.armor.equip_leather");
        Tier(Items::ChainmailHelmet, Items::ChainmailChestplate, Items::ChainmailLeggings, Items::ChainmailBoots, "item.armor.equip_chain");
        Tier(Items::IronHelmet,      Items::IronChestplate,      Items::IronLeggings,      Items::IronBoots,      "item.armor.equip_iron");
        Tier(Items::CopperHelmet,    Items::CopperChestplate,    Items::CopperLeggings,    Items::CopperBoots,    "item.armor.equip_copper");
        Tier(Items::GoldenHelmet,    Items::GoldenChestplate,    Items::GoldenLeggings,    Items::GoldenBoots,    "item.armor.equip_gold");
        Tier(Items::DiamondHelmet,   Items::DiamondChestplate,   Items::DiamondLeggings,   Items::DiamondBoots,   "item.armor.equip_diamond");
        Tier(Items::NetheriteHelmet, Items::NetheriteChestplate, Items::NetheriteLeggings, Items::NetheriteBoots, "item.armor.equip_netherite");
        SetArmor(Items::TurtleHelmet, EquipmentSlot::HEAD, "item.armor.equip_turtle");

        // The Aether (AetherArmorMaterials: aether's own equip sounds) and
        // Twilight Forest (TFArmorMaterials: ARMOR_EQUIP_GENERIC except
        // knightmetal's TFSounds.KNIGHTMETAL_EQUIP), docs/mod-ports.md.
        Tier(Items::ZaniteHelmet,      Items::ZaniteChestplate,      Items::ZaniteLeggings,      Items::ZaniteBoots,      "aether:item.armor.equip_zanite");
        Tier(Items::GravititeHelmet,   Items::GravititeChestplate,   Items::GravititeLeggings,   Items::GravititeBoots,   "aether:item.armor.equip_gravitite");
        Tier(Items::IronwoodHelmet,    Items::IronwoodChestplate,    Items::IronwoodLeggings,    Items::IronwoodBoots,    "item.armor.equip_generic");
        Tier(Items::SteeleafHelmet,    Items::SteeleafChestplate,    Items::SteeleafLeggings,    Items::SteeleafBoots,    "item.armor.equip_generic");
        Tier(Items::KnightmetalHelmet, Items::KnightmetalChestplate, Items::KnightmetalLeggings, Items::KnightmetalBoots, "twilightforest:item.knightmetal.equip");
        Tier(Items::FieryHelmet,       Items::FieryChestplate,       Items::FieryLeggings,       Items::FieryBoots,       "item.armor.equip_generic");
        SetArmor(Items::NagaChestplate, EquipmentSlot::CHEST, "item.armor.equip_generic");
        SetArmor(Items::NagaLeggings,   EquipmentSlot::LEGS,  "item.armor.equip_generic");

        // Elytra — CHEST slot, equip_elytra (Items.java elytra row). Data-only:
        // no gliding system, but it equips/renders in the chest slot.
        // `.setDamageOnHurt(false)`: the elytra wears only while gliding, never
        // from the hits that wear armour (LivingEntity.doHurtEquipment).
        SetArmor(Items::Elytra, EquipmentSlot::CHEST, "item.armor.equip_elytra");
        if (auto it = pureItems.find(Items::Elytra); it != pureItems.end()) {
            Equippable elytra = *it->second.defaultComponents.get(EQUIPPABLE);
            elytra.damageOnHurt = false;
            it->second.defaultComponents.set(EQUIPPABLE, elytra);
        }

        // The Hush's cloak of silence (docs/the-hush.md) — a CHEST wearable
        // like the elytra: no armour value, the equip sound of leather. What
        // it does is read where the listeners pick players
        // (HushItems::IsSoundCloaked).
        SetArmor(Items::CloakOfSilence, EquipmentSlot::CHEST, "item.armor.equip_leather");

        // Shield — Items.java shield row:
        //   .component(DataComponents.EQUIPPABLE, Equippable.builder(OFFHAND)
        //              .setEquipSound(ARMOR_EQUIP_GENERIC).setSwappable(false)…)
        //   .component(DataComponents.BLOCKS_ATTACKS, new BlocksAttacks(0.25F,
        //              1.0F, List.of(new DamageReduction(90.0F, empty, 0.0F,
        //              1.0F)), new ItemDamageFunction(3.0F, 1.0F, 1.0F),
        //              Optional.of(DamageTypeTags.BYPASSES_SHIELD),
        //              SHIELD_BLOCK, SHIELD_BREAK)
        //   .stacksTo(1)
        if (auto it = pureItems.find(Items::Shield); it != pureItems.end()) {
            it->second.defaultComponents.set(
                EQUIPPABLE,
                Equippable{EquipmentSlot::OFFHAND, "item.armor.equip_generic",
                           /*swappable=*/false});
            BlocksAttacks blocks;
            blocks.blockDelaySeconds    = 0.25f;
            blocks.disableCooldownScale = 1.0f;
            blocks.damageReductions     = {BlocksAttacks::DamageReduction{90.0f, 0.0f, 1.0f}};
            blocks.itemDamage           = BlocksAttacks::ItemDamageFunction{3.0f, 1.0f, 1.0f};
            blocks.blockSound           = "item.shield.block";
            blocks.disableSound         = "item.shield.break";
            it->second.defaultComponents.set(BLOCKS_ATTACKS, blocks);
            it->second.maxStackSize = 1;
        }

        Log::Info("[ItemRegistry] Registered EQUIPPABLE on 57 armor items + shield BLOCKS_ATTACKS");
    }

} // namespace Game
