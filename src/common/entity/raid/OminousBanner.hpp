// File: src/common/entity/raid/OminousBanner.hpp
//
// MC Raid.getOminousBannerInstance / getBannerComponentPatch — the illager
// captain's banner: a white banner with the eight fixed pattern layers
// (rhombus cyan, stripe_bottom light grey, stripe_center grey, border light
// grey, stripe_middle black, half_horizontal light grey, circle light grey,
// border black), item_name "block.minecraft.ominous_banner" and UNCOMMON
// rarity (MC also hides the pattern tooltip via TOOLTIP_DISPLAY, a component
// this engine does not carry — its tooltip never lists banner layers).
//
// A patrol leader wears it on its head (PatrollingMonster.finalizeSpawn,
// drop chance 2.0 — a guaranteed, undamaged drop), and Raider.isCaptain /
// the pillager loot table's captain predicate compare the head slot against
// it with ItemStack.matches.
#pragma once

#include "common/entity/Item.hpp"

namespace Game::Raid {

    // MC Raid.getOminousBannerInstance — a fresh one-count stack.
    ItemStack GetOminousBannerInstance();

    // MC ItemStack.matches(stack, getOminousBannerInstance()): same item,
    // same components, count 1 (a head slot only ever holds one).
    bool IsOminousBanner(const ItemStack& stack);

    // The translation key MC's item_name component carries; the engine's
    // ITEM_NAME is display text, so the NBT writer uses this to save the
    // banner's name as MC's translatable component.
    inline constexpr const char* kOminousBannerNameKey = "block.minecraft.ominous_banner";

} // namespace Game::Raid
