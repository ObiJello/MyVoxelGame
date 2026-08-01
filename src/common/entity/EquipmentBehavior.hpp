// File: src/common/entity/EquipmentBehavior.hpp
//
// The EQUIPPABLE component's behaviour — right-click auto-equip. Mirrors
// Equippable.swapWithEquipmentSlot (Equippable.java:54-83). Own TU because
// the implementation needs the full Server::ServerPlayer (same precedent as
// ConsumableBehavior / PortalGunBehavior).
#pragma once

#include "Item.hpp"
#include "../data/DataComponents.hpp"

namespace Server { class ServerPlayer; }

namespace Game::EquipmentBehavior {

    // Mirrors Equippable.swapWithEquipmentSlot — Equippable.java:54-83.
    // `hand` is the hand holding the equippable stack.
    UseResult SwapWithEquipmentSlot(Server::ServerPlayer& player, uint32_t hand,
                                    const Equippable& equippable);

} // namespace Game::EquipmentBehavior
