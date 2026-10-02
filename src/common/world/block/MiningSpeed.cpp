// File: src/common/world/block/MiningSpeed.cpp
#include "MiningSpeed.hpp"
#include "../../data/DataComponents.hpp"
#include <algorithm>

namespace Game {

    float GetItemDestroySpeed(const ItemStack& held, const Block& target) {
        // MC Item.getDestroySpeed(stack, state): the stack's TOOL
        // (Tool.getMiningSpeed — the first rule matching the block that
        // carries a speed, else default_mining_speed), 1.0 without one.
        if (held.IsEmpty()) return 1.0f;
        const auto tool = held.get(DataComponents::TOOL);
        return tool ? tool->GetMiningSpeed(target) : 1.0f;
    }

    float GetItemDestroySpeed(ItemID held, const Block& target) {
        return GetItemDestroySpeed(ItemStack(held, 1), target);
    }

    bool HasCorrectToolForDrops(const ItemStack& held, const Block& target) {
        // MC Player.hasCorrectToolForDrops: !requiresCorrectToolForDrops ||
        // mainHand.isCorrectToolForDrops (Tool.isCorrectForDrops — the first
        // rule matching the block that says either way).
        if (!target.requiresCorrectTool) return true;
        if (held.IsEmpty()) return false;
        const auto tool = held.get(DataComponents::TOOL);
        return tool && tool->IsCorrectForDrops(target);
    }

    bool HasCorrectToolForDrops(ItemID held, const Block& target) {
        return HasCorrectToolForDrops(ItemStack(held, 1), target);
    }

    float GetPlayerDestroySpeed(const ItemStack& held, const Block& target, bool onGround,
                                float effectMultiplier, bool eyeInWater,
                                float miningEfficiency, float submergedMiningSpeed) {
        float speed = GetItemDestroySpeed(held, target);
        // MC: `if (speed > 1.0F) speed += MINING_EFFICIENCY` — Efficiency
        // only helps a tool that is already fast on this block.
        // BLOCK_BREAK_SPEED is 1.0 for every player (no source modifies it
        // here).
        if (speed > 1.0f) speed += miningEfficiency;
        // Player.getDestroySpeed's two effect steps (MobEffectUtil
        // .hasDigSpeed → ×(1 + (amp + 1) · 0.2); MINING_FATIGUE → ×0.3^(amp + 1)),
        // folded by the caller into one factor.
        speed *= effectMultiplier;
        // MC: `if (isEyeInFluid(WATER)) speed *= SUBMERGED_MINING_SPEED` — the
        // attribute's base is 0.2; Aqua Affinity's ×5 total makes it 1.0.
        if (eyeInWater) speed *= submergedMiningSpeed;
        if (!onGround) speed /= 5.0f;
        return speed;
    }

    float GetDestroyProgressPerTick(const ItemStack& held, const Block& target, bool onGround,
                                    float effectMultiplier, bool eyeInWater,
                                    float miningEfficiency, float submergedMiningSpeed) {
        if (target.destroyTime < 0.0f) return 0.0f;       // unbreakable
        if (target.destroyTime <= 0.0f) return 1.0f;      // instant break
        const float playerSpeed = GetPlayerDestroySpeed(held, target, onGround, effectMultiplier,
                                                        eyeInWater, miningEfficiency,
                                                        submergedMiningSpeed);
        const float modifier = HasCorrectToolForDrops(held, target) ? 30.0f : 100.0f;
        return playerSpeed / target.destroyTime / modifier;
    }

    int GetDestroyStage(float progress) {
        if (progress <= 0.0f) return -1;
        int stage = static_cast<int>(progress * 10.0f);
        return std::clamp(stage, 0, 9);
    }

} // namespace Game
