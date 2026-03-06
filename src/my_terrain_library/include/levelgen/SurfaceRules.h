#pragma once

#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/IChunk.h"
#include "levelgen/Heightmap.h"
#include "random/PositionalRandomFactory.h"
#include "core/BlockPos.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <set>
#include <limits>
#include <iostream>

// Forward declarations
namespace minecraft {
    class NormalNoise;
    namespace levelgen {
        class RandomState;
        class NoiseChunk;
        class SurfaceSystem;
    }
    namespace world {
        namespace biome {
            class Biome;
        }
    }
}

// Reference: net/minecraft/world/level/levelgen/SurfaceRules.java

namespace minecraft {
namespace levelgen {

// Forward declarations within namespace
class Context;

/**
 * CaveSurface - Represents floor or ceiling of a cave
 * Reference: net/minecraft/world/level/levelgen/placement/CaveSurface.java
 */
enum class CaveSurface {
    CEILING,  // Direction::UP, y = 1
    FLOOR     // Direction::DOWN, y = -1
};

/**
 * WorldGenerationContext - Height bounds for world generation
 * Reference: net/minecraft/world/level/levelgen/WorldGenerationContext.java
 */
class WorldGenerationContext {
private:
    int32_t m_minY;
    int32_t m_height;

public:
    WorldGenerationContext(int32_t minY, int32_t height)
        : m_minY(minY), m_height(height) {}

    int32_t getMinGenY() const { return m_minY; }
    int32_t getGenDepth() const { return m_height; }
};

/**
 * VerticalAnchor - Resolves Y coordinates relative to world bounds
 * Reference: net/minecraft/world/level/levelgen/VerticalAnchor.java
 */
class VerticalAnchor {
public:
    enum class Type {
        ABSOLUTE,
        ABOVE_BOTTOM,
        BELOW_TOP
    };

private:
    Type m_type;
    int32_t m_value;

public:
    VerticalAnchor(Type type, int32_t value) : m_type(type), m_value(value) {}

    // Factory methods matching Java
    static VerticalAnchor absolute(int32_t value) {
        return VerticalAnchor(Type::ABSOLUTE, value);
    }

    static VerticalAnchor aboveBottom(int32_t offset) {
        return VerticalAnchor(Type::ABOVE_BOTTOM, offset);
    }

    static VerticalAnchor belowTop(int32_t offset) {
        return VerticalAnchor(Type::BELOW_TOP, offset);
    }

    static VerticalAnchor bottom() {
        return aboveBottom(0);
    }

    static VerticalAnchor top() {
        return belowTop(0);
    }

    // Reference: VerticalAnchor.java resolveY methods
    int32_t resolveY(const WorldGenerationContext& context) const {
        switch (m_type) {
            case Type::ABSOLUTE:
                return m_value;
            case Type::ABOVE_BOTTOM:
                return context.getMinGenY() + m_value;
            case Type::BELOW_TOP:
                return context.getGenDepth() - 1 + context.getMinGenY() - m_value;
        }
        return m_value;
    }

    Type getType() const { return m_type; }
    int32_t getValue() const { return m_value; }
};

//=============================================================================
// Core Interfaces - Reference: SurfaceRules.java lines 777-783
//=============================================================================

/**
 * Condition - Tests whether a surface rule should apply
 * Reference: SurfaceRules.java line 777-779
 */
class Condition {
public:
    virtual ~Condition() = default;
    virtual bool test() = 0;
};

/**
 * SurfaceRule - Applies a surface block replacement
 * Reference: SurfaceRules.java lines 781-783
 */
class SurfaceRule {
public:
    virtual ~SurfaceRule() = default;

    /**
     * Try to apply this rule at the given position
     * @return BlockState to use, or nullptr if rule doesn't apply
     * Reference: SurfaceRules.java line 782
     */
    virtual BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) = 0;
};

/**
 * ConditionSource - Factory for creating Condition instances
 * Reference: SurfaceRules.java lines 382-400
 */
class ConditionSource {
public:
    virtual ~ConditionSource() = default;

