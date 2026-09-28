// File: src/common/entity/raid/OminousBanner.cpp
//
// See OminousBanner.hpp. Reference: minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/world/entity/raid/Raid.java (getBannerComponentPatch).
#include "common/entity/raid/OminousBanner.hpp"

#include "common/data/DataComponents.hpp"
#include "common/text/Language.hpp"
#include "common/world/block/Blocks.hpp"

namespace Game::Raid {

    namespace {
        // DyeColor ordinals (white 0 … black 15).
        constexpr uint8_t kLightGray = 8;
        constexpr uint8_t kGray      = 7;
        constexpr uint8_t kCyan      = 9;
        constexpr uint8_t kBlack     = 15;

        const ItemStack& Template() {
            static const ItemStack s_banner = [] {
                ItemStack stack(ItemRegistry::FromBlock(BlockID::WhiteBanner), 1);
                // BannerPatternLayers.Builder.addIfRegistered, in MC's order
                // (the vanilla pack registers all seven patterns).
                BannerPatternLayers layers;
                layers.layers = {
                    { "minecraft:rhombus",         kCyan },
                    { "minecraft:stripe_bottom",   kLightGray },
                    { "minecraft:stripe_center",   kGray },
                    { "minecraft:border",          kLightGray },
                    { "minecraft:stripe_middle",   kBlack },
                    { "minecraft:half_horizontal", kLightGray },
                    { "minecraft:circle",          kLightGray },
                    { "minecraft:border",          kBlack },
                };
                stack.components.set(DataComponents::BANNER_PATTERNS, std::move(layers));
                stack.components.set(DataComponents::ITEM_NAME,
                                     Language::GetOrDefault(kOminousBannerNameKey, "Ominous Banner"));
                stack.components.set(DataComponents::RARITY, Rarity::UNCOMMON);
                return stack;
            }();
            return s_banner;
        }
    }

    ItemStack GetOminousBannerInstance() {
        return Template();
    }

    bool IsOminousBanner(const ItemStack& stack) {
        return ItemStacksMatch(stack, Template());
    }

} // namespace Game::Raid
