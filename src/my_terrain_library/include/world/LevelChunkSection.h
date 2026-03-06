#pragma once

#include "world/level/block/state/BlockState.h"
#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"
#include "world/biome/Climate.h"
#include "world/biome/BiomeSource.h"
#include "util/PalettedContainer.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <array>
#include <algorithm>
#include <iostream>
#include <atomic>

namespace minecraft {
namespace world {

/**
 * BlockRegistry - Simple registry for block types
 * Implements IdMap interface for use with PalettedContainer
 * Reference: Java equivalent uses Registry.byNameCodec() for name-based lookup
 *
 * Supports both legacy BlockState* lookup and new BlockState property-based lookup.
 */
class BlockRegistry : public util::IdMap<BlockState*> {
private:
    std::vector<BlockState*> m_idToBlock;
    std::unordered_map<BlockState*, int32_t> m_blockToId;
    std::unordered_map<std::string, BlockState*> m_nameToBlock;  // Name-based lookup

    // Property-based lookup: maps "name|prop1=val1|prop2=val2" to block
    std::unordered_map<std::string, BlockState*> m_stateByProperties;

    /**
     * Build a lookup key from block name and properties
     * Format: "name|prop1=val1|prop2=val2" (properties sorted alphabetically)
     */
    static std::string buildStateKey(const std::string& name,
                                      const std::unordered_map<std::string, std::string>& properties) {
        if (properties.empty()) {
            return name;
        }

        // Sort properties by name for consistent key
        std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
        std::sort(sorted.begin(), sorted.end());

        std::string key = name;
        for (const auto& [propName, propValue] : sorted) {
            key += "|" + propName + "=" + propValue;
        }
        return key;
    }

public:
    void registerBlock(BlockState* block) {
        int32_t id = static_cast<int32_t>(m_idToBlock.size());
        m_idToBlock.push_back(block);
        m_blockToId[block] = id;
        m_nameToBlock[block->getIdentifier()] = block;  // Register by name

        // Also register in state-by-properties lookup
        std::string stateKey = buildStateKey(block->getIdentifier(), block->getProperties());
        m_stateByProperties[stateKey] = block;
    }

    /**
     * Look up block by resource location name (e.g., "minecraft:stone")
     * Reference: Java Registry.getValue(ResourceLocation)
     */
    BlockState* getByName(const std::string& name) const {
        auto it = m_nameToBlock.find(name);
        return it != m_nameToBlock.end() ? it->second : nullptr;
    }

    /**
     * Look up specific block state by name and properties
     * Reference: Used for deserializing block states from NBT
     *
     * @param name Block identifier (e.g., "minecraft:oak_stairs")
     * @param properties Property name-value pairs (e.g., {"facing": "north", "half": "bottom"})
     * @return Matching block state, or nullptr if not found
     */
    BlockState* getBlockState(const std::string& name,
                               const std::unordered_map<std::string, std::string>& properties) const {
        std::string stateKey = buildStateKey(name, properties);
        auto it = m_stateByProperties.find(stateKey);
        if (it != m_stateByProperties.end()) {
            return it->second;
        }
        // Fall back to default block if exact state not found
        return getByName(name);
    }

    /**
     * Get default block state for a block name
     * Reference: Block.defaultBlockState()
     */
    BlockState* getDefaultBlockState(const std::string& name) const {
        return getByName(name);
    }

    int32_t getId(BlockState* value) const override {
        auto it = m_blockToId.find(value);
        return it != m_blockToId.end() ? it->second : -1;
    }

    BlockState* byId(int32_t id) const override {
        if (id >= 0 && id < static_cast<int32_t>(m_idToBlock.size())) {
            return m_idToBlock[id];
        }
        return nullptr;
    }

    BlockState* byIdOrThrow(int32_t id) const override {
        BlockState* block = byId(id);
        if (block == nullptr) {
            throw std::runtime_error("Unknown block ID: " + std::to_string(id));
        }
        return block;
    }

