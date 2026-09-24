// File: src/server/entity/MorphCarry.hpp
//
// /morph item: a player morphed into an item is picked up like one. Every
// tick, an item-morphed player whose pickup delay has run out and who
// touches another player's pickup box (MC Player.touch → ItemEntity.
// playerTouch: the player's bounding box grown by 1, 0.5, 1) is picked up:
// the item — one of the morph's kind, named after the player — goes into
// the holder's inventory as a real stack, the player turns invisible and
// their client pins itself to the holder, and every client plays the
// fly-in of a real pickup. They look around freely from the holder's hand.
//
// The stack IS the tether: Left Alt (`/morph ability` from the held
// client) throws them out as a Q drop, the holder dropping the stack
// throws them the same way, and the stack leaving the inventory any other
// way (placed, put in a chest) lets them go where the holder stands. So
// does morphing into something else. Server thread only.
#pragma once

#include "common/entity/Item.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Server {

    class PlayerSession;
    class PlayerSessionManager;
    class RemoteControlManager;

    class MorphCarry {
    public:
        MorphCarry(PlayerSessionManager& sessions, RemoteControlManager& control)
            : m_sessions(sessions), m_control(control) {}

        void Tick();
        // Let `heldId` go, thrown from the holder's hand when `throwIt`.
        // False when they were not held.
        bool Release(uint32_t heldId, bool throwIt);
        // The holder is dropping `stack` (Q, or a click outside a screen):
        // if it is a carried player's stack, that player is thrown instead
        // of an item entity spawning. True when it was.
        bool ReleaseByDrop(uint32_t holderId, const Game::ItemStack& stack);
        bool IsHeld(uint32_t playerId) const;
        // A fresh item morph waits like a fresh drop before it can be
        // picked up (MC ItemEntity default pickupDelay 10; a throw is 40).
        void SetPickupDelay(uint32_t playerId, int ticks) { m_pickupDelay[playerId] = ticks; }

    private:
        struct Held {
            uint32_t        held = 0;
            uint32_t        holder = 0;
            Game::ItemStack marker;   // the stack in the holder's inventory
        };
        void Pickup(uint32_t heldId, uint32_t holderId);
        static void SendRole(PlayerSession& to, uint8_t role, uint32_t otherId, const std::string& otherName);
        static bool IsMarker(const Game::ItemStack& stack, const Game::ItemStack& marker);

        PlayerSessionManager&  m_sessions;
        RemoteControlManager&  m_control;
        std::vector<Held>      m_held;
        std::unordered_map<uint32_t, int> m_pickupDelay;   // ticks left, per player
    };

} // namespace Server