    /**
     * Apply this condition source to create a condition for the given context
     * Reference: SurfaceRules.java - extends Function<Context, Condition>
     */
    virtual std::unique_ptr<Condition> apply(Context& context) = 0;
};

/**
 * RuleSource - Factory for creating SurfaceRule instances
 * Reference: SurfaceRules.java lines 402-413
 */
class RuleSource {
public:
    virtual ~RuleSource() = default;

    /**
     * Apply this rule source to create a rule for the given context
     * Reference: SurfaceRules.java - extends Function<Context, SurfaceRule>
     */
    virtual std::unique_ptr<SurfaceRule> apply(Context& context) = 0;
};

//=============================================================================
// Context - Holds state for surface rule evaluation
// Reference: SurfaceRules.java lines 139-298
//=============================================================================

/**
 * Context - Mutable state for evaluating surface rules
 * Reference: SurfaceRules.java line 139 (protected static final class Context)
 */
class Context {
private:
    // Constants - Reference: lines 140-143
    static constexpr int32_t HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE = 8;
    static constexpr int32_t SURFACE_CELL_BITS = 4;
    static constexpr int32_t SURFACE_CELL_SIZE = 16;
    static constexpr int32_t SURFACE_CELL_MASK = 15;

    // Core references - Reference: lines 144-153
    SurfaceSystem* m_system;
    std::unique_ptr<Condition> m_temperature;
    std::unique_ptr<Condition> m_steep;
    std::unique_ptr<Condition> m_hole;
    std::unique_ptr<Condition> m_abovePreliminarySurface;
    RandomState* m_randomState;
    ::world::IChunk* m_chunk;
    NoiseChunk* m_noiseChunk;
    std::function<void*(const ::minecraft::core::BlockPos&)> m_biomeGetter;  // Returns Holder<Biome>*
    WorldGenerationContext m_context;

    // Preliminary surface cache - Reference: lines 154-155
    int64_t m_lastPreliminarySurfaceCellOrigin;
    int32_t m_preliminarySurfaceCache[4];

    // XZ state - Reference: lines 156-163
    int64_t m_lastUpdateXZ;
    int32_t m_blockX;
    int32_t m_blockZ;
    int32_t m_surfaceDepth;
    int64_t m_lastSurfaceDepth2Update;
    double m_surfaceSecondary;
    int64_t m_lastMinSurfaceLevelUpdate;
    int32_t m_minSurfaceLevel;

    // Y state - Reference: lines 164-170
    int64_t m_lastUpdateY;
    ::minecraft::core::BlockPos::MutableBlockPos* m_pos;  // MutableBlockPos
    // Memoized biome lookup - Java uses Suppliers.memoize()
    void* m_cachedBiome;
    bool m_biomeComputed;
    int32_t m_biomeBlockX;
    int32_t m_biomeBlockY;
    int32_t m_biomeBlockZ;
    int32_t m_blockY;
    int32_t m_waterHeight;
    int32_t m_stoneDepthBelow;
    int32_t m_stoneDepthAbove;

    // Helper methods - Reference: lines 215-222
    static int32_t blockCoordToSurfaceCell(int32_t blockCoord) {
        return blockCoord >> SURFACE_CELL_BITS;
    }

    static int32_t surfaceCellToBlockCoord(int32_t cellCoord) {
        return cellCoord << SURFACE_CELL_BITS;
    }

public:
    /**
     * Constructor
     * Reference: SurfaceRules.java lines 172-183
     */
    Context(
        SurfaceSystem* system,
        RandomState* randomState,
        ::world::IChunk* chunk,
        NoiseChunk* noiseChunk,
        std::function<void*(const ::minecraft::core::BlockPos&)> biomeGetter,
        const WorldGenerationContext& genContext
    );

    ~Context();

    /**
     * Update context for new XZ position
     * Reference: SurfaceRules.java lines 185-191
     */
    void updateXZ(int32_t blockX, int32_t blockZ);

