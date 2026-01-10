#pragma once

#include "core/BlockPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/MinecraftBlockType.h"
#include "random/XoroshiroRandomSource.h"
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
    virtual BlockState* getState(XoroshiroRandomSource& random, const core::BlockPos& pos) const = 0;

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
        : m_state(static_cast<BlockState*>(::world::MinecraftBlocks::get(blockName))) {}

    BlockState* getState(XoroshiroRandomSource& random, const core::BlockPos& pos) const override {
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
    BlockState* getState(XoroshiroRandomSource& random, const core::BlockPos& pos) const override {
        if (m_states.empty() || m_totalWeight <= 0) {
            return static_cast<BlockState*>(::world::MinecraftBlocks::AIR());
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
     * Reference: RotatedBlockProvider.java getState()
     *
     * Note: For logs, this would set the axis property to X, Y, or Z randomly
     */
    BlockState* getState(XoroshiroRandomSource& random, const core::BlockPos& pos) const override {
        // For now, return the base state
        // A full implementation would modify the axis property using setValue()
        return m_state;
    }
};

/**
 * RandomizedIntStateProvider - Provides block states with randomized integer property
 * Reference: RandomizedIntStateProvider.java
 */
class RandomizedIntStateProvider : public BlockStateProvider {
private:
    std::shared_ptr<BlockStateProvider> m_source;
    std::string m_property;
    int m_minValue;
    int m_maxValue;

public:
    RandomizedIntStateProvider(
        std::shared_ptr<BlockStateProvider> source,
        const std::string& property,
        int minValue,
        int maxValue
    )
        : m_source(source)
        , m_property(property)
        , m_minValue(minValue)
        , m_maxValue(maxValue)
    {}

    BlockState* getState(XoroshiroRandomSource& random, const core::BlockPos& pos) const override {
        BlockState* baseState = m_source->getState(random, pos);
        // A full implementation would set the integer property using setValue()
        // For now, return base state
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
