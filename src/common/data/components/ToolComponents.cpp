// File: src/common/data/components/ToolComponents.cpp
#include "ToolComponents.hpp"

#include "common/entity/Inventory.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/tags/DataTags.hpp"

#include <limits>

namespace Game {

    namespace {

        bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
        }

        std::string_view StripMinecraftNs(std::string_view id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        // The mining level ToolMaterial's incorrect-for-drops tag stands for
        // (the number of needs_*_tool tags it lists); -1 for an unknown name.
        int MaterialLevel(std::string_view material) {
            if (material == "wooden" || material == "gold") return TierLevel(MiningTier::Wood);
            if (material == "stone" || material == "copper") return TierLevel(MiningTier::Stone);
            if (material == "iron")      return TierLevel(MiningTier::Iron);
            if (material == "diamond")   return TierLevel(MiningTier::Diamond);
            if (material == "netherite") return TierLevel(MiningTier::Netherite);
            if (material == "resonite")  return TierLevel(MiningTier::Resonite);
            return -1;
        }

        // The engine-data answer for a tool tag (see BlockInTag): nullopt
        // when the tag is not one the hardness data speaks for.
        std::optional<bool> EngineToolTag(const Block& block, std::string_view ns, std::string_view path) {
            constexpr std::string_view kMineable = "mineable/";
            if (ns == "minecraft" && path.rfind(kMineable, 0) == 0) {
                const std::string_view tool = path.substr(kMineable.size());
                ToolType type = ToolType::None;
                if (tool == "pickaxe")     type = ToolType::Pickaxe;
                else if (tool == "axe")    type = ToolType::Axe;
                else if (tool == "shovel") type = ToolType::Shovel;
                else if (tool == "hoe")    type = ToolType::Hoe;
                else return std::nullopt;
                return block.preferredTool == type;
            }
            // needs_<tier>_tool: the tier the block's minTier came from.
            if (path.rfind("needs_", 0) == 0 && EndsWith(path, "_tool")) {
                const std::string_view tier = path.substr(6, path.size() - 6 - 5);
                if (tier == "stone")    return block.minTier == MiningTier::Stone;
                if (tier == "iron")     return block.minTier == MiningTier::Iron;
                if (tier == "diamond")  return block.minTier == MiningTier::Diamond;
                if (tier == "resonite") return block.minTier == MiningTier::Resonite;
                return std::nullopt;
            }
            // incorrect_for_<material>_tool = every needs_* tag above the
            // material's level (vanilla's tag files, in any namespace).
            if (path.rfind("incorrect_for_", 0) == 0 && EndsWith(path, "_tool")) {
                const int level = MaterialLevel(path.substr(14, path.size() - 14 - 5));
                if (level < 0) return std::nullopt;
                return static_cast<int>(TierLevel(block.minTier)) > level;
            }
            if (ns == "minecraft" && path == "leaves") return EndsWith(block.registrySlug, "_leaves");
            if (ns == "minecraft" && path == "wool")   return EndsWith(block.registrySlug, "_wool");
            return std::nullopt;
        }

        const Tool::Rule* FirstRule(const Tool& tool, const Block& block, bool wantSpeed) {
            for (const Tool::Rule& rule : tool.rules) {
                if (wantSpeed ? !rule.speed.has_value() : !rule.correctForDrops.has_value()) continue;
                if (BlockMatchesHolderSet(block, rule.blocks)) return &rule;
            }
            return nullptr;
        }

    } // namespace

    // ── Tool ────────────────────────────────────────────────────────────────

    float Tool::GetMiningSpeed(const Block& block) const {
        const Rule* rule = FirstRule(*this, block, /*wantSpeed=*/true);
        return rule ? *rule->speed : defaultMiningSpeed;
    }

    bool Tool::IsCorrectForDrops(const Block& block) const {
        const Rule* rule = FirstRule(*this, block, /*wantSpeed=*/false);
        return rule ? *rule->correctForDrops : false;
    }

