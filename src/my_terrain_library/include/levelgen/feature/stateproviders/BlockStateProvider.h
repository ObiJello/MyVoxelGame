#pragma once

#include "core/BlockPos.h"
#include "math/Mth.h"
#include "levelgen/WorldgenRandom.h"
#include "random/LegacyRandomSource.h"
#include "synth/NormalNoise.h"
#include "util/InclusiveRange.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include <string>
#include <vector>
#include <memory>

// Reference: net/minecraft/world/level/levelgen/feature/stateproviders/BlockStateProvider.java
// Reference: net/minecraft/world/level/levelgen/feature/stateproviders/SimpleStateProvider.java
// Reference: net/minecraft/world/level/levelgen/feature/stateproviders/WeightedStateProvider.java
// Reference: net/minecraft/world/level/levelgen/feature/stateproviders/RotatedBlockProvider.java
// Reference: net/minecraft/world/level/levelgen/feature/stateproviders/NoiseBasedStateProvider.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace stateproviders {

/**
 * BlockStateProvider - Provides block states for feature placement
 * Reference: BlockStateProvider.java
 */
class BlockStateProvider {
public:
    virtual ~BlockStateProvider() = default;

    /**
     * Get a block state at the given position
     * Reference: BlockStateProvider.java line 23
     */
    virtual BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const = 0;

    /**
     * Create a simple state provider for a single block
     * Reference: BlockStateProvider.java lines 13-18
     */
    static std::shared_ptr<BlockStateProvider> simple(BlockState* blockState);
    static std::shared_ptr<BlockStateProvider> simple(minecraft::world::level::block::Block* block);
    static std::shared_ptr<BlockStateProvider> simple(const std::string& blockName);
};

/**
 * SimpleStateProvider - Always provides the same block state
 * Reference: SimpleStateProvider.java
 */
class SimpleStateProvider : public BlockStateProvider {
private:
    BlockState* m_state;

public:
    explicit SimpleStateProvider(BlockState* blockState) : m_state(blockState) {}
    explicit SimpleStateProvider(const std::string& blockName)
        : m_state(static_cast<BlockState*>(minecraft::world::level::block::Blocks::getDefaultState(blockName))) {}

    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        return m_state;
    }
};

/**
 * WeightedStateEntry - A block state with weight for WeightedStateProvider
 */
struct WeightedStateEntry {
    BlockState* state;
    int weight;

    WeightedStateEntry(BlockState* s, int w) : state(s), weight(w) {}
};

/**
 * WeightedStateProvider - Provides block states based on weights
 * Reference: WeightedStateProvider.java
 */
class WeightedStateProvider : public BlockStateProvider {
private:
    std::vector<WeightedStateEntry> m_states;
    int m_totalWeight;

public:
    explicit WeightedStateProvider(const std::vector<WeightedStateEntry>& states)
        : m_states(states)
        , m_totalWeight(0)
    {
        for (const auto& entry : m_states) {
            m_totalWeight += entry.weight;
        }
    }

    /**
     * Get weighted random state
     * Reference: WeightedStateProvider.java getState()
     */
    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        if (m_states.empty() || m_totalWeight <= 0) {
            return static_cast<BlockState*>(minecraft::world::level::block::Blocks::AIR->defaultBlockState());
        }

        int target = random.nextInt(m_totalWeight);
        int sum = 0;
        for (const auto& entry : m_states) {
            sum += entry.weight;
            if (target < sum) {
                return entry.state;
            }
        }
        return m_states.back().state;
    }
};

/**
 * Direction::Axis enum for log rotation
 * Reference: Direction.java Axis enum
 */
enum class Axis {
    X = 0,
    Y = 1,
    Z = 2
};

/**
 * RotatedBlockProvider - Provides a block state with random rotation
 * Reference: RotatedBlockProvider.java
 */
class RotatedBlockProvider : public BlockStateProvider {
private:
    BlockState* m_state;

public:
    explicit RotatedBlockProvider(BlockState* blockState) : m_state(blockState) {}

