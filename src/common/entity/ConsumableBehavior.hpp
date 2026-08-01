// File: src/common/entity/ConsumableBehavior.hpp
//
// The CONSUMABLE component's behaviour — eating/drinking. Mirrors
// world/item/component/Consumable.java (startConsuming/canConsume/onConsume)
// plus the finishUsingItem + USE_REMAINDER application from ItemStack.java.
//
// Lives in its own TU (not Item.cpp) because the implementation needs the
// full Server::ServerPlayer definition (startUsingItem, FoodData, game mode)
// — same common→server include precedent as PortalGunBehavior.cpp.
#pragma once

#include "Item.hpp"
#include "../data/DataComponents.hpp"

namespace Server { class ServerPlayer; }

namespace Game::ConsumableBehavior {

    // Mirrors Consumable.startConsuming — Consumable.java:39-52. Resolves
    // the CONSUMABLE component from the stack; Pass when absent.
    UseResult StartConsuming(World* world, Server::ServerPlayer& player,
                             uint32_t hand, ItemStack& stack);

    // Mirrors Consumable.canConsume — :72-79 (FOOD present → player.canEat).
    bool CanConsume(const Server::ServerPlayer& player, const ItemStack& stack);

    // Mirrors Consumable.onConsume — :54-70. Sound/particle/game-event stubs,
    // FOOD → FoodData.eat, on-consume effects logged, stack.consume(1)
    // (creative keeps the count, mirroring ItemStack.consume's
    // hasInfiniteMaterials check).
    void OnConsume(World* world, Server::ServerPlayer& player,
                   ItemStack& stack, const Consumable& consumable);

    // Mirrors Consumable.shouldEmitParticlesAndSounds — :107-112 (past
    // 21.875% of the duration, every 4 ticks).
    bool ShouldEmitParticlesAndSounds(const Consumable& consumable,
                                      int useItemRemainingTicks);

    // Mirrors ItemStack.onUseTick's CONSUMABLE branch (ItemStack.java:1060-1064)
    // — the periodic eat sound/particle stub while the timer runs.
    void OnUseTick(Server::ServerPlayer& player, const ItemStack& stack,
                   int remainingTicks);

    // Mirrors ItemStack.finishUsingItem (ItemStack.java:326-330 →
    // Item.finishUsingItem, Item.java:221-224) PLUS
    // applyAfterUseComponentSideEffects (ItemStack.java:332-348, the
    // USE_REMAINDER conversion). `handStack` is mutated (consume); the
    // returned stack is what should end up in the hand slot.
    ItemStack FinishUsing(Server::ServerPlayer& player, ItemStack& handStack);

} // namespace Game::ConsumableBehavior
