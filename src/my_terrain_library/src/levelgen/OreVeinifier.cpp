#include "levelgen/OreVeinifier.h"
#include "world/level/block/Blocks.h"
#include "math/Mth.h"
#include "random/XoroshiroRandomSource.h"
#include <cmath>

namespace minecraft {
namespace levelgen {

// VeinType properties (Reference: OreVeinifier.java lines 59-60)
// COPPER: ore=COPPER_ORE, rawOreBlock=RAW_COPPER_BLOCK, filler=GRANITE, minY=0, maxY=50
// IRON: ore=DEEPSLATE_IRON_ORE, rawOreBlock=RAW_IRON_BLOCK, filler=TUFF, minY=-60, maxY=-8

int OreVeinifier::getMinY(VeinType type) {
    switch (type) {
        case VeinType::COPPER: return 0;
        case VeinType::IRON: return -60;
        default: return 0;
    }
}

int OreVeinifier::getMaxY(VeinType type) {
    switch (type) {
        case VeinType::COPPER: return 50;
        case VeinType::IRON: return -8;
        default: return 0;
    }
}

BlockState* OreVeinifier::getOre(VeinType type) {
    switch (type) {
        case VeinType::COPPER: return minecraft::world::level::block::Blocks::COPPER_ORE->defaultBlockState();
        case VeinType::IRON: return minecraft::world::level::block::Blocks::DEEPSLATE_IRON_ORE->defaultBlockState();
        default: return nullptr;
    }
}

BlockState* OreVeinifier::getRawOreBlock(VeinType type) {
    switch (type) {
        case VeinType::COPPER: return minecraft::world::level::block::Blocks::RAW_COPPER_BLOCK->defaultBlockState();
        case VeinType::IRON: return minecraft::world::level::block::Blocks::RAW_IRON_BLOCK->defaultBlockState();
        default: return nullptr;
    }
}

BlockState* OreVeinifier::getFiller(VeinType type) {
    switch (type) {
        case VeinType::COPPER: return minecraft::world::level::block::Blocks::GRANITE->defaultBlockState();
        case VeinType::IRON: return minecraft::world::level::block::Blocks::TUFF->defaultBlockState();
        default: return nullptr;
    }
}

OreVeinifier::OreVeinifier(
    density::DensityFunction* veinToggle,
    density::DensityFunction* veinRidged,
    density::DensityFunction* veinGap,
    random::PositionalRandomFactory* oreVeinsPositionalRandomFactory
)
    : m_veinToggle(veinToggle)
    , m_veinRidged(veinRidged)
    , m_veinGap(veinGap)
    , m_positionalRandomFactory(oreVeinsPositionalRandomFactory)
{
}

// Reference: OreVeinifier.java lines 23-56
BlockState* OreVeinifier::calculate(
    const density::DensityFunction::FunctionContext& context
) const {
    // Line 26: double oreVeininessNoiseValue = veinToggle.compute(context);
    double oreVeininessNoiseValue = m_veinToggle->compute(context);

    // Line 27: int posY = context.blockY();
    int posY = context.blockY();

    // Line 28: VeinType veinType = oreVeininessNoiseValue > 0.0F ? COPPER : IRON;
    VeinType veinType = oreVeininessNoiseValue > 0.0 ? VeinType::COPPER : VeinType::IRON;

    // Line 29: double veininessRidged = Math.abs(oreVeininessNoiseValue);
    double veininessRidged = std::abs(oreVeininessNoiseValue);

    // Line 30-31: Distance from vein type boundaries
    int distanceFromTop = getMaxY(veinType) - posY;
    int distanceFromBottom = posY - getMinY(veinType);

    // Line 32: Check if within vein Y range
    if (distanceFromBottom < 0 || distanceFromTop < 0) {
        return nullptr;  // Outside vein range
    }

    // Line 33-34: Edge roundoff calculation
    int distanceFromEdge = std::min(distanceFromTop, distanceFromBottom);
    double edgeRoundoff = Mth::clampedMap(
        static_cast<double>(distanceFromEdge),
        0.0,
        static_cast<double>(EDGE_ROUNDOFF_BEGIN),
        -MAX_EDGE_ROUNDOFF,
        0.0
    );

    // Line 35-36: Check veininess threshold
    if (veininessRidged + edgeRoundoff < static_cast<double>(VEININESS_THRESHOLD)) {
        return nullptr;
    }

    // Line 38: Get positional random
    XoroshiroRandomSource positionalRandom = m_positionalRandomFactory->at(
        context.blockX(), posY, context.blockZ()
    );

    // Line 39-40: Random solidness check
    if (positionalRandom.nextFloat() > VEIN_SOLIDNESS) {
        return nullptr;
    }

    // Line 41-42: Vein ridged check
    if (m_veinRidged->compute(context) >= 0.0) {
        return nullptr;
    }

    // Line 44: Calculate richness
    double richness = Mth::clampedMap(
        veininessRidged,
        static_cast<double>(VEININESS_THRESHOLD),
        static_cast<double>(MAX_RICHNESS_THRESHOLD),
        static_cast<double>(MIN_RICHNESS),
        static_cast<double>(MAX_RICHNESS)
    );

    // Line 45-48: Determine block to place
    if (static_cast<double>(positionalRandom.nextFloat()) < richness &&
        m_veinGap->compute(context) > static_cast<double>(SKIP_ORE_IF_GAP_NOISE_IS_BELOW)) {
        // Line 46: Ore or raw ore block
        if (positionalRandom.nextFloat() < CHANCE_OF_RAW_ORE_BLOCK) {
            return getRawOreBlock(veinType);
        } else {
            return getOre(veinType);
        }
    } else {
        // Line 48: Filler block (granite for copper, tuff for iron)
        return getFiller(veinType);
    }
}

} // namespace levelgen
} // namespace minecraft