    int32_t size() const override {
        return static_cast<int32_t>(m_idToBlock.size());
    }
};

/**
 * LevelChunkSection - A 16x16x16 section of blocks in a chunk
 * Reference: net/minecraft/world/level/chunk/LevelChunkSection.java
 *
 * Chunks are divided into sections of 16x16x16 blocks for efficient storage.
 * Each section uses a PalettedContainer for block state storage.
 */
class LevelChunkSection {
public:
    static constexpr int32_t SECTION_WIDTH = 16;
    static constexpr int32_t SECTION_HEIGHT = 16;
    static constexpr int32_t SECTION_SIZE = 4096;  // 16 * 16 * 16
    static constexpr int32_t BIOMES_PER_SECTION = 64;  // 4 * 4 * 4 biomes per section

private:
    int16_t m_nonEmptyBlockCount;
    int16_t m_tickingBlockCount;
    int16_t m_tickingFluidCount;
    util::PalettedContainer<BlockState*> m_states;

    // Biome storage - 4x4x4 biomes per section (quart resolution)
    // Reference: LevelChunkSection.java biomes field
    std::array<biome::BiomeKey, BIOMES_PER_SECTION> m_biomes;

    // Reference counting for BulkSectionAccess
    // Reference: LevelChunkSection.java refCount field
    std::atomic<int32_t> m_refCount{0};

public:
    /**
     * Constructor with default air block
     */
    LevelChunkSection(BlockState* airBlock, util::BlockStateStrategy<BlockState*>& strategy)
        : m_nonEmptyBlockCount(0)
        , m_tickingBlockCount(0)
        , m_tickingFluidCount(0)
        , m_states(airBlock, strategy)
    {}

    /**
     * Copy constructor
     */
    LevelChunkSection(const LevelChunkSection& other)
        : m_nonEmptyBlockCount(other.m_nonEmptyBlockCount)
        , m_tickingBlockCount(other.m_tickingBlockCount)
        , m_tickingFluidCount(other.m_tickingFluidCount)
        , m_states(other.m_states)
    {}

    /**
     * Get block state at position
     * Reference: LevelChunkSection.java getBlockState() lines 43-45
     *
     * @param x Local X coordinate (0-15)
     * @param y Local Y coordinate (0-15)
     * @param z Local Z coordinate (0-15)
     */
    BlockState* getBlockState(int32_t x, int32_t y, int32_t z) const {
        return m_states.get(x, y, z);
    }

    /**
     * Set block state at position
     * Reference: LevelChunkSection.java setBlockState() lines 63-96
     *
     * @param x Local X coordinate (0-15)
     * @param y Local Y coordinate (0-15)
     * @param z Local Z coordinate (0-15)
     * @param state New block state
     * @param checkThreading Whether to use thread-safe access
     * @return Previous block state
     */
    BlockState* setBlockState(int32_t x, int32_t y, int32_t z, BlockState* state, bool checkThreading = true) {
        BlockState* previous;
        if (checkThreading) {
            previous = m_states.getAndSet(x, y, z, state);
        } else {
            previous = m_states.getAndSetUnchecked(x, y, z, state);
        }

        // Update counters
        if (!previous->isAir()) {
            --m_nonEmptyBlockCount;
            // Note: isRandomlyTicking would require additional interface methods
        }

        if (previous->isFluid()) {
            --m_tickingFluidCount;
        }

        if (!state->isAir()) {
            ++m_nonEmptyBlockCount;
        }

        if (state->isFluid()) {
            ++m_tickingFluidCount;
        }

        return previous;
    }

    /**
     * Check if section contains only air
     * Reference: LevelChunkSection.java hasOnlyAir() lines 98-100
     */
    bool hasOnlyAir() const {
        return m_nonEmptyBlockCount == 0;
    }

