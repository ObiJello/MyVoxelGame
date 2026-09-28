// File: src/common/entity/FireworkItems.cpp
//
// See FireworkItems.hpp. References (minecraft_code_26.3-pre-2/
// decompiled_net/minecraft/): world/item/FireworkRocketItem.java,
// world/item/CrossbowItem.java, world/item/ProjectileWeaponItem.java,
// world/item/component/Fireworks.java, FireworkExplosion.java,
// ChargedProjectiles.java, world/item/DyeColor.java,
// assets items/crossbow.json (the charge_type / using_item / crossbow/pull
// model tree).
#include "common/entity/FireworkItems.hpp"

#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/FireworkLaunch.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <algorithm>
#include <cmath>

namespace Game::FireworkItems {

    namespace {

        constexpr uint32_t kGray  = 0xFFAAAAAAu;   // ChatFormatting.GRAY

        // DyeColor, in ordinal order: name and fireworkColor.
        struct DyeRow { const char* name; int32_t fireworkColor; };
        constexpr DyeRow kDyes[16] = {
            { "white",      15790320 }, { "orange",     15435844 },
            { "magenta",    12801229 }, { "light_blue",  6719955 },
            { "yellow",     14602026 }, { "lime",        4312372 },
            { "pink",       14188952 }, { "gray",        4408131 },
            { "light_gray", 11250603 }, { "cyan",        2651799 },
            { "purple",      8073150 }, { "blue",        2437522 },
            { "brown",       5320730 }, { "green",       3887386 },
            { "red",        11743532 }, { "black",       1973019 },
        };

        // Component.translatable(key, args...).getString().
        std::string Translate(const char* key, std::initializer_list<std::string> args) {
            std::vector<Text::Component> with;
            with.reserve(args.size());
            for (const std::string& a : args) with.push_back(Text::Component::Literal(a));
            return Text::GetString(Text::Component::Translatable(key, std::move(with)));
        }

        // MC FireworkExplosion.getColorName.
        std::string ColorName(int32_t color) {
            const int dye = DyeColorByFireworkColor(color);
            if (dye < 0) return Language::Get("item.minecraft.firework_star.custom_color");
            return Language::Get(std::string("item.minecraft.firework_star.") + kDyes[dye].name);
        }

        // MC FireworkExplosion.appendColors.
        std::string AppendColors(std::string builder, const std::vector<int32_t>& colors) {
            for (size_t i = 0; i < colors.size(); ++i) {
                if (i > 0) builder += ", ";
                builder += ColorName(colors[i]);
            }
            return builder;
        }

        // ── Firework rocket (FireworkRocketItem) ────────────────────────────