    bool BlockInTag(const Block& block, std::string_view tag) {
        if (!tag.empty() && tag[0] == '#') tag.remove_prefix(1);
        std::string_view ns = "minecraft";
        std::string_view path = tag;
        if (const size_t colon = tag.find(':'); colon != std::string_view::npos) {
            ns = tag.substr(0, colon);
            path = tag.substr(colon + 1);
        }
        if (!block.registrySlug.empty() &&
            DataTags::HasTag(DataTags::Registry::Block, block.registrySlug, std::string(ns) + ":" + std::string(path))) {
            return true;
        }
        if (auto engine = EngineToolTag(block, ns, path)) return *engine;
        return false;
    }

    bool BlockMatchesHolderSet(const Block& block, const std::vector<std::string>& entries) {
        for (const std::string& entry : entries) {
            if (entry.empty()) continue;
            if (entry[0] == '#') {
                if (BlockInTag(block, entry)) return true;
                continue;
            }
            if (StripMinecraftNs(entry) == block.registrySlug) return true;
        }
        return false;
    }

    std::string IncorrectForDropsTag(MiningTier tier) {
        switch (tier) {
            case MiningTier::Wood:      return "#minecraft:incorrect_for_wooden_tool";
            case MiningTier::Stone:     return "#minecraft:incorrect_for_stone_tool";
            case MiningTier::Iron:      return "#minecraft:incorrect_for_iron_tool";
            case MiningTier::Diamond:   return "#minecraft:incorrect_for_diamond_tool";
            case MiningTier::Netherite: return "#minecraft:incorrect_for_netherite_tool";
            case MiningTier::Gold:      return "#minecraft:incorrect_for_gold_tool";
            case MiningTier::Resonite:  return "#obeycraft:incorrect_for_resonite_tool";
        }
        return "#minecraft:incorrect_for_wooden_tool";
    }

    std::string MineableTag(ToolType type) {
        switch (type) {
            case ToolType::Pickaxe: return "#minecraft:mineable/pickaxe";
            case ToolType::Axe:     return "#minecraft:mineable/axe";
            case ToolType::Shovel:  return "#minecraft:mineable/shovel";
            case ToolType::Hoe:     return "#minecraft:mineable/hoe";
            default:                return {};
        }
    }

    Tool MakeDiggerTool(std::string incorrectTag, std::string mineableTag, float speed) {
        Tool tool;
        tool.rules.push_back(Tool::Rule::DeniesDrops({std::move(incorrectTag)}));
        tool.rules.push_back(Tool::Rule::MinesAndDrops({std::move(mineableTag)}, speed));
        tool.defaultMiningSpeed = 1.0f;
        tool.damagePerBlock = 1;
        tool.canDestroyBlocksInCreative = true;
        return tool;
    }

    Tool MakeDiggerTool(ToolType type, MiningTier tier, float speed) {
        return MakeDiggerTool(IncorrectForDropsTag(tier), MineableTag(type), speed);
    }

    Tool MakeSwordTool() {
        Tool tool;
        tool.rules.push_back(Tool::Rule::MinesAndDrops({"minecraft:cobweb"}, 15.0f));
        tool.rules.push_back(Tool::Rule::OverrideSpeed({"#minecraft:sword_instantly_mines"},
                                                        std::numeric_limits<float>::max()));
        tool.rules.push_back(Tool::Rule::OverrideSpeed({"#minecraft:sword_efficient"}, 1.5f));
        tool.defaultMiningSpeed = 1.0f;
        tool.damagePerBlock = 2;
        tool.canDestroyBlocksInCreative = false;
        return tool;
    }

    Tool MakeShearsTool() {
        Tool tool;
        tool.rules.push_back(Tool::Rule::MinesAndDrops({"minecraft:cobweb"}, 15.0f));
        tool.rules.push_back(Tool::Rule::OverrideSpeed({"#minecraft:shears_extreme_breaking_speed"}, 15.0f));
        tool.rules.push_back(Tool::Rule::OverrideSpeed({"#minecraft:shears_major_breaking_speed"}, 5.0f));
        tool.rules.push_back(Tool::Rule::OverrideSpeed({"#minecraft:shears_minor_breaking_speed"}, 2.0f));
        tool.defaultMiningSpeed = 1.0f;
        tool.damagePerBlock = 1;
        tool.canDestroyBlocksInCreative = true;
        return tool;
    }

