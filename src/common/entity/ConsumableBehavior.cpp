// File: src/common/entity/ConsumableBehavior.cpp
// See header. MC citations per function; sounds/particles/game events are
// log-stubs (same pattern as ItemBehaviors.cpp's PlaySound).
#include "ConsumableBehavior.hpp"

#include "../core/Log.hpp"
#include "Inventory.hpp"
#include "server/player/ServerPlayer.hpp"

#include <array>

namespace Game::ConsumableBehavior {

    // Mirrors Consumable.startConsuming — Consumable.java:39-52.
    UseResult StartConsuming(World* world, Server::ServerPlayer& player,
                             uint32_t hand, ItemStack& stack) {
        auto consumable = stack.get(DataComponents::CONSUMABLE);
        if (!consumable) return UseResult::Pass;

        if (!CanConsume(player, stack)) {
            return UseResult::Fail;                       // :40-41
        }
        const bool consumesOverTime = consumable->consumeTicks() > 0;  // :43
        if (consumesOverTime) {
            player.startUsingItem(hand);                  // :45
            return UseResult::Consume;                    // :46
        }
        // Instant consume (:48-49) — no vanilla food uses 0 s, but the path
        // is kept for parity. Result lands via in-place mutation.
        OnConsume(world, player, stack, *consumable);
        return UseResult::Consume;
    }

    // Mirrors Consumable.canConsume — Consumable.java:72-79.
    bool CanConsume(const Server::ServerPlayer& player, const ItemStack& stack) {
        auto food = stack.get(DataComponents::FOOD);
        if (food) {
            // Player.canEat(canAlwaysEat) = canAlwaysEat || foodData.needsFood()
            return player.canEat(food->canAlwaysEat);
        }
        return true;   // non-food consumables (potions) always drinkable
    }

    // Mirrors Consumable.onConsume — Consumable.java:54-70.
    void OnConsume(World* world, Server::ServerPlayer& player,
                   ItemStack& stack, const Consumable& consumable) {
        (void)world;

        // :56 emitParticlesAndSounds(random, user, stack, 16 particles).
        // TODO(sounds/particles): consume sound + 16 item particles at the mouth.
        Log::Debug("[Consume] sound=%s particles=%s — TODO: wire sound/particle systems",
                   consumable.sound.c_str(),
                   consumable.hasConsumeParticles ? "yes" : "no");

        // :58-59 awardStat + CONSUME_ITEM criteria — no stats/advancements.

        // :62 ConsumableListener components — FOOD is our only listener
        // (FoodProperties.onConsume, FoodProperties.java:26-34): restore
        // hunger/saturation + the burp.
        if (auto food = stack.get(DataComponents::FOOD)) {
            player.getFoodData().eatFinal(food->nutrition, food->saturation);
            // FoodProperties.java:31 — PLAYER_BURP sound stub.
            Log::Debug("[Consume] burp — TODO: wire sound system");
        }

        // :63-65 on-consume effects — data carried, appliers stubbed (no
        // status-effect / random-teleport systems).
        for (const auto& effect : consumable.onConsumeEffects) {
            Log::Debug("[Consume] effect type=%u payload='%s' — TODO: apply "
                       "when the target system exists",
                       static_cast<unsigned>(effect.type), effect.payload.c_str());
        }

        // :67 gameEvent EAT/DRINK — game-event system TODO.

        // :68 stack.consume(1, user) — ItemStack.consume skips the shrink for
        // hasInfiniteMaterials (creative).
        if (player.getGameMode() != Server::GameMode::CREATIVE) {
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
        }
    }

    // Mirrors Consumable.shouldEmitParticlesAndSounds — Consumable.java:107-112.
    bool ShouldEmitParticlesAndSounds(const Consumable& consumable,
                                      int useItemRemainingTicks) {
        const int itemUsedForTicks = consumable.consumeTicks() - useItemRemainingTicks;
        const int waitTicks = static_cast<int>(
            static_cast<float>(consumable.consumeTicks()) * 0.21875f); // CONSUME_EFFECTS_START_FRACTION
        const bool isValidTime = itemUsedForTicks > waitTicks;
        return isValidTime && useItemRemainingTicks % 4 == 0;  // CONSUME_EFFECTS_INTERVAL
    }

    // Mirrors ItemStack.onUseTick's CONSUMABLE branch — ItemStack.java:1060-1064.
    void OnUseTick(Server::ServerPlayer& player, const ItemStack& stack,
                   int remainingTicks) {
        (void)player;
        auto consumable = stack.get(DataComponents::CONSUMABLE);
        if (consumable && ShouldEmitParticlesAndSounds(*consumable, remainingTicks)) {
            // :1063 emitParticlesAndSounds(…, 5 particles).
            Log::Debug("[Consume] tick sound=%s (remaining=%d) — TODO: wire sound system",
                       consumable->sound.c_str(), remainingTicks);
        }
    }

    // Mirrors ItemStack.finishUsingItem + applyAfterUseComponentSideEffects
    // (ItemStack.java:326-348).
    ItemStack FinishUsing(Server::ServerPlayer& player, ItemStack& handStack) {
        const int countBeforeUsing = handStack.count;   // :327

        // Item.finishUsingItem (Item.java:221-224): CONSUMABLE → onConsume.
        if (auto consumable = handStack.get(DataComponents::CONSUMABLE)) {
            OnConsume(nullptr, player, handStack, *consumable);
        }
        ItemStack result = handStack;

        // applyAfterUseComponentSideEffects (ItemStack.java:332-348) →
        // UseRemainder.convertIntoRemainder (UseRemainder.java:12-26).
        if (auto remainder = result.get(DataComponents::USE_REMAINDER)) {
            const bool hasInfiniteMaterials =
                player.getGameMode() == Server::GameMode::CREATIVE;
            if (!hasInfiniteMaterials && result.count < countBeforeUsing) {  // :13-16
                ItemStack remainderStack = remainder->convertInto;           // :18
                if (result.IsEmpty()) {
                    return remainderStack;                                   // :19-20
                }
                // :22 onExtraCreatedRemainder → Player.handleExtraItemsCreatedOnUse
                // (add to inventory; drop the overflow — no item entities).
                // Diff the inventory around the add so every slot the
                // remainder landed in gets broadcast via the dirty-slot
                // channel (the hand slot is handled by completeUsingItem).
                auto& inv = player.getInventory();
                std::array<ItemStack, Inventory::TOTAL_SIZE> before;
                for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) before[i] = inv.GetSlot(i);
                const int leftover = inv.AddItems(
                    remainderStack.itemId, remainderStack.count);
                for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) {
                    const auto& after = inv.GetSlot(i);
                    if (after.itemId != before[i].itemId || after.count != before[i].count) {
                        player.markSlotDirty(i);
                    }
                }
                if (leftover > 0) {
                    Log::Debug("[Consume] remainder overflow x%d — no item-entity "
                               "system, items lost", leftover);
                }
            }
        }
        return result;
    }

} // namespace Game::ConsumableBehavior
