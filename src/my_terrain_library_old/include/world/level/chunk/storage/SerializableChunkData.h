#pragma once

#include "world/ChunkPos.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/LevelChunkSection.h"
#include "world/IBlockType.h"
#include "nbt/CompoundTag.h"
#include "nbt/ListTag.h"
#include <memory>
#include <vector>
#include <map>
#include <optional>

// Reference: net/minecraft/world/level/chunk/storage/SerializableChunkData.java

namespace minecraft {

// Forward declarations
namespace server { namespace level { class ServerLevel; } }
namespace world {
class IChunk;
class ProtoChunk;
}

namespace world {
namespace level {
namespace chunk {
namespace storage {

// Forward declarations for chunk types
class LevelChunk;

// Bring types into scope
using ChunkStatusPtr = ::minecraft::world::chunk::status::ChunkStatus;
using ChunkAccess = ::minecraft::world::IChunk;
using ProtoChunk = ::minecraft::world::ProtoChunk;

/**
 * Heightmap accessor for world generation
 */
struct HeightmapData {
    std::map<levelgen::Heightmap::Types, std::vector<int64_t>> heightmaps;
};

/**
 * Packed tick data for scheduled ticks
 */
struct PackedTicks {
    nbt::ListTag blockTicks;
    nbt::ListTag fluidTicks;
};

/**
 * Section data for a 16x16x16 chunk section
 */
struct SectionData {
    int8_t y;
    std::unique_ptr<nbt::CompoundTag> blockStates;  // Paletted container
    std::unique_ptr<nbt::CompoundTag> biomes;       // Paletted container
    std::unique_ptr<nbt::CompoundTag> blockLight;
    std::unique_ptr<nbt::CompoundTag> skyLight;
};

/**
 * SerializableChunkData - Handles chunk serialization/deserialization
 * Reference: SerializableChunkData.java
 *
 * This class is responsible for converting between ChunkAccess objects
 * and NBT format for storage.
 */
class SerializableChunkData {
public:
    // =========================================================================
    // Static parsing/writing methods
    // Reference: SerializableChunkData.java lines 90-162
    // =========================================================================

    /**
     * Parse chunk data from NBT
     * Reference: SerializableChunkData.java parse(LevelHeightAccessor, RegistryAccess, CompoundTag)
     */
    static std::unique_ptr<SerializableChunkData> parse(
        int minY,
        int height,
        nbt::CompoundTag& chunkData
    );

    /**
     * Copy data from a chunk to create serializable data
     * Reference: SerializableChunkData.java copyOf(ServerLevel, ChunkAccess)
     */
    static std::unique_ptr<SerializableChunkData> copyOf(
        server::level::ServerLevel& level,
        ChunkAccess& chunk
    );

    /**
     * Write chunk data to NBT
     * Reference: SerializableChunkData.java write()
     */
    std::unique_ptr<nbt::CompoundTag> write() const;

    /**
     * Get chunk status from NBT tag (without full parsing)
     * Reference: SerializableChunkData.java getChunkStatusFromTag(CompoundTag)
     */
    static const ChunkStatusPtr* getChunkStatusFromTag(nbt::CompoundTag* tag);

    // =========================================================================
    // Chunk creation methods
    // Reference: SerializableChunkData.java lines 165-268
    // =========================================================================

    /**
     * Create a ProtoChunk from the serialized data
     * Reference: SerializableChunkData.java read(ServerLevel, PoiManager, RegionStorageInfo, ChunkPos)
     */
    std::unique_ptr<ProtoChunk> read(
        server::level::ServerLevel& level,
        const ChunkPos& pos
    ) const;

    /**
     * Create a ProtoChunk from the serialized data (direct parameters version)
     * This version doesn't require ServerLevel
     */
    std::unique_ptr<ProtoChunk> readDirect(
        int minY,
        int height,
        IBlockType* airBlock,
        IBlockType* defaultBlock,
        BlockRegistry* registry
    ) const;

    /**
     * Copy data from a ProtoChunk directly (without ServerLevel)
     */
    static std::unique_ptr<SerializableChunkData> copyOfDirect(
        ProtoChunk& chunk
    );

    // =========================================================================
    // Data accessors
    // =========================================================================

    const ChunkPos& getChunkPos() const { return m_chunkPos; }
    const ChunkStatusPtr& getChunkStatus() const { return *m_chunkStatus; }
    int64_t getLastUpdateTime() const { return m_lastUpdateTime; }
    int64_t getInhabitedTime() const { return m_inhabitedTime; }
    bool isLightCorrect() const { return m_lightCorrect; }

private:
    SerializableChunkData() = default;

    ChunkPos m_chunkPos;
    int m_minSectionY;
    int64_t m_lastUpdateTime;
    int64_t m_inhabitedTime;
    const ChunkStatusPtr* m_chunkStatus;
    bool m_lightCorrect;

    // Section data
    std::vector<SectionData> m_sectionData;

    // Heightmaps
    HeightmapData m_heightmaps;

    // Ticks
    PackedTicks m_packedTicks;

    // Entity/block entity data
    std::vector<std::unique_ptr<nbt::CompoundTag>> m_entities;
    std::vector<std::unique_ptr<nbt::CompoundTag>> m_blockEntities;

    // Structure data
    std::unique_ptr<nbt::CompoundTag> m_structureData;

    // Upgrade data (for version upgrades)
    std::unique_ptr<nbt::CompoundTag> m_upgradeData;

    // Post-processing sections (for deferred processing)
    std::vector<std::vector<int16_t>> m_postProcessingSections;

    // Blending data (for chunk borders)
    std::unique_ptr<nbt::CompoundTag> m_blendingData;

    // Below zero retrogen data (for cave generation below y=0)
    std::unique_ptr<nbt::CompoundTag> m_belowZeroRetrogen;

    // Carving mask
    std::unique_ptr<std::vector<int64_t>> m_carvingMask;

    // Helper methods
    static void parseSections(
        SerializableChunkData& data,
        nbt::ListTag& sections,
        int minY,
        int height
    );

    static void parseHeightmaps(
        SerializableChunkData& data,
        nbt::CompoundTag& heightmaps
    );

    static void parseTicks(
        SerializableChunkData& data,
        nbt::CompoundTag& chunkData
    );

    void writeSections(nbt::CompoundTag& tag) const;
    void writeHeightmaps(nbt::CompoundTag& tag) const;
    void writeTicks(nbt::CompoundTag& tag) const;
};

/**
 * ChunkSerializer - Namespace for chunk serialization utilities
 * Reference: ChunkSerializer.java (merged functionality)
 */
namespace ChunkSerializer {

/**
 * Current chunk data version
 * Reference: ChunkSerializer.java DATA_VERSION
 */
constexpr int32_t DATA_VERSION = 3953;  // 1.21.1

/**
 * Read a chunk from NBT, returning either ProtoChunk or LevelChunk
 */
std::unique_ptr<ChunkAccess> read(
    server::level::ServerLevel& level,
    const ChunkPos& pos,
    nbt::CompoundTag& tag
);

/**
 * Write a chunk to NBT
 */
std::unique_ptr<nbt::CompoundTag> write(
    server::level::ServerLevel& level,
    ChunkAccess& chunk
);

} // namespace ChunkSerializer

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
