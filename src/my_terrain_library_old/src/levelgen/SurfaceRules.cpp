#include "levelgen/SurfaceRules.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/Heightmap.h"
#include "world/biome/Biome.h"
#include "synth/NormalNoise.h"
#include "random/PositionalRandomFactory.h"
#include "random/XoroshiroRandomSource.h"
#include "core/BlockPos.h"
#include "math/Mth.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace minecraft {
namespace levelgen {

//=============================================================================
// Static Instance Definitions
//=============================================================================

AbovePreliminarySurfaceSource AbovePreliminarySurfaceSource::INSTANCE;
HoleSource HoleSource::INSTANCE;
TemperatureSource TemperatureSource::INSTANCE;
SteepSource SteepSource::INSTANCE;
BandlandsSource BandlandsSource::INSTANCE;

namespace SurfaceRules {

// Pre-defined condition sources - will be initialized by initializeStatics()
ConditionSource* ON_FLOOR = nullptr;
ConditionSource* UNDER_FLOOR = nullptr;
ConditionSource* DEEP_UNDER_FLOOR = nullptr;
ConditionSource* VERY_DEEP_UNDER_FLOOR = nullptr;
ConditionSource* ON_CEILING = nullptr;
ConditionSource* UNDER_CEILING = nullptr;

void initializeStatics() {
    // Reference: SurfaceRules.java lines 130-137
    if (ON_FLOOR == nullptr) {
        ON_FLOOR = stoneDepthCheck(0, false, CaveSurface::FLOOR);
        UNDER_FLOOR = stoneDepthCheck(0, true, CaveSurface::FLOOR);
        DEEP_UNDER_FLOOR = stoneDepthCheck(0, true, 6, CaveSurface::FLOOR);
        VERY_DEEP_UNDER_FLOOR = stoneDepthCheck(0, true, 30, CaveSurface::FLOOR);
        ON_CEILING = stoneDepthCheck(0, false, CaveSurface::CEILING);
        UNDER_CEILING = stoneDepthCheck(0, true, CaveSurface::CEILING);
    }
}

} // namespace SurfaceRules

//=============================================================================
// Context Implementation
// Reference: SurfaceRules.java lines 139-242
//=============================================================================

Context::Context(
    SurfaceSystem* system,
    RandomState* randomState,
    ::world::IChunk* chunk,
    NoiseChunk* noiseChunk,
    std::function<void*(const ::minecraft::core::BlockPos&)> biomeGetter,
    const WorldGenerationContext& genContext
)
    : m_system(system)
    , m_randomState(randomState)
    , m_chunk(chunk)
    , m_noiseChunk(noiseChunk)
    , m_biomeGetter(biomeGetter)
    , m_context(genContext)
    , m_lastPreliminarySurfaceCellOrigin(std::numeric_limits<int64_t>::max())
    , m_lastUpdateXZ(-9223372036854775807LL)  // Long.MIN_VALUE + 1 in Java
    , m_blockX(0)
    , m_blockZ(0)
    , m_surfaceDepth(0)
    , m_lastSurfaceDepth2Update(m_lastUpdateXZ - 1)
    , m_surfaceSecondary(0.0)
    , m_lastMinSurfaceLevelUpdate(m_lastUpdateXZ - 1)
    , m_minSurfaceLevel(0)
    , m_lastUpdateY(-9223372036854775807LL)
    , m_pos(new ::minecraft::core::BlockPos::MutableBlockPos())
    , m_cachedBiome(nullptr)
    , m_biomeComputed(false)
    , m_biomeBlockX(0)
    , m_biomeBlockY(0)
    , m_biomeBlockZ(0)
    , m_blockY(0)
    , m_waterHeight(std::numeric_limits<int32_t>::min())
    , m_stoneDepthBelow(0)
    , m_stoneDepthAbove(0)
{
    // Initialize cached conditions
    // Reference: SurfaceRules.java lines 145-148
    m_temperature = std::make_unique<TemperatureHelperCondition>(*this);
    m_steep = std::make_unique<SteepMaterialCondition>(*this);
    m_hole = std::make_unique<HoleCondition>(*this);
    m_abovePreliminarySurface = std::make_unique<AbovePreliminarySurfaceCondition>(*this);

    // Initialize preliminary surface cache
    for (int i = 0; i < 4; ++i) {
        m_preliminarySurfaceCache[i] = 0;
    }
}

Context::~Context() {
    delete m_pos;
}

// Reference: SurfaceRules.java lines 185-191
void Context::updateXZ(int32_t blockX, int32_t blockZ) {
    ++m_lastUpdateXZ;
    ++m_lastUpdateY;
    m_blockX = blockX;
    m_blockZ = blockZ;
    m_surfaceDepth = m_system->getSurfaceDepth(blockX, blockZ);
}

// Reference: SurfaceRules.java lines 193-200
void Context::updateY(int32_t stoneDepthAbove, int32_t stoneDepthBelow, int32_t waterHeight,
                      int32_t blockX, int32_t blockY, int32_t blockZ) {
    ++m_lastUpdateY;

    // Reset memoized biome for new Y position
    // Java uses Suppliers.memoize() which computes only once per Y level
    m_biomeComputed = false;
    m_cachedBiome = nullptr;
    m_biomeBlockX = blockX;
    m_biomeBlockY = blockY;
    m_biomeBlockZ = blockZ;

    m_blockY = blockY;
    m_waterHeight = waterHeight;
    m_stoneDepthBelow = stoneDepthBelow;
    m_stoneDepthAbove = stoneDepthAbove;
}

// Memoized biome getter - only computes once per Y level
// Reference: Java uses Suppliers.memoize() for this
void* Context::getBiome() {
    if (!m_biomeComputed) {
        m_pos->set(m_biomeBlockX, m_biomeBlockY, m_biomeBlockZ);
        m_cachedBiome = m_biomeGetter(*m_pos);
        m_biomeComputed = true;
    }
    return m_cachedBiome;
}

// Reference: SurfaceRules.java lines 202-209
double Context::getSurfaceSecondary() {
    if (m_lastSurfaceDepth2Update != m_lastUpdateXZ) {
        m_lastSurfaceDepth2Update = m_lastUpdateXZ;
        m_surfaceSecondary = m_system->getSurfaceSecondary(m_blockX, m_blockZ);
    }
    return m_surfaceSecondary;
}

// Reference: SurfaceRules.java lines 211-213
int32_t Context::getSeaLevel() const {
    return m_system->getSeaLevel();
}

// Reference: SurfaceRules.java lines 223-242
int32_t Context::getMinSurfaceLevel() {
    if (m_lastMinSurfaceLevelUpdate != m_lastUpdateXZ) {
        m_lastMinSurfaceLevelUpdate = m_lastUpdateXZ;

        int32_t cornerCellX = blockCoordToSurfaceCell(m_blockX);
        int32_t cornerCellZ = blockCoordToSurfaceCell(m_blockZ);

        // Pack cell coordinates into int64
        int64_t preliminarySurfaceCellOrigin =
            (static_cast<int64_t>(cornerCellX) & 0xFFFFFFFFL) |
            ((static_cast<int64_t>(cornerCellZ) & 0xFFFFFFFFL) << 32);

        if (m_lastPreliminarySurfaceCellOrigin != preliminarySurfaceCellOrigin) {
            m_lastPreliminarySurfaceCellOrigin = preliminarySurfaceCellOrigin;

            // Sample preliminary surface at 4 corners
            m_preliminarySurfaceCache[0] = m_noiseChunk->preliminarySurfaceLevel(
                surfaceCellToBlockCoord(cornerCellX), surfaceCellToBlockCoord(cornerCellZ));
            m_preliminarySurfaceCache[1] = m_noiseChunk->preliminarySurfaceLevel(
                surfaceCellToBlockCoord(cornerCellX + 1), surfaceCellToBlockCoord(cornerCellZ));
            m_preliminarySurfaceCache[2] = m_noiseChunk->preliminarySurfaceLevel(
                surfaceCellToBlockCoord(cornerCellX), surfaceCellToBlockCoord(cornerCellZ + 1));
            m_preliminarySurfaceCache[3] = m_noiseChunk->preliminarySurfaceLevel(
                surfaceCellToBlockCoord(cornerCellX + 1), surfaceCellToBlockCoord(cornerCellZ + 1));
        }

        // Bilinear interpolation
        float xFactor = static_cast<float>(m_blockX & SURFACE_CELL_MASK) / static_cast<float>(SURFACE_CELL_SIZE);
        float zFactor = static_cast<float>(m_blockZ & SURFACE_CELL_MASK) / static_cast<float>(SURFACE_CELL_SIZE);

        int32_t preliminarySurfaceLevel = static_cast<int32_t>(std::floor(Mth::lerp2(
            static_cast<double>(xFactor),
            static_cast<double>(zFactor),
            static_cast<double>(m_preliminarySurfaceCache[0]),
            static_cast<double>(m_preliminarySurfaceCache[1]),
            static_cast<double>(m_preliminarySurfaceCache[2]),
            static_cast<double>(m_preliminarySurfaceCache[3])
        )));

        m_minSurfaceLevel = preliminarySurfaceLevel + m_surfaceDepth - HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE;
    }

    return m_minSurfaceLevel;
}

//=============================================================================
// Condition Implementations
//=============================================================================

// TemperatureHelperCondition::compute()
// Reference: SurfaceRules.java lines 265-273
bool TemperatureHelperCondition::compute() {
    // Get biome from context and check if cold enough to snow
    // Reference: SurfaceRules.java lines 268-270

    // Get position
    core::BlockPos pos(m_context.getBlockX(), m_context.getBlockY(), m_context.getBlockZ());

    // Get biome at position
    void* biomePtr = m_context.getBiome();
    if (biomePtr == nullptr) {
        return false;
    }

    // Cast to Biome and check temperature
    const world::biome::Biome* biome = static_cast<const world::biome::Biome*>(biomePtr);

    // Get sea level from context
    int32_t seaLevel = m_context.getSeaLevel();

    // Check if cold enough to snow at this position
    return biome->coldEnoughToSnow(pos, seaLevel);
}

// SteepMaterialCondition::compute()
// Reference: SurfaceRules.java lines 275-297
bool SteepMaterialCondition::compute() {
    int32_t chunkBlockX = m_context.getBlockX() & 15;
    int32_t chunkBlockZ = m_context.getBlockZ() & 15;

    int32_t zNorth = std::max(chunkBlockZ - 1, 0);
    int32_t zSouth = std::min(chunkBlockZ + 1, 15);

    ::world::IChunk* chunk = m_context.getChunk();

    // Get heights using heightmap
    // Reference: SurfaceRules.java lines 283-297
    int32_t heightNorth = chunk->getHeight(
        static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), chunkBlockX, zNorth);
    int32_t heightSouth = chunk->getHeight(
        static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), chunkBlockX, zSouth);

    if (heightSouth >= heightNorth + 4) {
        return true;
    }

    int32_t xWest = std::max(chunkBlockX - 1, 0);
    int32_t xEast = std::min(chunkBlockX + 1, 15);

    int32_t heightWest = chunk->getHeight(
        static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), xWest, chunkBlockZ);
    int32_t heightEast = chunk->getHeight(
        static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), xEast, chunkBlockZ);

    return heightWest >= heightEast + 4;
}

