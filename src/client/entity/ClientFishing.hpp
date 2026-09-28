// File: src/client/entity/ClientFishing.hpp
//
// The client's half of fishing (Game::FishingHook): the hook's synched data
// arriving, the reel-in pull on the local player, and MC's FishingRodCast
// item-model property for the local player's rod.
//
// The common hook cannot see players on the client (they are not entities
// here), so the parts of MC FishingHook that touch its owner or its hooked
// player on the client side live here, next to the packet handler.
#pragma once

#include <cstdint>

namespace Network { struct FishingHookDataS2CPacket; }
namespace Game { class ClientPlayer; class FishingHook; struct ItemStack; }

namespace Client::Fishing {

    // FishingHookDataS2C: the bobber's owner, hooked entity, bite and
    // holding arm (MC onSyncedDataUpdated).
    void ApplyHookData(const Network::FishingHookDataS2CPacket& packet);

    // Entity event 31 (FishingHook.kEventPullHooked) on bobber `hookEntityId`:
    // MC handleEntityEvent → pullEntity(hookedIn) on the client, which moves
    // exactly one thing a client simulates itself — its own player, when that
    // is what the hook is stuck in. `localPlayer` may be null.
    void OnPullEvent(int32_t hookEntityId, Game::ClientPlayer* localPlayer);

    // MC player.fishing for the local player, in the level being played:
    // their live hook, or null.
    Game::FishingHook* LocalPlayerHook();

    // MC FishingRodCast for the local player: which hand's rod draws cast
    // (0 = main, 1 = off hand — FishingHookRenderer.getHoldingArm: the main
    // hand when it holds a rod, else the off hand), -1 when not fishing.
    int LocalCastHand();

    // FishingRodCast for a stack drawn in the GUI: MC compares the stack to
    // the one in the holding arm BY IDENTITY, so only the local player's
    // actual held rod — the hotbar or inventory slot that is that stack —
    // draws cast, not another rod beside it.
    bool IsCastStack(const Game::ItemStack& stack);

} // namespace Client::Fishing
