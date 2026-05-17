// File: src/common/entity/PortalGunBehavior.cpp
//
// Per-stack right-click behavior for the Portal Gun. Wired into the global
// ItemRegistry from Item.cpp::ItemRegistry::Initialize via gun.useOn = ....
//
// SCOPE OF THIS FILE (PHASE 1):
//   • Resolve next-fire color from the stack's PORTAL_GUN_NEXT_COLOR
//     DataComponent (default 0 / blue on a freshly-spawned gun).
//   • Lazy-assign the stack a stable PORTAL_GUN_INSTANCE_ID on first shot.
//     This id is the key the server-side PortalRegistry will use (Phase 2)
//     to track the gun's blue+orange pair.
//   • Toggle PORTAL_GUN_NEXT_COLOR for the next shot (right-click alternates
//     between blue and orange per the user's spec).
//   • Log the would-be placement so we can verify the input + dispatch path
//     end-to-end before the registry exists.
//
// What's INTENTIONALLY NOT here yet (lands in later phases):
//   • Actual PortalRegistry call to validate + place + broadcast (Phase 2).
//   • Network packet emission for client see-through render sync (Phase 3).
//   • Any rendering — that's all client-side, Phase 4+.
//
// Entire file gated on ENABLE_PORTAL_GUN — when the feature is off, this TU
// compiles to nothing AND CMakeLists.txt is configured to not even add the
// .cpp to the build target (defense in depth).

#include "../core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "Item.hpp"
#include "../data/DataComponents.hpp"
#include "../core/Log.hpp"
#include "server/portal/PortalRegistry.hpp"
#include "server/player/ServerPlayer.hpp"

namespace Game::Portal {

    // Right-click on a block with the portal gun.
    // ItemUseOnFn signature: UseResult(const UseOnContext&, ItemStack&).
    // ctx carries world / player / hand / hit; stack is the held gun stack
    // (mutable so we can write back the lazily-assigned instance id).
    //
    // BEHAVIOUR (matches the user's spec):
    //   • Plain right-click: place blue first, then orange. Once both are
    //     placed every subsequent shot replaces orange (blue stays put as
    //     the anchor; orange is the dynamic target). To re-place blue the
    //     user shift-right-clicks to wipe the pair, then fires again.
    //   • Shift+right-click: clear BOTH portals belonging to this gun.
    //     PlayerSession's `suppressBlockUse = sneaking && somethingInHands`
    //     guarantees we get this dispatch (it bypasses the block-use path
    //     that sneaking would otherwise trigger).
    UseResult OnGunUseOn(const UseOnContext& ctx, ItemStack& stack) {
        if (!ctx.world || !ctx.player) return UseResult::Pass;

        // Lazy-assign a stable per-stack id on first interaction (regardless
        // of whether it's a place or a clear). Zero (default) = unassigned.
        // The id keys this gun's portal pair in the registry — every
        // subsequent fire / clear from the same stack mutates the same pair.
        uint64_t id = stack.get(DataComponents::PORTAL_GUN_INSTANCE_ID)
                           .value_or(uint64_t{0});
        if (id == 0) {
            id = ServerRegistry().AllocId();
            stack.components.set(DataComponents::PORTAL_GUN_INSTANCE_ID, id);
            Log::Info("[PortalGun] Assigned instance id %llu to gun stack",
                      static_cast<unsigned long long>(id));
        }

        // Shift+right-click clears the pair. Sneaking is the trigger
        // (matches the prior gesture). Returns Success so the arm
        // swings as feedback.
        if (ctx.player->IsSneaking() && !ctx.altInteract) {
            ServerRegistry().ClearPair(id);
            return UseResult::Success;
        }

        // Color choice — explicit per-input-button:
        //   • Right-click (altInteract = false) → ORANGE
        //   • Left-click  (altInteract = true)  → BLUE
        // Each click REPLACES the existing portal of that color.
        const PortalColor color = ctx.altInteract ? PortalColor::Blue
                                                  : PortalColor::Orange;

        // Hand off to the registry: walks the candidate-orientation list
        // (vertical-up → vertical-down → horizontal → ...) and registers
        // the first valid placement.
        const PlaceResult result = ServerRegistry().PlacePortal(
            id, ctx.world, ctx.hitResult, color, ctx.player);

        if (result == PlaceResult::Fizzled) {
            // TODO(sound): play fizzle sound when the audio system lands.
            return UseResult::Fail;
        }
        return UseResult::Success;
    }

} // namespace Game::Portal

#endif // ENABLE_PORTAL_GUN