    /**
     * Update context for new Y position
     * Reference: SurfaceRules.java lines 193-200
     */
    void updateY(int32_t stoneDepthAbove, int32_t stoneDepthBelow, int32_t waterHeight,
                 int32_t blockX, int32_t blockY, int32_t blockZ);

    /**
     * Get surface secondary noise value (lazy computed)
     * Reference: SurfaceRules.java lines 202-209
     */
    double getSurfaceSecondary();

    /**
     * Get sea level from surface system
     * Reference: SurfaceRules.java lines 211-213
     */
    int32_t getSeaLevel() const;

    /**
     * Get minimum surface level (lazy computed)
     * Reference: SurfaceRules.java lines 223-242
     */
    int32_t getMinSurfaceLevel();

    // Accessors for condition evaluation
    int32_t getBlockX() const { return m_blockX; }
    int32_t getBlockY() const { return m_blockY; }
    int32_t getBlockZ() const { return m_blockZ; }
    int32_t getSurfaceDepth() const { return m_surfaceDepth; }
    int32_t getStoneDepthAbove() const { return m_stoneDepthAbove; }
    int32_t getStoneDepthBelow() const { return m_stoneDepthBelow; }
    int32_t getWaterHeight() const { return m_waterHeight; }
    int64_t getLastUpdateXZ() const { return m_lastUpdateXZ; }
    int64_t getLastUpdateY() const { return m_lastUpdateY; }

    ::world::IChunk* getChunk() const { return m_chunk; }
    NoiseChunk* getNoiseChunk() const { return m_noiseChunk; }
    RandomState* getRandomState() const { return m_randomState; }
    SurfaceSystem* getSystem() const { return m_system; }
    const WorldGenerationContext& getWorldGenContext() const { return m_context; }

    void* getBiome();
    ::minecraft::core::BlockPos* getPos() const { return m_pos; }

    // Access to cached conditions
    Condition* getTemperatureCondition() const { return m_temperature.get(); }
    Condition* getSteepCondition() const { return m_steep.get(); }
    Condition* getHoleCondition() const { return m_hole.get(); }
    Condition* getAbovePreliminarySurfaceCondition() const { return m_abovePreliminarySurface.get(); }
};

//=============================================================================
// Lazy Condition Base Classes - Reference: SurfaceRules.java lines 301-349
//=============================================================================

/**
 * LazyCondition - Base class for conditions that cache their result
 * Reference: SurfaceRules.java lines 301-329
 */
class LazyCondition : public Condition {
protected:
    Context& m_context;
    int64_t m_lastUpdate;
    bool m_hasResult;
    bool m_result;

    LazyCondition(Context& context)
        : m_context(context)
        , m_lastUpdate(-1)  // Will be updated on first test()
        , m_hasResult(false)
        , m_result(false)
    {}

    virtual int64_t getContextLastUpdate() const = 0;
    virtual bool compute() = 0;

public:
    bool test() override {
        int64_t lastContextUpdate = getContextLastUpdate();
        if (lastContextUpdate == m_lastUpdate) {
            if (!m_hasResult) {
                throw std::runtime_error("Update triggered but the result is null");
            }
            return m_result;
        } else {
            m_lastUpdate = lastContextUpdate;
            m_result = compute();
            m_hasResult = true;
            return m_result;
        }
    }
};

/**
 * LazyXZCondition - Lazy condition that updates on XZ changes
 * Reference: SurfaceRules.java lines 331-339
 */
class LazyXZCondition : public LazyCondition {
protected:
    LazyXZCondition(Context& context) : LazyCondition(context) {}

    int64_t getContextLastUpdate() const override {
        return m_context.getLastUpdateXZ();
    }
};

/**
 * LazyYCondition - Lazy condition that updates on Y changes
 * Reference: SurfaceRules.java lines 341-349
 */
class LazyYCondition : public LazyCondition {
protected:
    LazyYCondition(Context& context) : LazyCondition(context) {}

    int64_t getContextLastUpdate() const override {
        return m_context.getLastUpdateY();
    }
};

//=============================================================================
// Condition Implementations - Reference: SurfaceRules.java
//=============================================================================

/**
 * NotCondition - Inverts another condition
 * Reference: SurfaceRules.java lines 351-355
 */
class NotCondition : public Condition {
private:
    Condition* m_target;

public:
    NotCondition(Condition* target) : m_target(target) {}