        // FireworkRocketItem.useOn.
        UseResult UseOn_FireworkRocket(const UseOnContext& ctx, ItemStack& stack) {
            if (ctx.player && ctx.player->isFallFlying()) return UseResult::Pass;
            if (ctx.world && !ctx.world->IsClientSide()) {
                // clickLocation + the clicked face's step * 0.15.
                static constexpr int kStep[6][3] = {
                    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                const int face = std::clamp(ctx.hitResult.face, 0, 5);
                const glm::dvec3 at = ctx.hitResult.hitPoint +
                    glm::dvec3(kStep[face][0], kStep[face][1], kStep[face][2]) * kRocketPlacementOffset;
                FireworkLaunch::LaunchFromBlock(ctx.world, ctx.player, stack, at);
                // itemStack.shrink(1) — the creative count comes back in the
                // server's useItemOn (ServerPlayerGameMode).
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            return UseResult::Success;
        }

        // FireworkRocketItem.use — the elytra boost.
        UseResult Use_FireworkRocket(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                                     ItemStack& stack) {
            if (!player || !player->isFallFlying()) return UseResult::Pass;
            if (world && !world->IsClientSide()) {
                if (FireworkLaunch::BoostPlayer(*player, hand, stack)) {
                    // itemStack.consume(1, player): nothing spent by
                    // infinite materials.
                    if (!player->isCreative()) {
                        stack.count -= 1;
                        if (stack.count <= 0) stack.Clear();
                    }
                    player->markSlotDirty(player->handSlotIndex(hand));
                }
            }
            return UseResult::Success;
        }

        // ── Crossbow (CrossbowItem) ─────────────────────────────────────────

        // CrossbowItem.use: shoot what is loaded; else start drawing when
        // there is something to load; else FAIL.
        UseResult Use_Crossbow(ILevelWrite* world, IUsePlayer* player, uint32_t hand,
                               ItemStack& stack) {
            if (!player) return UseResult::Pass;
            if (IsCrossbowCharged(stack)) {
                // performShooting is a ServerLevel-only body; the client's
                // run answers CONSUME and waits for the slot update.
                if (world && !world->IsClientSide()) {
                    FireworkLaunch::CrossbowShoot(*player, hand, stack);
                }
                return UseResult::Consume;
            }
            if (world && world->IsClientSide()) {
                // The client's copy of player.getProjectile: its predicted
                // draw (StartPredictedUse) asks the same question.
                return UseResult::Consume;
            }
            return FireworkLaunch::CrossbowBeginDraw(*player, hand);
        }

        // CrossbowItem.onUseTick (server; LivingEntity.updateUsingItem).
        void UseTick_Crossbow(IUsePlayer& player, ItemStack& stack, int remainingTicks) {
            FireworkLaunch::CrossbowUseTick(player, stack, remainingTicks);
        }

        // items/crossbow.json: select(charge_type) → crossbow_arrow /
        // crossbow_firework; else condition(using_item) → range_dispatch
        // (crossbow/pull: 0.58 → pulling_1, 1.0 → pulling_2, else
        // pulling_0); else crossbow.
        std::string Sprite_Crossbow(const ItemStack& stack) {
            if (auto charged = stack.get(DataComponents::CHARGED_PROJECTILES); charged && !charged->IsEmpty()) {
                // CrossbowItem.ChargeType: ROCKET when any loaded projectile
                // is a firework (Items.CROSSBOW's charge_type property).
                return charged->Contains(Items::FireworkRocket) ? "crossbow_firework" : "crossbow_arrow";
            }
            // using_item + crossbow/pull, for THIS stack only.
            const int ticks = ItemStackUseTicks(stack);
            if (ticks < 0) return {};
            const float pull = static_cast<float>(ticks) /
                               static_cast<float>(std::max(1, CrossbowChargeDuration(stack)));
            if (pull >= 1.0f)  return "crossbow_pulling_2";
            if (pull >= 0.58f) return "crossbow_pulling_1";
            return "crossbow_pulling_0";
        }

        // items/bow.json: condition(using_item) → range_dispatch(use_duration,
        // scale 0.05: 0.65 → bow_pulling_1, 0.9 → bow_pulling_2, else
        // bow_pulling_0); else bow. Per rendered stack, like the crossbow's.
        std::string Sprite_Bow(const ItemStack& stack) {
            const int ticks = ItemStackUseTicks(stack);
            if (ticks < 0) return {};
            const float pull = static_cast<float>(ticks) * 0.05f;
            if (pull >= 0.9f)  return "bow_pulling_2";
            if (pull >= 0.65f) return "bow_pulling_1";
            return "bow_pulling_0";
        }

    } // namespace

    // ── DyeColor ──────────────────────────────────────────────────────────

    int32_t DyeFireworkColor(uint8_t dye) {
        return dye < 16 ? kDyes[dye].fireworkColor : kDyes[0].fireworkColor;
    }

    int DyeColorByFireworkColor(int32_t rgb) {
        for (int i = 0; i < 16; ++i) {
            if (kDyes[i].fireworkColor == rgb) return i;
        }
        return -1;
    }

    const char* DyeColorName(uint8_t dye) {
        return dye < 16 ? kDyes[dye].name : kDyes[0].name;
    }

    // ── Tooltips ──────────────────────────────────────────────────────────

    std::string ShapeDisplayName(FireworkExplosion::Shape shape) {
        return Language::Get("item.minecraft.firework_star.shape." +
                             std::string(FireworkExplosion::ShapeName(shape)));
    }

    void AddExplosionDetails(const FireworkExplosion& explosion, const std::string& indent,
                             std::vector<TooltipLine>& out) {
        if (!explosion.colors.empty()) {
            out.push_back({indent + AppendColors({}, explosion.colors), kGray});
        }
        if (!explosion.fadeColors.empty()) {
            out.push_back({indent + AppendColors(Language::Get("item.minecraft.firework_star.fade_to") + " ",
                                                 explosion.fadeColors),
                           kGray});
        }
        if (explosion.hasTrail) {
            out.push_back({indent + Language::Get("item.minecraft.firework_star.trail"), kGray});
        }
        if (explosion.hasTwinkle) {
            out.push_back({indent + Language::Get("item.minecraft.firework_star.flicker"), kGray});
        }
    }

    void AddExplosionTooltip(const FireworkExplosion& explosion, std::vector<TooltipLine>& out) {
        out.push_back({ShapeDisplayName(explosion.shape), kGray});
        AddExplosionDetails(explosion, {}, out);
    }

    void AddFireworksTooltip(const Fireworks& fireworks, std::vector<TooltipLine>& out) {
        if (fireworks.flightDuration > 0) {
            out.push_back({Translate("item.minecraft.firework_rocket.flight_duration",
                                     {std::to_string(fireworks.flightDuration)}),
                           kGray});
        }
        // Runs of equal explosions collapse into "N x <shape>".
        const auto addRun = [&out](const FireworkExplosion& explosion, int count) {
            const std::string shapeName = ShapeDisplayName(explosion.shape);
            if (count == 1) {
                out.push_back({Translate("item.minecraft.firework_rocket.single_star", {shapeName}), kGray});
            } else {
                out.push_back({Translate("item.minecraft.firework_rocket.multiple_stars",
                                         {std::to_string(count), shapeName}),
                               kGray});
            }
            AddExplosionDetails(explosion, "  ", out);
        };
        const FireworkExplosion* current = nullptr;
        int count = 0;
        for (const FireworkExplosion& explosion : fireworks.explosions) {
            if (!current) {
                current = &explosion;
                count = 1;
            } else if (*current == explosion) {
                ++count;
            } else {
                addRun(*current, count);
                current = &explosion;
                count = 1;
            }
        }
        if (current) addRun(*current, count);
    }

    // ── Components ────────────────────────────────────────────────────────

    void SetFireworks(ItemStack& stack, const Fireworks& value) {
        const auto prototype = ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::FIREWORKS);
        if (prototype && *prototype == value) {
            stack.components.remove(DataComponents::FIREWORKS);
        } else {
            Fireworks clamped = value;
            if (clamped.explosions.size() > Fireworks::kMaxExplosions) {
                clamped.explosions.resize(Fireworks::kMaxExplosions);
            }
            stack.components.set(DataComponents::FIREWORKS, std::move(clamped));
        }
    }

