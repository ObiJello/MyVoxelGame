#include "levelgen/material/MaterialRules.h"

#include "levelgen/density/WorldgenRegistries.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <limits>
#include <stdexcept>
#include <utility>

// Reference: levelgen.material.MaterialRules (26.3) - the static builders.

namespace minecraft {
namespace levelgen {
namespace material {
namespace MaterialRules {

namespace {

std::string normalize(const std::string& key) {
    return density::WorldgenRegistries::normalizeKey(key);
}

} // namespace

MaterialRulePtr getRule(const std::string& key) {
    const std::string id = normalize(key);
    return std::make_shared<RuleReference>(id, MaterialRuleRegistry::get().rule(id));
}

MaterialConditionPtr getCondition(const std::string& key) {
    const std::string id = normalize(key);
    return std::make_shared<ConditionReference>(id, MaterialRuleRegistry::get().condition(id));
}

MaterialConditionPtr stoneDepthCheck(int32_t offset, bool addSurfaceDepth, CaveSurface surfaceType) {
    return std::make_shared<StoneDepthCondition>(offset, addSurfaceDepth, 0, surfaceType);
}

MaterialConditionPtr stoneDepthCheck(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange,
                                     CaveSurface surfaceType) {
    return std::make_shared<StoneDepthCondition>(offset, addSurfaceDepth, secondaryDepthRange, surfaceType);
}

MaterialConditionPtr not_(MaterialConditionPtr target) {
    return std::make_shared<NotCondition>(std::move(target));
}

MaterialConditionPtr yBlockCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier) {
    return std::make_shared<YCondition>(anchor, surfaceDepthMultiplier, false);
}

MaterialConditionPtr yStartCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier) {
    return std::make_shared<YCondition>(anchor, surfaceDepthMultiplier, true);
}

MaterialConditionPtr waterBlockCheck(int32_t offset, int32_t surfaceDepthMultiplier) {
    return std::make_shared<WaterCondition>(offset, surfaceDepthMultiplier, false);
}

MaterialConditionPtr waterStartCheck(int32_t offset, int32_t surfaceDepthMultiplier) {
    return std::make_shared<WaterCondition>(offset, surfaceDepthMultiplier, true);
}

MaterialConditionPtr isBiome(std::vector<std::string> biomes) {
    for (std::string& biome : biomes) biome = normalize(biome);
    return std::make_shared<BiomeCondition>(BiomeSet::direct(std::move(biomes)));
}

MaterialConditionPtr noiseCondition2d(const std::string& noise, double minRange) {
    return noiseCondition2d(noise, minRange, std::numeric_limits<double>::max());
}

MaterialConditionPtr noiseCondition2d(const std::string& noise, double minRange, double maxRange) {
    return std::make_shared<NoiseThresholdCondition>(normalize(noise), minRange, maxRange, false);
}

MaterialConditionPtr noiseCondition3d(const std::string& noise, double minRange) {
    return noiseCondition3d(noise, minRange, std::numeric_limits<double>::max());
}

MaterialConditionPtr noiseCondition3d(const std::string& noise, double minRange, double maxRange) {
    return std::make_shared<NoiseThresholdCondition>(normalize(noise), minRange, maxRange, true);
}

MaterialConditionPtr verticalGradient(const std::string& randomName, const VerticalAnchor& trueAtAndBelow,
                                      const VerticalAnchor& falseAtAndAbove) {
    // Identifier.parse(randomName).
    return std::make_shared<VerticalGradientCondition>(normalize(randomName), trueAtAndBelow, falseAtAndAbove);
}

MaterialConditionPtr steep() {
    return SteepCondition::instance();
}

MaterialConditionPtr hole() {
    return HoleCondition::instance();
}

MaterialConditionPtr abovePreliminarySurface() {
    return AbovePreliminarySurfaceCondition::instance();
}

MaterialConditionPtr temperature() {
    return TemperatureCondition::instance();
}

MaterialRulePtr ifTrue(MaterialConditionPtr condition, MaterialRulePtr next) {
    return std::make_shared<ConditionRule>(std::move(condition), std::move(next));
}

MaterialRulePtr sequence(std::vector<MaterialRulePtr> rules) {
    if (rules.empty()) throw std::invalid_argument("Need at least 1 rule for a sequence");
    return std::make_shared<SequenceRule>(std::move(rules));
}

MaterialRulePtr state(BlockState* state) {
    return std::make_shared<BlockRule>(state);
}

MaterialRulePtr state(const std::string& blockName) {
    const std::string id = normalize(blockName);
    BlockState* blockState = world::level::block::Blocks::getDefaultState(id);
    if (blockState == nullptr) throw std::runtime_error("MaterialRules::state: unknown block " + id);
    return std::make_shared<BlockRule>(blockState);
}

MaterialRulePtr bandlands() {
    return BandlandsRule::instance();
}

} // namespace MaterialRules
} // namespace material
} // namespace levelgen
} // namespace minecraft
