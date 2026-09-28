// File: src/common/entity/FoodOnAStickItem.cpp
#include "common/entity/FoodOnAStickItem.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/ItemBasedSteering.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <optional>

namespace Game::FoodOnAStickItem {

    namespace {

        // A level without a random of its own (never a real server level)
        // still has to roll Unbreaking with something.
        JavaRandom& FallbackRandom() {
            thread_local JavaRandom random(static_cast<int64_t>(0x5C0A7E11));
            return random;
        }

        // MC FoodOnAStickItem.use for a rod bound to `canInteractWith`,
        // wearing `consumeItemDamage` a boost.
        UseResult UseRod(ILevelWrite* world, IUsePlayer* player, uint32_t hand, ItemStack& stack,
                         EntityTypeId canInteractWith, int consumeItemDamage) {
            if (!world || !player) return UseResult::Pass;
            // The client passes (its use packet still reaches the server,
            // which alone knows who steers what).
            if (world->IsClientSide()) return UseResult::Pass;

            // player.getControlledVehicle(): the vehicle whose controlling
            // passenger is this player — here the player's riding view.
            Entity* self = player->GameEventSource();
            Entity* vehicle = self ? self->GetVehicle() : nullptr;
            if (!self || !vehicle || vehicle->GetControllingPassenger() != self) return UseResult::Pass;

            auto* steerable = dynamic_cast<ItemSteerable*>(vehicle);
            if (!steerable || vehicle->GetType() != canInteractWith || !steerable->Boost()) {
                // (player.awardStat(ITEM_USED) — no statistics here.)
                return UseResult::Pass;
            }

            // itemStack.hurtAndConvertOnBreak(consumeItemDamage, FISHING_ROD,
            // player, hand.asEquipmentSlot()): the wear (none in creative —
            // hasInfiniteMaterials), and a rod that broke becomes a fishing
            // rod carrying the broken rod's components, undamaged.
            const EquipmentSlot slot = hand == 1 ? EquipmentSlot::OFFHAND : EquipmentSlot::MAINHAND;
            std::optional<ItemStack> broken;
            JavaRandom* random = world->Random();
            HurtAndBreak(stack, consumeItemDamage, random ? *random : FallbackRandom(), player->isCreative(),
                         [&](const ItemStack& b) {
                             broken = b;
                             player->OnEquippedItemBroken(b, slot);
                         });
            if (stack.IsEmpty() && broken) {
                ItemStack replacement(Items::FishingRod, 1);
                replacement.components = broken->components;
                if (IsDamageableItem(replacement)) SetDamageValue(replacement, 0);
                // InteractionResult.SUCCESS_SERVER.heldItemTransformedTo(result).
                stack = replacement;
            }
            player->markSlotDirty(player->handSlotIndex(hand));
            return UseResult::SuccessServer;
        }

        // Items.CARROT_ON_A_STICK: FoodOnAStickItem(EntityTypes.PIG, 7).
        UseResult Use_CarrotOnAStick(ILevelWrite* world, IUsePlayer* player, uint32_t hand, ItemStack& stack) {
            return UseRod(world, player, hand, stack, EntityTypeId::Pig, 7);
        }

        // Items.WARPED_FUNGUS_ON_A_STICK: FoodOnAStickItem(EntityTypes.STRIDER, 1).
        UseResult Use_WarpedFungusOnAStick(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                                           ItemStack& stack) {
            return UseRod(world, player, hand, stack, EntityTypeId::Strider, 1);
        }

    } // namespace

    void RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems) {
        if (auto it = pureItems.find(Items::CarrotOnAStick); it != pureItems.end()) {
            it->second.use = &Use_CarrotOnAStick;
        }
        if (auto it = pureItems.find(Items::WarpedFungusOnAStick); it != pureItems.end()) {
            it->second.use = &Use_WarpedFungusOnAStick;
        }
    }

} // namespace Game::FoodOnAStickItem