//=============================================================================
// ConditionSource Implementations
//=============================================================================

// NotConditionSource::apply
// Reference: SurfaceRules.java lines 422-424
std::unique_ptr<Condition> NotConditionSource::apply(Context& context) {
    auto targetCondition = m_target->apply(context);
    // We need to manage ownership - for now return a wrapped condition
    // In practice, the target condition needs to be kept alive
    return std::make_unique<NotCondition>(targetCondition.release());
}

// StoneDepthCheck::apply - creates StoneDepthCondition
// Reference: SurfaceRules.java lines 438-456
std::unique_ptr<Condition> StoneDepthCheck::apply(Context& context) {
    const bool ceiling = (m_surfaceType == CaveSurface::CEILING);
    const int32_t offset = m_offset;
    const bool addSurfaceDepth = m_addSurfaceDepth;
    const int32_t secondaryDepthRange = m_secondaryDepthRange;

    // Create a LazyYCondition that computes the stone depth check
    class StoneDepthCondition : public LazyYCondition {
    private:
        bool m_ceiling;
        int32_t m_offset;
        bool m_addSurfaceDepth;
        int32_t m_secondaryDepthRange;

    public:
        StoneDepthCondition(Context& ctx, bool ceiling, int32_t offset,
                           bool addSurfaceDepth, int32_t secondaryDepthRange)
            : LazyYCondition(ctx)
            , m_ceiling(ceiling)
            , m_offset(offset)
            , m_addSurfaceDepth(addSurfaceDepth)
            , m_secondaryDepthRange(secondaryDepthRange)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 447-451
            int32_t stoneDepth = m_ceiling ?
                m_context.getStoneDepthBelow() : m_context.getStoneDepthAbove();
            int32_t surfaceDepth = m_addSurfaceDepth ? m_context.getSurfaceDepth() : 0;
            int32_t secondarySurfaceDepth = 0;

            if (m_secondaryDepthRange != 0) {
                secondarySurfaceDepth = static_cast<int32_t>(Mth::map(
                    m_context.getSurfaceSecondary(),
                    -1.0, 1.0,
                    0.0, static_cast<double>(m_secondaryDepthRange)
                ));
            }

            bool result = stoneDepth <= 1 + m_offset + surfaceDepth + secondarySurfaceDepth;
            return result;
        }
    };

    return std::make_unique<StoneDepthCondition>(context, ceiling, offset,
                                                  addSurfaceDepth, secondaryDepthRange);
}

