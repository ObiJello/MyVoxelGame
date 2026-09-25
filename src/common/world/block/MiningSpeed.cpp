// File: src/common/world/block/MiningSpeed.cpp
#include "MiningSpeed.hpp"
#include "../../data/DataComponents.hpp"
#include <algorithm>

namespace Game {

    float GetItemDestroySpeed(ItemID held, const Block& target) {
        if (held == Items::Air) return 1.0f;

        const Item& item = ItemRegistry::Get(held);
        auto toolOpt = item.defaultComponents.get(DataComponents::TOOL);
        if (!toolOpt.has_value()) return 1.0f;
        const Tool& tool = *toolOpt;

        // MC: shears get a flat 1.5 vs most blocks, 15.0 vs wool/leaves/web.
        if (tool.type == ToolType::Shears) {
            const std::string& n = target.modelName.empty() ? target.name : target.modelName;
            if (n.find("_leaves") != std::string::npos ||
                n.find("_wool")   != std::string::npos ||
                n.find("cobweb")  != std::string::npos) {
                return 15.0f;
            }
            return tool.miningSpeed;
        }

        // MC: swords mine cobweb fast (15.0) and bamboo (variable).
        if (tool.type == ToolType::Sword) {
            const std::string& n = target.modelName.empty() ? target.name : target.modelName;
            if (n.find("cobweb") != std::string::npos) return 15.0f;
            if (n.find("bamboo") != std::string::npos) return tool.miningSpeed;
            // Swords don't get the bonus on other blocks — return 1.5 (MC's
            // baseline for "sword bare hand penalty avoided") only if the
            // block's preferred tool isn't Sword. Otherwise return tool speed.
            return (target.preferredTool == ToolType::Sword)
                ? tool.miningSpeed : 1.5f;
        }

        // Tool matches the block's preferred tool → get the speed boost.
        if (tool.type == target.preferredTool) {
            return tool.miningSpeed;
        }
        return 1.0f;
    }

    bool HasCorrectToolForDrops(ItemID held, const Block& target) {
        if (!target.requiresCorrectTool) return true;

        if (held == Items::Air) return false;
        const Item& item = ItemRegistry::Get(held);
        auto toolOpt = item.defaultComponents.get(DataComponents::TOOL);
        if (!toolOpt.has_value()) return false;
        const Tool& tool = *toolOpt;

        if (tool.type != target.preferredTool) return false;
        return TierLevel(tool.tier) >= TierLevel(target.minTier);
    }

    float GetPlayerDestroySpeed(ItemID held, const Block& target, bool onGround,
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

    float GetDestroyProgressPerTick(ItemID held, const Block& target, bool onGround,
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
