#pragma once

#include "levelgen/carver/CaveWorldCarver.h"
#include "world/level/block/Blocks.h"

// Reference: net/minecraft/world/level/levelgen/carver/NetherWorldCarver.java
// Extends CaveWorldCarver with: cave bound 10, thickness
// (nextFloat*2 + nextFloat) * 2, yScale 5.0, and a simplified carveBlock
// (replaceable -> LAVA at y <= minGenY+31 else CAVE_AIR; no aquifer/grass).
// Java also sets liquids = {LAVA, WATER}; the 26.1 carve loop has no
// disallowed-liquid abort (gate-proven for the overworld), so nothing to
// mirror there.

namespace minecraft {
namespace levelgen {
namespace carver {

class NetherWorldCarver : public CaveWorldCarver {
public:
    int32_t getCaveBound() const override { return 10; }

    float getThickness(XoroshiroRandomSource& random) const override {
        // CRITICAL: evaluate random calls in Java order (C++ expression order is unspecified)
        float r1 = random.nextFloat();
        float r2 = random.nextFloat();
        return (r1 * 2.0f + r2) * 2.0f;
    }

    float getThickness(LegacyRandomSource& random) const override {
        // CRITICAL: evaluate random calls in Java order (C++ expression order is unspecified)
        float r1 = random.nextFloat();
        float r2 = random.nextFloat();
        return (r1 * 2.0f + r2) * 2.0f;
    }

    double getYScale() const override { return 5.0; }

    bool carveBlock(
        CarvingContext& context,
        const CaveCarverConfiguration& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        CarvingMask& mask,
        core::BlockPos::MutableBlockPos& blockPos,
        core::BlockPos::MutableBlockPos& helperPos,
        Aquifer* aquifer,
        bool& hasGrass
    ) override {
        (void)biomeGetter;
        (void)mask;
        (void)helperPos;
        (void)aquifer;
        (void)hasGrass;
        BlockState* blockType = chunk->getBlockState(blockPos);
        if (blockType == nullptr || !canReplaceBlock(configuration, blockType)) {
            return false;
        }
        BlockState* state;
        if (blockPos.getY() <= context.getMinGenY() + 31) {
            state = world::level::block::Blocks::LAVA->defaultBlockState();
        } else {
            static BlockState* const s_caveAir =
                world::level::block::Blocks::getDefaultState("minecraft:cave_air");
            state = s_caveAir;
        }
        chunk->setBlockState(blockPos, state, false);
        return true;
    }
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
