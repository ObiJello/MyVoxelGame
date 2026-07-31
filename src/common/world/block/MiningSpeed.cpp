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

    float GetPlayerDestroySpeed(ItemID held, const Block& target, bool onGround) {
        float speed = GetItemDestroySpeed(held, target);
        // TODO: efficiency enchant, haste / mining-fatigue effects, attribute.
        // TODO: submerged (in-water) ÷5 once water is mining-aware.
        if (!onGround) speed /= 5.0f;
        return speed;
    }

    float GetDestroyProgressPerTick(ItemID held, const Block& target, bool onGround) {
        if (target.destroyTime < 0.0f) return 0.0f;       // unbreakable
        if (target.destroyTime <= 0.0f) return 1.0f;      // instant break
        const float playerSpeed = GetPlayerDestroySpeed(held, target, onGround);
        const float modifier = HasCorrectToolForDrops(held, target) ? 30.0f : 100.0f;
        return playerSpeed / target.destroyTime / modifier;
    }

    int GetDestroyStage(float progress) {
        if (progress <= 0.0f) return -1;
        int stage = static_cast<int>(progress * 10.0f);
        return std::clamp(stage, 0, 9);
    }

} // namespace Game
