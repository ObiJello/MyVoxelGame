#pragma once

#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "world/ChunkPos.h"
#include "world/LevelChunkSection.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/biome/BiomeSource.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/Biomes.h"
#include "levelgen/Heightmap.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/NoiseChunk.h"
#include "core/BlockPos.h"
#include "core/QuartPos.h"
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <iostream>
#include <functional>

namespace minecraft {

namespace world {

/**
 * ProtoChunk - A chunk during world generation
 * Reference: net/minecraft/world/level/chunk/ProtoChunk.java
 *
 * ProtoChunk is used during terrain generation to store block data
 * before the chunk becomes a full LevelChunk. It implements IChunk
 * for compatibility with the existing codebase.
 */
class ProtoChunk : public IChunk, public biome::BiomeManager::NoiseBiomeSource {
public:
    static constexpr int32_t SECTION_COUNT_OVERWORLD = 24;  // 384 / 16
    static constexpr int32_t SECTION_COUNT_NETHER = 8;      // 128 / 16
    static constexpr int32_t SECTION_COUNT_END = 16;        // 256 / 16

private:
    ChunkPos m_pos;
    int32_t m_minY;
    int32_t m_height;
    int32_t m_sectionCount;

    // Block storage
    std::vector<std::unique_ptr<LevelChunkSection>> m_sections;
    util::BlockStateStrategy<BlockState*> m_blockStrategy;
    BlockState* m_airBlock;
    BlockState* m_defaultBlock;

    // Heightmaps
    std::map<levelgen::Heightmap::Types, std::unique_ptr<levelgen::Heightmap>> m_heightmaps;

    // Post-processing positions (packed as shorts)
    std::vector<std::set<int16_t>> m_postProcessing;

    // Carving mask for cave carving
    std::unique_ptr<levelgen::carver::CarvingMask> m_carvingMask;

    // Cached NoiseChunk for reuse across generation stages
    // Reference: ProtoChunk.java noiseChunk field
    // Java uses getOrCreateNoiseChunk() to cache and reuse
    levelgen::NoiseChunk* m_noiseChunk;

    // Chunk generation status
    // Reference: ProtoChunk.java status field
    const chunk::status::ChunkStatus* m_status;

    // Time players have spent in this chunk (in ticks)
    // Reference: ChunkAccess.java inhabitedTime field
    int64_t m_inhabitedTime;

public:
    // Convert Y to section index
    int32_t getSectionIndex(int32_t y) const {
        return (y - m_minY) >> 4;  // Divide by 16
    }

    // Get Y coordinate at bottom of section
    int32_t getSectionYFromIndex(int32_t index) const {
        return (index << 4) + m_minY;
    }

    /**
     * Get or create the carving mask for this chunk
     */
    levelgen::carver::CarvingMask& getOrCreateCarvingMask() {
        if (!m_carvingMask) {
            m_carvingMask = std::make_unique<levelgen::carver::CarvingMask>(m_height, m_minY);
        }
        return *m_carvingMask;
    }

    /**
     * Get or create the NoiseChunk for this chunk
     * Reference: ProtoChunk.java getOrCreateNoiseChunk()
     *
     * Java signature: public NoiseChunk getOrCreateNoiseChunk(Function<ChunkAccess, NoiseChunk> supplier)
     *
     * This caches the NoiseChunk so it can be reused across BIOMES, NOISE, SURFACE, and CARVERS stages.
     * Creating a NoiseChunk is expensive (~10ms), so caching it saves significant time.
     *
     * @param supplier Function that creates the NoiseChunk if it doesn't exist
     * @return The cached or newly created NoiseChunk
     */
    levelgen::NoiseChunk* getOrCreateNoiseChunk(std::function<levelgen::NoiseChunk*(IChunk*)> supplier) {
        if (m_noiseChunk == nullptr) {
            m_noiseChunk = supplier(this);
        }
        return m_noiseChunk;
    }

    /**
     * Set the cached NoiseChunk (for ownership transfer)
     */
    void setNoiseChunk(levelgen::NoiseChunk* noiseChunk) {
        if (m_noiseChunk != nullptr && m_noiseChunk != noiseChunk) {
            delete m_noiseChunk;
        }
        m_noiseChunk = noiseChunk;
    }

    /**
     * Get the cached NoiseChunk (may be nullptr)
     */
    levelgen::NoiseChunk* getNoiseChunk() const {
        return m_noiseChunk;
    }