    /**
     * Get state with random axis
     * Reference: RotatedBlockProvider.java getState() line 25
     *
     * CRITICAL: Must consume random.nextInt(3) for axis selection to match Java.
     * Java code:
     *   Direction.Axis randomAxis = Direction.Axis.getRandom(random);
     *   return this.block.defaultBlockState().trySetValue(RotatedPillarBlock.AXIS, randomAxis);
     *
     * Direction.Axis.getRandom calls Util.getRandom(VALUES, random) which calls:
     *   random.nextInt(array.length) = random.nextInt(3)
     */
    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        // Reference: Direction.Axis.getRandom(random) -> Util.getRandom(VALUES, random)
        // CRITICAL: This MUST consume nextInt(3) for random state parity
        int axisIndex = random.nextInt(3);  // 0=X, 1=Y, 2=Z
        if (!m_state) {
            return nullptr;
        }

        return m_state->trySetValue(
            *minecraft::world::level::block::RotatedPillarBlock::AXIS,
            static_cast<core::Axis>(axisIndex)
        );
    }
};

/**
 * RandomizedIntStateProvider - Provides block states with randomized integer property
 * Reference: RandomizedIntStateProvider.java
 *
 * CRITICAL: Java uses an IntProvider for 'values' which consumes random.
 * We must consume random in the same way for parity.
 */
class RandomizedIntStateProvider : public BlockStateProvider {
private:
    std::shared_ptr<BlockStateProvider> m_source;
    std::string m_propertyName;
    int m_minValue;
    int m_maxValue;

public:
    RandomizedIntStateProvider(
        std::shared_ptr<BlockStateProvider> source,
        const std::string& propertyName,
        int minValue,
        int maxValue
    )
        : m_source(source)
        , m_propertyName(propertyName)
        , m_minValue(minValue)
        , m_maxValue(maxValue)
    {}

    /**
     * Get state with randomized integer property
     * Reference: RandomizedIntStateProvider.java getState()
     *
     * Java code:
     *   BlockState unmodifiedState = this.source.getState(random, pos);
     *   // ... property lookup ...
     *   return unmodifiedState.setValue(this.property, this.values.sample(random));
     *
     * CRITICAL: Must consume random for property value selection to match Java.
     * Java's IntProvider.sample(random) typically calls random.nextInt().
     */
    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        // Reference: RandomizedIntStateProvider.java getState()
        // First: get base state from source (may consume random if source does)
        BlockState* baseState = m_source->getState(random, pos);
        if (!baseState) {
            return nullptr;
        }

        // Second: sample random value for property (CRITICAL - must consume random)
        // This matches UniformInt.sample() behavior: random.nextInt(range) + min
        int range = m_maxValue - m_minValue + 1;
        int value = random.nextInt(range) + m_minValue;

        const auto* propertyBase = baseState->getBlock()->getStateDefinition().getProperty(m_propertyName);
        if (const auto* property = dynamic_cast<const minecraft::world::level::block::state::properties::Property<int>*>(propertyBase)) {
            return baseState->trySetValue(*property, value);
        }

        return baseState;
    }
};

class NoiseBasedStateProvider : public BlockStateProvider {
protected:
    int64_t m_seed;
    NormalNoise::NoiseParameters m_parameters;
    float m_scale;
    NormalNoise m_noise;

    NoiseBasedStateProvider(int64_t seed, const NormalNoise::NoiseParameters& parameters, float scale)
        : m_seed(seed)
        , m_parameters(parameters)
        , m_scale(scale)
        , m_noise([&]() {
            WorldgenRandom random{LegacyRandomSource(seed)};
            return NormalNoise::create(random, parameters);
        }()) {}

    double getNoiseValue(const core::BlockPos& pos, double scale) const {
        return m_noise.getValue(
            static_cast<double>(pos.getX()) * scale,
            static_cast<double>(pos.getY()) * scale,
            static_cast<double>(pos.getZ()) * scale
        );
    }
};

class NoiseProvider : public NoiseBasedStateProvider {
protected:
    std::vector<BlockState*> m_states;

    BlockState* getRandomState(const std::vector<BlockState*>& states, const core::BlockPos& pos, double scale) const {
        return getRandomState(states, getNoiseValue(pos, scale));
    }

    BlockState* getRandomState(const std::vector<BlockState*>& states, double noiseValue) const {
        double placementValue = Mth::clamp((1.0 + noiseValue) / 2.0, 0.0, 0.9999);
        return states[static_cast<size_t>(placementValue * static_cast<double>(states.size()))];
    }

public:
    NoiseProvider(
        int64_t seed,
        const NormalNoise::NoiseParameters& parameters,
        float scale,
        const std::vector<BlockState*>& states
    )
        : NoiseBasedStateProvider(seed, parameters, scale)
        , m_states(states) {}

    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        (void)random;
        return getRandomState(m_states, pos, static_cast<double>(m_scale));
    }
};

