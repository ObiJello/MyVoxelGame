// File: src/common/entity/WeaponItems.hpp
//
// The trident's and the spears' item behaviours, reachable from common: the
// Item callbacks ItemBehaviors registers (use / release / the use tick) and
// the jab the server's action handler runs. Same bridge shape as HushItems:
// common declares, the server (server/items/TridentSpearItems.cpp) defines —
// the throw spawns an entity owned by the player's entity view, the charge
// walks the player's level, the riptide arms the ServerPlayer's spin.
//
// The client predicts its own half separately (PlayerController): the use
// pose, the spear's use sound, and the riptide launch — the push, the lift
// and the spin — which only the client, owning its movement, can apply.
#pragma once

#include "common/entity/Item.hpp"

#include <cstdint>

namespace Game {

    class IUsePlayer;
    struct ItemStack;

    namespace WeaponItems {

        // ── Spears (Item.use / ItemStack.onUseTick for a KINETIC_WEAPON) ──
        // Item.use: startUsingItem(hand), then kineticWeapon.makeSound —
        // everyone but the player hears it (their client plays its own).
        UseResult SpearBegin(IUsePlayer& player, uint32_t hand);
        // ItemStack.onUseTick → KineticWeapon.damageEntities (ItemUseTickFn).
        void      SpearUseTick(IUsePlayer& player, ItemStack& spear, int remainingTicks);
        // ServerboundPlayerActionPacket STAB: PiercingWeapon.attack with the
        // main-hand spear, unless the attack charge is not full
        // (cannotAttackWithItem(item, 5)).
        void      Stab(IUsePlayer& player);

        // ── Trident (TridentItem) ─────────────────────────────────────────
        // TridentItem.use: FAIL when the next point of wear would break it,
        // or when it has Riptide and the player is neither in water nor in
        // the rain; otherwise the draw starts.
        UseResult TridentBegin(IUsePlayer& player, uint32_t hand);
        // TridentItem.releaseUsing (ItemReleaseUsingFn): after 10 ticks of
        // draw, a throw (ThrownTrident at 2.5, the stack moved into it) or —
        // with Riptide, in water or rain and not riding — the spin attack.
        void      TridentRelease(IUsePlayer& player, ItemStack& trident, int remainingTicks);

        // TridentItem.releaseUsing's shared gates, for the client's
        // prediction as much as the server: the draw long enough, and — for
        // a Riptide trident — water or rain and no vehicle; never a trident
        // one point from breaking.
        bool CanReleaseTrident(const ItemStack& trident, int ticksHeld, bool inWaterOrRain, bool passenger);

    } // namespace WeaponItems

} // namespace Game
