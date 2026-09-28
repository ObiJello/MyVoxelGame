// File: src/common/entity/PlayerArmPose.hpp
//
// MC AvatarRenderer.getArmPose — which HumanoidModel.ArmPose a player's arms
// take from what they hold and use. Shared by the server (it sends the two
// poses with every PlayerUpdateS2C, so other clients can pose a morphed
// player's humanoid body) and the client (the local player's own body in
// third person). The values are HumanoidModel.ArmPose ORDINALS:
//   0 EMPTY  1 ITEM  2 BLOCK  3 BOW_AND_ARROW  4 THROW_TRIDENT
//   5 CROSSBOW_CHARGE  6 CROSSBOW_HOLD  7 SPYGLASS  8 TOOT_HORN  9 BRUSH
//   10 SPEAR
//
// Every player here is right-handed (Avatar.getMainArm = RIGHT): the main
// hand is the right arm. CROSSBOW_HOLD is a loaded crossbow at rest
// (CrossbowItem.isCharged — CHARGED_PROJECTILES); the spear's usesSpearPose
// falls back to ITEM.
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/ItemUseAnimation.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/FireworkItems.hpp"   // CrossbowItem.getChargeDuration

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace Game {

    namespace PlayerArmPose {
        constexpr uint8_t kEmpty = 0, kItem = 1, kBlock = 2, kBowAndArrow = 3, kThrowTrident = 4,
                          kCrossbowCharge = 5, kCrossbowHold = 6, kSpyglass = 7, kTootHorn = 8,
                          kBrush = 9, kSpear = 10;

        // ItemTags.SPEARS (VanillaItemTagsProvider): the seven spears.
        inline bool IsSpear(ItemID id) {
            return id == Items::WoodenSpear || id == Items::StoneSpear || id == Items::CopperSpear ||
                   id == Items::IronSpear || id == Items::GoldenSpear || id == Items::DiamondSpear ||
                   id == Items::NetheriteSpear;
        }

        // MC ArmPose.isTwoHanded.
        inline bool IsTwoHanded(uint8_t pose) {
            return pose == kBowAndArrow || pose == kCrossbowCharge || pose == kCrossbowHold;
        }

        // MC CrossbowItem.getChargeDuration without Quick Charge: 1.25 s.
        constexpr float kCrossbowChargeTicks = 25.0f;
        // MC HumanoidRenderState.maxCrossbowChargeDuration for the item in
        // use: CrossbowItem.getChargeDuration (Quick Charge shortens it) —
        // floor(modified 1.25 s × 20), Quick Charge's −0.25 s a level.
        inline float MaxCrossbowCharge(const ItemStack& usedItem) {
            if (usedItem.IsEmpty() || usedItem.itemId != Items::Crossbow) return kCrossbowChargeTicks;
            return static_cast<float>(FireworkItems::CrossbowChargeDuration(usedItem));
        }

        // MC getArmPose(avatar, itemInHand, hand): `usingThisHand` is
        // `getUsedItemHand() == hand && getUseItemRemainingTicks() > 0`.
        // `swinging` is LivingEntity.isSwinging: a loaded crossbow drops its
        // aim for the length of a swing.
        inline uint8_t ForHand(const ItemStack& item, bool usingThisHand, bool swinging = false) {
            if (item.IsEmpty()) return kEmpty;
            // A loaded crossbow is held up to aim, ahead of any use pose
            // (AvatarRenderer.getArmPose: !isSwinging && isCharged).
            if (!swinging && item.itemId == Items::Crossbow) {
                if (auto charged = item.get(DataComponents::CHARGED_PROJECTILES); charged && !charged->IsEmpty()) {
                    return kCrossbowHold;
                }
            }
            if (usingThisHand) {
                switch (GetUseAnimation(item)) {
                    case ItemUseAnimation::BLOCK:     return kBlock;
                    case ItemUseAnimation::BOW:       return kBowAndArrow;
                    case ItemUseAnimation::TRIDENT:   return kThrowTrident;
                    case ItemUseAnimation::CROSSBOW:  return kCrossbowCharge;
                    case ItemUseAnimation::SPYGLASS:  return kSpyglass;
                    case ItemUseAnimation::TOOT_HORN: return kTootHorn;
                    case ItemUseAnimation::BRUSH:     return kBrush;
                    case ItemUseAnimation::SPEAR:     return kSpear;
                    default: break;
                }
            }
            // HumanoidMobRenderer.usesSpearPose: a spear (ItemTags.SPEARS) is
            // carried levelled — SPEAR — whether or not it is used.
            return IsSpear(item.itemId) ? kSpear : kItem;
        }

        // MC getArmPose(avatar, arm) for both arms of a right-handed player:
        // a two-handed main-hand pose leaves the off hand EMPTY or ITEM.
        // `usedHand` is 0 for the main hand, 1 for the off hand.
        inline void Compute(const ItemStack& mainHand, const ItemStack& offHand, bool usingItem,
                            uint32_t usedHand, uint8_t& rightArm, uint8_t& leftArm,
                            bool swinging = false) {
            const uint8_t mainPose = ForHand(mainHand, usingItem && usedHand == 0, swinging);
            uint8_t offPose = ForHand(offHand, usingItem && usedHand == 1, swinging);
            if (IsTwoHanded(mainPose)) offPose = offHand.IsEmpty() ? kEmpty : kItem;
            rightArm = mainPose;
            leftArm  = offPose;
        }
    } // namespace PlayerArmPose

} // namespace Game
