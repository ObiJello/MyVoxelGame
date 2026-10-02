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
#include "Mob.hpp"
#include "../sound/SoundEvents.hpp"
#include "../world/block/Blocks.hpp"
#include "server/player/ServerPlayer.hpp"

#include <string>
#include <unordered_map>
#include <vector>

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
    void ItemRegistry_RegisterMountEquipment(std::unordered_map<ItemID, Item>& pureItems);

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

        // Elytra — CHEST slot, equip_elytra (Items.java elytra row). Its
        // GLIDER is the item itself: the glide is PlayerPhysics' fall-flying
        // (client) and ServerPlayer::canGlide / the wear tick (server).
        // `.setDamageOnHurt(false)`: the elytra wears only while gliding, never
        // from the hits that wear armour (LivingEntity.doHurtEquipment).
        SetArmor(Items::Elytra, EquipmentSlot::CHEST, "item.armor.equip_elytra");
        if (auto it = pureItems.find(Items::Elytra); it != pureItems.end()) {
            Equippable elytra = *it->second.defaultComponents.get(EQUIPPABLE);
            elytra.damageOnHurt = false;
            elytra.assetId = "minecraft:elytra";   // EquipmentAssets.ELYTRA
            it->second.defaultComponents.set(EQUIPPABLE, elytra);
            // Items.java ELYTRA .component(DataComponents.GLIDER, Unit.INSTANCE).
            it->second.defaultComponents.set(DataComponents::GLIDER, true);
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
            blocks.bypassedBy           = {"#minecraft:bypasses_shield"};
            blocks.blockSound           = "item.shield.block";
            blocks.disableSound         = "item.shield.break";
            it->second.defaultComponents.set(BLOCKS_ATTACKS, blocks);
            it->second.maxStackSize = 1;
        }

        Log::Info("[ItemRegistry] Registered EQUIPPABLE on 57 armor items + shield BLOCKS_ATTACKS");

        ItemRegistry_RegisterMountEquipment(pureItems);
    }

    namespace {

        // DyeColor order (DyeColor.java) — ColorCollection registers the
        // sixteen harnesses and carpets in it.
        constexpr const char* kDyeColorNames[16] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
        };

        // MC ItemStack.interactLivingEntity's component half: an EQUIPPABLE
        // with equipOnInteract goes through Equippable.equipOnTarget before
        // the item's own interactLivingEntity (none of these items has one).
        // The saddle, the harnesses and nautilus armour carry it; the mob
        // classes reach it from their mobInteract exactly where MC calls
        // itemStack.interactLivingEntity (AbstractHorse, Camel, the nautili)
        // and the interaction dispatch after a PASS (the pig, the strider,
        // the happy ghast).
        UseResult InteractEntity_EquipOnInteract(ItemStack& stack, LivingEntity& target) {
            const auto equippable = stack.get(DataComponents::EQUIPPABLE);
            if (!equippable || !equippable->equipOnInteract) return UseResult::Pass;
            auto* mob = dynamic_cast<Mob*>(&target);
            if (!mob) return UseResult::Pass;
            return mob->EquipOnTarget(*equippable, stack);
        }

    } // namespace

    // The mount equipment of Items.java 26.3 — Equippable.saddle(),
    // Equippable.harness(color), Item.Properties.horseArmor /
    // nautilusArmor (their BODY attribute rows are GeneratedItemAttributes'),
    // with every field as the builders set it.
    void ItemRegistry_RegisterMountEquipment(std::unordered_map<ItemID, Item>& pureItems) {
        using DataComponents::EQUIPPABLE;
        int registered = 0;
        auto set = [&](ItemID id, const Equippable& equippable) {
            auto it = pureItems.find(id);
            if (it == pureItems.end()) return;
            it->second.defaultComponents.set(EQUIPPABLE, equippable);
            it->second.maxStackSize = 1;   // .stacksTo(1)
            if (equippable.equipOnInteract) it->second.interactLivingEntity = &InteractEntity_EquipOnInteract;
            ++registered;
        };

        // Equippable.saddle(): SADDLE, HORSE_SADDLE, asset "saddle",
        // #can_equip_saddle, equipOnInteract, canBeSheared with
        // SADDLE_UNEQUIP.
        {
            Equippable saddle;
            saddle.slot            = EquipmentSlot::SADDLE;
            saddle.equipSound      = SoundEvents::HORSE_SADDLE;
            saddle.assetId         = "saddle";
            saddle.allowedEntities = {"#minecraft:can_equip_saddle"};
            saddle.equipOnInteract = true;
            saddle.canBeSheared    = true;
            saddle.shearingSound   = SoundEvents::SADDLE_UNEQUIP;
            set(Items::Saddle, saddle);
        }

        // Equippable.harness(color): BODY, HARNESS_EQUIP, asset
        // "<color>_harness", #can_equip_harness, equipOnInteract,
        // canBeSheared with HARNESS_UNEQUIP.
        {
            static constexpr ItemID kHarnesses[16] = {
                Items::WhiteHarness, Items::OrangeHarness, Items::MagentaHarness, Items::LightBlueHarness,
                Items::YellowHarness, Items::LimeHarness, Items::PinkHarness, Items::GrayHarness,
                Items::LightGrayHarness, Items::CyanHarness, Items::PurpleHarness, Items::BlueHarness,
                Items::BrownHarness, Items::GreenHarness, Items::RedHarness, Items::BlackHarness,
            };
            for (int color = 0; color < 16; ++color) {
                Equippable harness;
                harness.slot            = EquipmentSlot::BODY;
                harness.equipSound      = SoundEvents::HARNESS_EQUIP;
                harness.assetId         = std::string(kDyeColorNames[color]) + "_harness";
                harness.allowedEntities = {"#minecraft:can_equip_harness"};
                harness.equipOnInteract = true;
                harness.canBeSheared    = true;
                harness.shearingSound   = SoundEvents::HARNESS_UNEQUIP;
                set(kHarnesses[color], harness);
            }
        }

        // Item.Properties.horseArmor(material): BODY, HORSE_ARMOR, the
        // material's asset, #can_wear_horse_armor, no wear on hurt,
        // canBeSheared with HORSE_ARMOR_UNEQUIP (not equip-on-interact —
        // AbstractHorse.mobInteract equips it itself).
        auto horseArmor = [&](ItemID id, const char* asset) {
            Equippable armor;
            armor.slot            = EquipmentSlot::BODY;
            armor.equipSound      = SoundEvents::HORSE_ARMOR;
            armor.assetId         = asset;
            armor.allowedEntities = {"#minecraft:can_wear_horse_armor"};
            armor.damageOnHurt    = false;
            armor.canBeSheared    = true;
            armor.shearingSound   = SoundEvents::HORSE_ARMOR_UNEQUIP;
            set(id, armor);
        };
        horseArmor(Items::LeatherHorseArmor,   "leather");
        horseArmor(Items::CopperHorseArmor,    "copper");
        horseArmor(Items::IronHorseArmor,      "iron");
        horseArmor(Items::GoldenHorseArmor,    "gold");
        horseArmor(Items::DiamondHorseArmor,   "diamond");
        horseArmor(Items::NetheriteHorseArmor, "netherite");

        // Item.Properties.nautilusArmor(material): BODY, ARMOR_EQUIP_NAUTILUS,
        // #can_wear_nautilus_armor, no wear on hurt, equipOnInteract,
        // canBeSheared with ARMOR_UNEQUIP_NAUTILUS.
        auto nautilusArmor = [&](ItemID id, const char* asset) {
            Equippable armor;
            armor.slot            = EquipmentSlot::BODY;
            armor.equipSound      = SoundEvents::ARMOR_EQUIP_NAUTILUS;
            armor.assetId         = asset;
            armor.allowedEntities = {"#minecraft:can_wear_nautilus_armor"};
            armor.damageOnHurt    = false;
            armor.equipOnInteract = true;
            armor.canBeSheared    = true;
            armor.shearingSound   = SoundEvents::ARMOR_UNEQUIP_NAUTILUS;
            set(id, armor);
        };
        nautilusArmor(Items::CopperNautilusArmor,    "copper");
        nautilusArmor(Items::IronNautilusArmor,      "iron");
        nautilusArmor(Items::GoldenNautilusArmor,    "gold");
        nautilusArmor(Items::DiamondNautilusArmor,   "diamond");
        nautilusArmor(Items::NetheriteNautilusArmor, "netherite");

        Log::Info("[ItemRegistry] Registered EQUIPPABLE on %d mount equipment items", registered);
    }

    void ItemRegistry_RegisterBlockItemEquipment(std::vector<Item>& blockItems) {
        // Items.java's wool carpets: `.component(EQUIPPABLE,
        // Equippable.llamaSwag(color))` — BODY, LLAMA_SWAG, asset
        // "<color>_carpet", allowed llama and trader llama, canBeSheared with
        // LLAMA_CARPET_UNEQUIP. They keep their stack size of 64.
        static constexpr BlockID kCarpets[16] = {
            BlockID::WhiteCarpet, BlockID::OrangeCarpet, BlockID::MagentaCarpet, BlockID::LightBlueCarpet,
            BlockID::YellowCarpet, BlockID::LimeCarpet, BlockID::PinkCarpet, BlockID::GrayCarpet,
            BlockID::LightGrayCarpet, BlockID::CyanCarpet, BlockID::PurpleCarpet, BlockID::BlueCarpet,
            BlockID::BrownCarpet, BlockID::GreenCarpet, BlockID::RedCarpet, BlockID::BlackCarpet,
        };
        for (int color = 0; color < 16; ++color) {
            const size_t index = static_cast<size_t>(kCarpets[color]);
            if (index >= blockItems.size()) continue;
            Equippable swag;
            swag.slot            = EquipmentSlot::BODY;
            swag.equipSound      = SoundEvents::LLAMA_SWAG;
            swag.assetId         = std::string(kDyeColorNames[color]) + "_carpet";
            swag.allowedEntities = {"minecraft:llama", "minecraft:trader_llama"};
            swag.canBeSheared    = true;
            swag.shearingSound   = SoundEvents::LLAMA_CARPET_UNEQUIP;
            blockItems[index].defaultComponents.set(DataComponents::EQUIPPABLE, swag);
        }

        // Items.java: CARVED_PUMPKIN's Equippable.builder(HEAD).setSwappable
        // (false).setCameraOverlay("misc/pumpkinblur"), and the heads'
        // `.equippableUnswappable(HEAD)` (skull, wither skull, player,
        // zombie, creeper, dragon, piglin): worn only through the armour
        // slot / a dispenser, never swapped on by a right-click.
        const auto head = [&blockItems](BlockID block, const char* overlay) {
            const size_t index = static_cast<size_t>(block);
            if (index >= blockItems.size()) return;
            Equippable e;
            e.slot          = EquipmentSlot::HEAD;
            e.equipSound    = SoundEvents::ARMOR_EQUIP_GENERIC;
            e.swappable     = false;
            if (overlay) e.cameraOverlay = overlay;
            blockItems[index].defaultComponents.set(DataComponents::EQUIPPABLE, e);
        };
        head(BlockID::CarvedPumpkin, "minecraft:misc/pumpkinblur");
        for (const BlockID b : { BlockID::SkeletonSkull, BlockID::WitherSkeletonSkull, BlockID::PlayerHead,
                                 BlockID::ZombieHead, BlockID::CreeperHead, BlockID::DragonHead, BlockID::PiglinHead }) {
            head(b, nullptr);
        }
    }

} // namespace Game