// YConditionSource::apply - creates YCondition
// Reference: SurfaceRules.java lines 504-517
std::unique_ptr<Condition> YConditionSource::apply(Context& context) {
    const VerticalAnchor anchor = m_anchor;
    const int32_t surfaceDepthMultiplier = m_surfaceDepthMultiplier;
    const bool addStoneDepth = m_addStoneDepth;
    const WorldGenerationContext& genContext = context.getWorldGenContext();

    class YCondition : public LazyYCondition {
    private:
        VerticalAnchor m_anchor;
        int32_t m_surfaceDepthMultiplier;
        bool m_addStoneDepth;
        const WorldGenerationContext& m_genContext;

    public:
        YCondition(Context& ctx, const VerticalAnchor& anchor,
                   int32_t surfaceDepthMultiplier, bool addStoneDepth,
                   const WorldGenerationContext& genContext)
            : LazyYCondition(ctx)
            , m_anchor(anchor)
            , m_surfaceDepthMultiplier(surfaceDepthMultiplier)
            , m_addStoneDepth(addStoneDepth)
            , m_genContext(genContext)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 511-512
            int32_t blockY = m_context.getBlockY();
            int32_t stoneDepthAbove = m_addStoneDepth ? m_context.getStoneDepthAbove() : 0;
            int32_t anchorY = m_anchor.resolveY(m_genContext);
            int32_t surfaceDepth = m_context.getSurfaceDepth();

            return blockY + stoneDepthAbove >= anchorY + surfaceDepth * m_surfaceDepthMultiplier;
        }
    };

    return std::make_unique<YCondition>(context, anchor, surfaceDepthMultiplier,
                                         addStoneDepth, genContext);
}