    void SetChargedProjectiles(ItemStack& stack, const ChargedProjectiles& value) {
        const auto prototype =
            ItemRegistry::Get(stack.itemId).defaultComponents.get(DataComponents::CHARGED_PROJECTILES);
        if (prototype && prototype->IsEmpty() && value.IsEmpty()) {
            stack.components.remove(DataComponents::CHARGED_PROJECTILES);
        } else {
            stack.components.set(DataComponents::CHARGED_PROJECTILES, value);
        }
    }

    ItemStack MakeRocket(int flightDuration, int count) {
        ItemStack rocket(Items::FireworkRocket, count);
        Fireworks fireworks;
        fireworks.flightDuration = flightDuration;
        SetFireworks(rocket, fireworks);
        return rocket;
    }

    // ── Crossbow ──────────────────────────────────────────────────────────

    bool IsArrowItem(ItemID id) {
        return id == Items::Arrow || id == Items::TippedArrow || id == Items::SpectralArrow;
    }

    bool IsCrossbowHeldProjectile(ItemID id) {
        return IsArrowItem(id) || id == Items::FireworkRocket;
    }

    bool IsCrossbowCharged(const ItemStack& crossbow) {
        auto charged = crossbow.get(DataComponents::CHARGED_PROJECTILES);
        return charged && !charged->IsEmpty();
    }