    /**
     * Constructor
     * Reference: ChunkAccess.java constructor
     *
     * @param pos - Chunk position
     * @param minY - Minimum Y coordinate (e.g., -64 for Overworld)
     * @param height - Height in blocks (e.g., 384 for Overworld)
     * @param airBlock - Block type for air
     * @param defaultBlock - Default block for unset positions
     * @param registry - Block registry for palette
     */
    ProtoChunk(
        const ChunkPos& pos,
        int32_t minY,
        int32_t height,
        BlockState* airBlock,
        BlockState* defaultBlock,
        BlockRegistry* registry
    )
        : m_pos(pos)
        , m_minY(minY)
        , m_height(height)
        , m_sectionCount(height >> 4)
        , m_blockStrategy(registry)
        , m_airBlock(airBlock)
        , m_defaultBlock(defaultBlock)
        , m_noiseChunk(nullptr)
        , m_status(&chunk::status::ChunkStatus::EMPTY)
        , m_inhabitedTime(0)
    {
        // Initialize sections
        m_sections.resize(m_sectionCount);
        for (int32_t i = 0; i < m_sectionCount; ++i) {
            m_sections[i] = std::make_unique<LevelChunkSection>(airBlock, m_blockStrategy);
        }

        // Initialize post-processing
        m_postProcessing.resize(m_sectionCount);

        // Create worldgen heightmaps with block getter
        auto blockGetter = [this](int32_t x, int32_t y, int32_t z) -> const BlockState* {
            return this->getBlockState(x, y, z);
        };

        m_heightmaps[levelgen::Heightmap::Types::WORLD_SURFACE_WG] =
            std::make_unique<levelgen::Heightmap>(minY, height, levelgen::Heightmap::Types::WORLD_SURFACE_WG, blockGetter);
        m_heightmaps[levelgen::Heightmap::Types::OCEAN_FLOOR_WG] =
            std::make_unique<levelgen::Heightmap>(minY, height, levelgen::Heightmap::Types::OCEAN_FLOOR_WG, blockGetter);
    }

    /**
     * Destructor - cleans up cached NoiseChunk
     */
    ~ProtoChunk() {
        if (m_noiseChunk != nullptr) {
            delete m_noiseChunk;
            m_noiseChunk = nullptr;
        }
    }

    // IChunk interface implementation

    ChunkPos getPos() const override {
        return m_pos;
    }

    int getMinBuildHeight() const override {
        return m_minY;
    }

    int getMaxBuildHeight() const override {
        return m_minY + m_height;
    }

    /**
     * Check if Y coordinate is outside build height
     */
    bool isOutsideBuildHeight(int32_t y) const {
        return y < m_minY || y >= m_minY + m_height;
    }

    /**
     * Get number of sections
     */
    int32_t getSectionsCount() const override {
        return m_sectionCount;
    }

    /**
     * Get section by index
     * Reference: ChunkAccess.java getSection()
     */
    LevelChunkSection& getSection(int32_t index) override {
        return *m_sections[index];
    }

    const LevelChunkSection& getSection(int32_t index) const {
        return *m_sections[index];
    }

    /**
     * Get block state at absolute position
     * Reference: ProtoChunk.java getBlockState() lines 77-85
     */
    BlockState* getBlockState(const core::BlockPos& pos) override {
        return getBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15);
    }

    /**
     * Get block state at local position
     * Reference: ProtoChunk.java getBlockState() lines 77-85
     */
    BlockState* getBlockState(int localX, int y, int localZ) override {
        if (isOutsideBuildHeight(y)) {
            return m_airBlock;  // VOID_AIR in Minecraft
        }

        int32_t sectionIndex = getSectionIndex(y);
        LevelChunkSection& section = *m_sections[sectionIndex];

        if (section.hasOnlyAir()) {
            return m_airBlock;
        }

        // NOTE: Despite the parameter names, callers may pass world coords
        // Convert to local coords just like the BlockPos version does
        return section.getBlockState(localX & 15, y & 15, localZ & 15);
    }

    /**
     * Set block state at absolute position
     * Reference: ProtoChunk.java setBlockState() lines 97-151
     */
    BlockState* setBlockState(const core::BlockPos& pos, BlockState* state, bool moved) override {
        return setBlockState(pos.getX() & 15, pos.getY(), pos.getZ() & 15, state, moved);
    }

