#include "levelgen/flat/FlatLevelGeneratorSettings.h"

#include "data/worldgen/BiomeFeatureRegistry.h"
#include "data/worldgen/placement/MiscOverworldPlacements.h"
#include "levelgen/GenerationStep.h"
#include "levelgen/Heightmap.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/placement/PlacedFeature.h"
#include "world/level/block/Blocks.h"

#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

// Reference: net/minecraft/world/level/levelgen/flat/*.java +
// PresetFlatWorldScreen.java (string format). Layer stacks are bottom-first.

namespace minecraft {
namespace levelgen {
namespace flat {

using world::level::block::Blocks;
using world::level::block::Block;

namespace {

// Reference: DimensionType.Y_SIZE = (1 << BlockPos.PACKED_Y_LENGTH) - 32.
constexpr int32_t Y_SIZE = 4064;

// Owned storage for FILL_LAYER inline placed features (PlacementUtils.
// inlinePlaced: a PlacedFeature with no modifiers). Never freed - settings
// hand out raw pointers into the feature system like the data registries do.
struct FillLayerStore {
    FillLayerFeature fillLayerFeature;
    std::deque<LayerConfiguration> configs;
    std::deque<std::unique_ptr<ConfiguredFeature>> configured;
    std::deque<std::unique_ptr<placement::PlacedFeature>> placed;