    bool test() override {
        return !m_target->test();
    }
};

/**
 * HoleCondition - Tests if at a "hole" (surface depth <= 0)
 * Reference: SurfaceRules.java lines 244-252
 */
class HoleCondition : public LazyXZCondition {
public:
    HoleCondition(Context& context) : LazyXZCondition(context) {}

protected:
    bool compute() override {
        return m_context.getSurfaceDepth() <= 0;
    }
};

/**
 * AbovePreliminarySurfaceCondition - Tests if above preliminary surface
 * Reference: SurfaceRules.java lines 254-263
 */
class AbovePreliminarySurfaceCondition : public Condition {
private:
    Context& m_context;

public:
    AbovePreliminarySurfaceCondition(Context& context) : m_context(context) {}

    bool test() override {
        return m_context.getBlockY() >= m_context.getMinSurfaceLevel();
    }
};

/**
 * TemperatureHelperCondition - Tests if cold enough to snow
 * Reference: SurfaceRules.java lines 265-273
 */
class TemperatureHelperCondition : public LazyYCondition {
public:
    TemperatureHelperCondition(Context& context) : LazyYCondition(context) {}

protected:
    bool compute() override;  // Implemented in .cpp - needs Biome access
};

/**
 * SteepMaterialCondition - Tests if terrain is steep (height diff >= 4)
 * Reference: SurfaceRules.java lines 275-297
 */
class SteepMaterialCondition : public LazyXZCondition {
public:
    SteepMaterialCondition(Context& context) : LazyXZCondition(context) {}

protected:
    bool compute() override;  // Implemented in .cpp - needs heightmap access
};

//=============================================================================
// Rule Implementations - Reference: SurfaceRules.java lines 357-380
//=============================================================================

/**
 * StateRule - Returns a fixed block type
 * Reference: SurfaceRules.java lines 357-361
 */
class StateRule : public SurfaceRule {
private:
    BlockState* m_block;

public:
    StateRule(BlockState* block) : m_block(block) {}

    BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
        return m_block;
    }
};

/**
 * TestRule - Applies followup rule if condition passes
 * Reference: SurfaceRules.java lines 363-367
 */
class TestRule : public SurfaceRule {
private:
    Condition* m_condition;
    SurfaceRule* m_followup;

public:
    TestRule(Condition* condition, SurfaceRule* followup)
        : m_condition(condition), m_followup(followup) {}

    BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
        if (!m_condition->test()) {
            return nullptr;
        }
        return m_followup->tryApply(blockX, blockY, blockZ);
    }
};

/**
 * SequenceRule - Tries rules in order, returns first non-null result
 * Reference: SurfaceRules.java lines 369-379
 */
class SequenceRule : public SurfaceRule {
private:
    std::vector<SurfaceRule*> m_rules;

public:
    SequenceRule(const std::vector<SurfaceRule*>& rules) : m_rules(rules) {}

    BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
        for (SurfaceRule* rule : m_rules) {
            BlockState* block = rule->tryApply(blockX, blockY, blockZ);
            if (block != nullptr) {
                return block;
            }
        }
        return nullptr;
    }
};

//=============================================================================
// ConditionSource Implementations
//=============================================================================

/**
 * NotConditionSource - Creates NotCondition
 * Reference: SurfaceRules.java lines 415-429
 */
class NotConditionSource : public ConditionSource {
private:
    ConditionSource* m_target;

public:
    NotConditionSource(ConditionSource* target) : m_target(target) {}

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * StoneDepthCheck - Condition source for stone depth check
 * Reference: SurfaceRules.java lines 431-457
 */
class StoneDepthCheck : public ConditionSource {
private:
    int32_t m_offset;
    bool m_addSurfaceDepth;
    int32_t m_secondaryDepthRange;
    CaveSurface m_surfaceType;

public:
    StoneDepthCheck(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange, CaveSurface surfaceType)
        : m_offset(offset)
        , m_addSurfaceDepth(addSurfaceDepth)
        , m_secondaryDepthRange(secondaryDepthRange)
        , m_surfaceType(surfaceType)
    {}