// WaterConditionSource::apply - creates WaterCondition
// Reference: SurfaceRules.java lines 527-540
std::unique_ptr<Condition> WaterConditionSource::apply(Context& context) {
    const int32_t offset = m_offset;
    const int32_t surfaceDepthMultiplier = m_surfaceDepthMultiplier;
    const bool addStoneDepth = m_addStoneDepth;

    class WaterCondition : public LazyYCondition {
    private:
        int32_t m_offset;
        int32_t m_surfaceDepthMultiplier;
        bool m_addStoneDepth;

    public:
        WaterCondition(Context& ctx, int32_t offset,
                       int32_t surfaceDepthMultiplier, bool addStoneDepth)
            : LazyYCondition(ctx)
            , m_offset(offset)
            , m_surfaceDepthMultiplier(surfaceDepthMultiplier)
            , m_addStoneDepth(addStoneDepth)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 534-535
            int32_t waterHeight = m_context.getWaterHeight();
            if (waterHeight == std::numeric_limits<int32_t>::min()) {
                return true;
            }

            int32_t blockY = m_context.getBlockY();
            int32_t stoneDepthAbove = m_addStoneDepth ? m_context.getStoneDepthAbove() : 0;
            int32_t surfaceDepth = m_context.getSurfaceDepth();

            return blockY + stoneDepthAbove >= waterHeight + m_offset +
                   surfaceDepth * m_surfaceDepthMultiplier;
        }
    };

    return std::make_unique<WaterCondition>(context, offset, surfaceDepthMultiplier, addStoneDepth);
}