    const placement::PlacedFeature* inlineFillLayer(int32_t height, minecraft::BlockState* state) {
        configs.emplace_back(height, state);
        configured.push_back(std::make_unique<
            ConfiguredFeatureImpl<LayerConfiguration, FillLayerFeature>>(
            &fillLayerFeature, configs.back()));
        placed.push_back(std::make_unique<placement::PlacedFeature>(
            configured.back().get(), std::vector<placement::PlacementModifier*>{},
            "inline:fill_layer"));
        return placed.back().get();
    }
};

FillLayerStore& fillLayerStore() {
    static FillLayerStore store;
    return store;
}

} // namespace

FlatLayerInfo::FlatLayerInfo(int32_t h, const std::string& id)
    : height(h), blockId(id), block(Blocks::getBlock(id)) {
    if (block == nullptr) {
        throw std::runtime_error("flat layer references unregistered block: " + id);
    }
}

std::string FlatLayerInfo::toString() const {
    // Reference: FlatLayerInfo.toString().
    return (height != 1 ? std::to_string(height) + "*" : "") + blockId;
}

FlatLevelGeneratorSettings FlatLevelGeneratorSettings::make(
        std::optional<std::vector<std::string>> structureOverrides,
        const std::string& biome, bool decoration, bool addLakes,
        std::vector<FlatLayerInfo> layersBottomFirst) {
    FlatLevelGeneratorSettings s;
    s.m_structureOverrides = std::move(structureOverrides);
    s.m_biome = biome;
    s.m_decoration = decoration;
    s.m_addLakes = addLakes;
    s.m_layersInfo = std::move(layersBottomFirst);
    s.updateLayersAndAdjust();
    return s;
}

FlatLevelGeneratorSettings FlatLevelGeneratorSettings::getDefault() {
    // Reference: FlatLevelGeneratorSettings.getDefault().
    return make(std::vector<std::string>{"minecraft:strongholds", "minecraft:villages"},
                "minecraft:plains", /*decoration=*/false, /*addLakes=*/false,
                {FlatLayerInfo(1, "minecraft:bedrock"), FlatLayerInfo(2, "minecraft:dirt"),
                 FlatLayerInfo(1, "minecraft:grass_block")});
}

const std::vector<std::string>& FlatLevelGeneratorSettings::presetNames() {
    static const std::vector<std::string> names = {
        "classic_flat", "tunnelers_dream", "water_world", "overworld",
        "snowy_kingdom", "bottomless_pit", "desert", "redstone_ready", "the_void"};
    return names;
}

FlatLevelGeneratorSettings FlatLevelGeneratorSettings::preset(const std::string& name) {
    // Reference: FlatLevelGeneratorPresets.Bootstrap.run(). register() adds
    // the vararg layers in REVERSE, so the stacks below are written
    // bottom-first directly.
    auto L = [](int32_t h, const char* id) { return FlatLayerInfo(h, id); };
    if (name == "classic_flat")
        return make(std::vector<std::string>{"minecraft:villages"}, "minecraft:plains",
                    false, false,
                    {L(1, "minecraft:bedrock"), L(2, "minecraft:dirt"),
                     L(1, "minecraft:grass_block")});
    if (name == "tunnelers_dream")
        return make(std::vector<std::string>{"minecraft:mineshafts", "minecraft:strongholds"},
                    "minecraft:windswept_hills", true, false,
                    {L(1, "minecraft:bedrock"), L(230, "minecraft:stone"),
                     L(5, "minecraft:dirt"), L(1, "minecraft:grass_block")});
    if (name == "water_world")
        return make(std::vector<std::string>{"minecraft:ocean_ruins", "minecraft:shipwrecks",
                                             "minecraft:ocean_monuments"},
                    "minecraft:deep_ocean", false, false,
                    {L(1, "minecraft:bedrock"), L(64, "minecraft:deepslate"),
                     L(5, "minecraft:stone"), L(5, "minecraft:dirt"),
                     L(5, "minecraft:gravel"), L(90, "minecraft:water")});
    if (name == "overworld")
        return make(std::vector<std::string>{"minecraft:villages", "minecraft:mineshafts",
                                             "minecraft:pillager_outposts",
                                             "minecraft:ruined_portals",
                                             "minecraft:strongholds"},
                    "minecraft:plains", true, true,
                    {L(1, "minecraft:bedrock"), L(59, "minecraft:stone"),
                     L(3, "minecraft:dirt"), L(1, "minecraft:grass_block")});
    if (name == "snowy_kingdom")
        return make(std::vector<std::string>{"minecraft:villages", "minecraft:igloos"},
                    "minecraft:snowy_plains", false, false,
                    {L(1, "minecraft:bedrock"), L(59, "minecraft:stone"),
                     L(3, "minecraft:dirt"), L(1, "minecraft:grass_block"),
                     L(1, "minecraft:snow")});
    if (name == "bottomless_pit")
        return make(std::vector<std::string>{"minecraft:villages"}, "minecraft:plains",
                    false, false,
                    {L(2, "minecraft:cobblestone"), L(3, "minecraft:dirt"),
                     L(1, "minecraft:grass_block")});
    if (name == "desert")
        return make(std::vector<std::string>{"minecraft:villages", "minecraft:desert_pyramids",
                                             "minecraft:mineshafts", "minecraft:strongholds"},
                    "minecraft:desert", true, false,
                    {L(1, "minecraft:bedrock"), L(3, "minecraft:stone"),
                     L(52, "minecraft:sandstone"), L(8, "minecraft:sand")});
    if (name == "redstone_ready")
        return make(std::vector<std::string>{}, "minecraft:desert", false, false,
                    {L(1, "minecraft:bedrock"), L(3, "minecraft:stone"),
                     L(116, "minecraft:sandstone")});
    if (name == "the_void")
        return make(std::vector<std::string>{}, "minecraft:the_void", true, false,
                    {L(1, "minecraft:air")});
    throw std::runtime_error("unknown flat preset: " + name);
}

FlatLevelGeneratorSettings FlatLevelGeneratorSettings::fromString(
        const std::string& definition, const FlatLevelGeneratorSettings& base) {
    // Reference: PresetFlatWorldScreen.fromString().
    std::vector<std::string> parts;
    {
        std::stringstream ss(definition);
        std::string part;
        while (std::getline(ss, part, ';')) parts.push_back(part);
    }
    if (parts.empty()) return getDefault();

    // Reference: getLayersInfoFromString - any bad layer -> empty -> default.
    std::vector<FlatLayerInfo> layers;
    {
        std::stringstream ss(parts[0]);
        std::string spec;
        int32_t firstFree = 0;
        while (std::getline(ss, spec, ',')) {
            // Reference: getLayerInfoFromString - Splitter.on('*').limit(2).
            std::string blockId = spec;
            int64_t height = 1;
            auto star = spec.find('*');
            if (star != std::string::npos) {
                try {
                    height = std::max<int64_t>(std::stoll(spec.substr(0, star)), 0);
                } catch (...) {
                    return getDefault();
                }
                blockId = spec.substr(star + 1);
            }
            if (blockId.find(':') == std::string::npos) blockId = "minecraft:" + blockId;
            Block* block = Blocks::getBlock(blockId);
            if (block == nullptr) {
                std::cerr << "[flat] unknown block in preset string: " << blockId << std::endl;
                return getDefault();
            }
            int32_t firstAbove = static_cast<int32_t>(
                std::min<int64_t>(firstFree + height, Y_SIZE));
            int32_t actualHeight = firstAbove - firstFree;
            int32_t maxHeight = Y_SIZE - firstFree;
            if (maxHeight > 0) {
                // heightLimited(maxHeight)
                FlatLayerInfo info(std::min(actualHeight, maxHeight), blockId);
                layers.push_back(info);
                firstFree += info.height;
            }
        }
        if (layers.empty()) return getDefault();
    }

    std::string biome = "minecraft:plains";
    if (parts.size() > 1 && !parts[1].empty()) {
        std::string biomeName = parts[1];
        if (biomeName.find(':') == std::string::npos) biomeName = "minecraft:" + biomeName;
        // Biomes::has() auto-create trap - validate against the real
        // vanilla biome list instead.
        if (data::worldgen::BiomeFeatureRegistry::isKnownBiomeKey(biomeName)) {
            biome = biomeName;
        } else {
            std::cerr << "[flat] invalid biome in preset string: " << parts[1]
                      << " (defaulting to plains)" << std::endl;
        }
    }

    // Reference: settings.withBiomeAndLayers(layers, settings.
    // structureOverrides(), biome) - flags and overrides carry from `base`.
    return make(base.m_structureOverrides, biome, base.m_decoration, base.m_addLakes,
                std::move(layers));
}

std::string FlatLevelGeneratorSettings::toString() const {
    // Reference: PresetFlatWorldScreen.save().
    std::string out;
    for (size_t i = 0; i < m_layersInfo.size(); ++i) {
        if (i > 0) out += ",";
        out += m_layersInfo[i].toString();
    }
    out += ";";
    out += m_biome;
    return out;
}

void FlatLevelGeneratorSettings::updateLayersAndAdjust() {
    // Layer adjustment reads placed-feature registries.
    data::worldgen::BiomeFeatureRegistry::bootstrap();

    // Reference: updateLayers().
    m_layers.clear();
    for (const FlatLayerInfo& info : m_layersInfo) {
        for (int32_t y = 0; y < info.height; ++y) {
            m_layers.push_back(info.block->defaultBlockState());
        }
    }
    m_voidGen = true;
    for (auto* s : m_layers) {
        if (s != Blocks::AIR->defaultBlockState()) { m_voidGen = false; break; }
    }

    // Reference: adjustGenerationSettings(this.biome).
    using GS = GenerationStep::Decoration;
    m_adjustedFeatures.assign(static_cast<size_t>(GenerationStep::DECORATION_COUNT), {});

    if (m_addLakes) {
        // Reference: the lakes list order (LAKE_LAVA_UNDERGROUND, LAKE_LAVA_SURFACE).
        m_adjustedFeatures[static_cast<size_t>(GS::LAKES)].push_back(
            data::worldgen::placement::MiscOverworldPlacements::LAKE_LAVA_UNDERGROUND);
        m_adjustedFeatures[static_cast<size_t>(GS::LAKES)].push_back(
            data::worldgen::placement::MiscOverworldPlacements::LAKE_LAVA_SURFACE);
    }

    bool biomeDecoration =
        (!m_voidGen || m_biome == "minecraft:the_void") && m_decoration;
    if (biomeDecoration) {
        const auto& biomeFeatures =
            data::worldgen::BiomeFeatureRegistry::getFeaturesForBiome(m_biome);
        for (size_t stepIndex = 0; stepIndex < biomeFeatures.size(); ++stepIndex) {
            if (stepIndex == static_cast<size_t>(GS::UNDERGROUND_STRUCTURES)) continue;
            if (stepIndex == static_cast<size_t>(GS::SURFACE_STRUCTURES)) continue;
            if (m_addLakes && stepIndex == static_cast<size_t>(GS::LAKES)) continue;
            if (stepIndex >= m_adjustedFeatures.size()) m_adjustedFeatures.resize(stepIndex + 1);
            for (const placement::PlacedFeature* f : biomeFeatures[stepIndex]) {
                m_adjustedFeatures[stepIndex].push_back(f);
            }
        }
    }

    // Reference: the non-opaque layer pass - MOTION_BLOCKING.isOpaque()
    // failures become null layers + inline FILL_LAYER features.
    auto motionBlockingOpaque =
        Heightmap::getOpaquePredicate(Heightmap::Types::MOTION_BLOCKING);
    for (size_t i = 0; i < m_layers.size(); ++i) {
        auto* layer = m_layers[i];
        if (layer != nullptr && !motionBlockingOpaque(layer)) {
            m_layers[i] = nullptr;
            m_adjustedFeatures[static_cast<size_t>(GS::TOP_LAYER_MODIFICATION)].push_back(
                fillLayerStore().inlineFillLayer(static_cast<int32_t>(i), layer));
        }
    }
}

} // namespace flat
} // namespace levelgen
} // namespace minecraft