    int32_t offset() const { return m_offset; }
    bool addSurfaceDepth() const { return m_addSurfaceDepth; }
    int32_t secondaryDepthRange() const { return m_secondaryDepthRange; }
    CaveSurface surfaceType() const { return m_surfaceType; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * YConditionSource - Condition source for Y block check
 * Reference: SurfaceRules.java lines 497-518
 */
class YConditionSource : public ConditionSource {
private:
    VerticalAnchor m_anchor;
    int32_t m_surfaceDepthMultiplier;
    bool m_addStoneDepth;

public:
    YConditionSource(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier, bool addStoneDepth)
        : m_anchor(anchor)
        , m_surfaceDepthMultiplier(surfaceDepthMultiplier)
        , m_addStoneDepth(addStoneDepth)
    {}

    const VerticalAnchor& anchor() const { return m_anchor; }
    int32_t surfaceDepthMultiplier() const { return m_surfaceDepthMultiplier; }
    bool addStoneDepth() const { return m_addStoneDepth; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * WaterConditionSource - Condition source for water check
 * Reference: SurfaceRules.java lines 520-541
 */
class WaterConditionSource : public ConditionSource {
private:
    int32_t m_offset;
    int32_t m_surfaceDepthMultiplier;
    bool m_addStoneDepth;

public:
    WaterConditionSource(int32_t offset, int32_t surfaceDepthMultiplier, bool addStoneDepth)
        : m_offset(offset)
        , m_surfaceDepthMultiplier(surfaceDepthMultiplier)
        , m_addStoneDepth(addStoneDepth)
    {}

    int32_t offset() const { return m_offset; }
    int32_t surfaceDepthMultiplier() const { return m_surfaceDepthMultiplier; }
    bool addStoneDepth() const { return m_addStoneDepth; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * BiomeConditionSource - Condition source for biome check
 * Reference: SurfaceRules.java lines 543-596
 */
class BiomeConditionSource : public ConditionSource {
private:
    std::vector<std::string> m_biomes;  // ResourceKey<Biome> as strings
    std::set<std::string> m_biomeNameTest;

public:
    BiomeConditionSource(const std::vector<std::string>& biomes)
        : m_biomes(biomes)
        , m_biomeNameTest(biomes.begin(), biomes.end())
    {}

    const std::vector<std::string>& biomes() const { return m_biomes; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * NoiseThresholdConditionSource - Condition source for noise threshold
 * Reference: SurfaceRules.java lines 598-622
 */
class NoiseThresholdConditionSource : public ConditionSource {
private:
    std::string m_noise;  // ResourceKey<NormalNoise.NoiseParameters>
    double m_minThreshold;
    double m_maxThreshold;

public:
    NoiseThresholdConditionSource(const std::string& noise, double minThreshold, double maxThreshold)
        : m_noise(noise)
        , m_minThreshold(minThreshold)
        , m_maxThreshold(maxThreshold)
    {}

    const std::string& noise() const { return m_noise; }
    double minThreshold() const { return m_minThreshold; }
    double maxThreshold() const { return m_maxThreshold; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

/**
 * VerticalGradientConditionSource - Condition source for vertical gradient
 * Reference: SurfaceRules.java lines 624-658
 */
class VerticalGradientConditionSource : public ConditionSource {
private:
    std::string m_randomName;
    VerticalAnchor m_trueAtAndBelow;
    VerticalAnchor m_falseAtAndAbove;

public:
    VerticalGradientConditionSource(const std::string& randomName,
                                     const VerticalAnchor& trueAtAndBelow,
                                     const VerticalAnchor& falseAtAndAbove)
        : m_randomName(randomName)
        , m_trueAtAndBelow(trueAtAndBelow)
        , m_falseAtAndAbove(falseAtAndAbove)
    {}

    const std::string& randomName() const { return m_randomName; }
    const VerticalAnchor& trueAtAndBelow() const { return m_trueAtAndBelow; }
    const VerticalAnchor& falseAtAndAbove() const { return m_falseAtAndAbove; }

    std::unique_ptr<Condition> apply(Context& context) override;
};

//=============================================================================
// Singleton ConditionSource implementations
//=============================================================================

/**
 * ConditionWrapper - Wraps a non-owning pointer to a condition
 * Used for cached conditions that are owned by Context
 */
class ConditionWrapper : public Condition {
private:
    Condition* m_wrapped;
public:
    ConditionWrapper(Condition* wrapped) : m_wrapped(wrapped) {}
    bool test() override { return m_wrapped->test(); }
};

/**
 * AbovePreliminarySurface - Singleton condition source
 * Reference: SurfaceRules.java lines 459-476
 */
class AbovePreliminarySurfaceSource : public ConditionSource {
public:
    static AbovePreliminarySurfaceSource INSTANCE;

    std::unique_ptr<Condition> apply(Context& context) override {
        return std::make_unique<ConditionWrapper>(context.getAbovePreliminarySurfaceCondition());
    }
};

/**
 * Hole - Singleton condition source
 * Reference: SurfaceRules.java lines 478-495
 */
class HoleSource : public ConditionSource {
public:
    static HoleSource INSTANCE;

    std::unique_ptr<Condition> apply(Context& context) override {
        return std::make_unique<ConditionWrapper>(context.getHoleCondition());
    }
};

/**
 * Temperature - Singleton condition source
 * Reference: SurfaceRules.java lines 660-677
 */
class TemperatureSource : public ConditionSource {
public:
    static TemperatureSource INSTANCE;

    std::unique_ptr<Condition> apply(Context& context) override {
        return std::make_unique<ConditionWrapper>(context.getTemperatureCondition());
    }
};

/**
 * Steep - Singleton condition source
 * Reference: SurfaceRules.java lines 679-696
 */
class SteepSource : public ConditionSource {
public:
    static SteepSource INSTANCE;

    std::unique_ptr<Condition> apply(Context& context) override {
        return std::make_unique<ConditionWrapper>(context.getSteepCondition());
    }
};

//=============================================================================
// RuleSource Implementations
//=============================================================================

/**
 * BlockRuleSource - Creates StateRule for a fixed block
 * Reference: SurfaceRules.java lines 698-716
 */
class BlockRuleSource : public RuleSource {
private:
    BlockState* m_resultBlock;

public:
    BlockRuleSource(BlockState* block) : m_resultBlock(block) {}

    BlockState* resultBlock() const { return m_resultBlock; }

    std::unique_ptr<SurfaceRule> apply(Context& context) override;
};

/**
 * TestRuleSource - Creates TestRule with condition and followup
 * Reference: SurfaceRules.java lines 718-728
 */
class TestRuleSource : public RuleSource {
private:
    ConditionSource* m_ifTrue;
    RuleSource* m_thenRun;

public:
    TestRuleSource(ConditionSource* ifTrue, RuleSource* thenRun)
        : m_ifTrue(ifTrue), m_thenRun(thenRun) {}

    ConditionSource* ifTrue() const { return m_ifTrue; }
    RuleSource* thenRun() const { return m_thenRun; }

    std::unique_ptr<SurfaceRule> apply(Context& context) override;
};

/**
 * SequenceRuleSource - Creates SequenceRule from multiple rules
 * Reference: SurfaceRules.java lines 730-754
 */
class SequenceRuleSource : public RuleSource {
private:
    std::vector<RuleSource*> m_sequence;

public:
    SequenceRuleSource(const std::vector<RuleSource*>& sequence) : m_sequence(sequence) {}

    const std::vector<RuleSource*>& sequence() const { return m_sequence; }

    std::unique_ptr<SurfaceRule> apply(Context& context) override;
};

/**
 * Bandlands - Creates rule that uses SurfaceSystem's band for badlands
 * Reference: SurfaceRules.java lines 756-775
 */
class BandlandsSource : public RuleSource {
public:
    static BandlandsSource INSTANCE;

    std::unique_ptr<SurfaceRule> apply(Context& context) override;
};

//=============================================================================
// Static Factory Methods - Reference: SurfaceRules.java lines 41-124
//=============================================================================

namespace SurfaceRules {

// Pre-defined condition sources - Reference: lines 34-39, 130-137
extern ConditionSource* ON_FLOOR;
extern ConditionSource* UNDER_FLOOR;
extern ConditionSource* DEEP_UNDER_FLOOR;
extern ConditionSource* VERY_DEEP_UNDER_FLOOR;
extern ConditionSource* ON_CEILING;
extern ConditionSource* UNDER_CEILING;

/**
 * Initialize static condition sources
 * Must be called before using any surface rules
 */
void initializeStatics();

// Factory methods matching Java - Reference: lines 41-124
inline ConditionSource* stoneDepthCheck(int32_t offset, bool addSurfaceDepth, CaveSurface surfaceType) {
    return new StoneDepthCheck(offset, addSurfaceDepth, 0, surfaceType);
}

inline ConditionSource* stoneDepthCheck(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange, CaveSurface surfaceType) {
    return new StoneDepthCheck(offset, addSurfaceDepth, secondaryDepthRange, surfaceType);
}

inline ConditionSource* not_(ConditionSource* target) {
    return new NotConditionSource(target);
}

inline ConditionSource* yBlockCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier) {
    return new YConditionSource(anchor, surfaceDepthMultiplier, false);
}

inline ConditionSource* yStartCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier) {
    return new YConditionSource(anchor, surfaceDepthMultiplier, true);
}

inline ConditionSource* waterBlockCheck(int32_t offset, int32_t surfaceDepthMultiplier) {
    return new WaterConditionSource(offset, surfaceDepthMultiplier, false);
}

inline ConditionSource* waterStartCheck(int32_t offset, int32_t surfaceDepthMultiplier) {
    return new WaterConditionSource(offset, surfaceDepthMultiplier, true);
}

inline ConditionSource* isBiome(const std::vector<std::string>& biomes) {
    return new BiomeConditionSource(biomes);
}

inline ConditionSource* noiseCondition(const std::string& noise, double minRange) {
    return new NoiseThresholdConditionSource(noise, minRange, std::numeric_limits<double>::max());
}

inline ConditionSource* noiseCondition(const std::string& noise, double minRange, double maxRange) {
    return new NoiseThresholdConditionSource(noise, minRange, maxRange);
}

inline ConditionSource* verticalGradient(const std::string& randomName,
                                          const VerticalAnchor& trueAtAndBelow,
                                          const VerticalAnchor& falseAtAndAbove) {
    return new VerticalGradientConditionSource(randomName, trueAtAndBelow, falseAtAndAbove);
}

inline ConditionSource* steep() {
    return &SteepSource::INSTANCE;
}

inline ConditionSource* hole() {
    return &HoleSource::INSTANCE;
}

inline ConditionSource* abovePreliminarySurface() {
    return &AbovePreliminarySurfaceSource::INSTANCE;
}

inline ConditionSource* temperature() {
    return &TemperatureSource::INSTANCE;
}

// Rule source factory methods
inline RuleSource* ifTrue(ConditionSource* condition, RuleSource* next) {
    return new TestRuleSource(condition, next);
}

inline RuleSource* sequence(const std::vector<RuleSource*>& rules) {
    if (rules.empty()) {
        throw std::invalid_argument("Need at least 1 rule for a sequence");
    }
    return new SequenceRuleSource(rules);
}

inline RuleSource* state(BlockState* block) {
    return new BlockRuleSource(block);
}

// Convenience overload that takes a block name
inline RuleSource* state(const std::string& blockName) {
    return new BlockRuleSource(minecraft::world::level::block::Blocks::getDefaultState(blockName));
}

inline RuleSource* bandlands() {
    return &BandlandsSource::INSTANCE;
}

} // namespace SurfaceRules

} // namespace levelgen
} // namespace minecraft
