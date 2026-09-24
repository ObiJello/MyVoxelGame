#pragma once
#include <atomic>
#include <unordered_map>
#include <cstdint>

#include "world/level/block/state/BlockState.h"
#include "levelgen/WorldgenRandom.h"
#include "core/BlockPos.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/RuleTest.java
// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/TagMatchTest.java
// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/BlockMatchTest.java
// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/AlwaysTrueTest.java
// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/RandomBlockMatchTest.java
// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/RandomBlockStateMatchTest.java

namespace minecraft {
namespace levelgen {
namespace blockpredicates {
bool matchesBlockTagName(BlockState* state, const std::string& tag);
} // namespace blockpredicates

namespace structure {
namespace templatesystem {

/**
 * RuleTest - Abstract base class for testing if a block state matches
 * Reference: RuleTest.java
 *
 * Used by OreFeature to determine which blocks can be replaced.
 */
class RuleTest {
public:
    virtual ~RuleTest() = default;

    /**
     * Test if the block state at pos matches this rule
     * Reference: RuleTest.java test(state, pos, random) — 26.3 passes the
     * position so HeightMatchTest can gate on y.
     */
    virtual bool test(BlockState* state, const core::BlockPos& pos, WorldgenRandom& random) const = 0;
};

/**
 * AlwaysTrueTest - Always returns true
 * Reference: AlwaysTrueTest.java
 */
class AlwaysTrueTest : public RuleTest {
public:
    static AlwaysTrueTest INSTANCE;

    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        return true;
    }
};

/**
 * BlockMatchTest - Matches a specific block
 * Reference: BlockMatchTest.java
 */
class BlockMatchTest : public RuleTest {
private:
    std::string m_block;

public:
    explicit BlockMatchTest(const std::string& block) : m_block(block) {}

    /**
     * Test if block matches
     * Reference: BlockMatchTest.java lines 17-19
     */
    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        return state->getIdentifier() == m_block;
    }
};

/**
 * BlockStateMatchTest - Matches a specific block state (with properties)
 * Reference: BlockStateMatchTest.java
 */
class BlockStateMatchTest : public RuleTest {
private:
    BlockState* m_state;

public:
    explicit BlockStateMatchTest(BlockState* state) : m_state(state) {}

    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        // Match block name (full state match would require property comparison)
        return state->getIdentifier() == m_state->getIdentifier();
    }
};

/**
 * TagMatchTest - Matches blocks in a tag
 * Reference: TagMatchTest.java
 */
class TagMatchTest : public RuleTest {
private:
    std::string m_tag;
    mutable std::atomic<uintptr_t> m_last{0};

public:
    explicit TagMatchTest(const std::string& tag) : m_tag(tag) {}

    /**
     * Test if block is in tag
     * Reference: TagMatchTest.java lines 18-20
     */
    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        // Per-block-type memo: the string compares in testSlow ran for EVERY
        // candidate ore block (measured 2026-08-30, 7% of ore placement).
        // Java resolves the tag to a set of Blocks once. Thread-local because
        // rule tests are also reachable from structure processors on pool
        // threads.
        // Ore candidates are overwhelmingly one block type (stone/deepslate):
        // remember the last Block and its answer. Packed into one atomic so a
        // shared instance stays race-free (relaxed: a stale value is only a
        // cache miss).
        const uintptr_t block = reinterpret_cast<uintptr_t>(state->getBlock());
        const uintptr_t last = m_last.load(std::memory_order_relaxed);
        if ((last & ~uintptr_t(1)) == block && block != 0) return (last & 1) != 0;
        const bool r = testSlow(state, random);
        m_last.store(block | (r ? 1 : 0), std::memory_order_relaxed);
        return r;
    }
    bool testSlow(BlockState* state, WorldgenRandom& /*random*/) const {
        // Reference: TagMatchTest.java test() = state.is(tag), resolved from
        // the data/minecraft/tags/block files (26.3: deepslate_ore_replaceables
        // no longer holds tuff — see height_specific_ore_replaceables).
        return blockpredicates::matchesBlockTagName(state, m_tag);
    }
};

/**
 * RandomBlockMatchTest - Matches a block with probability
 * Reference: RandomBlockMatchTest.java
 */
class RandomBlockMatchTest : public RuleTest {
private:
    std::string m_block;
    float m_probability;

public:
    RandomBlockMatchTest(const std::string& block, float probability)
        : m_block(block), m_probability(probability) {}

    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        if (state->getIdentifier() != m_block) {
            return false;
        }
        return random.nextFloat() < m_probability;
    }
};

/**
 * RandomBlockStateMatchTest - Matches a block state with probability
 * Reference: RandomBlockStateMatchTest.java
 */