class NoiseThresholdProvider : public NoiseBasedStateProvider {
public:
    NoiseThresholdProvider(
        int64_t seed,
        const NormalNoise::NoiseParameters& parameters,
        float scale,
        float threshold,
        float highChance,
        BlockState* defaultState,
        const std::vector<BlockState*>& lowStates,
        const std::vector<BlockState*>& highStates
    )
        : NoiseBasedStateProvider(seed, parameters, scale)
        , m_threshold(threshold)
        , m_highChance(highChance)
        , m_defaultState(defaultState)
        , m_lowStates(lowStates)
        , m_highStates(highStates) {}

    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        double localValue = getNoiseValue(pos, static_cast<double>(m_scale));
        if (localValue < static_cast<double>(m_threshold)) {
            return m_lowStates[random.nextInt(static_cast<int32_t>(m_lowStates.size()))];
        }

        return random.nextFloat() < m_highChance
            ? m_highStates[random.nextInt(static_cast<int32_t>(m_highStates.size()))]
            : m_defaultState;
    }

private:
    float m_threshold;
    float m_highChance;
    BlockState* m_defaultState;
    std::vector<BlockState*> m_lowStates;
    std::vector<BlockState*> m_highStates;
};

class DualNoiseProvider : public NoiseProvider {
public:
    DualNoiseProvider(
        const minecraft::util::InclusiveRange<int>& variety,
        const NormalNoise::NoiseParameters& slowNoiseParameters,
        float slowScale,
        int64_t seed,
        const NormalNoise::NoiseParameters& parameters,
        float scale,
        const std::vector<BlockState*>& states
    )
        : NoiseProvider(seed, parameters, scale, states)
        , m_variety(variety)
        , m_slowNoiseParameters(slowNoiseParameters)
        , m_slowScale(slowScale)
        , m_slowNoise([&]() {
            WorldgenRandom random{LegacyRandomSource(seed)};
            return NormalNoise::create(random, slowNoiseParameters);
        }()) {}

    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const override {
        (void)random;

        double varietyNoise = getSlowNoiseValue(pos);
        int localVariety = static_cast<int>(Mth::clampedMap(
            varietyNoise,
            static_cast<double>(-1.0f),
            static_cast<double>(1.0f),
            static_cast<double>(m_variety.minInclusive()),
            static_cast<double>(m_variety.maxInclusive() + 1)
        ));

        std::vector<BlockState*> possibleStates;
        possibleStates.reserve(static_cast<size_t>(localVariety));
        for (int i = 0; i < localVariety; ++i) {
            possibleStates.push_back(getRandomState(
                m_states,
                getSlowNoiseValue(pos.offset(i * 54545, 0, i * 34234))
            ));
        }

        return getRandomState(possibleStates, pos, static_cast<double>(m_scale));
    }

private:
    double getSlowNoiseValue(const core::BlockPos& pos) const {
        return m_slowNoise.getValue(
            static_cast<double>(static_cast<float>(pos.getX()) * m_slowScale),
            static_cast<double>(static_cast<float>(pos.getY()) * m_slowScale),
            static_cast<double>(static_cast<float>(pos.getZ()) * m_slowScale)
        );
    }

    minecraft::util::InclusiveRange<int> m_variety;
    NormalNoise::NoiseParameters m_slowNoiseParameters;
    float m_slowScale;
    NormalNoise m_slowNoise;
};

// Implement static factory methods
inline std::shared_ptr<BlockStateProvider> BlockStateProvider::simple(BlockState* blockState) {
    return std::make_shared<SimpleStateProvider>(blockState);
}

inline std::shared_ptr<BlockStateProvider> BlockStateProvider::simple(minecraft::world::level::block::Block* block) {
    return std::make_shared<SimpleStateProvider>(block ? block->defaultBlockState() : nullptr);
}

inline std::shared_ptr<BlockStateProvider> BlockStateProvider::simple(const std::string& blockName) {
    return std::make_shared<SimpleStateProvider>(blockName);
}

} // namespace stateproviders
} // namespace feature
} // namespace levelgen
} // namespace minecraft
