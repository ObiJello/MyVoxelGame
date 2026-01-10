#pragma once

#include "levelgen/SurfaceRules.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/RandomState.h"
#include "core/BlockPos.h"
#include "world/IChunk.h"
#include <functional>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/carver/CarvingContext.java

namespace minecraft {

// Forward declarations
class NoiseBasedChunkGenerator;

namespace levelgen {
namespace carver {

/**
 * CarvingContext - Context for world carving operations
 * Extends WorldGenerationContext with additional carving state
 * Reference: CarvingContext.java
 */
class CarvingContext : public WorldGenerationContext {
private:
    NoiseChunk* m_noiseChunk;
    RandomState* m_randomState;
    RuleSource* m_surfaceRule;

public:
    /**
     * Constructor
     * Reference: CarvingContext.java lines 24-30
     */
    CarvingContext(
        int32_t minY,
        int32_t height,
        NoiseChunk* noiseChunk,
        RandomState* randomState,
        RuleSource* surfaceRule
    )
        : WorldGenerationContext(minY, height)
        , m_noiseChunk(noiseChunk)
        , m_randomState(randomState)
        , m_surfaceRule(surfaceRule)
    {}

    /**
     * Get the top material for a position (used when carving exposes dirt under grass)
     * Reference: CarvingContext.java lines 34-36
     *
     * When carvers carve a grass_block, Java replaces the dirt below with the "top material"
     * which is grass_block. This is done by evaluating surface rules with stoneDepthAbove=1
     * so that ON_FLOOR=true and biomeSurfaceRule applies.
     *
     * @return IBlockType* or nullptr if no material applies
     */
    ::world::IBlockType* topMaterial(
        std::function<void*(const core::BlockPos&)> biomeGetter,
        ::world::IChunk* chunk,
        const core::BlockPos& pos,
        bool underFluid
    ) const {
        if (!m_randomState || !m_randomState->surfaceSystem() || !m_surfaceRule || !m_noiseChunk) {
            return nullptr;
        }

        // Create surface rules context
        // Reference: SurfaceSystem.java lines 180-181
        Context context(
            m_randomState->surfaceSystem(),
            m_randomState,
            chunk,
            m_noiseChunk,
            biomeGetter,
            *this  // WorldGenerationContext
        );

        // Apply the rule source to get a surface rule
        // Reference: SurfaceSystem.java line 182
        std::unique_ptr<SurfaceRule> rule = m_surfaceRule->apply(context);
        if (!rule) {
            return nullptr;
        }

        int32_t blockX = pos.getX();
        int32_t blockY = pos.getY();
        int32_t blockZ = pos.getZ();

        // Update context with position
        // Reference: SurfaceSystem.java lines 186-187
        context.updateXZ(blockX, blockZ);
        // CRITICAL: stoneDepthAbove=1, stoneDepthBelow=1 ensures ON_FLOOR=true
        // This is how Java ensures we get grass_block instead of dirt
        int32_t waterHeight = underFluid ? blockY + 1 : INT32_MIN;
        context.updateY(1, 1, waterHeight, blockX, blockY, blockZ);

        // Try to apply the rule
        // Reference: SurfaceSystem.java lines 188-189
        return rule->tryApply(blockX, blockY, blockZ);
    }

    /**
     * Get the random state
     * Reference: CarvingContext.java lines 44-46
     */
    RandomState* randomState() const { return m_randomState; }

    /**
     * Get the noise chunk
     */
    NoiseChunk* noiseChunk() const { return m_noiseChunk; }

    /**
     * Get the surface rule
     */
    RuleSource* surfaceRule() const { return m_surfaceRule; }
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
