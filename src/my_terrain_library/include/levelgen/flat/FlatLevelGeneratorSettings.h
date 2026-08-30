#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/flat/FlatLayerInfo.java,
// FlatLevelGeneratorSettings.java, FlatLevelGeneratorPresets.java, and the
// preset-string format of client/gui/screens/PresetFlatWorldScreen.java.

namespace minecraft {

namespace world { namespace level { namespace block {
class Block;
namespace state { class BlockState; }
} } }
namespace levelgen { namespace placement { class PlacedFeature; } }

namespace levelgen {
namespace flat {

using FlatBlockState = world::level::block::state::BlockState;

/**
 * FlatLayerInfo - one run of identical blocks in a flat world's layer stack.
 * Reference: FlatLayerInfo.java. Layers are stored BOTTOM-first (the order
 * the preset string lists them and the order Java's layersInfo holds them).
 */
struct FlatLayerInfo {
    int32_t height = 1;
    std::string blockId;                       // e.g. "minecraft:bedrock"
    world::level::block::Block* block = nullptr;

    FlatLayerInfo() = default;
    FlatLayerInfo(int32_t h, const std::string& id);

    // Reference: FlatLayerInfo.toString() - "<height>*<id>" or "<id>".
    std::string toString() const;
};

/**
 * FlatLevelGeneratorSettings - superflat world configuration.
 * Reference: FlatLevelGeneratorSettings.java.
 *
 * The adjusted per-step feature lists (Java adjustGenerationSettings) are
 * computed EAGERLY at construction here, where Java computes them lazily on
 * the first feature query. The lazily-timed variant makes non-opaque layers
 * (e.g. snowy_kingdom's snow) placement-order-dependent in vanilla (layers
 * are nulled when the memoized getter first runs, racing fillFromNoise of
 * other chunks); eager adjustment is the deterministic steady state.
 */
class FlatLevelGeneratorSettings {
public:
    // Reference: FlatLevelGeneratorSettings.getDefault() - strongholds +
    // villages overrides, plains, [bedrock, 2*dirt, grass_block].
    static FlatLevelGeneratorSettings getDefault();

    /**
     * Reference: FlatLevelGeneratorPresets - the 9 vanilla presets by short
     * name (classic_flat, tunnelers_dream, water_world, overworld,
     * snowy_kingdom, bottomless_pit, desert, redstone_ready, the_void).
     * Throws on an unknown name.
     */
    static FlatLevelGeneratorSettings preset(const std::string& name);
    static const std::vector<std::string>& presetNames();

    /**
     * Reference: PresetFlatWorldScreen.fromString() - "<layers>;<biome>",
     * layers = comma list of "<height>*<id>"|"<id>" bottom-first. Unparseable
     * layers -> getDefault(); unknown biome -> plains (with a warning).
     * Structure overrides and the lakes/decoration flags carry over from
     * `base` (the currently-selected preset), exactly like the vanilla screen.
     */
    static FlatLevelGeneratorSettings fromString(const std::string& definition,
                                                 const FlatLevelGeneratorSettings& base);

    // Reference: PresetFlatWorldScreen.save() - "<layers>;<biome>".
    std::string toString() const;

    const std::string& biome() const { return m_biome; }
    const std::optional<std::vector<std::string>>& structureOverrides() const {
        return m_structureOverrides;
    }
    const std::vector<FlatLayerInfo>& layersInfo() const { return m_layersInfo; }

    /**
     * Flattened per-y BlockState list (index 0 = world bottom). Entries are
     * nullptr where adjustGenerationSettings moved a non-opaque layer into a
     * FILL_LAYER feature. Reference: getLayers() after adjustment.
     */
    const std::vector<FlatBlockState*>& layers() const { return m_layers; }

    /**
     * Adjusted per-step feature lists for this settings' biome.
     * Reference: adjustGenerationSettings(biome) - lakes when addLakes, the
     * biome's own features when decoration (minus structure steps and LAKES
     * when addLakes), FILL_LAYER features for non-opaque layers.
     * Indexed [GenerationStep ordinal][feature]. Always DECORATION_COUNT long.
     */
    const std::vector<std::vector<const placement::PlacedFeature*>>& adjustedFeatures() const {
        return m_adjustedFeatures;
    }

private:
    FlatLevelGeneratorSettings() = default;

    static FlatLevelGeneratorSettings make(
        std::optional<std::vector<std::string>> structureOverrides,
        const std::string& biome, bool decoration, bool addLakes,
        std::vector<FlatLayerInfo> layersBottomFirst);

    // Reference: updateLayers() + the layer part of adjustGenerationSettings.
    void updateLayersAndAdjust();

    std::optional<std::vector<std::string>> m_structureOverrides;  // structure_set ids
    std::vector<FlatLayerInfo> m_layersInfo;                       // bottom-first
    std::string m_biome = "minecraft:plains";
    bool m_decoration = false;
    bool m_addLakes = false;
    bool m_voidGen = false;
    std::vector<FlatBlockState*> m_layers;
    std::vector<std::vector<const placement::PlacedFeature*>> m_adjustedFeatures;
};

} // namespace flat
} // namespace levelgen
} // namespace minecraft