// BiomeConditionSource::apply - creates BiomeCondition
// Reference: SurfaceRules.java lines 559-571
//
// OPTIMIZATION: Changed from LazyYCondition to LazyXZCondition
// Biome is effectively constant per XZ column for surface generation.
// The BiomeManager fiddling algorithm does use Y, but within a column
// the biome rarely changes, and surface rules work from top-down anyway.
// This avoids recomputing biome for every Y level (~200 levels per column).
std::unique_ptr<Condition> BiomeConditionSource::apply(Context& context) {
    const std::set<std::string>& biomeNameTest = m_biomeNameTest;

    class BiomeCondition : public LazyXZCondition {
    private:
        const std::set<std::string>& m_biomeNameTest;

    public:
        BiomeCondition(Context& ctx, const std::set<std::string>& biomeNameTest)
            : LazyXZCondition(ctx)
            , m_biomeNameTest(biomeNameTest)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 566-567
            // Get biome from context - now cached per XZ column
            void* biomePtr = m_context.getBiome();
            if (biomePtr == nullptr) {
                return false;
            }

            // Cast to Biome and get name
            const world::biome::Biome* biome = static_cast<const world::biome::Biome*>(biomePtr);
            const std::string& biomeName = biome->getName();

            // Check if biome name is in the test set
            return m_biomeNameTest.count(biomeName) > 0;
        }
    };

    return std::make_unique<BiomeCondition>(context, biomeNameTest);
}

// NoiseThresholdConditionSource::apply - creates NoiseThresholdCondition
// Reference: SurfaceRules.java lines 605-621
std::unique_ptr<Condition> NoiseThresholdConditionSource::apply(Context& context) {
    // Get noise from random state
    NormalNoise* noise = context.getRandomState()->getOrCreateNoise(m_noise);
    const double minThreshold = m_minThreshold;
    const double maxThreshold = m_maxThreshold;

    class NoiseThresholdCondition : public LazyXZCondition {
    private:
        NormalNoise* m_noise;
        double m_minThreshold;
        double m_maxThreshold;

    public:
        NoiseThresholdCondition(Context& ctx, NormalNoise* noise,
                                double minThreshold, double maxThreshold)
            : LazyXZCondition(ctx)
            , m_noise(noise)
            , m_minThreshold(minThreshold)
            , m_maxThreshold(maxThreshold)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 614-616
            double value = m_noise->getValue(
                static_cast<double>(m_context.getBlockX()),
                0.0,
                static_cast<double>(m_context.getBlockZ())
            );
            return value >= m_minThreshold && value <= m_maxThreshold;
        }
    };

    return std::make_unique<NoiseThresholdCondition>(context, noise, minThreshold, maxThreshold);
}