    int CrossbowChargeDuration(const ItemStack& crossbow) {
        const float seconds = EnchantmentHelper::ModifyCrossbowChargingTime(crossbow, kCrossbowMaxChargeDuration);
        return static_cast<int>(std::floor(seconds * 20.0f));
    }

    float CrossbowPowerForTime(int timeHeld, const ItemStack& crossbow) {
        const int duration = CrossbowChargeDuration(crossbow);
        // A zero charge time (Quick Charge past V) is instantly full.
        float pow = duration > 0 ? static_cast<float>(timeHeld) / static_cast<float>(duration) : 1.0f;
        return std::min(pow, 1.0f);
    }

    int FindCrossbowProjectileSlot(const Inventory& inventory) {
        // ProjectileWeaponItem.getHeldProjectile(ARROW_OR_FIREWORK): the off
        // hand, then the main hand.
        const int off  = Inventory::OFFHAND_BEGIN;
        const int main = Inventory::HotbarToIndex(inventory.GetSelectedSlot());
        if (IsCrossbowHeldProjectile(inventory.GetSlot(off).itemId))  return off;
        if (IsCrossbowHeldProjectile(inventory.GetSlot(main).itemId)) return main;
        // Then the container in Player-inventory order — hotbar, main, the
        // armor slots, the off hand — arrows only.
        for (int h = 0; h < Inventory::HOTBAR_SIZE; ++h) {
            const int i = Inventory::HotbarToIndex(h);
            if (IsArrowItem(inventory.GetSlot(i).itemId)) return i;
        }
        for (int i = Inventory::MAIN_BEGIN; i < Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE; ++i) {
            if (IsArrowItem(inventory.GetSlot(i).itemId)) return i;
        }
        for (int i = Inventory::ARMOR_BEGIN; i < Inventory::ARMOR_BEGIN + Inventory::ARMOR_SIZE; ++i) {
            if (IsArrowItem(inventory.GetSlot(i).itemId)) return i;
        }
        if (IsArrowItem(inventory.GetSlot(off).itemId)) return off;
        return -1;
    }

    bool CanDrawCrossbow(const Inventory& inventory, bool creative) {
        return creative || FindCrossbowProjectileSlot(inventory) >= 0;
    }

    // ── Registration ──────────────────────────────────────────────────────

    void RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems) {
        if (auto it = pureItems.find(Items::FireworkRocket); it != pureItems.end()) {
            Item& rocket = it->second;
            rocket.useOn = &UseOn_FireworkRocket;
            rocket.use   = &Use_FireworkRocket;
            // Items.java: .component(FIREWORKS, new Fireworks(1, List.of())).
            Fireworks fireworks;
            fireworks.flightDuration = 1;
            rocket.defaultComponents.set(DataComponents::FIREWORKS, fireworks);
        }
        if (auto it = pureItems.find(Items::Crossbow); it != pureItems.end()) {
            Item& crossbow = it->second;
            crossbow.use          = &Use_Crossbow;
            crossbow.useDuration  = 72000;                       // CrossbowItem.getUseDuration
            crossbow.useAnimation = ItemUseAnimation::CROSSBOW;  // getUseAnimation
            crossbow.onUseTick    = &UseTick_Crossbow;
            crossbow.useOnRelease = true;                        // useOnRelease
            crossbow.stackSprite  = &Sprite_Crossbow;
            // Items.java: .component(CHARGED_PROJECTILES, EMPTY).
            crossbow.defaultComponents.set(DataComponents::CHARGED_PROJECTILES, ChargedProjectiles{});
        }
        // The bow's pulling frames (its draw is ItemBehaviors.cpp's).
        if (auto it = pureItems.find(Items::Bow); it != pureItems.end()) {
            it->second.stackSprite = &Sprite_Bow;
        }
    }

} // namespace Game::FireworkItems
