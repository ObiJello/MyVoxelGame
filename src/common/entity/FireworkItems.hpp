// File: src/common/entity/FireworkItems.hpp
//
// MC FireworkRocketItem, the firework components' tooltips (Fireworks /
// FireworkExplosion .addToTooltip), DyeColor's firework colours, and
// CrossbowItem — the crossbow is here because it is the rocket's launcher
// and the two share the projectile rules (ProjectileWeaponItem
// .ARROW_OR_FIREWORK).
//
// The behaviours run on both sides (Item.useOn / use are shared code); what
// only the server can do — spawn the rocket with the player's entity as its
// owner, draw the crossbow's ammunition, shoot — goes through the bridges in
// common/world/level/FireworkLaunch.hpp (defined in server/items/
// FireworkItems.cpp), the same split as HushItems.
#pragma once

#include "common/data/FireworkExplosion.hpp"
#include "common/entity/Item.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game {

    struct Fireworks;
    struct ChargedProjectiles;
    class Inventory;

    namespace FireworkItems {

        // MC FireworkRocketItem.CRAFTABLE_DURATIONS / ROCKET_PLACEMENT_OFFSET.
        inline constexpr int    kCraftableDurations[] = { 1, 2, 3 };
        inline constexpr double kRocketPlacementOffset = 0.15;

        // MC CrossbowItem constants.
        inline constexpr float kCrossbowMaxChargeDuration = 1.25f;
        inline constexpr float kCrossbowArrowPower   = 3.15f;
        inline constexpr float kCrossbowFireworkPower = 1.6f;
        inline constexpr float kCrossbowStartSoundPercent = 0.2f;
        inline constexpr float kCrossbowMidSoundPercent   = 0.5f;

        // ── DyeColor (fireworkColor, byFireworkColor, getName) ────────────
        int32_t     DyeFireworkColor(uint8_t dye);            // 0xRRGGBB
        int         DyeColorByFireworkColor(int32_t rgb);     // ordinal, -1 = none
        const char* DyeColorName(uint8_t dye);                // "light_blue"

        // ── Tooltips ────────────────────────────────────────────────────────
        struct TooltipLine {
            std::string text;
            uint32_t    argb;
        };
        // MC FireworkExplosion.Shape.getName.
        std::string ShapeDisplayName(FireworkExplosion::Shape shape);
        // MC Fireworks.addToTooltip: "Flight Duration: N", then each run of
        // equal stars as "<shape>" / "N x <shape>" with its details indented.
        void AddFireworksTooltip(const Fireworks& fireworks, std::vector<TooltipLine>& out);
        // MC FireworkExplosion.addToTooltip: the shape, then the details.
        void AddExplosionTooltip(const FireworkExplosion& explosion, std::vector<TooltipLine>& out);
        // MC FireworkExplosion.addAdditionalTooltip: colours, "Fade to …",
        // "Trail", "Twinkle" — each prefixed with `indent`.
        void AddExplosionDetails(const FireworkExplosion& explosion, const std::string& indent,
                                 std::vector<TooltipLine>& out);

        // ── Components with MC's patch semantics ──────────────────────────
        // A value equal to the item's own default is no patch (the stack
        // then stacks with a fresh one), as PatchedDataComponentMap drops it.
        void SetFireworks(ItemStack& stack, const Fireworks& value);
        void SetChargedProjectiles(ItemStack& stack, const ChargedProjectiles& value);
        // A rocket with `flightDuration` and no stars (the creative tab's
        // generateFireworksAllDurations).
        ItemStack MakeRocket(int flightDuration, int count = 1);

        // ── Crossbow (MC CrossbowItem / ProjectileWeaponItem) ─────────────
        // MC ItemTags.ARROWS — arrow, tipped arrow, spectral arrow.
        bool IsArrowItem(ItemID id);
        // MC CrossbowItem.getSupportedHeldProjectiles (ARROW_OR_FIREWORK).
        bool IsCrossbowHeldProjectile(ItemID id);
        // MC CrossbowItem.isCharged.
        bool IsCrossbowCharged(const ItemStack& crossbow);
        // MC CrossbowItem.getChargeDuration: floor(Quick-Charge-modified
        // 1.25 s * 20).
        int  CrossbowChargeDuration(const ItemStack& crossbow);
        // MC CrossbowItem.getPowerForTime.
        float CrossbowPowerForTime(int timeHeld, const ItemStack& crossbow);
        // MC Player.getProjectile(crossbow) as a slot search over an
        // inventory: off hand, then main hand (ARROW_OR_FIREWORK), then the
        // inventory (arrows only). -1 = none. `selectedSlot` is the main
        // hand's inventory index.
        int  FindCrossbowProjectileSlot(const Inventory& inventory);
        // Whether CrossbowItem.use would start the draw: the player has a
        // projectile or infinite materials.
        bool CanDrawCrossbow(const Inventory& inventory, bool creative);

        // ── Registration ──────────────────────────────────────────────────
        // Wires firework_rocket (useOn / use, FIREWORKS default), crossbow
        // (use / onUseTick / useOnRelease / the charged sprites,
        // CHARGED_PROJECTILES default). Called from ItemRegistry_
        // RegisterBehaviors.
        void RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems);

    } // namespace FireworkItems

} // namespace Game
