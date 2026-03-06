#pragma once

#include "levelgen/DensityFunction.h"
#include "levelgen/BlockStateFiller.h"
#include "random/PositionalRandomFactory.h"
#include "world/level/block/state/BlockState.h"

namespace minecraft {
namespace levelgen {

/**
 * OreVeinifier - Generates large ore veins (copper and iron)
 *
 * Reference: net/minecraft/world/level/levelgen/OreVeinifier.java
 *
 * This class creates the large copper and iron ore veins found in Minecraft.
 * - Copper veins: Y 0-50, with copper ore, raw copper blocks, and granite filler
 * - Iron veins: Y -60 to -8, with deepslate iron ore, raw iron blocks, and tuff filler
 */
class OreVeinifier : public BlockStateFiller {
public:
    // Constants from Java (lines 10-18)
    static constexpr float VEININESS_THRESHOLD = 0.4F;
    static constexpr int EDGE_ROUNDOFF_BEGIN = 20;
    static constexpr double MAX_EDGE_ROUNDOFF = 0.2;
    static constexpr float VEIN_SOLIDNESS = 0.7F;
    static constexpr float MIN_RICHNESS = 0.1F;
    static constexpr float MAX_RICHNESS = 0.3F;
    static constexpr float MAX_RICHNESS_THRESHOLD = 0.6F;
    static constexpr float CHANCE_OF_RAW_ORE_BLOCK = 0.02F;
    static constexpr float SKIP_ORE_IF_GAP_NOISE_IS_BELOW = -0.3F;

    /**
     * VeinType - Defines properties for copper and iron veins
     * Reference: OreVeinifier.java lines 58-75
     */
    enum class VeinType {
        COPPER,  // Y 0-50
        IRON     // Y -60 to -8
    };

    /**
     * Constructor
     *
     * @param veinToggle - Density function for ore veininess (determines copper vs iron)
     * @param veinRidged - Density function for vein ridge pattern
     * @param veinGap - Density function for gaps in veins
     * @param oreVeinsPositionalRandomFactory - Random factory for positional randomness
     */
    OreVeinifier(
        density::DensityFunction* veinToggle,
        density::DensityFunction* veinRidged,
        density::DensityFunction* veinGap,
        random::PositionalRandomFactory* oreVeinsPositionalRandomFactory
    );

    virtual ~OreVeinifier() = default;

    /**
     * Calculate the block state at a given position
     * Reference: OreVeinifier.java lines 23-56 (lambda)
     *
     * @param context - The function context with position information
     * @return Block type to place, or nullptr for default/air
     */
    BlockState* calculate(const density::DensityFunction::FunctionContext& context) const override;

    /**
     * Get vein type properties
     */
    static int getMinY(VeinType type);
    static int getMaxY(VeinType type);
    static BlockState* getOre(VeinType type);
    static BlockState* getRawOreBlock(VeinType type);
    static BlockState* getFiller(VeinType type);

private:
    density::DensityFunction* m_veinToggle;
    density::DensityFunction* m_veinRidged;
    density::DensityFunction* m_veinGap;
    random::PositionalRandomFactory* m_positionalRandomFactory;
};

} // namespace levelgen
} // namespace minecraft