    bool CanDestroyBlockWith(const ItemStack& held, bool instabuild) {
        if (held.IsEmpty() || !instabuild) return true;
        const auto tool = held.get(DataComponents::TOOL);
        return !tool || tool->canDestroyBlocksInCreative;
    }

    // ── Glider ──────────────────────────────────────────────────────────────

    bool CanGlideUsing(const ItemStack& stack, EquipmentSlot slot) {
        if (stack.IsEmpty() || !stack.get(DataComponents::GLIDER)) return false;
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        return equippable && equippable->slot == slot && !NextDamageWillBreak(stack);
    }

    std::vector<EquipmentSlot> InventoryGliderSlots(const Inventory& inventory) {
        std::vector<EquipmentSlot> out;
        // EquipmentSlot.VALUES; a player has no BODY / SADDLE slot.
        static constexpr EquipmentSlot kSlots[] = {
            EquipmentSlot::MAINHAND, EquipmentSlot::OFFHAND, EquipmentSlot::FEET,
            EquipmentSlot::LEGS, EquipmentSlot::CHEST, EquipmentSlot::HEAD,
        };
        for (const EquipmentSlot slot : kSlots) {
            const int index = slot == EquipmentSlot::MAINHAND
                ? Inventory::HotbarToIndex(inventory.GetSelectedSlot())
                : InventoryIndexFor(slot);
            if (index < 0) continue;
            if (CanGlideUsing(inventory.GetSlot(index), slot)) out.push_back(slot);
        }
        return out;
    }

    // ── Equippable ──────────────────────────────────────────────────────────

    std::string EquipmentAssetPath(const ItemStack& stack) {
        if (stack.IsEmpty()) return {};
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        if (!equippable || equippable->assetId.empty()) return {};
        return std::string(StripMinecraftNs(equippable->assetId));
    }

    bool EquipmentAssetHasWings(std::string_view assetPath) {
        return StripMinecraftNs(assetPath) == "elytra";
    }

    std::string CameraOverlayTexturePath(std::string_view overlayId) {
        std::string_view ns = "minecraft";
        std::string_view path = overlayId;
        if (const size_t colon = overlayId.find(':'); colon != std::string_view::npos) {
            ns = overlayId.substr(0, colon);
            path = overlayId.substr(colon + 1);
        }
        if (path.empty()) return {};
        if (ns == "minecraft") return "assets/textures/" + std::string(path) + ".png";
        return "assets/" + std::string(ns) + "/textures/" + std::string(path) + ".png";
    }

    bool IsEquippableInPlayerSlot(const ItemStack& stack, EquipmentSlot slot) {
        const auto equippable = stack.get(DataComponents::EQUIPPABLE);
        // LivingEntity.isEquippableInSlot: with no EQUIPPABLE only the main
        // hand takes the stack — never an armour slot.
        if (!equippable) return slot == EquipmentSlot::MAINHAND;
        // canUseSlot: a player has no BODY / SADDLE slot.
        if (slot == EquipmentSlot::BODY || slot == EquipmentSlot::SADDLE) return false;
        return equippable->slot == slot && equippable->CanBeEquippedBy("minecraft:player");
    }

    // ── Damage resistance ───────────────────────────────────────────────────

    bool ItemStackCanBeHurtBy(const ItemStack& stack, std::string_view damageTypeId) {
        const auto resistant = stack.get(DataComponents::DAMAGE_RESISTANT);
        if (!resistant || resistant->empty()) return true;
        // DamageResistant.isResistantTo: source.is(types).
        return !DataTags::HasTag(DataTags::Registry::DamageType, damageTypeId, *resistant);
    }

} // namespace Game

namespace Game::DataComponents {

    namespace {
        // Unit.STREAM_CODEC: nothing on the wire — presence is the value.
        void SerUnitGlider(Network::PacketBuffer&, const bool&) {}
        bool DeUnitGlider(Network::PacketReader&) { return true; }
    }

    const DataComponentType<bool> GLIDER{"glider", 250, &SerUnitGlider, &DeUnitGlider};

} // namespace Game::DataComponents