class RandomBlockStateMatchTest : public RuleTest {
private:
    BlockState* m_state;
    float m_probability;

public:
    RandomBlockStateMatchTest(BlockState* state, float probability)
        : m_state(state), m_probability(probability) {}

    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        if (state->getIdentifier() != m_state->getIdentifier()) {
            return false;
        }
        return random.nextFloat() < m_probability;
    }
};

/**
 * MultiBlockMatchTest - Matches any of multiple blocks
 * Not in vanilla Java, but useful for C++ implementation
 */
class MultiBlockMatchTest : public RuleTest {
private:
    std::unordered_set<std::string> m_blocks;

public:
    explicit MultiBlockMatchTest(const std::vector<std::string>& blocks)
        : m_blocks(blocks.begin(), blocks.end()) {}

    bool test(BlockState* state, const core::BlockPos& /*pos*/, WorldgenRandom& random) const override {
        return m_blocks.find(state->getIdentifier()) != m_blocks.end();
    }
};

/**
 * HeightMatchTest - Matches any block whose y lies in [minInclusive, maxInclusive]
 * Reference: HeightMatchTest.java (26.3)
 */
class HeightMatchTest : public RuleTest {
private:
    int m_minInclusive;
    int m_maxInclusive;

public:
    // DimensionType.MIN_Y / MAX_Y
    static constexpr int DIMENSION_MIN_Y = -2032;
    static constexpr int DIMENSION_MAX_Y = 2031;

    HeightMatchTest(int minInclusive, int maxInclusive)
        : m_minInclusive(minInclusive), m_maxInclusive(maxInclusive) {}

    static std::shared_ptr<RuleTest> min(int minInclusive) {
        return std::make_shared<HeightMatchTest>(minInclusive, DIMENSION_MAX_Y);
    }
    static std::shared_ptr<RuleTest> max(int maxInclusive) {
        return std::make_shared<HeightMatchTest>(DIMENSION_MIN_Y, maxInclusive);
    }

    bool test(BlockState* /*state*/, const core::BlockPos& pos, WorldgenRandom& /*random*/) const override {
        return m_minInclusive <= pos.getY() && pos.getY() <= m_maxInclusive;
    }
};

/**
 * AllOfRuleTest - true when every rule matches (short-circuits in order)
 * Reference: AllOfRuleTest.java (26.3)
 */
class AllOfRuleTest : public RuleTest {
private:
    std::vector<std::shared_ptr<RuleTest>> m_rules;

public:
    explicit AllOfRuleTest(std::vector<std::shared_ptr<RuleTest>> rules) : m_rules(std::move(rules)) {}

    bool test(BlockState* state, const core::BlockPos& pos, WorldgenRandom& random) const override {
        for (const auto& rule : m_rules) {
            if (!rule->test(state, pos, random)) return false;
        }
        return true;
    }
};

/**
 * AnyOfRuleTest - true when some rule matches (short-circuits in order)
 * Reference: AnyOfRuleTest.java (26.3)
 */
class AnyOfRuleTest : public RuleTest {
private:
    std::vector<std::shared_ptr<RuleTest>> m_rules;

public:
    explicit AnyOfRuleTest(std::vector<std::shared_ptr<RuleTest>> rules) : m_rules(std::move(rules)) {}

    bool test(BlockState* state, const core::BlockPos& pos, WorldgenRandom& random) const override {
        for (const auto& rule : m_rules) {
            if (rule->test(state, pos, random)) return true;
        }
        return false;
    }
};

/**
 * NotRuleTest - negation
 * Reference: NotRuleTest.java (26.3)
 */
class NotRuleTest : public RuleTest {
private:
    std::shared_ptr<RuleTest> m_rule;

public:
    explicit NotRuleTest(std::shared_ptr<RuleTest> rule) : m_rule(std::move(rule)) {}

    bool test(BlockState* state, const core::BlockPos& pos, WorldgenRandom& random) const override {
        return !m_rule->test(state, pos, random);
    }
};

/**
 * RuleTest.either(condition, ifTrue, ifFalse)
 *   = anyOf(allOf(condition, ifTrue), allOf(not(condition), ifFalse))
 * Reference: RuleTest.java either() (26.3). The condition is evaluated twice,
 * exactly as vanilla does.
 */
inline std::shared_ptr<RuleTest> eitherRuleTest(const std::shared_ptr<RuleTest>& condition,
                                                const std::shared_ptr<RuleTest>& ifTrue,
                                                const std::shared_ptr<RuleTest>& ifFalse) {
    return std::make_shared<AnyOfRuleTest>(std::vector<std::shared_ptr<RuleTest>>{
        std::make_shared<AllOfRuleTest>(std::vector<std::shared_ptr<RuleTest>>{condition, ifTrue}),
        std::make_shared<AllOfRuleTest>(std::vector<std::shared_ptr<RuleTest>>{
            std::make_shared<NotRuleTest>(condition), ifFalse})
    });
}

} // namespace templatesystem
} // namespace structure
} // namespace levelgen
} // namespace minecraft