    /**
     * Increment reference count for BulkSectionAccess tracking
     * Reference: LevelChunkSection.java acquire()
     */
    void acquire() {
        m_refCount.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * Decrement reference count
     * Reference: LevelChunkSection.java release()
     */
    void release() {
        m_refCount.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * Get current reference count
     */
    int32_t getRefCount() const {
        return m_refCount.load(std::memory_order_relaxed);
    }

    /**
     * Check if section has any randomly ticking blocks or fluids
     * Reference: LevelChunkSection.java isRandomlyTicking() lines 102-104
     */
    bool isRandomlyTicking() const {
        return m_tickingBlockCount > 0 || m_tickingFluidCount > 0;
    }

    /**
     * Recalculate block counts from scratch
     * Reference: LevelChunkSection.java recalcBlockCounts() lines 114-149
     */
    void recalcBlockCounts() {
        int32_t nonEmptyCount = 0;
        int32_t tickingBlockCount = 0;
        int32_t tickingFluidCount = 0;

        m_states.count([&](BlockState* state, int32_t count) {
            if (!state->isAir()) {
                nonEmptyCount += count;
            }
            if (state->isFluid()) {
                tickingFluidCount += count;
            }
        });

        m_nonEmptyBlockCount = static_cast<int16_t>(nonEmptyCount);
        m_tickingBlockCount = static_cast<int16_t>(tickingBlockCount);
        m_tickingFluidCount = static_cast<int16_t>(tickingFluidCount);
    }

    /**
     * Get internal block states container
     */
    util::PalettedContainer<BlockState*>& getStates() {
        return m_states;
    }

    const util::PalettedContainer<BlockState*>& getStates() const {
        return m_states;
    }

    /**
     * Check if section might contain a block matching predicate
     * Reference: LevelChunkSection.java maybeHas() lines 183-185
     */
    bool maybeHas(const std::function<bool(BlockState*)>& predicate) const {
        return m_states.maybeHas(predicate);
    }

    /**
     * Create a copy of this section
     * Reference: LevelChunkSection.java copy() lines 206-208
     */
    LevelChunkSection copy() const {
        return LevelChunkSection(*this);
    }

    /**
     * Get non-empty block count
     */
    int16_t getNonEmptyBlockCount() const {
        return m_nonEmptyBlockCount;
    }

    //=========================================================================
    // Biome Methods
    // Reference: LevelChunkSection.java biome methods
    //=========================================================================

    /**
     * Get biome at quart-local coordinates (0-3 for each axis)
     * Reference: LevelChunkSection.java getNoiseBiome() line 187-189
     *
     * @param quartX Local quart X (0-3)
     * @param quartY Local quart Y (0-3)
     * @param quartZ Local quart Z (0-3)
     * @return BiomeKey at this position
     */
    biome::BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const {
        // Index into 4x4x4 array: x + z*4 + y*16
        int32_t index = (quartX & 3) + ((quartZ & 3) << 2) + ((quartY & 3) << 4);
        return m_biomes[index];
    }

    /**
     * Set biome at quart-local coordinates
     *
     * @param quartX Local quart X (0-3)
     * @param quartY Local quart Y (0-3)
     * @param quartZ Local quart Z (0-3)
     * @param biome BiomeKey to set
     */
    void setNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ, biome::BiomeKey biome) {
        int32_t index = (quartX & 3) + ((quartZ & 3) << 2) + ((quartY & 3) << 4);
        m_biomes[index] = biome;
    }

    /**
     * Fill biomes from noise for this section
     * Reference: LevelChunkSection.java fillBiomesFromNoise() lines 191-204
     *
     * @param biomeSource The biome source to sample from
     * @param sampler Climate sampler for noise
     * @param quartMinX Quart X of section origin
     * @param quartMinY Quart Y of section origin
     * @param quartMinZ Quart Z of section origin
     */
    void fillBiomesFromNoise(
        biome::BiomeSource* biomeSource,
        const biome::Climate::Sampler& sampler,
        int32_t quartMinX,
        int32_t quartMinY,
        int32_t quartMinZ
    ) {
        // Reference: LevelChunkSection.java lines 192-203
        // CRITICAL: Must iterate in X-Y-Z order (X outer, Z inner) to match Java!
        // The RTree biome lookup uses lastResult caching, so iteration order affects results
        // at biome boundaries. Java uses: for(x) for(y) for(z)
        for (int32_t x = 0; x < 4; ++x) {
            for (int32_t y = 0; y < 4; ++y) {
                for (int32_t z = 0; z < 4; ++z) {
                    int32_t quartX = quartMinX + x;
                    int32_t quartY = quartMinY + y;
                    int32_t quartZ = quartMinZ + z;

                    // Get biome from source at absolute quart coordinates
                    biome::BiomeKey biome = biomeSource->getNoiseBiome(
                        quartX, quartY, quartZ, sampler
                    );

                    setNoiseBiome(x, y, z, biome);
                }
            }
        }
    }

    /**
     * Get raw biome array for serialization
     */
    std::array<biome::BiomeKey, BIOMES_PER_SECTION>& getBiomes() {
        return m_biomes;
    }

    const std::array<biome::BiomeKey, BIOMES_PER_SECTION>& getBiomes() const {
        return m_biomes;
    }
};

} // namespace world
} // namespace minecraft