    /**
     * Set block state at local position
     * Reference: ProtoChunk.java setBlockState() lines 97-151
     */
    BlockState* setBlockState(int localX, int y, int localZ, BlockState* state, bool moved) override {
        if (isOutsideBuildHeight(y)) {
            return m_airBlock;
        }

        int32_t sectionIndex = getSectionIndex(y);
        LevelChunkSection& section = *m_sections[sectionIndex];
        bool wasEmpty = section.hasOnlyAir();

        // Optimization: skip if already air and setting air
        if (wasEmpty && state->isAir()) {
            return state;
        }

        int32_t localY = y & 15;
        BlockState* oldState = section.setBlockState(localX, localY, localZ, state, !moved);

        // Update heightmaps
        // Reference: ProtoChunk.java lines 144-146
        for (auto& [type, heightmap] : m_heightmaps) {
            heightmap->update(localX, y, localZ, state);
        }

        return oldState;
    }

    /**
     * Mark position for post-processing (fluid updates, etc.)
     * Reference: ProtoChunk.java markPosForPostprocessing() lines 236-240
     */
    void markPosForPostprocessing(const core::BlockPos& pos) override {
        if (!isOutsideBuildHeight(pos.getY())) {
            int32_t sectionIndex = getSectionIndex(pos.getY());

            // Pack position: x | (y << 4) | (z << 8)
            int16_t packed = static_cast<int16_t>(
                (pos.getX() & 15) |
                ((pos.getY() & 15) << 4) |
                ((pos.getZ() & 15) << 8)
            );

            m_postProcessing[sectionIndex].insert(packed);
        }
    }

    /**
     * Get height at position
     * Reference: ChunkAccess.java getHeight() returns getFirstAvailable() - 1
     * This returns the Y of the highest solid block (not the air above)
     */
    int getHeight(int heightmapType, int localX, int localZ) const override {
        auto type = static_cast<levelgen::Heightmap::Types>(heightmapType);
        auto it = m_heightmaps.find(type);
        if (it != m_heightmaps.end()) {
            return it->second->getHighestTaken(localX, localZ);
        }
        return m_minY;
    }

    /**
     * Get heightmap by type
     */
    levelgen::Heightmap* getHeightmap(levelgen::Heightmap::Types type) {
        auto it = m_heightmaps.find(type);
        return it != m_heightmaps.end() ? it->second.get() : nullptr;
    }

    /**
     * Get or create heightmap
     * Reference: ChunkAccess.java getOrCreateHeightmapUnprimed()
     */
    levelgen::Heightmap& getOrCreateHeightmap(levelgen::Heightmap::Types type) {
        auto it = m_heightmaps.find(type);
        if (it == m_heightmaps.end()) {
            auto blockGetter = [this](int32_t x, int32_t y, int32_t z) -> const BlockState* {
                return this->getBlockState(x, y, z);
            };
            auto result = m_heightmaps.emplace(
                type,
                std::make_unique<levelgen::Heightmap>(m_minY, m_height, type, blockGetter)
            );
            return *result.first->second;
        }
        return *it->second;
    }

    /**
     * Get or create heightmap (IChunk interface implementation)
     * Reference: ChunkAccess.java getOrCreateHeightmapUnprimed()
     */
    levelgen::Heightmap* getOrCreateHeightmapUnprimed(int heightmapType) override {
        auto type = static_cast<levelgen::Heightmap::Types>(heightmapType);
        return &getOrCreateHeightmap(type);
    }

    /**
     * Get index of highest non-air section
     * Reference: ChunkAccess.java getHighestFilledSectionIndex()
     */
    int32_t getHighestFilledSectionIndex() const {
        for (int32_t i = m_sectionCount - 1; i >= 0; --i) {
            if (!m_sections[i]->hasOnlyAir()) {
                return i;
            }
        }
        return -1;
    }

    /**
     * Get Y coordinate of highest non-air section
     * Reference: ChunkAccess.java getHighestSectionPosition()
     */
    int32_t getHighestSectionPosition() const override {
        int32_t index = getHighestFilledSectionIndex();
        return index == -1 ? m_minY : getSectionYFromIndex(index);
    }

    /**
     * Get all sections
     */
    const std::vector<std::unique_ptr<LevelChunkSection>>& getSections() const {
        return m_sections;
    }

    std::vector<std::unique_ptr<LevelChunkSection>>& getSections() {
        return m_sections;
    }

    /**
     * Get air block
     */
    BlockState* getAirBlock() const {
        return m_airBlock;
    }

    /**
     * Get default block (stone)
     */
    BlockState* getDefaultBlock() const {
        return m_defaultBlock;
    }

    /**
     * Get height
     */
    int32_t getHeight() const {
        return m_height;
    }

    /**
     * Get the persisted chunk status
     * Reference: ProtoChunk.java getPersistedStatus()
     */
    const chunk::status::ChunkStatus* getPersistedStatus() const override {
        return m_status;
    }

