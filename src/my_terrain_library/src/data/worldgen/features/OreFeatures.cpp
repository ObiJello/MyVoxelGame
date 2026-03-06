#include "data/worldgen/features/OreFeatures.h"

// Reference: net/minecraft/data/worldgen/features/OreFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace levelgen::structure::templatesystem;

// Static members
OreFeature OreFeatures::s_oreFeature;
std::shared_ptr<TagMatchTest> OreFeatures::s_stoneOreReplaceables;
std::shared_ptr<TagMatchTest> OreFeatures::s_deepslateOreReplaceables;
std::shared_ptr<TagMatchTest> OreFeatures::s_naturalStone;
std::shared_ptr<TagMatchTest> OreFeatures::s_netherrack;
std::shared_ptr<TagMatchTest> OreFeatures::s_netherOreReplaceables;
bool OreFeatures::s_initialized = false;

// Configured feature pointers
ConfiguredFeature* OreFeatures::ORE_COAL = nullptr;
ConfiguredFeature* OreFeatures::ORE_COAL_BURIED = nullptr;
ConfiguredFeature* OreFeatures::ORE_IRON = nullptr;
ConfiguredFeature* OreFeatures::ORE_IRON_SMALL = nullptr;
ConfiguredFeature* OreFeatures::ORE_GOLD = nullptr;
ConfiguredFeature* OreFeatures::ORE_GOLD_BURIED = nullptr;
ConfiguredFeature* OreFeatures::ORE_REDSTONE = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIAMOND_SMALL = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIAMOND_MEDIUM = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIAMOND_LARGE = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIAMOND_BURIED = nullptr;
ConfiguredFeature* OreFeatures::ORE_LAPIS = nullptr;
ConfiguredFeature* OreFeatures::ORE_LAPIS_BURIED = nullptr;
ConfiguredFeature* OreFeatures::ORE_EMERALD = nullptr;
ConfiguredFeature* OreFeatures::ORE_COPPER_SMALL = nullptr;
ConfiguredFeature* OreFeatures::ORE_COPPER_LARGE = nullptr;
ConfiguredFeature* OreFeatures::ORE_INFESTED = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIRT = nullptr;
ConfiguredFeature* OreFeatures::ORE_GRAVEL = nullptr;
ConfiguredFeature* OreFeatures::ORE_GRANITE = nullptr;
ConfiguredFeature* OreFeatures::ORE_DIORITE = nullptr;
ConfiguredFeature* OreFeatures::ORE_ANDESITE = nullptr;
ConfiguredFeature* OreFeatures::ORE_TUFF = nullptr;
ConfiguredFeature* OreFeatures::ORE_CLAY = nullptr;

// Storage for ConfiguredFeatureImpl instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_configuredFeatures;

OreConfiguration OreFeatures::createOreConfig(
    BlockState* stoneOre,
    BlockState* deepslateOre,
    int32_t size,
    float discardChance
) {
    std::vector<OreConfiguration::TargetBlockState> targets = {
        OreConfiguration::target(s_stoneOreReplaceables, stoneOre),
        OreConfiguration::target(s_deepslateOreReplaceables, deepslateOre)
    };
    return OreConfiguration(targets, size, discardChance);
}

OreConfiguration OreFeatures::createSingleOreConfig(
    std::shared_ptr<RuleTest> target,
    BlockState* ore,
    int32_t size,
    float discardChance
) {
    std::vector<OreConfiguration::TargetBlockState> targets = {
        OreConfiguration::target(target, ore)
    };
    return OreConfiguration(targets, size, discardChance);
}