// VerticalGradientConditionSource::apply - creates VerticalGradientCondition
// Reference: SurfaceRules.java lines 631-657
std::unique_ptr<Condition> VerticalGradientConditionSource::apply(Context& context) {
    const WorldGenerationContext& genContext = context.getWorldGenContext();
    const int32_t trueAtAndBelow = m_trueAtAndBelow.resolveY(genContext);
    const int32_t falseAtAndAbove = m_falseAtAndAbove.resolveY(genContext);

    // Get positional random factory
    random::PositionalRandomFactory* randomFactory =
        context.getRandomState()->getOrCreateRandomFactory(m_randomName);

    class VerticalGradientCondition : public LazyYCondition {
    private:
        int32_t m_trueAtAndBelow;
        int32_t m_falseAtAndAbove;
        random::PositionalRandomFactory* m_randomFactory;

    public:
        VerticalGradientCondition(Context& ctx, int32_t trueAtAndBelow,
                                   int32_t falseAtAndAbove,
                                   random::PositionalRandomFactory* randomFactory)
            : LazyYCondition(ctx)
            , m_trueAtAndBelow(trueAtAndBelow)
            , m_falseAtAndAbove(falseAtAndAbove)
            , m_randomFactory(randomFactory)
        {}

    protected:
        bool compute() override {
            // Reference: SurfaceRules.java lines 642-652
            int32_t blockY = m_context.getBlockY();

            if (blockY <= m_trueAtAndBelow) {
                return true;
            } else if (blockY >= m_falseAtAndAbove) {
                return false;
            } else {
                double probability = Mth::map(
                    static_cast<double>(blockY),
                    static_cast<double>(m_trueAtAndBelow),
                    static_cast<double>(m_falseAtAndAbove),
                    1.0, 0.0
                );

                XoroshiroRandomSource random = m_randomFactory->at(
                    m_context.getBlockX(), blockY, m_context.getBlockZ()
                );

                return random.nextFloat() < probability;
            }
        }
    };

    return std::make_unique<VerticalGradientCondition>(context, trueAtAndBelow,
                                                        falseAtAndAbove, randomFactory);
}

//=============================================================================
// RuleSource Implementations
//=============================================================================

// BlockRuleSource::apply
// Reference: SurfaceRules.java lines 709-711
std::unique_ptr<SurfaceRule> BlockRuleSource::apply(Context& context) {
    return std::make_unique<StateRule>(m_resultBlock);
}

// TestRuleSource::apply
// Reference: SurfaceRules.java lines 725-727
std::unique_ptr<SurfaceRule> TestRuleSource::apply(Context& context) {
    if (!m_ifTrue) {
        throw std::runtime_error("TestRuleSource: m_ifTrue is null");
    }
    if (!m_thenRun) {
        throw std::runtime_error("TestRuleSource: m_thenRun is null");
    }

    auto condition = m_ifTrue->apply(context);
    if (!condition) {
        throw std::runtime_error("TestRuleSource: condition returned null from apply()");
    }

    auto followup = m_thenRun->apply(context);
    if (!followup) {
        throw std::runtime_error("TestRuleSource: followup returned null from apply()");
    }

    // Need to manage ownership - conditions/rules need to stay alive
    // For now, release ownership to the TestRule
    return std::make_unique<TestRule>(condition.release(), followup.release());
}

// SequenceRuleSource::apply
// Reference: SurfaceRules.java lines 737-749
std::unique_ptr<SurfaceRule> SequenceRuleSource::apply(Context& context) {
    if (m_sequence.size() == 1) {
        return m_sequence[0]->apply(context);
    }

    std::vector<SurfaceRule*> rules;
    for (RuleSource* source : m_sequence) {
        auto rule = source->apply(context);
        rules.push_back(rule.release());  // Transfer ownership
    }

    return std::make_unique<SequenceRule>(rules);
}

// BandlandsSource::apply
// Reference: SurfaceRules.java lines 765-769
std::unique_ptr<SurfaceRule> BandlandsSource::apply(Context& context) {
    SurfaceSystem* system = context.getSystem();

    // Create a rule that delegates to SurfaceSystem::getBand
    class BandlandsRule : public SurfaceRule {
    private:
        SurfaceSystem* m_system;

    public:
        BandlandsRule(SurfaceSystem* system) : m_system(system) {}

        ::world::IBlockType* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
            // Get the band block from the surface system
            return m_system->getBand(blockX, blockY, blockZ);
        }
    };

    return std::make_unique<BandlandsRule>(system);
}

} // namespace levelgen
} // namespace minecraft
