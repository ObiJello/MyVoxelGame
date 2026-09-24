#pragma once

#include "core/BlockPos.h"
#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/DensityVolume.h"
#include "levelgen/material/MaterialCondition.h"
#include "levelgen/material/MaterialRule.h"
#include "levelgen/material/VerticalAnchor.h"
#include "random/AnyPositionalRandomFactory.h"
#include "world/biome/Biomes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

// Reference: levelgen.material.MaterialRuleContext (26.3) - the mutable state
// of one surface pass (a chunk in buildSurface, one block in topMaterial):
// the current column (updateXZ) and block (updateY), the lazily computed
// column values (surface secondary noise, minimum surface level), the noise
// samplers conditions share, and the density getters ore veins read.
//
// Update counters: every updateXZ also bumps the Y counter, so an evaluator
// cached per Y is recomputed at a new column too. LazyXZCondition and
// LazyYCondition memoise a result until the matching counter moves.
//
// A context lives on the stack of the pass that made it; evaluators compiled
// against it borrow it and must be destroyed first.

namespace minecraft {
namespace levelgen {
namespace density {
class RandomState;
}

namespace material {

class MaterialSystem;

using BiomeGetter = std::function<world::biome::BiomeHolder(const core::BlockPos&)>;
// Holder<Biome> identity is the biome id for the engine's interned biomes.
using PossibleBiomes = std::set<world::biome::BiomeKey>;

// java.util.function.DoubleSupplier (the context's cached noise samplers).
class DoubleSupplier {
public:
    virtual ~DoubleSupplier() = default;
    virtual double getAsDouble() = 0;
};

class MaterialRuleContext;

// MaterialRules.DensityGetter: a density function at the context's current
// block, read from a buffer prefilled over the pass's volume when asked for
// (and the block is inside it), sampled at the block otherwise.
class DensityGetter {
public:
    DensityGetter(const MaterialRuleContext& context, density::BoundSampler sampler,
                  std::unique_ptr<density::DensityBuffer> prefilled);

    float get() const;

private:
    const MaterialRuleContext* m_context;
    density::BoundSampler m_sampler;
    std::unique_ptr<density::DensityBuffer> m_buffer;   // null: no prefill
};

class MaterialRuleContext {
public:
    static constexpr int32_t HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE = 8;

    MaterialRuleContext(const MaterialSystem& system, density::RandomState& randomState,
                        const density::DensityVolume& expectedVolume, const density::DensitySamplerSet& densitySamplers,
                        BiomeGetter biomeGetter, const GenerationContext& context, const PossibleBiomes* possibleBiomes);

    MaterialRuleContext(const MaterialRuleContext&) = delete;
    MaterialRuleContext& operator=(const MaterialRuleContext&) = delete;

    // Package-private in Java; MaterialSystem drives them.
    void updateXZ(int32_t blockX, int32_t blockZ, int32_t surfaceGradientX, int32_t surfaceGradientZ);
    void updateY(int32_t stoneDepthAbove, int32_t stoneDepthBelow, int32_t waterHeight, int32_t blockY);

    double getSurfaceSecondary();
    world::biome::BiomeHolder getBiome();
    int32_t getSeaLevel() const;
    int32_t getMinSurfaceLevel();

    // The context's sampler for a noise (keyed by full identifier), shared by
    // every condition that names it: 2d caches per column, 3d per block.
    DoubleSupplier& getNoiseSampler(const std::string& noiseId, bool is3d);
    random::AnyPositionalRandomFactory getOrCreateRandomFactory(const std::string& name);
    DensityGetter getDensitiesInChunk(const density::DensityFunctionPtr& function, bool prefill);

    const PossibleBiomes* possibleBiomes() const { return m_possibleBiomes; }
    int32_t stoneDepthAbove() const { return m_stoneDepthAbove; }
    int32_t stoneDepthBelow() const { return m_stoneDepthBelow; }
    int32_t surfaceDepth() const { return m_surfaceDepth; }
    int32_t waterHeight() const { return m_waterHeight; }
    int32_t blockX() const { return m_blockX; }
    int32_t blockY() const { return m_blockY; }
    int32_t blockZ() const { return m_blockZ; }
    const core::BlockPos& blockPos() { return m_pos.set(m_blockX, m_blockY, m_blockZ); }
    int32_t surfaceGradientX() const { return m_surfaceGradientX; }
    int32_t surfaceGradientZ() const { return m_surfaceGradientZ; }
    int32_t resolveAnchorY(const VerticalAnchor& anchor) const { return anchor.resolveY(m_context); }
    BlockState* getBand(int32_t x, int32_t y, int32_t z) const;

    const density::DensityVolume& expectedVolume() const { return m_expectedVolume; }
    int64_t lastUpdateXZ() const { return m_lastUpdateXZ; }
    int64_t lastUpdateY() const { return m_lastUpdateY; }

    // MaterialRuleContext.LazyYCondition: computed once per updateY.
    class LazyYCondition : public ConditionEvaluator {
    public:
        bool test() final;

    protected:
        explicit LazyYCondition(MaterialRuleContext& context);
        virtual bool compute() = 0;

        MaterialRuleContext& context;

    private:
        int64_t m_lastUpdate;
        std::optional<bool> m_result;
    };

    // MaterialRuleContext.LazyXZCondition: computed once per updateXZ.
    class LazyXZCondition : public ConditionEvaluator {
    public:
        bool test() final;

    protected:
        explicit LazyXZCondition(MaterialRuleContext& context);
        virtual bool compute() = 0;

        MaterialRuleContext& context;

    private:
        int64_t m_lastUpdate;
        std::optional<bool> m_result;
    };

private:
    class NoiseSampler2d;
    class NoiseSampler3d;

    const MaterialSystem& m_system;
    density::RandomState& m_randomState;
    density::DensityVolume m_expectedVolume;
    density::DensitySamplerSet m_densitySamplers;
    BiomeGetter m_biomeGetter;
    GenerationContext m_context;
    const PossibleBiomes* m_possibleBiomes;
    density::DensityVolume m_preliminarySurfaceVolume;
    std::unique_ptr<density::DensityBuffer> m_preliminarySurfaceBuffer;
    std::unordered_map<std::string, std::unique_ptr<DoubleSupplier>> m_noiseSamplers2d;
    std::unordered_map<std::string, std::unique_ptr<DoubleSupplier>> m_noiseSamplers3d;

    int64_t m_lastUpdateXZ = -9223372036854775807LL;   // Long.MIN_VALUE + 1
    int32_t m_blockX = 0;
    int32_t m_blockZ = 0;
    int32_t m_surfaceGradientX = 0;
    int32_t m_surfaceGradientZ = 0;
    int32_t m_surfaceDepth = 0;
    int64_t m_lastSurfaceDepth2Update;
    double m_surfaceSecondary = 0.0;
    int64_t m_lastMinSurfaceLevelUpdate;
    int32_t m_minSurfaceLevel = 0;
    int64_t m_lastUpdateY = -9223372036854775807LL;
    core::BlockPos::MutableBlockPos m_pos;
    world::biome::BiomeHolder m_biome = nullptr;
    bool m_biomeResolved = false;
    int32_t m_blockY = 0;
    int32_t m_waterHeight = 0;
    int32_t m_stoneDepthBelow = 0;
    int32_t m_stoneDepthAbove = 0;
};

} // namespace material
} // namespace levelgen
} // namespace minecraft