void OreFeatures::bootstrap() {
    if (s_initialized) return;

    // Initialize RuleTests
    // Reference: OreFeatures.java lines 50-54
    s_stoneOreReplaceables = std::make_shared<TagMatchTest>("minecraft:stone_ore_replaceables");
    s_deepslateOreReplaceables = std::make_shared<TagMatchTest>("minecraft:deepslate_ore_replaceables");
    s_naturalStone = std::make_shared<TagMatchTest>("minecraft:base_stone_overworld");
    s_netherrack = std::make_shared<TagMatchTest>("minecraft:netherrack");
    s_netherOreReplaceables = std::make_shared<TagMatchTest>("minecraft:base_stone_nether");

    // Helper to create and store a configured feature
    auto createFeature = [](const OreConfiguration& config) -> ConfiguredFeature* {
        auto feature = std::make_unique<ConfiguredFeatureImpl<OreConfiguration, OreFeature>>(
            &s_oreFeature,
            config
        );
        ConfiguredFeature* ptr = feature.get();
        s_configuredFeatures.push_back(std::move(feature));
        return ptr;
    };

    // Helper to get blocks by name using dynamic lookup
    auto block = [](const char* name) -> BlockState* {
        return minecraft::world::level::block::Blocks::getDefaultState(name);
    };

    // Reference: OreFeatures.java lines 67-92 (bootstrap method)
    // Coal - size 17
    ORE_COAL = createFeature(createOreConfig(
        block("minecraft:coal_ore"),
        block("minecraft:deepslate_coal_ore"),
        17, 0.0f
    ));

    ORE_COAL_BURIED = createFeature(createOreConfig(
        block("minecraft:coal_ore"),
        block("minecraft:deepslate_coal_ore"),
        17, 0.5f
    ));

    // Iron - size 9 and 4
    ORE_IRON = createFeature(createOreConfig(
        block("minecraft:iron_ore"),
        block("minecraft:deepslate_iron_ore"),
        9, 0.0f
    ));

    ORE_IRON_SMALL = createFeature(createOreConfig(
        block("minecraft:iron_ore"),
        block("minecraft:deepslate_iron_ore"),
        4, 0.0f
    ));

    // Gold - size 9
    ORE_GOLD = createFeature(createOreConfig(
        block("minecraft:gold_ore"),
        block("minecraft:deepslate_gold_ore"),
        9, 0.0f
    ));

    ORE_GOLD_BURIED = createFeature(createOreConfig(
        block("minecraft:gold_ore"),
        block("minecraft:deepslate_gold_ore"),
        9, 0.5f
    ));

    // Redstone - size 8
    ORE_REDSTONE = createFeature(createOreConfig(
        block("minecraft:redstone_ore"),
        block("minecraft:deepslate_redstone_ore"),
        8, 0.0f
    ));

    // Diamond - sizes 4, 8, 12
    ORE_DIAMOND_SMALL = createFeature(createOreConfig(
        block("minecraft:diamond_ore"),
        block("minecraft:deepslate_diamond_ore"),
        4, 0.5f
    ));

    ORE_DIAMOND_MEDIUM = createFeature(createOreConfig(
        block("minecraft:diamond_ore"),
        block("minecraft:deepslate_diamond_ore"),
        8, 0.5f
    ));

    ORE_DIAMOND_LARGE = createFeature(createOreConfig(
        block("minecraft:diamond_ore"),
        block("minecraft:deepslate_diamond_ore"),
        12, 0.7f
    ));

    ORE_DIAMOND_BURIED = createFeature(createOreConfig(
        block("minecraft:diamond_ore"),
        block("minecraft:deepslate_diamond_ore"),
        8, 1.0f
    ));

    // Lapis - size 7
    ORE_LAPIS = createFeature(createOreConfig(
        block("minecraft:lapis_ore"),
        block("minecraft:deepslate_lapis_ore"),
        7, 0.0f
    ));

    ORE_LAPIS_BURIED = createFeature(createOreConfig(
        block("minecraft:lapis_ore"),
        block("minecraft:deepslate_lapis_ore"),
        7, 1.0f
    ));

    // Emerald - size 3
    ORE_EMERALD = createFeature(createOreConfig(
        block("minecraft:emerald_ore"),
        block("minecraft:deepslate_emerald_ore"),
        3, 0.0f
    ));

    // Copper - sizes 10 and 20
    ORE_COPPER_SMALL = createFeature(createOreConfig(
        block("minecraft:copper_ore"),
        block("minecraft:deepslate_copper_ore"),
        10, 0.0f
    ));

    ORE_COPPER_LARGE = createFeature(createOreConfig(
        block("minecraft:copper_ore"),
        block("minecraft:deepslate_copper_ore"),
        20, 0.0f
    ));

    // Infested stone - size 9
    ORE_INFESTED = createFeature(createOreConfig(
        block("minecraft:infested_stone"),
        block("minecraft:infested_deepslate"),
        9, 0.0f
    ));

    // Stone variants and dirt/gravel - natural stone replacement
    ORE_DIRT = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:dirt"),
        33, 0.0f
    ));

    ORE_GRAVEL = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:gravel"),
        33, 0.0f
    ));

    ORE_GRANITE = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:granite"),
        64, 0.0f
    ));

    ORE_DIORITE = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:diorite"),
        64, 0.0f
    ));

    ORE_ANDESITE = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:andesite"),
        64, 0.0f
    ));

    ORE_TUFF = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:tuff"),
        64, 0.0f
    ));

    ORE_CLAY = createFeature(createSingleOreConfig(
        s_naturalStone,
        block("minecraft:clay"),
        33, 0.0f
    ));

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