    /**
     * Set the chunk status
     * Reference: ProtoChunk.java setPersistedStatus()
     */
    void setPersistedStatus(const chunk::status::ChunkStatus& status) override {
        m_status = &status;
    }

    /**
     * Set status by pointer (for internal use)
     */
    void setStatus(const chunk::status::ChunkStatus* status) {
        m_status = status;
    }

    /**
     * Get the time players have spent in this chunk (in ticks)
     * Reference: ChunkAccess.java getInhabitedTime()
     */
    int64_t getInhabitedTime() const override {
        return m_inhabitedTime;
    }

    /**
     * Set the time players have spent in this chunk
     * Reference: ChunkAccess.java setInhabitedTime()
     */
    void setInhabitedTime(int64_t time) override {
        m_inhabitedTime = time;
    }

    //=========================================================================
    // Biome Methods
    // Reference: ChunkAccess.java biome methods
    //=========================================================================

    /**
     * Get biome at block position
     * Reference: ChunkAccess.java getNoiseBiome() lines 417-430
     *
     * This is the main biome lookup method used by buildSurface and other phases.
     * Converts block coordinates to quart coordinates and looks up from sections.
     */
    biome::BiomeHolder getBiome(const core::BlockPos& pos) const override {
        // Convert block to quart coordinates
        int32_t quartX = core::QuartPos::fromBlock(pos.getX());
        int32_t quartY = core::QuartPos::fromBlock(pos.getY());
        int32_t quartZ = core::QuartPos::fromBlock(pos.getZ());
        return getNoiseBiome(quartX, quartY, quartZ);
    }

    /**
     * Get biome at quart coordinates
     * Reference: ChunkAccess.java getNoiseBiome() lines 417-430
     *
     * @param quartX Quart X coordinate (block X / 4)
     * @param quartY Quart Y coordinate (block Y / 4)
     * @param quartZ Quart Z coordinate (block Z / 4)
     * @return BiomeHolder for the biome at this position
     */
    biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
        // Reference: ChunkAccess.java lines 419-423
        int32_t quartMinY = core::QuartPos::fromBlock(m_minY);
        int32_t quartMaxY = quartMinY + core::QuartPos::fromBlock(m_height) - 1;
        int32_t clampedQuartY = std::clamp(quartY, quartMinY, quartMaxY);

        // Get section index from quart Y
        int32_t sectionIndex = getSectionIndex(core::QuartPos::toBlock(clampedQuartY));
        if (sectionIndex < 0 || sectionIndex >= m_sectionCount) {
            // Return empty biome if out of bounds
            return biome::Biomes::get(biome::BiomeKeys::PLAINS);
        }

        // Get biome from section (use local quart coords)
        biome::BiomeKey key = m_sections[sectionIndex]->getNoiseBiome(
            quartX & 3,
            clampedQuartY & 3,
            quartZ & 3
        );
        return biome::Biomes::get(key);
    }

    /**
     * Fill biomes from a biome source
     * Reference: ChunkAccess.java fillBiomesFromNoise() lines 432-444
     *
     * This is called during the BIOMES phase to populate biome data.
     *
     * @param biomeSource The biome source to sample from
     * @param sampler Climate sampler for the noise
     */
    void fillBiomesFromNoise(
        biome::BiomeSource* biomeSource,
        const biome::Climate::Sampler& sampler
    ) {
        // Reference: ChunkAccess.java lines 433-443
        int32_t quartMinX = core::QuartPos::fromBlock(m_pos.getMinBlockX());
        int32_t quartMinZ = core::QuartPos::fromBlock(m_pos.getMinBlockZ());

        // Get height range in sections
        int32_t minSectionY = m_minY >> 4;
        int32_t maxSectionY = (m_minY + m_height - 1) >> 4;

        for (int32_t sectionY = minSectionY; sectionY <= maxSectionY; ++sectionY) {
            int32_t sectionIndex = getSectionIndex(sectionY << 4);
            if (sectionIndex >= 0 && sectionIndex < m_sectionCount) {
                // Quart Y for the bottom of this section
                int32_t quartMinY = core::QuartPos::fromSection(sectionY);
                m_sections[sectionIndex]->fillBiomesFromNoise(
                    biomeSource,
                    sampler,
                    quartMinX,
                    quartMinY,
                    quartMinZ
                );
            }
        }
    }
};

} // namespace world
} // namespace minecraft

// Convenience typedef in the world namespace for compatibility
namespace world {
    using ProtoChunk = minecraft::world::ProtoChunk;
}
