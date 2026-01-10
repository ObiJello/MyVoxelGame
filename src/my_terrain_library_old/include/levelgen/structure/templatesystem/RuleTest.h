#pragma once

#include "world/level/block/state/BlockState.h"
#include "random/XoroshiroRandomSource.h"
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
     * Test if the block state matches this rule
     * Reference: RuleTest.java line 11
     */
    virtual bool test(BlockState* state, XoroshiroRandomSource& random) const = 0;
};

/**
 * AlwaysTrueTest - Always returns true
 * Reference: AlwaysTrueTest.java
 */
class AlwaysTrueTest : public RuleTest {
public:
    static AlwaysTrueTest INSTANCE;

    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
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
    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
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

    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
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

public:
    explicit TagMatchTest(const std::string& tag) : m_tag(tag) {}

    /**
     * Test if block is in tag
     * Reference: TagMatchTest.java lines 18-20
     */
    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
        const std::string& name = state->getIdentifier();

        // Implement common block tags
        if (m_tag == "minecraft:stone_ore_replaceables") {
            return name == "minecraft:stone" ||
                   name == "minecraft:granite" ||
                   name == "minecraft:diorite" ||
                   name == "minecraft:andesite" ||
                   name == "minecraft:tuff";
        }
        if (m_tag == "minecraft:deepslate_ore_replaceables") {
            return name == "minecraft:deepslate" ||
                   name == "minecraft:tuff";
        }
        if (m_tag == "minecraft:base_stone_overworld") {
            return name == "minecraft:stone" ||
                   name == "minecraft:granite" ||
                   name == "minecraft:diorite" ||
                   name == "minecraft:andesite" ||
                   name == "minecraft:tuff" ||
                   name == "minecraft:deepslate";
        }
        if (m_tag == "minecraft:base_stone_nether") {
            return name == "minecraft:netherrack" ||
                   name == "minecraft:basalt" ||
                   name == "minecraft:blackstone";
        }
        if (m_tag == "minecraft:nether_carver_replaceables") {
            return name == "minecraft:netherrack" ||
                   name == "minecraft:basalt" ||
                   name == "minecraft:blackstone" ||
                   name == "minecraft:soul_sand" ||
                   name == "minecraft:soul_soil" ||
                   name == "minecraft:gravel" ||
                   name == "minecraft:magma_block";
        }

        return false;
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

    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
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

    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
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

    bool test(BlockState* state, XoroshiroRandomSource& random) const override {
        return m_blocks.find(state->getIdentifier()) != m_blocks.end();
    }
};

} // namespace templatesystem
} // namespace structure
} // namespace levelgen
} // namespace minecraft
