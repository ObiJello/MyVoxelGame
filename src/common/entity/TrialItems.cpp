// File: src/common/entity/TrialItems.cpp
//
// The trial chambers' item defaults (Items.java rows):
//
//   OMINOUS_BOTTLE  .rarity(UNCOMMON)
//                   .component(CONSUMABLE, Consumables.OMINOUS_BOTTLE)
//                   .component(OMINOUS_BOTTLE_AMPLIFIER, new OminousBottleAmplifier(0))
//                   — Consumables.OMINOUS_BOTTLE is defaultDrink()
//                   .soundAfterConsume(OMINOUS_BOTTLE_DISPOSE): a PlaySound
//                   consume effect after the drink. The amplifier is the
//                   ConsumableListener that gives Bad Omen
//                   (ConsumableBehavior::OnConsume).
//   TRIAL_KEY, OMINOUS_TRIAL_KEY
//                   plain items: all they do is match a vault's key_item
//                   (VaultBlockEntity::TryInsertKey).
#include "Item.hpp"
#include "GeneratedItemList.hpp"
#include "../data/DataComponents.hpp"
#include "../sound/SoundEvents.hpp"

#include <unordered_map>

namespace Game {

    void ItemRegistry_RegisterTrialItems(std::unordered_map<ItemID, Item>& pureItems) {
        auto it = pureItems.find(Items::OminousBottle);
        if (it == pureItems.end()) return;
        Item& bottle = it->second;

        // Consumables.defaultDrink(): 1.6 s, DRINK, entity.generic.drink, no
        // consume particles — then soundAfterConsume.
        Consumable consumable;
        consumable.consumeSeconds      = 1.6f;
        consumable.animation           = ItemUseAnimation::DRINK;
        consumable.sound               = "entity.generic.drink";
        consumable.hasConsumeParticles = false;
        consumable.onConsumeEffects.push_back(
            ConsumeEffect::PlaySoundEvent(SoundEvents::OMINOUS_BOTTLE_DISPOSE));
        bottle.defaultComponents.set(DataComponents::CONSUMABLE, consumable);
        bottle.defaultComponents.set(DataComponents::OMINOUS_BOTTLE_AMPLIFIER, 0);
        bottle.defaultComponents.set(DataComponents::RARITY, Rarity::UNCOMMON);
    }

} // namespace Game
