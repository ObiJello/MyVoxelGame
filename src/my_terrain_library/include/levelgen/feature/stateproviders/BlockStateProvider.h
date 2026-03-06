#pragma once

#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "levelgen/WorldgenRandom.h"
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
        Axis axis = static_cast<Axis>(axisIndex);

        // TODO: Implement BlockState::setValue() to actually set the axis property
        // For now, we consume the random to maintain parity, then return base state
        (void)axis;  // Suppress unused warning - random IS consumed

        return m_state;
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

        // Second: sample random value for property (CRITICAL - must consume random)
        // This matches UniformInt.sample() behavior: random.nextInt(range) + min
        int range = m_maxValue - m_minValue + 1;
        int value = random.nextInt(range) + m_minValue;

        // TODO: Implement BlockState::setValue() to actually set the property
        // For now, consume random to maintain parity, then return base state
        (void)value;  // Suppress unused warning - random IS consumed

        return baseState;
    }
};

// Implement static factory methods
inline std::shared_ptr<BlockStateProvider> BlockStateProvider::simple(BlockState* blockState) {
    return std::make_shared<SimpleStateProvider>(blockState);
}

inline std::shared_ptr<BlockStateProvider> BlockStateProvider::simple(const std::string& blockName) {
    return std::make_shared<SimpleStateProvider>(blockName);
}

} // namespace stateproviders
} // namespace feature
} // namespace levelgen
} // namespace minecraft
